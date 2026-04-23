#define EI_CLASSIFIER_ALLOCATION_STATIC 0
#define EI_CLASSIFIER_ALLOCATION_PSRAM 1
#define EI_CLASSIFIER_TFLITE_ENABLE_PSRAM 1
#include <CrySense_AI_inferencing.h>
#include "libs/CrySense_trigger_inferencing/src/model-parameters/model_variables.h"
#include "driver/i2s.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <WiFi.h>

#define ENABLE_OTA_RUNTIME 0
#define VERBOSE_IA_TIMING 0
#define ENABLE_TRIGGER_STAGE 1

// Debug de heap para detectar corrupção
#include "heap_debug.h"

#include "audio_player.h"
#include "config_manager.h"
#include "display_manager.h"
#include "firebase_manager.h"
#include "log_manager.h"
#include "secrets.h"
#include "sensor_bme280.h"
#include "web_server.h"

#define CICLOS_PARA_RESETAR 120
#define ALERT_COOLDOWN_MS   30000UL


// =============================================================================
// ESTADO COMPARTILHADO (Instanciação das globais do config_manager)
// =============================================================================
SistemaState gState;
SemaphoreHandle_t gMutexState = nullptr;
WebDashboardState gWebState = {};
AnalyticsState gAnalytics;

SemaphoreHandle_t semAudioPronto = nullptr;
QueueHandle_t qResultadoIA = nullptr;
QueueHandle_t qFirebase = nullptr;
static bool gOtaReady = false;

// Task handles para controle durante OTA
static TaskHandle_t hTaskIA = nullptr;
static TaskHandle_t hTaskDecisao = nullptr;
static TaskHandle_t hTaskAudio = nullptr;
static TaskHandle_t hTaskPlayer = nullptr;
static TaskHandle_t hTaskHMI = nullptr;
static TaskHandle_t hTaskIOT = nullptr;

// =============================================================================
// BUFFER DE INFERÊNCIA (em PSRAM)
// =============================================================================
float *inference_buffer = nullptr;

int get_signal_data_callback(size_t offset, size_t length, float *out_ptr) {
  if (!inference_buffer || !out_ptr)
    return -1;
  if ((offset + length) > EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE)
    return -1;
  memcpy(out_ptr, inference_buffer + offset, length * sizeof(float));
  return 0;
}

static inline EI_IMPULSE_ERROR run_trigger_model(signal_t *signal,
                                                 ei_impulse_result_t *result) {
  return run_classifier(&ei_default_impulse_trigger, signal, result, false);
}

static inline EI_IMPULSE_ERROR run_type_model(signal_t *signal,
                                              ei_impulse_result_t *result) {
  return run_classifier(&ei_default_impulse, signal, result, false);
}

