// =============================================================================
// CrySense AI v2.0 — config_manager.h
// Gerenciamento de configurações persistentes via NVS (Preferences)
// Permite troca de WiFi/parâmetros sem reflash via interface web
// =============================================================================
#pragma once
#include <Preferences.h>
#include "secrets.h"

struct CryConfig {
    char  wifi_ssid[33];
    char  wifi_pass[65];
    char  firebase_url[128];
    char  firebase_auth[128];
    char  audio_url[200];      // URL de áudio na nuvem (opcional)
    float confianca_minima;    // Threshold de confiança da IA (0.0-1.0)
    float temp_max_conforto;   // Temp. máx. confortável (°C) — padrão 28
    float temp_min_conforto;   // Temp. mín. confortável (°C) — padrão 18
    uint32_t sensor_intervalo_ms; // Intervalo leitura BME280 (ms)
    uint8_t  volume_audio;     // Volume do áudio (0-100)
    uint32_t light_sleep_min;  // Minutos inativo para light sleep (0=desativado)
    uint8_t  gatilho_decisao;  // Acertos mínimos no buffer para alerta
    uint8_t  gatilho_silencio; // Acertos silêncio para resetar crise
};
// =============================================================================
// ESTRUTURAS COMPARTILHADAS (Tipos e Filas Globais)
// =============================================================================
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

#define TAMANHO_BUFFER      10
#define LABEL_HUNGER     "hunger"
#define LABEL_COLIC      "colic"
#define LABEL_SLEEP      "sleep"
#define LABEL_NOISE      "noise"

enum TipoChoro { SILENCIO_RUIDO=0, FOME=1, COLICA_DOR=2, SONO=3 };

struct SistemaState {
    TipoChoro historico[TAMANHO_BUFFER];
    int  indiceAtual   = 0;
    int  ciclosTotais  = 0;
    bool choroCritico  = false;
    int  ciclosSilencio = 0;

    char  labelAtual[16]  = "noise";
    float confianca       = 0.0f;
    float scores[4]       = {0};
    float infMs           = 0.0f;
    uint32_t totalInfs    = 0;

    char ultimoAlerta[16] = "";
    uint32_t tsAlerta     = 0;
    uint32_t heapMin      = UINT32_MAX;
};

struct InferenceResult {
    char  label[16];
    float confianca;
    float scores[4];
    float infMs;
    bool  isSilencioReal;
};

struct FirebaseMsg {
    char tipo[16];
    float confianca, temp, umid;
    bool acalmado;
};

struct WebDashboardState {
    // Performance
    uint32_t uptime_s, heap, heap_min, psram, psram_tot, flash_used, flash_tot;
    uint8_t  cpu0, cpu1;
    float    inf_ms;
    bool     wifi_ok;
    char     ip[20];
    uint32_t fb_envios, fb_erros;
    // IA
    char     label[16];
    float    confianca;
    float    scores[4];           // colic, hunger, noise, sleep
    String   estado;              // "calmo" | "crise" | "monitorando"
    bool     audio_ativo;
    char     ultimo_alerta[16];
    // Sensores
    float    temp, umid, pres;
    bool     conforto_ok;
    // Config
    float    audio_spiffs_size;
};

// Declarações externas das Globais do Sistema
extern SistemaState gState;
extern WebDashboardState gWebState;
extern SemaphoreHandle_t semAudioPronto;
extern QueueHandle_t     qResultadoIA;
extern QueueHandle_t     qFirebase;

// =============================================================================

