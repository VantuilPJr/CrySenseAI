# CrySense AI - Status tecnico atual

Atualizado em 2026-03-15.

Complemento de contexto histórico validado:

- `CHAT_LEGADO_TRIAGEM_SEM_EDTECH.md`
- `DOCUMENTACAO_TDE_CrySense_v2.md`
- `DOCUMENTACAO_TDE_PRE_BANCA_ABNT.md` (versao recomendada para pre-banca)

## 1. Objetivo atual do sistema

O projeto esta tentando detectar choro de bebe no ESP32-S3 e, somente quando houver choro real, classificar o tipo entre:

- colica
- fome
- sono

O comportamento desejado deixou de ser um classificador unico sempre escolhendo entre ruido, colica, fome e sono. A direcao atual e uma cascata:

1. detectar se existe choro ou nao
2. se existir choro, classificar qual tipo de choro

## 2. Arquitetura atual do firmware

Hoje o fluxo real esta assim:

1. `audio_player.h` captura audio em 16 kHz.
2. Existe um gate de energia RMS antes da IA.
3. Se `rms < 0.025f`, o sistema injeta `noise` diretamente e evita inferencia.
4. Se houver energia suficiente, a TaskIA roda o modelo Edge Impulse atual.
5. Em `crySence.ino`, a primeira etapa local tenta decidir se o audio de 1 segundo e predominantemente choro.
6. Se passar nessa etapa, a segunda fase escolhe entre colica, fome e sono.
7. Depois disso, ainda existe uma logica historica de confirmacao em buffer para marcar estado de crise.

Ou seja, ja existe uma cascata local, mas ela ainda depende de um modelo multiclasses unico na origem.

## 3. Problema principal encontrado

O erro observado foi um falso positivo forte: musica, TV ou fala podiam ativar a primeira etapa de choro e depois o sistema acabava classificando como fome ou outro tipo, chegando a mostrar `crise` na interface.

O problema de raiz nao era apenas a interface. O problema principal era este:

- a etapa 1 ainda estava sendo tomada a partir das probabilidades do modelo multiclasses atual
- isso permitia que audio nao-choro com distribuicao parecida com classes de choro passasse pelo gate
- como a etapa 2 obrigatoriamente escolhe uma classe de choro, o sistema transformava um falso positivo de deteccao em um falso positivo de subtipo

Em resumo: o modelo atual nao separa bem `choro` de `nao choro`, e a cascata local ainda estava acoplada demais a essa saida multiclasses.

## 4. O que foi aprendido ao inspecionar o codigo

### 4.1 Gate de silencio real

Ja existia uma defesa util antes da IA:

- `audio_player.h` calcula RMS do buffer de entrada
- abaixo de `0.025f`, o sistema envia `LABEL_NOISE` com `isSilencioReal = true`
- isso economiza CPU e evita falsos positivos em silencio real

Esse trecho esta correto como pre-gate e deve continuar existindo mesmo com novos modelos.

### 4.2 Etapa 1 local estava permissiva

Antes do ajuste, a etapa 1 aceitava audio com base em soma de probabilidades de choro e uma comparacao relativamente fraca com `noise`. Isso ainda deixava espaco para musica e voz humana passarem.

### 4.3 Etapa 2 usa apenas as classes de choro

Uma vez que o audio passa pelo gate local, a decisao final de subtipo considera so as classes de choro. Existe ainda um bonus manual em colica:

- `pColica = scoreColica * 1.45f`

Isso continua sendo uma regra de negocio valida, mas ela so faz sentido se a etapa 1 for realmente confiavel.

### 4.4 A interface nao estava conseguindo configurar tudo corretamente

O frontend e o backend nao falavam exatamente o mesmo dialeto de configuracao.

O formulario web enviava campos como:

- `confianca`
- `volume`
- `temp_max`
- `temp_min`
- `sensor_s`
- `sleep_min`

Mas o firmware esperava nomes como:

- `confianca_minima`
- `volume_audio`
- `temp_max_conforto`
- `temp_min_conforto`
- `sensor_intervalo_ms`
- `light_sleep_min`

Na pratica, parte do tuning feito pela interface podia nao estar chegando ao firmware.

## 5. Mudancas ja aplicadas no codigo

### 5.1 Endurecimento da etapa 1 em `crySence.ino`

