#include "../sdbus_policy.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static const char *evals(const char *pol, sdbus_req *r) {
    sdbus_policy *p = sdbus_policy_parse(pol);
    assert(p != NULL);
    const char *v = sdbus_policy_eval(p, r);
    sdbus_policy_free(p);
    return v;
}

int main(void) {
    const char *names[] = {"org.example.Svc"};
    sdbus_req send = { .op="send", .uid=1000, .interface="org.example.If",
        .member="Do", .msgtype="method_call", .destination=":1.5",
        .dest_names=names, .n_dest_names=1 };

    /* empty policy -> deny (start verdict) */
    assert(strcmp(evals("", &send), "deny") == 0);

    /* default context allowing this destination -> allow */
    const char *p1 = "context = default\nallow = send_destination:org.example.Svc\n";
    assert(strcmp(evals(p1, &send), "allow") == 0);

    /* send_destination is set-membership: different name -> deny */
    const char *p2 = "context = default\nallow = send_destination:org.other\n";
    assert(strcmp(evals(p2, &send), "deny") == 0);

    /* last-match-wins across ordered contexts: default allow, user deny */
    const char *p3 = "context = default\nallow = send_destination:org.example.Svc\n"
                     "context = user:1000\ndeny = send_destination:org.example.Svc\n";
    assert(strcmp(evals(p3, &send), "deny") == 0);

    /* order is default->group->user->mandatory regardless of file order:
       a user-allow overrides a default-deny even when written first */
    const char *p3b = "context = user:1000\nallow = send_destination:org.example.Svc\n"
                      "context = default\ndeny = send_destination:org.example.Svc\n";
    assert(strcmp(evals(p3b, &send), "allow") == 0);

    /* requested-reply exemption: reply short-circuits to allow even with no rule */
    sdbus_req reply = { .op="send", .uid=1000, .msgtype="method_return",
        .has_reply_serial=1, .destination=":1.9" };
    assert(strcmp(evals("", &reply), "allow") == 0);

    /* a method_call with a reply_serial is NOT a requested reply -> still gated */
    sdbus_req notreply = { .op="send", .uid=1000, .msgtype="method_call",
        .has_reply_serial=1, .destination=":1.9" };
    assert(strcmp(evals("", &notreply), "deny") == 0);

    /* own gating: own rule matches only op=="own" */
    sdbus_req own = { .op="own", .uid=0, .name="org.example.Svc" };
    const char *p4 = "context = default\nallow = own:org.example.Svc\n";
    assert(strcmp(evals(p4, &own), "allow") == 0);
    const char *p5 = "context = default\nallow = own:org.other\n";
    assert(strcmp(evals(p5, &own), "deny") == 0);

    /* own_prefix */
    const char *p6 = "context = default\nallow = own_prefix:org.example\n";
    assert(strcmp(evals(p6, &own), "allow") == 0);

    /* an own rule does not match a send op */
    assert(strcmp(evals(p4, &send), "deny") == 0);

    /* group context by gid membership */
    int gids[] = {10, 981};
    sdbus_req gsend = { .op="send", .uid=1000, .gids=gids, .n_gids=2,
        .destination=":1.5", .dest_names=names, .n_dest_names=1 };
    const char *p7 = "context = group:981\nallow = send_destination:org.example.Svc\n";
    assert(strcmp(evals(p7, &gsend), "allow") == 0);
    const char *p8 = "context = group:7\nallow = send_destination:org.example.Svc\n";
    assert(strcmp(evals(p8, &gsend), "deny") == 0);

    /* send_destination:* requires a non-empty owned-name set */
    const char *pstar = "context = default\nallow = send_destination:*\n";
    assert(strcmp(evals(pstar, &send), "allow") == 0);
    sdbus_req nodest = { .op="send", .uid=1000, .msgtype="method_call" };
    assert(strcmp(evals(pstar, &nodest), "deny") == 0);

    /* unknown predicate never matches -> no allow */
    const char *pbogus = "context = default\nallow = bogus_pred:x, send_destination:org.example.Svc\n";
    assert(strcmp(evals(pbogus, &send), "deny") == 0);

    /* comments and blank lines are ignored */
    const char *pcomment = "# a comment\n\ncontext = default  # trailing\nallow = send_destination:org.example.Svc\n";
    assert(strcmp(evals(pcomment, &send), "allow") == 0);

    /* receive: the default receive_type backbone allows all types; a deny-receive
       in a user context removes it for that recipient, matched against the
       sender's well-known names (set membership, unique as fallback) */
    const char *rp =
        "context = default\n"
        "allow = receive_type:signal\n"
        "context = user:0\n"
        "deny = receive_sender:org.wpa,receive_type:signal\n";
    const char *wpa[] = { "org.wpa" };
    sdbus_req r_root = { .op="receive", .uid=0, .msgtype="signal",
                         .sender_names=wpa, .n_sender_names=1, .sender_name=":1.5" };
    assert(!strcmp(evals(rp, &r_root), "deny"));       /* root recipient: denied */
    sdbus_req r_user = { .op="receive", .uid=1000, .msgtype="signal",
                         .sender_names=wpa, .n_sender_names=1, .sender_name=":1.5" };
    assert(!strcmp(evals(rp, &r_user), "allow"));      /* other uid: default allow */
    const char *othn[] = { "org.other" };
    sdbus_req r_other = { .op="receive", .uid=0, .msgtype="signal",
                          .sender_names=othn, .n_sender_names=1, .sender_name=":1.9" };
    assert(!strcmp(evals(rp, &r_other), "allow"));     /* different sender: allowed */
    /* a multi-name sender cannot evade the deny by owning the denied name second */
    const char *multi[] = { "org.other", "org.wpa" };
    sdbus_req r_multi = { .op="receive", .uid=0, .msgtype="signal",
                          .sender_names=multi, .n_sender_names=2, .sender_name=":1.7" };
    assert(!strcmp(evals(rp, &r_multi), "deny"));
    /* unique-name fallback when the sender owns no well-known name */
    sdbus_req r_uniq = { .op="receive", .uid=0, .msgtype="signal", .sender_name="org.wpa" };
    assert(!strcmp(evals(rp, &r_uniq), "deny"));

    /* has_recv_rule set when ANY rule (allow or deny) has a receive_* predicate,
       so an allow-whitelist still enables enforcement (engine default-denies the
       rest) -- not only an explicit deny-receive */
    sdbus_policy *pd = sdbus_policy_parse(rp);
    assert(pd && pd->has_recv_rule == 1);
    sdbus_policy_free(pd);
    sdbus_policy *pw = sdbus_policy_parse(
        "context = default\nallow = receive_sender:org.only\n");
    assert(pw && pw->has_recv_rule == 1);              /* allow-whitelist counts */
    sdbus_req r_wl = { .op="receive", .uid=1000, .msgtype="signal", .sender_name="org.other" };
    assert(!strcmp(sdbus_policy_eval(pw, &r_wl), "deny"));  /* not whitelisted -> default deny */
    sdbus_policy_free(pw);
    sdbus_policy *ps = sdbus_policy_parse(
        "context = default\nallow = send_type:signal\ndeny = send_type:method_call\n");
    assert(ps && ps->has_recv_rule == 0);              /* no receive rule -> skip */
    sdbus_policy_free(ps);

    printf("all sdbus_policy tests passed\n");
    return 0;
}
