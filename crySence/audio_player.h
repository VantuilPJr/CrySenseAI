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
#include "log_manager.h"

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
#define REMOTE_CLIP_SECONDS   6
#define REMOTE_CLIP_SAMPLES   (SAMPLE_RATE * REMOTE_CLIP_SECONDS)
#define REMOTE_CLIP_PCM_BYTES (REMOTE_CLIP_SAMPLES * sizeof(int16_t))
#define REMOTE_CLIP_WAV_BYTES (44 + REMOTE_CLIP_PCM_BYTES)
// BUGFIX: Reduzido de 5.0f para 2.0f.
// INMP441 já tem ganho interno elevado. Com 5x, o piso de ruído eletrônico
// era amplificado para ~0.010-0.020 RMS — acima do limiar de silêncio (0.015),
// fazendo silêncio real ser enviado à IA como "som detectável".
#define GAIN_MULTIPLIER 2.0f

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
static int16_t*       _clipPcm = nullptr; // Captura remota (6s PCM16)
static uint8_t*       _clipWav = nullptr; // WAV pronto para upload HTTP
static volatile bool  _clipRequested = false;
static volatile bool  _clipInProgress = false;
static volatile bool  _clipBusy = false; // true enquanto envio remoto está em andamento
static volatile size_t _clipPos = 0;
static float          _clipTriggerConf = 0.0f;

static void _writeLE16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void _writeLE32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void _fillWavHeader(uint8_t* hdr, uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample, uint32_t dataBytes) {
    memcpy(hdr + 0, "RIFF", 4);
    _writeLE32(hdr + 4, 36 + dataBytes);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    _writeLE32(hdr + 16, 16);           // PCM chunk size
    _writeLE16(hdr + 20, 1);            // PCM format
    _writeLE16(hdr + 22, channels);
    _writeLE32(hdr + 24, sampleRate);
    _writeLE32(hdr + 28, sampleRate * channels * (bitsPerSample / 8));
    _writeLE16(hdr + 32, channels * (bitsPerSample / 8));
    _writeLE16(hdr + 34, bitsPerSample);
    memcpy(hdr + 36, "data", 4);
    _writeLE32(hdr + 40, dataBytes);
}

static void _finalizarClip() {
    _clipInProgress = false;
    _clipPos = 0;
    if (!_clipPcm || !_clipWav || !qAudioUpload) {
        _clipBusy = false;
        strlcpy(gWebState.pipeline, "error", sizeof(gWebState.pipeline));
        gWebState.remote_busy = false;
        gWebState.remote_err++;
        return;
    }

    _fillWavHeader(_clipWav, SAMPLE_RATE, 1, 16, REMOTE_CLIP_PCM_BYTES);
    memcpy(_clipWav + 44, _clipPcm, REMOTE_CLIP_PCM_BYTES);

    AudioClipMsg msg = {};
    msg.wavData = _clipWav;
    msg.wavLen = REMOTE_CLIP_WAV_BYTES;
    msg.triggerConf = _clipTriggerConf;
    msg.ts_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    if (xQueueSend(qAudioUpload, &msg, 0) != pdTRUE) {
        _clipBusy = false;
        strlcpy(gWebState.pipeline, "error", sizeof(gWebState.pipeline));
        gWebState.remote_busy = false;
        gWebState.remote_err++;
        LogManager::warning("[TaskAudio] Fila qAudioUpload cheia. Clip descartado.");
        return;
    }
    strlcpy(gWebState.pipeline, "uploading", sizeof(gWebState.pipeline));
}

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
    _clipPcm = (int16_t*)ps_malloc(REMOTE_CLIP_PCM_BYTES);
    _clipWav = (uint8_t*)ps_malloc(REMOTE_CLIP_WAV_BYTES);
    if (!_clipPcm || !_clipWav) {
        LogManager::error("[AUDIO] Falha ao alocar buffers de captura remota.");
    }
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

// --- Ruído Contínuo Suave (Chuva / Pink Noise) ---
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
            
            // Paul Kellet's Pink Noise Filter (muito estável e soa perfeito como chuva)
            b0 = 0.99886f * b0 + white * 0.0555179f;
            b1 = 0.99332f * b1 + white * 0.0750759f;
            b2 = 0.96900f * b2 + white * 0.1538520f;
            b3 = 0.86650f * b3 + white * 0.3104856f;
            b4 = 0.55000f * b4 + white * 0.5329522f;
            b5 = -0.7616f * b5 - white * 0.0168980f;
            float pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362f;
            b6 = white * 0.115926f;
            
            // Limitar para evitar distorção física
            if (pink >  4.0f) pink =  4.0f;
            if (pink < -4.0f) pink = -4.0f;
            
            // Fator de ganho para o alto-falante ficar alto o suficiente (aprox 6000)
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

    int duracao = (int)pv;
    if (duracao <= 0) duracao = 60; // default 1 minuto

    _tocarChuva(duracao);

    _ampOff();
    _tocando    = false;
    _taskHandle = nullptr;
    vTaskDelete(nullptr);
}

