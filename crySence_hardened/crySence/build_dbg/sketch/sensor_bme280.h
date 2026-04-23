#line 1 "C:\\Arduino\\crySense_ai\\crySence_hardened\\crySence\\sensor_bme280.h"
// =============================================================================
// =============================================================================
// =============================================================================
#pragma once
#include <Wire.h>
#include <Adafruit_BME280.h>

#define BME280_SDA_PIN  8
#define BME280_SCL_PIN  9
#define BME280_I2C_ADDR 0x76

namespace Sensores {

struct DadosSensor {
    float temperatura;
    float umidade;
    float pressao;
    bool  valido;
    uint32_t timestamp;
};

static Adafruit_BME280  _bme;
static DadosSensor      _dados    = {0, 0, 0, false, 0};
static SemaphoreHandle_t _mutex   = nullptr;
static bool              _started = false;

bool begin() {
    Wire.begin(BME280_SDA_PIN, BME280_SCL_PIN);
    _mutex = xSemaphoreCreateMutex();
    
    if (!_bme.begin(BME280_I2C_ADDR)) {
        if (!_bme.begin(0x77)) {
            Serial.println("[BME280] Sensor não encontrado! Verifique a fiação.");
            return false;
        }
    }
    _bme.setSampling(
        Adafruit_BME280::MODE_NORMAL,
        Adafruit_BME280::SAMPLING_X2,
        Adafruit_BME280::SAMPLING_X16,
        Adafruit_BME280::SAMPLING_X1,
        Adafruit_BME280::FILTER_X16,
        Adafruit_BME280::STANDBY_MS_500
    );
    _started = true;
    Serial.println("[BME280] Sensor inicializado com sucesso.");
    return true;
}

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

DadosSensor ler() {
    DadosSensor copia = _dados;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        copia = _dados;
        xSemaphoreGive(_mutex);
    }
    return copia;
}

bool ambienteDesconfortavel(float tempMax = 28.0f, float tempMin = 18.0f, float umidMax = 75.0f) {
    DadosSensor d = ler();
    if (!d.valido) return false;
    return (d.temperatura > tempMax || d.temperatura < tempMin || d.umidade > umidMax);
}

}
