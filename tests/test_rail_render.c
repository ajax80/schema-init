#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../rail.h"

static char buf[1024];

#define EQ(got, want) do { if (strcmp((got), (want))) { \
    fprintf(stderr, "%s:%d\n got: %s\nwant: %s\n", __FILE__, __LINE__, (got), (want)); exit(1); } } while (0)

static const char *line(const char *name, rail_chip_t chip, int n, const char *reason, long long t, int rt, int font) {
    rail_line_t l = { .name = name, .chip = chip, .retry_n = n, .t_ms = t, .runtime = rt };
    snprintf(l.reason, sizeof l.reason, "%s", reason);
    rail_fmt_line(buf, sizeof buf, &l, font);
    return buf;
}

static void chips(void) {
    EQ(line("mount-fstab", CHIP_ONLINE, 0, "", 210, 0, 1),
       "\033[32m┣━◆\033[0m \033[1;37mmount-fstab      \033[0m \033[30;42m ONLINE  \033[0m  \033[90m+0.21s\033[0m\n");
    EQ(line("dbus", CHIP_PERFECT, 0, "", 520, 0, 1),
       "\033[35m┣━◆\033[0m \033[1;37mdbus             \033[0m \033[30;45m PERFECT \033[0m  \033[90m+0.52s\033[0m\n");
    EQ(line("nm", CHIP_SPAWN, 0, "", 400, 0, 1),
       "\033[2;32m┣━◇\033[0m \033[1;37mnm               \033[0m \033[2;30;42m SPAWN   \033[0m  \033[90m+0.40s\033[0m\n");
    EQ(line("bluetooth", CHIP_RETRY, 2, "exit 1 · backoff 2s", 880, 0, 1),
       "\033[33m┣━▲\033[0m \033[1;37mbluetooth        \033[0m \033[30;43m RETRY 2 \033[0m  \033[33mexit 1 · backoff 2s\033[0m\n");
    EQ(line("mcelog", CHIP_CUT, 0, "exit 1", 1020, 0, 1),
       "\033[31m┣━■\033[0m \033[1;37mmcelog           \033[0m \033[30;41m CUT     \033[0m  \033[31mexit 1\033[0m\n");
    EQ(line("a", CHIP_DONE, 0, "", 12345, 0, 1),
       "\033[32m┣━◆\033[0m \033[1;37ma                \033[0m \033[30;42m DONE    \033[0m  \033[90m+12.34s\033[0m\n");
    EQ(line("a", CHIP_WAITING, 0, "", 0, 0, 1),
       "\033[2;32m┣━◇\033[0m \033[1;37ma                \033[0m \033[2;30;42m WAITING \033[0m  \033[90m+0.00s\033[0m\n");
    EQ(line("a", CHIP_DORMANT, 0, "", 0, 0, 1),
       "\033[34m┣━□\033[0m \033[1;37ma                \033[0m \033[30;44m DORMANT \033[0m  \033[90m+0.00s\033[0m\n");
    EQ(line("a", CHIP_STOPPED, 0, "", 0, 0, 1),
       "\033[34m┣━□\033[0m \033[1;37ma                \033[0m \033[30;44m STOPPED \033[0m  \033[90m+0.00s\033[0m\n");
    EQ(line("a", CHIP_EXITED, 0, "", 0, 0, 1),
       "\033[90m┣━□\033[0m \033[1;37ma                \033[0m \033[30;100m EXITED  \033[0m  \033[90m+0.00s\033[0m\n");
    EQ(line("a", CHIP_KILLED, 0, "", 0, 0, 1),
       "\033[31m┣━■\033[0m \033[1;37ma                \033[0m \033[30;41m KILLED  \033[0m  \033[90m+0.00s\033[0m\n");
    EQ(line("a", CHIP_STUCK, 0, "", 0, 0, 1),
       "\033[31m┣━■\033[0m \033[1;37ma                \033[0m \033[30;41m STUCK   \033[0m  \033[90m+0.00s\033[0m\n");
    EQ(line("a", CHIP_TIMER, 0, "", 0, 0, 1),
       "\033[36m┣━◆\033[0m \033[1;37ma                \033[0m \033[30;46m TIMER   \033[0m  \033[90m+0.00s\033[0m\n");
}

static void runtime_and_ascii(void) {
    /* 2026-11-20 16:42:07 UTC */
    EQ(line("bluetooth", CHIP_ONLINE, 0, "", 1795192927000LL, 1, 1),
       "\033[32m╍━◆\033[0m \033[1;37mbluetooth        \033[0m \033[30;42m ONLINE  \033[0m  \033[90m16:42:07\033[0m\n");
    EQ(line("mcelog", CHIP_CUT, 0, "exit 1", 1020, 0, 0),
       "\033[31m+-#\033[0m \033[1;37mmcelog           \033[0m \033[30;41m CUT     \033[0m  \033[31mexit 1\033[0m\n");
    EQ(line("x", CHIP_ONLINE, 0, "", 1795192927000LL, 1, 0),
       "\033[32m:-*\033[0m \033[1;37mx                \033[0m \033[30;42m ONLINE  \033[0m  \033[90m16:42:07\033[0m\n");
}

