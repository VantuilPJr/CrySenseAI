#line 1 "C:\\Arduino\\crySense_ai\\crySence_hardened\\crySence\\config_manager.h"
// =============================================================================
// =============================================================================
#pragma once
#include <math.h>
#include <string.h>
#include <Preferences.h>
#include "secrets.h"

struct CryConfig {
    char  wifi_ssid[33];
    char  wifi_pass[65];
    char  firebase_url[128];
    char  firebase_auth[128];
    char  audio_url[200];
    float confianca_minima;
    float temp_max_conforto;
    float temp_min_conforto;
    float rms_threshold;
    uint32_t sensor_intervalo_ms;
    uint8_t  volume_audio;
    uint32_t light_sleep_min;
    uint8_t  gatilho_decisao;
    uint8_t  gatilho_silencio;
};
// =============================================================================
// =============================================================================
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

#define TAMANHO_BUFFER      10

// =============================================================================
// LABELS DOS MODELOS EDGE IMPULSE
// =============================================================================
#define LABEL_CRY        "cry"
#define LABEL_NOISE      "noise"

#define LABEL_HUNGER     "hunger"
#define LABEL_COLIC      "colic"
#define LABEL_SLEEP      "sleep"

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

// =============================================================================
// ANALYTICS — Histórico de episódios de choro (em RAM, sem Flash)
// =============================================================================
#define ANALYTICS_MAX_EVENTS 48

struct CryEvent {
    uint32_t ts_s;
    char     tipo[16];
    uint8_t  confianca;
    float    temp;
    float    umid;
};

struct AnalyticsState {
    CryEvent eventos[ANALYTICS_MAX_EVENTS];
    int      head   = 0;
    int      count  = 0;

    void registrar(const char* tipo, uint8_t conf, float t, float u, uint32_t uptime_s) {
        eventos[head].ts_s      = uptime_s;
        strlcpy(eventos[head].tipo, tipo, sizeof(eventos[head].tipo));
        eventos[head].confianca = conf;
        eventos[head].temp      = t;
        eventos[head].umid      = u;
        head = (head + 1) % ANALYTICS_MAX_EVENTS;
        if (count < ANALYTICS_MAX_EVENTS) count++;
    }
};

struct WebDashboardState {
    uint32_t uptime_s, heap, heap_min, psram, psram_tot, flash_used, flash_tot;
    uint8_t  cpu0, cpu1;
    float    inf_ms;
    bool     wifi_ok;
    char     ip[20];
    uint32_t fb_envios, fb_erros;
    char     label[16];
    float    confianca;
    float    scores[4];
    char     estado[16] = "calmo";
    bool     audio_ativo;
    char     ultimo_alerta[16];
    float    temp, umid, pres;
    bool     conforto_ok;
    // Config
    float    audio_spiffs_size;
};

// Declarações externas das Globais do Sistema
extern SistemaState gState;
extern WebDashboardState gWebState;
extern AnalyticsState    gAnalytics;
extern SemaphoreHandle_t semAudioPronto;
extern QueueHandle_t     qResultadoIA;
extern QueueHandle_t     qFirebase;

// =============================================================================

