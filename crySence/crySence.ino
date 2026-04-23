// --- Alocação do modelo Edge Impulse em PSRAM ---
#define EI_CLASSIFIER_ALLOCATION_STATIC 0
#define EI_CLASSIFIER_ALLOCATION_PSRAM 1
#define EI_CLASSIFIER_TFLITE_ENABLE_PSRAM 1
#define ei_default_impulse ei_default_impulse_trigger
#include <CrySense_trigger_inferencing.h>
#undef ei_default_impulse
#include "C:/Arduino/libraries/CrySense_trigger_inferencing/src/model-parameters/model_variables.h"

#include "driver/i2s.h"
#include "esp_sleep.h"

#include "esp_wifi.h"
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <DNSServer.h>

DNSServer dnsServer;

// --- Módulos do sistema ---
#include "audio_player.h"
#include "config_manager.h"
#include "display_manager.h"
#include "firebase_manager.h"
#include "log_manager.h"
#include "secrets.h"
#include "sensor_bme280.h"
#include "remote_classifier.h"

// web_server tem dependência de AudioPlayer (tamanhoArquivo e salvarWav)
#include "web_server.h"

// =============================================================================
// ESTADO COMPARTILHADO (Instanciação das globais do config_manager)
// =============================================================================
SistemaState gState;
SemaphoreHandle_t gMutexState = nullptr;
WebDashboardState gWebState = {};
AnalyticsState gAnalytics; // histórico de episódios para os gráficos

SemaphoreHandle_t semAudioPronto = nullptr; // Audio → IA
QueueHandle_t qResultadoIA = nullptr;       // IA → Decisao
QueueHandle_t qFirebase = nullptr;          // Decisao → IOT
QueueHandle_t qAudioUpload = nullptr;       // Audio -> Classificador remoto
QueueHandle_t qRemoteResult = nullptr;      // Classificador remoto -> Decisao
static bool gOtaReady = false;

// Task handles para controle durante OTA
static TaskHandle_t hTaskIA = nullptr;
static TaskHandle_t hTaskDecisao = nullptr;
static TaskHandle_t hTaskAudio = nullptr;
static TaskHandle_t hTaskPlayer = nullptr;
static TaskHandle_t hTaskHMI = nullptr;
static TaskHandle_t hTaskIOT = nullptr;
static TaskHandle_t hTaskRemote = nullptr;

// =============================================================================
// BUFFER DE INFERÊNCIA (em PSRAM)
// =============================================================================
float *inference_buffer = nullptr; // 16000 floats = 64KB → PSRAM

int get_signal_data_callback(size_t offset, size_t length, float *out_ptr) {
  memcpy(out_ptr, inference_buffer + offset, length * sizeof(float));
  return 0;
}

// Handle do modelo TRIGGER via alias estável (não depende de Project ID
// numérico). ei_default_impulse_trigger é declarado em model_variables.h do
// trigger (linha 193) e referencia impulse_handle_935943_1 (CrySense_trigger, 2
// classes: cry / noise).
static inline EI_IMPULSE_ERROR run_trigger_model(signal_t *signal,
                                                 ei_impulse_result_t *result) {
  return run_classifier(&ei_default_impulse_trigger, signal, result, false);
}

// =============================================================================
// INTERRUPÇÃO DE HARDWARE — GPIO 0 (Botão BOOT)
// Reseta o estado de crise manualmente — para o áudio e volta ao monitoramento
// =============================================================================
static volatile bool flagResetISR = false;
static volatile TickType_t gLastResetIsrTick = 0;
static uint32_t gBootMs = 0;

void IRAM_ATTR isrBotaoReset() {
  const TickType_t now = xTaskGetTickCountFromISR();
  // Debounce em ISR para evitar rajadas no GPIO0 (pino BOOT é sensível).
  if ((now - gLastResetIsrTick) >= pdMS_TO_TICKS(400)) {
    gLastResetIsrTick = now;
    flagResetISR = true;
  }
}