// =============================================================================
// INTERRUPÇÃO DE HARDWARE — GPIO 0 (Botão BOOT)
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
// =============================================================================
void TaskIA(void *pv) {
  LogManager::info("[TaskIA] Iniciada no Core 1");
  while (true) {
    if (!SHOULD_RUN_TASKIA) {
      vTaskDelay(pdMS_TO_TICKS(1000)); // Desabilitada — sleep
      continue;
    }
    
    if (xSemaphoreTake(semAudioPronto, pdMS_TO_TICKS(1500)) != pdTRUE) {
      static uint32_t lastWaitLogMs = 0;
      uint32_t nowMs = millis();
      if ((nowMs - lastWaitLogMs) >= 5000UL) {
        lastWaitLogMs = nowMs;
        LogManager::info("[TaskIA] Aguardando frame de audio (semAudioPronto)");
      }
      continue;
    }

    uint32_t freeHeapBefore = ESP.getFreeHeap();
    uint64_t t0 = esp_timer_get_time();

    float bufEnergy = 0.0f;
    for (int i = 0; i < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; i++)
      bufEnergy += inference_buffer[i] * inference_buffer[i];
    float bufRMS = sqrtf(bufEnergy / EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);
    if (bufRMS < 0.005f) {
      // Buffer praticamente vazio — injetar silêncio sem inferir
      InferenceResult irZero = {};
      strlcpy(irZero.label, LABEL_NOISE, sizeof(irZero.label));
      irZero.confianca = 1.0f;
      irZero.isSilencioReal = true;
      irZero.infMs = 0.0f;
      if (xQueueSend(qResultadoIA, &irZero, 0) != pdTRUE) {
        LogManager::warning("[TaskIA] Fila qResultadoIA cheia (silencio)");
      }
    #if VERBOSE_IA_TIMING
      Serial.printf("[TaskIA] Buffer vazio (RMS=%.4f) — descartado\n", bufRMS);
    #endif
      uint32_t freeHeapAfter = ESP.getFreeHeap();
      if ((freeHeapBefore - freeHeapAfter) > 1024) {
        LogManager::warningf("[TaskIA-LEAK] Heap drop detectado: %u -> %u KB",
                             freeHeapBefore / 1024, freeHeapAfter / 1024);
      }
      continue;
    }

    signal_t signal;
    signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
    signal.get_data = &get_signal_data_callback;

    InferenceResult ir;
    ir.confianca = 0;
    ir.isSilencioReal = false;
    strlcpy(ir.label, LABEL_NOISE, sizeof(ir.label));
    for (int i = 0; i < 4; i++)
      ir.scores[i] = 0.0f;

    CryConfig &cfg = ConfigManager::get();
    float minCry = cfg.confianca_minima;
    if (minCry < 0.60f)
      minCry = 0.60f;

    float scoreCry = 1.0f;
    float scoreNoise = 0.0f;
    bool hasCry = true;

#if ENABLE_TRIGGER_STAGE
    ei_impulse_result_t triggerResult = {0};
    EI_IMPULSE_ERROR err = run_trigger_model(&signal, &triggerResult);
    if (err != EI_IMPULSE_OK) {
      LogManager::errorf("[TaskIA] Erro trigger: %d", err);
      continue;
    }

    scoreCry = 0.0f;
    scoreNoise = 1.0f;
    const size_t triggerCount = sizeof(triggerResult.classification) /
                                sizeof(triggerResult.classification[0]);
    for (size_t i = 0; i < triggerCount; i++) {
      const char *lbl = triggerResult.classification[i].label;
      const float sc = triggerResult.classification[i].value;
      if (!lbl || lbl[0] == '\0')
        continue;
      if (strcmp(lbl, LABEL_CRY) == 0)
        scoreCry = sc;
      else if (strcmp(lbl, LABEL_NOISE) == 0)
        scoreNoise = sc;
    }

    hasCry = (scoreCry >= scoreNoise) && (scoreCry >= minCry);
#else
    // Bypass temporario do modelo trigger por instabilidade (abort no Core 1).
    // Usa gate por RMS para manter o comportamento de silencio/ruido.
    const float rmsGate = cfg.rms_threshold * 1.15f;
    if (bufRMS < rmsGate) {
      hasCry = false;
      scoreCry = 0.0f;
      scoreNoise = 1.0f;
    }
#endif

    if (!hasCry) {
      ir.confianca = scoreNoise;
      strlcpy(ir.label, LABEL_NOISE, sizeof(ir.label));
      ir.scores[2] = 1.0f;
      Serial.printf("[IA-TRIGGER] cry=%.2f noise=%.2f -> noise\n", scoreCry,
                    scoreNoise);
    } else {
      ei_impulse_result_t typeResult = {0};
      EI_IMPULSE_ERROR err = run_type_model(&signal, &typeResult);
      if (err != EI_IMPULSE_OK) {
        LogManager::errorf("[TaskIA] Erro classifier tipo: %d", err);
        continue;
      }

      float scoreColica = 0.0f;
      float scoreFome = 0.0f;
      float scoreSono = 0.0f;

      const size_t typeCount = sizeof(typeResult.classification) /
                               sizeof(typeResult.classification[0]);
      for (size_t i = 0; i < typeCount; i++) {
        const char *lbl = typeResult.classification[i].label;
        const float sc = typeResult.classification[i].value;
        if (!lbl || lbl[0] == '\0')
          continue;
        if (strcmp(lbl, LABEL_COLIC) == 0)
          scoreColica = sc;
        else if (strcmp(lbl, LABEL_HUNGER) == 0)
          scoreFome = sc;
        else if (strcmp(lbl, LABEL_SLEEP) == 0)
          scoreSono = sc;
      }
      // ==========================================================
      // ==========================================================
      float melhorScoreReal = scoreColica;
      if (scoreFome > melhorScoreReal)  melhorScoreReal = scoreFome;
      if (scoreSono > melhorScoreReal)  melhorScoreReal = scoreSono;

      bool rejeitarComoFalsoPositivo =
          (scoreCry < 0.85f && melhorScoreReal < 0.65f) ||
          (melhorScoreReal < 0.40f);

      if (rejeitarComoFalsoPositivo) {
        ir.confianca = scoreNoise > 0 ? scoreNoise : 1.0f;
        strlcpy(ir.label, LABEL_NOISE, sizeof(ir.label));
        Serial.printf("[IA-FP] Rejeitado: cry=%.2f melhorTipo=%.2f -> noise\n",
                      scoreCry, melhorScoreReal);
      } else {
        const char *classMax;
        float confReal;
        if ((scoreColica * 1.85f) >= scoreFome && (scoreColica * 1.85f) >= scoreSono) {
          classMax = LABEL_COLIC;
          confReal  = scoreColica;
        } else if (scoreFome >= scoreSono) {
          classMax = LABEL_HUNGER;
          confReal  = scoreFome;
        } else {
          classMax = LABEL_SLEEP;
          confReal  = scoreSono;
        }
        ir.confianca = confReal;
        strlcpy(ir.label, classMax, sizeof(ir.label));
      }

      const float somaTipos = scoreColica + scoreFome + scoreSono;
      if (somaTipos > 0.001f) {
        ir.scores[0] = scoreColica / somaTipos;
        ir.scores[1] = scoreFome / somaTipos;
        ir.scores[2] = 0.0f;
        ir.scores[3] = scoreSono / somaTipos;
      }

      Serial.printf("[IA-TRIGGER] cry=%.2f noise=%.2f -> cry | [IA-TYPE] "
                    "c=%.2f h=%.2f s=%.2f | label=%s\n",
                    scoreCry, scoreNoise, scoreColica, scoreFome, scoreSono, ir.label);
    }

    float infMs = (float)(esp_timer_get_time() - t0) / 1000.0f;
    ir.infMs = infMs;

    if (xQueueSend(qResultadoIA, &ir, 0) != pdTRUE) {
      LogManager::warning("[TaskIA] Fila qResultadoIA cheia (resultado IA)");
    }

    strlcpy(gWebState.label, ir.label, sizeof(gWebState.label));
    gWebState.confianca = ir.confianca;
    gWebState.inf_ms = ir.infMs;
    for (int i = 0; i < 4; i++)
      gWebState.scores[i] = ir.scores[i];

    uint32_t fh = ESP.getFreeHeap();
    if (fh < gState.heapMin)
      gState.heapMin = fh;

#if VERBOSE_IA_TIMING
    Serial.printf("[IA] %s %.0f%% (%.1fms)\n", ir.label, ir.confianca * 100.0f,
                  infMs);
#endif

    uint32_t freeHeapAfter = ESP.getFreeHeap();
    if ((freeHeapBefore - freeHeapAfter) > 1024) {
      LogManager::warningf("[TaskIA-LEAK] Heap drop: %u -> %u KB",
                           freeHeapBefore / 1024, freeHeapAfter / 1024);
    }
  }
}