namespace ConfigManager {

static Preferences _prefs;
static CryConfig   _cfg;
static bool        _loaded = false;

static void _setDefaults() {
    strlcpy(_cfg.wifi_ssid,   WIFI_SSID,   sizeof(_cfg.wifi_ssid));
    strlcpy(_cfg.wifi_pass,   WIFI_PASS,   sizeof(_cfg.wifi_pass));
    strlcpy(_cfg.firebase_url,  FIREBASE_URL,  sizeof(_cfg.firebase_url));
    strlcpy(_cfg.firebase_auth, FIREBASE_AUTH, sizeof(_cfg.firebase_auth));
    strlcpy(_cfg.audio_url,   AUDIO_CLOUD_URL, sizeof(_cfg.audio_url));
    _cfg.confianca_minima    = 0.65f;
    _cfg.temp_max_conforto   = 28.0f;
    _cfg.temp_min_conforto   = 18.0f;
    _cfg.rms_threshold       = 0.010f;
    _cfg.sensor_intervalo_ms = 5000;
    _cfg.volume_audio        = 100;
    _cfg.light_sleep_min     = 10;
    _cfg.gatilho_decisao     = 7;
    _cfg.gatilho_silencio    = 3;
}

void load() {
    _setDefaults();
    _prefs.begin("crysense", true);
    _prefs.getString("wifi_ssid",   _cfg.wifi_ssid,    sizeof(_cfg.wifi_ssid));
    _prefs.getString("wifi_pass",   _cfg.wifi_pass,    sizeof(_cfg.wifi_pass));
    _prefs.getString("fb_url",      _cfg.firebase_url, sizeof(_cfg.firebase_url));
    _prefs.getString("fb_auth",     _cfg.firebase_auth,sizeof(_cfg.firebase_auth));
    _prefs.getString("audio_url",   _cfg.audio_url,    sizeof(_cfg.audio_url));
    _cfg.confianca_minima    = _prefs.getFloat("conf_min",     _cfg.confianca_minima);
    _cfg.temp_max_conforto   = _prefs.getFloat("temp_max",     _cfg.temp_max_conforto);
    _cfg.temp_min_conforto   = _prefs.getFloat("temp_min",     _cfg.temp_min_conforto);
    _cfg.rms_threshold       = _prefs.getFloat("rms_thr",      _cfg.rms_threshold);
    _cfg.sensor_intervalo_ms = _prefs.getUInt( "sensor_ms",    _cfg.sensor_intervalo_ms);
    _cfg.volume_audio        = _prefs.getUChar("volume",       _cfg.volume_audio);
    _cfg.light_sleep_min     = _prefs.getUInt( "sleep_min",    _cfg.light_sleep_min);
    _cfg.gatilho_decisao     = _prefs.getUChar("gatilho_d",    _cfg.gatilho_decisao);
    _cfg.gatilho_silencio    = _prefs.getUChar("gatilho_s",    _cfg.gatilho_silencio);
    _prefs.end();
    _loaded = true;
}

bool save(const CryConfig& novo) {
    const float eps = 0.0001f;

    if (memcmp(&_cfg, &novo, sizeof(CryConfig)) == 0) {
        return false;
    }

    bool wrote = false;
    _prefs.begin("crysense", false);

    if (strcmp(_cfg.wifi_ssid, novo.wifi_ssid) != 0) {
        _prefs.putString("wifi_ssid", novo.wifi_ssid);
        wrote = true;
    }
    if (strcmp(_cfg.wifi_pass, novo.wifi_pass) != 0) {
        _prefs.putString("wifi_pass", novo.wifi_pass);
        wrote = true;
    }
    if (strcmp(_cfg.firebase_url, novo.firebase_url) != 0) {
        _prefs.putString("fb_url", novo.firebase_url);
        wrote = true;
    }
    if (strcmp(_cfg.firebase_auth, novo.firebase_auth) != 0) {
        _prefs.putString("fb_auth", novo.firebase_auth);
        wrote = true;
    }
    if (strcmp(_cfg.audio_url, novo.audio_url) != 0) {
        _prefs.putString("audio_url", novo.audio_url);
        wrote = true;
    }
    if (fabsf(_cfg.confianca_minima - novo.confianca_minima) > eps) {
        _prefs.putFloat("conf_min", novo.confianca_minima);
        wrote = true;
    }
    if (fabsf(_cfg.temp_max_conforto - novo.temp_max_conforto) > eps) {
        _prefs.putFloat("temp_max", novo.temp_max_conforto);
        wrote = true;
    }
    if (fabsf(_cfg.temp_min_conforto - novo.temp_min_conforto) > eps) {
        _prefs.putFloat("temp_min", novo.temp_min_conforto);
        wrote = true;
    }
    if (fabsf(_cfg.rms_threshold - novo.rms_threshold) > eps) {
        _prefs.putFloat("rms_thr", novo.rms_threshold);
        wrote = true;
    }
    if (_cfg.sensor_intervalo_ms != novo.sensor_intervalo_ms) {
        _prefs.putUInt("sensor_ms", novo.sensor_intervalo_ms);
        wrote = true;
    }
    if (_cfg.volume_audio != novo.volume_audio) {
        _prefs.putUChar("volume", novo.volume_audio);
        wrote = true;
    }
    if (_cfg.light_sleep_min != novo.light_sleep_min) {
        _prefs.putUInt("sleep_min", novo.light_sleep_min);
        wrote = true;
    }
    if (_cfg.gatilho_decisao != novo.gatilho_decisao) {
        _prefs.putUChar("gatilho_d", novo.gatilho_decisao);
        wrote = true;
    }
    if (_cfg.gatilho_silencio != novo.gatilho_silencio) {
        _prefs.putUChar("gatilho_s", novo.gatilho_silencio);
        wrote = true;
    }

    _prefs.end();
    _cfg = novo;
    return wrote;
}

void reset() {
    _prefs.begin("crysense", false);
    _prefs.clear();
    _prefs.end();
    _setDefaults();
}

CryConfig& get() {
    if (!_loaded) load();
    return _cfg;
}

}