// --- API Pública ---
void setVolume(uint8_t vol) { _volume = min(vol, (uint8_t)100); }

void iniciar(int duracao_seg = 60) {
    if (_tocando) return;
    _parar = false;
    xTaskCreatePinnedToCore(
        _taskAudio, "TaskAudio_Spk",
        8192, (void*)duracao_seg, 2, &_taskHandle, 0
    );
}

void parar() {
    _parar = true;
    // Aguarda task terminar (máx 3s)
    for (int i = 0; i < 30 && _tocando; i++) vTaskDelay(pdMS_TO_TICKS(100));
}

bool tocando() { return _tocando; }

bool solicitarClipRemoto(float triggerConf = 0.0f) {
    if (_clipBusy || _clipInProgress || _clipRequested) return false;
    if (!_clipPcm || !_clipWav) return false;
    _clipTriggerConf = triggerConf;
    _clipRequested = true;
    gWebState.remote_busy = true;
    strlcpy(gWebState.pipeline, "recording", sizeof(gWebState.pipeline));
    return true;
}

bool clipRemotoOcupado() { return _clipBusy || _clipInProgress || _clipRequested; }

void liberarClipRemoto() {
    _clipBusy = false;
    if (!_clipInProgress && !_clipRequested) {
        gWebState.remote_busy = false;
    }
}

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

extern bool gOtaInProgress;