// =============================================================================
// TASK 1 — INFERÊNCIA IA (Core 1, Prioridade 4)
// Aguarda áudio pronto → run_classifier → envia resultado
// =============================================================================
void TaskIA(void *pv) {
  LogManager::info("[TaskIA] Iniciada no Core 1");
  while (true) {
    // Aguarda semáforo do TaskAudio (bloqueante)
    xSemaphoreTake(semAudioPronto, portMAX_DELAY);

    uint64_t t0 = esp_timer_get_time();
    CryConfig &cfg = ConfigManager::get();

    // BUGFIX: Validação de integridade do buffer antes de qualquer inferência.
    // Se o buffer for majoritariamente zeros (falha I2S no boot, ou frame
    // descartado), a rede neural pode interpretar o padrão DC como uma classe
    // específica. Calculamos a energia do buffer e abortamos se for ínfima.
    float bufEnergy = 0.0f;
    for (int i = 0; i < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; i++)
      bufEnergy += inference_buffer[i] * inference_buffer[i];
    float bufRMS = sqrtf(bufEnergy / EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);
    // Ajuste: o gate de energia aqui precisa ser mais baixo que o gate do TaskAudio.
    // Caso contrário, quadros válidos de choro distante passam no TaskAudio e são
    // descartados nesta etapa como "buffer vazio".
    float emptyFloor = cfg.rms_threshold * 0.25f;
    if (emptyFloor < 0.0020f) emptyFloor = 0.0020f;
    if (emptyFloor > 0.0200f) emptyFloor = 0.0200f; // Expandido acompanhando o config limit de 0.06
    if (bufRMS < emptyFloor) {
      // Buffer praticamente vazio — injetar silêncio sem inferir
      InferenceResult irZero = {};
      strlcpy(irZero.label, LABEL_NOISE, sizeof(irZero.label));
      irZero.confianca = 1.0f;
      irZero.isSilencioReal = true;
      irZero.infMs = 0.0f;
      xQueueSend(qResultadoIA, &irZero, 0);
      Serial.printf("[TaskIA] Buffer vazio (RMS=%.4f, floor=%.4f) — descartado\n",
                    bufRMS, emptyFloor);
      continue;
    }

    signal_t signal;
    signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
    signal.get_data = &get_signal_data_callback;

    ei_impulse_result_t triggerResult = {0};
    EI_IMPULSE_ERROR err = run_trigger_model(&signal, &triggerResult);

    if (err != EI_IMPULSE_OK) {
      LogManager::errorf("[TaskIA] Erro trigger: %d", err);
      continue;
    }

    // Resultado enviado para a lógica de decisão (ordem fixa:
    // colic,hunger,noise,reservado)
    InferenceResult ir;
    ir.confianca = 0;
    ir.isSilencioReal = false;
    strlcpy(ir.label, LABEL_NOISE, sizeof(ir.label));
    for (int i = 0; i < 4; i++)
      ir.scores[i] = 0.0f;

    // Trigger local (cry/noise) — a classificação de tipo é remota.
    float scoreCry = 0.0f;
    float scoreNoise = 1.0f;
    const size_t triggerCount = sizeof(triggerResult.classification) /
                                sizeof(triggerResult.classification[0]);
    for (size_t i = 0; i < triggerCount; i++) {
      const char *lbl = triggerResult.classification[i].label;
      const float sc = triggerResult.classification[i].value;
      if (!lbl || lbl[0] == '\0')
        continue;
      if (strcmp(lbl, LABEL_CRY) == 0)   // Trigger: classe "cry"
        scoreCry = sc;
      else if (strcmp(lbl, LABEL_NOISE) == 0) // Trigger: classe "noise"
        scoreNoise = sc;
    }

    // Requisito do sistema: só considera "choro" quando confiança >= 75%.
    // Mantém também a condição scoreCry >= scoreNoise para evitar inversões.
    float minCry = cfg.confianca_minima;
    if (minCry < 0.75f) minCry = 0.75f;
    if (minCry > 0.95f) minCry = 0.95f;
    const bool hasCry = (scoreCry >= scoreNoise) && (scoreCry >= minCry);

    if (!hasCry) {
      ir.confianca = scoreNoise;
      strlcpy(ir.label, LABEL_NOISE, sizeof(ir.label));
      ir.scores[2] = 1.0f;
      Serial.printf("[IA-TRIGGER] cry=%.2f noise=%.2f -> noise\n", scoreCry,
                    scoreNoise);
    } else {
      ir.confianca = scoreCry;
      strlcpy(ir.label, LABEL_CRY, sizeof(ir.label));
      ir.scores[2] = scoreNoise;
      Serial.printf("[IA-TRIGGER] cry=%.2f noise=%.2f -> cry\n", scoreCry, scoreNoise);
    }

    float infMs = (float)(esp_timer_get_time() - t0) / 1000.0f;
    ir.infMs = infMs;

    // Envia resultado para TaskDecisao (não bloqueante)
    xQueueSend(qResultadoIA, &ir, 0);
    gState.totalInfs++;

    // Atualiza estado web (sem mutex — eventual consistency OK)
    strlcpy(gWebState.label, hasCry ? "choro" : LABEL_NOISE, sizeof(gWebState.label));
    gWebState.confianca = ir.confianca; // 0.0-1.0; a API multiplica por 100
    gWebState.inf_ms = ir.infMs;
    for (int i = 0; i < 4; i++)
      gWebState.scores[i] = ir.scores[i];

    // Rastreia heap mínimo
    uint32_t fh = ESP.getFreeHeap();
    if (fh < gState.heapMin)
      gState.heapMin = fh;

    Serial.printf("[IA] %s %.0f%% (%.1fms)\n", ir.label, ir.confianca * 100.0f,
                  infMs);
    
  }
}

