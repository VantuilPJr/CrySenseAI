# CrySense AI — implementação ESP32

**Protótipo de monitoramento infantil com firmware embarcado, painel web e classificação de áudio.**

Esta implementação combina firmware para ESP32, uma interface React e componentes Python para classificação remota. O objetivo é explorar captura de áudio, inferência embarcada, sensores e comunicação entre dispositivo e aplicação.

**Stack:** Arduino/C++ · ESP32 · modelo Edge Impulse · React/Vite · Python · FastAPI · scikit-learn

## Componentes do repositório

| Caminho | Conteúdo |
| --- | --- |
| `crySence/crySence.ino` | Inicialização e loop principal do firmware |
| `crySence/*.h` | Áudio, configuração, display, sensores, histórico, rede e classificação remota |
| `crySence/data/` | Arquivos utilizados pela interface servida no dispositivo |
| `crySence/frontend/react-app/` | Código da interface React/Vite |
| `crySence/notebook_classifier/` | Componentes Python, modelos e materiais de classificação |
| `crySence/docs/` | Materiais técnicos e acadêmicos |

## Interface web

```bash
git clone https://github.com/VantuilPJr/CrySenseAI.git
cd CrySenseAI/crySence/frontend/react-app
npm install
npm run dev
```

O servidor de desenvolvimento inicia a interface. A comunicação com o dispositivo depende do firmware, da rede e das configurações correspondentes. Para gerar o build:

```bash
npm run build
```

## Firmware e classificação

Abra `crySence/crySence.ino` em um ambiente Arduino compatível com ESP32. O firmware referencia a biblioteca de inferência `CrySense_trigger_inferencing` e um caminho absoluto do ambiente original de desenvolvimento; ajuste a instalação dessa biblioteca antes de compilar. Confira também os módulos de hardware e forneça suas próprias configurações locais em `secrets.h`, sem publicá-las.

As dependências Python estão em `crySence/notebook_classifier/requirements.txt`. Use um ambiente virtual próprio e consulte os arquivos desse diretório para o fluxo de treinamento e execução correspondente. A presença dos componentes não significa que todas as integrações funcionem sem configuração adicional.

## Relação com CrySense

O repositório [CrySense](https://github.com/VantuilPJr/CrySense) apresenta outra implementação, baseada em Raspberry Pi, com aplicativo Android e documentação própria. Não misture as instruções de hardware dos dois projetos.

## Estado do projeto

Protótipo experimental. As classificações de áudio não realizam diagnóstico médico, não comprovam a causa do choro e não substituem supervisão humana. O repositório não apresenta uma validação clínica ou um ambiente de firmware integralmente reproduzível com dependências fixadas.

## English summary

This CrySense implementation explores an ESP32-based baby-monitoring prototype, combining Arduino/C++ firmware, Edge Impulse inference, a React/Vite dashboard and Python/FastAPI classification components. It requires hardware, model-library and network configuration. It is separate from the Raspberry Pi implementation in the CrySense repository and does not claim medical or clinical validation.