Foi implementado um gate mais conservador para a pergunta: `o som e predominantemente choro?`

Agora a etapa 1 exige simultaneamente:

- pico de choro suficientemente alto
- soma de choros suficientemente alta
- vantagem absoluta sobre ruido
- vantagem proporcional sobre ruido
- separacao minima da segunda melhor classe de choro

As regras atuais sao:

- `minPicoChoro = confianca_minima - 0.05`, limitado para o intervalo `0.55` ate `0.85`
- `minSomaChoro = confianca_minima + 0.08`, limitado para o intervalo `0.75` ate `0.92`
- `maxChoroSpike >= scoreRuido + 0.12`
- `maxChoroSpike >= scoreRuido * 1.35`
- `maxChoroSpike - segundoChoroSpike >= 0.08`

Se isso falhar, o firmware forca o resultado para `noise` imediatamente naquela janela de 1 segundo.

### 5.2 Compatibilidade de configuracao em `web_server.h`

O endpoint `/api/config` foi ajustado para aceitar tanto os nomes internos do firmware quanto os nomes enviados pelo frontend atual.

Mapeamentos adicionados:

- `confianca` -> `confianca_minima` com conversao de porcentagem
- `volume` -> `volume_audio`
- `temp_max` -> `temp_max_conforto`
- `temp_min` -> `temp_min_conforto`
- `sensor_s` -> `sensor_intervalo_ms` multiplicando por 1000
- `sleep_min` -> `light_sleep_min`

## 6. Estado atual apos as mudancas

### Melhorias reais

- a etapa 1 esta mais dificil de ser enganada por musica, TV e fala
- o ajuste de confianca feito pela interface passou a ter efeito pratico no firmware
- o fluxo atual esta mais coerente com a intencao da cascata

### O que ainda NAO esta resolvido na raiz

- a etapa 1 ainda nasce das saidas do mesmo modelo multiclasses
- portanto o sistema ainda nao tem um detector dedicado de `choro` versus `nao choro`
- falsos positivos podem diminuir bastante, mas nao estao eliminados por principio

## 7. Erros e bloqueios encontrados

### 7.1 Bloqueio de build

Foi feita tentativa de compilacao real do firmware para `esp32:esp32:esp32s3`.

O bloqueio encontrado nao foi erro de sintaxe nos ajustes. O bloqueio foi a ausencia do header gerado pelo Edge Impulse:

- `CrySense_AI_inferencing.h`

Sem esse arquivo e seus artefatos gerados, o build do firmware nao fecha.

### 7.2 Validacao em hardware ainda pendente

Ainda falta validar em dispositivo real com uma bateria minima de audios:

- silencio
- fala humana
- TV
- musica
- ruido de ventilador
- cachorro
- choro real
- choro misturado com ruido de fundo

Sem isso, qualquer ajuste atual ainda e uma melhora heuristica, nao uma validacao final.

### 7.3 Risco de segredos em `secrets.h`

O projeto possui `secrets.h` com credenciais sensiveis de Wi-Fi e Firebase.

Boas praticas obrigatorias:

- nao versionar esse arquivo
- manter `secrets.h` no `.gitignore`
- rotacionar credenciais imediatamente se esse arquivo ja tiver sido publicado ou compartilhado

## 8. O que os artigos adicionados ensinaram ao projeto

Foram analisados os PDFs presentes no workspace.

### 8.1 Sobre classificacao de choro

O paper de classificacao de choro reforca:

- MFCC e variantes log-Mel seguem sendo uma base forte
- data augmentation melhora robustez
- avaliacao com sons mistos e essencial
- ruido de fundo e uma das maiores causas de falso positivo

O paper explicitamente trata cenarios como:

- TV
- fala
- ventilador
- risadas
- latidos
- sons domesticos em geral

Isso combina exatamente com o erro observado no CrySense AI.

### 8.2 Sobre a acustica do choro

O paper sobre F0 mostra que:

- a frequencia fundamental pode refletir arousal
- o inicio do choro pode ser diferente do restante do episodio
- segmentos iniciais e dinamica temporal podem carregar sinal util

Implicacao pratica: uma janela unica pode nao capturar tudo da mesma forma. Pode valer a pena usar estabilidade temporal ou features adicionais ao longo de mais de uma janela.

## 9. Nova direcao recomendada para a IA

