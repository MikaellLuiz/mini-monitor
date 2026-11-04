---
tipo: resumo
data: 2026-11-04
projetos: [mini-monitor]
tags: []
---

# Redesenho da interface, unificação de IA e limpeza de telemetria

Sessão de refinamento da interface visual do [[Mini-monitor]] (commit `887c9ab`).

## O que foi feito

- **Redesenho das páginas da UI (`src/usage_ui.cpp`, `src/clock_ui.cpp`, `src/pc_ui.cpp`, `src/spotify_ui.cpp`, `src/settings_ui.cpp`)**:
  - Unificação da visualização de IA (`5 paginas, IA junta`) com cartões redesenhados e melhor hierarquia visual.
  - Novos componentes e ajustes no seletor de temas e configurações (`src/theme.cpp`, `src/theme.h`).
  - Atualização dos tokens no agente PC (`tools/pc_agent/ui/tokens.css`) e simulador desktop (`sim/`).
- **Limpeza de telemetria**:
  - Remoção da flag `-DUI_PERF` de `platformio.ini`, eliminando a sobreposição de FPS na tela para manter o display limpo.
- **Validação de estabilidade em hardware**:
  - Interface verificada no CYD: uso de memória LVGL de ~33% a 48% (com ~32–41 KB livres de 64 KB).
  - Sem impacto na estabilidade de TLS (`reserva=2` preservada continuamente, heap livre ~61 KB).

## Por quê

O firmware estava estável após a correção da alocação TLS (`c7367c3`), permitindo consolidar uma experiência de interface mais polida e sem poluição visual.

## Pendente

- Enviar os commits locais para o GitHub (`git push`).
