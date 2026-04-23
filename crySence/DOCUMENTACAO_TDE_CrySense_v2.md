# Trabalho Discente Efetivo (TDE)

Escola/Campus: Escola Politecnica  
Curso: Engenharia de Software  
Ano/Semestre: 2026/1  
Disciplina: Performance em Sistemas Ciberfisicos  
Professor Responsavel: Fabio Bettio  

Titulo do projeto: CrySense AI v2.0 - Monitoramento embarcado de choro infantil com Edge AI e observabilidade web

## 1. Introducao

Este documento apresenta a documentacao tecnica e academica do sistema CrySense AI v2.0, desenvolvido como sistema ciberfisico real em ESP32-S3. O projeto integra captura de audio digital, inferencia local com Edge AI, logica temporal de decisao, sensor ambiental, atuacao local e interface web embarcada.

O trabalho foi estruturado com foco em desempenho, concorrencia, uso de recursos, persistencia e monitoramento operacional. A implementacao atual foi analisada diretamente no codigo-fonte e confrontada com os requisitos do TDE, com rastreabilidade para requisitos atendidos, parcialmente atendidos e pendentes.

Fontes internas utilizadas nesta consolidacao:

- STATUS_TECNICO.md
- EDGE_IMPULSE_DATASET_SPEC.md
- CHAT_LEGADO_TRIAGEM_SEM_EDTECH.md
- implementation_plan.md.resolved
- CrySenseAI-main/README.md
- CrySenseAI-main/react-app/README.md

## 2. Objetivo

Desenvolver, implementar e validar um sistema ciberfisico real, com firmware multitarefa e interface web embarcada, capaz de:

- detectar e classificar eventos de choro infantil em tempo real
- monitorar indicadores de desempenho (CPU, memoria e latencia)
- registrar e exportar logs persistentes
- permitir configuracao dinamica sem reflash
- aplicar estrategia de gerenciamento de energia

## 3. Requisitos do sistema

### 3.1 Requisitos funcionais

Analise de aderencia da implementacao atual aos requisitos funcionais do TDE.

| Requisito | Evidencia tecnica atual | Status |
|---|---|---|
| Interface web embarcada | Servidor AsyncWebServer em web_server.h com arquivos estaticos via SPIFFS | Atende |
| Guia de performance unificada | Frontend possui aba Performance com CPU, heap, PSRAM e inferencia | Parcial |
| Uso de CPU | firebase_manager.h calcula cpu0/cpu1 com idle counters e publica em estado | Atende |
| Uso de memoria (heap/flash/PSRAM) | /api/status expone heap, heap_min, flash_used, flash_tot, psram, psram_tot | Atende |
| Tempo de execucao de no minimo 5 funcoes principais | Existem tempos no Serial de TaskAudio, TaskIA, TaskDecisao, TaskHMI, TaskIOT, mas nao estao consolidados na UI de performance | Parcial |
| Dados dos sensores | BME280 lido em TaskHMI e publicado na API/status | Atende |
| Informacoes de tasks/threads | Nao ha pagina/endpoint com tabela de tasks (nome, stack water mark, estado) | Nao atende |
| Estado Wi-Fi e AP | setup e status web tratam STA/AP e expoem wifi_ok e ip | Atende |
| Graficos de performance e series temporais | Frontend mostra graficos de heap, inferencia, temperatura e umidade | Atende |
| Sistema de logs com niveis | log_manager.h implementa I/W/E/A com persistencia | Atende |
| Exportacao de logs | Endpoint /api/logs/csv implementado | Atende |
| Configuracao via web (sensores e parametros) | Endpoint /api/config e aba Config implementados | Atende |
| Troca do ponto de acesso Wi-Fi sem regravar | /api/config permite atualizar SSID/senha e reiniciar | Atende |
| Guia Sobre com universidade, integrantes, email, GitHub | AboutView possui universidade e autor; nao possui email e link GitHub | Parcial |
| Guia de changelog acessivel na GUI | Aba Changelog presente no App.jsx | Atende |

### 3.2 Requisitos nao funcionais

| Requisito | Evidencia tecnica atual | Status |
|---|---|---|
| Persistencia minima de 24h de todas as informacoes | Logs persistem em SPIFFS com rotacao; configuracoes persistem em NVS; historico completo de sensores/eventos ainda nao esta garantido por 24h | Parcial |
| Proibicao de rotinas bloqueantes (ex.: delay()) | crySence.ino ainda usa delay(500), delay(800), delay(1000) no boot | Nao atende |
| Uso de mais de uma task | TaskAudio, TaskIA, TaskDecisao, TaskHMI, TaskIOT | Atende |
| Uso de interrupcao de hardware | attachInterrupt no GPIO0 para reset manual de estado | Atende |
| Gerenciamento de energia (Light/Deep Sleep) | Light sleep implementado com timer wakeup e politicas por inatividade | Atende |
| Linguagem C/C++ ou MicroPython | Projeto em C/C++ (Arduino/ESP-IDF APIs) | Atende |
| Codigo versionado em GitHub publico | Requisito depende de evidencia externa ao workspace local | Pendente de comprovacao |

