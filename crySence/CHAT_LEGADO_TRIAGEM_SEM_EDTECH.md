# Triagem do chat legado (sem EdTech)

Data: 2026-03-15

Objetivo deste arquivo:

- aproveitar o valor do chat antigo
- remover ruído de contexto não relevante
- separar o que está confirmado no código atual do que está desatualizado ou não confirmado

## 1) O que do relatório legado está consistente com o projeto atual

Itens confirmados no código atual:

- Projeto está em ESP32-S3 com Edge Impulse e arquitetura em tarefas.
- Existe separação clara entre captura de áudio, inferência e decisão.
- Há buffer histórico de tamanho 10 para histerese temporal.
- O projeto já usa sensor BME280 e interface web.
- A lógica de decisão trabalha com classes de choro e ruído.
- Há foco real em reduzir falso positivo por música/voz/TV.

## 2) O que no relatório legado está desatualizado ou divergente

### 2.1 GAIN_MULTIPLIER

No texto legado aparece ajuste para 2.5f/3.0f como estado final.

No código atual:

- `audio_player.h` está com `GAIN_MULTIPLIER = 5.0f`.

Conclusão: o relatório antigo não reflete o valor atual em uso.

### 2.2 Gatilho da máquina temporal

No texto legado aparece gatilho 8/10 como versão consolidada.

No código atual:

- default em `config_manager.h` é `gatilho_decisao = 7`.
- buffer continua 10 posições.

Conclusão: hoje o padrão está 7/10, não 8/10.

### 2.3 Sensor secundário citado

No texto legado há menção de DHT22 como pendência principal.

No código atual:

- sensor integrado é BME280 (`sensor_bme280.h`).

Conclusão: DHT22 não é o sensor ativo no firmware atual.

### 2.4 Classes de choro do texto histórico

No texto legado aparece "desconforto" em alguns pontos de contexto.

No código atual, classes usadas:

- `colic`
- `hunger`
- `sleep`
- `noise`

Conclusão: o vocabulário atual de classes é colic/hunger/sleep/noise.

### 2.5 Critério de reset por 60 segundos

No texto legado aparece regra fixa de 60s de silêncio.

No código atual:

- existe `CICLOS_PARA_RESETAR 60` no arquivo principal
- também existe lógica parametrizada com `gatilho_silencio`

Conclusão: esse ponto precisa de validação operacional no comportamento final, pois há mecanismo misto no código.

## 3) Itens úteis do legado que devem ser preservados

Mesmo com divergências, os seguintes pontos históricos são valiosos:

- narrativa de causa raiz sobre falso positivo por saturação/ruído
- decisão de usar histerese temporal com buffer circular
- foco em robustez antes de disparar alerta
- preocupação com estabilidade de memória e tarefas
- preocupação com métricas de aceite e banca acadêmica

## 4) Itens do legado que devem ser tratados como hipótese

Não estão confirmados por evidência no estado atual e não devem ser tratados como fato sem nova verificação:

- sequência exata de erros antigos (`-1002`, `-5`) como histórico definitivo
- valor final de ganho 3.0f
- gatilho definitivo 8/10
- dependência obrigatória de DHT22
- interpretação de que arquitetura atual ainda estaria em loop simples (hoje há tarefas explícitas)

## 5) Estado técnico confiável para continuidade

Para retomar desenvolvimento sem se perder, usar como verdade operacional:

- Gate RMS ativo no áudio (pré-filtro).
- Inferência em janela curta com cascata local endurecida.
- Histerese temporal de 10 ciclos com gatilhos configuráveis.
- API web e configuração persistente já implementadas.
- Sensor ativo no firmware: BME280.
- Proposta estratégica já definida: migrar para dois modelos (binário + subtipo).

## 6) Próximos passos recomendados com base na triagem

1. Validar em hardware o comportamento atual com pacote de áudios negativos e choro real.
2. Confirmar e congelar valores de produção para `GAIN_MULTIPLIER`, `gatilho_decisao` e `gatilho_silencio`.
3. Seguir a especificação dos datasets em `EDGE_IMPULSE_DATASET_SPEC.md`.
4. Só depois avançar para integração de dois modelos no firmware.

## 7) Decisão prática sobre o chat legado

Como usar o chat antigo daqui para frente:

- usar para contexto histórico e justificativa
- não usar valores numéricos antigos sem checagem contra o código atual
- registrar todo novo valor validado diretamente no repositório
