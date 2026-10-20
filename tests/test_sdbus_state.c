#define _GNU_SOURCE
#include "../sdbus_state.h"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>

static sdbus_conn *mk(int id, int fd) {
    sdbus_conn *c = calloc(1, sizeof *c);
    c->id = id; c->fd = fd;
    return c;
}

static void free_conns(sdbus_conn **cs, int n) {
    for (int i = 0; i < n; i++) { sdbus_conn_free_fields(cs[i]); free(cs[i]); }
}

static int rule_has(sdbus_matchset *m, const char *raw) {
    for (int i = 0; m && i < m->n; i++) if (!strcmp(m->rules[i].raw, raw)) return 1;
    return 0;
}

static int snapshot(int mfd, sdbus_conn **cs, int n, sdbus_names *nm, sdbus_replies *rp,
                    unsigned next_id, unsigned next_unique) {
    sdbus_state_hdr h = { next_id, next_unique, 99 };
    return sdbus_state_write(mfd, &h, cs, n, nm, rp);
}

int main(void) {
    int mfd = memfd_create("sdbus-state", 0);
    assert(mfd >= 0);

    sdbus_names *nm = sdbus_names_new();
    sdbus_replies *rp = sdbus_replies_new();
    sdbus_conn *cs[3];
    cs[0] = mk(1, 10); cs[1] = mk(2, 11); cs[2] = mk(3, 12);
    cs[0]->authed = 1; cs[0]->saw_nul = 1;
    cs[1]->authed = cs[1]->auth_ok = cs[1]->said_hello = cs[1]->negotiated_fd = cs[1]->saw_nul = 1;
    cs[1]->uid = 1000; cs[1]->gid = 1001; cs[1]->pid = 4242;
    cs[1]->gids[0] = 1001; cs[1]->gids[1] = 27; cs[1]->n_gids = 2;
    cs[1]->unique = sdbus_names_alloc_unique(nm, 2);
    sdbus_conn_in_append(cs[1], "0123456789", 10);
    cs[1]->pending_fds[0] = 77; cs[1]->n_pending_fds = 1;
    cs[1]->matches = sdbus_match_new();
    assert(sdbus_match_add(cs[1]->matches, "type='signal',interface='a.B'") == 0);
    assert(sdbus_match_add(cs[1]->matches, "type='signal',member='NameOwnerChanged',arg0=':1.7'") == 0);
    cs[2]->authed = cs[2]->auth_ok = cs[2]->said_hello = 1;
    cs[2]->is_monitor = 1;
    cs[2]->mon_matches = sdbus_match_new();
    assert(sdbus_match_add(cs[2]->mon_matches, "type='method_call'") == 0);

    sdbus_transition tr[2]; int ntr;
    assert(sdbus_names_request(nm, 2, "org.a", 0, tr, &ntr) == SDBUS_REQ_PRIMARY_OWNER);
    assert(sdbus_names_request(nm, 3, "org.a", SDBUS_REQ_DO_NOT_QUEUE, tr, &ntr) == SDBUS_REQ_EXISTS);
    assert(sdbus_names_request(nm, 3, "org.a", 0, tr, &ntr) == SDBUS_REQ_IN_QUEUE);
    nm->names[0].holders[1].flags = 4;
    sdbus_replies_record(rp, 2, 555, 3);
    rp->ents[0].deadline = sdbus__now_ms() + 5000;

    assert(snapshot(mfd, cs, 3, nm, rp, 41, 17) == 0);
    free_conns(cs, 3);
    sdbus_names_free(nm); sdbus_replies_free(rp);

    sdbus_names *nm2 = sdbus_names_new();
    sdbus_replies *rp2 = sdbus_replies_new();
    sdbus_state_hdr h = {0};
    sdbus_conn *rc[8]; int nrc = 0;
    int cfds[3] = {20, 21, 22};
    int pfds[1] = {88};
    assert(sdbus_state_read(mfd, &h, rc, 8, &nrc, cfds, 3, pfds, 1, nm2, rp2) == 0);
    assert(nrc == 3);
    assert(h.next_id == 41 && h.next_unique == 17 && h.driver_serial == 99);
    assert(nm2->next_unique == 17);
    assert(rc[0]->id == 1 && rc[1]->id == 2 && rc[2]->id == 3);
    assert(rc[0]->fd == 20 && rc[1]->fd == 21 && rc[2]->fd == 22);
    assert(rc[0]->authed && !rc[0]->auth_ok && !rc[0]->said_hello && rc[0]->saw_nul && !rc[0]->negotiated_fd);
    assert(rc[0]->unique == NULL);
    assert(rc[1]->authed && rc[1]->auth_ok && rc[1]->said_hello && rc[1]->negotiated_fd && rc[1]->saw_nul);
    assert(rc[1]->uid == 1000 && rc[1]->gid == 1001 && rc[1]->pid == 4242);
    assert(rc[1]->n_gids == 2 && rc[1]->gids[0] == 1001 && rc[1]->gids[1] == 27);
    assert(rc[1]->unique && !strcmp(rc[1]->unique, ":1.1"));
    assert(sdbus_names_unique(nm2, 2) == rc[1]->unique);
    assert(rc[1]->in_len == 10 && !memcmp(rc[1]->in, "0123456789", 10));
    assert(rc[1]->n_pending_fds == 1 && rc[1]->pending_fds[0] == 88);
    assert(rc[1]->matches && rc[1]->matches->n == 2);
    assert(rule_has(rc[1]->matches, "type='signal',interface='a.B'"));
    assert(rule_has(rc[1]->matches, "type='signal',member='NameOwnerChanged',arg0=':1.7'"));
    {
        sdbus_match_rule *r = &rc[1]->matches->rules[1];
        assert(r->arg0 && !strcmp(r->arg0, ":1.7"));
    }
    assert(rc[1]->mon_matches == NULL && !rc[1]->is_monitor);
    assert(rc[2]->is_monitor && rc[2]->mon_matches && rc[2]->mon_matches->n == 1);
    assert(rule_has(rc[2]->mon_matches, "type='method_call'"));
    assert(rc[2]->n_pending_fds == 0);
    assert(sdbus_names_owner(nm2, "org.a") == 2);
    sdbus_name_ent *e = sdbus__name_find(nm2, "org.a");
    assert(e && e->n_holders == 2 && e->holders[0].conn_id == 2 && e->holders[1].conn_id == 3);
    assert(e->holders[1].flags == 4);
    assert(rp2->n == 1 && rp2->ents[0].live && rp2->ents[0].serial == 555);
    assert(rp2->ents[0].caller == 3 && rp2->ents[0].callee == 2);
    long left = rp2->ents[0].deadline - sdbus__now_ms();
    assert(left > 4900 && left <= 5100);
    free_conns(rc, nrc);
    sdbus_names_free(nm2); sdbus_replies_free(rp2);

    {
        sdbus_names *n3 = sdbus_names_new();
        sdbus_replies *r3 = sdbus_replies_new();
        sdbus_conn *c3[1] = { mk(1, 5) };
        assert(snapshot(mfd, c3, 1, n3, r3, 41, 17) == 0);
        sdbus_state_hdr h3 = {0};
        sdbus_names *n4 = sdbus_names_new();
        sdbus_replies *r4 = sdbus_replies_new();
        sdbus_conn *o[2]; int no = 0; int f1[1] = {30};
        assert(sdbus_state_read(mfd, &h3, o, 2, &no, f1, 1, NULL, 0, n4, r4) == 0);
        assert(h3.next_id == 41 && h3.next_unique == 17 && n4->next_unique == 17);
        assert(no == 1 && o[0]->id == 1);
        const char *u = sdbus_names_alloc_unique(n4, 9);
        assert(!strcmp(u, ":1.17"));
        free_conns(o, no); free_conns(c3, 1);
        sdbus_names_free(n3); sdbus_replies_free(r3); sdbus_names_free(n4); sdbus_replies_free(r4);
    }

    {
        sdbus_names *n3 = sdbus_names_new();
        sdbus_replies *r3 = sdbus_replies_new();
        sdbus_conn *c3[1] = { mk(1, 5) };
        assert(snapshot(mfd, c3, 1, n3, r3, 2, 2) == 0);
        uint32_t v = SDBUS_STATE_VERSION + 1;
        assert(pwrite(mfd, &v, sizeof v, sizeof(uint32_t)) == sizeof v);
        sdbus_state_hdr h3 = {0};
        sdbus_names *n4 = sdbus_names_new();
        sdbus_replies *r4 = sdbus_replies_new();
        sdbus_conn *o[2]; int no = 0; int f1[1] = {30};
        assert(sdbus_state_read(mfd, &h3, o, 2, &no, f1, 1, NULL, 0, n4, r4) == -2);
        assert(no == 0);
        free_conns(c3, 1);
        sdbus_names_free(n3); sdbus_replies_free(r3); sdbus_names_free(n4); sdbus_replies_free(r4);
    }

    {
        sdbus_names *n3 = sdbus_names_new();
        sdbus_replies *r3 = sdbus_replies_new();
        sdbus_conn *c3[3] = { mk(1, 5), mk(2, 6), mk(3, 7) };
        sdbus_conn_in_append(c3[2], "abcdefghij", 10);
        c3[1]->matches = sdbus_match_new();
        sdbus_match_add(c3[1]->matches, "type='signal'");
        assert(snapshot(mfd, c3, 3, n3, r3, 4, 1) == 0);
        off_t sz = lseek(mfd, 0, SEEK_END);
        assert(ftruncate(mfd, sz / 2) == 0);
        sdbus_state_hdr h3 = {0};
        sdbus_names *n4 = sdbus_names_new();
        sdbus_replies *r4 = sdbus_replies_new();
        sdbus_conn *o[4]; int no = 0; int f3[3] = {30, 31, 32};
        assert(sdbus_state_read(mfd, &h3, o, 4, &no, f3, 3, NULL, 0, n4, r4) == -1);
        assert(no == 0);

        assert(ftruncate(mfd, sz) == 0);
        int f2[2] = {30, 31};
        assert(snapshot(mfd, c3, 3, n3, r3, 4, 1) == 0);
        assert(sdbus_state_read(mfd, &h3, o, 4, &no, f2, 2, NULL, 0, n4, r4) == -1);
        assert(no == 0);
        free_conns(c3, 3);
        sdbus_names_free(n3); sdbus_replies_free(r3); sdbus_names_free(n4); sdbus_replies_free(r4);
    }

    close(mfd);
    printf("test_sdbus_state: OK\n");
    return 0;
}
