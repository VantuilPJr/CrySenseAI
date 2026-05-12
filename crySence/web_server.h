#pragma once
#include "config_manager.h"
#include "history_manager.h"
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <esp_timer.h>
#include <WiFi.h>
#include <Update.h>

extern TaskHandle_t hTaskIA;
extern TaskHandle_t hTaskDecisao;
extern TaskHandle_t hTaskAudio;
extern TaskHandle_t hTaskPlayer;
extern TaskHandle_t hTaskHMI;
extern TaskHandle_t hTaskIOT;
extern TaskHandle_t hTaskRemote;
extern TaskHandle_t hTaskLoop;

extern volatile bool gOtaFinished;
extern volatile bool gOtaError;
extern volatile uint32_t gOtaIndex;
extern volatile bool gOtaInProgress;
extern volatile uint32_t gOtaLastActivity;
extern SemaphoreHandle_t semAudioPronto;

void suspendAllTasks();
void resumeAllTasks();
void killAllAppTasks();

namespace WebServer {

static AsyncWebServer _server(80);
static bool _started = false;
static WebDashboardState *_state = nullptr;
static constexpr size_t OTA_WEB_CHUNK_BUF_SIZE = 8192;

static void _cleanupWebOta() {
  ::gOtaInProgress = false;
  ::gOtaFinished = false;
  // Restaura I2S (microfone/alto-falante) e retoma todas as tasks
  AudioPlayer::resumeI2S();
  resumeAllTasks();
}

static bool _waitWebOtaFinished(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (::gOtaInProgress && !::gOtaFinished && !::gOtaError &&
         (millis() - start < timeoutMs)) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return ::gOtaFinished && !::gOtaError;
}

static void _scheduleRestartMs(uint32_t ms) {
  static esp_timer_handle_t restartTimer = nullptr;
  if (restartTimer) {
    esp_timer_stop(restartTimer);
    esp_timer_delete(restartTimer);
    restartTimer = nullptr;
  }

  const esp_timer_create_args_t args = {
      .callback = [](void*) { ESP.restart(); },
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "ws_restart",
      .skip_unhandled_events = true,
  };

  if (esp_timer_create(&args, &restartTimer) == ESP_OK) {
    esp_timer_start_once(restartTimer, (uint64_t)ms * 1000ULL);
  } else {
    ESP.restart();
  }
}

// ===========================  HELPERS  ===========================
static const char *labelPt(const char *label) {
  if (!label) return "desconhecido";
  if (strcmp(label, "colic")  == 0) return "Cólica";
  if (strcmp(label, "hunger") == 0) return "Fome";
  if (strcmp(label, "sleep")  == 0) return "Sono";
  if (strcmp(label, "noise")  == 0) return "Ruído";
  if (strcmp(label, "choro")  == 0) return "Analisando...";
  return label;
}

// ===================================================================
// SETUP DO SERVIDOR
// ===================================================================

void begin(WebDashboardState *state, CryConfig *cfgPtr) {
  _state = state;

  // BUGFIX: ESPAsyncWebServer não reconhece a extensão .js no ESP32 e envia
  // Content-Type: application/octet-stream. Browsers recusam executar
  // <script type="module"> sem Content-Type: text/javascript.
  // Nomes FIXOS configurados no vite.config.js (sem hash) — sobrescrevem a cada build.
  _server.on("/assets/index.js", HTTP_GET, [](AsyncWebServerRequest* req) {
      logOtaSpiffsAccess("WebServer::/assets/index.js SPIFFS send");
      req->send(SPIFFS, "/assets/index.js", "text/javascript");
  });
  _server.on("/assets/index.css", HTTP_GET, [](AsyncWebServerRequest* req) {
      logOtaSpiffsAccess("WebServer::/assets/index.css SPIFFS send");
      req->send(SPIFFS, "/assets/index.css", "text/css");
  });

  // Serve arquivos estáticos do SPIFFS sem cache para forçar atualização imediata.
  _server.serveStatic("/", SPIFFS, "/")
      .setDefaultFile("index.html")
      .setCacheControl("no-store, no-cache, must-revalidate, max-age=0");

  // ================================================================
  // API: /api/status — Status completo do sistema
  // Inclui: trigger local (cry/noise), pipeline remoto (gravação/upload/resultado),
  // sensores (temp+umid+pressão+conforto), performance, wifi e firebase
  // ================================================================
  _server.on("/api/status", HTTP_GET, [cfgPtr](AsyncWebServerRequest *req) {
    if (!_state) { req->send(500); return; }

    JsonDocument doc;

    // --- Performance ---
    doc["uptime_s"]   = _state->uptime_s;
    doc["heap"]       = _state->heap;
    doc["heap_min"]   = _state->heap_min;
    doc["psram"]      = _state->psram;
    doc["psram_tot"]  = _state->psram_tot;
    doc["flash_used"] = _state->flash_used;
    doc["flash_tot"]  = _state->flash_tot;
    doc["cpu0"]       = _state->cpu0;
    doc["cpu1"]       = _state->cpu1;
    doc["inf_ms"]     = _state->inf_ms;
    doc["log_pending"]= LogManager::getPending(); // entradas ainda em RAM

    // --- Rede ---
    doc["wifi_ok"]   = _state->wifi_ok;
    doc["ip"]        = _state->ip;
    doc["fb_envios"] = _state->fb_envios;
    doc["fb_erros"]  = _state->fb_erros;

    // --- IA + Pipeline Remoto ---
    // Etapa 1: trigger local (cry/noise)
    // Etapa 2: notebook classifica tipo e retorna label/scores
    doc["label"]         = _state->label;
    doc["label_pt"]      = labelPt(_state->label);
    doc["confianca"]     = (int)(_state->confianca * 100); // 0-100
    doc["estado"]        = _state->estado;
    doc["audio"]         = _state->audio_ativo;
    doc["ultimo_alerta"] = _state->ultimo_alerta;
    doc["pipeline"]      = _state->pipeline;
    doc["remote_busy"]   = _state->remote_busy;
    doc["result_label"]  = _state->result_label;
    doc["result_label_pt"] = labelPt(_state->result_label);
    doc["result_conf"]   = (int)(_state->result_conf * 100);
    doc["remote_latency_ms"] = _state->remote_latency_ms;
    doc["remote_ok"]     = _state->remote_ok;
    doc["remote_err"]    = _state->remote_err;

    // Compatibilidade do frontend: mantém "sleep" no payload com valor 0.0 no MVP.
    const char *labels[] = {"colic", "hunger", "noise", "sleep"};
    JsonObject sc = doc["scores"].to<JsonObject>();
    bool isCrise = (strcmp(_state->estado, "crise") == 0);
    for (int i = 0; i < 4; i++) {
      if (isCrise) {
        sc[labels[i]] = (int)(_state->scores[i] * 100) / 100.0f;
      } else {
        // Fora de crise: mostra só a barra do sinal atual (sem alarmar)
        if (strcmp(_state->label, "choro") == 0 && strcmp(labels[i], "noise") == 0) {
          sc[labels[i]] = (int)(_state->confianca * 100) / 100.0f;
        } else if (strcmp(labels[i], _state->label) == 0) {
          sc[labels[i]] = (int)(_state->confianca * 100) / 100.0f;
        } else {
          sc[labels[i]] = 0.0f;
        }
      }
    }

    // --- Sensores BME280 (completo: temp + umid + pressão + conforto) ---
    doc["temp"]        = (int)(_state->temp * 10) / 10.0f;   // 1 decimal
    doc["umid"]        = (int)(_state->umid);
    doc["pres"]        = (int)(_state->pres * 10) / 10.0f;   // 1 decimal em hPa
    doc["conforto_ok"] = _state->conforto_ok;

    // --- Áudio ---
    doc["audio_spiffs_size"] = (int)_state->audio_spiffs_size;

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // ================================================================
  // API: /api/analytics — Histórico de eventos de choro para gráficos
  // Retorna até 48 eventos em RAM: timestamp, tipo, confiança, temp, umid
  // ================================================================
  _server.on("/api/analytics", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;

    // Contadores por tipo
    int nColica = 0, nFome = 0, nSono = 0, nTotal = 0;

    // Array de eventos (ordem cronológica do mais antigo ao mais novo)
    JsonArray arr = doc["eventos"].to<JsonArray>();
    int n    = gAnalytics.count;
    int start = (gAnalytics.head - n + ANALYTICS_MAX_EVENTS) % ANALYTICS_MAX_EVENTS;

    for (int i = 0; i < n; i++) {
      int idx = (start + i) % ANALYTICS_MAX_EVENTS;
      CryEvent& e = gAnalytics.eventos[idx];
      JsonObject o = arr.add<JsonObject>();
      o["ts"]    = e.ts_s;
      o["tipo"]  = e.tipo;
      o["conf"]  = e.confianca;
      o["temp"]  = (int)(e.temp * 10) / 10.0f;
      o["umid"]  = (int)(e.umid);

      if (strcmp(e.tipo, "COLICA") == 0)      nColica++;
      else if (strcmp(e.tipo, "FOME") == 0)   nFome++;
      else if (strcmp(e.tipo, "SONO") == 0)   nSono++;
      nTotal++;
    }

    // Resumo contagem por tipo
    JsonObject resumo = doc["resumo"].to<JsonObject>();
    resumo["total"]  = nTotal;
    resumo["colica"] = nColica;
    resumo["fome"]   = nFome;
    resumo["sono"]   = nSono;

    // Uptime atual para o eixo X dos gráficos
    doc["uptime_s"] = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // ================================================================
  // API: /api/history — Série histórica de 24h (sensores + estado IA)
  // Dados em RAM/PSRAM — leitura zero-custo para a Flash
  // Query: ?limit=N (padrão = todas as amostras disponíveis)
  // ================================================================
  _server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest *req) {
    int limit = HistoryManager::count(); // Padrão: todas as amostras
    if (req->hasParam("limit")) {
      limit = req->getParam("limit")->value().toInt();
      if (limit <= 0 || limit > HISTORY_MAX_SAMPLES) limit = HISTORY_MAX_SAMPLES;
    }
    // toJson() lê apenas a PSRAM — nunca toca a Flash
    req->send(200, "application/json", HistoryManager::toJson(limit));
  });

  // ================================================================
  // API: /api/logs
  // ================================================================
  _server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", LogManager::getLogs(300));
  });
  _server.on("/api/logs/csv", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/csv", LogManager::exportCsv());
  });
  _server.on("/api/logs/clear", HTTP_POST, [](AsyncWebServerRequest *req) {
    LogManager::clear();
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // ================================================================
  // API: /api/config GET
  // ================================================================
  _server.on("/api/config", HTTP_GET, [cfgPtr](AsyncWebServerRequest *req) {
    if (!cfgPtr) { req->send(500); return; }
    JsonDocument doc;
    doc["wifi_ssid"]          = cfgPtr->wifi_ssid;
    doc["firebase_url"]       = cfgPtr->firebase_url;
    doc["firebase_auth"]      = "****"; // ocultado por segurança
    doc["audio_url"]          = cfgPtr->audio_url;
    doc["classifier_url"]     = cfgPtr->classifier_url;
    doc["classifier_timeout_ms"] = cfgPtr->classifier_timeout_ms;
    doc["volume_audio"]       = cfgPtr->volume_audio;
    doc["temp_max_conforto"]  = cfgPtr->temp_max_conforto;
    doc["temp_min_conforto"]  = cfgPtr->temp_min_conforto;
    doc["sensor_intervalo_ms"]= cfgPtr->sensor_intervalo_ms;
    doc["light_sleep_min"]    = cfgPtr->light_sleep_min;
    doc["ota_password"]       = "****"; // ocultado por segurança
    doc["audio_spiffs_size"]  = (int)(AudioPlayer::tamanhoArquivo());
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // ================================================================
  // API: /api/config POST
  // ================================================================
  AsyncCallbackJsonWebHandler *cfgHandler = new AsyncCallbackJsonWebHandler(
    "/api/config", [cfgPtr](AsyncWebServerRequest *req, JsonVariant &json) {
      JsonObject obj = json.as<JsonObject>();
      CryConfig novo = *cfgPtr;
      if (obj["wifi_ssid"])
        strlcpy(novo.wifi_ssid, obj["wifi_ssid"], sizeof(novo.wifi_ssid));
      if (obj["wifi_pass"] && strcmp(obj["wifi_pass"], "__UNCHANGED__") != 0)
        strlcpy(novo.wifi_pass, obj["wifi_pass"], sizeof(novo.wifi_pass));
      if (obj["firebase_url"])
        strlcpy(novo.firebase_url, obj["firebase_url"], sizeof(novo.firebase_url));
      if (obj["firebase_auth"] && strcmp(obj["firebase_auth"], "****") != 0)
        strlcpy(novo.firebase_auth, obj["firebase_auth"], sizeof(novo.firebase_auth));
      if (obj["audio_url"])
        strlcpy(novo.audio_url, obj["audio_url"], sizeof(novo.audio_url));
      if (obj["classifier_url"])
        strlcpy(novo.classifier_url, obj["classifier_url"], sizeof(novo.classifier_url));
      if (obj["classifier_timeout_ms"])
        novo.classifier_timeout_ms = obj["classifier_timeout_ms"];
      if (obj["confianca_minima"])
        novo.confianca_minima = obj["confianca_minima"];
      else if (obj["confianca"])
        novo.confianca_minima = ((float)obj["confianca"]) / 100.0f;
      if (obj["volume_audio"])       novo.volume_audio        = obj["volume_audio"];
      if (obj["temp_max_conforto"])  novo.temp_max_conforto   = obj["temp_max_conforto"];
      if (obj["temp_min_conforto"])  novo.temp_min_conforto   = obj["temp_min_conforto"];
      if (obj["sensor_intervalo_ms"])novo.sensor_intervalo_ms = obj["sensor_intervalo_ms"];
      if (obj["ota_password"] && strcmp(obj["ota_password"], "****") != 0)
        strlcpy(novo.ota_password, obj["ota_password"], sizeof(novo.ota_password));
      bool changed = ConfigManager::save(novo);
      if (changed) {
        req->send(200, "application/json", "{\"ok\":true,\"changed\":true}");
        LogManager::info("Config atualizada via web. Reiniciando...");
        _scheduleRestartMs(500);
      } else {
        req->send(200, "application/json", "{\"ok\":true,\"changed\":false}");
      }
    });
  _server.addHandler(cfgHandler);

  // ================================================================
  // API: /api/config/reset
  // ================================================================
  _server.on("/api/config/reset", HTTP_POST, [](AsyncWebServerRequest *req) {
    ConfigManager::reset();
    req->send(200, "application/json", "{\"ok\":true}");
    _scheduleRestartMs(300);
  });

  // ================================================================
  // API: /api/wifi/scan (Start Scan)
  // ================================================================
  _server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *req) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED || n >= 0) {
        WiFi.scanNetworks(true, true); // (async, mostra ocultas)
    }
    req->send(202, "application/json", "{\"status\":\"scanning\"}");
  });

  // ================================================================
  // API: /api/wifi/scan/result (Get Scan Results)
  // ================================================================
  _server.on("/api/wifi/scan/result", HTTP_GET, [](AsyncWebServerRequest *req) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        req->send(202, "application/json", "{\"status\":\"scanning\"}");
        return;
    }
    if (n == WIFI_SCAN_FAILED) {
        req->send(500, "application/json", "{\"error\":\"scan_failed\"}");
        return;
    }
    DynamicJsonDocument doc(1024);
    JsonArray arr = doc.to<JsonArray>();
    if (n > 0) {
      for (int i = 0; i < n; ++i) {
        JsonObject o = arr.createNestedObject();
        o["ssid"] = WiFi.SSID(i);
        o["rssi"] = WiFi.RSSI(i);
        o["enc"]  = (int)WiFi.encryptionType(i);
        o["hidden"] = false;
      }
    }
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });



  _server.on("/api/ota/upload", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasHeader("X-OTA-Password") ||
          req->header("X-OTA-Password") != ConfigManager::get().ota_password) {
          req->send(401, "text/plain", "Unauthorized");
          _cleanupWebOta();
          return;
      }

      bool ok = _waitWebOtaFinished(20000);
      if (!ok && !::gOtaError) {
          ::gOtaError = true;
          Update.abort();
      }

      if (::gOtaError) {
          req->send(500, "text/plain", "Update Failed");
      } else {
          req->send(200, "text/plain", "Update Success");
          _scheduleRestartMs(1000);
      }
      _cleanupWebOta();
  }, [](AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (!req->hasHeader("X-OTA-Password") ||
          req->header("X-OTA-Password") != ConfigManager::get().ota_password) {
          return;
      }

      if (!index) {
          Serial.printf("[OTA-WEB] Update Start: %s\n", filename.c_str());
          ::gOtaError = false;
          ::gOtaFinished = false;
          ::gOtaLastActivity = millis();

          // BUGFIX: Ordem correta de shutdown antes de sinalizar gOtaInProgress.
          // 1) Para o I2S (microfone/speaker) para liberar o i2s_read pendente.
          // 2) Sinaliza parada do TaskAudio_Spk.
          // 3) Aguarda tasks chegarem a pontos seguros antes de suspendê-las.
          // 4) Só então sinaliza gOtaInProgress=true para que nenhuma task
          //    acorde do suspend checando a flag e entrando em loop de espera.
          AudioPlayer::_parar = true;
          AudioPlayer::suspendI2S();
          vTaskDelay(pdMS_TO_TICKS(250)); // Aguarda i2s_read retornar
          suspendAllTasks();

          // Drena semáforo de áudio: evita que TaskIA acorde após ser suspensa
          // caso um Give() tenha ocorrido antes da suspensão.
          xSemaphoreTake(semAudioPronto, 0);

          // Apenas agora é seguro sinalizar o progresso do OTA.
          ::gOtaInProgress = true;

          uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
          if (!Update.begin(maxSketchSpace, U_FLASH)) {
            Update.printError(Serial);
            ::gOtaError = true;
            ::gOtaFinished = true;
            AudioPlayer::resumeI2S();
            ::gOtaInProgress = false;
            resumeAllTasks();
            return;
          }
      }

      if (::gOtaError) return;
      if (len > OTA_WEB_CHUNK_BUF_SIZE) {
          Serial.printf("[OTA-WEB] Chunk maior que o buffer: %uB\n", (unsigned)len);
          ::gOtaError = true;
          ::gOtaFinished = true;
          Update.abort();
          return;
      }

      if (Update.write(data, len) != len) {
        Update.printError(Serial);
        ::gOtaError = true;
        ::gOtaFinished = true;
        Update.abort();
        return;
      }

      ::gOtaIndex = index;
      ::gOtaLastActivity = millis();

      if (final) {
        if (Update.end(true)) {
          Serial.printf("[OTA-WEB] Update Success: %uB\n", index + len);
          ::gOtaFinished = true;
        } else {
          Update.printError(Serial);
          ::gOtaError = true;
          ::gOtaFinished = true;
        }
      }
  });

  // ================================================================
  // API: Upload WAV (áudio de cólica)
  // ================================================================
  _server.on("/api/audio/upload", HTTP_POST,
    [](AsyncWebServerRequest *req) { req->send(200, "application/json", "{\"ok\":true}"); },
    nullptr,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
      static uint8_t *wavBuf = nullptr;
      static size_t   wavPos = 0;
      if (index == 0) {
        if (wavBuf) free(wavBuf);
        wavBuf = (uint8_t *)ps_malloc(total);
        wavPos = 0;
      }
      if (wavBuf && wavPos + len <= total) {
        memcpy(wavBuf + wavPos, data, len);
        wavPos += len;
      }
      if (wavPos == total && wavBuf) {
        AudioPlayer::salvarWavSpiffs(wavBuf, total);
        free(wavBuf);
        wavBuf = nullptr;
        wavPos = 0;
      }
    });

  _server.on("/api/audio/delete", HTTP_POST, [](AsyncWebServerRequest *req) {
    SPIFFS.remove(AUDIO_SPIFFS_PATH);
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // Fallback genérico: detecta extensão para garantir MIME correto em futuros
  // rebuilds do Vite que gerem nomes de arquivo diferentes. E também assume o Captive Portal!
  _server.onNotFound([](AsyncWebServerRequest *req) {
    if (req->method() == HTTP_OPTIONS) { req->send(200); return; }

    // Interceptação de Captive Portal (OSx/Android/Windows)
    if (_state && !_state->wifi_ok) {
        String actHost = req->host();
        String myIpStr = WiFi.softAPIP().toString();
        // Se a requisição foi feita para 'detectportal.firefox.com' ou similar, manda o redirect
        if (actHost != myIpStr && actHost != "crysense-ai.local") {
            AsyncWebServerResponse *response = req->beginResponse(302, "text/plain", "Redirecting...");
            response->addHeader("Location", String("http://") + myIpStr + "/");
            req->send(response);
            return;
        }
    }

    String path = req->url();
    if (SPIFFS.exists(path)) {
      logOtaSpiffsAccess("WebServer::onNotFound SPIFFS send");
      String mime = "text/plain";
      if      (path.endsWith(".js"))   mime = "text/javascript";
      else if (path.endsWith(".css"))  mime = "text/css";
      else if (path.endsWith(".html")) mime = "text/html";
      else if (path.endsWith(".json")) mime = "application/json";
      else if (path.endsWith(".svg"))  mime = "image/svg+xml";
      else if (path.endsWith(".png"))  mime = "image/png";
      else if (path.endsWith(".ico"))  mime = "image/x-icon";
      req->send(SPIFFS, path, mime);
    } else {
      req->send(404, "text/plain", "Not found");
    }
  });

  _server.begin();
  _started = true;
  Serial.println("[WEB] Servidor iniciado na porta 80");
}

bool started() { return _started; }

} // namespace WebServer
