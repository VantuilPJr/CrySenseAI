# Guia de Estudo do Código Principal (crySence.ino)

Este arquivo contém a explicação detalhada das principais partes do código C++ do ESP32, comentadas linha a linha, para que todos do grupo possam entender como o sistema funciona.

## 1. Importações e Configurações Iniciais

```cpp
// --- Alocação do modelo Edge Impulse em PSRAM ---
#define EI_CLASSIFIER_ALLOCATION_STATIC 0    // Desativa a alocação de memória estática para a Inteligência Artificial
#define EI_CLASSIFIER_ALLOCATION_PSRAM 1     // Ativa a alocação na PSRAM (memória RAM externa do ESP32, maior capacidade)
#define EI_CLASSIFIER_TFLITE_ENABLE_PSRAM 1  // Diz para o TensorFlow Lite (motor da IA) usar a PSRAM

// Estas linhas preparam o modelo de Inteligência Artificial (Edge Impulse) para rodar no ESP32
#define ei_default_impulse ei_default_impulse_trigger 
#include <CrySense_trigger_inferencing.h> // Inclui a biblioteca gerada pela Edge Impulse com o modelo de IA
#undef ei_default_impulse
#include "C:/Arduino/libraries/CrySense_trigger_inferencing/src/model-parameters/model_variables.h" // Arquivo com as variáveis do modelo

#include "driver/i2s.h"       // Biblioteca para controlar o microfone digital (protocolo I2S)
#include "esp_sleep.h"        // Biblioteca para gerenciar o modo de economia de energia (sleep) do ESP32
#include "esp_wifi.h"         // Biblioteca nativa do ESP32 para funções avançadas de Wi-Fi

#include <ArduinoJson.h>      // Biblioteca para manipular dados no formato JSON (usado para enviar/receber dados da web)
#include <ArduinoOTA.h>       // Biblioteca para atualização de código "Over-The-Air" (pelo Wi-Fi, sem cabo USB)
#include <WiFi.h>             // Biblioteca padrão do Arduino para conexões Wi-Fi
#include <DNSServer.h>        // Biblioteca para criar um servidor DNS (usado no modo Captive Portal/AP)

DNSServer dnsServer;          // Cria um objeto (instância) do servidor DNS

// --- Módulos do sistema (Arquivos próprios do projeto) ---
#include "audio_player.h"     // Controla a reprodução do ruído branco
#include "config_manager.h"   // Gerencia as configurações salvas na memória interna (NVS)
#include "display_manager.h"  // Controla o que aparece na tela OLED
#include "firebase_manager.h" // Gerencia o envio de dados para o banco de dados Firebase
#include "log_manager.h"      // Sistema de registro de mensagens (logs) para debug
#include "secrets.h"          // Arquivo com as senhas e URLs sensíveis
#include "sensor_bme280.h"    // Lê a temperatura e umidade
#include "remote_classifier.h"// Responsável por enviar o áudio para o servidor (notebook/celular) classificar

// web_server tem dependência de AudioPlayer, então vem depois
#include "web_server.h"       // Controla o painel web (dashboard) local do ESP32
```

## 2. Variáveis Globais (Estado Compartilhado)

```cpp
SistemaState gState;                     // Variável que guarda o estado atual do sistema (se está em crise, contadores, etc)
SemaphoreHandle_t gMutexState = nullptr; // Semáforo: uma "trava" para evitar que duas tarefas tentem mexer nos mesmos dados ao mesmo tempo
WebDashboardState gWebState = {};        // Guarda as informações que serão mostradas na página web (Dashboard)
AnalyticsState gAnalytics;               // Guarda o histórico de choros recentes para gerar gráficos

// Filas e Semáforos (FreeRTOS) - Usados para comunicação entre as Tasks (Tarefas) rodando em paralelo
SemaphoreHandle_t semAudioPronto = nullptr; // Semáforo que avisa a Inteligência Artificial: "Tem áudio novo gravado, pode analisar!"
QueueHandle_t qResultadoIA = nullptr;       // Fila onde a IA coloca o resultado da detecção (Choro ou Ruído)
QueueHandle_t qFirebase = nullptr;          // Fila onde colocamos mensagens para serem enviadas ao Firebase
QueueHandle_t qAudioUpload = nullptr;       // Fila que avisa: "Mande esse áudio para o servidor do notebook"
QueueHandle_t qRemoteResult = nullptr;      // Fila onde o servidor do notebook devolve o resultado (Cólica ou Fome)
static bool gOtaReady = false;              // Variável que avisa se o sistema de atualização por Wi-Fi está pronto

// "Handles" (Identificadores) para podermos controlar as Tarefas (Tasks) do FreeRTOS
static TaskHandle_t hTaskIA = nullptr;      // Identificador da Tarefa da IA local
static TaskHandle_t hTaskDecisao = nullptr; // Identificador da Tarefa que toma as decisões (se liga áudio, avisa web, etc)
static TaskHandle_t hTaskAudio = nullptr;   // Identificador da Tarefa que grava do microfone
static TaskHandle_t hTaskPlayer = nullptr;  // Identificador da Tarefa que toca música
static TaskHandle_t hTaskHMI = nullptr;     // Identificador da Tarefa da tela OLED
static TaskHandle_t hTaskIOT = nullptr;     // Identificador da Tarefa do Firebase
static TaskHandle_t hTaskRemote = nullptr;  // Identificador da Tarefa que fala com o notebook
```

## 3. Buffer de Áudio para a IA

