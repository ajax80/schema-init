#include "../verify_boot.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FX "tests/fixtures/verify/boot/"
#define SAVED "${saved_entry}"

static char words[64][512];
static grubenv_t env[32];

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
    assert(nextboot_resolve(env, n, ents, 3, SAVED, 0, &nb) == 0);
    assert(!strcmp(nb.actual, "two-initrd") && !strcmp(nb.normal, "normal"));
    /* no schema_fallback_id: the newest schema-snap-* */
    assert(!strcmp(nb.fallback, "schema-snap-20261118-175630-post-bootfix"));
    assert(!nb.unresolved);

    n = grubenv_read(FX "grubenv-fallback", env, 32);
    assert(nextboot_resolve(env, n, ents, 3, SAVED, 0, &nb) == 0);
    assert(!strcmp(nb.actual, "schema-snap-20261118-175630-post-bootfix"));
    assert(!strcmp(nb.normal, "normal"));
    assert(!strcmp(nb.fallback, "schema-snap-20261118-175630-post-bootfix"));

    n = grubenv_read(FX "grubenv-unknown", env, 32);
    assert(nextboot_resolve(env, n, ents, 3, SAVED, 0, &nb) == 2);
    assert(nb.unresolved && !strcmp(nb.fallback, "schema-snap-19990101-000000-gone"));

    /* no saved_entry: the first entry, a menu position known only when every entry has
     * the version key blscfg sorts by (two-initrd has none) and none was cut */
    n = env_from("# GRUB Environment Block\nblsdir=/x\n");
    assert(nextboot_resolve(env, n, ents, 1, SAVED, 0, &nb) == 0);
    assert(!strcmp(nb.actual, "normal") && !strcmp(nb.normal, "normal"));
    assert(nextboot_resolve(env, n, ents, 3, SAVED, 0, &nb) == 1 && nb.unresolved && !nb.actual);
    assert(nextboot_resolve(env, n, ents, 1, SAVED, 1, &nb) == 1 && nb.unresolved && !nb.actual);

    /* grub.cfg's header default other than ${saved_entry}: saved_entry is not what boots */
    n = env_from("saved_entry=two-initrd\n");
    bls_entry_t two[2] = { ents[2], ents[0] };
    assert(nextboot_resolve(env, n, two, 2, "0", 0, &nb) == 0 && !strcmp(nb.actual, ents[2].id));
    assert(nextboot_resolve(env, n, two, 2, "1", 0, &nb) == 0 && !strcmp(nb.actual, "normal"));
    assert(nextboot_resolve(env, n, two, 2, "2", 0, &nb) == 1 && !nb.actual);
    assert(nextboot_resolve(env, n, two, 2, "0", 1, &nb) == 1 && !nb.actual);
    assert(nextboot_resolve(env, n, ents, 3, "0", 0, &nb) == 1 && !nb.actual);
    assert(nextboot_resolve(env, n, ents, 3, "normal", 0, &nb) == 0 && !strcmp(nb.actual, "normal"));
    assert(nextboot_resolve(env, n, ents, 3, "Fedora Linux (6.1.0) 44", 0, &nb) == 1 && !nb.actual);
    assert(nextboot_resolve(env, n, ents, 3, NULL, 0, &nb) == 1 && !nb.actual);
    /* next_entry still wins over the header's default */
    n = env_from("next_entry=two-initrd\n");
    assert(nextboot_resolve(env, n, ents, 3, "normal", 0, &nb) == 0 && !strcmp(nb.actual, "two-initrd"));

    /* a saved_entry naming nothing is GRUB falling back to entry 0, not a known choice */
    n = env_from("saved_entry=gone\n");
    assert(nextboot_resolve(env, n, ents, 3, SAVED, 0, &nb) == 1 && nb.unresolved);

    /* next_entry also yields to an exhausted guard */
    n = env_from("saved_entry=normal\nnext_entry=two-initrd\nboot_counter=-1\nboot_success=0\n"
                 "schema_fallback_id=schema-snap-20261118-175630-post-bootfix\n");
    assert(nextboot_resolve(env, n, ents, 3, SAVED, 0, &nb) == 0);
    assert(!strcmp(nb.actual, "schema-snap-20261118-175630-post-bootfix") && !strcmp(nb.normal, "normal"));

    /* a confirmed boot never falls back */
    n = env_from("saved_entry=normal\nboot_counter=0\nboot_success=1\n"
                 "schema_fallback_id=schema-snap-20261118-175630-post-bootfix\n");
    assert(nextboot_resolve(env, n, ents, 3, SAVED, 0, &nb) == 0 && !strcmp(nb.actual, "normal"));

    /* 08_fallback_counting decrements 1 to 0 before 09 reads it */
    n = env_from("saved_entry=normal\nboot_counter=1\nboot_success=0\n"
                 "schema_fallback_id=schema-snap-20261118-175630-post-bootfix\n");
    assert(nextboot_resolve(env, n, ents, 3, SAVED, 0, &nb) == 0);
    assert(!strcmp(nb.actual, "schema-snap-20261118-175630-post-bootfix"));

    /* exhausted with no fallback id: 08 sets default=1, a menu position, not an id */
    n = env_from("saved_entry=normal\nboot_counter=0\nboot_success=0\n");
    assert(nextboot_resolve(env, n, ents, 3, SAVED, 0, &nb) == 1 && nb.unresolved && !nb.actual);
    assert(!strcmp(nb.normal, "normal"));
    n = env_from("saved_entry=normal\nboot_counter=1\nboot_success=0\n");
    assert(nextboot_resolve(env, n, ents, 3, SAVED, 0, &nb) == 0 && !strcmp(nb.actual, "normal"));

    /* freshly armed (counter 2): this boot is still the normal one */
    n = env_from("saved_entry=normal\nboot_counter=2\nboot_success=0\n"
                 "schema_fallback_id=schema-snap-20261118-175630-post-bootfix\n");
    assert(nextboot_resolve(env, n, ents, 3, SAVED, 0, &nb) == 0 && !strcmp(nb.actual, "normal"));
}