// --- Task de Leitura e Gate do Microfone ---
static void TaskAudio(void* pv) {
    LogManager::info("[TaskAudio] Iniciada no Core 1");
    const int CHUNK = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE / 4; 
    float dc_offset = 0.0f; // Filtro passa-alta estimador de Media DC
    while (true) {
        if (gOtaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint64_t t0 = esp_timer_get_time();
        if (!_rawBuf || !inference_buffer) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int samplesPerChunk = CHUNK; 
        int totalLidos = 0;
        
        for (int chunk = 0; chunk < 4; chunk++) {
            size_t bytesRead = 0;
            i2s_read(MIC_PORT, _rawBuf, samplesPerChunk * sizeof(int32_t),
                     &bytesRead, pdMS_TO_TICKS(2000));
            int n = bytesRead / sizeof(int32_t);
            int off = chunk * samplesPerChunk;
            
            for (int i = 0; i < n; i++) {
                // Shift para converter 32-bit I2S (onde os dados relevantes estão nos bits mais significativos)
                // Para MAX9814/INMP441, o valor de 32-bit tem 24 bits úteis.
                float raw_s = ((float)(_rawBuf[i] >> 8) / 8388608.0f) * GAIN_MULTIPLIER; 
                
                // Filtro passa-alta simples para zerar o DC Offset em tempo real
                dc_offset = 0.999f * dc_offset + 0.001f * raw_s; 
                float s = raw_s - dc_offset;
                
                if (s >  1.0f) s =  1.0f;
                if (s < -1.0f) s = -1.0f;
                inference_buffer[off + i] = s;

                if (_clipRequested && !_clipInProgress && !_clipBusy) {
                    _clipRequested = false;
                    _clipInProgress = true;
                    _clipBusy = true;
                    _clipPos = 0;
                    strlcpy(gWebState.pipeline, "recording", sizeof(gWebState.pipeline));
                }
                if (_clipInProgress && _clipPos < REMOTE_CLIP_SAMPLES) {
                    int32_t pcm = (int32_t)(s * 32767.0f);
                    _clipPcm[_clipPos++] = (int16_t)constrain(pcm, -32768, 32767);
                    if (_clipPos >= REMOTE_CLIP_SAMPLES) {
                        _finalizarClip();
                    }
                }
            }
            if (n < samplesPerChunk) {
                memset(&inference_buffer[off + n], 0, (samplesPerChunk - n) * sizeof(float));
                int missing = samplesPerChunk - n;
                for (int k = 0; _clipInProgress && _clipPos < REMOTE_CLIP_SAMPLES && k < missing; k++) {
                    _clipPcm[_clipPos++] = 0;
                    if (_clipPos >= REMOTE_CLIP_SAMPLES) {
                        _finalizarClip();
                        break;
                    }
                }
            }
            totalLidos += n;
        }

        // --- Calcula Energia Real (RMS já com o sinal centrado em zero) ---
        float sumSq = 0.0f;
        for (int i = 0; i < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; i++) {
            float val = inference_buffer[i]; 
            sumSq += val * val;
            // inference_buffer[i] agora não possui DC Offset, o sinal acústico permanece preservado
        }
        float rms = sqrt(sumSq / EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);

        // Limiar RMS adaptativo para não "matar" choro distante.
        // Mantemos um teto conservador e deixamos uma faixa limítrofe
        // passar para IA, evitando ficar preso em SILENCIO contínuo.
        float cfgThr = ConfigManager::get().rms_threshold;
        if (cfgThr < 0.004f) cfgThr = 0.004f;
        // Removida a limitação abusiva de 0.012f. Permitindo o ajuste escalar até 0.060.
        if (cfgThr > 0.060f) cfgThr = 0.060f;

        const int amostrasMinimasRequeridas = (EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE * 3) / 4;
        bool frameIncompleto = (totalLidos < amostrasMinimasRequeridas);

        static float noiseFloor = 0.006f;
        if (!_tocando && !frameIncompleto && rms < cfgThr) {
            noiseFloor = (noiseFloor * 0.98f) + (rms * 0.02f);
        }
        float adaptiveThr = noiseFloor + 0.0018f;
        if (adaptiveThr < 0.005f) adaptiveThr = 0.005f;
        if (adaptiveThr > cfgThr) adaptiveThr = cfgThr;

        const float RMS_THRESHOLD = (cfgThr < adaptiveThr) ? cfgThr : adaptiveThr;

        // BUGFIX: Separa DOIS cenários distintos de "silêncio":
        //
        // 1. Speaker tocando (_tocando=true): NÃO injetar nada na fila.
        //    TaskDecisao fará timeout natural de 2s. Isso impede que o contador
        //    ciclosSilencio avance enquanto o bebê ainda pode estar chorando sob
        //    o áudio — evitando o reset prematuro da crise após 60s de áudio.
        //
        // 2. Ambiente silencioso real (rms < threshold, speaker desligado):
        //    Injetar SILENCIO com isSilencioReal=true — alimenta o reset normal.
        //
        // 3. Frame incompleto (I2S timeout): também injeta silêncio.
        bool silencioReal    = (rms < RMS_THRESHOLD) && !_tocando;
        bool lowLevelCandidate =
            !_tocando && !frameIncompleto &&
            (rms >= (RMS_THRESHOLD * 0.75f)) && (rms < RMS_THRESHOLD);
        bool speakerAtivo    = _tocando;

        if (speakerAtivo) {
            // Speaker ligado: NÃO enviar nada. TaskDecisao faz timeout e não
            // avança ciclosSilencio. Mantém a crise ativa até o ambiente
            // realmente ficar quieto DEPOIS que o áudio parar.
            static uint8_t _logCnt = 0;
            if (++_logCnt >= 10) { // Log de 10 em 10 ciclos p/ não poluir
                _logCnt = 0;
                Serial.printf("[TaskAudio] Speaker ativo (RMS=%.3f) - mic gateado\n", rms);
            }
        } else if ((silencioReal && !lowLevelCandidate) || frameIncompleto) {
            // Silêncio real do ambiente (ou frame corrompido): injeta noise.
            InferenceResult ir = {};
            strlcpy(ir.label, LABEL_NOISE, sizeof(ir.label));
            ir.confianca      = 1.0f;
            ir.isSilencioReal = silencioReal; // true apenas p/ silêncio real
            ir.infMs          = 0.0f;
            xQueueSend(qResultadoIA, &ir, 0);

            float execMs = (float)(esp_timer_get_time() - t0) / 1000.0f;
            Serial.printf("[TaskAudio] RMS=%.4f (limiar=%.3f) SILENCIO%s (%.1fms)\n",
                          rms, RMS_THRESHOLD,
                          frameIncompleto ? " [FRAME-INC]" : "",
                          execMs);
        } else {
            // Áudio com energia suficiente e frame completo: acorda TaskIA
            xSemaphoreGive(semAudioPronto);

            float execMs = (float)(esp_timer_get_time() - t0) / 1000.0f;
            Serial.printf("[TaskAudio] RMS=%.4f (limiar=%.3f)%s SOM (%.1fms)\n",
                          rms, RMS_THRESHOLD,
                          lowLevelCandidate ? " [LIMIAR]" : "",
                          execMs);
        }
        
    }
}

// --- Funções de API Mestre de Áudio ---
static void beginTasks(TaskHandle_t* pTaskAudio = nullptr, TaskHandle_t* pTaskPlayer = nullptr) {
    setupHardware();
    
    // Spawn Task de Microfone (Prio 5)
    xTaskCreatePinnedToCore(TaskAudio, "TaskMic", 8192, nullptr, 5, &_taskMicHandle, 1);
    
    // Se handle de áudio foi fornecido, guarda referência
    if (pTaskAudio) {
        *pTaskAudio = _taskMicHandle;
    }
    
    // Nota: TaskPlayer está integrada e não é suspensível
    if (pTaskPlayer) {
        *pTaskPlayer = nullptr; // Não há handle separado disponível
    }
}

} // namespace AudioPlayer
