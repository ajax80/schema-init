#!/usr/bin/env python3
# bdf2psf.py IN.bdf OUT.psfu — a 10x20, 256-glyph BDF to PSF2 with a Unicode table
# (one code point per glyph: its ENCODING).
import struct
import sys

W, H, N = 10, 20, 256


def main():
    src, out = sys.argv[1], sys.argv[2]
    glyphs, cur, rows = [], None, None
    for line in open(src):
        line = line.rstrip('\n')
        if line.startswith('ENCODING '):
            cur = int(line.split()[1])
        elif line.startswith('BBX ') and line.split()[1:] != ['10', '20', '0', '-4']:
            sys.exit(f'bdf2psf: U+{cur:04X}: {line}, want BBX 10 20 0 -4')
        elif line == 'BITMAP':
            rows = []
        elif line == 'ENDCHAR':
            if len(rows) != H:
                sys.exit(f'bdf2psf: U+{cur:04X}: {len(rows)} rows')
            glyphs.append((cur, rows))
            cur, rows = None, None
        elif rows is not None:
            rows.append(int(line, 16))
    if len(glyphs) != N:
        sys.exit(f'bdf2psf: {len(glyphs)} glyphs, want {N}')
    bpg = H * ((W + 7) // 8)
    data = struct.pack('<8I', 0x864AB572, 0, 32, 1, N, bpg, H, W)
    for _, rows in glyphs:
        data += b''.join(struct.pack('>H', r) for r in rows)
    for cp, _ in glyphs:
        data += chr(cp).encode('utf-8') + b'\xff'
    open(out, 'wb').write(data)


if __name__ == '__main__':
    main()