```cpp
// Esse ponteiro vai apontar para um bloco grande de memória (64KB) na PSRAM, onde guardamos o som que o microfone ouviu
float *inference_buffer = nullptr; 

// Função "callback" (chamada automaticamente pela biblioteca da Edge Impulse) para ela ler o áudio gravado
int get_signal_data_callback(size_t offset, size_t length, float *out_ptr) {
  // Copia o áudio do nosso 'inference_buffer' para o ponteiro 'out_ptr' que a IA pediu
  memcpy(out_ptr, inference_buffer + offset, length * sizeof(float));
  return 0; // Retorna 0 indicando que deu tudo certo
}

// Função que roda o modelo de Inteligência Artificial local (que detecta se é CHORO ou RUIDO genérico)
static inline EI_IMPULSE_ERROR run_trigger_model(signal_t *signal, ei_impulse_result_t *result) {
  // Chama a função da biblioteca Edge Impulse passando o sinal de áudio e a variável para receber o resultado
  return run_classifier(&ei_default_impulse_trigger, signal, result, false);
}
```

## 4. Função de Interrupção do Botão Físico (Botão BOOT do ESP32)

```cpp
static volatile bool flagResetISR = false;      // Bandeira (flag) que avisa que o botão foi apertado
static volatile TickType_t gLastResetIsrTick = 0; // Guarda quando o botão foi apertado pela última vez (para evitar "repique" do botão)
static uint32_t gBootMs = 0;                    // Guarda o tempo em milissegundos que o ESP32 ligou

// IRAM_ATTR diz para colocar essa função na memória RAM interna super rápida, pois é uma Interrupção de Hardware
void IRAM_ATTR isrBotaoReset() {
  const TickType_t now = xTaskGetTickCountFromISR(); // Pega o tempo atual
  // Se passaram mais de 400 milissegundos desde o último aperto (debounce)
  if ((now - gLastResetIsrTick) >= pdMS_TO_TICKS(400)) {
    gLastResetIsrTick = now;  // Atualiza o último tempo
    flagResetISR = true;      // Levanta a bandeira dizendo "O botão foi apertado de verdade"
  }
}
```

## 5. Tarefa Principal: Inteligência Artificial Local (TaskIA)

```cpp
// Função que vai rodar continuamente no Core 1 do processador do ESP32
void TaskIA(void *pv) {
  LogManager::info("[TaskIA] Iniciada no Core 1"); // Registra no log
  
  while (true) { // Loop infinito da tarefa
    // A tarefa "dorme" aqui até que a Tarefa do Microfone solte este semáforo avisando "Tem áudio pronto!"
    xSemaphoreTake(semAudioPronto, portMAX_DELAY);

    uint64_t t0 = esp_timer_get_time(); // Marca o tempo inicial para sabermos quanto demorou
    CryConfig &cfg = ConfigManager::get(); // Pega as configurações atuais (como thresholds)

    // Calculamos a 'energia' do som no buffer para ver se não é só um silêncio absoluto (erro no mic)
    float bufEnergy = 0.0f;
    for (int i = 0; i < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; i++)
      bufEnergy += inference_buffer[i] * inference_buffer[i]; // Soma o quadrado da onda sonora
    float bufRMS = sqrtf(bufEnergy / EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE); // Tira a raiz média quadrática (RMS = Root Mean Square)

    // Ajusta o limite de silêncio baseado na configuração
    float emptyFloor = cfg.rms_threshold * 0.25f;
    if (emptyFloor < 0.0020f) emptyFloor = 0.0020f; // Limite mínimo de segurança
    if (emptyFloor > 0.0200f) emptyFloor = 0.0200f; // Limite máximo de segurança

    // Se o som for muito baixo (silêncio), a gente descarta, economizando CPU e evitando a IA dar um "falso positivo"
    if (bufRMS < emptyFloor) {
      InferenceResult irZero = {}; // Cria resultado vazio
      strlcpy(irZero.label, LABEL_NOISE, sizeof(irZero.label)); // Marca como "Ruído" (Noise)
      irZero.confianca = 1.0f;     // 100% de certeza que é ruído (silêncio)
      irZero.isSilencioReal = true;// Marca flag dizendo que era silêncio mesmo
      irZero.infMs = 0.0f;         // Demorou 0ms
      xQueueSend(qResultadoIA, &irZero, 0); // Envia o resultado pela fila para a Tarefa de Decisão
      continue; // Volta pro começo do 'while(true)' e espera próximo áudio
    }

    // Se passou do teste de silêncio, prepara os dados para a Inteligência Artificial
    signal_t signal;
    signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; // Tamanho dos dados
    signal.get_data = &get_signal_data_callback; // Diz pra IA qual função chamar para pegar o áudio

    ei_impulse_result_t triggerResult = {0}; // Variável que vai receber o resultado puro da IA
    // Manda a IA classificar o áudio! O código para aqui até a IA terminar de "pensar"
    EI_IMPULSE_ERROR err = run_trigger_model(&signal, &triggerResult);

    if (err != EI_IMPULSE_OK) { // Se deu erro na IA
      continue; // Ignora e volta pro começo
    }

    // ... [O restante do código avalia o 'triggerResult', vê qual classe ganhou (Choro ou Ruído), 
    // formata isso num 'InferenceResult' e manda para a 'TaskDecisao' usando 'xQueueSend']
  }
}
```

*Nota: As outras tasks (`TaskDecisao`, `setup`, `loop`) seguem a mesma lógica do FreeRTOS explicada acima: laços de repetição infinitos, leitura de dados utilizando semáforos/filas, invocação de processamento externo, e repasse do resultado.*
