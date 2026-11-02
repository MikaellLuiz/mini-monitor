"use strict";
// Interface do Painel CYD. A pagina e servida pelo proprio programa em 127.0.0.1 (janela pywebview ou
// navegador) e fala com ele por POST em /api/<metodo>, com o token desta execucao (meta "token").

const TOKEN = (document.querySelector('meta[name="token"]') || {}).content || "";

async function api(name, ...args) {
  const r = await fetch("/api/" + name, {
    method: "POST", headers: { "Content-Type": "application/json", "X-Token": TOKEN }, body: JSON.stringify(args),
  });
  return await r.json();
}

// ---- utilitarios de DOM (sem innerHTML: nomes de PC vem da rede/placa) ------------------------

function h(tag, props, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (k === "class") el.className = v;
    else if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
    else if (v === true) el.setAttribute(k, "");
    else if (v !== false && v != null) el.setAttribute(k, v);
  }
  for (const kid of kids.flat()) if (kid != null && kid !== false) el.append(kid.nodeType ? kid : document.createTextNode(kid));
  return el;
}

const ICONS = {
  board: '<rect x="6" y="6" width="12" height="12" rx="2"/><path d="M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4"/>',
  wifi: '<path d="M2 9a15 15 0 0 1 20 0M5.5 12.5a10 10 0 0 1 13 0M9 16a5 5 0 0 1 6 0"/><circle cx="12" cy="19.5" r="1"/>',
  pc: '<rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8 20h8M12 16v4"/>',
  usb: '<path d="M12 3v14M12 17a3 3 0 1 0 0 .01M12 8l-4 3v3M12 11l4-2v3"/>',
  bolt: '<path d="M13 2 4 14h7l-1 8 9-12h-7z"/>',
  power: '<path d="M12 3v9M6.4 6.4a8 8 0 1 0 11.2 0"/>',
  auto: '<path d="M4 7h12l-3-3M20 17H8l3 3"/>',
  user: '<circle cx="12" cy="8" r="4"/><path d="M4 21a8 8 0 0 1 16 0"/>',
};
function icon(name) {
  const d = h("span", { class: "ic" });
  d.innerHTML = '<svg viewBox="0 0 24 24">' + ICONS[name] + "</svg>"; // so os desenhos fixos acima
  return d;
}

function row(ic, title, sub, trail, onclick) {
  return h("div", { class: "row" + (onclick ? " click" : ""), onclick },
    ic ? icon(ic) : null,
    h("div", { class: "txt" }, h("div", { class: "t" }, title), sub ? h("div", { class: "s" }, sub) : null),
    trail);
}

function toast(msg, err) {
  const t = document.getElementById("toast");
  t.textContent = msg;
  t.className = "toast show" + (err ? " err" : "");
  clearTimeout(toast.t);
  toast.t = setTimeout(() => (t.className = "toast"), 2600);
}

// Roda uma chamada, mostra o resultado e atualiza o estado.
async function act(promise, okMsg) {
  const r = await promise;
  if (r && r.ok) { if (okMsg) toast(okMsg); }
  else toast((r && r.msg) || "Nao deu certo.", true);
  await refresh(true);
  return r;
}

// ---- estado -------------------------------------------------------------------------------------

let S = null, tab = "placa", lastSig = "", pending = false;
let draft = null;       // atalhos em edicao (so enviados ao Salvar)
let armed = {};         // botoes de dois toques
const TABS = [["placa", "Placa"], ["pcs", "PCs"], ["atalhos", "Atalhos"], ["sistema", "Sistema"]];

async function refresh(force) {
  try { S = await api("state"); } catch (e) { return; }
  const sig = JSON.stringify([S, tab]);
  const typing = document.activeElement && ["INPUT", "SELECT"].includes(document.activeElement.tagName);
  if (!force && sig === lastSig) return;
  if (!force && typing) { pending = true; return; }   // nao tira o foco de quem digita
  lastSig = sig; pending = false;
  render();
}
document.addEventListener("focusout", () => { if (pending) setTimeout(() => refresh(true), 50); });

function chip() {
  const dot = document.querySelector("#chip .dot"), text = document.getElementById("chipText");
  const u = S.usb.state;
  const m = {
    autenticado: ["ok", "Placa conectada"], sem_placa: ["bad", "Sem placa"], precisa_pin: ["warn", "Falta autorizar"],
    precisa_criar_pin: ["warn", "Falta criar o PIN"], bloqueada: ["bad", "Placa bloqueada"], desligado: ["", "USB desligado"],
  }[u] || ["", u];
  dot.className = "dot " + m[0];
  text.textContent = m[1];
}