namespace ConfigManager {

static Preferences _prefs;
static CryConfig   _cfg;
static bool        _loaded = false;

// --- Defaults ---
static void _setDefaults() {
    strlcpy(_cfg.wifi_ssid,   WIFI_SSID,   sizeof(_cfg.wifi_ssid));
    strlcpy(_cfg.wifi_pass,   WIFI_PASS,   sizeof(_cfg.wifi_pass));
    strlcpy(_cfg.firebase_url,  FIREBASE_URL,  sizeof(_cfg.firebase_url));
    strlcpy(_cfg.firebase_auth, FIREBASE_AUTH, sizeof(_cfg.firebase_auth));
    strlcpy(_cfg.audio_url,   AUDIO_CLOUD_URL, sizeof(_cfg.audio_url));
    _cfg.confianca_minima    = 0.70f;  // 70% — limiar ajustado
    _cfg.temp_max_conforto   = 28.0f;
    _cfg.temp_min_conforto   = 18.0f;
    _cfg.sensor_intervalo_ms = 5000;
    _cfg.volume_audio        = 100;
    _cfg.light_sleep_min     = 10;
    _cfg.gatilho_decisao     = 7;   // 7/10 ciclos para validar tipo de choro
    _cfg.gatilho_silencio    = 6;
}

// --- Carrega configurações da NVS ---
void load() {
    _setDefaults();
    _prefs.begin("crysense", true); // read-only
    _prefs.getString("wifi_ssid",   _cfg.wifi_ssid,    sizeof(_cfg.wifi_ssid));
    _prefs.getString("wifi_pass",   _cfg.wifi_pass,    sizeof(_cfg.wifi_pass));
    _prefs.getString("fb_url",      _cfg.firebase_url, sizeof(_cfg.firebase_url));
    _prefs.getString("fb_auth",     _cfg.firebase_auth,sizeof(_cfg.firebase_auth));
    _prefs.getString("audio_url",   _cfg.audio_url,    sizeof(_cfg.audio_url));
    _cfg.confianca_minima    = _prefs.getFloat("conf_min",     _cfg.confianca_minima);
    _cfg.temp_max_conforto   = _prefs.getFloat("temp_max",     _cfg.temp_max_conforto);
    _cfg.temp_min_conforto   = _prefs.getFloat("temp_min",     _cfg.temp_min_conforto);
    _cfg.sensor_intervalo_ms = _prefs.getUInt( "sensor_ms",    _cfg.sensor_intervalo_ms);
    _cfg.volume_audio        = _prefs.getUChar("volume",       _cfg.volume_audio);
    _cfg.light_sleep_min     = _prefs.getUInt( "sleep_min",    _cfg.light_sleep_min);
    _cfg.gatilho_decisao     = _prefs.getUChar("gatilho_d",    _cfg.gatilho_decisao);
    _cfg.gatilho_silencio    = _prefs.getUChar("gatilho_s",    _cfg.gatilho_silencio);
    _prefs.end();
    _loaded = true;
}

// --- Salva configurações na NVS ---
void save(const CryConfig& novo) {
    _cfg = novo;
    _prefs.begin("crysense", false); // read-write
    _prefs.putString("wifi_ssid",  _cfg.wifi_ssid);
    _prefs.putString("wifi_pass",  _cfg.wifi_pass);
    _prefs.putString("fb_url",     _cfg.firebase_url);
    _prefs.putString("fb_auth",    _cfg.firebase_auth);
    _prefs.putString("audio_url",  _cfg.audio_url);
    _prefs.putFloat( "conf_min",   _cfg.confianca_minima);
    _prefs.putFloat( "temp_max",   _cfg.temp_max_conforto);
    _prefs.putFloat( "temp_min",   _cfg.temp_min_conforto);
    _prefs.putUInt(  "sensor_ms",  _cfg.sensor_intervalo_ms);
    _prefs.putUChar( "volume",     _cfg.volume_audio);
    _prefs.putUInt(  "sleep_min",  _cfg.light_sleep_min);
    _prefs.putUChar( "gatilho_d",  _cfg.gatilho_decisao);
    _prefs.putUChar( "gatilho_s",  _cfg.gatilho_silencio);
    _prefs.end();
}

// --- Reseta para defaults e apaga NVS ---
void reset() {
    _prefs.begin("crysense", false);
    _prefs.clear();
    _prefs.end();
    _setDefaults();
}

// --- Acessor ---
CryConfig& get() {
    if (!_loaded) load();
    return _cfg;
}

} // namespace ConfigManager