static void banner_finish(void) {
    rail_fmt_banner(buf, sizeof buf, "INIT", "0.4.29", "blakbox", 67, 1);
    EQ(buf, "\033[35m▓▒░\033[0m \033[95mS C H E M A\033[36m//\033[92mI N I T\033[0m \033[35m░▒▓\033[0m"
            "  \033[90mv0.4.29 · blakbox · 67 services\033[0m\n\033[35m┃\033[0m\n");
    rail_fmt_banner(buf, sizeof buf, "INIT", "v0.4.29-3-gabc", "blakbox", 1, 0);
    EQ(buf, "\033[35m==\033[0m \033[95mS C H E M A\033[36m//\033[92mI N I T\033[0m \033[35m==\033[0m"
            "  \033[90mv0.4.29-3-gabc - blakbox - 1 services\033[0m\n\033[35m|\033[0m\n");
    rail_fmt_banner(buf, sizeof buf, "HALT", NULL, NULL, 0, 1);
    EQ(buf, "\033[35m▓▒░\033[0m \033[95mS C H E M A\033[36m//\033[91mH A L T\033[0m \033[35m░▒▓\033[0m\n");

    rail_counts_t c = { .online = 65, .friction = 1, .cut = 1 };
    rail_fmt_finish(buf, sizeof buf, &c, 2710, 0, 1);
    EQ(buf, "\033[35m━━►\033[0m \033[30;42m 65 ONLINE \033[0m \033[30;43m 1 FRICTION \033[0m \033[30;41m 1 CUT \033[0m"
            "  \033[36mrail settled in 2.71s\033[0m\n");
    rail_counts_t d = { .online = 3, .waiting = 2, .starting = 1 };
    rail_fmt_finish(buf, sizeof buf, &d, 120000, 1, 0);
    EQ(buf, "\033[35m==>\033[0m \033[30;42m 3 ONLINE \033[0m \033[30;100m 2 WAITING \033[0m \033[30;43m 1 STARTING \033[0m"
            "  \033[33mrail still starting after 120s\033[0m\n");

    rail_fmt_shutdown_end(buf, sizeof buf, 1800, 0, 1);
    EQ(buf, "\033[35m━━►\033[0m \033[36mall down in 1.8s · powering off\033[0m\n");
    rail_fmt_shutdown_end(buf, sizeof buf, 250, 1, 0);
    EQ(buf, "\033[35m==>\033[0m \033[36mall down in 0.2s - rebooting\033[0m\n");
    rail_fmt_note(buf, sizeof buf, "filesystems read-only", 1);
    EQ(buf, "\033[90m┣━ filesystems read-only\033[0m\n");
}

static void name_widths(void) {
    char n[128];
    rail_name_cols(n, sizeof n, "abc", 1);
    EQ(n, "abc              ");
    rail_name_cols(n, sizeof n, "seventeen-chars-x", 1);
    EQ(n, "seventeen-chars-x");
    rail_name_cols(n, sizeof n, "eighteen-chars-xyz", 1);
    EQ(n, "eighteen-chars-x…");
    rail_name_cols(n, sizeof n, "eighteen-chars-xyz", 0);
    EQ(n, "eighteen-chars-x~");
    /* Cyrillic is not in the font: one ? per code point, never per byte */
    rail_name_cols(n, sizeof n, "сервис-журнала-событий", 1);
    EQ(n, "\?\?\?\?\?\?-\?\?\?\?\?\?\?-\?…");
    rail_name_cols(n, sizeof n, "café", 1);
    EQ(n, "café             ");
    rail_name_cols(n, sizeof n, "café", 0);
    EQ(n, "caf?             ");
    rail_name_cols(n, sizeof n, "bad\xff\x01x", 1);
    EQ(n, "bad??x           ");
}

static void ascii_only(void) {
    static const rail_chip_t all[] = { CHIP_ONLINE, CHIP_PERFECT, CHIP_DONE, CHIP_SPAWN, CHIP_WAITING, CHIP_RETRY,
                                       CHIP_DORMANT, CHIP_CUT, CHIP_STOPPED, CHIP_EXITED, CHIP_KILLED, CHIP_STUCK, CHIP_TIMER };
    char out[8][1024];
    rail_counts_t c = { 1, 1, 1, 1, 1 };
    for (size_t i = 0; i < sizeof all / sizeof *all; i++)
        for (int rt = 0; rt < 2; rt++) {
            line("naïve-☃", all[i], 3, i % 2 ? "why · x" : "", 5000, rt, 0);
            for (const char *p = buf; *p; p++) assert((unsigned char)*p < 0x80);
        }
    rail_fmt_banner(out[0], 1024, "INIT", "1", "höst", 2, 0);
    rail_fmt_finish(out[1], 1024, &c, 1, 0, 0);
    rail_fmt_finish(out[2], 1024, &c, 1, 1, 0);
    rail_fmt_shutdown_end(out[3], 1024, 1, 1, 0);
    rail_fmt_note(out[4], 1024, "ok", 0);
    rail_fmt_banner(out[5], 1024, "HALT", NULL, NULL, 0, 0);
    line("x", CHIP_CUT, 0, "", 1, 0, 0);
    snprintf(out[6], 1024, "%s", buf);
    EQ(out[0], "\033[35m==\033[0m \033[95mS C H E M A\033[36m//\033[92mI N I T\033[0m \033[35m==\033[0m"
               "  \033[90mv1 - h?st - 2 services\033[0m\n\033[35m|\033[0m\n");
    line("x", CHIP_RETRY, 1, "exit 1 · backoff 2s", 1, 0, 0);
    EQ(buf, "\033[33m+-^\033[0m \033[1;37mx                \033[0m \033[30;43m RETRY 1 \033[0m  \033[33mexit 1 - backoff 2s\033[0m\n");
    for (int k = 0; k < 7; k++)
        for (const char *p = out[k]; *p; p++)
            if ((unsigned char)*p >= 0x80) { fprintf(stderr, "non-ASCII in fallback output %d: %s\n", k, out[k]); exit(1); }
}

int main(void) {
    setenv("TZ", "UTC", 1);
    tzset();
    chips();
    runtime_and_ascii();
    banner_finish();
    name_widths();
    ascii_only();
    printf("test_rail_render: ok\n");
    return 0;
}
