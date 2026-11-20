#!/usr/bin/env python3
# mkschemafont.py SPLEEN.bdf OUT.bdf — cut Spleen 16x32 down to the 256 glyphs of
# schema-16x32 and draw the rail glyphs Spleen lacks. 256, not 512: a 512-glyph
# console font costs the VT its bright colours (attribute bit 3 becomes a glyph bit).
import re
import sys

W, H = 16, 32


def parse(path):
    glyphs, cur, rows = {}, None, None
    for line in open(path):
        line = line.rstrip('\n')
        if line.startswith('ENCODING '):
            cur = int(line.split()[1])
        elif line == 'BITMAP':
            rows = []
        elif line == 'ENDCHAR':
            if cur is not None and rows is not None:
                glyphs[cur] = rows
            cur, rows = None, None
        elif rows is not None:
            rows.append(int(line, 16))
    return glyphs


def blank():
    return [0] * H


def setpx(g, x, y):
    if 0 <= x < W and 0 <= y < H:
        g[y] |= 1 << (W - 1 - x)


# drawn on Spleen's metrics: x-height band rows 11..25, 2 px strokes
def diamond_outline():
    g = blank()
    cx, cy, r = 7.5, 18, 6
    for y in range(H):
        for x in range(W):
            d = abs(x - cx) + abs(y - cy)
            if r - 1.5 <= d <= r + 0.5:
                setpx(g, x, y)
    return g


def square_outline():
    g = blank()
    for y in range(12, 25):
        for x in range(2, 14):
            if y in (12, 13, 23, 24) or x in (2, 3, 12, 13):
                setpx(g, x, y)
    return g


def pointer_right():
    g = blank()
    for y in range(11, 26):
        half = 7 - abs(y - 18)
        for x in range(2, 2 + 2 * half + 1):
            setpx(g, x, y)
    return g


DRAWN = {0x25C7: diamond_outline, 0x25A1: square_outline, 0x25BA: pointer_right}

# what the rail emits beyond ASCII/Latin-1 (keep in step with rail_glyphs.h)
RAIL = [0x2501, 0x2503, 0x2523, 0x2517, 0x254D, 0x25C6, 0x25C7, 0x25B2, 0x25A0, 0x25A1,
        0x25BA, 0x2026, 0x2593, 0x2592, 0x2591]
LIGHT_BOX = [0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C, 0x2534, 0x253C]
# filler up to 256, most useful first
EXTRA = [0xFFFD, 0x2588, 0x2580, 0x2584, 0x258C, 0x2590, 0x25CF, 0x25CB, 0x2190, 0x2191, 0x2192,
         0x2193, 0x2550, 0x2551, 0x2554, 0x2557, 0x255A, 0x255D, 0x2560, 0x2563, 0x2566, 0x2569,
         0x256C, 0x250F, 0x2513, 0x251B, 0x252B, 0x2533, 0x253B, 0x254B, 0x2022, 0x2018, 0x2019,
         0x201C, 0x201D, 0x2013, 0x2014, 0x20AC, 0x2122, 0x2713, 0x2717, 0x25BC, 0x25C0, 0x25B6,
         0x2665, 0x2666, 0x2663, 0x2660, 0x221E, 0x2248, 0x2260, 0x2264, 0x2265, 0x00D7]


def main():
    src, out = sys.argv[1], sys.argv[2]
    sp = parse(src)
    order = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) + LIGHT_BOX + RAIL
    chosen, glyphs = [], {}
    for cp in order + EXTRA:
        if len(chosen) == 256:
            break
        if cp in glyphs:
            continue
        if cp in sp:
            glyphs[cp] = sp[cp]
        elif cp in DRAWN:
            glyphs[cp] = DRAWN[cp]()
        else:
            if cp in order:
                sys.exit(f'mkschemafont: U+{cp:04X} needed but neither in Spleen nor drawn')
            continue
        chosen.append(cp)
    if len(chosen) != 256:
        sys.exit(f'mkschemafont: {len(chosen)} glyphs, want 256 (add EXTRA candidates)')
    head = re.split(r'^CHARS ', open(src).read(), flags=re.M)[0]
    head = head.replace('FAMILY_NAME "Spleen"', 'FAMILY_NAME "schema"')
    head = head.replace('-misc-spleen-', '-schema-schema-')
    with open(out, 'w') as f:
        f.write('COMMENT schema-16x32: Spleen 16x32 2.1.0 cut to 256 glyphs, rail glyphs added\n')
        f.write(head)
        f.write(f'CHARS {len(chosen)}\n')
        for cp in chosen:
            f.write(f'STARTCHAR uni{cp:04X}\nENCODING {cp}\nSWIDTH 500 0\nDWIDTH 16 0\nBBX 16 32 0 -6\nBITMAP\n')
            for r in glyphs[cp]:
                f.write(f'{r:04X}\n')
            f.write('ENDCHAR\n')
        f.write('ENDFONT\n')


if __name__ == '__main__':
    main()