// =============================================================================
static void registrar(TipoChoro t) {
  gState.historico[gState.indiceAtual] = t;
  gState.indiceAtual = (gState.indiceAtual + 1) % TAMANHO_BUFFER;
  if (gState.ciclosTotais < TAMANHO_BUFFER)
    gState.ciclosTotais++;
}

static void dispararAlerta(const char *tipo, float conf) {
  strlcpy(gState.ultimoAlerta, tipo, sizeof(gState.ultimoAlerta));
  gState.tsAlerta = millis();
  gState.choroCritico = true;
  strlcpy(gWebState.estado, "crise", sizeof(gWebState.estado));
  strlcpy(gWebState.ultimo_alerta, tipo, sizeof(gWebState.ultimo_alerta));
  auto sens = Sensores::ler();
  gAnalytics.registrar(tipo, (uint8_t)(conf * 100), sens.temperatura,
                       sens.umidade, esp_timer_get_time() / 1000000ULL);

  const char *titulo = strcmp(tipo, "COLICA") == 0 ? "COLICA!"
                       : strcmp(tipo, "FOME") == 0 ? "FOME!"
                                                   : "SONO!";
  const char *subtitulo = strcmp(tipo, "COLICA") == 0 ? "Ruido branco ativo"
                          : strcmp(tipo, "FOME") == 0 ? "Precisa mamar"
                                                      : "Quer dormir";

  Display::mostrarAlerta(titulo, subtitulo, conf, strcmp(tipo, "COLICA") == 0);
  LogManager::alertaf("ALERTA: %s (conf %.0f%%)", tipo, conf * 100);

  CryConfig &cfg = ConfigManager::get();
  AudioPlayer::setVolume(cfg.volume_audio);
  AudioPlayer::iniciar(60);
  gWebState.audio_ativo = true;

  FirebaseMsg fm;
  strlcpy(fm.tipo, tipo, sizeof(fm.tipo));
  fm.confianca = conf;
  fm.acalmado = false;
  fm.temp = sens.temperatura;
  fm.umid = sens.umidade;
  xQueueSend(qFirebase, &fm, 0);
}