function render() {
  chip();
  const tabs = document.getElementById("tabs");
  tabs.replaceChildren(...TABS.map(([id, label]) =>
    h("button", { class: "tab" + (tab === id ? " on" : ""), onclick: () => { tab = id; draft = null; lastSig = ""; if (S) render(); refresh(true); } }, label)));
  const view = document.getElementById("view");
  view.replaceChildren(...({ placa: viewPlaca, pcs: viewPcs, atalhos: viewAtalhos, sistema: viewSistema }[tab])());
}

// ---- Placa --------------------------------------------------------------------------------------

function pinField(id, ph) {
  return h("input", { class: "field pin", id, type: "password", inputmode: "numeric", maxlength: "6", placeholder: ph || "••••••",
    autocomplete: "off" });
}
const val = (id) => document.getElementById(id).value;

function viewPlaca() {
  const u = S.usb, out = [];
  if (u.state === "desligado" || u.state === "sem_placa") {
    out.push(h("div", { class: "card" }, h("h2", {}, "Nenhuma placa encontrada"),
      h("p", { class: "muted" }, "Conecte a placa pelo cabo USB. Feche programas que usem a porta serial (monitor serial, gravador de firmware)."),
      h("div", { class: "btns" }, h("button", { class: "btn", onclick: () => refresh(true) }, "Procurar de novo"))));
  } else if (u.state === "precisa_criar_pin") {
    out.push(h("div", { class: "card" }, h("h2", {}, "Crie o PIN da placa"),
      h("p", { class: "muted" }, "Seis digitos. Ele autoriza cada PC novo; guarde-o: se esquecer, so a redefinicao completa (que apaga Wi-Fi, contas e PCs) deixa criar outro."),
      h("label", { class: "l" }, "Novo PIN"), pinField("pin1"),
      h("label", { class: "l" }, "Repita o PIN"), pinField("pin2"),
      h("div", { class: "btns" }, h("button", { class: "btn primary", onclick: async () => {
        if (val("pin1") !== val("pin2")) return toast("Os dois PINs precisam ser iguais.", true);
        await act(api("create_pin", val("pin1")), "PC autorizado");
      } }, "Criar PIN e autorizar este PC"))));
  } else if (u.state === "precisa_pin") {
    out.push(h("div", { class: "card" }, h("h2", {}, "Autorizar este PC"),
      h("p", { class: "muted" }, "Digite o PIN da placa. Ele so e pedido uma vez neste PC."),
      pinField("pin"),
      h("div", { class: "btns" }, h("button", { class: "btn primary", onclick: () => act(api("login", val("pin")), "PC autorizado") }, "Autorizar"))));
  } else if (u.state === "bloqueada") {
    out.push(h("div", { class: "card" }, h("h2", {}, "Placa bloqueada"),
      h("p", { class: "muted" }, `Muitas tentativas erradas. Espere ${u.retry_s || "alguns"} s e tente de novo.`),
      h("div", { class: "btns" }, h("button", { class: "btn", onclick: () => refresh(true) }, "Tentar de novo"))));
  }
  if (!S.board) return out;

  const b = S.board, w = b.wifi || {};
  out.push(h("div", { class: "list" },
    row("board", "Firmware " + b.fw, `Conectada em ${u.port}`),
    row("wifi", w.state === "connected" ? w.ssid : "Wi-Fi " + (w.state || ""),
      w.state === "connected" ? `${w.ip} • ${w.rssi} dBm • ${w.saved} redes salvas` : `${w.saved || 0} redes salvas`)));

  // Brilho (barra em pilula como no aparelho)
  const pct = Math.round(b.bright * 100 / 255);
  const fill = h("div", { class: "fill", style: `width:${pct}%` }), num = h("span", {}, pct + "%");
  const slider = h("input", { type: "range", min: 10, max: 255, value: b.bright,
    oninput: (e) => { const p = Math.round(e.target.value * 100 / 255); fill.style.width = p + "%"; num.textContent = p + "%"; },
    onchange: (e) => act(api("brightness", +e.target.value)) });
  out.push(h("div", { class: "bright" }, fill, h("div", { class: "lbl" }, h("span", {}, "Brilho"), num), slider));

  out.push(h("div", { class: "card" }, h("h2", {}, "Adicionar rede Wi-Fi"),
    h("p", { class: "muted" }, "A rede entra no topo da lista da placa e ela tenta conectar."),
    h("div", { class: "grid2" }, h("input", { class: "field", id: "ssid", placeholder: "Nome da rede", maxlength: 32 }),
      h("input", { class: "field", id: "wpass", type: "password", placeholder: "Senha (8 a 63)", maxlength: 63 })),
    h("div", { class: "btns" },
      h("button", { class: "btn", onclick: async () => {
        const r = await act(api("wifi_set", val("ssid"), val("wpass")), "Rede salva; a placa vai conectar");
      } }, "Salvar rede"),
      h("button", { class: "btn mini danger", onclick: (e) => twoTap(e.target, "restart", "Reiniciar a placa", async () => act(api("restart"), "Reiniciando...")) }, "Reiniciar a placa"))));
  return out;
}