// =============================================================================
// TASK 2 — DECISÃO / LÓGICA PRINCIPAL (Core 0, Prioridade 3)
// =============================================================================
static void _tipoUpperFromRemoteLabel(const char* label, char* out, size_t outLen) {
  if (!label || !out || outLen == 0) return;
  if (strcmp(label, "colic") == 0 || strcmp(label, "COLICA") == 0) {
    strlcpy(out, "COLICA", outLen);
  } else if (strcmp(label, "hunger") == 0 || strcmp(label, "FOME") == 0) {
    strlcpy(out, "FOME", outLen);
  } else {
    out[0] = '\0';
  }
}

static void _aplicarResultadoRemoto(const RemoteClassResult& rr) {
  if (!rr.ok) {
    strlcpy(gWebState.pipeline, "error", sizeof(gWebState.pipeline));
    strlcpy(gWebState.estado, "monitorando", sizeof(gWebState.estado));
    return;
  }

  strlcpy(gWebState.result_label, rr.label, sizeof(gWebState.result_label));
  gWebState.result_conf = rr.confianca;
  gWebState.remote_latency_ms = rr.latency_ms;
  for (int i = 0; i < 4; i++) gWebState.scores[i] = rr.scores[i];

  auto sens = Sensores::ler();
  const bool isColic = (strcmp(rr.label, LABEL_COLIC) == 0);
  const bool isHunger = (strcmp(rr.label, LABEL_HUNGER) == 0);
  if (!isColic && !isHunger) {
    strlcpy(gWebState.pipeline, "error", sizeof(gWebState.pipeline));
    strlcpy(gWebState.estado, "monitorando", sizeof(gWebState.estado));
    LogManager::warningf("[TaskDecisao] Label remoto invalido: %s", rr.label);
    return;
  }

  char tipo[16] = "COLICA";
  _tipoUpperFromRemoteLabel(rr.label, tipo, sizeof(tipo));
  if (tipo[0] == '\0') {
    strlcpy(gWebState.pipeline, "error", sizeof(gWebState.pipeline));
    strlcpy(gWebState.estado, "monitorando", sizeof(gWebState.estado));
    LogManager::warningf("[TaskDecisao] Normalizacao falhou para label: %s", rr.label);
    return;
  }
  // Só aceita classificação remota quando confiança >= 75% e sem empate.
  // Isso evita falso "FOME 50%" em áudio ambíguo/sem choro real.
  CryConfig& cfg = ConfigManager::get();
  float minRemoteConf = cfg.confianca_minima;
  if (minRemoteConf < 0.75f) minRemoteConf = 0.75f;
  if (minRemoteConf > 0.95f) minRemoteConf = 0.95f;
  const float margin = fabsf(rr.scores[0] - rr.scores[1]);
  const bool confidenceOk = rr.confianca >= minRemoteConf;
  const bool marginOk = margin >= 0.20f; // 20 pontos percentuais de separação.
  if (!confidenceOk || !marginOk) {
    gState.choroCritico = false;
    gState.ciclosSilencio = 0;
    strlcpy(gWebState.label, LABEL_NOISE, sizeof(gWebState.label));
    gWebState.confianca = 0.0f;
    strlcpy(gWebState.result_label, LABEL_NOISE, sizeof(gWebState.result_label));
    gWebState.result_conf = 0.0f;
    for (int i = 0; i < 4; i++) gWebState.scores[i] = 0.0f;
    gWebState.scores[2] = 1.0f;
    strlcpy(gWebState.estado, "monitorando", sizeof(gWebState.estado));
    strlcpy(gWebState.ultimo_alerta, "INCONCLUSIVO", sizeof(gWebState.ultimo_alerta));
    gWebState.audio_ativo = false;
    AudioPlayer::parar();
    LogManager::infof(
        "[TaskDecisao] Resultado remoto inconclusivo: %s %.0f%% (margem %.0f%%, min %.0f%%)",
        rr.label, rr.confianca * 100.0f, margin * 100.0f, minRemoteConf * 100.0f);
    return;
  }
  // Mantém retrocompatibilidade para UIs legadas que ainda leem `label/confianca`
  // ao invés de `result_label/result_conf`.
  strlcpy(gWebState.label, rr.label, sizeof(gWebState.label));
  gWebState.confianca = rr.confianca;
  strlcpy(gState.ultimoAlerta, tipo, sizeof(gState.ultimoAlerta));
  gState.tsAlerta = millis();
  gState.choroCritico = true;
  gState.ciclosSilencio = 0;
  strlcpy(gWebState.estado, "crise", sizeof(gWebState.estado));
  strlcpy(gWebState.ultimo_alerta, tipo, sizeof(gWebState.ultimo_alerta));

  gAnalytics.registrar(tipo, (uint8_t)(rr.confianca * 100), sens.temperatura,
                       sens.umidade, esp_timer_get_time() / 1000000ULL);

  const char* titulo = strcmp(tipo, "COLICA") == 0 ? "COLICA!"
                                                    : "FOME!";
  const char* subtitulo = strcmp(tipo, "COLICA") == 0 ? "Ruido branco ativo"
                                                       : "Precisa mamar";
  Display::mostrarAlerta(titulo, subtitulo, rr.confianca, strcmp(tipo, "COLICA") == 0);

  // Ruído branco somente para cólica.
  if (strcmp(tipo, "COLICA") == 0) {
    AudioPlayer::setVolume(cfg.volume_audio);
    AudioPlayer::iniciar(60);
    gWebState.audio_ativo = true;
  } else {
    AudioPlayer::parar();
    gWebState.audio_ativo = false;
  }

  FirebaseMsg fm;
  strlcpy(fm.tipo, tipo, sizeof(fm.tipo));
  fm.confianca = rr.confianca;
  fm.acalmado = false;
  fm.temp = sens.temperatura;
  fm.umid = sens.umidade;
  xQueueSend(qFirebase, &fm, 0);
}