### 3.3 Requisitos de hardware

| Requisito | Evidencia tecnica atual | Status |
|---|---|---|
| Minimo de 2 sensores de entrada | INMP441 (audio) + BME280 (ambiental) | Atende |
| Minimo de 2 dispositivos de acionamento (nao LED/buzzer) | Alto-falante MAX98357A e display OLED SSD1306 | Parcial (validar aceitacao docente do OLED como atuador) |
| Caixa fisica apropriada | Nao verificavel por codigo | Pendente de validacao fisica |
| Montagem organizada e segura | Nao verificavel por codigo | Pendente de validacao fisica |
| Entrega fisica montada e funcional | Nao verificavel por codigo | Pendente de validacao fisica |

## 4. Fundamentacao teorica

### 4.1 Sistemas ciberfisicos

Sistemas ciberfisicos combinam componentes computacionais e fisicos com realimentacao continua. No CrySense AI, sensores (microfone e BME280) alimentam tarefas de software em tempo real, que por sua vez acionam atuadores locais e interfaces de monitoramento.

### 4.2 Concorrencia e paralelismo em embarcados

A arquitetura baseada em FreeRTOS separa responsabilidades em tarefas independentes com prioridades distintas, reduzindo acoplamento e permitindo melhor previsibilidade temporal. O uso de filas e semaforos evita bloqueios longos em seccoes criticas.

### 4.3 Gerenciamento de memoria e desempenho

O projeto utiliza:

- PSRAM para buffer de inferencia
- monitoramento de heap minimo
- coleta periodica de uso de CPU por nucleo
- processamento por janelas de audio e gate RMS para evitar inferencias desnecessarias

Essas escolhas impactam diretamente latencia, robustez e consumo.

### 4.4 RTOS e escalonamento

A divisao em TaskAudio, TaskIA, TaskDecisao, TaskHMI e TaskIOT organiza o fluxo em pipeline e favorece observabilidade operacional. O uso de portMAX_DELAY e vTaskDelay em pontos controlados reduz busy-wait.

### 4.5 Gerenciamento de energia em IoT

O firmware aplica light sleep condicional por inatividade, com wakeup por timer e ajustes de politica Wi-Fi, buscando equilibrio entre disponibilidade e economia de energia.

### 4.6 Observabilidade, logging e monitoramento

A observabilidade e tratada em tres eixos:

- logs persistentes com niveis (I/W/E/A)
- metricas de desempenho via API/status
- dashboard web com graficos e estado operacional

## 5. Metodologia

Foi adotada metodologia incremental e iterativa, com ciclos de:

1. diagnostico de comportamento em runtime
2. ajuste de regras de inferencia e decisao
3. validacao via telemetria local/web
4. consolidacao documental

A analise desta versao seguiu triangulacao entre:

- codigo atual do firmware
- frontend React/Vite
- documentos internos de status e plano

Adicionalmente, o documento implementation_plan.md.resolved foi tratado como baseline historica e confrontado com o estado real atual para evitar heranca de decisoes obsoletas.

## 6. Desenvolvimento do sistema

### 6.1 Arquitetura geral

Pipeline funcional atual:

1. Captura de audio I2S (INMP441)
2. Gate RMS para silencio real
3. Inferencia Edge Impulse (janela de 1s)
4. Cascata local de deteccao e subtipo
5. Histerese temporal em buffer circular
6. Atuacao local (audio) e notificacao IoT
7. Exposicao de estado para dashboard web

### 6.2 Organizacao do firmware

Modulos principais:

- crySence.ino: orquestracao, setup, tarefas de IA e decisao
- audio_player.h: captura I2S, gate RMS e atuacao de audio
- sensor_bme280.h: sensoriamento ambiental
- display_manager.h: HMI OLED e telemetria de sensores
- firebase_manager.h: envio RTDB e metricas CPU
- web_server.h: API REST e arquivos estaticos
- log_manager.h: persistencia e exportacao de logs
- config_manager.h: configuracoes persistentes em NVS

### 6.3 Estrutura das tasks

Tarefas implementadas:

- TaskAudio (captura e pre-processamento)
- TaskIA (inferencia)
- TaskDecisao (logica de negocio e estado)
- TaskHMI (display, sensores e envio periodico)
- TaskIOT (fila Firebase e heartbeat)

### 6.4 Uso de interrupcoes de hardware

Interrupcao GPIO0 (botao BOOT) para reset manual do estado de crise, com flag atomica e tratamento assinado na TaskDecisao.

### 6.5 Estrategias de energia

- Light sleep quando inatividade supera limite configurado
- wakeup por timer
- ajuste de politica de energia do Wi-Fi

### 6.6 Estrutura da interface web

A interface embarcada contem abas de:

- Performance
- Sensores
- IA Status
- Logs
- Config
- Sobre
- Changelog

APIs principais:

- /api/status
- /api/logs
- /api/logs/csv
- /api/config (GET/POST)
- /api/config/reset
- /api/audio/upload
- /api/audio/delete

