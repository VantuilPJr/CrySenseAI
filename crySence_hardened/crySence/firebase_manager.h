// =============================================================================
// =============================================================================
#pragma once
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "log_manager.h"
#include "config_manager.h"
#include "heap_debug.h"

#define VERBOSE_IOT_TIMING 0

namespace Firebase {

static bool    _iniciado  = false;
static uint32_t _erros    = 0;
static uint32_t _envios   = 0;
static uint32_t _ultimoOk = 0;

static bool _patch(const char* fbUrl, const char* fbAuth, const char* path, const String& body) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(5);

    HTTPClient http;
    String url = String(fbUrl) + path + ".json?auth=" + String(fbAuth);
    if (!http.begin(client, url)) return false;
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);

    int code = http.PATCH(body);
    bool ok = (code == 200 || code == 204);
    if (ok) {
        _envios++;
        _ultimoOk = millis();
    } else {
        _erros++;
        Serial.printf("[Firebase] Erro HTTP %d\n", code);
    }
    http.end();
    return ok;
}

void begin(const char* fbUrl, const char* fbAuth) {
    _iniciado = (strlen(fbUrl) > 10 && strlen(fbAuth) > 5);
    if (_iniciado) Serial.printf("[Firebase] Configurado: %s\n", fbUrl);
    else           Serial.println("[Firebase] Desabilitado — credenciais ausentes.");
}

bool enviarAlerta(const char* fbUrl, const char* fbAuth,
                  const char* tipo, float confianca,
                  float temp, float umid) {
    if (!_iniciado) return false;
    JsonDocument doc;
    doc["tipo"]        = tipo;
    doc["confianca"]   = (int)(confianca * 100);
    doc["temperatura"] = (int)(temp * 10) / 10.0;
    doc["umidade"]     = (int)(umid);
    doc["timestamp"]   = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    doc["ativo"]       = true;
    String body;
    serializeJson(doc, body);
    bool ok = _patch(fbUrl, fbAuth, "/crySense/status", body);
    if (ok) Serial.printf("[Firebase] Alerta enviado: %s (%.0f%%)\n", tipo, confianca*100);
    return ok;
}

bool enviarSensores(const char* fbUrl, const char* fbAuth, float temp, float umid, float pressao) {
    if (!_iniciado) return false;
    JsonDocument doc;
    doc["temperatura"] = (int)(temp * 10) / 10.0;
    doc["umidade"]     = (int)(umid);
    doc["pressao"]     = (int)(pressao * 10) / 10.0;
    doc["ts"]          = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    String body;
    serializeJson(doc, body);
    return _patch(fbUrl, fbAuth, "/crySense/sensores", body);
}

bool enviarHeartbeat(const char* fbUrl, const char* fbAuth,
                     uint32_t uptime_s, uint32_t freeHeap, float inferencia_ms) {
    if (!_iniciado) return false;
    JsonDocument doc;
    doc["uptime"]       = uptime_s;
    doc["heap_livre"]   = freeHeap;
    doc["inferencia_ms"]= (int)(inferencia_ms);
    doc["ts"]           = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    doc["ativo"]        = true;
    String body;
    serializeJson(doc, body);
    return _patch(fbUrl, fbAuth, "/crySense/heartbeat", body);
}

bool enviarAccalmado(const char* fbUrl, const char* fbAuth) {
    if (!_iniciado) return false;
    JsonDocument doc;
    doc["ativo"]  = false;
    doc["tipo"]   = "calmo";
    doc["ts"]     = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    String body;
    serializeJson(doc, body);
    return _patch(fbUrl, fbAuth, "/crySense/status", body);
}

uint32_t totalEnvios() { return _envios; }
uint32_t totalErros()  { return _erros;  }
uint32_t msDesdeUltimoOk() { return millis() - _ultimoOk; }
bool     conectado() { return _iniciado && (millis() - _ultimoOk < 120000); }