void TaskDecisao(void *pv) {
  LogManager::info("[TaskDecisao] Iniciada no Core 0");
  RemoteClassResult rr;
  InferenceResult ir;
  static constexpr uint8_t CRY_WINDOW_SIZE = 5;
  static constexpr uint8_t CRY_WINDOW_MIN_VALID = 3;
  bool cryWindow[CRY_WINDOW_SIZE] = {false};
  uint8_t cryWindowPos = 0;
  uint8_t cryWindowCount = 0;
  uint8_t cryWindowValid = 0;
  float lastCryConf = 0.0f;
  uint32_t tUltimaCaptura = 0;
  const uint32_t CAPTURE_COOLDOWN_MS = 9000UL;

  while (true) {
    // ISR de reset (botão GPIO0)
    if (flagResetISR) {
      // Ignora pulsos espúrios durante o boot/rede inicial.
      if ((millis() - gBootMs) < 15000UL) {
        flagResetISR = false;
      } else {
        flagResetISR = false;
        gState.choroCritico = false;
        gState.ciclosSilencio = 0;
        gState.ciclosTotais = 0;
        gState.indiceAtual = 0;
        for (int i = 0; i < TAMANHO_BUFFER; i++)
          gState.historico[i] = SILENCIO_RUIDO;
        memset(cryWindow, 0, sizeof(cryWindow));
        cryWindowPos = 0;
        cryWindowCount = 0;
        cryWindowValid = 0;
        lastCryConf = 0.0f;
        AudioPlayer::parar();
        gWebState.audio_ativo = false;
        gWebState.remote_busy = false;
        strlcpy(gWebState.pipeline, "idle", sizeof(gWebState.pipeline));
        strlcpy(gWebState.result_label, "noise", sizeof(gWebState.result_label));
        gWebState.result_conf = 0.0f;
        strlcpy(gWebState.estado, "calmo", sizeof(gWebState.estado));
        // O OLED já é atualizado pela TaskHMI; evitar acesso concorrente aqui.
        LogManager::info("[ISR] Estado resetado via botão");
      }
    }

    while (xQueueReceive(qRemoteResult, &rr, 0) == pdTRUE) {
      _aplicarResultadoRemoto(rr);
    }

    // Aguarda resultado da IA (timeout curto para manter task responsiva)
    if (xQueueReceive(qResultadoIA, &ir, pdMS_TO_TICKS(500)) != pdTRUE) {
      continue;
    }

    uint64_t t0 = esp_timer_get_time();

    CryConfig &cfg = ConfigManager::get();
    int gatilhoSilencio = cfg.gatilho_silencio;
    if (gatilhoSilencio < 1) gatilhoSilencio = 1;
    if (gatilhoSilencio > TAMANHO_BUFFER) gatilhoSilencio = TAMANHO_BUFFER;

    const bool cryDetectado = (strcmp(ir.label, LABEL_CRY) == 0);
    const bool cryValido = cryDetectado && (ir.confianca >= 0.75f);
    if (cryWindowCount < CRY_WINDOW_SIZE) {
      cryWindow[cryWindowPos] = cryValido;
      if (cryValido) cryWindowValid++;
      cryWindowCount++;
    } else {
      if (cryWindow[cryWindowPos] && cryWindowValid > 0) cryWindowValid--;
      cryWindow[cryWindowPos] = cryValido;
      if (cryValido) cryWindowValid++;
    }
    cryWindowPos = (uint8_t)((cryWindowPos + 1) % CRY_WINDOW_SIZE);

    if (cryValido) {
      lastCryConf = ir.confianca;
      gState.ciclosSilencio = 0;
      if (!gState.choroCritico) {
        strlcpy(gWebState.estado, "monitorando", sizeof(gWebState.estado));
      }
    } else {
      if (cryWindowValid == 0) lastCryConf = 0.0f;
      gState.ciclosSilencio++;
      if (gState.choroCritico && gState.ciclosSilencio >= gatilhoSilencio &&
          !AudioPlayer::clipRemotoOcupado()) {
        gState.choroCritico = false;
        gState.ciclosSilencio = 0;
        strlcpy(gWebState.estado, "calmo", sizeof(gWebState.estado));
        strlcpy(gWebState.pipeline, "idle", sizeof(gWebState.pipeline));
        strlcpy(gWebState.ultimo_alerta, "CALMO", sizeof(gWebState.ultimo_alerta));
        strlcpy(gWebState.result_label, LABEL_NOISE, sizeof(gWebState.result_label));
        gWebState.result_conf = 0.0f;
        for (int i = 0; i < 4; i++) gWebState.scores[i] = 0.0f;
        gWebState.scores[2] = 1.0f;
        gWebState.audio_ativo = false;
        memset(cryWindow, 0, sizeof(cryWindow));
        cryWindowPos = 0;
        cryWindowCount = 0;
        cryWindowValid = 0;
        lastCryConf = 0.0f;
        AudioPlayer::parar();

        auto sens = Sensores::ler();
        FirebaseMsg fm = {"CALMO", 0, sens.temperatura, sens.umidade, true};
        xQueueSend(qFirebase, &fm, 0);
        LogManager::info("[TaskDecisao] Crise encerrada por silencio local");
      }
      if (!gState.choroCritico && !AudioPlayer::clipRemotoOcupado()) {
        strlcpy(gWebState.estado, "calmo", sizeof(gWebState.estado));
      }
    }

    const uint8_t minValidWindow = CRY_WINDOW_MIN_VALID;
    const bool janelaCompleta = (cryWindowCount >= CRY_WINDOW_SIZE);
    const bool janelaValida = janelaCompleta && (cryWindowValid >= minValidWindow);
    float triggerConf = lastCryConf;
    if (triggerConf < 0.0f) triggerConf = 0.0f;
    if (triggerConf > 1.0f) triggerConf = 1.0f;

    const bool cooldownOk = (millis() - tUltimaCaptura) >= CAPTURE_COOLDOWN_MS;
    if (janelaValida && cooldownOk && !AudioPlayer::clipRemotoOcupado()) {
      if (AudioPlayer::solicitarClipRemoto(triggerConf)) {
        const uint8_t windowCountBefore = cryWindowCount;
        const uint8_t windowValidBefore = cryWindowValid;
        tUltimaCaptura = millis();
        memset(cryWindow, 0, sizeof(cryWindow));
        cryWindowPos = 0;
        cryWindowCount = 0;
        cryWindowValid = 0;
        lastCryConf = 0.0f;
        strlcpy(gWebState.pipeline, "recording", sizeof(gWebState.pipeline));
        strlcpy(gWebState.estado, "monitorando", sizeof(gWebState.estado));
        LogManager::infof(
            "[TaskDecisao] Gravacao remota iniciada (trigger %.0f%%, janela=%u, validos=%u/%u, min=%u)",
            triggerConf * 100.0f, (unsigned int)windowCountBefore,
            (unsigned int)windowValidBefore, (unsigned int)CRY_WINDOW_SIZE,
            (unsigned int)minValidWindow);
      }
    }

    float execMs = (float)(esp_timer_get_time() - t0) / 1000.0f;
    Serial.printf("[TaskDecisao] Tempo de execucao: %.1fms\n", execMs);

    // Atualiza estado web
    gWebState.uptime_s = esp_timer_get_time() / 1000000ULL;
    gWebState.heap = ESP.getFreeHeap();
    gWebState.heap_min = gState.heapMin;
    gWebState.psram = ESP.getFreePsram();
    gWebState.psram_tot = ESP.getPsramSize();

    if (!gState.choroCritico && strcmp(gWebState.pipeline, "result") != 0) {
      strlcpy(gWebState.label,
              (strcmp(ir.label, LABEL_CRY) == 0) ? "choro" : LABEL_NOISE,
              sizeof(gWebState.label));
    }
  }
}

