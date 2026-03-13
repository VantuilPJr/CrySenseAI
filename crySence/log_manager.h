// =============================================================================
// CrySense AI v2.0 — log_manager.h
// Log persistente em SPIFFS com rotação automática (mín. 24h)
// Formato CSV: timestamp_ms,level,message
// =============================================================================
#pragma once
#include <SPIFFS.h>
#include <ArduinoJson.h>

#define LOG_FILE_PATH    "/crylogs.csv"
#define LOG_MAX_BYTES    (512 * 1024)   // 500KB = ~6000 entradas (> 24h em ciclos de 1s)
#define LOG_MAX_ENTRIES  6000

namespace LogManager {

static SemaphoreHandle_t  _mutex    = nullptr;
static uint32_t           _count    = 0;
static bool               _started  = false;

// Nível: 'I'=info, 'W'=warning, 'E'=error, 'A'=alerta
static void _write(char level, const char* msg) {
    if (!_started) return;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    // Rotação: se arquivo > limite, remove primeiros 10% das linhas
    size_t sz = 0;
    if (SPIFFS.exists(LOG_FILE_PATH)) {
        File fSz = SPIFFS.open(LOG_FILE_PATH, "r");
        if (fSz) { sz = fSz.size(); fSz.close(); }  // BUGFIX: fechar handle após leitura
    }
    if (sz > LOG_MAX_BYTES) {
        // Abre, descarta header, reescreve sem primeiras 600 linhas
        File fIn = SPIFFS.open(LOG_FILE_PATH, "r");
        File fTmp = SPIFFS.open("/crylogs_tmp.csv", "w");
        int skip = 600;
        while (fIn.available()) {
            String line = fIn.readStringUntil('\n');
            if (skip > 0) { skip--; continue; }
            fTmp.println(line);
        }
        fIn.close(); fTmp.close();
        SPIFFS.remove(LOG_FILE_PATH);
        SPIFFS.rename("/crylogs_tmp.csv", LOG_FILE_PATH);
    }

    File f = SPIFFS.open(LOG_FILE_PATH, "a");
    if (f) {
        uint64_t ts = (uint64_t)esp_timer_get_time() / 1000ULL;
        f.printf("%llu,%c,%s\n", ts, level, msg);
        f.close();
        _count++;
        // Espelha no Serial
        Serial.printf("[LOG %c] %s\n", level, msg);
    }
    xSemaphoreGive(_mutex);
}

// --- API pública ---
void begin() {
    if (!SPIFFS.begin(true)) {
        Serial.println("[LOG] SPIFFS falhou!");
        return;
    }
    _mutex   = xSemaphoreCreateMutex();
    _started = true;

    // Conta entradas existentes
    if (SPIFFS.exists(LOG_FILE_PATH)) {
        File f = SPIFFS.open(LOG_FILE_PATH, "r");
        while (f.available()) { f.readStringUntil('\n'); _count++; }
        f.close();
    }
    Serial.printf("[LOG] SPIFFS OK. Entradas existentes: %lu\n", _count);
}

inline void info   (const char* msg) { _write('I', msg); }
inline void warning(const char* msg) { _write('W', msg); }
inline void error  (const char* msg) { _write('E', msg); }
inline void alerta (const char* msg) { _write('A', msg); }

// Printf-style
void infof(const char* fmt, ...) {
    char buf[200]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    _write('I', buf);
}
void alertaf(const char* fmt, ...) {
    char buf[200]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    _write('A', buf);
}
void errorf(const char* fmt, ...) {
    char buf[200]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    _write('E', buf);
}

// Retorna últimas N entradas como JSON array
String getLogs(int limit = 200) {
    if (!_started || !SPIFFS.exists(LOG_FILE_PATH)) return "[]";
    
    // Lê arquivo em buffer circular (mantém últimas 'limit' linhas)
    std::vector<String> lines;
    lines.reserve(limit);
    File f = SPIFFS.open(LOG_FILE_PATH, "r");
    while (f.available()) {
        String l = f.readStringUntil('\n');
        l.trim();
        if (l.length() > 0) {
            if ((int)lines.size() >= limit) lines.erase(lines.begin());
            lines.push_back(l);
        }
    }
    f.close();

    String json = "[";
    for (size_t i = 0; i < lines.size(); i++) {
        // Format: "timestamp,level,message"
        int c1 = lines[i].indexOf(',');
        int c2 = lines[i].indexOf(',', c1 + 1);
        if (c1 < 0 || c2 < 0) continue;
        String ts  = lines[i].substring(0, c1);
        String lvl = lines[i].substring(c1+1, c2);
        String msg = lines[i].substring(c2+1);
        msg.replace("\"", "\\\"");
        if (i > 0) json += ",";
        json += "{\"ts\":" + ts + ",\"l\":\"" + lvl + "\",\"m\":\"" + msg + "\"}";
    }
    json += "]";
    return json;
}

// Exporta como CSV (para download)
String exportCsv() {
    if (!_started || !SPIFFS.exists(LOG_FILE_PATH)) return "timestamp_ms,level,message\n";
    File f = SPIFFS.open(LOG_FILE_PATH, "r");
    String csv = "timestamp_ms,level,message\n";
    while (f.available()) csv += f.readStringUntil('\n') + "\n";
    f.close();
    return csv;
}

void clear() {
    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    SPIFFS.remove(LOG_FILE_PATH);
    _count = 0;
    if (_mutex) xSemaphoreGive(_mutex);
}

uint32_t getCount() { return _count; }

} // namespace LogManager
