// =============================================================================
// CrySense AI v2.0 — web_server.h
// Interface web embarcada (AsyncWebServer, porta 80)
// Guias: Performance | Sensores | IA | Logs | Config | Sobre | Changelog
// =============================================================================
#pragma once
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>      // AsyncCallbackJsonWebHandler
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include "config_manager.h"

namespace WebServer {

static AsyncWebServer _server(80);
static bool           _started = false;

// ===========================  HELPERS  ===========================

// Tipos de choro para tradução PT
static const char* labelPt(const char* label) {
    if (!label) return "desconhecido";
    if (strcmp(label,"colic")==0)      return "Cólica";
    if (strcmp(label,"hunger")==0)     return "Fome";
    if (strcmp(label,"sleep")==0)      return "Sono";
    if (strcmp(label,"noise")==0)      return "Ruído";
    return label;
}

static const char* badgeClass(const char* label) {
    if (!label) return "calmo";
    if (strcmp(label,"colic")==0)      return "colica";
    if (strcmp(label,"hunger")==0)     return "fome";
    if (strcmp(label,"sleep")==0)      return "sono";
    return "ruido";
}

// ===================================================================
// Ponteiros para o estado global — preenchidos em begin()
// ===================================================================
static WebDashboardState* _state = nullptr;

// ===================================================================
// SETUP DO SERVIDOR
// ===================================================================
void begin(WebDashboardState* state, CryConfig* cfgPtr) {
    _state = state;

    // SPIFFS já foi iniciado pelo LogManager::begin() — apenas serve os arquivos
    _server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

    // ---- API: status + performance (tudo junto para reduzir latência) ----
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
        doc["wifi_ok"]    = _state->wifi_ok;
        doc["ip"]         = _state->ip;
        doc["fb_envios"]  = _state->fb_envios;
        doc["fb_erros"]   = _state->fb_erros;
        doc["label"]      = _state->label;
        doc["label_pt"]   = labelPt(_state->label);
        doc["confianca"]  = (int)(_state->confianca * 100);
        doc["estado"]     = _state->estado;
        doc["audio"]      = _state->audio_ativo;
        doc["ultimo_alerta"] = _state->ultimo_alerta;
        doc["temp"]       = (int)(_state->temp * 10) / 10.0f;
        doc["umid"]       = (int)(_state->umid);
        doc["pres"]       = (int)(_state->pres);
        doc["conforto_ok"]= _state->conforto_ok;
        doc["audio_spiffs_size"] = (int)_state->audio_spiffs_size;
        // Scores das 4 classes
        const char* labels[] = {"colic","hunger","noise","sleep"};
        JsonObject sc = doc["scores"].to<JsonObject>();
        for (int i=0; i<4; i++) sc[labels[i]] = (int)(_state->scores[i]*100)/100.0f;
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // ---- API: Logs ----
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

    // ---- API: Config GET ----
    _server.on("/api/config", HTTP_GET, [cfgPtr](AsyncWebServerRequest *req) {
        if (!cfgPtr) { req->send(500); return; }
        JsonDocument doc;
        doc["wifi_ssid"]          = cfgPtr->wifi_ssid;
        doc["firebase_url"]       = cfgPtr->firebase_url;
        doc["firebase_auth"]      = "****"; // oculta por segurança
        doc["audio_url"]          = cfgPtr->audio_url;
        doc["confianca_minima"]   = cfgPtr->confianca_minima;
        doc["volume_audio"]       = cfgPtr->volume_audio;
        doc["temp_max_conforto"]  = cfgPtr->temp_max_conforto;
        doc["temp_min_conforto"]  = cfgPtr->temp_min_conforto;
        doc["sensor_intervalo_ms"]= cfgPtr->sensor_intervalo_ms;
        doc["light_sleep_min"]    = cfgPtr->light_sleep_min;
        doc["audio_spiffs_size"]  = (int)(AudioPlayer::tamanhoArquivo());
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // ---- API: Config POST ----
    AsyncCallbackJsonWebHandler* cfgHandler = new AsyncCallbackJsonWebHandler(
        "/api/config", [cfgPtr](AsyncWebServerRequest* req, JsonVariant& json) {
        JsonObject obj = json.as<JsonObject>();
        CryConfig novo = *cfgPtr;
        if (obj["wifi_ssid"])           strlcpy(novo.wifi_ssid,    obj["wifi_ssid"],    sizeof(novo.wifi_ssid));
        if (obj["wifi_pass"] && strcmp(obj["wifi_pass"],"__UNCHANGED__")!=0)
                                         strlcpy(novo.wifi_pass,    obj["wifi_pass"],    sizeof(novo.wifi_pass));
        if (obj["firebase_url"])         strlcpy(novo.firebase_url, obj["firebase_url"], sizeof(novo.firebase_url));
        if (obj["firebase_auth"] && strcmp(obj["firebase_auth"],"****")!=0)
                                         strlcpy(novo.firebase_auth,obj["firebase_auth"],sizeof(novo.firebase_auth));
        if (obj["audio_url"])            strlcpy(novo.audio_url,   obj["audio_url"],    sizeof(novo.audio_url));
        if (obj["confianca_minima"])     novo.confianca_minima    = obj["confianca_minima"];
        if (obj["volume_audio"])         novo.volume_audio        = obj["volume_audio"];
        if (obj["temp_max_conforto"])    novo.temp_max_conforto   = obj["temp_max_conforto"];
        if (obj["temp_min_conforto"])    novo.temp_min_conforto   = obj["temp_min_conforto"];
        if (obj["sensor_intervalo_ms"])  novo.sensor_intervalo_ms = obj["sensor_intervalo_ms"];
        if (obj["light_sleep_min"])      novo.light_sleep_min     = obj["light_sleep_min"];
        ConfigManager::save(novo);
        req->send(200, "application/json", "{\"ok\":true}");
        LogManager::info("Config atualizada via web. Reiniciando...");
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP.restart();
    });
    _server.addHandler(cfgHandler);

    // ---- API: Config Reset ----
    _server.on("/api/config/reset", HTTP_POST, [](AsyncWebServerRequest *req) {
        ConfigManager::reset();
        req->send(200, "application/json", "{\"ok\":true}");
        vTaskDelay(pdMS_TO_TICKS(300));
        ESP.restart();
    });

    // ---- API: Upload WAV ----
    _server.on("/api/audio/upload", HTTP_POST,
        [](AsyncWebServerRequest* req){ req->send(200,"application/json","{\"ok\":true}"); },
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            static uint8_t* wavBuf = nullptr;
            static size_t   wavPos = 0;
            if (index == 0) {
                if (wavBuf) free(wavBuf);
                wavBuf = (uint8_t*)ps_malloc(total);
                wavPos = 0;
            }
            if (wavBuf && wavPos+len <= total) {
                memcpy(wavBuf+wavPos, data, len);
                wavPos += len;
            }
            if (wavPos == total && wavBuf) {
                AudioPlayer::salvarWavSpiffs(wavBuf, total);
                free(wavBuf); wavBuf = nullptr; wavPos = 0;
            }
        }
    );

    // ---- API: Deletar áudio local ----
    _server.on("/api/audio/delete", HTTP_POST, [](AsyncWebServerRequest *req) {
        SPIFFS.remove(AUDIO_SPIFFS_PATH);
        req->send(200, "application/json", "{\"ok\":true}");
    });

    _server.onNotFound([](AsyncWebServerRequest *req){req->send(404,"text/plain","Not found");});
    _server.begin();
    _started = true;
    Serial.println("[WEB] Servidor iniciado na porta 80");
}

bool started() { return _started; }

} // namespace WebServer
