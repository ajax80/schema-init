#!/usr/bin/env python3
# mkschemafont.py TERMINUS.bdf OUT.bdf — cut Terminus ter-u20n down to the 256 glyphs
# of schema-10x20 and draw the rail glyphs Terminus lacks. 256, not 512: a 512-glyph
# console font costs the VT its bright colours (attribute bit 3 becomes a glyph bit).
import re
import sys

W, H = 10, 20


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


# BDF rows are whole bytes, left-aligned: a 10-px row is 16 bits, pixel 0 at bit 15
def setpx(g, x, y):
    if 0 <= x < W and 0 <= y < H:
        g[y] |= 1 << (15 - x)


# drawn on Terminus 20n's metrics: its U+25C6 is centred (4, 9) over rows 5..13,
# its U+25A0 fills cols 2..6 rows 6..13, its U+2501 is rows 9..10
def dash_heavy():
    g = blank()
    for y in (9, 10):
        for x in (1, 2, 3, 6, 7, 8):
            setpx(g, x, y)
    return g


def diamond_outline():
    g = blank()
    for y in range(H):
        for x in range(W):
            if 3 <= abs(x - 4) + abs(y - 9) <= 4:
                setpx(g, x, y)
    return g


def square_outline():
    g = blank()
    for y in range(6, 14):
        for x in range(2, 7):
            if y in (6, 13) or x in (2, 6):
                setpx(g, x, y)
    return g


DRAWN = {0x254D: dash_heavy, 0x25C7: diamond_outline, 0x25A1: square_outline}

# what the rail emits beyond ASCII/Latin-1 (keep in step with rail_glyphs.h)
RAIL = [0x2501, 0x2503, 0x2523, 0x2517, 0x254D, 0x25C6, 0x25C7, 0x25B2, 0x25A0, 0x25A1,
        0x25BA, 0x2026, 0x2593, 0x2592, 0x2591]
LIGHT_BOX = [0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C, 0x2534, 0x253C]
# filler up to 256, most useful first
EXTRA = [0xFFFD, 0x2588, 0x2580, 0x2584, 0x258C, 0x2590, 0x25CF, 0x25CB, 0x2190, 0x2191, 0x2192,
         0x2193, 0x2550, 0x2551, 0x2554, 0x2557, 0x255A, 0x255D, 0x2560, 0x2563, 0x2566, 0x2569,
         0x256C, 0x250F, 0x2513, 0x251B, 0x252B, 0x2533, 0x253B, 0x254B, 0x2022, 0x2018, 0x2019,
         0x201C, 0x201D, 0x2013, 0x2014, 0x20AC, 0x2122, 0x2713, 0x2717, 0x25BC, 0x25C0, 0x25B6,
         0x2665, 0x2666, 0x2663, 0x2660, 0x221E, 0x2248, 0x2260, 0x2264, 0x2265,
         0x256D, 0x256E, 0x2570, 0x256F, 0x2039, 0x203A]


def main():
    src, out = sys.argv[1], sys.argv[2]
    sp = parse(src)
    # ASCII and Latin-1 at their own positions: the kernel clears and pads with
    # raw glyph 0x20, never through the Unicode map. The rest fill 0x00-0x1F
    # and 0x7F-0x9F.
    slots = [None] * 256
    for cp in list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)):
        slots[cp] = cp
    free = list(range(0x00, 0x20)) + list(range(0x7F, 0xA0))
    glyphs = {}
    for cp in LIGHT_BOX + RAIL + EXTRA:
        if not free:
            break
        if cp in sp or cp in DRAWN:
            slots[free.pop(0)] = cp
        elif cp in LIGHT_BOX + RAIL:
            sys.exit(f'mkschemafont: U+{cp:04X} needed but neither in Terminus nor drawn')
    if free:
        sys.exit(f'mkschemafont: {len(free)} slots empty (add EXTRA candidates)')
    for cp in slots:
        glyphs[cp] = sp[cp] if cp in sp else DRAWN[cp]()
    chosen = slots
    head = re.split(r'^CHARS ', open(src).read(), flags=re.M)[0]
    head = head.replace('FAMILY_NAME "Terminus"', 'FAMILY_NAME "schema"')
    head = head.replace('-xos4-Terminus-', '-schema-schema-')
    head = head.replace('FOUNDRY "xos4"', 'FOUNDRY "schema"')
    # BDF wants STARTFONT first; the provenance COMMENT goes right after it
    first, rest = head.split('\n', 1)
    head = first + '\nCOMMENT schema-10x20: Terminus 4.49.1 ter-u20n cut to 256 glyphs, rail glyphs added\n' + rest
    with open(out, 'w') as f:
        f.write(head)
        f.write(f'CHARS {len(chosen)}\n')
        for cp in chosen:
            f.write(f'STARTCHAR uni{cp:04X}\nENCODING {cp}\nSWIDTH 500 0\nDWIDTH 10 0\nBBX 10 20 0 -4\nBITMAP\n')
            for r in glyphs[cp]:
                f.write(f'{r:04X}\n')
            f.write('ENDCHAR\n')
        f.write('ENDFONT\n')


if __name__ == '__main__':
    main()
