// =============================================================================
// =============================================================================
#pragma once
#include "driver/i2s.h"
#include <SPIFFS.h>
#include <HTTPClient.h>
#include "config_manager.h"
#include "log_manager.h"
#include "heap_debug.h"

#define SPK_WS      18
#define SPK_SCK     17
#define SPK_DIN     16
#define SPK_SD_PIN   5
#define SPK_PORT    I2S_NUM_1
#define SPK_SRATE   16000
#define SPK_BUF_SZ  2048

#define MIC_WS      15
#define MIC_SD      13
#define MIC_SCK     14
#define MIC_PORT    I2S_NUM_0
#define SAMPLE_RATE 16000
#define GAIN_MULTIPLIER 2.0f

#define AUDIO_SPIFFS_PATH "/audio_colica.wav"
#define VERBOSE_AUDIO_TIMING 0

extern SemaphoreHandle_t semAudioPronto;
extern float* inference_buffer;
#ifndef EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE
#define EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE 16000
#endif

namespace AudioPlayer {

static volatile bool  _tocando   = false;
static volatile bool  _parar     = false;
static uint8_t        _volume    = 70;
static TaskHandle_t   _taskHandle = nullptr;
static TaskHandle_t   _taskMicHandle = nullptr;
static int32_t*       _rawBuf = nullptr;
static size_t         _rawBufSamples = 0;

static void setupHardware() {
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
    digitalWrite(SPK_SD_PIN, LOW);

    // CHUNK eh lido em amostras de 32 bits pelo i2s_read.
    // O tamanho anterior (FRAME/4) era insuficiente e causava overflow.
    _rawBufSamples = (size_t)(EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE / 2);
    _rawBuf = (int32_t*)ps_malloc(_rawBufSamples * sizeof(int32_t));
    if (!_rawBuf) {
        Serial.println("[AUDIO] ERRO: falha ao alocar buffer de captura");
    }
}
static inline void _ampOn()  { digitalWrite(SPK_SD_PIN, HIGH); vTaskDelay(pdMS_TO_TICKS(30)); }
static inline void _ampOff() { i2s_zero_dma_buffer(SPK_PORT); vTaskDelay(pdMS_TO_TICKS(50)); digitalWrite(SPK_SD_PIN, LOW); }

static void _aplicarVolume(int16_t* buf, size_t n) {
    float gain = _volume / 100.0f;
    for (size_t i = 0; i < n; i++) {
        int32_t s = (int32_t)(buf[i] * gain);
        buf[i] = (int16_t)constrain(s, -32768, 32767);
    }
}

static void _tocarChuva(int duracao_seg) {
    Serial.println("[AUDIO] Iniciando Som de Chuva Contínuo");
    static int16_t chunk[SPK_BUF_SZ / 2];
    size_t bw;
    
    uint64_t t_start = esp_timer_get_time();
    uint64_t t_end = t_start + ((uint64_t)duracao_seg * 1000000ULL);
    
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    
    while (!_parar && esp_timer_get_time() < t_end) {
        for (int i = 0; i < (int)(SPK_BUF_SZ / 2); i++) {
            float white = ((int32_t)(esp_random() % 24000) - 12000) / 12000.0f;
            
            b0 = 0.99886f * b0 + white * 0.0555179f;
            b1 = 0.99332f * b1 + white * 0.0750759f;
            b2 = 0.96900f * b2 + white * 0.1538520f;
            b3 = 0.86650f * b3 + white * 0.3104856f;
            b4 = 0.55000f * b4 + white * 0.5329522f;
            b5 = -0.7616f * b5 - white * 0.0168980f;
            float pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362f;
            b6 = white * 0.115926f;
            
            if (pink >  4.0f) pink =  4.0f;
            if (pink < -4.0f) pink = -4.0f;
            
            chunk[i] = (int16_t)(pink * 6000.0f * (_volume / 100.0f));
        }
        i2s_write(SPK_PORT, chunk, SPK_BUF_SZ, &bw, pdMS_TO_TICKS(200));
        taskYIELD();
    }
}

static void _taskAudio(void* pv) {
    _tocando = true;
    _parar   = false;
    _ampOn();

    int duracao = (int)(intptr_t)pv;
    if (duracao <= 0) duracao = 60;

    _tocarChuva(duracao);

    _ampOff();
    _tocando    = false;
    _taskHandle = nullptr;
    vTaskDelete(nullptr);
}

void setVolume(uint8_t vol) { _volume = min(vol, (uint8_t)100); }

void iniciar(int duracao_seg = 60) {
    if (_tocando) return;
    _parar = false;
    xTaskCreatePinnedToCore(
        _taskAudio, "TaskAudio_Spk",
        12288, (void*)(intptr_t)duracao_seg, 2, &_taskHandle, 0
    );
}

void parar() {
    _parar = true;
    for (int i = 0; i < 30 && _tocando; i++) vTaskDelay(pdMS_TO_TICKS(100));
}

bool tocando() { return _tocando; }

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

static void TaskAudio(void* pv) {
    LogManager::info("[TaskAudio] Iniciada no Core 1");
    const int CHUNK = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE / 2;
    bool primed = false;
    while (true) {
        uint64_t t0 = esp_timer_get_time();
        if (!_rawBuf || !inference_buffer) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int samplesPerChunk = CHUNK;
        int totalLidos = 0;

        int loops = primed ? 1 : 2;
        if (primed) {
            memmove(inference_buffer,
                    inference_buffer + CHUNK,
                    CHUNK * sizeof(float));
        }

        for (int chunk = 0; chunk < loops; chunk++) {
            size_t bytesRead = 0;
            i2s_read(MIC_PORT, _rawBuf, samplesPerChunk * sizeof(int32_t),
                     &bytesRead, pdMS_TO_TICKS(2000));
            int n = bytesRead / sizeof(int32_t);
            if (n > samplesPerChunk) n = samplesPerChunk;
            int off = primed ? CHUNK : (chunk * samplesPerChunk);
            
            for (int i = 0; i < n; i++) {
                float s = ((float)(_rawBuf[i] >> 8) / 8388608.0f) * GAIN_MULTIPLIER; 
                if (s >  1.0f) s =  1.0f;
                if (s < -1.0f) s = -1.0f;
                inference_buffer[off + i] = s;
            }
            if (n < samplesPerChunk) {
                memset(&inference_buffer[off + n], 0, (samplesPerChunk - n) * sizeof(float));
            }
            totalLidos += n;
        }
        if (!primed) primed = true;

        float sum = 0.0f;
        for (int i = 0; i < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; i++) {
            sum += inference_buffer[i];
        }
        float mean = sum / EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;

        float sumSq = 0.0f;
        for (int i = 0; i < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; i++) {
            float val = inference_buffer[i] - mean;
            sumSq += val * val;
        }
        float rms = sqrt(sumSq / EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);

        float cfgThr = ConfigManager::get().rms_threshold;
        if (cfgThr < 0.004f) cfgThr = 0.004f;
        if (cfgThr > 0.012f) cfgThr = 0.012f;

        const int amostrasEsperadas = primed ? CHUNK : EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
        const int amostrasMinimasRequeridas = (amostrasEsperadas * 3) / 4;
        bool frameIncompleto = (totalLidos < amostrasMinimasRequeridas);

        static float noiseFloor = 0.006f;
        if (!_tocando && !frameIncompleto && rms < cfgThr) {
            noiseFloor = (noiseFloor * 0.98f) + (rms * 0.02f);
        }
        float adaptiveThr = noiseFloor + 0.0018f;
        if (adaptiveThr < 0.005f) adaptiveThr = 0.005f;
        if (adaptiveThr > 0.010f) adaptiveThr = 0.010f;

        const float RMS_THRESHOLD = (cfgThr < adaptiveThr) ? cfgThr : adaptiveThr;

        //    TaskDecisao fará timeout natural de 2s. Isso impede que o contador
        bool silencioReal    = (rms < RMS_THRESHOLD) && !_tocando;
        bool lowLevelCandidate =
            !_tocando && !frameIncompleto &&
            (rms >= (RMS_THRESHOLD * 0.75f)) && (rms < RMS_THRESHOLD);
        bool speakerAtivo    = _tocando;

        if (speakerAtivo) {
            static uint8_t _logCnt = 0;
            if (++_logCnt >= 10) {
                _logCnt = 0;
#if VERBOSE_AUDIO_TIMING
                Serial.printf("[TaskAudio] Speaker ativo (RMS=%.3f) - mic gateado\n", rms);
#endif
            }
        } else if ((silencioReal && !lowLevelCandidate) || frameIncompleto) {
            InferenceResult ir = {};
            strlcpy(ir.label, LABEL_NOISE, sizeof(ir.label));
            ir.confianca      = 1.0f;
            ir.isSilencioReal = silencioReal;
            ir.infMs          = 0.0f;
            if (xQueueSend(qResultadoIA, &ir, 0) != pdTRUE) {
                LogManager::warning("[TaskAudio] Fila qResultadoIA cheia (silencio)");
            }

#if VERBOSE_AUDIO_TIMING
            float execMs = (float)(esp_timer_get_time() - t0) / 1000.0f;
            Serial.printf("[TaskAudio] RMS=%.4f (limiar=%.3f) SILENCIO%s (%.1fms)\n",
                          rms, RMS_THRESHOLD,
                          frameIncompleto ? " [FRAME-INC]" : "",
                          execMs);
#endif
        } else {
            xSemaphoreGive(semAudioPronto);

#if VERBOSE_AUDIO_TIMING
            float execMs = (float)(esp_timer_get_time() - t0) / 1000.0f;
            Serial.printf("[TaskAudio] RMS=%.4f (limiar=%.3f)%s SOM (%.1fms)\n",
                          rms, RMS_THRESHOLD,
                          lowLevelCandidate ? " [LIMIAR]" : "",
                          execMs);
#endif
        }
    }
}

static void beginTasks(TaskHandle_t* pTaskAudio = nullptr, TaskHandle_t* pTaskPlayer = nullptr) {
    setupHardware();
    
    xTaskCreatePinnedToCore(TaskAudio, "TaskMic", 12288, nullptr, 5, &_taskMicHandle, 1);
    
    if (pTaskAudio) {
        *pTaskAudio = _taskMicHandle;
    }
    
    if (pTaskPlayer) {
        *pTaskPlayer = nullptr;
    }
}

}
