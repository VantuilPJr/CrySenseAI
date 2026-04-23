# DOCUMENTAÇÃO PRÉ-BANCA (MODELO ABNT)

## CAPA

PONTIFÍCIA UNIVERSIDADE CATÓLICA DO PARANÁ  
ESCOLA POLITÉCNICA  
CURSO DE ENGENHARIA DE SOFTWARE  

VANTUIL PLASTER  

CRYSENSE AI V2.0: SISTEMA CIBERFÍSICO EMBARCADO PARA MONITORAMENTO DE CHORO INFANTIL COM EDGE AI, OBSERVABILIDADE E INTERFACE WEB  

CURITIBA  
2026

## FOLHA DE ROSTO

VANTUIL PLASTER  

CRYSENSE AI V2.0: SISTEMA CIBERFÍSICO EMBARCADO PARA MONITORAMENTO DE CHORO INFANTIL COM EDGE AI, OBSERVABILIDADE E INTERFACE WEB  

Trabalho Discente Efetivo apresentado à disciplina Performance em Sistemas Ciberfísicos, do Curso de Engenharia de Software da Pontifícia Universidade Católica do Paraná, como requisito acadêmico parcial para avaliação na disciplina.  

Professor responsável: Fábio Bettio  

CURITIBA  
2026

## RESUMO

Este trabalho apresenta o desenvolvimento e a análise técnica do CrySense AI v2.0, um sistema ciberfísico real implementado em ESP32-S3 para monitoramento de choro infantil com inferência local. A solução integra captura de áudio digital via I2S, classificação por modelo TinyML, histerese temporal de decisão, monitoramento ambiental, atuação local, persistência de dados e interface web embarcada. A implementação foi analisada frente aos requisitos do TDE da disciplina, com matriz de conformidade funcional, não funcional e de hardware. Como base científica para o núcleo de áudio e classificação, foram utilizados dois estudos: o trabalho de Ozcan e Gungor (2025), que demonstra ganhos de robustez com MFCC e data augmentation em classificação de sons de bebê, e o estudo de Goberman e Whitfield (2013), que evidencia o papel da frequência fundamental como marcador de arousal em choro de dor. Os resultados indicam aderência elevada aos requisitos de arquitetura multitarefa, logging persistente, configuração sem reflash, monitoramento web e gerenciamento de energia. As principais pendências para fechamento de banca concentram-se na eliminação de chamadas bloqueantes de inicialização, na consolidação de métricas de tempo de cinco funções na GUI e na formalização de evidências físicas de montagem final.

Palavras-chave: sistema ciberfísico; TinyML; ESP32-S3; MFCC; observabilidade; FreeRTOS.

## ABSTRACT

This work presents the development and technical analysis of CrySense AI v2.0, a real cyber-physical system implemented on ESP32-S3 for infant cry monitoring with on-device inference. The solution integrates digital audio capture through I2S, TinyML-based classification, temporal hysteresis for decision making, environmental sensing, local actuation, data persistence, and an embedded web interface. The implementation was assessed against the course TDE requirements using a functional, non-functional, and hardware compliance matrix. Two studies were used as the scientific basis for the audio and classification core: Ozcan and Gungor (2025), which reports robustness gains with MFCC and data augmentation for baby-sound classification, and Goberman and Whitfield (2013), which highlights fundamental frequency as an arousal marker in pain cries. Results indicate strong compliance in multitask architecture, persistent logging, no-reflash configuration, web monitoring, and power management. The main pending items before the final defense are removing blocking startup calls, consolidating execution-time metrics of five core functions in the GUI, and providing formal evidence of final physical assembly.

Keywords: cyber-physical system; TinyML; ESP32-S3; MFCC; observability; FreeRTOS.

## SUMÁRIO (ESTRUTURA)

1 Introdução  
2 Objetivo  
3 Requisitos do sistema  
4 Fundamentação teórica  
5 Metodologia  
6 Desenvolvimento do sistema  
7 Testes e validação  
8 Resultados  
9 Conclusão  
10 Referências  
11 Anexo técnico de conformidade pré-banca  