// Dois toques em 3 s para o que nao tem volta (como na tela do aparelho).
function twoTap(btn, key, label, fn) {
  if (armed[key]) { clearTimeout(armed[key]); armed[key] = 0; btn.textContent = label; return fn(); }
  btn.textContent = "Toque de novo";
  armed[key] = setTimeout(() => { armed[key] = 0; btn.textContent = label; }, 3000);
}

// ---- PCs ----------------------------------------------------------------------------------------

function viewPcs() {
  const out = [], b = S.board;
  if (!b) {
    out.push(h("div", { class: "card" }, h("h2", {}, "Autorize este PC primeiro"),
      h("p", { class: "muted" }, "Na aba Placa, conecte a placa e digite o PIN. Depois voce escolhe aqui qual PC ela mostra.")));
  } else {
    const src = { usb: "USB", wifi: "Wi-Fi", none: "nenhuma" }[b.source] || b.source;
    out.push(h("div", { class: "list" }, row("pc", b.active || "Nenhum PC", `Mostrado agora • via ${src} • ${b.online ? "online" : "offline"}`,
      b.pinned ? h("span", { class: "muted" }, "fixado") : h("span", { class: "muted" }, "automatico"))));
    out.push(h("h2", { class: "muted", style: "margin:14px 4px 6px" }, "ESCOLHER PC"));
    const items = [row("auto", "Automatico", "USB e depois o primeiro do Wi-Fi", b.pinned ? null : h("span", { class: "check" }, "✓"),
      () => act(api("select_pc", "auto"), "Automatico"))];
    for (const p of b.pcs || []) {
      const via = [p.usb ? "USB" : "", p.wifi ? "Wi-Fi" : ""].filter(Boolean).join(" e ") || "fora de alcance";
      items.push(row(p.usb ? "usb" : "wifi", p.name, via, b.pinned === p.name ? h("span", { class: "check" }, "✓") : null,
        () => act(api("select_pc", p.name), "PC fixado: " + p.name)));
    }
    out.push(h("div", { class: "list" }, items));
    out.push(h("p", { class: "muted", style: "margin:0 4px 14px" }, "Um PC fixado vale mesmo desligado. Automatico: o do USB, depois o primeiro do Wi-Fi."));

    out.push(h("h2", { class: "muted", style: "margin:14px 4px 6px" }, "PCS AUTORIZADOS NA PLACA (USB)"));
    out.push(h("div", { class: "list" }, (b.clients || []).map((n) =>
      row("user", n, n === S.app.name ? "este PC" : null,
        h("button", { class: "btn mini danger", onclick: (e) => twoTap(e.target, "rv" + n, "Remover", () => act(api("revoke", n), "PC removido")) }, "Remover")))));
  }

  out.push(h("h2", { class: "muted", style: "margin:14px 4px 6px" }, "PAREAMENTO POR WI-FI"));
  const boards = S.wifi_boards.map((ip) => row("board", "Placa em " + ip, null,
    h("button", { class: "btn mini danger", onclick: (e) => twoTap(e.target, "fb" + ip, "Remover", () => act(api("forget_wifi_board", ip), "Pareamento removido")) }, "Remover")));
  if (boards.length) out.push(h("div", { class: "list" }, boards));
  out.push(h("div", { class: "card" },
    h("p", { class: "muted" }, "Para a placa consultar este PC pelo Wi-Fi: na placa, va em PC > engrenagem > Parear um PC e digite aqui o codigo da tela."),
    h("div", { class: "grid2" }, h("input", { class: "field", id: "paircode", inputmode: "numeric", maxlength: 6, placeholder: "Codigo de 6 digitos" }),
      h("button", { class: "btn", onclick: () => act(api("pair_wifi", val("paircode")), "Pareado") }, "Parear"))));
  return out;
}

// ---- Atalhos ------------------------------------------------------------------------------------

