#pragma once
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "audio_player.h"
#include "config_manager.h"
#include "log_manager.h"

namespace RemoteClassifier {

static uint32_t _ok = 0;
static uint32_t _err = 0;
// BUGFIX #2: _txChunk movido para variável local em _postClipHttpRaw.

struct HttpEndpoint {
    char host[96];
    uint16_t port;
    char path[128];
};

static void _normalizeLabel(char* dst, size_t dstLen, const char* src) {
    if (!dst || dstLen == 0) return;
    dst[0] = '\0';
    if (!src) return;
    strlcpy(dst, src, dstLen);
    for (size_t i = 0; dst[i] != '\0'; i++) {
        if (dst[i] >= 'A' && dst[i] <= 'Z') dst[i] = (char)(dst[i] - 'A' + 'a');
    }
    if (strcmp(dst, "colica") == 0) strlcpy(dst, "colic", dstLen);
    else if (strcmp(dst, "fome") == 0) strlcpy(dst, "hunger", dstLen);

    if (strcmp(dst, "colic") != 0 && strcmp(dst, "hunger") != 0) {
        dst[0] = '\0';
    }
}

static bool _parseResponse(const String& payload, RemoteClassResult& rr) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err != DeserializationError::Ok) {
        LogManager::warningf("[Remote] JSON Invalido (%s). Payload bruto: '%s'", err.c_str(), payload.c_str());
        strlcpy(rr.erro, "json_parse_error", sizeof(rr.erro));
        return false;
    }

    // BUGFIX #3: default true (pois alguns servidores podem não enviar o campo 'ok').
    bool ok = doc["ok"] | true;
    if (!ok) {
        const char* msg = doc["error"] | "server_error";
        strlcpy(rr.erro, msg, sizeof(rr.erro));
        return false;
    }

    char normalized[16] = "";
    _normalizeLabel(normalized, sizeof(normalized), doc["label"] | "");
    if (normalized[0] == '\0') {
        strlcpy(rr.erro, "invalid_label", sizeof(rr.erro));
        return false;
    }

    strlcpy(rr.label, normalized, sizeof(rr.label));
    rr.confianca = doc["confianca"] | 0.0f;
    rr.scores[0] = doc["scores"]["colic"] | 0.0f;
    rr.scores[1] = doc["scores"]["hunger"] | 0.0f;
    rr.scores[2] = 0.0f; // MVP: classe noise removida do classificador remoto
    rr.scores[3] = 0.0f; // MVP: classe sleep removida do classificador remoto
    rr.ok = true;
    rr.erro[0] = '\0';
    return true;
}

static bool _parseHttpEndpoint(const char* url, HttpEndpoint& ep, RemoteClassResult& rr) {
    if (!url || strncmp(url, "http://", 7) != 0) {
        strlcpy(rr.erro, "unsupported_scheme", sizeof(rr.erro));
        return false;
    }

    const char* p = url + 7;
    if (*p == '\0') {
        strlcpy(rr.erro, "invalid_url", sizeof(rr.erro));
        return false;
    }

    const char* slash = strchr(p, '/');
    const char* hostEnd = slash ? slash : (p + strlen(p));
    const char* colon = nullptr;
    for (const char* it = p; it < hostEnd; it++) {
        if (*it == ':') {
            colon = it;
            break;
        }
    }

    const char* hostStop = colon ? colon : hostEnd;
    size_t hostLen = (size_t)(hostStop - p);
    if (hostLen == 0 || hostLen >= sizeof(ep.host)) {
        strlcpy(rr.erro, "invalid_host", sizeof(rr.erro));
        return false;
    }

    memcpy(ep.host, p, hostLen);
    ep.host[hostLen] = '\0';
    ep.port = 80;

    if (colon) {
        uint32_t port = 0;
        const char* portStart = colon + 1;
        if (portStart >= hostEnd) {
            strlcpy(rr.erro, "invalid_port", sizeof(rr.erro));
            return false;
        }
        for (const char* it = portStart; it < hostEnd; it++) {
            if (*it < '0' || *it > '9') {
                strlcpy(rr.erro, "invalid_port", sizeof(rr.erro));
                return false;
            }
            port = (port * 10U) + (uint32_t)(*it - '0');
            if (port > 65535U) {
                strlcpy(rr.erro, "invalid_port", sizeof(rr.erro));
                return false;
            }
        }
        if (port == 0) {
            strlcpy(rr.erro, "invalid_port", sizeof(rr.erro));
            return false;
        }
        ep.port = (uint16_t)port;
    }

    if (slash && *slash) strlcpy(ep.path, slash, sizeof(ep.path));
    else strlcpy(ep.path, "/", sizeof(ep.path));

    return true;
}