## 1 INTRODUÇÃO

O CrySense AI v2.0 foi concebido como sistema ciberfísico embarcado para monitoramento de choro infantil com processamento local, priorizando privacidade, baixa latência e operação contínua. A solução combina hardware de captura e atuação, firmware multitarefa, inferência de áudio e interface web para observabilidade do comportamento do sistema em campo.

Do ponto de vista acadêmico, o projeto endereça diretamente os eixos da disciplina: desempenho, concorrência, paralelismo, persistência, monitoramento e gestão de energia. O escopo desta versão pré-banca é documentar com rigor o estado real do código, evidenciar aderência à rubrica do TDE e explicitar lacunas objetivas para fechamento final.

## 2 OBJETIVO

Desenvolver, implementar e validar um sistema ciberfísico real em ESP32-S3 com:

- monitoramento de áudio e classificação local de choro
- painel web embarcado para monitoramento de desempenho e configuração
- persistência de configurações e logs operacionais
- uso de multitarefa, interrupção de hardware e estratégia de economia de energia

## 3 REQUISITOS DO SISTEMA

### 3.1 Requisitos funcionais

Tabela de conformidade (código atual):

| Requisito do TDE | Evidência no sistema atual | Status |
|---|---|---|
| Interface web embarcada | AsyncWebServer e frontend React publicado em SPIFFS | Atende |
| Guia de performance | Aba Performance com CPU, heap, PSRAM, inferência, uptime | Parcial |
| Memória (heap/stack/flash/PSRAM) | Heap/flash/PSRAM expostos; stack por task ainda não exposta na GUI | Parcial |
| Tempo de no mínimo 5 funções | Tempos de TaskAudio, TaskIA, TaskDecisao, TaskHMI, TaskIOT no Serial | Parcial |
| Dados de sensores | Temperatura/umidade/pressão coletadas e publicadas | Atende |
| Informações de tasks/threads | Não há endpoint dedicado de estado de tasks | Não atende |
| Estado Wi-Fi/AP | Estado STA/AP e IP expostos em /api/status | Atende |
| Gráficos de performance | Gráficos temporais no frontend para recursos e sensores | Atende |
| Logs erro/warning/info | LogManager com níveis I/W/E/A persistentes | Atende |
| Exportação de logs | /api/logs/csv implementado | Atende |
| Configuração dinâmica via web | /api/config GET/POST com persistência em NVS | Atende |
| Troca de Wi-Fi sem reflash | Alteração de SSID/senha e reboot controlado | Atende |
| Guia Sobre completa | Universidade e autor presentes; e-mail e GitHub faltantes | Parcial |
| Guia Changelog | Aba Changelog presente | Atende |

### 3.2 Requisitos não funcionais

| Requisito do TDE | Evidência no sistema atual | Status |
|---|---|---|
| Persistência mínima de 24h | Logs persistentes com rotação em SPIFFS e config em NVS; histórico integral 24h de todas as variáveis ainda não comprovado | Parcial |
| Sem rotinas bloqueantes | Ainda existem delay(500), delay(800) e delay(1000) no setup | Não atende |
| Mais de uma task | Cinco tasks principais em FreeRTOS | Atende |
| Interrupção de hardware | ISR no GPIO0 (botão BOOT) para reset de estado | Atende |
| Gerenciamento de energia | Light sleep com wakeup por timer e políticas Wi-Fi | Atende |
| Linguagem permitida | C/C++ (Arduino + APIs ESP-IDF) | Atende |
| GitHub público | Necessita comprovação externa | Pendente |

### 3.3 Requisitos de hardware

| Requisito do TDE | Evidência no sistema atual | Status |
|---|---|---|
| Mínimo 2 sensores de entrada | INMP441 e BME280 | Atende |
| Mínimo 2 atuadores (não LED/buzzer) | MAX98357A (áudio) e OLED SSD1306 | Parcial (validar aceitação de OLED como atuador na banca) |
| Caixa física e montagem segura | Exige evidência física de bancada | Pendente |
| Entrega física funcional | Exige validação presencial | Pendente |