static void TaskIOT(void* pv) {
    LogManager::info("[TaskIOT] Iniciada no Core 0");
    FirebaseMsg fm;
    uint32_t tUltimoHB       = 0;
    uint32_t tUltimoCpu      = 0;
    uint32_t tUltimoWifiCheck = 0;
    uint32_t tUltimoReconnect = 0;

    static uint32_t lastIdle0 = 0, lastIdle1 = 0;

    while (true) {
        uint64_t t0 = esp_timer_get_time();

        // =================================================================
        // configurados no setup(), isso serve como segurança extra.
        // =================================================================
        uint32_t agoraW = millis();
        if (agoraW - tUltimoWifiCheck >= 5000UL) {
            tUltimoWifiCheck = agoraW;
            if (WiFi.status() != WL_CONNECTED && WiFi.getMode() == WIFI_STA) {
                if (agoraW - tUltimoReconnect >= 30000UL) {
                    tUltimoReconnect = agoraW;
                    Serial.println("[WiFi] Conexão perdida — tentando reconectar...");
                    WiFi.reconnect();
                }
            } else if (WiFi.status() == WL_CONNECTED) {
                strlcpy(gWebState.ip, WiFi.localIP().toString().c_str(), sizeof(gWebState.ip));
                gWebState.wifi_ok = true;
            }
        }

        if (!_iniciado) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        while (xQueueReceive(qFirebase, &fm, pdMS_TO_TICKS(1000)) == pdTRUE) {
            CryConfig& cfg = ConfigManager::get();
            if (fm.acalmado) {
                enviarAccalmado(cfg.firebase_url, cfg.firebase_auth);
            } else {
                enviarAlerta(cfg.firebase_url, cfg.firebase_auth,
                             fm.tipo, fm.confianca, fm.temp, fm.umid);
            }
            gWebState.fb_envios = totalEnvios();
            gWebState.fb_erros  = totalErros();
        }

        uint32_t agora = millis();
        if (agora - tUltimoCpu >= 5000) {
            uint32_t dtMs = agora - tUltimoCpu;
            tUltimoCpu = agora;

            uint32_t idle0Now = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(0);
            uint32_t idle1Now = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(1);

            uint32_t totalUs = dtMs * 1000UL;
            uint32_t d0 = idle0Now - lastIdle0;
            uint32_t d1 = idle1Now - lastIdle1;
            lastIdle0 = idle0Now;
            lastIdle1 = idle1Now;

            gWebState.cpu0 = (d0 >= totalUs) ? 0 : (uint8_t)(100 - (d0 * 100UL / totalUs));
            gWebState.cpu1 = (d1 >= totalUs) ? 0 : (uint8_t)(100 - (d1 * 100UL / totalUs));
        }

        if (millis() - tUltimoHB > 60000) {
            tUltimoHB = millis();
            CryConfig& cfg = ConfigManager::get();
            enviarHeartbeat(
                cfg.firebase_url, cfg.firebase_auth,
                esp_timer_get_time() / 1000000ULL,
                ESP.getFreeHeap(),
                gWebState.inf_ms);

            gWebState.uptime_s   = esp_timer_get_time() / 1000000ULL;
            gWebState.wifi_ok    = (WiFi.status() == WL_CONNECTED);
            gWebState.flash_used = ESP.getSketchSize();
            gWebState.flash_tot  = ESP.getFlashChipSize();
            strlcpy(gWebState.ip, WiFi.localIP().toString().c_str(), sizeof(gWebState.ip));
            gWebState.audio_spiffs_size = (float)AudioPlayer::tamanhoArquivo();
        }

#if VERBOSE_IOT_TIMING
        float execMs = (float)(esp_timer_get_time() - t0) / 1000.0f;
        Serial.printf("[TaskIOT] Tempo de execucao: %.1fms\n", execMs);
#endif

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void beginTask(TaskHandle_t* pTaskIOT = nullptr) {
    TaskHandle_t hTask = nullptr;
    xTaskCreatePinnedToCore(TaskIOT, "TaskIOT", 16384, nullptr, 1, &hTask, 0);
    if (pTaskIOT) {
        *pTaskIOT = hTask;
    }
}

}
