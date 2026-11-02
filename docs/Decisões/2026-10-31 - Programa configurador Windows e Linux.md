---
tipo: decisão
data: 2026-10-31
status: aceita (fases 1 e 1b implementadas, falta testar na placa)
tags: [tema/configurador, tema/agente-pc, tema/firmware, tema/seguranca]
---

# Programa configurador para Windows e Linux

## Contexto

O primeiro contato com as APIs é chato: cada serviço tem um script próprio em
`tools/`, e o agente do PC é instalado à mão. O pedido é um programa de
configuração típico de Windows e Linux que:

- configure tudo o que hoje se faz pela placa ou pelos scripts: Wi-Fi, Claude,
  Codex, Antigravity, Spotify, PCs pareados, brilho, páginas;
- funcione pela rede ou pelo cabo USB, **sem BOOT/RST**, no máximo com um
  "Reiniciar" na própria interface;
- ative o monitoramento do PC com a maior compatibilidade de peças possível;
- funcione com **mais de um PC**: por exemplo um PC ligado por cabo e um
  notebook na rede.

**Restrição:** o programa **complementa** a configuração feita na placa e não a
substitui. Tudo continua possível pela tela e pelo portal da placa.

A pesquisa que embasa esta decisão está em
[[2026-10-31 - Projetos parecidos e sensores do PC]].

## Decisão

Um programa só, "Painel CYD", que substitui o `tools/pc_agent/agent.py`. Ele
fica em segundo plano (métricas e atalhos) e abre uma janela de configuração
quando pedido.

### 1. Firmware: um núcleo único de configuração

Hoje a tela, o portal de contas (`src/account_portal_esp32.cpp`) e o
pareamento gravam a NVS cada um a seu modo. A decisão é juntar tudo num único
conjunto de comandos (Wi-Fi, contas, Spotify, PCs, ajustes, reiniciar), usado
por três caminhos:

| Caminho | Como | Autorização |
| --- | --- | --- |
| Tela da placa | continua igual | toque |
| Cabo USB | linhas JSON na mesma UART dos logs, cada uma com o prefixo `@PC `; a porta abre sem reset (como `tools/serial_log.py` já faz) | PIN de 6 dígitos na primeira vez de cada PC, depois um token |
| Rede | HTTP na placa | o mesmo PIN e o mesmo token (fase 1b) |

As mudanças valem na hora. Quando for preciso reiniciar, `esp_restart()` é
chamado pelo botão do programa ou da placa.

### 2. Transporte das métricas e dos atalhos

As métricas e os atalhos do PC podem chegar por dois caminhos:

- **USB:** o programa do PC detecta a placa, autentica e **envia** as métricas a
  cada 2 s. A placa considera o USB vivo enquanto chegarem métricas válidas; ela
  o dá como perdido depois de **6 s de silêncio** (sem handshake de
  manutenção, como no cydrust). O toque num atalho volta ao PC como um evento
  `{"evt":"action","idx":N}` com o número do atalho; o PC procura a ação na
  própria configuração.
- **Wi-Fi:** como hoje. A placa consulta o agente do PC por HTTP, com token por
  painel, e usa mDNS para reencontrar o PC quando o IP muda.

A placa guarda até 3 PCs pareados por Wi-Fi (`PC_MAX_AGENTS`) e mostra **um PC
por vez**.

### 3. Qual PC aparece (ordem de prioridade)

1. **PC fixado pelo usuário:** vale sempre, esteja conectado ou não. Se estiver
   offline, a placa mostra "PC offline" e **não** troca de PC.
2. **Sem PC fixado, PC por USB:** vale o PC que estiver enviando métricas pelo
   cabo.
3. **Sem USB:** vale o primeiro PC do Wi-Fi que respondeu, e a placa fica nele
   (sem alternar à toa). Se ele cair, passa ao próximo pareado, como hoje.
4. **"Auto"** desfaz a fixação e volta à cadeia acima.

Escolha e fixação são feitas na tela PC da placa (engrenagem > "Escolher PC") e
no programa. Se o PC do cabo também estiver pareado por Wi-Fi, a placa os trata
como um só, pelo nome. A fixação fica na NVS.

### 4. Autorização: PIN de 6 dígitos

O USB deixa de ser "acesso físico basta". O modelo é parecido com o de parear um
relógio com um aparelho novo:

1. **Placa sem PIN (de fábrica):** o primeiro programa que conectar pede para
   criar o PIN. Só vale por USB, onde há acesso físico; a rede não cria PIN.
2. **PC novo com a placa já configurada:** o programa pede o PIN. Se estiver
   certo, a placa entrega um **token só daquele PC**, que o programa guarda no
   perfil do usuário. O PIN é pedido uma vez por PC.
3. **Esqueceu o PIN:** a única saída é a **redefinição completa**: apaga PIN,
   Wi-Fi e todos os tokens de conta. Ela existe na tela da placa (Ajustes), com
   dois toques de confirmação, e por USB com o PIN.
4. **Revogar um PC:** o programa lista os PCs autorizados e remove um sem apagar
   o resto.

Cuidados na implementação:

- A placa guarda **só o hash do PIN** (SHA-256 iterado, com sal) e só o hash de
  cada token.
- Seis dígitos são 1 milhão de combinações, então a placa **limita as
  tentativas**: o contador de falhas fica na NVS e sobrevive a reinício; a
  partir da 5ª falha vem um bloqueio crescente (30 s, 1 min, 2 min... até 1 h).
