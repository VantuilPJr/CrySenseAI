// =============================================================================
// CrySense AI v2.0 — firebase_manager.h
// Envio de dados para Firebase RTDB via HTTP REST (sem biblioteca externa)
// Usa WiFiClientSecure para HTTPS
// =============================================================================
#pragma once
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

namespace Firebase {

static bool    _iniciado  = false;
static uint32_t _erros    = 0;
static uint32_t _envios   = 0;
static uint32_t _ultimoOk = 0;

// --- Envia dado para RTDB via HTTP PATCH (merge) ---
static bool _patch(const char* fbUrl, const char* fbAuth, const char* path, const String& body) {
    WiFiClientSecure client;
    client.setInsecure(); // aceita qualquer certificado (compatível com todos os projetos Firebase)
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

// --- Inicializa (apenas verifica configuração) ---
void begin(const char* fbUrl, const char* fbAuth) {
    _iniciado = (strlen(fbUrl) > 10 && strlen(fbAuth) > 5);
    if (_iniciado) Serial.printf("[Firebase] Configurado: %s\n", fbUrl);
    else           Serial.println("[Firebase] Desabilitado — credenciais ausentes.");
}

// --- Envia alerta de choro ---
// Path: /crySense/status
bool enviarAlerta(const char* fbUrl, const char* fbAuth,
                  const char* tipo, float confianca,
                  float temp, float umid) {
    if (!_iniciado) return false;
    JsonDocument doc;
    doc["tipo"]        = tipo;
    doc["confianca"]   = (int)(confianca * 100); // 0-100
    doc["temperatura"] = (int)(temp * 10) / 10.0; // 1 decimal
    doc["umidade"]     = (int)(umid);
    doc["timestamp"]   = (uint32_t)(esp_timer_get_time() / 1000000ULL); // unix approx (boot-relative)
    doc["ativo"]       = true;
    String body;
    serializeJson(doc, body);
    bool ok = _patch(fbUrl, fbAuth, "/crySense/status", body);
    if (ok) Serial.printf("[Firebase] Alerta enviado: %s (%.0f%%)\n", tipo, confianca*100);
    return ok;
}

// --- Envia dados dos sensores ---
// Path: /crySense/sensores
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

// --- Heartbeat: prova que o sistema está ativo ---
// Path: /crySense/status
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

// --- Envia acalmado (critério de silêncio atingido) ---
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

// --- Status do módulo ---
uint32_t totalEnvios() { return _envios; }
uint32_t totalErros()  { return _erros;  }
uint32_t msDesdeUltimoOk() { return millis() - _ultimoOk; }
bool     conectado() { return _iniciado && (millis() - _ultimoOk < 120000); }

// --- Task IOT: Drena fila RTDB e gera Heartbeats de CPU/Heap ---
static void TaskIOT(void* pv) {
    LogManager::info("[TaskIOT] Iniciada no Core 0");
    FirebaseMsg fm;
    uint32_t tUltimoHB   = 0;
    uint32_t tUltimoCpu  = 0;

    static uint32_t lastIdle0 = 0, lastIdle1 = 0;

    while (true) {
        uint64_t t0 = esp_timer_get_time();

        if (!_iniciado) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        // Drena fila de Firebase (não bloqueante além de 1s)
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

        // --- Medição de CPU a cada 5s ---
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

        // Heartbeat a cada 60s
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
            strncpy(gWebState.ip, WiFi.localIP().toString().c_str(), sizeof(gWebState.ip));
            gWebState.audio_spiffs_size = (float)AudioPlayer::tamanhoArquivo();
        }

        float execMs = (float)(esp_timer_get_time() - t0) / 1000.0f;
        Serial.printf("[TaskIOT] Tempo de execucao: %.1fms\n", execMs);

        vTaskDelay(pdMS_TO_TICKS(100)); // cede CPU
    }
}

static void beginTask() {
    xTaskCreatePinnedToCore(TaskIOT, "TaskIOT", 8192, nullptr, 1, nullptr, 0);
}

} // namespace Firebase
