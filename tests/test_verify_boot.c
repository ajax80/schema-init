#include "../verify_boot.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FX "tests/fixtures/verify/boot/"

static char words[64][512];
static char env[32][2][256];

static char *slurp(const char *path) {
    FILE *f = fopen(path, "r");
    assert(f);
    static char buf[16384];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int check_opts(const bls_entry_t *e, char *why) {
    int n = karg_split(e->options, words, 64);
    assert(n >= 0);
    return karg_check(words, n, why, 256);
}

static void test_bls(void) {
    bls_entry_t e;
    char why[256];

    assert(bls_parse(FX "normal.conf", &e) == 0);
    assert(!strcmp(e.id, "normal"));
    assert(!strcmp(e.linux_path, "/root/boot/vmlinuz-6.1.0"));
    /* $tuned_initrd is a GRUB variable, not a path */
    assert(e.n_initrd == 1 && !strcmp(e.initrd[0], "/root/boot/initramfs-6.1.0.img"));
    assert(!e.blank_before_last && !e.parse_err);
    assert(check_opts(&e, why) == 0);

    /* the 11-18 entry: blank line where initrd was, options dropped by blscfg */
    assert(bls_parse(FX "schema-snap-20261118-175630-post-bootfix.conf", &e) == 0);
    assert(!strcmp(e.id, "schema-snap-20261118-175630-post-bootfix"));
    assert(e.blank_before_last == 1 && e.n_initrd == 0);

    assert(bls_parse(FX "two-initrd.conf", &e) == 0);
    assert(e.n_initrd == 2 && !strcmp(e.initrd[0], "/boot/intel-ucode.img") &&
           !strcmp(e.initrd[1], "/boot/initramfs-6.1.0.img"));
    assert(!e.blank_before_last);

    assert(bls_parse(FX "nine-initrd.conf", &e) == 0);
    assert(e.parse_err);

    assert(bls_parse(FX "noroot.conf", &e) == 0);
    assert(check_opts(&e, why) == -1 && strstr(why, "root="));

    assert(bls_parse(FX "duproot.conf", &e) == 0);
    assert(check_opts(&e, why) == -1 && strstr(why, "root="));

    assert(bls_parse(FX "quoted.conf", &e) == 0);
    int n = karg_split(e.options, words, 64);
    assert(n == 3);
    assert(!strcmp(words[1], "SCHEMA_SVC_DIR=/etc/my services"));
    assert(check_opts(&e, why) == 0);

    /* blscfg takes the first linux and options; root= only in a second options line is not booted */
    assert(bls_parse(FX "two-options.conf", &e) == 0);
    assert(!strcmp(e.linux_path, "/boot/vmlinuz-a") && e.extra_linux);
    assert(!strcmp(e.options, "ro quiet") && e.extra_options);
    assert(check_opts(&e, why) == -1);
    assert(bls_parse(FX "normal.conf", &e) == 0 && !e.extra_linux && !e.extra_options);

    assert(bls_parse(FX "no-such.conf", &e) == -1);
}

static void test_karg(void) {
    char why[256];
    assert(karg_split("  a  b\tc\n", words, 64) == 3 && !strcmp(words[2], "c"));
    assert(karg_split("\"quoted word\" x=\"a -- b\" --", words, 64) == 3);
    assert(!strcmp(words[0], "quoted word") && !strcmp(words[1], "x=a -- b") && !strcmp(words[2], "--"));
    /* the kernel strips only a leading quote (word or value) and its closer */
    assert(karg_split("a=b\"c d\"", words, 64) == 1 && !strcmp(words[0], "a=b\"c d\""));
    assert(karg_split("", words, 64) == 0);
    assert(karg_split("a b c", words, 2) == -1);

    assert(karg_split("root=/dev/a rootflags=x rootflags=y", words, 64) == 3);
    assert(karg_check(words, 3, why, sizeof why) == -1 && strstr(why, "rootflags="));
    assert(karg_split("root=/dev/a init=/a init=/a", words, 64) == 3);
    assert(karg_check(words, 3, why, sizeof why) == -1 && strstr(why, "init="));
    /* after -- the words are init's argv */
    assert(karg_split("root=/dev/a -- root=/dev/b", words, 64) == 3);
    assert(karg_check(words, 3, why, sizeof why) == 0);
    assert(karg_split("ro -- root=/dev/b", words, 64) == 3);
    assert(karg_check(words, 3, why, sizeof why) == -1);
}

static bls_entry_t ents[3];

static void load_ents(void) {
    assert(bls_parse(FX "normal.conf", &ents[0]) == 0);
    assert(bls_parse(FX "two-initrd.conf", &ents[1]) == 0);
    assert(bls_parse(FX "schema-snap-20261118-175630-post-bootfix.conf", &ents[2]) == 0);
}

static const char *tmpd = "/tmp";

static int env_from(const char *text) {
    char p[4200];
    snprintf(p, sizeof p, "%s/vboot-env-XXXXXX", tmpd);
    int fd = mkstemp(p);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    fputs(text, f);
    fclose(f);
    int n = grubenv_read(p, env, 32);
    remove(p);
    return n;
}

static void test_nextboot(void) {
    nextboot_t nb;
    load_ents();

    int n = grubenv_read(FX "grubenv-next", env, 32);
    assert(n == 3);
    assert(!strcmp(env[0][0], "blsdir") && !strcmp(env[0][1], "/root/boot/loader/entries"));
    assert(nextboot_resolve(env, n, ents, 3, &nb) == 0);
    assert(!strcmp(nb.actual, "two-initrd") && !strcmp(nb.normal, "normal"));
    /* no schema_fallback_id: the newest schema-snap-* */
    assert(!strcmp(nb.fallback, "schema-snap-20261118-175630-post-bootfix"));
    assert(!nb.unresolved);

    n = grubenv_read(FX "grubenv-fallback", env, 32);
    assert(nextboot_resolve(env, n, ents, 3, &nb) == 0);
    assert(!strcmp(nb.actual, "schema-snap-20261118-175630-post-bootfix"));
    assert(!strcmp(nb.normal, "normal"));
    assert(!strcmp(nb.fallback, "schema-snap-20261118-175630-post-bootfix"));

    n = grubenv_read(FX "grubenv-unknown", env, 32);
    assert(nextboot_resolve(env, n, ents, 3, &nb) == 2);
    assert(nb.unresolved && !strcmp(nb.fallback, "schema-snap-19990101-000000-gone"));

    /* no saved_entry: the first entry */
    n = env_from("# GRUB Environment Block\nblsdir=/x\n");
    assert(nextboot_resolve(env, n, ents, 3, &nb) == 0);
    assert(!strcmp(nb.actual, "normal") && !strcmp(nb.normal, "normal"));

    /* a saved_entry naming nothing is GRUB falling back to entry 0, not a known choice */
    n = env_from("saved_entry=gone\n");
    assert(nextboot_resolve(env, n, ents, 3, &nb) == 1 && nb.unresolved);

    /* next_entry also yields to an exhausted guard */
    n = env_from("saved_entry=normal\nnext_entry=two-initrd\nboot_counter=-1\nboot_success=0\n"
                 "schema_fallback_id=schema-snap-20261118-175630-post-bootfix\n");
    assert(nextboot_resolve(env, n, ents, 3, &nb) == 0);
    assert(!strcmp(nb.actual, "schema-snap-20261118-175630-post-bootfix") && !strcmp(nb.normal, "normal"));

    /* a confirmed boot never falls back */
    n = env_from("saved_entry=normal\nboot_counter=0\nboot_success=1\n"
                 "schema_fallback_id=schema-snap-20261118-175630-post-bootfix\n");
    assert(nextboot_resolve(env, n, ents, 3, &nb) == 0 && !strcmp(nb.actual, "normal"));

    /* 08_fallback_counting decrements 1 to 0 before 09 reads it */
    n = env_from("saved_entry=normal\nboot_counter=1\nboot_success=0\n"
                 "schema_fallback_id=schema-snap-20261118-175630-post-bootfix\n");
    assert(nextboot_resolve(env, n, ents, 3, &nb) == 0);
    assert(!strcmp(nb.actual, "schema-snap-20261118-175630-post-bootfix"));

    /* exhausted with no fallback id: 08 sets default=1, a menu position, not an id */
    n = env_from("saved_entry=normal\nboot_counter=0\nboot_success=0\n");
    assert(nextboot_resolve(env, n, ents, 3, &nb) == 1 && nb.unresolved && !nb.actual);
    assert(!strcmp(nb.normal, "normal"));
    n = env_from("saved_entry=normal\nboot_counter=1\nboot_success=0\n");
    assert(nextboot_resolve(env, n, ents, 3, &nb) == 0 && !strcmp(nb.actual, "normal"));

    /* freshly armed (counter 2): this boot is still the normal one */
    n = env_from("saved_entry=normal\nboot_counter=2\nboot_success=0\n"
                 "schema_fallback_id=schema-snap-20261118-175630-post-bootfix\n");
    assert(nextboot_resolve(env, n, ents, 3, &nb) == 0 && !strcmp(nb.actual, "normal"));
}

static void test_grubcfg(void) {
    assert(grubcfg_live_fallback(slurp(FX "grub-real.cfg")) == 1);
    assert(grubcfg_live_fallback(slurp(FX "grub-comment.cfg")) == 0);
    assert(grubcfg_live_fallback("set default=\"${saved_entry}\"\n") == 0);
    /* outside the condition */
    assert(grubcfg_live_fallback("set default=\"${saved_entry}\"\nset default=\"${schema_fallback_id}\"\n") == 0);
    static const char block[] =
        "if [ -n \"${boot_counter}\" -a \"${boot_success}\" = \"0\" ]; then\n"
        "  if [ \"${boot_counter}\" = \"0\" -o \"${boot_counter}\" = \"-1\" ]; then\n"
        "    if [ -n \"${schema_fallback_id}\" ]; then\n"
        "      set default=\"${schema_fallback_id}\"\n"
        "    fi\n  fi\nfi\n";
    char buf[2048];
    /* the header setting default afterwards overrides it */
    snprintf(buf, sizeof buf, "%sset default=\"${saved_entry}\"\n", block);
    assert(grubcfg_live_fallback(buf) == 0);
    snprintf(buf, sizeof buf, "set default=\"${saved_entry}\"\n%s", block);
    assert(grubcfg_live_fallback(buf) == 1);
    /* an else branch runs when the condition is false */
    assert(grubcfg_live_fallback(
        "if [ -n \"${boot_counter}\" -a \"${boot_success}\" = \"0\" ]; then\n  true\nelse\n"
        "  set default=\"${schema_fallback_id}\"\nfi\n") == 0);
    /* a function body is not run where it is defined */
    assert(grubcfg_live_fallback(
        "function f {\nif [ -n \"${boot_counter}\" -a \"${boot_success}\" = \"0\" ]; then\n"
        "  set default=\"${schema_fallback_id}\"\nfi\n}\n") == 0);
}

int main(int argc, char **argv) {
    if (argc > 1) tmpd = argv[1];
    test_karg();
    test_bls();
    test_nextboot();
    test_grubcfg();
    printf("verify_boot: ok\n");
    return 0;
}
