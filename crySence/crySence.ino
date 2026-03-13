// =============================================================================
// CrySense AI v2.0 — Firmware Principal
// ESP32-S3 | FreeRTOS | Edge Impulse | Firebase RTDB | BME280 | OLED | Áudio
// TDE — Performance de Sistemas Ciberfísicos — PUCPR
// =============================================================================

// --- Alocação do modelo Edge Impulse em PSRAM ---
#define EI_CLASSIFIER_ALLOCATION_STATIC       0
#define EI_CLASSIFIER_ALLOCATION_PSRAM        1
#define EI_CLASSIFIER_TFLITE_ENABLE_PSRAM     1

#include <CrySense_AI_inferencing.h>
#include "driver/i2s.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include <WiFi.h>
#include <ArduinoJson.h>

// --- Módulos do sistema ---
#include "secrets.h"
#include "config_manager.h"
#include "log_manager.h"
#include "sensor_bme280.h"


#include "audio_player.h"
#include "firebase_manager.h"
#include "display_manager.h"

// web_server tem dependência de AudioPlayer (tamanhoArquivo e salvarWav)
#include "web_server.h"

#define CICLOS_PARA_RESETAR 60

// =============================================================================
// ESTADO COMPARTILHADO (Instanciação das globais do config_manager)
// =============================================================================
SistemaState      gState;
SemaphoreHandle_t gMutexState = nullptr;
WebDashboardState gWebState = {};

SemaphoreHandle_t semAudioPronto = nullptr; // Audio → IA
QueueHandle_t     qResultadoIA   = nullptr; // IA → Decisao
QueueHandle_t     qFirebase      = nullptr; // Decisao → IOT

// =============================================================================
// BUFFER DE INFERÊNCIA (em PSRAM)
// =============================================================================
float* inference_buffer = nullptr; // 16000 floats = 64KB → PSRAM

int get_signal_data_callback(size_t offset, size_t length, float* out_ptr) {
    memcpy(out_ptr, inference_buffer + offset, length * sizeof(float));
    return 0;
}

// =============================================================================
// INTERRUPÇÃO DE HARDWARE — GPIO 0 (Botão BOOT)
// Reseta o estado de crise manualmente — para o áudio e volta ao monitoramento
// =============================================================================
static volatile bool flagResetISR = false;

void IRAM_ATTR isrBotaoReset() {
    flagResetISR = true;
}

// =============================================================================
// TASK 1 — INFERÊNCIA IA (Core 1, Prioridade 4)
// Aguarda áudio pronto → run_classifier → envia resultado
// =============================================================================

// =============================================================================
// TASK 2 — INFERÊNCIA IA (Core 1, Prioridade 4)
// Aguarda áudio pronto → run_classifier → envia resultado
// =============================================================================
void TaskIA(void* pv) {
    LogManager::info("[TaskIA] Iniciada no Core 1");
    while (true) {
        // Aguarda semáforo do TaskAudio (bloqueante)
        xSemaphoreTake(semAudioPronto, portMAX_DELAY);

        uint64_t t0 = esp_timer_get_time();

        signal_t signal;
        signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
        signal.get_data     = &get_signal_data_callback;

        ei_impulse_result_t result = {0};
        EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);

        float infMs = (float)(esp_timer_get_time() - t0) / 1000.0f;

        if (err != EI_IMPULSE_OK) {
            LogManager::errorf("[TaskIA] Erro classifier: %d", err);
            continue;
        }

        // Encontra classe com maior score
        InferenceResult ir;
        ir.infMs = infMs;
        ir.confianca = 0;
        strlcpy(ir.label, "noise", sizeof(ir.label));

        // --- Pesos para reduzir falsos positivos ---
        // Se a classe "noise" tiver algum score razoável, damos um pequeno boost 
        // para ela vencer disputas acirradas contra choro fraco
        for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
            float s = result.classification[i].value;
            // --- Heurística: Priorizar Cólica & Destruir Falsos Fomes ---
            if (strcmp(result.classification[i].label, LABEL_COLIC) == 0) {
                s *= 1.45f; // Bônus extremo na cólica para ela ganhar em empates sutis
            }
            // Limiar de confiança para fome: 80%
            if (strcmp(result.classification[i].label, LABEL_HUNGER) == 0) {
                if (s < 0.80f) s = 0.0f; // Descarta totalmente se for < 80%
            }
            if (s > 1.0f) s = 1.0f;

            if (strcmp(result.classification[i].label, LABEL_NOISE) == 0) {
                s *= 1.1f; // Bonifica o ruído para ele assumir o lugar das "fomes leves"
            }
            ir.scores[i] = s;
        }

        // Pega o maior score APÓS aplicar os pesos
        for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
            if (ir.scores[i] > ir.confianca) {
                ir.confianca = ir.scores[i];
                strlcpy(ir.label, result.classification[i].label, sizeof(ir.label));
            }
        }

        // Envia resultado para TaskDecisao (não bloqueante)
        xQueueSend(qResultadoIA, &ir, 0);

        // Atualiza estado web (sem mutex — eventual consistency OK)
        strlcpy(gWebState.label, ir.label, sizeof(gWebState.label));
        gWebState.confianca = ir.confianca;  // 0.0-1.0; a API multiplica por 100
        for (int i=0; i<4; i++) gWebState.scores[i] = ir.scores[i];

        // Rastreia heap mínimo
        uint32_t fh = ESP.getFreeHeap();
        if (fh < gState.heapMin) gState.heapMin = fh;

        Serial.printf("[IA] %s %.0f%% (%.1fms)\n",
                      ir.label, ir.confianca * 100.0f, infMs);
    }
}