## 4 FUNDAMENTAÇÃO TEÓRICA

### 4.1 Sistemas ciberfísicos e observabilidade

Sistemas ciberfísicos exigem acoplamento entre sensoriamento físico, processamento embarcado e atuação em malha de decisão. No CrySense AI, esse acoplamento ocorre por pipeline de tarefas com observabilidade operacional em dashboard e logs persistentes.

### 4.2 Processamento de áudio e MFCC

O uso de MFCC como representação de características é justificável por sua capacidade de capturar informação perceptualmente relevante para classificação de sinais acústicos. O estudo de Ozcan e Gungor demonstra desempenho competitivo com MFCC associado a data augmentation e ajuste de hiperparâmetros em classificação de sons de bebê (OZCAN; GUNGOR, 2025).

### 4.3 Arousal, F0 e dinâmica temporal do choro

Goberman e Whitfield mostram que a frequência fundamental varia ao longo do episódio de choro de dor e se relaciona com arousal, reforçando que o comportamento temporal importa para interpretação robusta (GOBERMAN; WHITFIELD, 2013). Essa evidência sustenta a escolha de histerese temporal no firmware.

### 4.4 Concorrência e desempenho em RTOS

A separação do fluxo em tarefas independentes com sincronização por semáforos e filas reduz acoplamento e melhora previsibilidade. Em sistemas embarcados de áudio em tempo real, essa abordagem é essencial para estabilidade operacional sob restrição de recursos.

### 4.5 Gerenciamento de energia em IoT embarcado

A aplicação de light sleep por inatividade e políticas de economia no subsistema Wi-Fi atende ao princípio de eficiência energética sem perda de funcionalidade essencial de monitoramento.

## 5 METODOLOGIA

Foi utilizada metodologia incremental e iterativa, com três eixos de avaliação:

1. análise estática e funcional do código atual
2. confronto com requisitos oficiais do TDE (documento v1.1)
3. triangulação com documentos internos de status e plano

Documentos analisados:

- STATUS_TECNICO.md
- EDGE_IMPULSE_DATASET_SPEC.md
- CHAT_LEGADO_TRIAGEM_SEM_EDTECH.md
- implementation_plan.md.resolved

Crítico desta fase: implementation_plan.md.resolved foi tratado como referência histórica, não como espelho do estado atual, devido a divergências confirmadas no firmware em produção.

## 6 DESENVOLVIMENTO DO SISTEMA

### 6.1 Arquitetura geral

Pipeline operacional:

1. captura de áudio I2S (TaskAudio)
2. gate RMS para silêncio real
3. inferência TinyML (TaskIA)
4. cascata local de detecção e subtipo
5. histerese temporal (buffer circular de 10 ciclos)
6. decisão e atuação (TaskDecisao)
7. telemetria e observabilidade (TaskHMI, TaskIOT e WebServer)

### 6.2 Organização do firmware e módulos

Módulos centrais:

- crySence.ino (orquestração e lógica de decisão)
- audio_player.h (captura, gate RMS e reprodução)
- sensor_bme280.h (telemetria ambiental)
- display_manager.h (HMI OLED)
- firebase_manager.h (integração RTDB e métricas de CPU)
- web_server.h (API e interface web)
- log_manager.h (persistência e exportação de logs)
- config_manager.h (NVS)

### 6.3 Estrutura das tasks

- TaskAudio
- TaskIA
- TaskDecisao
- TaskHMI
- TaskIOT

Sincronização por semáforo de áudio pronto e filas de inferência/firebase.

### 6.4 Uso de interrupção de hardware

ISR no GPIO0 para reset manual de estado de crise, com tratamento no contexto de task para evitar lógica pesada na interrupção.

### 6.5 Estratégias de gerenciamento de energia

- configuração de modo de economia Wi-Fi
- entrada em light sleep por inatividade
- wakeup periódico por timer

### 6.6 Interface web e observabilidade

Abas implementadas:

- Performance
- Sensores
- IA Status
- Logs
- Config
- Sobre
- Changelog

Endpoints ativos:

- /api/status
- /api/logs
- /api/logs/csv
- /api/logs/clear
- /api/config
- /api/config/reset
- /api/audio/upload
- /api/audio/delete

### 6.7 Persistência de dados

- NVS: configurações operacionais
- SPIFFS: logs com rotação

Lacuna para fechamento ABNT/TDE: demonstrar persistência de 24h para o conjunto de informações requerido pela rubrica, com evidências de coleta.

### 6.8 Revisão crítica do plano histórico

Foram identificadas divergências entre implementation_plan.md.resolved e estado atual:

- classes e thresholds históricos não refletem a cascata atual
- parâmetros do plano não correspondem aos defaults atuais de configuração
- parte da arquitetura foi substituída por implementações mais recentes

Conclusão: o plano histórico permanece útil para contextualização, mas a fonte normativa de estado passa a ser o código atual e esta documentação pré-banca.

## 7 TESTES E VALIDAÇÃO

### 7.1 Testes isolados

Estado atual por módulo:

- Áudio e inferência: funcional
- BME280: funcional
- OLED: funcional
- Logs persistentes: funcional
- API web: funcional

### 7.2 Testes de integração

Integrações validadas em código:

- inferência -> decisão -> alerta/atuação
- sensores -> estado global -> dashboard
- config web -> NVS -> reboot
- fila firebase -> RTDB

Pendências para banca:

- protocolo de teste contínuo 24h
- consolidação de tempos de cinco funções na GUI
- relatório de consumo com e sem light sleep
- matriz de testes de campo com áudios negativos e positivos

## 8 RESULTADOS

Resultados técnicos consolidados:

- firmware multitarefa operacional no ESP32-S3
- observabilidade web funcional
- persistência de logs implementada e exportável
- estratégia de energia implementada
- interrupção de hardware implementada

Resultados de conformidade:

- aderência alta em arquitetura e monitoramento
- aderência parcial em requisitos de interface detalhada e evidências de 24h
- pendências objetivas para fechamento final de rubrica

## 9 CONCLUSÃO

O CrySense AI v2.0 apresenta base técnica consistente para defesa pré-banca, com arquitetura ciberfísica funcional, modular e observável. O sistema já cumpre parcela majoritária dos requisitos centrais do TDE em firmware, web e pipeline de decisão.

As ações prioritárias para fechamento final são:

- remover delays de boot para conformidade total de não bloqueio
- expor na GUI tempos de execução de pelo menos cinco funções principais
- completar a aba Sobre com e-mail e repositório GitHub
- produzir evidência formal de persistência de 24h
- consolidar evidência física de montagem/caixa

## 10 REFERÊNCIAS (ABNT)

GOBERMAN, Alexander M.; WHITFIELD, Jason A. Acoustics of infant pain cries: fundamental frequency as a measure of arousal. Perspectives on Speech Science and Orofacial Disorders, [S. l.], 2013.

OZCAN, Tayyip; GUNGOR, Hafize. Baby cry classification using structure-tuned artificial neural networks with data augmentation and MFCC features. Applied Sciences, Basel, v. 15, n. 5, art. 2648, 2025. DOI: https://doi.org/10.3390/app15052648.

## 11 ANEXO TÉCNICO DE CONFORMIDADE PRÉ-BANCA

### 11.1 Checklist de fechamento para banca final

- [ ] substituir delay de setup por estratégia não bloqueante
- [ ] incluir painel de tempos de 5 funções principais na GUI
- [ ] incluir e-mail e link GitHub na aba Sobre
- [ ] executar teste contínuo de 24h e anexar evidências
- [ ] registrar evidências fotográficas da montagem física em caixa

### 11.2 Observação final de escopo científico

A base conceitual de áudio e classificação deste trabalho foi explicitamente sustentada por dois artigos utilizados no projeto:

- Ozcan e Gungor (2025), para robustez de classificação por MFCC com augmentação
- Goberman e Whitfield (2013), para interpretação temporal e acústica de arousal via F0

Essa fundamentação orienta tanto a arquitetura atual quanto os próximos ciclos de melhoria do modelo.