static bool _postClipHttpRaw(const CryConfig& cfg, const AudioClipMsg& clip, RemoteClassResult& rr) {
    HttpEndpoint ep = {};
    if (!_parseHttpEndpoint(cfg.classifier_url, ep, rr)) return false;

    WiFiClient client;
    // BUGFIX #4: desabilita algoritmo de Nagle → elimina delay de até 200ms
    // nos pequenoswrite de header antes do body grande do WAV.
    client.setNoDelay(true);
    client.setTimeout((int)cfg.classifier_timeout_ms);

    if (!client.connect(ep.host, ep.port, cfg.classifier_timeout_ms)) {
        strlcpy(rr.erro, "connect_fail", sizeof(rr.erro));
        return false;
    }

    char trigConf[16];
    snprintf(trigConf, sizeof(trigConf), "%.3f", clip.triggerConf);

    char header[448];
    int hdrLen = snprintf(
        header,
        sizeof(header),
        "POST %s HTTP/1.0\r\n"
        "Host: %s:%u\r\n"
        "User-Agent: CrySense/2.0\r\n"
        "Connection: close\r\n"
        "Content-Type: audio/wav\r\n"
        "X-Trigger-Confidence: %s\r\n"
        "Content-Length: %u\r\n\r\n",
        ep.path,
        ep.host,
        (unsigned int)ep.port,
        trigConf,
        (unsigned int)clip.wavLen
    );
    if (hdrLen <= 0 || hdrLen >= (int)sizeof(header)) {
        strlcpy(rr.erro, "header_build_fail", sizeof(rr.erro));
        client.stop();
        return false;
    }

    if (client.write((const uint8_t*)header, (size_t)hdrLen) != (size_t)hdrLen) {
        strlcpy(rr.erro, "header_write_fail", sizeof(rr.erro));
        client.stop();
        return false;
    }

    // BUGFIX #2: _txChunk agora é variável local (não mais static no namespace).
    // BUGFIX #5: vTaskDelay removido — WiFiClient bloqueia naturalmente quando o
    //            TX buffer TCP enche; o delay manual apenas adicionava ~6ms extras.
    static constexpr size_t TX_CHUNK_SZ = 1024;
    uint8_t txChunk[TX_CHUNK_SZ];
    size_t sent = 0;
    while (sent < clip.wavLen) {
        size_t n = clip.wavLen - sent;
        if (n > TX_CHUNK_SZ) n = TX_CHUNK_SZ;
        memcpy(txChunk, clip.wavData + sent, n);
        size_t w = client.write(txChunk, n);
        if (w == 0) {
            snprintf(rr.erro, sizeof(rr.erro), "body_write_%u", (unsigned int)sent);
            client.stop();
            return false;
        }
        sent += w;
    }

    client.flush();

    uint32_t waitStart = millis();
    while ((client.connected() || client.available() > 0) && client.available() == 0 && (millis() - waitStart) < cfg.classifier_timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    String statusLine = client.readStringUntil('\n');
    statusLine.trim();
    int code = -1;
    int sp1 = statusLine.indexOf(' ');
    if (sp1 > 0) {
        int sp2 = statusLine.indexOf(' ', sp1 + 1);
        String codeStr = (sp2 > sp1) ? statusLine.substring(sp1 + 1, sp2)
                                     : statusLine.substring(sp1 + 1);
        code = codeStr.toInt();
    }
    if (code <= 0) {
        strlcpy(rr.erro, "http_status_invalid", sizeof(rr.erro));
        client.stop();
        return false;
    }

    uint32_t hdrStart = millis();
    while ((client.connected() || client.available() > 0) && (millis() - hdrStart) < cfg.classifier_timeout_ms) {
        if (client.available() > 0) {
            String line = client.readStringUntil('\n');
            if (line == "\r" || line == "\r\n" || line.length() == 0) {
                break; // Fim dos headers
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    // BUGFIX #1: Leitura em bloco de 512B → elimina dezenas de realloc() causados
    //             pela concatenação byte-a-byte que era o principal gargalo de latência.
    String payload;
    payload.reserve(512);
    uint8_t rxBuf[512];
    uint32_t lastRx = millis();
    while ((millis() - lastRx) < cfg.classifier_timeout_ms) {
        int avail = client.available();
        if (avail > 0) {
            int toRead = avail < (int)sizeof(rxBuf) ? avail : (int)sizeof(rxBuf);
            int got = client.read(rxBuf, toRead);
            if (got > 0) {
                for (int i = 0; i < got; i++) {
                    payload += (char)rxBuf[i];
                }
                lastRx = millis();
            }
        } else {
            if (!client.connected()) break;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    client.stop();

    if (code != 200 && code != 201) {
        snprintf(rr.erro, sizeof(rr.erro), "http_%d", code);
        return false;
    }
    if (payload.length() == 0) {
        strlcpy(rr.erro, "empty_payload", sizeof(rr.erro));
        return false;
    }
    return _parseResponse(payload, rr);
}

static bool _postClip(const CryConfig& cfg, const AudioClipMsg& clip, RemoteClassResult& rr) {
    rr = {};
    rr.ok = false;
    rr.latency_ms = 0;
    if (cfg.classifier_url[0] == '\0') {
        strlcpy(rr.erro, "classifier_url_empty", sizeof(rr.erro));
        return false;
    }
    if (!clip.wavData || clip.wavLen < 44 || clip.wavLen > REMOTE_CLIP_WAV_BYTES) {
        strlcpy(rr.erro, "clip_invalid", sizeof(rr.erro));
        return false;
    }

    const uint32_t t0 = millis();
    bool ok = false;
    const bool isHttps = (strncmp(cfg.classifier_url, "https://", 8) == 0);
    if (!isHttps) {
        ok = _postClipHttpRaw(cfg, clip, rr);
    } else {
        HTTPClient http;
        WiFiClientSecure client;
        client.setInsecure();
        int code = -1;
        String payload;
        if (!http.begin(client, cfg.classifier_url)) {
            strlcpy(rr.erro, "http_begin_fail", sizeof(rr.erro));
            rr.latency_ms = millis() - t0;
            return false;
        }
        http.addHeader("Content-Type", "audio/wav");
        http.addHeader("X-Trigger-Confidence", String(clip.triggerConf, 3));
        http.setTimeout((int)cfg.classifier_timeout_ms);
        code = http.POST(clip.wavData, clip.wavLen);
        if (code > 0) payload = http.getString();
        http.end();
        if (code <= 0) {
            snprintf(rr.erro, sizeof(rr.erro), "http_%d", code);
            rr.latency_ms = millis() - t0;
            return false;
        }
        if (code != 200 && code != 201) {
            snprintf(rr.erro, sizeof(rr.erro), "http_%d", code);
            rr.latency_ms = millis() - t0;
            return false;
        }
        ok = _parseResponse(payload, rr);
    }
    rr.latency_ms = millis() - t0;
    return ok;
}

static void TaskRemoteClassifier(void* pv) {
    LogManager::info("[TaskRemoteClassifier] Iniciada no Core 0");
    AudioClipMsg clip;
    while (true) {
        if (xQueueReceive(qAudioUpload, &clip, portMAX_DELAY) != pdTRUE) continue;
        if (!clip.wavData || clip.wavLen < 44 || clip.wavLen > REMOTE_CLIP_WAV_BYTES) {
            _err++;
            gWebState.remote_err = _err;
            strlcpy(gWebState.pipeline, "error", sizeof(gWebState.pipeline));
            LogManager::warningf("[Remote] Clip invalido: wav=%uB", (unsigned int)clip.wavLen);
            AudioPlayer::liberarClipRemoto();
            gWebState.remote_busy = false;
            continue;
        }

        gWebState.remote_busy = true;
        strlcpy(gWebState.pipeline, "waiting_result", sizeof(gWebState.pipeline));

        RemoteClassResult rr = {};
        CryConfig& cfg = ConfigManager::get();
        bool ok = _postClip(cfg, clip, rr);
        if (!ok) {
            _err++;
            gWebState.remote_err = _err;
            gWebState.remote_latency_ms = rr.latency_ms;
            strlcpy(gWebState.pipeline, "error", sizeof(gWebState.pipeline));
            rr.ok = false;
            LogManager::warningf("[Remote] Falha classificacao: %s (url=%s, wav=%uB, trig=%.2f, tmo=%lums)",
                                 rr.erro, cfg.classifier_url, (unsigned int)clip.wavLen,
                                 clip.triggerConf, (unsigned long)cfg.classifier_timeout_ms);
        } else {
            _ok++;
            gWebState.remote_ok = _ok;
            gWebState.remote_latency_ms = rr.latency_ms;
            strlcpy(gWebState.result_label, rr.label, sizeof(gWebState.result_label));
            gWebState.result_conf = rr.confianca;
            strlcpy(gWebState.pipeline, "result", sizeof(gWebState.pipeline));
            LogManager::infof("[Remote] Resultado %s (%.0f%%, %lums)",
                              rr.label, rr.confianca * 100.0f, rr.latency_ms);
        }

        xQueueSend(qRemoteResult, &rr, 0);
        AudioPlayer::liberarClipRemoto();
        gWebState.remote_busy = false;
        UBaseType_t stackLeft = uxTaskGetStackHighWaterMark(nullptr);
        if (stackLeft < 512) {
            LogManager::warningf("[Remote] Stack baixo: %lu words", (unsigned long)stackLeft);
        }
    }
}

static void beginTask(TaskHandle_t* pTaskRemote = nullptr) {
    TaskHandle_t hTask = nullptr;
    xTaskCreatePinnedToCore(TaskRemoteClassifier, "TaskRemoteCls", 16384, nullptr, 2, &hTask, 0);
    if (pTaskRemote) {
        *pTaskRemote = hTask;
    }
}

} // namespace RemoteClassifier
