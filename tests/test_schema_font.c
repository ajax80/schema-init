#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../rail_glyphs.h"

static uint32_t utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c;
    int n;
    if (s[0] < 0x80) { c = s[0]; n = 1; }
    else if ((s[0] & 0xe0) == 0xc0) { c = s[0] & 0x1f; n = 2; }
    else if ((s[0] & 0xf0) == 0xe0) { c = s[0] & 0x0f; n = 3; }
    else { c = s[0] & 0x07; n = 4; }
    for (int i = 1; i < n; i++) c = (c << 6) | (s[i] & 0x3f);
    *p += n;
    return c;
}

static const char *const *ab_strings(const struct rail_alphabet *a, int *n) {
    static const char *v[16];
    int k = 0;
    v[k++] = a->track_v; v[k++] = a->branch; v[k++] = a->branch_rt; v[k++] = a->finish;
    v[k++] = a->g_on; v[k++] = a->g_start; v[k++] = a->g_warn; v[k++] = a->g_cut; v[k++] = a->g_stop;
    v[k++] = a->sep; v[k++] = a->trunc; v[k++] = a->banner_l; v[k++] = a->banner_r;
    *n = k;
    return v;
}

int main(void) {
    int n;
    const char *const *v;

    assert(sizeof schema_font_glyphs == 256 * 64);

    v = ab_strings(&rail_font_ab, &n);
    for (int i = 0; i < n; i++)
        for (const char *p = v[i]; *p;) {
            uint32_t cp = utf8_next(&p);
            if (!schema_font_has(cp)) { fprintf(stderr, "font alphabet U+%04X has no glyph\n", cp); return 1; }
        }
    for (uint32_t c = 0x20; c <= 0x7e; c++) assert(schema_font_has(c));
    for (uint32_t c = 0xa0; c <= 0xff; c++) assert(schema_font_has(c));

    v = ab_strings(&rail_ascii_ab, &n);
    for (int i = 0; i < n; i++)
        for (const char *p = v[i]; *p; p++) assert((unsigned char)*p < 0x80);

    for (unsigned i = 1; i < schema_font_unimap_len; i++)
        assert(schema_font_unimap[i - 1].unicode < schema_font_unimap[i].unicode);
    for (unsigned i = 0; i < schema_font_unimap_len; i++) assert(schema_font_unimap[i].fontpos < 256);

    printf("test_schema_font: ok\n");
    return 0;
}