// =============================================================================
// TASK 3 — DECISÃO / LÓGICA PRINCIPAL (Core 0, Prioridade 3)
// =============================================================================
static void registrar(TipoChoro t) {
    gState.historico[gState.indiceAtual] = t;
    gState.indiceAtual = (gState.indiceAtual + 1) % TAMANHO_BUFFER;
    if (gState.ciclosTotais < TAMANHO_BUFFER) gState.ciclosTotais++;
}

static void dispararAlerta(const char* tipo, float conf) {
    // Atualiza estado
    strlcpy(gState.ultimoAlerta, tipo, sizeof(gState.ultimoAlerta));
    gState.tsAlerta = millis();
    gState.choroCritico = true;
    gWebState.estado = "crise";
    strlcpy(gWebState.ultimo_alerta, tipo, sizeof(gWebState.ultimo_alerta));

    // OLED
    const char* titulo   = strcmp(tipo,"COLICA")==0 ? "COLICA!" :
                           strcmp(tipo,"FOME")==0   ? "FOME!"   : "SONO!";
    const char* subtitulo= strcmp(tipo,"COLICA")==0 ? "Ruido branco ativo" :
                           strcmp(tipo,"FOME")==0   ? "Precisa mamar"      : "Quer dormir";

    Display::mostrarAlerta(titulo, subtitulo, conf,
                           strcmp(tipo,"COLICA")==0);
    LogManager::alertaf("ALERTA: %s (conf %.0f%%)", tipo, conf*100);

    // Áudio apenas para cólica
    if (strcmp(tipo,"COLICA") == 0) {
        CryConfig& cfg = ConfigManager::get();
        AudioPlayer::setVolume(cfg.volume_audio);
        AudioPlayer::iniciar(cfg.audio_url);
        gWebState.audio_ativo = true;
    }

    // Firebase (via fila — não bloqueia esta task)
    FirebaseMsg fm;
    strlcpy(fm.tipo, tipo, sizeof(fm.tipo));
    fm.confianca = conf;
    fm.acalmado  = false;
    auto sens = Sensores::ler();
    fm.temp = sens.temperatura;
    fm.umid = sens.umidade;
    xQueueSend(qFirebase, &fm, 0);
}

