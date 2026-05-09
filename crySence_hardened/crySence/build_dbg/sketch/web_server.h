#line 1 "C:\\Arduino\\crySense_ai\\crySence_hardened\\crySence\\web_server.h"
// =============================================================================
// =============================================================================
#pragma once
#include "config_manager.h"
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <esp_timer.h>

namespace WebServer {

static AsyncWebServer _server(80);
static bool _started = false;
static const size_t MAX_WAV_UPLOAD_BYTES = 2 * 1024 * 1024;
static volatile bool _uploadBusy = false;
static bool _lastUploadOk = true;
static char _lastUploadErr[80] = "";
static WebDashboardState *_state = nullptr;

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

  _server.on("/assets/index.js", HTTP_GET, [](AsyncWebServerRequest* req) {
      req->send(SPIFFS, "/assets/index.js", "text/javascript");
  });
  _server.on("/assets/index.css", HTTP_GET, [](AsyncWebServerRequest* req) {
      req->send(SPIFFS, "/assets/index.css", "text/css");
  });

  _server.serveStatic("/", SPIFFS, "/")
      .setDefaultFile("index.html")
      .setCacheControl("no-store, no-cache, must-revalidate, max-age=0");

  // ================================================================
  // API: /api/status — Status completo do sistema
  // ================================================================
  _server.on("/api/status", HTTP_GET, [cfgPtr](AsyncWebServerRequest *req) {
    if (!_state) { req->send(500); return; }

    JsonDocument doc;

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
    doc["log_pending"]= LogManager::getPending();

    doc["wifi_ok"]   = _state->wifi_ok;
    doc["ip"]        = _state->ip;
    doc["fb_envios"] = _state->fb_envios;
    doc["fb_erros"]  = _state->fb_erros;

    doc["label"]         = _state->label;
    doc["label_pt"]      = labelPt(_state->label);
    doc["confianca"]     = (int)(_state->confianca * 100);
    doc["estado"]        = _state->estado;
    doc["audio"]         = _state->audio_ativo;
    doc["ultimo_alerta"] = _state->ultimo_alerta;

    const char *labels[] = {"colic", "hunger", "noise", "sleep"};
    JsonObject sc = doc["scores"].to<JsonObject>();
    bool isCrise = (strcmp(_state->estado, "crise") == 0);
    for (int i = 0; i < 4; i++) {
      if (isCrise) {
        sc[labels[i]] = (int)(_state->scores[i] * 100) / 100.0f;
      } else {
        if (strcmp(_state->label, "choro") == 0 && strcmp(labels[i], "noise") == 0) {
          sc[labels[i]] = (int)(_state->confianca * 100) / 100.0f;
        } else if (strcmp(labels[i], _state->label) == 0) {
          sc[labels[i]] = (int)(_state->confianca * 100) / 100.0f;
        } else {
          sc[labels[i]] = 0.0f;
        }
      }
    }

    doc["temp"]        = (int)(_state->temp * 10) / 10.0f;
    doc["umid"]        = (int)(_state->umid);
    doc["pres"]        = (int)(_state->pres * 10) / 10.0f;
    doc["conforto_ok"] = _state->conforto_ok;

    doc["audio_spiffs_size"] = (int)_state->audio_spiffs_size;

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // ================================================================
  // API: /api/analytics — Histórico de eventos de choro para gráficos
  // ================================================================
  _server.on("/api/analytics", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;

    int nColica = 0, nFome = 0, nSono = 0, nTotal = 0;

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

    JsonObject resumo = doc["resumo"].to<JsonObject>();
    resumo["total"]  = nTotal;
    resumo["colica"] = nColica;
    resumo["fome"]   = nFome;
    resumo["sono"]   = nSono;

    doc["uptime_s"] = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
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
    doc["firebase_auth"]      = "****";
    doc["audio_url"]          = cfgPtr->audio_url;
    doc["confianca_minima"]   = cfgPtr->confianca_minima;
    doc["volume_audio"]       = cfgPtr->volume_audio;
    doc["temp_max_conforto"]  = cfgPtr->temp_max_conforto;
    doc["temp_min_conforto"]  = cfgPtr->temp_min_conforto;
    doc["sensor_intervalo_ms"]= cfgPtr->sensor_intervalo_ms;
    doc["light_sleep_min"]    = cfgPtr->light_sleep_min;
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
      if (obj["confianca_minima"])
        novo.confianca_minima = obj["confianca_minima"];
      else if (obj["confianca"])
        novo.confianca_minima = ((float)obj["confianca"]) / 100.0f;
      if (obj["volume_audio"])       novo.volume_audio        = obj["volume_audio"];
      if (obj["temp_max_conforto"])  novo.temp_max_conforto   = obj["temp_max_conforto"];
      if (obj["temp_min_conforto"])  novo.temp_min_conforto   = obj["temp_min_conforto"];
      if (obj["sensor_intervalo_ms"])novo.sensor_intervalo_ms = obj["sensor_intervalo_ms"];
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
  // API: Upload WAV (áudio de cólica)
  // ================================================================
  _server.on("/api/audio/upload", HTTP_POST,
    [](AsyncWebServerRequest *req) {
      if (_lastUploadOk) {
        req->send(200, "application/json", "{\"ok\":true}");
      } else {
        String out = String("{\"ok\":false,\"error\":\"") + _lastUploadErr + "\"}";
        req->send(400, "application/json", out);
      }
    },
    nullptr,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
      static uint8_t *wavBuf = nullptr;
      static size_t   wavPos = 0;

      if (index == 0) {
        if (_uploadBusy) {
          _lastUploadOk = false;
          strlcpy(_lastUploadErr, "upload_em_andamento", sizeof(_lastUploadErr));
          return;
        }

        _uploadBusy = true;
        _lastUploadOk = true;
        _lastUploadErr[0] = '\0';

        if (total == 0 || total > MAX_WAV_UPLOAD_BYTES) {
          _lastUploadOk = false;
          strlcpy(_lastUploadErr, "tamanho_invalido", sizeof(_lastUploadErr));
          _uploadBusy = false;
          return;
        }

        if (wavBuf) free(wavBuf);
        wavBuf = (uint8_t *)ps_malloc(total);
        wavPos = 0;

        if (!wavBuf) {
          _lastUploadOk = false;
          strlcpy(_lastUploadErr, "memoria_insuficiente", sizeof(_lastUploadErr));
          _uploadBusy = false;
          return;
        }
      }

      if (!_uploadBusy || !_lastUploadOk) {
        return;
      }

      if (!wavBuf || wavPos + len > total) {
        _lastUploadOk = false;
        strlcpy(_lastUploadErr, "payload_invalido", sizeof(_lastUploadErr));
      } else {
        memcpy(wavBuf + wavPos, data, len);
        wavPos += len;
      }

      if (wavPos == total && wavBuf) {
        if (!AudioPlayer::salvarWavSpiffs(wavBuf, total)) {
          _lastUploadOk = false;
          strlcpy(_lastUploadErr, "falha_salvar_spiffs", sizeof(_lastUploadErr));
        }
        free(wavBuf);
        wavBuf = nullptr;
        wavPos = 0;
        _uploadBusy = false;
      } else if (index + len >= total) {
        if (wavBuf) {
          free(wavBuf);
          wavBuf = nullptr;
        }
        wavPos = 0;
        _uploadBusy = false;
      }
    });

  _server.on("/api/audio/delete", HTTP_POST, [](AsyncWebServerRequest *req) {
    SPIFFS.remove(AUDIO_SPIFFS_PATH);
    req->send(200, "application/json", "{\"ok\":true}");
  });

  _server.onNotFound([](AsyncWebServerRequest *req) {
    String path = req->url();
    if (SPIFFS.exists(path)) {
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

}