### Recomendacao principal

Substituir a logica atual por duas IAs separadas no Edge Impulse:

1. Modelo binario: `choro` vs `nao_choro`
2. Modelo de subtipo: `colica`, `fome`, `sono`

### Por que essa arquitetura e melhor

- separa o problema de deteccao do problema de classificacao fina
- reduz o efeito cascata de um multiclasses mal calibrado
- combina com o que a literatura trata como problema real: primeiro detectar choro em meio a sons mistos
- permite treinar o modelo binario com dataset negativo muito mais forte e diverso
- evita que a etapa 2 precise aprender `noise`, o que so polui a fronteira entre subtipos de choro

## 10. Como treinar melhor os novos modelos

### 10.1 Modelo 1: binario `choro` vs `nao_choro`

Classe positiva:

- choro real de bebe
- choro em diferentes distancias
- choro com diferentes intensidades
- choro com ruido de fundo

Classe negativa:

- musica
- TV
- fala masculina e feminina
- ventilador
- transito
- cachorro
- brinquedos sonoros
- chuva/ruido branco
- silencio com ruido ambiente normal

Ponto importante: o negativo precisa ser mais diverso do que o atual. Esse e o modelo que segura o problema de raiz.

### 10.2 Modelo 2: subtipo de choro

Classes:

- colica
- fome
- sono

Esse segundo modelo deve receber somente amostras de choro. Nao deve existir classe `noise` aqui.

### 10.3 Data augmentation recomendada

- variacao de ganho
- ruido de fundo sintetico e real
- mixagem com fala e TV em niveis controlados
- pequenas variacoes temporais

## 11. Proposta de integracao no firmware

Fluxo sugerido:

1. manter o gate RMS atual
2. se passar no RMS, rodar modelo binario `choro` vs `nao_choro`
3. so rodar o modelo de subtipo se o binario disser `choro`
4. aplicar buffer temporal para confirmar crise usando a saida binaria, nao a de subtipo
5. usar o subtipo apenas para rotular o choro ja confirmado

Isso corrige o erro conceitual atual: hoje a confirmacao temporal ainda nasce de um processo cuja primeira camada depende do multiclasses antigo.

## 12. Ideias extras de implementacao

### 12.1 Histerese temporal separada por funcao

Usar dois niveis de estabilidade:

- estabilidade curta para `ha choro ou nao`
- estabilidade separada para `qual tipo de choro`

Isso evita que pequenas flutuacoes de subtipo mudem o estado geral de crise.

### 12.2 Janela de subtipo so quando necessario

Para economizar CPU e reduzir ruido de classificacao:

- rodar o classificador de subtipo apenas quando o detector binario estiver acima de threshold por N janelas

### 12.3 Registro de falsos positivos

Criar um log especifico para exemplos negativos importantes, com tags como:

- `music_fp`
- `tv_fp`
- `speech_fp`
- `dog_fp`

Isso acelera a curadoria de dataset para a proxima iteracao.

### 12.4 Revisar a UX da aba de IA

Como a interface oculta `noise` em estado de crise, vale manter o cuidado para nao mascarar incerteza quando o sistema ainda estiver em transicao. A UX final deve refletir a nova arquitetura binaria + subtipo.

## 13. Pendencias praticas imediatas

1. Recuperar ou gerar os arquivos do Edge Impulse, incluindo `CrySense_AI_inferencing.h`.
2. Validar o firmware atual em hardware com sons negativos reais.
3. Decidir se a proxima iteracao sera apenas tuning do modelo atual ou migracao imediata para dois modelos.
4. Seguir a especificacao operacional dos dois datasets em `EDGE_IMPULSE_DATASET_SPEC.md`.
5. Ajustar a API e a UI para refletirem explicitamente `deteccao de choro` e `tipo de choro` como estados distintos.

## 14. Conclusao tecnica

O sistema melhorou com o endurecimento local da etapa 1 e com a correcao da API de configuracao, mas isso ainda e uma correcao tatica.

A correcao estrategica recomendada e:

- manter o gate RMS
- criar um modelo binario forte para `choro` vs `nao_choro`
- usar um segundo modelo apenas para `colica`, `fome` e `sono`

Essa direcao esta alinhada com:

- o erro observado no projeto
- o comportamento atual do firmware
- a literatura lida nos PDFs do workspace