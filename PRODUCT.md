# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

Nota: o valor `web` é o mais próximo aceito pelo schema. A superfície principal é a tela da placa (firmware C++ com LVGL 8.3, 320x240, toque resistivo, sem PSRAM); a interface do programa do PC é HTML/CSS e compartilha os tokens de cor do aparelho (`src/theme.h` → `ui/tokens.css`).

## Users
Hoje, o autor, que usa o painel na mesa ao lado do PC. A intenção é que outras pessoas também montem e usem (público ainda não detalhado). O uso é de relance: olhar o painel, tocar um atalho, ajustar brilho ou tema.

## Product Purpose
Painel de bolso (ESP32-2432S028, "Cheap Yellow Display" 2,8") que mostra o uso de assistentes de IA (Claude, Codex, Antigravity), o Spotify, o status do PC e atalhos do computador. Sucesso: informação útil de relance e atalhos instantâneos, sem abrir o PC.

## Positioning
Hardware de baixo custo (ESP32 comum) dedicado ao uso de IA, com o resto (Spotify, PC, atalhos) numa tela só, sem nuvem própria. Um Stream Deck ou monitor de hardware genérico não mostra cotas de uso de IA.

## Operating Context
- Tela 320x240 em paisagem, toque resistivo, navegação por gestos entre páginas; gesto para baixo abre ajustes rápidos.
- Wi-Fi com portal e QR; USB como segundo canal para o agente do PC.
- Programa do PC (Windows e Linux) para configuração e atalhos, com ícone de bandeja.
- Memória muito limitada (pool do LVGL de 60 KB, TLS precisa de ~42 KB de heap): telas pesadas checam memória antes de abrir.
- Tema escuro e claro, texto em português (sem acentos nas strings do firmware).

## Capabilities and Constraints
- Páginas: uso de IA (junta ou uma por IA), Spotify, PC, Relógio (4 estilos), atalhos.
- Configurações em três categorias (Contas, Tela, Sistema). Componentes novos usam os estilos compartilhados de `src/theme.cpp`; cores novas entram na lista `TH_TOKENS`.
- Simulador no PC cobre layout, navegação e textos, mas não velocidade, SPI nem toque resistivo.
- Público além do autor, preço e distribuição: indecisos.

## Evidence on Hand
Firmware funcionando na placa (marcos 1 a 7), simulador com capturas em `sim_shots/`, documentação em `docs/`. Não há depoimentos, clientes nem métricas de uso de terceiros.

## Product Principles
1. Legível de relance: o essencial primeiro, em tela pequena.
2. Cabe na memória: o design respeita o orçamento de RAM da placa.
3. Um sistema de tokens para placa e programa do PC.
4. Funciona sem contas ou nuvem própria, com estado claro quando algo falha.
