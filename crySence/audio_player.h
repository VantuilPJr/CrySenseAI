// =============================================================================
// CrySense AI v2.0 — audio_player.h
// Reprodução de áudio para mitigação de cólica com 3 modos em cascata:
//   1. SPIFFS: /audio_colica.wav (uploaded via web ou serial)
//   2. Cloud:  URL HTTP configurável (streaming)
//   3. Sintético: Brown noise gerado por LFSR (fallback offline)
// Formato WAV esperado: PCM 16-bit, mono, 16kHz
// =============================================================================
#pragma once
#include "driver/i2s.h"
#include <SPIFFS.h>
#include <HTTPClient.h>
#include "config_manager.h"  // InferenceResult, qResultadoIA, LABEL_NOISE

// --- Pinos / Port do alto-falante (I2S_1 = MAX98357A) ---
#define SPK_WS      18
#define SPK_SCK     17
#define SPK_DIN     16
#define SPK_SD_PIN   5    // Shutdown/Enable do amplificador
#define SPK_PORT    I2S_NUM_1
#define SPK_SRATE   16000
#define SPK_BUF_SZ  2048  // bytes por envio I2S

#define MIC_WS      15
#define MIC_SD      13
#define MIC_SCK     14
#define MIC_PORT    I2S_NUM_0
#define SAMPLE_RATE 16000
#define GAIN_MULTIPLIER 2.5f

#define AUDIO_SPIFFS_PATH "/audio_colica.wav"

extern SemaphoreHandle_t semAudioPronto;
extern float* inference_buffer;
// Fallback caso a biblioteca de IA ainda não esteja incluída (evita erros do compilador)
#ifndef EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE
#define EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE 16000
#endif

namespace AudioPlayer {

static volatile bool  _tocando   = false;
static volatile bool  _parar     = false;
static uint8_t        _volume    = 70;  // 0-100
static TaskHandle_t   _taskHandle = nullptr;
static TaskHandle_t   _taskMicHandle = nullptr;
static int32_t*       _rawBuf = nullptr; // Buffer do microfone

// --- Inicializa Hardware I2S (Microfone e Speaker) ---
static void setupHardware() {
    // --- MICROFONE ---
    i2s_config_t cfg_mic = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 512,
        .use_apll = false
    };
    i2s_pin_config_t pins_mic = {
        .bck_io_num   = MIC_SCK,
        .ws_io_num    = MIC_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = MIC_SD
    };
    i2s_driver_install(MIC_PORT, &cfg_mic, 0, NULL);
    i2s_set_pin(MIC_PORT, &pins_mic);

    // --- SPEAKER ---
    i2s_config_t cfg_spk = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SPK_SRATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false
    };
    i2s_pin_config_t pins_spk = {
        .bck_io_num   = SPK_SCK,
        .ws_io_num    = SPK_WS,
        .data_out_num = SPK_DIN,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };
    i2s_driver_install(SPK_PORT, &cfg_spk, 0, NULL);
    i2s_set_pin(SPK_PORT, &pins_spk);
    pinMode(SPK_SD_PIN, OUTPUT);
    digitalWrite(SPK_SD_PIN, LOW); // Silenciado inicialmente

    _rawBuf = (int32_t*)ps_malloc((EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE/4) * sizeof(int32_t));
}
// --- Liga/desliga amplificador ---
static inline void _ampOn()  { digitalWrite(SPK_SD_PIN, HIGH); vTaskDelay(pdMS_TO_TICKS(30)); }
static inline void _ampOff() { i2s_zero_dma_buffer(SPK_PORT); vTaskDelay(pdMS_TO_TICKS(50)); digitalWrite(SPK_SD_PIN, LOW); }

// --- Aplica volume a buffer int16 ---
static void _aplicarVolume(int16_t* buf, size_t n) {
    float gain = _volume / 100.0f;
    for (size_t i = 0; i < n; i++) {
        int32_t s = (int32_t)(buf[i] * gain);
        buf[i] = (int16_t)constrain(s, -32768, 32767);
    }
}