// =============================================================================
// SETUP PRINCIPAL
// =============================================================================
void setup() {
  Serial.begin(115200);
  vTaskDelay(pdMS_TO_TICKS(500));

  Serial.println("\n=========================================");
  Serial.println(" CrySense AI v2.0 — Inicializando...");
  Serial.println("=========================================");

  // --- PSRAM ---
  if (psramInit()) {
    Serial.printf("[BOOT] PSRAM: %u KB livres\n", ESP.getFreePsram() / 1024);
  } else {
    Serial.println("[BOOT] PSRAM: não disponível!");
  }

  // --- Buffers em PSRAM ---
  inference_buffer =
      (float *)ps_malloc(EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE * sizeof(float));
  if (!inference_buffer) {
    Serial.println("[BOOT] ERRO: falha ao alocar buffers em PSRAM!");
    while (true)
      ; // Halt
  }
  memset(inference_buffer, 0,
         EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE * sizeof(float));

  // --- Config (NVS) ---
  ConfigManager::load();
  CryConfig &cfg = ConfigManager::get();

  // --- Log (SPIFFS com buffer em RAM — não escreve Flash por ciclo) ---
  LogManager::begin();
  LogManager::info("CrySense AI v2.0 iniciando...");

  // --- Hardware ---
  Sensores::begin();

  // --- OLED ---
  Display::begin();
  Display::mostrarBoot();
  vTaskDelay(pdMS_TO_TICKS(800));

  gBootMs = millis();

  // --- Interrupt hardware (GPIO 0 = botão BOOT) ---
  pinMode(0, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(0), isrBotaoReset, FALLING);

  // --- WiFi ---
  Display::mostrarWiFi(true, cfg.wifi_ssid);
  Serial.printf("[WiFi] Conectando a %s...\n", cfg.wifi_ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
  // BUGFIX: setAutoReconnect garante reconexão automática sem loop manual
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false); // Não grava credenciais na Flash a cada conexão!

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 30) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print('.');
    tentativas++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Conectado! IP: %s\n",
                  WiFi.localIP().toString().c_str());
    Display::mostrarWiFi(false, cfg.wifi_ssid,
                         WiFi.localIP().toString().c_str());
    strncpy(gWebState.ip, WiFi.localIP().toString().c_str(),
            sizeof(gWebState.ip));
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
    
    // Inicia DNS na porta 53 redirecionando qualquer URL (*) pro IP do ESP32
    dnsServer.start(53, "*", IP);
  }
  vTaskDelay(pdMS_TO_TICKS(1000));

  // --- Firebase ---
  Firebase::begin(cfg.firebase_url, cfg.firebase_auth);

  // --- Web Server ---
  WebServer::begin(&gWebState, &cfg);
  if (gWebState.wifi_ok) {
    LogManager::infof("Web: http://%s", WiFi.localIP().toString().c_str());
  } else {
    LogManager::infof("Web (AP): http://%s",
                      WiFi.softAPIP().toString().c_str());
  }

  // --- Arduino OTA (Over-The-Air) ---
  ArduinoOTA.setHostname("CrySense-AI");
  ArduinoOTA.setRebootOnSuccess(true);

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("\n[OTA] Start updating " + type);
    
    // BUGFIX: Suspende as rotinas pesadas para evitar que a CPU seja sequestrada
    // Isto garante 100% de estabilidade de rede e flash durante o OTA!
    if (hTaskIA) vTaskSuspend(hTaskIA);
    if (hTaskDecisao) vTaskSuspend(hTaskDecisao);
    if (hTaskAudio) vTaskSuspend(hTaskAudio);
    if (hTaskPlayer) vTaskSuspend(hTaskPlayer);
    if (hTaskHMI) vTaskSuspend(hTaskHMI);
    if (hTaskIOT) vTaskSuspend(hTaskIOT);
    if (hTaskRemote) vTaskSuspend(hTaskRemote);
    
    AudioPlayer::parar();
  });

  ArduinoOTA.onEnd([]() { Serial.println("\n[OTA] End - Reiniciando..."); });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    uint8_t percent = (progress / (total / 100));
    Serial.printf("Progress: %u%%\r", percent);
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR)
      Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR)
      Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR)
      Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR)
      Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR)
      Serial.println("End Failed");
  });

  ArduinoOTA.begin();
  gOtaReady = true;
  LogManager::info("[OTA] Servico de gravacao sem fio iniciado!");

  // --- Estado inicial ---
  gState.heapMin = ESP.getFreeHeap();
  for (int i = 0; i < TAMANHO_BUFFER; i++)
    gState.historico[i] = SILENCIO_RUIDO;
  strlcpy(gWebState.estado, "calmo", sizeof(gWebState.estado));
  strlcpy(gWebState.pipeline, "idle", sizeof(gWebState.pipeline));
  strlcpy(gWebState.result_label, "noise", sizeof(gWebState.result_label));
  gWebState.result_conf = 0.0f;
  gWebState.remote_busy = false;
  AudioPlayer::setVolume(cfg.volume_audio);
  gWebState.flash_used = ESP.getSketchSize();
  gWebState.flash_tot = ESP.getFlashChipSize();
  gWebState.audio_spiffs_size = (float)AudioPlayer::tamanhoArquivo();

  // --- Sincronizadores inter-task ---
  gMutexState = xSemaphoreCreateMutex();
  semAudioPronto = xSemaphoreCreateBinary();
  qResultadoIA = xQueueCreate(2, sizeof(InferenceResult));
  qFirebase = xQueueCreate(5, sizeof(FirebaseMsg));
  qAudioUpload = xQueueCreate(1, sizeof(AudioClipMsg));
  qRemoteResult = xQueueCreate(2, sizeof(RemoteClassResult));

  // ==========================================================================
  // CRIAÇÃO DAS TASKS FREERTOS
  // ==========================================================================
  AudioPlayer::beginTasks(&hTaskAudio, &hTaskPlayer); // TaskAudio + TaskPlayer
  Display::beginTask(&hTaskHMI);                      // TaskHMI
  Firebase::beginTask(&hTaskIOT);                     // TaskIOT
  RemoteClassifier::beginTask(&hTaskRemote);          // TaskRemoteClassifier

  // Lógica e IA (no documento .ino)
  xTaskCreatePinnedToCore(TaskIA, "TaskIA", 16384, nullptr, 4, &hTaskIA, 1);
  xTaskCreatePinnedToCore(TaskDecisao, "TaskDecisao", 12288, nullptr, 3,
                          &hTaskDecisao, 0);

  LogManager::info("=== CrySense AI v2.0 — Sistema Pronto ===");
  Serial.println("\n[BOOT] Todas as tasks iniciadas. Sistema em operação.\n");
}

// =============================================================================
// LOOP — Gerencia OTA e flush periódico de logs (protege a Flash)
// =============================================================================
void loop() {
  dnsServer.processNextRequest();

  if (gOtaReady) {
    ArduinoOTA.handle();
  }

  // BUGFIX: Descarrega o buffer de log RAM → SPIFFS apenas a cada 60s.
  // BUGFIX: Flash Wear Leveling. O usuário solicitou que não houvesse escrita
  // "em loop" na memória flash. O LogManager::flush() foi removido daqui.
  // Os logs de 60s rotativos ficarão apenas no buffer circular em RAM,
  // preservando fisicamente a vida útil da NAND Flash do ESP32.
  // Se o usuário pedir o CSV pelo painel, os dados da RAM serão servidos.

  // O usuário relatou que o consumo cravado em 100% no Core 1 devia ser
  // reduzido. Voltando para um micro-delay de 10ms. É tempo mais do que
  // suficiente para o FreeRTOS alimentar a thread "IDLE", zerando o bug do
  // "100% CPU", sem causar absolutamente nenhum atraso nocivo para a amostragem
  // de áudio (porque a leitura I2S tem DMA buffer e vive numa task isolada
  // prioritária).
  vTaskDelay(pdMS_TO_TICKS(10));
}