void TaskDecisao(void *pv) {
  LogManager::info("[TaskDecisao] Iniciada no Core 0");
  InferenceResult ir;

  while (true) {
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
      AudioPlayer::parar();
      gWebState.audio_ativo = false;
      strlcpy(gWebState.estado, "calmo", sizeof(gWebState.estado));
      // O OLED já é atualizado pela TaskHMI; evitar acesso concorrente aqui.
      LogManager::info("[ISR] Estado resetado via botão");
      }
    }

    if (xQueueReceive(qResultadoIA, &ir, pdMS_TO_TICKS(2000)) != pdTRUE)
      continue;

    uint64_t t0 = esp_timer_get_time();

    CryConfig &cfg = ConfigManager::get();

    TipoChoro prev = SILENCIO_RUIDO;
    if (strcmp(ir.label, LABEL_NOISE) != 0) {
      if (strcmp(ir.label, LABEL_HUNGER) == 0)
        prev = FOME;
      else if (strcmp(ir.label, LABEL_COLIC) == 0)
        prev = COLICA_DOR;
      else if (strcmp(ir.label, LABEL_SLEEP) == 0)
        prev = SONO;
    }

    registrar(prev);

    int janela = min(gState.ciclosTotais, TAMANHO_BUFFER);
    if (janela == 0)
      continue;

    int cFome = 0, cColica = 0, cSilencio = 0, cSono = 0;
    float wFome = 0.0f, wColica = 0.0f, wSono = 0.0f;
    int start = (gState.indiceAtual - 1 + TAMANHO_BUFFER) % TAMANHO_BUFFER;
    if (ir.isSilencioReal)
      cSilencio += 2;

    for (int i = 0; i < janela; i++) {
      int idx = (start - i + TAMANHO_BUFFER) % TAMANHO_BUFFER;
      float w = 1.0f + 0.15f * (float)(janela - 1 - i);
      switch (gState.historico[idx]) {
      case FOME:
        cFome++;
        wFome += w;
        break;
      case COLICA_DOR:
        cColica++;
        wColica += w;
        break;
      case SONO:
        cSono++;
        wSono += w;
        break;
      default:
        cSilencio++;
        break;
      }
    }

    // ===============================
    // ===============================
    int gatilhoChoro = cfg.gatilho_decisao;
    if (gatilhoChoro < 4)
      gatilhoChoro = 4;
    if (gatilhoChoro > TAMANHO_BUFFER)
      gatilhoChoro = TAMANHO_BUFFER;

    int gatilhoS = cfg.gatilho_silencio;
    if (gatilhoS > 5)
      gatilhoS = 5;

    int gatilhoManter = gatilhoChoro - 2;
    if (gatilhoManter < 3)
      gatilhoManter = 3;

    int totalChoros = cColica + cFome + cSono;
    float totalChorosPonderado = wColica + wFome + wSono;
    float maxPonderado = wColica;
    if (wFome > maxPonderado) maxPonderado = wFome;
    if (wSono > maxPonderado) maxPonderado = wSono;
    bool tipoConsistente = (totalChorosPonderado > 0.001f) &&
                           ((maxPonderado / totalChorosPonderado) >= 0.45f);

    bool choroConfirmado = (totalChoros >= gatilhoChoro) && tipoConsistente;
    bool manterCrise = (totalChoros >= gatilhoManter);

    if (gState.choroCritico) {
      if (manterCrise) {
        gState.ciclosSilencio = 0;
        strlcpy(gWebState.estado, "crise", sizeof(gWebState.estado));
      } else if ((cSilencio + cSono) >= gatilhoS) {
        gState.ciclosSilencio++;
        if (gState.ciclosSilencio >= CICLOS_PARA_RESETAR) {
          gState.choroCritico = false;
          gState.ciclosSilencio = 0;
          for (int i = 0; i < TAMANHO_BUFFER; i++)
            gState.historico[i] = SILENCIO_RUIDO;
          AudioPlayer::parar();
          gWebState.audio_ativo = false;
          strlcpy(gWebState.estado, "calmo", sizeof(gWebState.estado));
          LogManager::info("Bebe se acalmou. Sistema rearmado.");
          FirebaseMsg fm = {"CALMO", 0, 0, 0, true};
          xQueueSend(qFirebase, &fm, 0);
        }
      }
    } else if (choroConfirmado) {
      uint32_t agoraMs = millis();
      if (gState.tsAlerta != 0 && (agoraMs - gState.tsAlerta) < ALERT_COOLDOWN_MS) {
        strlcpy(gWebState.estado, "monitorando", sizeof(gWebState.estado));
      } else {
        float conf = ir.confianca;
        if (wColica >= wFome && wColica >= wSono) {
          dispararAlerta("COLICA", conf);
        } else if (wFome >= wColica && wFome >= wSono) {
          dispararAlerta("FOME", conf);
        } else {
          dispararAlerta("SONO", conf);
        }
      }
    } else if ((cSilencio + cSono) >= gatilhoS) {
      strlcpy(gWebState.estado, "monitorando", sizeof(gWebState.estado));
    }

  #if VERBOSE_IA_TIMING
    float execMs = (float)(esp_timer_get_time() - t0) / 1000.0f;
    Serial.printf("[TaskDecisao] Tempo de execucao: %.1fms\n", execMs);
  #endif

    gWebState.uptime_s = esp_timer_get_time() / 1000000ULL;
    gWebState.heap = ESP.getFreeHeap();
    gWebState.heap_min = gState.heapMin;
    gWebState.psram = ESP.getFreePsram();
    gWebState.psram_tot = ESP.getPsramSize();

    // ==========================================================
    // ==========================================================
    if (!gState.choroCritico) {
      if (strcmp(ir.label, LABEL_NOISE) != 0) {
        strlcpy(gWebState.label, "choro", sizeof(gWebState.label));
      } else {
        strlcpy(gWebState.label, LABEL_NOISE, sizeof(gWebState.label));
      }
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

  if (psramInit()) {
    Serial.printf("[BOOT] PSRAM: %u KB livres\n", ESP.getFreePsram() / 1024);
  } else {
    Serial.println("[BOOT] PSRAM: não disponível!");
  }

  Serial.printf("[BOOT] Heap antes alloc IA: DRAM=%u KB | PSRAM=%u KB\n",
                ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024);

  inference_buffer =
      (float *)ps_malloc(EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE * sizeof(float));
  if (!inference_buffer) {
    Serial.println("[BOOT] ERRO: falha ao alocar buffers em PSRAM!");
    while (true)
      ;
  }
  memset(inference_buffer, 0,
         EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE * sizeof(float));

  Serial.printf("[BOOT] Heap apos alloc IA: DRAM=%u KB | PSRAM=%u KB\n",
                ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024);

  ConfigManager::load();
  CryConfig &cfg = ConfigManager::get();

  LogManager::begin();
  LogManager::info("CrySense AI v2.0 iniciando...");

  Sensores::begin();

  Display::begin();
  Display::mostrarBoot();
  vTaskDelay(pdMS_TO_TICKS(800));

  gBootMs = millis();

  pinMode(0, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(0), isrBotaoReset, FALLING);

  Display::mostrarWiFi(true, cfg.wifi_ssid);
  Serial.printf("[WiFi] Conectando a %s...\n", cfg.wifi_ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

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
        strlcpy(gWebState.ip, WiFi.localIP().toString().c_str(),
          sizeof(gWebState.ip));
    gWebState.wifi_ok = true;
    LogManager::infof("WiFi OK: %s", WiFi.localIP().toString().c_str());
    esp_wifi_set_ps(WIFI_PS_NONE);
  } else {
    Serial.println("\n[WiFi] Falha! Iniciando modo AP (CrySense-Setup).");
    LogManager::warning("[WiFi] Falha — iniciando AP");
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("CrySense-Setup", "12345678");
    IPAddress IP = WiFi.softAPIP();
    Serial.printf("[WiFi] AP IP: %s\n", IP.toString().c_str());
    Display::mostrarWiFi(false, "CrySense-Setup", IP.toString().c_str());
    strlcpy(gWebState.ip, IP.toString().c_str(), sizeof(gWebState.ip));
    gWebState.wifi_ok = false;
  }
  vTaskDelay(pdMS_TO_TICKS(1000));

  Firebase::begin(cfg.firebase_url, cfg.firebase_auth);

  WebServer::begin(&gWebState, &cfg);
  if (gWebState.wifi_ok) {
    LogManager::infof("Web: http://%s", WiFi.localIP().toString().c_str());
  } else {
    LogManager::infof("Web (AP): http://%s",
                      WiFi.softAPIP().toString().c_str());
  }

#if ENABLE_OTA_RUNTIME
  ArduinoOTA.setHostname("CrySense-AI");
  ArduinoOTA.setRebootOnSuccess(true);

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("\n[OTA] Start updating " + type);

    if (hTaskIA) vTaskSuspend(hTaskIA);
    if (hTaskDecisao) vTaskSuspend(hTaskDecisao);
    if (hTaskAudio) vTaskSuspend(hTaskAudio);
    if (hTaskPlayer) vTaskSuspend(hTaskPlayer);
    if (hTaskHMI) vTaskSuspend(hTaskHMI);
    if (hTaskIOT) vTaskSuspend(hTaskIOT);

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
#else
  gOtaReady = false;
  LogManager::warning("[OTA] Desativado temporariamente para estabilizar rede.");
#endif

  gState.heapMin = ESP.getFreeHeap();
  for (int i = 0; i < TAMANHO_BUFFER; i++)
    gState.historico[i] = SILENCIO_RUIDO;
  strlcpy(gWebState.estado, "calmo", sizeof(gWebState.estado));
  AudioPlayer::setVolume(cfg.volume_audio);
  gWebState.flash_used = ESP.getSketchSize();
  gWebState.flash_tot = ESP.getFlashChipSize();
  gWebState.audio_spiffs_size = (float)AudioPlayer::tamanhoArquivo();

  gMutexState = xSemaphoreCreateMutex();
  semAudioPronto = xSemaphoreCreateBinary();
  qResultadoIA = xQueueCreate(2, sizeof(InferenceResult));
  qFirebase = xQueueCreate(5, sizeof(FirebaseMsg));

  // ==========================================================================
  // CRIAÇÃO DAS TASKS FREERTOS
  // ==========================================================================
  AudioPlayer::beginTasks(&hTaskAudio, &hTaskPlayer);
  Display::beginTask(&hTaskHMI);
  Firebase::beginTask(&hTaskIOT);

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
  if (gOtaReady) {
    ArduinoOTA.handle();
  }


  vTaskDelay(pdMS_TO_TICKS(10));
}
