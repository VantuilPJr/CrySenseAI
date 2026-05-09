// =============================================================================
// CrySense AI v2.0 — history_manager.h
// Histórico contínuo de 24h de leituras de sensores + estado da IA
//
// Estratégia de proteção da Flash:
//   - Dados acumulados em PSRAM (ring buffer circular, sem custo de Flash)
//   - Flush para SPIFFS em lote a cada 30 minutos → ~48 escritas/dia
//   - Com 100.000 ciclos de garantia da NAND → vida útil estimada > 5 anos
//   - No boot: recarrega /history.bin do SPIFFS para restaurar o histórico
// =============================================================================
#pragma once
#include <SPIFFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config_manager.h"

// --- Parâmetros configuráveis ---
#define HISTORY_SAMPLE_INTERVAL_MS  30000UL   // Amostra a cada 30 segundos
#define HISTORY_FLUSH_INTERVAL_MS   1800000UL // Flush para SPIFFS a cada 30 minutos
#define HISTORY_MAX_SAMPLES         2880      // 24h * 60min * 2 amostras/min = 2880
#define HISTORY_FILE_PATH           "/history.bin"

namespace HistoryManager {

// --- Estrutura de uma amostra (28 bytes, compacta para minimizar RAM) ---
struct SensorSample {
    uint32_t ts_s;       // Timestamp: segundos desde o boot
    float    temp;       // Temperatura em °C
    float    umid;       // Umidade relativa em %
    float    pres;       // Pressão atmosférica em hPa
    uint8_t  conf;       // Confiança da IA local (0-100)
    uint8_t  result_conf;// Confiança do classificador remoto (0-100)
    char     label[8];   // "noise", "cry", "colic", "hunger"
    char     estado[8];  // "calmo", "crise", "monitorando"
};

// Ring buffer em PSRAM
static SensorSample* _buf      = nullptr;
static int           _head     = 0;     // Próxima posição de escrita
static int           _count    = 0;     // Total de amostras no buffer
static SemaphoreHandle_t _mutex = nullptr;
static bool          _started  = false;

static uint32_t _lastSampleMs = 0;
static uint32_t _lastFlushMs  = 0;

// =============================================================================
// Internos
// =============================================================================

// Salva o buffer completo no SPIFFS como arquivo binário
static void _flush() {
    if (!_started || !_buf || _count == 0) return;

    // Pega mutex com timeout de 500ms para não bloquear tasks críticas
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;

    File f = SPIFFS.open(HISTORY_FILE_PATH, "w");
    if (f) {
        // Escreve em ordem cronológica: do mais antigo ao mais novo
        int start = (_head - _count + HISTORY_MAX_SAMPLES) % HISTORY_MAX_SAMPLES;
        for (int i = 0; i < _count; i++) {
            int idx = (start + i) % HISTORY_MAX_SAMPLES;
            f.write((const uint8_t*)&_buf[idx], sizeof(SensorSample));
        }
        f.close();
        Serial.printf("[HISTORY] Flush: %d amostras → %s (%.1fKB)\n",
                      _count, HISTORY_FILE_PATH,
                      (_count * sizeof(SensorSample)) / 1024.0f);
    } else {
        Serial.println("[HISTORY] ERRO: Não foi possível abrir arquivo para escrita");
    }

    xSemaphoreGive(_mutex);
}

// Carrega o arquivo binário salvo anteriormente de volta para o ring buffer
static void _load() {
    if (!SPIFFS.exists(HISTORY_FILE_PATH)) {
        Serial.println("[HISTORY] Nenhum histórico anterior encontrado.");
        return;
    }

    File f = SPIFFS.open(HISTORY_FILE_PATH, "r");
    if (!f) return;

    int loaded = 0;
    SensorSample s;
    // Limita ao máximo do buffer
    while (f.available() >= (int)sizeof(SensorSample) && _count < HISTORY_MAX_SAMPLES) {
        if (f.read((uint8_t*)&s, sizeof(SensorSample)) == sizeof(SensorSample)) {
            _buf[_head] = s;
            _head = (_head + 1) % HISTORY_MAX_SAMPLES;
            if (_count < HISTORY_MAX_SAMPLES) _count++;
            loaded++;
        }
    }
    f.close();
    Serial.printf("[HISTORY] Histórico carregado: %d amostras de %s\n", loaded, HISTORY_FILE_PATH);
}

// =============================================================================
// API pública
// =============================================================================

// Inicializa o módulo. Chamar no setup() após LogManager::begin().
void begin() {
    // Aloca o ring buffer na PSRAM (não toca a heap interna do chip)
    _buf = (SensorSample*)ps_malloc(HISTORY_MAX_SAMPLES * sizeof(SensorSample));
    if (!_buf) {
        Serial.println("[HISTORY] ERRO: Falha ao alocar buffer na PSRAM!");
        return;
    }
    memset(_buf, 0, HISTORY_MAX_SAMPLES * sizeof(SensorSample));

    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) {
        Serial.println("[HISTORY] ERRO: Falha ao criar mutex!");
        free(_buf);
        _buf = nullptr;
        return;
    }