/* the env block is 1 KB and a value can fill most of it (blakbox's kernelopts is 270) */
static void test_grubenv_long(void) {
    char text[4096], val[1100];
    memset(val, 'x', sizeof val);
    snprintf(val + 580, sizeof val - 580, " init=/sbin/schema-init");
    snprintf(text, sizeof text, "saved_entry=normal\nkernelopts=root=/dev/a %s\nboot_success=1\n", val);
    int n = env_from(text);
    assert(n == 3 && !strcmp(env[1][0], "kernelopts") && strstr(env[1][1], " init=/sbin/schema-init"));
    assert(!strcmp(env[2][0], "boot_success") && !strcmp(env[2][1], "1"));

    /* longer than any env block holds: kept as cut, never as a shorter value */
    memset(val, 'y', sizeof val - 1);
    val[sizeof val - 1] = '\0';
    snprintf(text, sizeof text, "kernelopts=%s\nsaved_entry=normal\n", val);
    n = env_from(text);
    assert(n == 2 && !strcmp(env[0][0], "kernelopts") && !strcmp(env[0][1], GRUBENV_CUT));
    assert(!strcmp(env[1][1], "normal"));

    /* a cut selection variable is not a known choice */
    nextboot_t nb;
    load_ents();
    snprintf(text, sizeof text, "saved_entry=%s\n", val);
    n = env_from(text);
    assert(nextboot_resolve(env, n, ents, 3, SAVED, 0, &nb) == 1 && nb.unresolved);
    snprintf(text, sizeof text, "saved_entry=normal\nschema_fallback_id=%s\n", val);
    n = env_from(text);
    assert(nextboot_resolve(env, n, ents, 3, SAVED, 0, &nb) == 1 && nb.unresolved);
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

static void test_grubcfg_default(void) {
    char v[256], buf[8192];
    assert(grubcfg_default(slurp(FX "grub-real.cfg"), v, sizeof v) == 0 && !strcmp(v, "${saved_entry}"));
    /* GRUB_DEFAULT=0: the header's else branch */
    static const char hdr0[] =
        "if [ \"${next_entry}\" ] ; then\n   set default=\"${next_entry}\"\n   set next_entry=\n"
        "   save_env next_entry\n   set boot_once=true\nelse\n   set default=\"0\"\nfi\n";
    static const char guard[] =
        "if [ -n \"${boot_counter}\" -a \"${boot_success}\" = \"0\" ]; then\n"
        "  if  [ \"${boot_counter}\" = \"0\" -o \"${boot_counter}\" = \"-1\" ]; then\n"
        "    set default=1\n  fi\nfi\n";
    snprintf(buf, sizeof buf, "%s%sinsmod blscfg\nblscfg\n", hdr0, guard);
    assert(grubcfg_default(buf, v, sizeof v) == 0 && !strcmp(v, "0"));
    /* GRUB_DEFAULT=<title>: a name, returned as is */
    assert(grubcfg_default("set default=\"Fedora Linux\"\n", v, sizeof v) == 0 && !strcmp(v, "Fedora Linux"));
    assert(grubcfg_default("set default=saved\n", v, sizeof v) == 0 && !strcmp(v, "saved"));
    /* a menu entry ahead of blscfg's: a position no longer names a BLS entry */
    snprintf(buf, sizeof buf, "%smenuentry 'x' {\n  chainloader /x\n}\nblscfg\n", hdr0);
    assert(grubcfg_default(buf, v, sizeof v) == -1);
    snprintf(buf, sizeof buf, "%sblscfg\nmenuentry 'x' {\n  chainloader /x\n}\n", hdr0);
    assert(grubcfg_default(buf, v, sizeof v) == 0 && !strcmp(v, "0"));
    /* no default set, or one only under a condition verify can't judge */
    assert(grubcfg_default("blscfg\n", v, sizeof v) == -1);
    assert(grubcfg_default("if [ -f /x ]; then\n  set default=2\nfi\n", v, sizeof v) == -1);
    assert(grubcfg_default("if [ -f /x ]; then\n  set default=2\nfi\nset default=\"${saved_entry}\"\n", v, sizeof v) == 0);
    /* in a function body or a comment it is not run */
    assert(grubcfg_default("set default=\"${saved_entry}\"\nfunction f {\n  set default=3\n}\n# set default=4\n", v, sizeof v) == 0 &&
           !strcmp(v, "${saved_entry}"));
}

/* env_block=512+1: the variables GRUB loads after grubenv, from sector 512 of its device */
static void test_envblock(void) {
    char dev[4200], blk[512];
    snprintf(dev, sizeof dev, "%s/vboot-envdev", tmpd);
    FILE *f = fopen(dev, "w");
    assert(f);
    memset(blk, '#', sizeof blk);
    static const char text[] = "# GRUB Environment Block\nboot_success=1\nboot_indeterminate=0\n";
    memcpy(blk, text, sizeof text - 1);
    fseek(f, 512L * 512, SEEK_SET);
    fwrite(blk, 1, sizeof blk, f);
    fclose(f);
    int n = grubenv_block_read(dev, "512+1", env, 32);
    assert(n == 2 && !strcmp(env[0][0], "boot_success") && !strcmp(env[0][1], "1") && !strcmp(env[1][0], "boot_indeterminate"));
    /* not an env block there, a range GRUB's editenv never writes, no device: errors, never empty */
    assert(grubenv_block_read(dev, "511+1", env, 32) == -1);
    assert(grubenv_block_read(dev, "512+1,600+1", env, 32) == -1);
    assert(grubenv_block_read(dev, "x", env, 32) == -1);
    assert(grubenv_block_read("/nonexistent/dev", "512+1", env, 32) == -1);
    remove(dev);
}

int main(int argc, char **argv) {
    if (argc > 1) tmpd = argv[1];
    test_karg();
    test_bls();
    test_nextboot();
    test_grubcfg();
    test_grubenv_long();
    test_grubcfg_default();
    test_envblock();
    printf("verify_boot: ok\n");
    return 0;
}
