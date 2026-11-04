# Mini Monitor (Painel CYD) 🖥️⚡

Um painel inteligente de mesa, de baixo custo e código aberto, feito com a **ESP32-2432S028** (*"Cheap Yellow Display"* / CYD de 2.8" touch).

O Mini Monitor foi desenvolvido para ficar ao lado do seu computador e fornecer informações essenciais de relance: **consumo de cotas de IA**, **controle do Spotify**, **status de hardware do PC** e **botões de atalho rápidos**, tudo sem depender de nuvens pagas ou servidores intermediários.

---

## ✨ Funcionalidades Principais

* 🤖 **Monitor de Cotas de Inteligência Artificial**:
  * Acompanhamento em tempo real dos limites das suas contas de IA:
    * **Claude (Anthropic)**: cotas da janela de 5 horas e 7 dias.
    * **Codex / ChatGPT (OpenAI)**: janelas de taxa e uso da conta.
    * **Antigravity / Gemini (Google Cloud Code)**: limites de 5 horas e semanais.
  * Suporta visualização unificada em tela única ou páginas individuais dedicadas por IA.
* 🎵 **Controle Spotify Dedicado**:
  * Exibição da faixa em reprodução, artista e capa do álbum.
  * Play / Pause, avançar e voltar músicas com resposta rápida (latência de ~500 ms via retomada de sessão TLS).
* 💻 **Status e Atalhos do PC**:
  * Mostra métricas em tempo real do computador: CPU, memória RAM, temperatura e status.
  * Grid de botões touch com atalhos executados instantaneamente na máquina (volume, controle de mídia, abrir apps, scripts ou ações personalizadas).
* ⏰ **Relógio de Mesa e Ajustes Rápidos**:
  * 4 estilos visuais configuráveis (Digital grande, Analógico, Minimalista e Anéis).
  * Painel de controle rápido com gesto de deslizar para baixo (brilho, tema claro/escuro, Wi-Fi e opções).
* 🔒 **Segurança e Comunicação Local**:
  * Funciona via **Wi-Fi** (com portal cativo e QR code para conexão inicial) ou **USB** direta.
  * Pareamento autenticado entre o PC e a placa via PIN de segurança.

---

## 🛠️ Hardware Necessário

* **Placa**: ESP32-2432S028 (CYD - Cheap Yellow Display):
  * Microcontrolador: ESP32 Dual-Core 240 MHz (4 MB Flash, sem PSRAM).
  * Tela: 2.8" TFT LCD (320x240 pixels), driver ST7789.
  * Touchscreen: Resistivo, controlador XPT2046.
  * Conector Micro-USB / Type-C (com chip serial CH340).
* **Cabo USB**: Para alimentação e comunicação de dados.

---

## 🚀 Instalação e Uso Rápido

### 1. Aplicativo Companheiro no PC (Linux e Windows)

O aplicativo companheiro gerencia o envio de métricas do sistema e responde aos atalhos configurados no painel.

* **Linux**:
  Execute o instalador na pasta do projeto:
  ```bash
  bash tools/pc_agent/install_linux.sh
  ```
  Isso adiciona o executável local em `~/.local/bin/painel-cyd` com ícone no menu do sistema e suporte a inicialização automática.

* **Windows**:
  Basta compilar ou executar o executável gerado:
  ```powershell
  python -m venv .venv
  .venv\Scripts\pip install -r tools\pc_agent\requirements.txt
  .venv\Scripts\python tools\pc_agent\build_app.py
  # O executável será gerado em dist\app\PainelCYD.exe
  ```

---

### 2. Gravando o Firmware na Placa

O firmware foi desenvolvido com [PlatformIO](https://platformio.org/).

1. **Clone o repositório e prepare o ambiente**:
   ```bash
   git clone https://github.com/MikaellLuiz/mini-monitor.git
   cd mini-monitor

   python3 -m venv .venv
   source .venv/bin/activate  # No Windows: .venv\Scripts\activate
   pip install platformio esptool
   ```

2. **Compile o firmware**:
   ```bash
   pio run
   ```

3. **Grave no dispositivo**:
   > ⚠️ **Atenção**: As placas CYD comuns não possuem circuito de auto-reset para gravação automática.
   > 
   > 1. Conecte a placa via USB.
   > 2. Pressione e **mantenha segurado o botão BOOT** na placa.
   > 3. Dê um **toque rápido no botão RST** (reset) e continue segurando o **BOOT**.
   > 4. Inicie o comando de gravação:

   ```bash
   # No Linux (/dev/ttyUSB0):
   pio run -t upload --upload-port /dev/ttyUSB0

   # No Windows (exemplo COM5):
   pio run -t upload --upload-port COM5
   ```
   *(Assim que a barra de porcentagem começar a subir, você pode soltar o botão BOOT).*

---

### 3. Configuração Inicial

1. Ao ligar pela primeira vez, a tela exibirá as instruções para configurar o Wi-Fi.
2. Conecte-se à rede Wi-Fi temporária criada pela placa (`Painel CYD`) pelo celular ou PC e insira os dados da sua rede local no portal cativo.
3. Abra o aplicativo **Painel CYD** no computador para cadastrar seu PIN e configurar suas chaves/tokens de API e contas (Spotify, Anthropic, OpenAI, etc.).
4. O painel sincronizará automaticamente e começará a exibir seus dados!

---

## 💻 Simulador de Interface (Opcional)

Você pode testar e pré-visualizar a interface visual diretamente no seu computador sem precisar da placa conectada:

```bash
# Executa a janela interativa com suporte a mouse
python tools/build_sim.py --run

# Gera capturas de tela das páginas no diretório sim_shots/
python tools/build_sim.py --script sim/scripts/tour.txt
```

---

## 📂 Arquitetura Técnica e Documentação

Para desenvolvedores e detalhes de implementação interna:
* **Framework**: Arduino-ESP32 + LVGL 8.3 + LovyanGFX.
* **Alocação e Memória TLS**: A placa opera no limite de RAM (~95 KB livres). O firmware utiliza uma reserva dedicada de buffers gerenciada direto via `mbedtls_platform_set_calloc_free` para garantir conexões HTTPS seguras sem fragmentação do heap.
* Consulte a documentação interna em [`docs/Índice.md`](docs/Índice.md) e [`docs/Contexto atual.md`](docs/Contexto%20atual.md).