function viewAtalhos() {
  if (!draft) draft = S.shortcuts.map((s) => ({ label: s.label || "", kind: s.action === "lock" ? "lock" : s.run ? "run" : "keys",
    value: s.keys || s.run || "" }));
  const rows = draft.map((d, i) => h("div", { class: "sc" },
    h("input", { class: "field", value: d.label, maxlength: 12, placeholder: "Nome", oninput: (e) => (d.label = e.target.value) }),
    h("select", { class: "field", onchange: (e) => { d.kind = e.target.value; render(); } },
      ...[["keys", "Teclas"], ["run", "Programa"], ["lock", "Bloquear"]].map(([v, t]) => h("option", { value: v, selected: d.kind === v }, t))),
    d.kind === "lock" ? h("span", { class: "muted" }, "Bloqueia a tela") :
      h("input", { class: "field", value: d.value, placeholder: d.kind === "keys" ? "ctrl+shift+esc" : "caminho do programa",
        oninput: (e) => (d.value = e.target.value) }),
    h("button", { class: "btn mini", title: "Testar neste PC", onclick: () => testShortcut(i) }, "Testar"),
    h("button", { class: "btn mini danger", onclick: () => { draft.splice(i, 1); render(); } }, "Tirar")));
  return [
    h("div", { class: "card" }, h("h2", {}, "Atalhos da placa"),
      h("p", { class: "muted" }, "A placa mostra o nome; ao tocar, so o numero do atalho vem para este PC, que executa o que esta aqui. Ate 6, nomes de ate 12 letras (sem acento aparecem melhor).")),
    h("div", { class: "list" }, rows),
    h("div", { class: "btns" },
      draft.length < 6 ? h("button", { class: "btn", onclick: () => { draft.push({ label: "", kind: "keys", value: "" }); render(); } }, "Adicionar atalho") : null,
      h("button", { class: "btn primary", onclick: saveShortcuts }, "Salvar atalhos")),
    h("p", { class: "muted", style: "margin:12px 4px" }, "Teclas aceitas: " + S.key_names.join(", ") + ", e letras/numeros. Combine com +."),
  ];
}
function toStored(d) {
  return d.kind === "lock" ? { label: d.label, action: "lock" } : d.kind === "run" ? { label: d.label, run: d.value } : { label: d.label, keys: d.value };
}
async function saveShortcuts() {
  const r = await act(api("save_shortcuts", draft.map(toStored)), "Atalhos salvos");
  if (r && r.ok) draft = null;
}
async function testShortcut(i) {   // executa o que esta na tela, sem salvar
  const t = await api("test_shortcut", toStored(draft[i]));
  toast(t.ok ? "Atalho executado" : t.msg, !t.ok);
}

// ---- Sistema ------------------------------------------------------------------------------------

function viewSistema() {
  const a = S.app, out = [];
  const sw = h("label", { class: "switch" }, h("input", { type: "checkbox", checked: a.autostart,
    onchange: (e) => act(api("set_autostart", e.target.checked), e.target.checked ? "Vai iniciar com o sistema" : "Nao inicia mais com o sistema") }), h("i"));
  out.push(h("div", { class: "list" },
    row("power", "Iniciar com o sistema", a.platform === "win32" ? "Sem administrador, so para o seu usuario" : "Entrada em ~/.config/autostart", sw),
    row("pc", "Nome deste PC", a.name, h("span", { class: "muted" }, "v" + a.version))));

  if (S.board) {
    out.push(h("h2", { class: "muted", style: "margin:14px 4px 6px" }, "REDEFINIR A PLACA"));
    out.push(h("div", { class: "card" },
      h("p", {}, "Apaga TUDO o que esta gravado na placa: PIN, redes Wi-Fi, tokens das contas (Claude, Codex, Antigravity, Spotify), PCs pareados e a calibracao do toque."),
      h("p", { class: "muted" }, "Digite REDEFINIR e o PIN. Nao tem volta."),
      h("div", { class: "grid2" }, h("input", { class: "field", id: "rconf", placeholder: "REDEFINIR" }), pinField("rpin")),
      h("div", { class: "btns" }, h("button", { class: "btn danger", onclick: async () => {
        if (val("rconf").trim() !== "REDEFINIR") return toast("Digite REDEFINIR para confirmar.", true);
        await act(api("reset_board", val("rpin")), "Placa redefinida; ela vai reiniciar");
      } }, "Redefinir tudo"))));
  }
  return out;
}

// ---- inicio -------------------------------------------------------------------------------------

function start() {
  refresh(true);
  setInterval(() => { if (!document.hidden) refresh(false); }, 2000);
}
start();
