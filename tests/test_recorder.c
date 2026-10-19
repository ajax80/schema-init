#include "../recorder.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void roundtrip(void) {
    rec_event_t e, p;
    char buf[1024];
    memset(&e, 0, sizeof e);
    e.ts = 1760000000; e.src = REC_SRC_EARLYOOM; e.crit = 1; e.pid = 42; e.rc = 9;
    strcpy(e.kind, "kill"); strcpy(e.comm, "cosmic-player"); strcpy(e.subj, "unit.svc");
    strcpy(e.msg, "--thumbnail a b.mkv");
    assert(rec_format(buf, sizeof buf, &e) > 0);
    memset(&p, 0, sizeof p);
    assert(rec_parse(buf, &p) == 0);
    assert(p.ts == e.ts && p.src == e.src && p.crit == e.crit && p.pid == 42 && p.rc == 9);
    assert(!strcmp(p.kind, "kill") && !strcmp(p.comm, "cosmic-player"));
    assert(!strcmp(p.subj, "unit.svc") && !strcmp(p.msg, "--thumbnail a b.mkv"));
}

static void hostile_comm(void) {
    rec_event_t e, p;
    char buf[1024];
    memset(&e, 0, sizeof e);
    e.ts = 1; e.src = REC_SRC_EARLYOOM;
    strcpy(e.kind, "kill"); strcpy(e.comm, "evil name=x\nsrc=pid1");
    assert(rec_format(buf, sizeof buf, &e) > 0);
    assert(rec_parse(buf, &p) == 0);
    assert(p.src == REC_SRC_EARLYOOM);
    assert(!strpbrk(p.comm, " =\n"));
}

static void msg_last(void) {
    rec_event_t e, p;
    char buf[1024];
    memset(&e, 0, sizeof e);
    e.ts = 1; e.src = REC_SRC_PID1;
    strcpy(e.kind, "k"); strcpy(e.msg, "a=b c=d");
    assert(rec_format(buf, sizeof buf, &e) > 0);
    assert(rec_parse(buf, &p) == 0);
    assert(!strcmp(p.msg, "a=b c=d"));
}

static void msg_unterminated(void) {
    rec_event_t e, p;
    char buf[1024];
    memset(&e, 0, sizeof e);
    e.ts = 1; e.src = REC_SRC_PID1;
    strcpy(e.kind, "k");
    memset(e.msg, 'x', sizeof e.msg);
    assert(rec_format(buf, sizeof buf, &e) > 0);
    assert(strlen(buf) < 400);
    assert(rec_parse(buf, &p) == 0);
    assert(strlen(p.msg) == sizeof p.msg - 1);
    memset(&e, 0, sizeof e);
    e.ts = 1; e.src = REC_SRC_PID1;
    strcpy(e.kind, "k"); strcpy(e.msg, "a\nb\rc");
    assert(rec_format(buf, sizeof buf, &e) > 0);
    assert(!strpbrk(buf, "\n\r"));
}

static void missing_required(void) {
    rec_event_t e;
    assert(rec_parse("pid=3 msg=x", &e) == -1);
}

static void send_no_listener(void) {
    rec_event_t e;
    memset(&e, 0, sizeof e);
    e.src = REC_SRC_PID1; strcpy(e.kind, "k");
    alarm(2);
    rec_send("/nonexistent/sock", &e);
    alarm(0);
}

int main(void) {
    roundtrip();
    hostile_comm();
    msg_last();
    msg_unterminated();
    missing_required();
    send_no_listener();
    printf("test_recorder: OK\n");
    return 0;
}
