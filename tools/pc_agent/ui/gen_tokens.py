"""Gera tokens.css a partir de src/theme.h (TH_TOKENS): a janela usa as MESMAS cores do aparelho.

Rode de novo quando o tema do aparelho mudar:  python tools/pc_agent/ui/gen_tokens.py
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
THEME_H = os.path.join(HERE, "..", "..", "..", "src", "theme.h")

text = open(THEME_H, encoding="utf-8").read()
tokens = re.findall(r"X\((\w+),\s*0x([0-9A-Fa-f]{6}),\s*0x([0-9A-Fa-f]{6})\)", text)
if not tokens:
    raise SystemExit("TH_TOKENS nao encontrado em src/theme.h")


def block(idx):
    return "\n".join(f"  --{n.lower().replace('_', '-')}: #{v[idx].lower()};" for n, *v in
                     [(t[0], t[1], t[2]) for t in tokens])


dark = "\n".join(f"  --{n.lower().replace('_', '-')}: #{d.lower()};" for n, d, l in tokens)
light = "\n".join(f"  --{n.lower().replace('_', '-')}: #{l.lower()};" for n, d, l in tokens)
css = f"""/* Gerado por gen_tokens.py a partir de src/theme.h. Nao edite a mao. */
:root {{
  color-scheme: dark;
{dark}
}}
@media (prefers-color-scheme: light) {{
  :root:not([data-theme="dark"]) {{
    color-scheme: light;
{light}
  }}
}}
:root[data-theme="light"] {{
  color-scheme: light;
{light}
}}
"""
open(os.path.join(HERE, "tokens.css"), "w", encoding="utf-8").write(css)
print(f"{len(tokens)} tokens -> ui/tokens.css")