void TaskDecisao(void* pv) {
    LogManager::info("[TaskDecisao] Iniciada no Core 0");
    InferenceResult ir;

    while (true) {
        // ISR de reset (botão GPIO0)
        if (flagResetISR) {
            flagResetISR = false;
            gState.choroCritico  = false;
            gState.ciclosSilencio = 0;
            gState.ciclosTotais  = 0;
            gState.indiceAtual   = 0;
            for (int i=0; i<TAMANHO_BUFFER; i++) gState.historico[i] = SILENCIO_RUIDO;
            AudioPlayer::parar();
            gWebState.audio_ativo = false;
            gWebState.estado = "calmo";
            Display::mostrarNormal(gWebState.temp, gWebState.umid, "Reset manual",
                                   esp_timer_get_time()/1000000ULL);
            LogManager::info("[ISR] Estado resetado via botão");
        }

        // Aguarda resultado da IA (timeout 2s)
        if (xQueueReceive(qResultadoIA, &ir, pdMS_TO_TICKS(2000)) != pdTRUE) continue;

        uint64_t t0 = esp_timer_get_time();

        CryConfig& cfg = ConfigManager::get();

        // --- Classifica previsão deste ciclo ---
        // Exige hardcoded 50% (0.50f) pois a IA já foi duramente peneirada e penalizada
        // acima na função de scores
        TipoChoro prev = SILENCIO_RUIDO;
        if (ir.confianca >= 0.50f) {
            if      (strcmp(ir.label, LABEL_HUNGER)     == 0) prev = FOME;
            else if (strcmp(ir.label, LABEL_COLIC)      == 0) prev = COLICA_DOR;
            else if (strcmp(ir.label, LABEL_SLEEP)      == 0) prev = SONO;
            // NOISE → SILENCIO_RUIDO (ignorado)
        }

        registrar(prev);

        // --- Análise do histórico ---
        int janela = min(gState.ciclosTotais, TAMANHO_BUFFER);
        if (janela == 0) continue;

        int cFome=0, cColica=0, cSilencio=0, cSono=0;
        int start = (gState.indiceAtual - 1 + TAMANHO_BUFFER) % TAMANHO_BUFFER;
        
        // Se for silêncio absoluto via RMS, conta dobrado no histórico para resetar a IA rápido
        if (ir.isSilencioReal) cSilencio += 2; 

        for (int i = 0; i < janela; i++) {
            int idx = (start - i + TAMANHO_BUFFER) % TAMANHO_BUFFER;
            switch (gState.historico[idx]) {
                case FOME:        cFome++;    break;
                case COLICA_DOR:  cColica++;  break;
                case SONO:        cSono++;    break;
                default:          cSilencio++;break;
            }
        }

        // Regra do Usuário: Limiares de disparo
        int gatilhoColica = 7;
        int gatilhoFome   = 7;
        int gatilhoS      = 4;

        // Prioridade: CÓLICA > FOME > SONO
        // Se COLICA empatar com FOME, COLICA dispara primeiro
        bool choroCrit = (cColica  >= gatilhoColica ||
                          cFome    >= gatilhoFome);

        if (choroCrit) {
            gState.ciclosSilencio = 0;
            gWebState.estado = "crise";
            if (!gState.choroCritico) {
                if      (cColica  >= gatilhoColica) dispararAlerta("COLICA", ir.confianca);
                else if (cFome    >= gatilhoFome)   dispararAlerta("FOME",   ir.confianca);
            }
        } else if ((cSilencio + cSono) >= gatilhoS) {
            if (gState.choroCritico) {
                gState.ciclosSilencio++;
                if (gState.ciclosSilencio >= CICLOS_PARA_RESETAR) {
                    gState.choroCritico   = false;
                    gState.ciclosSilencio = 0;
                    AudioPlayer::parar();
                    gWebState.audio_ativo = false;
                    gWebState.estado = "calmo";
                    LogManager::info("Bebe se acalmou. Sistema rearmado.");
                    // Notifica Firebase
                    FirebaseMsg fm = {"CALMO", 0, 0, 0, true};
                    xQueueSend(qFirebase, &fm, 0);
                }
            } else {
                gWebState.estado = "monitorando";
            }
        }

        float execMs = (float)(esp_timer_get_time() - t0) / 1000.0f;
        Serial.printf("[TaskDecisao] Tempo de execucao: %.1fms\n", execMs);

        // Atualiza estado web
        gWebState.uptime_s = esp_timer_get_time() / 1000000ULL;
        gWebState.heap      = ESP.getFreeHeap();
        gWebState.heap_min  = gState.heapMin;
        gWebState.psram     = ESP.getFreePsram();
        gWebState.psram_tot = ESP.getPsramSize();

        // Gerenciamento de energia — Light Sleep por inatividade
        if (cfg.light_sleep_min > 0 && !gState.choroCritico) {
            uint32_t inativo_ms = millis() - gState.tsAlerta;
            if (gState.tsAlerta > 0 &&
                inativo_ms > (uint32_t)cfg.light_sleep_min * 60000UL) {
                LogManager::infof("Entrando em light sleep (%u min inativo)", cfg.light_sleep_min);
                Serial.flush();
                esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
                esp_sleep_enable_timer_wakeup(30ULL * 1000000ULL); // acorda em 30s
                esp_light_sleep_start();
                esp_wifi_set_ps(WIFI_PS_NONE);
            }
        }
    }
}

// =============================================================================
// SETUP E INICIALIZAÇÃO
// =============================================================================

// =============================================================================
// HARDWARE SETUP
// =============================================================================
// Hardware setup movido para audio_player.h

