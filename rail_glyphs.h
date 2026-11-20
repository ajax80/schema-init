#ifndef RAIL_GLYPHS_H
#define RAIL_GLYPHS_H

#include <stdint.h>
#include <linux/kd.h>

/* schema_font.c is generated from fonts/schema-16x32.bdf (tools/bdf2psf.py, tools/psf2c.py) */
extern const unsigned char schema_font_glyphs[256 * 64];
extern const struct unipair schema_font_unimap[];
extern const unsigned schema_font_unimap_len;
int schema_font_has(uint32_t cp);

struct rail_alphabet {
    const char *track_v, *branch, *branch_rt, *finish;
    const char *g_on, *g_start, *g_warn, *g_cut, *g_stop;
    const char *sep, *trunc, *banner_l, *banner_r;
};

/* every glyph here must be in schema-16x32 (tests/test_schema_font.c) */
static const struct rail_alphabet rail_font_ab = {
    "┃", "┣━", "╍━", "━━►",
    "◆", "◇", "▲", "■", "□",
    "·", "…", "▓▒░", "░▒▓",
};

/* the font didn't load: only bytes every console font has */
static const struct rail_alphabet rail_ascii_ab = {
    "|", "+-", ":-", "==>",
    "*", "o", "^", "#", "-",
    "-", "~", "==", "==",
};

#endif
