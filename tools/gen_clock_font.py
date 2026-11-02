"""Gera src/clock_font.c: fontes LVGL so com "0123456789:" para os numeros grandes do Relogio.

As fontes prontas do LVGL vao ate 48 px. Estas usam a mesma Montserrat Medium do LVGL,
em 4 bpp, e ocupam poucos KB (so 11 glifos). Digitos com largura fixa: a hora nao
"anda" quando muda de 1 para 8.

Uso: .venv\\Scripts\\python tools\\gen_clock_font.py
Precisa do Pillow e de um `pio run` antes (a fonte vem de .pio/libdeps/cyd/lvgl).
"""
import os
import sys

from PIL import ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TTF = os.path.join(ROOT, ".pio", "libdeps", "cyd", "lvgl", "scripts", "built_in_font", "Montserrat-Medium.ttf")
OUT = os.path.join(ROOT, "src", "clock_font.c")
CHARS = "0123456789:"
FONTS = [("font_clock_big", 80), ("font_clock_huge", 100)]


def build(name, size):
    font = ImageFont.truetype(TTF, size)
    ascent, descent = font.getmetrics()
    digit_adv = max(font.getlength(c) for c in "0123456789")
    glyphs, bitmap = [], bytearray()
    nibbles = []
    for ch in CHARS:
        left, top, right, bottom = font.getbbox(ch, anchor="ls")  # origem: base da linha, a esquerda
        w, h = right - left, bottom - top
        mask = font.getmask(ch, mode="L")  # coberturas 0-255 do glifo, sem a margem
        # getmask devolve so a caixa do glifo, na ordem das linhas
        px = list(mask)
        start = len(nibbles)
        for v in px[: w * h]:
            nibbles.append(min(15, (v + 8) // 17))
        adv = digit_adv if ch.isdigit() else font.getlength(ch)
        # digitos tabulares: cada um centrado na largura fixa (o "1" nao fica colado a esquerda)
        ox = round((digit_adv - w) / 2) if ch.isdigit() else left
        glyphs.append((start // 2, round(adv * 16), w, h, ox, -bottom, start))
        if len(nibbles) % 2:  # cada glifo comeca num byte
            nibbles.append(0)
    for i in range(0, len(nibbles), 2):
        bitmap.append((nibbles[i] << 4) | nibbles[i + 1])
    # bitmap_index precisa ser em bytes: recalcula com o preenchimento
    pos, fixed = 0, []
    for ch, (_, adv, w, h, ox, oy, _) in zip(CHARS, glyphs):
        fixed.append((pos, adv, w, h, ox, oy))
        pos += (w * h + 1) // 2
    return name, size, ascent, descent, fixed, bitmap


def emit(f, name, size, ascent, descent, glyphs, bitmap):
    f.write(f"\n/* ---- {name}: Montserrat Medium {size} px, so \"{CHARS}\" ---- */\n")
    f.write(f"static const uint8_t {name}_bitmap[] = {{\n")
    for i in range(0, len(bitmap), 24):
        f.write("    " + ", ".join(f"0x{b:02x}" for b in bitmap[i : i + 24]) + ",\n")
    f.write("};\n")
    f.write(f"static const lv_font_fmt_txt_glyph_dsc_t {name}_glyphs[] = {{\n")
    f.write("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},\n")
    for idx, adv, w, h, ox, oy in glyphs:
        f.write(f"    {{.bitmap_index = {idx}, .adv_w = {adv}, .box_w = {w}, .box_h = {h}, .ofs_x = {ox}, .ofs_y = {oy}}},\n")
    f.write("};\n")
    f.write(f"static const lv_font_fmt_txt_cmap_t {name}_cmaps[] = {{\n")
    f.write("    {.range_start = 48, .range_length = 11, .glyph_id_start = 1, .unicode_list = NULL,\n")
    f.write("     .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},\n};\n")
    f.write(f"static lv_font_fmt_txt_glyph_cache_t {name}_cache;\n")
    f.write(f"static const lv_font_fmt_txt_dsc_t {name}_dsc = {{\n")
    f.write(f"    .glyph_bitmap = {name}_bitmap, .glyph_dsc = {name}_glyphs, .cmaps = {name}_cmaps,\n")
    f.write("    .kern_dsc = NULL, .kern_scale = 0, .cmap_num = 1, .bpp = 4, .kern_classes = 0,\n")
    f.write(f"    .bitmap_format = 0, .cache = &{name}_cache}};\n")
    f.write(f"const lv_font_t {name} = {{\n")
    f.write("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n")
    f.write("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n")
    f.write(f"    .line_height = {ascent + descent},\n    .base_line = {descent},\n")
    f.write("    .subpx = LV_FONT_SUBPX_NONE,\n    .underline_position = -4,\n    .underline_thickness = 2,\n")
    f.write(f"    .dsc = &{name}_dsc}};\n")


def main():
    if not os.path.exists(TTF):
        sys.exit("Montserrat-Medium.ttf nao encontrado: rode `pio run` uma vez antes.")
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("// GERADO por tools/gen_clock_font.py - nao edite a mao.\n")
        f.write('#include <lvgl.h>\n\n#include "clock_font.h"\n')
        total = 0
        for name, size in FONTS:
            r = build(name, size)
            emit(f, *r)
            total += len(r[5])
    print(f"ok: {os.path.relpath(OUT, ROOT)} ({total} bytes de bitmap)")


if __name__ == "__main__":
    main()