// --- Modo 3: Brown noise sintético (LFSR + integrador) ---
static void _tocarBrownNoise() {
    Serial.println("[AUDIO] Modo 3: Brown noise sintético");
    uint16_t lfsr = 0xACE1u;
    int32_t  brown = 0;
    static int16_t chunk[SPK_BUF_SZ / 2];
    size_t bw;
    while (!_parar) {
        for (int i = 0; i < (int)(SPK_BUF_SZ / 2); i++) {
            lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
            brown += ((int16_t)(lfsr & 0x1FF)) - 255;
            brown  = constrain(brown, -12000, 12000);
            chunk[i] = (int16_t)(brown * (_volume / 100.0f));
        }
        i2s_write(SPK_PORT, chunk, SPK_BUF_SZ, &bw, pdMS_TO_TICKS(200));
        taskYIELD();
    }
}

// --- Modo 1: Reproduz WAV do SPIFFS (pula cabeçalho de 44 bytes) ---
static bool _tocarSpiffs() {
    if (!SPIFFS.exists(AUDIO_SPIFFS_PATH)) return false;
    File f = SPIFFS.open(AUDIO_SPIFFS_PATH, "r");
    if (!f || f.size() < 44) { f.close(); return false; }
    Serial.println("[AUDIO] Modo 1: SPIFFS " AUDIO_SPIFFS_PATH);
    f.seek(44); // pula cabeçalho WAV
    static uint8_t buf[SPK_BUF_SZ];
    size_t bw;
    while (f.available() && !_parar) {
        size_t n = f.read(buf, SPK_BUF_SZ);
        _aplicarVolume((int16_t*)buf, n / 2);
        i2s_write(SPK_PORT, buf, n, &bw, pdMS_TO_TICKS(200));
        taskYIELD();
    }
    f.close();
    return true;
}

// --- Modo 2: Stream HTTP WAV ---
static bool _tocarStream(const char* url) {
    if (!url || strlen(url) == 0) return false;
    Serial.printf("[AUDIO] Modo 2: Stream %s\n", url);
    HTTPClient http;
    WiFiClient client;
    http.begin(client, url);
    http.setTimeout(5000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        Serial.printf("[AUDIO] HTTP erro %d — fallback\n", code);
        return false;
    }
    WiFiClient* stream = http.getStreamPtr();
    static uint8_t buf[SPK_BUF_SZ];
    size_t bw;
    bool header_skip = true;
    int skipped = 0;
    while (http.connected() && !_parar) {
        size_t avail = stream->available();
        if (avail == 0) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        size_t n = stream->readBytes(buf, min(avail, (size_t)SPK_BUF_SZ));
        if (header_skip) {
            // Pula cabeçalho WAV acumulando 44 bytes
            if (skipped + (int)n >= 44) {
                int off = 44 - skipped;
                _aplicarVolume((int16_t*)(buf+off), (n-off)/2);
                i2s_write(SPK_PORT, buf+off, n-off, &bw, pdMS_TO_TICKS(200));
                header_skip = false;
            }
            skipped += n;
        } else {
            _aplicarVolume((int16_t*)buf, n/2);
            i2s_write(SPK_PORT, buf, n, &bw, pdMS_TO_TICKS(200));
        }
        taskYIELD();
    }
    http.end();
    return true;
}

// --- Task de reprodução (roda em Core 0 com baixa prioridade) ---
struct AudioParams { char url[200]; };
static AudioParams _params;

static void _taskAudio(void* pv) {
    _tocando = true;
    _parar   = false;
    _ampOn();

    // Cascata: SPIFFS → HTTP Stream → Brown noise
    bool ok = _tocarSpiffs();
    if (!ok && strlen(_params.url) > 0) ok = _tocarStream(_params.url);
    if (!ok) _tocarBrownNoise();  // fallback infinito até _parar=true

    _ampOff();
    _tocando    = false;
    _taskHandle = nullptr;
    vTaskDelete(nullptr);
}

// --- API Pública ---
void setVolume(uint8_t vol) { _volume = min(vol, (uint8_t)100); }

