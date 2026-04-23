// =============================================================================
// =============================================================================
#pragma once
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "log_manager.h"
#include "sensor_bme280.h"
#include "firebase_manager.h"
#include "config_manager.h"
#include "heap_debug.h"

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_ADDR 0x3C
#define OLED_RESET -1
#define VERBOSE_HMI_TIMING 0

namespace Display {

static Adafruit_SSD1306 _oled(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
static bool _started = false;

enum Tela {
  TELA_BOOT,
  TELA_NORMAL,
  TELA_ALERTA_COLICA,
  TELA_ALERTA_FOME,
  TELA_ALERTA_SONO,
  TELA_WIFI_CONECTANDO,
  TELA_WIFI_OK,
  TELA_ERRO
};

bool begin() {
  if (!_oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[OLED] Falha ao inicializar! Verifique endereço I2C.");
    return false;
  }
  _oled.clearDisplay();
  _oled.setTextColor(SSD1306_WHITE);
  _oled.cp437(true);
  _started = true;
  Serial.println("[OLED] Display inicializado.");
  return true;
}

static void _drawBar(int x, int y, int w, int h, float pct) {
  _oled.drawRect(x, y, w, h, SSD1306_WHITE);
  int fill = (int)(pct * (w - 2));
  if (fill > 0)
    _oled.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);
}

void mostrarBoot() {
  if (!_started)
    return;
  _oled.clearDisplay();
  _oled.setTextSize(1);
  _oled.setCursor(20, 5);
  _oled.print("CrySense AI");
  _oled.setCursor(36, 18);
  _oled.print("v2.0");
  _oled.setCursor(10, 35);
  _oled.print("Inicializando...");
  _oled.drawLine(0, 30, 127, 30, SSD1306_WHITE);
  _oled.display();
}

void mostrarNormal(float temp, float umid, const char *estado,
                   uint32_t uptime_s) {
  if (!_started)
    return;
  _oled.clearDisplay();
  _oled.fillRect(0, 0, 128, 12, SSD1306_WHITE);
  _oled.setTextColor(SSD1306_BLACK);
  _oled.setTextSize(1);
  _oled.setCursor(15, 2);
  _oled.print("CrySense AI v2.0");
  _oled.setTextColor(SSD1306_WHITE);
  _oled.setTextSize(1);
  _oled.setCursor(0, 16);
  _oled.printf("Temp: %.1f C", temp);
  _oled.setCursor(0, 27);
  _oled.printf("Umid: %.0f %%", umid);
  // Estado da IA
  _oled.drawLine(0, 38, 127, 38, SSD1306_WHITE);
  _oled.setCursor(0, 42);
  _oled.print("IA: ");
  _oled.print(estado);
  _oled.setCursor(0, 54);
  uint32_t h = uptime_s / 3600, m = (uptime_s % 3600) / 60, s = uptime_s % 60;
  _oled.printf("Up: %02lu:%02lu:%02lu", h, m, s);
  _oled.display();
}

void mostrarAlerta(const char *titulo, const char *subtitulo, float confianca,
                   bool audioAtivo) {
  if (!_started)
    return;
  _oled.clearDisplay();
  _oled.drawRect(0, 0, 128, 64, SSD1306_WHITE);
  _oled.drawRect(2, 2, 124, 60, SSD1306_WHITE);
  _oled.setTextSize(2);
  _oled.setCursor(5, 8);
  _oled.print(titulo);
  _oled.setTextSize(1);
  _oled.setCursor(5, 30);
  _oled.print(subtitulo);
  _oled.setCursor(5, 42);
  _oled.printf("Conf: %.0f%%", confianca * 100.0f);
  _drawBar(5, 52, 90, 8, confianca);
  if (audioAtivo) {
    _oled.setCursor(98, 52);
    _oled.print("[SOM]");
  }
  _oled.display();
}

void mostrarWiFi(bool conectando, const char *ssid, const char *ip = "") {
  if (!_started)
    return;
  _oled.clearDisplay();
  _oled.setTextSize(1);
  _oled.setCursor(0, 0);
  _oled.print(conectando ? "Conectando WiFi..." : "WiFi Conectado!");
  _oled.setCursor(0, 16);
  _oled.print("SSID: ");
  _oled.print(ssid);
  if (!conectando && strlen(ip) > 0) {
    _oled.setCursor(0, 30);
    _oled.print("IP: ");
    _oled.print(ip);
    _oled.setCursor(0, 44);
    _oled.print("Acesse o navegador");
    _oled.setCursor(0, 54);
    _oled.print("para configurar");
  }
  _oled.display();
}

void mostrarErro(const char *msg) {
  if (!_started)
    return;
  _oled.clearDisplay();
  _oled.setTextSize(1);
  _oled.setCursor(0, 0);
  _oled.print("!!! ERRO !!!");
  _oled.setCursor(0, 16);
  _oled.print(msg);
  _oled.display();
}

void limpar() {
  if (!_started)
    return;
  _oled.clearDisplay();
  _oled.display();
}

static void TaskHMI(void *pv) {
  LogManager::info("[TaskHMI] Iniciada no Core 0");
  uint32_t tUltimoDisplay = 0;

  while (true) {
    uint64_t t0 = esp_timer_get_time();

    CryConfig &cfg = ConfigManager::get();
    vTaskDelay(pdMS_TO_TICKS(cfg.sensor_intervalo_ms));

    Sensores::atualizar();
    auto d = Sensores::ler();

    if (d.valido) {
      gWebState.temp = d.temperatura;
      gWebState.umid = d.umidade;
      gWebState.pres = d.pressao;
      gWebState.conforto_ok = !Sensores::ambienteDesconfortavel(
          cfg.temp_max_conforto, cfg.temp_min_conforto);
    }

    static uint32_t tUltimoFbSens = 0;
    if (millis() - tUltimoFbSens > 60000) {
      tUltimoFbSens = millis();
      Firebase::enviarSensores(ConfigManager::get().firebase_url,
                               ConfigManager::get().firebase_auth,
                               d.temperatura, d.umidade, d.pressao);
    }

    if (!gState.choroCritico) {
      const char *estadoStr = (strcmp(gWebState.estado, "crise") == 0) ? "ALERTA!"
              : (strcmp(gWebState.estado, "calmo") == 0) ? "Calmo"
                               : "Monitorando";
      mostrarNormal(d.valido ? d.temperatura : 0.0f,
                    d.valido ? d.umidade : 0.0f, estadoStr,
                    esp_timer_get_time() / 1000000ULL);
    }

    static uint32_t tUltimoLogSens = 0;
    if (millis() - tUltimoLogSens > 300000) {
      tUltimoLogSens = millis();
      if (d.valido) {
        LogManager::infof("BME280: %.1f°C %.0f%% %.0fhPa", d.temperatura,
                          d.umidade, d.pressao);
      }
    }

#if VERBOSE_HMI_TIMING
    float execMs = (float)(esp_timer_get_time() - t0) / 1000.0f;
    Serial.printf("[TaskHMI] Tempo de execucao: %.1fms\n", execMs);
#endif
  }
}

static void beginTask(TaskHandle_t* pTaskHMI = nullptr) {
  TaskHandle_t hTask = nullptr;
  xTaskCreatePinnedToCore(TaskHMI, "TaskHMI", 8192, nullptr, 2, &hTask, 0);
  if (pTaskHMI) {
      *pTaskHMI = hTask;
  }
}

}
