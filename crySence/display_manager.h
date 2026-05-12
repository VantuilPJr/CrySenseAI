#pragma once
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "log_manager.h"
#include "sensor_bme280.h"
#include "firebase_manager.h"
#include "config_manager.h"

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_ADDR 0x3C // Tente 0x3D se não funcionar
#define OLED_RESET -1  // Não tem pino de reset separado

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

// --- Inicializa ---
bool begin() {
  // Wire já iniciado pelo BME280 (mesmo barramento)
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

// --- Barra de progresso horizontal ---
static void _drawBar(int x, int y, int w, int h, float pct) {
  _oled.drawRect(x, y, w, h, SSD1306_WHITE);
  int fill = (int)(pct * (w - 2));
  if (fill > 0)
    _oled.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);
}

// --- Tela de boot ---
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

// --- Tela normal: temperatura + estado ---
void mostrarNormal(float temp, float umid, const char *estado,
                   uint32_t uptime_s) {
  if (!_started)
    return;
  _oled.clearDisplay();
  // Header
  _oled.fillRect(0, 0, 128, 12, SSD1306_WHITE);
  _oled.setTextColor(SSD1306_BLACK);
  _oled.setTextSize(1);
  _oled.setCursor(15, 2);
  _oled.print("CrySense AI v2.0");
  _oled.setTextColor(SSD1306_WHITE);
  // Temperatura e umidade
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
  // Uptime
  _oled.setCursor(0, 54);
  uint32_t h = uptime_s / 3600, m = (uptime_s % 3600) / 60, s = uptime_s % 60;
  _oled.printf("Up: %02lu:%02lu:%02lu", h, m, s);
  _oled.display();
}

// --- Tela de alerta com barra de confiança ---
void mostrarAlerta(const char *titulo, const char *subtitulo, float confianca,
                   bool audioAtivo) {
  if (!_started)
    return;
  _oled.clearDisplay();
  // Borda piscante de alerta
  _oled.drawRect(0, 0, 128, 64, SSD1306_WHITE);
  _oled.drawRect(2, 2, 124, 60, SSD1306_WHITE);
  // Título grande
  _oled.setTextSize(2);
  _oled.setCursor(5, 8);
  _oled.print(titulo);
  // Subtítulo
  _oled.setTextSize(1);
  _oled.setCursor(5, 30);
  _oled.print(subtitulo);
  // Barra de confiança
  _oled.setCursor(5, 42);
  _oled.printf("Conf: %.0f%%", confianca * 100.0f);
  _drawBar(5, 52, 90, 8, confianca);
  // Ícone de áudio
  if (audioAtivo) {
    _oled.setCursor(98, 52);
    _oled.print("[SOM]");
  }
  _oled.display();
}

// --- Tela de WiFi ---
void mostrarWiFi(bool conectando, const char *ssid, const char *ip = "") {
  if (!_started)
    return;
  _oled.clearDisplay();
  _oled.setTextSize(1);
  _oled.setCursor(0, 0);
  _oled.print(conectando ? "WiFi Conectando..." : "WiFi Conectado!");
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

// --- Tela de erro ---
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

// --- Limpa display ---
void limpar() {
  if (!_started)
    return;
  _oled.clearDisplay();
  _oled.display();
}

// --- Task HMI: Atualiza OLED periodicamente e manda sensores pro Firebase ---
static void TaskHMI(void *pv) {
  LogManager::info("[TaskHMI] Iniciada no Core 0");
  uint32_t tUltimoDisplay = 0;

  while (true) {
    while (gOtaInProgress) { vTaskDelay(pdMS_TO_TICKS(100)); }
    uint64_t t0 = esp_timer_get_time();
    
    CryConfig &cfg = ConfigManager::get();
    vTaskDelay(pdMS_TO_TICKS(cfg.sensor_intervalo_ms));

    // Lê BME280
    Sensores::atualizar();
    auto d = Sensores::ler();

    if (d.valido) {
      gWebState.temp = d.temperatura;
      gWebState.umid = d.umidade;
      gWebState.pres = d.pressao;
      gWebState.conforto_ok = !Sensores::ambienteDesconfortavel(
          cfg.temp_max_conforto, cfg.temp_min_conforto);
    }

    // Envia sensores ao Firebase a cada 60s
    static uint32_t tUltimoFbSens = 0;
    if (millis() - tUltimoFbSens > 60000) {
      tUltimoFbSens = millis();
      Firebase::enviarSensores(ConfigManager::get().firebase_url,
                               ConfigManager::get().firebase_auth,
                               d.temperatura, d.umidade, d.pressao);
    }

    // Atualiza OLED somente se não estiver em crise (crise atualiza no alerta)
    if (!gState.choroCritico) {
      const char *estadoStr = (strcmp(gWebState.estado, "crise") == 0) ? "ALERTA!"
              : (strcmp(gWebState.estado, "calmo") == 0) ? "Calmo"
                              : "Monitorando";
      mostrarNormal(d.valido ? d.temperatura : 0.0f,
                    d.valido ? d.umidade : 0.0f, estadoStr,
                    esp_timer_get_time() / 1000000ULL);
    }

    // Log periódico de sensores (a cada 5 minutos)
    static uint32_t tUltimoLogSens = 0;
    if (millis() - tUltimoLogSens > 300000) {
      tUltimoLogSens = millis();
      if (d.valido) {
        LogManager::infof("BME280: %.1f°C %.0f%% %.0fhPa", d.temperatura,
                          d.umidade, d.pressao);
      }
    }

    float execMs = (float)(esp_timer_get_time() - t0) / 1000.0f;
    Serial.printf("[TaskHMI] Tempo de execucao: %.1fms\n", execMs);
    
  }
}

static void beginTask(TaskHandle_t* pTaskHMI = nullptr) {
  TaskHandle_t hTask = nullptr;
  xTaskCreatePinnedToCore(TaskHMI, "TaskHMI", 6144, nullptr, 2, &hTask, 0);
  if (pTaskHMI) {
      *pTaskHMI = hTask;
  }
}

} // namespace Display