// =============================================================================
// SETUP PRINCIPAL
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(500); // BUG FIX: while(!Serial) trava ESP32

    Serial.println("\n=========================================");
    Serial.println(" CrySense AI v2.0 — Inicializando...");
    Serial.println("=========================================");

    // --- PSRAM ---
    if (psramInit()) {
        Serial.printf("[BOOT] PSRAM: %u KB livres\n", ESP.getFreePsram()/1024);
    } else {
        Serial.println("[BOOT] PSRAM: não disponível!");
    }

    // --- Buffers em PSRAM ---
    inference_buffer = (float*)ps_malloc(EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE * sizeof(float));
    if (!inference_buffer) {
        Serial.println("[BOOT] ERRO: falha ao alocar buffers em PSRAM!");
        while(true); // Halt
    }
    memset(inference_buffer, 0, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE * sizeof(float));

    // --- Config (NVS) ---
    ConfigManager::load();
    CryConfig& cfg = ConfigManager::get();

    // --- Log (SPIFFS) ---
    LogManager::begin();
    LogManager::info("CrySense AI v2.0 iniciando...");

    // --- Hardware ---
    Sensores::begin();

    // --- OLED ---
    Display::begin();
    Display::mostrarBoot();
    delay(800);

    // --- Interrupt hardware (GPIO 0 = botão BOOT) ---
    pinMode(0, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(0), isrBotaoReset, FALLING);
    LogManager::info("[HW] Interrupt configurado no GPIO0 (BOOT button)");

    // --- WiFi ---
    Display::mostrarWiFi(true, cfg.wifi_ssid);
    Serial.printf("[WiFi] Conectando a %s...\n", cfg.wifi_ssid);
    WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
    int tentativas = 0;
    while (WiFi.status() != WL_CONNECTED && tentativas < 30) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print('.');
        tentativas++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Conectado! IP: %s\n", WiFi.localIP().toString().c_str());
        Display::mostrarWiFi(false, cfg.wifi_ssid, WiFi.localIP().toString().c_str());
        strncpy(gWebState.ip, WiFi.localIP().toString().c_str(), sizeof(gWebState.ip));
        gWebState.wifi_ok = true;
        LogManager::infof("WiFi OK: %s", WiFi.localIP().toString().c_str());
        esp_wifi_set_ps(WIFI_PS_NONE); // Máxima performance WiFi
    } else {
        Serial.println("\n[WiFi] Falha! Iniciando modo AP (CrySense-Setup).");
        LogManager::warning("[WiFi] Falha — iniciando AP");
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP("CrySense-Setup", "12345678");
        IPAddress IP = WiFi.softAPIP();
        Serial.printf("[WiFi] AP IP: %s\n", IP.toString().c_str());
        Display::mostrarWiFi(false, "CrySense-Setup", IP.toString().c_str());
        strncpy(gWebState.ip, IP.toString().c_str(), sizeof(gWebState.ip));
        gWebState.wifi_ok = false;
    }
    delay(1000);

    // --- Firebase ---
    Firebase::begin(cfg.firebase_url, cfg.firebase_auth);

    // --- Web Server ---
    WebServer::begin(&gWebState, &cfg);
    if (gWebState.wifi_ok) {
        LogManager::infof("Web: http://%s", WiFi.localIP().toString().c_str());
    } else {
        LogManager::infof("Web (AP): http://%s", WiFi.softAPIP().toString().c_str());
    }

    // --- Estado inicial ---
    gState.heapMin = ESP.getFreeHeap();
    for (int i=0; i<TAMANHO_BUFFER; i++) gState.historico[i] = SILENCIO_RUIDO;
    gWebState.estado = "calmo";
    AudioPlayer::setVolume(cfg.volume_audio);
    gWebState.flash_used = ESP.getSketchSize();
    gWebState.flash_tot  = ESP.getFlashChipSize();
    gWebState.audio_spiffs_size = (float)AudioPlayer::tamanhoArquivo();

    // --- Sincronizadores inter-task ---
    gMutexState    = xSemaphoreCreateMutex();   // BUGFIX: criado antes de tudo
    semAudioPronto = xSemaphoreCreateBinary();
    qResultadoIA   = xQueueCreate(2, sizeof(InferenceResult));
    qFirebase      = xQueueCreate(5, sizeof(FirebaseMsg));

    // ==========================================================================
    // ==========================================================================
    // CRIAÇÃO DAS TASKS FREERTOS
    // ==========================================================================
    AudioPlayer::beginTasks(); // TaskAudio (Core 1) + TaskPlayer (Core 1 / 2)
    Display::beginTask();      // TaskHMI (Core 0)
    Firebase::beginTask();     // TaskIOT (Core 0)

    // Lógica e IA (no documento .ino)
    xTaskCreatePinnedToCore(TaskIA,      "TaskIA",      16384, nullptr, 4, nullptr, 1);
    xTaskCreatePinnedToCore(TaskDecisao, "TaskDecisao", 8192,  nullptr, 3, nullptr, 0);

    LogManager::info("=== CrySense AI v2.0 — Sistema Pronto ===");
    Serial.println("\n[BOOT] Todas as tasks iniciadas. Sistema em operação.\n");
}

// =============================================================================
// LOOP — Vazio: toda lógica está nas FreeRTOS tasks
// Não usar delay() — viola requisito TDE de ausência de rotinas bloqueantes
// =============================================================================
void loop() {
    vTaskDelay(pdMS_TO_TICKS(10000)); // Cede CPU; loop nunca é usado
}
