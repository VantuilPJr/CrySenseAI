#pragma once
#include <SPIFFS.h>
#include <ArduinoJson.h>

#define LOG_FILE_PATH           "/crylogs.csv"
#define LOG_MAX_BYTES           (512 * 1024)   // 500KB limite do arquivo SPIFFS
#define LOG_RAM_SIZE            150            // Entradas aumentadas no buffer RAM (pois flush auto foi desativado)
#define LOG_FLUSH_INTERVAL_MS   60000UL        // (Depreciado) Flush para SPIFFS a cada 60s

namespace LogManager {

// --- Estrutura de uma entrada de log em RAM ---
struct LogEntry {
    uint64_t ts;       // timestamp em ms (esp_timer)
    char     level;    // 'I', 'W', 'E', 'A'
    char     msg[192]; // mensagem (truncada se necessário)
};

static SemaphoreHandle_t  _mutex      = nullptr;
static bool               _started    = false;
static uint32_t           _count      = 0;

// Ring buffer em RAM — nenhuma escrita na Flash até flush()
static LogEntry           _ramBuf[LOG_RAM_SIZE];
static int                _ramHead    = 0;  // índice de escrita (circular)
static int                _ramCount   = 0;  // quantas entradas pendentes de flush

// --- Rotação do arquivo SPIFFS: remove primeiras 600 linhas se > limite ---
static void _rotacionar() {
    logOtaSpiffsAccess("LogManager::_rotacionar SPIFFS open/remove/rename");
    size_t sz = 0;
    if (SPIFFS.exists(LOG_FILE_PATH)) {
        File fSz = SPIFFS.open(LOG_FILE_PATH, "r");
        if (fSz) { sz = fSz.size(); fSz.close(); }
    }
    if (sz <= LOG_MAX_BYTES) return;

    File fIn  = SPIFFS.open(LOG_FILE_PATH, "r");
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

// --- Escreve apenas no buffer RAM (rápido, sem Flash) ---
static void _write(char level, const char* msg) {
    if (!_started) {
        // Antes do begin(), espelha só no Serial para não perder mensagens de boot
        Serial.printf("[LOG %c] %s\n", level, msg);
        return;
    }

    // Tenta pegar mutex sem bloquear por mais de 20ms (seguro em ISR context)
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;

    LogEntry& e = _ramBuf[_ramHead];
    e.ts    = (uint64_t)esp_timer_get_time() / 1000ULL;
    e.level = level;
    strlcpy(e.msg, msg, sizeof(e.msg));

    _ramHead = (_ramHead + 1) % LOG_RAM_SIZE;
    if (_ramCount < LOG_RAM_SIZE) _ramCount++;

    // Espelha no Serial imediatamente (sem custo de Flash)
    Serial.printf("[LOG %c] %s\n", level, msg);

    xSemaphoreGive(_mutex);

    // Se buffer está cheio, agenda flush imediato na próxima oportunidade
    // (não fazemos flush aqui pois podemos estar em contexto de task/ISR)
}

// =============================================================================
// flush() — Grava buffer RAM no SPIFFS. Deve ser chamado periodicamente
// pelo loop() principal. Seguro chamar de qualquer task.
// =============================================================================
void flush() {
    if (!_started) return;
    logOtaSpiffsAccess("LogManager::flush SPIFFS open/append");
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;

    if (_ramCount == 0) {
        xSemaphoreGive(_mutex);
        return;
    }

    // Rotaciona se arquivo muito grande
    _rotacionar();

    // Abre arquivo em modo append — UMA única abertura para TODAS as entradas
    File f = SPIFFS.open(LOG_FILE_PATH, "a");
    if (f) {
        // Calcula índice da entrada mais antiga no ring buffer circular
        int startIdx = (_ramHead - _ramCount + LOG_RAM_SIZE) % LOG_RAM_SIZE;
        for (int i = 0; i < _ramCount; i++) {
            int idx = (startIdx + i) % LOG_RAM_SIZE;
            f.printf("%llu,%c,%s\n", _ramBuf[idx].ts, _ramBuf[idx].level, _ramBuf[idx].msg);
            _count++;
        }
        f.close();
        Serial.printf("[LOG] Flush: %d entradas gravadas em SPIFFS (total: %lu)\n",
                      _ramCount, _count);
    }
    _ramCount = 0;  // reseta buffer após flush

    xSemaphoreGive(_mutex);
}

// =============================================================================
// API pública
// =============================================================================
void begin() {
    logOtaSpiffsAccess("LogManager::begin SPIFFS.begin/load count");
    if (!SPIFFS.begin(true)) {
        Serial.println("[LOG] SPIFFS falhou!");
        return;
    }
    _mutex   = xSemaphoreCreateMutex();
    _started = true;
    _ramHead = 0;
    _ramCount = 0;

    // Conta entradas existentes (sem gravar nada)
    if (SPIFFS.exists(LOG_FILE_PATH)) {
        File f = SPIFFS.open(LOG_FILE_PATH, "r");
        while (f.available()) { f.readStringUntil('\n'); _count++; }
        f.close();
    }
    Serial.printf("[LOG] SPIFFS OK. Entradas existentes: %lu. Buffer RAM: %d slots.\n",
                  _count, LOG_RAM_SIZE);
}

inline void info   (const char* msg) { _write('I', msg); }
inline void warning(const char* msg) { _write('W', msg); }
inline void error  (const char* msg) { _write('E', msg); }
inline void alerta (const char* msg) { _write('A', msg); }

// Printf-style
void infof(const char* fmt, ...) {
    char buf[192]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    _write('I', buf);
}
void alertaf(const char* fmt, ...) {
    char buf[192]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    _write('A', buf);
}
void errorf(const char* fmt, ...) {
    char buf[192]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    _write('E', buf);
}
void warningf(const char* fmt, ...) {
    char buf[192]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    _write('W', buf);
}

// Retorna últimas N entradas como JSON array
// Inclui tanto entradas já em SPIFFS quanto as ainda no buffer RAM
String getLogs(int limit = 200) {
    if (!_started) return "[]";

    logOtaSpiffsAccess("LogManager::getLogs SPIFFS read");

    std::vector<String> lines;
    lines.reserve(limit);

    // 1. Lê SPIFFS
    // Para evitar lines.erase(lines.begin()) que é O(n), apenas contamos e pulamos
    int totalSpiffsLines = 0;
    if (SPIFFS.exists(LOG_FILE_PATH)) {
        File f = SPIFFS.open(LOG_FILE_PATH, "r");
        while (f.available()) { f.readStringUntil('\n'); totalSpiffsLines++; }
        f.close();
    }
    
    int skip = 0;
    if (totalSpiffsLines + _ramCount > limit) {
        skip = (totalSpiffsLines + _ramCount) - limit;
    }

    if (SPIFFS.exists(LOG_FILE_PATH)) {
        File f = SPIFFS.open(LOG_FILE_PATH, "r");
        while (f.available()) {
            String l = f.readStringUntil('\n');
            if (skip > 0) { skip--; continue; }
            l.trim();
            if (l.length() > 0) {
                lines.push_back(l);
            }
        }
        f.close();
    }

    // 2. Adiciona entradas pendentes no RAM buffer (mais recentes)
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int startIdx = (_ramHead - _ramCount + LOG_RAM_SIZE) % LOG_RAM_SIZE;
        for (int i = 0; i < _ramCount; i++) {
            if (skip > 0) { skip--; continue; }
            int idx = (startIdx + i) % LOG_RAM_SIZE;
            char tmp[220];
            snprintf(tmp, sizeof(tmp), "%llu,%c,%s",
                     _ramBuf[idx].ts, _ramBuf[idx].level, _ramBuf[idx].msg);
            lines.push_back(String(tmp));
        }
        xSemaphoreGive(_mutex);
    }

    String json = "[";
    for (size_t i = 0; i < lines.size(); i++) {
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
    // Flush antes de exportar para incluir entradas recentes
    flush();
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
    _count    = 0;
    _ramCount = 0;
    _ramHead  = 0;
    if (_mutex) xSemaphoreGive(_mutex);
}

uint32_t getCount()    { return _count + _ramCount; }
int      getPending()  { return _ramCount; }

} // namespace LogManager