void iniciar(const char* cloudUrl = "") {
    if (_tocando) return;
    strlcpy(_params.url, cloudUrl ? cloudUrl : "", sizeof(_params.url));
    _parar = false;
    xTaskCreatePinnedToCore(
        _taskAudio, "TaskAudio_Spk",
        8192, nullptr, 2, &_taskHandle, 0
    );
}

void parar() {
    _parar = true;
    // Aguarda task terminar (máx 3s)
    for (int i = 0; i < 30 && _tocando; i++) vTaskDelay(pdMS_TO_TICKS(100));
}

bool tocando() { return _tocando; }

// --- Upload de arquivo WAV via dados brutos (usado pela web) ---
bool salvarWavSpiffs(const uint8_t* data, size_t len) {
    File f = SPIFFS.open(AUDIO_SPIFFS_PATH, "w");
    if (!f) return false;
    f.write(data, len);
    f.close();
    Serial.printf("[AUDIO] WAV salvo em SPIFFS: %u bytes\n", len);
    return true;
}

bool temArquivoLocal() { return SPIFFS.exists(AUDIO_SPIFFS_PATH); }
size_t tamanhoArquivo() {
    if (!SPIFFS.exists(AUDIO_SPIFFS_PATH)) return 0;
    File f = SPIFFS.open(AUDIO_SPIFFS_PATH, "r");
    size_t s = f.size(); f.close(); return s;
}

// --- Task de Leitura e Gate do Microfone ---
static void TaskAudio(void* pv) {
    LogManager::info("[TaskAudio] Iniciada no Core 1");
    const int CHUNK = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE / 4; 
    while (true) {
        uint64_t t0 = esp_timer_get_time();
        if (!_rawBuf || !inference_buffer) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        for (int chunk = 0; chunk < 4; chunk++) {
            size_t bytesRead = 0;
            i2s_read(MIC_PORT, _rawBuf, CHUNK * sizeof(int32_t),
                     &bytesRead, pdMS_TO_TICKS(2000));
            int n = bytesRead / sizeof(int32_t);
            int off = chunk * CHUNK;
            for (int i = 0; i < n; i++) {
                float s = ((float)_rawBuf[i] / 2147483648.0f) * GAIN_MULTIPLIER;
                if (s >  1.0f) s =  1.0f;
                if (s < -1.0f) s = -1.0f;
                inference_buffer[off + i] = s;
            }
            if (n < CHUNK) {
                memset(&inference_buffer[off + n], 0, (CHUNK - n) * sizeof(float));
            }
        }

        // --- Gate de Energia (RMS) ---
        float sumSq = 0;
        for (int i = 0; i < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; i++) {
            sumSq += inference_buffer[i] * inference_buffer[i];
        }
        float rms = sqrt(sumSq / EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);

        // Threshold de 0.025f: com ganho 2.5x, corresponde a ~1% de amplitude real.
        // Abaixo disso é silêncio — não desperdiça CPU de inferência e evita falsos positivos.
        bool silencioso = (rms < 0.025f);

        if (silencioso) {
            // Injeta resultado "noise/silêncio" diretamente na fila, bypassando a IA
            InferenceResult ir = {};
            strlcpy(ir.label, LABEL_NOISE, sizeof(ir.label));
            ir.confianca     = 1.0f;
            ir.isSilencioReal = true;
            ir.infMs          = 0.0f;
            xQueueSend(qResultadoIA, &ir, 0);
        } else {
            // Áudio com energia: acorda TaskIA para inferência completa
            xSemaphoreGive(semAudioPronto);
        }

        float execMs = (float)(esp_timer_get_time() - t0) / 1000.0f;
        Serial.printf("[TaskAudio] RMS=%.4f %s (%.1fms)\n",
                      rms, silencioso ? "SILENCIO" : "som", execMs);
    }
}

// --- Funções de API Mestre de Áudio ---
static void beginTasks() {
    setupHardware();
    
    // Spawn Task de Microfone (Prio 5)
    xTaskCreatePinnedToCore(TaskAudio, "TaskMic", 8192, nullptr, 5, &_taskMicHandle, 1);
}

} // namespace AudioPlayer