### 6.7 Persistencia de dados

Persistencia atual:

- NVS (Preferences): configuracoes operacionais
- SPIFFS: logs CSV com rotacao por tamanho

Lacuna atual:

- nao ha confirmacao de trilha completa de 24h para todas as informacoes de sistema (nao apenas logs)

### 6.8 Confronto tecnico com implementation_plan.md.resolved

Pontos do plano historico que estao obsoletos frente ao codigo atual:

- classe discomfort nao aparece no fluxo final atual de inferencia local
- threshold e gatilhos do plano nao sao mais os mesmos do firmware atual
- parte das propostas do plano ja foi substituida por implementacao diferente (cascata endurecida e mapeamento de config web)

Conclusao: implementation_plan.md.resolved e util como historico de intencao arquitetural, mas nao deve ser usado isoladamente como fonte de estado atual.

## 7. Testes e validacao

### 7.1 Testes isolados

Status por componente:

- Audio + inferencia: funcional, com gate RMS e classificacao por janela
- Sensor BME280: integrado e lido periodicamente
- Display OLED: funcional em estados de boot, normal e alerta
- Logs SPIFFS: escrita/leitura/exportacao implementadas
- API web: endpoints essenciais implementados

### 7.2 Testes de integracao

Integracoes funcionais presentes:

- sensores -> estado global -> dashboard
- IA -> decisao -> atuacao local e Firebase
- web config -> NVS -> reboot controlado

Pendencias de validacao formal:

- ensaio de 24h com evidencia de persistencia continua
- validacao objetiva de consumo em light sleep
- consolidacao de tempo de execucao de no minimo 5 funcoes na GUI
- matriz de testes de campo com audios negativos e positivos

## 8. Resultados

### 8.1 Resultados tecnicos observados

- Arquitetura multitarefa operacional no ESP32-S3
- Dashboard web funcional para operacao e configuracao
- Logs persistentes com exportacao CSV
- Gerenciamento de energia implementado (light sleep)
- Interrupcao de hardware implementada

### 8.2 Resultados de conformidade com rubrica

Resumo da aderencia atual:

- atendidos: base de firmware, multitarefa, interrupcao, web, logs/exportacao, configuracao sem reflash
- parciais: performance unificada com tempos de 5 funcoes na GUI, persistencia completa 24h, requisitos completos da aba Sobre
- pendentes: comprovacao fisica de caixa/montagem e criterio de 2 atuadores sem ambiguidade

## 9. Conclusao

O CrySense AI v2.0 encontra-se tecnicamente maduro para a fase de validacao formal do TDE, com base embarcada robusta e aderencia significativa aos requisitos da disciplina. O sistema ja apresenta pipeline de sensoriamento, inferencia, decisao, monitoramento, persistencia e configuracao remota.

As principais lacunas para fechamento academico sao objetivas e trataveis:

- eliminar chamadas delay no boot
- expor na GUI tempos de execucao de pelo menos cinco funcoes principais
- completar requisitos da aba Sobre (email e GitHub)
- comprovar persistencia de 24h para o conjunto exigido de informacoes
- formalizar validacao fisica de hardware e atuadores para a banca

Com essas correcoes, o projeto tende a atingir alta aderencia na rubrica do TDE.

## 10. Referencias (formato ABNT)

BABY CRY CLASSIFICATION USING STRUCTURE-TUNED ARTIFICIAL NEURAL NETWORKS WITH DATA AUGMENTATION AND MFCC FEATURES. [S.l.: s.n.], 2025.

DAVIS, S.; MERMELSTEIN, P. Comparison of parametric representations for monosyllabic word recognition in continuously spoken sentences. IEEE Transactions on Acoustics, Speech, and Signal Processing, v. 28, n. 4, p. 357-366, 1980.

ESPRESSIF SYSTEMS. ESP-IDF Programming Guide: FreeRTOS, Sleep Modes, Wi-Fi Power Save. Disponivel em: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/. Acesso em: 15 mar. 2026.

GOBERMAN, A. M.; WHITFIELD, J. A. Acoustics of Infant Pain Cries: Fundamental Frequency as a Measure of Arousal. SIG 5 Perspectives on Voice and Voice Disorders, 2013.

## 11. Criterios de avaliacao e autoavaliacao tecnica

Autoavaliacao sintetica frente a rubrica do TDE:

- Implementacao tecnica do sistema: alta, com pendencia de retirada de delay no boot.
- Interface web e monitoramento: media-alta, com pendencia de tempos de 5 funcoes na GUI e informacoes de tasks mais detalhadas.
- Persistencia e logs: media, pois logs estao robustos, mas persistencia integral de 24h para todas as informacoes ainda requer demonstracao formal.
- Hardware e execucao fisica: pendente de evidencia presencial para fechamento.
- Energia e performance: boa base implementada com light sleep, faltando consolidacao de resultados experimentais.
- Documentacao academica: estrutura alinhada ao template v1.1 com pontos de aprofundamento para versao final ABNT completa (formatacao final em editor academico).
