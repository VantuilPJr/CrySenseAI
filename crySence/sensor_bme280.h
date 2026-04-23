#pragma once
#include <Wire.h>
#include <Adafruit_BME280.h>

// --- Pinos I2C (compatível com SSD1306 no mesmo barramento) ---
#define BME280_SDA_PIN  8
#define BME280_SCL_PIN  9
// Endereço I2C: SDO→GND = 0x76 | SDO→VCC = 0x77
#define BME280_I2C_ADDR 0x76

namespace Sensores {

struct DadosSensor {
    float temperatura;   // °C
    float umidade;       // %
    float pressao;       // hPa
    bool  valido;        // leitura bem-sucedida
    uint32_t timestamp;  // ms desde boot
};

static Adafruit_BME280  _bme;
static DadosSensor      _dados    = {0, 0, 0, false, 0};
static SemaphoreHandle_t _mutex   = nullptr;
static bool              _started = false;

// --- Inicializa ---
bool begin() {
    Wire.begin(BME280_SDA_PIN, BME280_SCL_PIN);
    _mutex = xSemaphoreCreateMutex();
    
    if (!_bme.begin(BME280_I2C_ADDR)) {
        // Tenta endereço alternativo
        if (!_bme.begin(0x77)) {
            Serial.println("[BME280] Sensor não encontrado! Verifique a fiação.");
            return false;
        }
    }
    // Modo de amostragem otimizado para monitoramento de temperatura ambiente
    _bme.setSampling(
        Adafruit_BME280::MODE_NORMAL,
        Adafruit_BME280::SAMPLING_X2,   // temperatura
        Adafruit_BME280::SAMPLING_X16,  // pressão
        Adafruit_BME280::SAMPLING_X1,   // umidade
        Adafruit_BME280::FILTER_X16,
        Adafruit_BME280::STANDBY_MS_500
    );
    _started = true;
    Serial.println("[BME280] Sensor inicializado com sucesso.");
    return true;
}

// --- Lê sensor (chamar da task periódica) ---
void atualizar() {
    if (!_started) return;
    DadosSensor novo;
    novo.temperatura = _bme.readTemperature();
    novo.umidade     = _bme.readHumidity();
    novo.pressao     = _bme.readPressure() / 100.0f;
    novo.valido      = !isnan(novo.temperatura) && !isnan(novo.umidade);
    novo.timestamp   = millis();

    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        _dados = novo;
        xSemaphoreGive(_mutex);
    }
}

// --- Leitura thread-safe ---
DadosSensor ler() {
    DadosSensor copia;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        copia = _dados;
        xSemaphoreGive(_mutex);
    }
    return copia;
}

// --- Avalia conforto térmico ---
// Retorna +1 se ambiente desconfortável (muito quente/frio/úmido), 0 se ok
bool ambienteDesconfortavel(float tempMax = 28.0f, float tempMin = 18.0f, float umidMax = 75.0f) {
    DadosSensor d = ler();
    if (!d.valido) return false;
    return (d.temperatura > tempMax || d.temperatura < tempMin || d.umidade > umidMax);
}

} // namespace Sensores