    _head  = 0;
    _count = 0;
    _started = true;

    // Tenta restaurar histórico da sessão anterior
    _load();

    _lastSampleMs = millis();
    _lastFlushMs  = millis();

    Serial.printf("[HISTORY] Iniciado. Buffer: %d amostras × %uB = %.1fKB na PSRAM\n",
                  HISTORY_MAX_SAMPLES,
                  (unsigned int)sizeof(SensorSample),
                  (HISTORY_MAX_SAMPLES * sizeof(SensorSample)) / 1024.0f);
}

// Registra uma nova amostra. Chamar periodicamente na TaskDecisao.
// Retorna true se registrou, false se ainda no cooldown de 30s.
bool sample(const WebDashboardState& st) {
    if (!_started || !_buf) return false;

    const uint32_t now = millis();
    if ((now - _lastSampleMs) < HISTORY_SAMPLE_INTERVAL_MS) return false;
    _lastSampleMs = now;

    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;

    SensorSample& s = _buf[_head];
    s.ts_s       = st.uptime_s;
    s.temp       = st.temp;
    s.umid       = st.umid;
    s.pres       = st.pres;
    s.conf       = (uint8_t)(st.confianca * 100.0f);
    s.result_conf= (uint8_t)(st.result_conf * 100.0f);
    strlcpy(s.label,  st.label,  sizeof(s.label));
    strlcpy(s.estado, st.estado, sizeof(s.estado));

    _head = (_head + 1) % HISTORY_MAX_SAMPLES;
    if (_count < HISTORY_MAX_SAMPLES) _count++;

    xSemaphoreGive(_mutex);
    return true;
}

// Verifica se é hora de fazer flush e executa se necessário.
// Chamar no loop() principal — operação não-bloqueante.
void flushIfNeeded() {
    if (!_started) return;
    const uint32_t now = millis();
    if ((now - _lastFlushMs) >= HISTORY_FLUSH_INTERVAL_MS) {
        _lastFlushMs = now;
        _flush();
    }
}

// Força flush imediato (ex: antes de reiniciar via OTA).
void forceFlush() {
    if (!_started) return;
    _flush();
    _lastFlushMs = millis();
}

// Retorna o número de amostras no buffer.
int count() { return _count; }

// Serializa as últimas N amostras como JSON para a API web.
// Mantém dados na RAM, não lê a Flash.
String toJson(int limit = HISTORY_MAX_SAMPLES) {
    if (!_started || !_buf || _count == 0) return "[]";
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return "[]";

    const int n     = _count < limit ? _count : limit;
    const int start = (_head - n + HISTORY_MAX_SAMPLES) % HISTORY_MAX_SAMPLES;

    // Estima tamanho: cada amostra vira ~120 chars de JSON
    String json;
    json.reserve(n * 120 + 4);
    json = "[";

    for (int i = 0; i < n; i++) {
        const int idx = (start + i) % HISTORY_MAX_SAMPLES;
        const SensorSample& s = _buf[idx];
        if (i > 0) json += ",";
        json += "{\"ts\":";   json += s.ts_s;
        json += ",\"temp\":"; json += s.temp;
        json += ",\"umid\":"; json += s.umid;
        json += ",\"pres\":"; json += s.pres;
        json += ",\"conf\":"; json += s.conf;
        json += ",\"rc\":";   json += s.result_conf;
        json += ",\"lbl\":\""; json += s.label; json += "\"";
        json += ",\"est\":\""; json += s.estado; json += "\"";
        json += "}";
    }
    json += "]";

    xSemaphoreGive(_mutex);
    return json;
}

} // namespace HistoryManager