- **Não apaga tudo sozinha** depois de N erros: quem estivesse na rede poderia
  apagar as contas de propósito. O bloqueio longo basta.
- **Trocar o PIN só apagando tudo.** Se a tela deixasse definir um PIN novo sem
  apagar, quem pegasse a placa trocaria o PIN e manteria os tokens.
- Comandos de configuração só valem numa sessão autenticada. A sessão USB expira
  depois de 60 s sem tráfego.
- A redefinição completa pede o PIN de novo, mesmo com a sessão autenticada.

**O que o PIN não cobre:** a NVS não é criptografada. Quem ler a flash em modo
download lê os tokens, com PIN ou sem. O PIN protege os caminhos USB e rede.
Proteger a flash exigiria criptografia de flash e de NVS, que é irreversível e
fica como melhoria futura. A segurança do projeto é relativa e isso foi aceito.

### 5. Contas

- **Claude:** o programa roda `claude setup-token`, captura o token e o envia
  para a placa sem exibi-lo. Esse token é separado do login do CLI.
- **Codex:** a placa continua fazendo o login por código de dispositivo; o
  programa mostra o código e abre o navegador. **Nunca copiar
  `~/.codex/auth.json`**: o refresh token gira, e usá-lo em dois lugares
  desloga o CLI.
- **Antigravity e Spotify:** os fluxos de `tools/antigravity_creds.py`,
  `tools/send_antigravity.py` e `tools/spotify_login.py` viram botões.

### 6. Monitoramento

O agente envia só os campos que existem naquele PC, e a placa esconde o que
falta.

- **Windows:**
  - psutil: CPU, RAM, disco e rede.
  - NVML (pynvml) no lugar do `nvidia-smi`.
  - Contadores "GPU Engine" do Windows para GPUs AMD e Intel.
  - Temperatura da CPU **opcional**, pelo `data.json` do LibreHardwareMonitor
    0.9.6 ou mais recente, com PawnIO. Nunca usar o WinRing0.
- **Linux:**
  - psutil.
  - `/sys/class/hwmon` (`k10temp`, `coretemp`, `zenpower`).
  - `amdgpu` (`gpu_busy_percent`) e NVML.
  - Bloqueio por `loginctl lock-session` e mídia por MPRIS.
  - O USB exige o usuário no grupo `dialout`: o instalador configura uma regra
    udev ou avisa.
- **Inicialização com o sistema:** opcional e desligada por padrão.
  - Windows: valor por usuário em `HKCU\...\Run` (sem administrador), ligado pela chave
    "Iniciar com o sistema" do programa (`tools/pc_agent/autostart.py`).
  - Linux: `~/.config/autostart/painel-cyd.desktop` (a bandeja precisa da sessão gráfica,
    então não serve um serviço `systemd --user`).

### 7. Tecnologia

Recomendado: **Python com pywebview** (WebView2 no Windows, GTK ou Qt no
Linux) e ícone na bandeja (pystray).

- Reaproveita o agente e os scripts.
- O mesmo código roda nos dois sistemas.
- Distribuição:
  - Windows: instalador por usuário, sem administrador.
  - Linux: AppImage.

Alternativa: **Tauri** (Rust), como no esp32-ai-monitor. É mais leve, mas o
agente precisaria ser reescrito ou rodar como processo à parte.

### Fases

1. **Núcleo no firmware e protocolo USB** (em implementação):
   - PIN, tokens por PC e bloqueio de tentativas;
   - protocolo USB com prefixo `@PC `;
   - fonte USB de métricas e atalhos, fixação de PC e cadeia de prioridade;
   - tela "Escolher PC" e "Redefinir tudo" na placa;
   - lado do PC: ponte USB no agente e comandos de linha (`setup`, `wifi`,
     `select`, `restart`, `reset`, `clients`).
   - **1b (feita):** a janela do programa (pywebview no Windows, navegador mais bandeja no
     Linux), com o visual do aparelho. **Adiado:** HTTP de configuração na placa com o mesmo PIN e
     token.
2. Fluxos das quatro contas no programa.
3. Monitoramento no Windows e no Linux, atalhos e inicialização com o sistema.
4. Atualização de firmware pela rede (marco 8). A partição `min_spiffs` já tem
   `ota_0` e `ota_1`.

## Consequências e limitações

- A **primeira gravação** do firmware continua exigindo BOOT, porque a placa não
  tem auto-reset. Depois da fase 4, as atualizações vão pela rede.
- A UART passa a carregar logs e comandos juntos. O protocolo usa o prefixo
  `@PC ` e ignora qualquer linha sem ele; o PC faz o mesmo com as respostas.
  O cydrust reconhece o ACK por substring e confunde linhas de log, por isso
  aqui só vale a linha que **começa** com o prefixo.
- A memória continua limitada: uma conexão TLS por vez, já que cada uma custa
  cerca de 42 KB. O servidor HTTP de configuração não pode competir com ela.
- A placa pareada por Wi-Fi deixa de consultar o PC por HTTP enquanto o USB
  está vivo, o que poupa rede e memória.
- Quem só tem o PC no cabo (fora da rede) não precisa de Wi-Fi para as métricas.

## Pendente

- Linux: a janela e os atalhos não foram testados (só Windows); o pywebview no Linux exige GTK ou Qt.
- O programa mostra só a tela de configuração; os fluxos das contas (Claude, Codex, Antigravity,
  Spotify) continuam nos scripts de `tools/` (fase 2).
- Decidir se o PC fixado, quando offline, pode ceder ao restante da cadeia
  (hoje: não cede).
