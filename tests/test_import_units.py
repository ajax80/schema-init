#!/usr/bin/env python3
"""schema-import unit tests — pure translation + a temp-tree drain. No systemctl."""
import os, sys, tempfile, subprocess, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-import.py")
spec = importlib.util.spec_from_file_location("schema_import", MOD)
si = importlib.util.module_from_spec(spec); spec.loader.exec_module(si)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

# --- parse_unit ---
s = si.parse_unit(
    "[Unit]\nDescription=x\n\n[Service]\n# comment\nType=simple\n"
    "ExecStart=/usr/bin/foo \\\n  --flag a\nEnvironment=A=1\nEnvironment=B=2\n"
    "[Install]\nWantedBy=multi-user.target\n")
check("sections parsed", set(s) == {"Unit", "Service", "Install"})
check("line continuation joined", si._get_last(s["Service"], "ExecStart") == "/usr/bin/foo --flag a")
check("duplicate keys collected", si._get_all(s["Service"], "Environment") == ["A=1", "B=2"])

# --- unit_to_svc: simple daemon, no Restart, no User ---
body = si.unit_to_svc("foo", si.parse_unit(
    "[Service]\nExecStart=/usr/bin/foo -D --x\n[Install]\nWantedBy=multi-user.target\n"))
check("exec extracted", "exec=/usr/bin/foo\n" in body)
check("args split one per line", "args=-D\n" in body and "args=--x\n" in body)
check("no User -> needs_root", "needs_root=1\n" in body)
check("Restart absent -> no_restart", "no_restart=1\n" in body)
check("critical default", "critical=0\n" in body)

try:
    si.unit_to_svc("console-getty", si.parse_unit(
        "[Service]\nExecStart=-/sbin/agetty --noclear --keep-baud - 115200 $TERM\n"
        "StandardInput=tty\nTTYPath=/dev/console\n"))
    check("StandardInput=tty skipped", False)
except si.Skip as e:
    check("StandardInput=tty skipped", "StandardInput=tty" in str(e))

# --- Restart / Type / User / Environment ---
b2 = si.unit_to_svc("bar", si.parse_unit(
    "[Service]\nType=oneshot\nRestart=on-failure\nUser=nobody\n"
    "Environment=\"FOO=a b\" BAR=c\nExecStart=/bin/bar\n[Install]\nWantedBy=x\n"))
check("Type=oneshot", "oneshot=1\n" in b2)
check("Restart=on-failure -> restart kept", "no_restart" not in b2)
check("no RestartSec -> no restart_sec", "restart_sec" not in b2)
def _rs(v, restart="always"):
    return si.unit_to_svc("rs", si.parse_unit("[Service]\nExecStart=/bin/rs\nRestart=%s\nRestartSec=%s\n" % (restart, v)))
check("RestartSec=5 -> restart_sec=5", "restart_sec=5\n" in _rs("5"))
check("RestartSec=500ms rounds up to 1", "restart_sec=1\n" in _rs("500ms"))
check("RestartSec=1min 30s", "restart_sec=90\n" in _rs("1min 30s"))
check("RestartSec=0 -> none", "restart_sec" not in _rs("0"))
check("RestartSec with Restart=no -> none", "restart_sec" not in _rs("5", "no"))
check("RestartSec=2h clamped to 3600", "restart_sec=3600\n" in _rs("2h"))
check("User=nobody -> user=, no needs_root", "user=nobody\n" in b2 and "needs_root" not in b2)
# F44 GA chrony: User=chrony with ExecStart=! — systemd runs the main process as
# root (chronyd drops privileges itself); as user=chrony it dies "Not superuser".
for pfx in ("!", "+", "-!"):
    bp = si.unit_to_svc("chronyd", si.parse_unit(
        "[Service]\nType=notify\nUser=chrony\nExecStart=%s/usr/sbin/chronyd -n\n" % pfx))
    check("ExecStart=%s ignores User= -> needs_root" % pfx, "needs_root=1\n" in bp and "user=" not in bp)
bp = si.unit_to_svc("capd", si.parse_unit("[Service]\nUser=foo\nExecStart=!!/usr/bin/foo\n"))
check("ExecStart=!! keeps User= (ambient caps exist on F44)", "user=foo\n" in bp)
# + is full privileges: the sandbox goes too; ! only drops User=
_sb = "[Service]\nUser=foo\nNoNewPrivileges=yes\nPrivateTmp=yes\nProtectSystem=strict\nProtectHome=yes\nExecStart=%s/usr/bin/foo\n"
bp = si.unit_to_svc("sbx", si.parse_unit(_sb % "+"))
check("ExecStart=+ drops the sandbox", all(k + "=0\n" in bp for k in
      ("no_new_privs", "private_tmp", "protect_system", "protect_home")) and "unsandboxed" in bp)
bp = si.unit_to_svc("sbx", si.parse_unit(_sb.replace("ExecStart=", "ExecStartPre=/usr/bin/setup\nExecStart=") % "+"))
check("ExecStart=+ with a plain ExecStartPre keeps the sandbox",
      "protect_system=full\n" in bp and "kept the sandbox" in bp)
bp = si.unit_to_svc("sbx", si.parse_unit(_sb % "!"))
check("ExecStart=! keeps the sandbox", "protect_system=full\n" in bp and "private_tmp=1\n" in bp)
check("quoted env pair", "env=FOO=a b\n" in b2)
check("second env pair", "env=BAR=c\n" in b2)

# --- EnvironmentFile -> env_file= + runtime expansion ---
be = si.unit_to_svc("chronyd", si.parse_unit(
    "[Service]\nEnvironment=A=1\nEnvironmentFile=-/etc/sysconfig/chronyd\n"
    "ExecStart=/usr/sbin/chronyd -n $OPTIONS --a=${A}\n[Install]\nWantedBy=x\n"))
check("env_file= emitted with - prefix", "env_file=-/etc/sysconfig/chronyd\n" in be)
check("$OPTIONS kept raw for PID1", "args=$OPTIONS\n" in be and "args=--a=${A}\n" in be)
check("expand_args=1 set", "expand_args=1\n" in be)
check("Environment= still env=", "env=A=1\n" in be)
check("no dropped-EnvironmentFile note", "dropped EnvironmentFile" not in be and "unresolved" not in be)
bn = si.unit_to_svc("irq", si.parse_unit(
    "[Service]\nEnvironmentFile=/etc/sysconfig/irq\nExecStart=/usr/sbin/irq --foreground\n[Install]\nWantedBy=x\n"))
check("env_file without $ args -> no expand_args", "env_file=/etc/sysconfig/irq\n" in bn and "expand_args" not in bn)
br = si.unit_to_svc("r", si.parse_unit(
    "[Service]\nEnvironmentFile=/etc/a\nEnvironmentFile=\nEnvironmentFile=%h/x\n"
    "ExecStart=/bin/r $X\n[Install]\nWantedBy=x\n"))
check("empty EnvironmentFile= resets; specifier dropped with note",
      "env_file=" not in br and "dropped EnvironmentFile=%h/x" in br and "dropped 1 unresolved" in br)
try:
    si.unit_to_svc("v", si.parse_unit(
        "[Service]\nEnvironmentFile=/etc/v\nExecStart=$BIN -x\n[Install]\nWantedBy=x\n"))
    check("variable binary with env file skipped", False)
except si.Skip:
    check("variable binary with env file skipped", True)
bb = si.unit_to_svc("bx", si.parse_unit(
    "[Service]\nEnvironment=BIN=/usr/sbin/x\nEnvironmentFile=-/etc/sysconfig/x\n"
    "ExecStart=${BIN} -n $OPTS\n[Install]\nWantedBy=x\n"))
check("binary from Environment= resolved even with env file",
      "exec=/usr/sbin/x\n" in bb and "args=$OPTS\n" in bb and "expand_args=1\n" in bb)

# --- ExecStartPre -> exec_pre= ---
bp = si.unit_to_svc("sssd", si.parse_unit(
    "[Service]\nUser=sssd\nExecStartPre=+-/bin/chown -f -R root:sssd /etc/sssd\n"
    "ExecStartPre=+-/bin/sh -c \"/bin/chown -f -h sssd:sssd /var/lib/sss/db/*.ldb\"\n"
    "ExecStart=/usr/bin/sssd -i\n[Install]\nWantedBy=x\n"))
check("+- prefix kept, plain words unquoted", "exec_pre=+-/bin/chown -f -R root:sssd /etc/sssd\n" in bp)
check("sh -c script double-quoted", 'exec_pre=+-/bin/sh -c "/bin/chown -f -h sssd:sssd /var/lib/sss/db/*.ldb"\n' in bp)
check("no dropped-ExecStartPre note", "ExecStartPre" not in bp)
bq = si.unit_to_svc("q", si.parse_unit(
    "[Service]\nExecStartPre=/bin/sh -c \"grep '^X' /etc/a && echo \\\"y\\\" \\\\ z\"\n"
    "ExecStart=/bin/q\n[Install]\nWantedBy=x\n"))
check("quotes and backslashes escaped", 'exec_pre=/bin/sh -c "grep \'^X\' /etc/a && echo \\"y\\" \\\\ z"\n' in bq)
bm = si.unit_to_svc("mariadb", si.parse_unit(
    "[Service]\nExecStartPre=/usr/libexec/mariadb-prepare-db-dir %n\nExecStartPre=/bin/sh -c \"echo 100%% %t\"\n"
    "ExecStart=/bin/m\n[Install]\nWantedBy=x\n"))
check("%n / %t / %% specifiers expanded", "exec_pre=/usr/libexec/mariadb-prepare-db-dir mariadb.service\n" in bm
      and 'exec_pre=/bin/sh -c "echo 100% /run"\n' in bm)
try:
    si.unit_to_svc("u", si.parse_unit("[Service]\nExecStartPre=/bin/a %h\nExecStart=/bin/u\n[Install]\nWantedBy=x\n"))
    check("unknown specifier skipped", False)
except si.Skip:
    check("unknown specifier skipped", True)
be = si.unit_to_svc("e", si.parse_unit("[Service]\nExecStart=/bin/e --pid %t/e.pid --unit %n 100%%\n[Install]\nWantedBy=x\n"))
check("ExecStart specifiers expanded", "args=/run/e.pid\n" in be and "args=e.service\n" in be and "args=100%\n" in be)
for bad in ("/bin/a 100%", "/bin/a %h"):
    try:
        si.unit_to_svc("u", si.parse_unit("[Service]\nExecStart=%s\n[Install]\nWantedBy=x\n" % bad))
        check("unmappable ExecStart specifier skipped: " + bad, False)
    except si.Skip:
        check("unmappable ExecStart specifier skipped: " + bad, True)
bt = si.timer_to_svc("foo", si.parse_unit("[Timer]\nOnCalendar=daily\nUnit=bar.service\n"),
                     si.parse_unit("[Service]\nType=oneshot\nExecStartPre=/bin/prep %n\nExecStart=/bin/bar %N\n"), unit="bar")
check("timer: specifiers name the service, not the timer", "exec_pre=/bin/prep bar.service\n" in bt and "args=bar\n" in bt)
bo = si.unit_to_svc("o", si.parse_unit(
    "[Service]\nExecStartPre=/bin/a\nExecStartPre=+/bin/b\nExecStart=/bin/o\n[Install]\nWantedBy=x\n"))
check("plain before + warns of reorder", "order changed" in bo)
br2 = si.unit_to_svc("r2", si.parse_unit(
    "[Service]\nExecStartPre=/bin/a\nExecStartPre=\nExecStartPre=/bin/b\nExecStart=/bin/r\n[Install]\nWantedBy=x\n"))
check("empty ExecStartPre= resets", "exec_pre=/bin/a" not in br2 and "exec_pre=/bin/b\n" in br2)
bv = si.unit_to_svc("v2", si.parse_unit(
    "[Service]\nEnvironmentFile=/etc/v\nExecStartPre=/bin/prep $OPTS\nExecStart=/bin/v\n[Install]\nWantedBy=x\n"))
check("$VAR only in exec_pre still sets expand_args", "exec_pre=/bin/prep $OPTS\n" in bv and "expand_args=1\n" in bv)
for bad, why in (("@/bin/a x", "prefix"), ("/bin/sh -c \"open", "quotes"), ("no-such-cmd-zz", "not found")):
    try:
        si.unit_to_svc("b", si.parse_unit("[Service]\nExecStartPre=%s\nExecStart=/bin/b\n[Install]\nWantedBy=x\n" % bad))
        check("ExecStartPre skip: " + why, False)
    except si.Skip:
        check("ExecStartPre skip: " + why, True)
try:
    si.unit_to_svc("n9", si.parse_unit("[Service]\n" + "ExecStartPre=/bin/true\n" * 9 + "ExecStart=/bin/b\n[Install]\nWantedBy=x\n"))
    check("9 ExecStartPre skipped", False)
except si.Skip:
    check("9 ExecStartPre skipped", True)

# --- ExecStart prefix stripping ---
b3 = si.unit_to_svc("p", si.parse_unit("[Service]\nExecStart=@-/bin/p arg\n[Install]\nWantedBy=x\n"))
check("prefix chars stripped", "exec=/bin/p\n" in b3 and "args=arg\n" in b3)

# --- $VAR expansion / dropping ---
b4 = si.unit_to_svc("v", si.parse_unit(
    "[Service]\nEnvironment=OPTS=-x\nExecStart=/bin/v $OPTS $UNSET -z\n[Install]\nWantedBy=x\n"))
check("known $VAR expanded", "args=-x\n" in b4)
check("unresolved $VAR dropped", "$UNSET" not in b4 and "args=-z\n" in b4)
check("drop noted", "dropped 1 unresolved" in b4)

# --- hardening knobs: always all four, explicit ---
def hard(txt):
    return si.unit_to_svc("h", si.parse_unit("[Service]\nExecStart=/bin/h\n" + txt + "[Install]\nWantedBy=x\n"))
h0 = hard("")
check("no directives -> four explicit =0",
      all(k + "=0\n" in h0 for k in ("no_new_privs", "private_tmp", "protect_system", "protect_home")))
check("no directives -> no warning", si.WARN_PREFIX not in h0)
h1 = hard("NoNewPrivileges=yes\nPrivateTmp=true\nProtectSystem=yes\nProtectHome=yes\n")
check("all yes -> four =1",
      all(k + "=1\n" in h1 for k in ("no_new_privs", "private_tmp", "protect_system", "protect_home")))
check("all yes -> no warning", si.WARN_PREFIX not in h1)
check("ProtectSystem=full", "protect_system=full\n" in hard("ProtectSystem=full\n"))
hs = hard("ProtectSystem=strict\n")
check("ProtectSystem=strict -> full + warning",
      "protect_system=full\n" in hs and si.WARN_PREFIX + "ProtectSystem=strict" in hs)
for v in ("read-only", "tmpfs"):
    hh = hard("ProtectHome=%s\n" % v)
    check("ProtectHome=%s -> 0 + warning" % v,
          "protect_home=0\n" in hh and si.WARN_PREFIX + "ProtectHome=" + v in hh)
hn = hard("NoNewPrivileges=maybe\n")
check("unknown bool -> 0 + warning", "no_new_privs=0\n" in hn and "NoNewPrivileges=maybe" in hn)
check("last assignment wins", "private_tmp=0\n" in hard("PrivateTmp=yes\nPrivateTmp=no\n"))
hw = hard("ProtectSystem=strict\nProtectHome=tmpfs\n")
check("warnings lead the file", hw.startswith(si.WARN_PREFIX) and hw.count(si.WARN_PREFIX) == 2)

# --- SocketBindAllow/Deny= -> landlock_bind_tcp= ---
si._landlock_tcp_ok = lambda: True
# ReadWritePaths= under a read-only protect_system/protect_home level: schema-init
# has no writable exception, so the level is lowered and warned
hp = hard("User=pihole\nProtectSystem=full\nReadWriteDirectories=/etc/pihole\n")
check("pihole-FTL: ReadWriteDirectories=/etc/pihole lowers full -> 1, warned",
      "protect_system=1\n" in hp and "protect_system=full\n" not in hp
      and si.WARN_PREFIX + "ReadWriteDirectories=/etc/pihole" in hp)
hp = hard("ProtectSystem=strict\nReadWritePaths=/etc/x\n")
check("strict + /etc path -> 1", "protect_system=1\n" in hp and "/etc/x" in hp)
hp = hard("ProtectSystem=yes\nReadWritePaths=-/usr/local/state\n")
check("ProtectSystem=yes + -/usr path -> 0", "protect_system=0\n" in hp and "/usr/local/state" in hp)
hp = hard("ProtectSystem=full\nProtectHome=yes\nReadWritePaths=/ /run\n")
check("a parent of the read-only dirs (/, /run) -> unchanged: the narrower mount wins",
      "protect_system=full\n" in hp and "protect_home=1\n" in hp)
hp = hard("ProtectSystem=full\nProtectHome=yes\nReadWritePaths=/var/lib/x /run/x\n")
check("paths outside the read-only dirs -> unchanged, no warning",
      "protect_system=full\n" in hp and "protect_home=1\n" in hp and si.WARN_PREFIX not in hp)
hp = hard("ProtectHome=yes\nReadWritePaths=/home/x\n")
check("ProtectHome=yes + /home path -> protect_home=0, warned", "protect_home=0\n" in hp and "/home/x" in hp)
hp = hard("ProtectSystem=full\nReadWritePaths=/etc/a\nReadWritePaths=\nReadWritePaths=/var/b\n")
check("ReadWritePaths= empty resets", "protect_system=full\n" in hp)

def sb(txt):
    return hard(txt)
s1 = sb("SocketBindAllow=tcp:443\nSocketBindAllow=80\nSocketBindDeny=any\n")
check("SocketBind allow-list over deny any -> landlock_bind_tcp (+0 for ephemeral)",
      "landlock_bind_tcp=0,80,443\n" in s1)
check("SocketBind deny any -> UDP-not-enforced warning", si.WARN_PREFIX + "SocketBindDeny" in s1 and "UDP" in s1)
s2 = sb("SocketBindAllow=tcp:8000-8002\nSocketBindDeny=tcp\n")
check("port range expanded, TCP-only deny -> no warning",
      "landlock_bind_tcp=0,8000,8001,8002\n" in s2 and si.WARN_PREFIX not in s2)
check("deny all TCP, no allow -> ephemeral only", "landlock_bind_tcp=0\n" in sb("SocketBindDeny=tcp\n"))
check("allow without deny -> nothing (systemd allows anyway)",
      "\nlandlock_bind_tcp=" not in sb("SocketBindAllow=tcp:443\n"))
s3 = sb("SocketBindAllow=tcp\nSocketBindDeny=any\n")
check("all TCP allowed -> no landlock_bind_tcp", "\nlandlock_bind_tcp=" not in s3)
s4 = sb("SocketBindAllow=10000-65535\nSocketBindDeny=tcp\n")
check("more than 15 ports -> not translated + warning",
      "\nlandlock_bind_tcp=" not in s4 and si.WARN_PREFIX + "SocketBindAllow" in s4)
s5 = sb("SocketBindDeny=tcp:22\n")
check("partial deny -> not translated + warning",
      "\nlandlock_bind_tcp=" not in s5 and si.WARN_PREFIX + "SocketBindDeny" in s5)
s6 = sb("SocketBindDeny=ipv6:tcp\n")
check("one-family deny -> not translated + warning",
      "\nlandlock_bind_tcp=" not in s6 and si.WARN_PREFIX + "SocketBindDeny" in s6)
s7 = sb("SocketBindAllow=ipv4:tcp:80\nSocketBindDeny=tcp\n")
check("one-family allow -> both families + warning",
      "landlock_bind_tcp=0,80\n" in s7 and si.WARN_PREFIX + "SocketBindAllow" in s7)
check("same ports both families -> no warning",
      si.WARN_PREFIX not in sb("SocketBindAllow=ipv4:tcp:80\nSocketBindAllow=ipv6:tcp:80\nSocketBindDeny=tcp\n"))
check("empty assignment resets the list",
      "landlock_bind_tcp=0,443\n" in sb("SocketBindAllow=80\nSocketBindAllow=\nSocketBindAllow=443\nSocketBindDeny=tcp\n"))
check("udp-only allow ignored", "landlock_bind_tcp=0\n" in sb("SocketBindAllow=udp:53\nSocketBindDeny=tcp\n"))
s8 = sb("SocketBindAllow=tcp:http\nSocketBindDeny=tcp\n")
check("bad rule -> not translated + warning", "\nlandlock_bind_tcp=" not in s8 and "tcp:http" in s8)
s9 = si.unit_to_svc("p", si.parse_unit(
    "[Service]\nExecStart=+/bin/p\nSocketBindAllow=443\nSocketBindDeny=tcp\n[Install]\nWantedBy=x\n"))
check("ExecStart=+ keeps SocketBind (cgroup-level in systemd)", "landlock_bind_tcp=0,443\n" in s9)

s10 = sb("SocketBindAllow=ipv4\nSocketBindAllow=ipv6:tcp:443\nSocketBindDeny=tcp\n")
check("one family all, other limited -> not translated, honest warning",
      "\nlandlock_bind_tcp=" not in s10 and "not translated: one address family" in s10
      and "applied to both" not in s10)
for bad in ("tcp:", "ipv4:", "ipv6:tcp:", "tcp:80:"):
    sx = sb("SocketBindDeny=%s\n" % bad)
    check("trailing colon %r rejected like systemd" % bad,
          "\nlandlock_bind_tcp=" not in sx and "SocketBindDeny=%s not understood" % bad in sx)
s11 = sb("SocketBindAllow=443\nSocketBindAllow=\nSocketBindAllow=tcp:http\nSocketBindDeny=tcp\n")
check("bad-rule warning names only the bad rule", "SocketBindAllow=tcp:http not understood" in s11)
check("all TCP + ports allowed -> no bogus too-many warning",
      si.WARN_PREFIX not in sb("SocketBindAllow=tcp\nSocketBindAllow=443\nSocketBindDeny=tcp\n"))
check("huge range repeated -> too-many warning", "more than 15 ports" in
      sb("SocketBindAllow=1-65535\n" * 128 + "SocketBindDeny=tcp\n"))
s12 = si.unit_to_svc("p", si.parse_unit(
    "[Service]\nExecStart=+/bin/p\nSocketBindAllow=443\nSocketBindDeny=tcp\n[Install]\nWantedBy=x\n"))
check("ExecStart=+ warning admits SocketBind kept", "SocketBind kept" in s12)
si._landlock_tcp_ok = lambda: False
s13 = sb("SocketBindAllow=443\nSocketBindDeny=tcp\n")
check("no Landlock TCP -> not translated + warning",
      "\nlandlock_bind_tcp=" not in s13 and "kernel lacks Landlock" in s13)
si._landlock_tcp_ok = lambda: True

check("system mode: Environment= specifiers left alone",
      "env=FMT=100%%\n" in hard("Environment=FMT=100%%\n"))

# --- ExecStop= / ExecStopPost= -> exec_stop= / exec_stop_post= ---
si.set_user_mode(si.user_context("/home/u", 1000, "u", {"XDG_RUNTIME_DIR": "/run/user/1000"}))
q = si.unit_to_svc("probe", si.parse_unit(
    "[Service]\nType=notify\nExecStart=/usr/bin/podman run --name systemd-%N -d img\n"
    "ExecStop=/usr/bin/podman rm -v -f -i systemd-%N\n"
    "ExecStopPost=-/usr/bin/podman rm -v -f -i systemd-%N\n[Install]\nWantedBy=x\n"))
check("ExecStop mapped, specifiers expanded",
      "exec_stop=/usr/bin/podman rm -v -f -i systemd-probe\n" in q)
check("ExecStopPost keeps its - prefix",
      "exec_stop_post=-/usr/bin/podman rm -v -f -i systemd-probe\n" in q)
check("no ExecStop -> no exec_stop", "exec_stop" not in hard(""))
check("empty ExecStop resets",
      "exec_stop=" not in hard("ExecStop=/bin/a\nExecStop=\n"))
check("ExecStop=+ dropped with a warning, unit still imported",
      "exec_stop" not in hard("ExecStop=+/bin/a\n") and "ExecStop not translated" in hard("ExecStop=+/bin/a\n"))
hv = hard("Environment=OPT=-q\nExecStop=/bin/foo $OPT\nExecStop=/bin/kill -s TERM $MAINPID\n")
check("ExecStop $VAR expanded from Environment=", "exec_stop=/bin/foo -q\n" in hv)
check("ExecStop with $MAINPID dropped with a warning",
      "exec_stop=/bin/kill" not in hv and si.WARN_PREFIX + "ExecStop=/bin/kill" in hv)
hl40 = hard("ExecStop=/bin/a " + " ".join(["x"] * 40) + "\n")
check("ExecStop over 32 words dropped with a warning", "exec_stop=" not in hl40 and "ExecStop" in hl40)
si.set_user_mode(None)
check("system mode: ExecStop not mapped", "exec_stop" not in hard("ExecStop=/bin/a\n"))

check("system mode: no main_pid_notify", "main_pid_notify" not in hard("Type=notify\nNotifyAccess=all\n"))
si.set_user_mode(si.user_context("/home/u", 1000, "u", {"XDG_RUNTIME_DIR": "/run/user/1000"}))
check("Type=notify + NotifyAccess=all -> main_pid_notify=1",
      "main_pid_notify=1\n" in hard("Type=notify\nNotifyAccess=all\n"))
check("Type=notify alone -> no main_pid_notify", "main_pid_notify" not in hard("Type=notify\n"))
check("NotifyAccess=all without notify -> no main_pid_notify",
      "main_pid_notify" not in hard("NotifyAccess=all\n"))
si.set_user_mode(None)

# --- podman limits -> verify_*_max= (user mode) ---
si.set_user_mode(si.user_context("/home/u", 1000, "u", {"XDG_RUNTIME_DIR": "/run/user/1000"}))
def pod(args, tail=""):
    return si.unit_to_svc("p", si.parse_unit(
        "[X-Container]\nImage=img\n[Service]\nType=notify\nExecStart=/usr/bin/podman run --name x %s -d img%s\n"
        "[Install]\nWantedBy=x\n" % (args, tail)))
check("verify_keys_forms: --memory=64m", "verify_memory_max=67108864\n" in pod("--memory=64m"))
check("verify_keys_forms: -m 1g", "verify_memory_max=1073741824\n" in pod("-m 1g"))
check("verify_keys_forms: --memory 512k", "verify_memory_max=524288\n" in pod("--memory 512k"))
check("verify_keys_forms: --cpus=1.5", "verify_cpu_max=150000 100000\n" in pod("--cpus=1.5"))
check("verify_keys_forms: --cpus 0.25", "verify_cpu_max=25000 100000\n" in pod("--cpus 0.25"))
check("verify_keys_forms: --pids-limit 100", "verify_pids_max=100\n" in pod("--pids-limit 100"))
check("verify_keys_forms: --pids-limit=50", "verify_pids_max=50\n" in pod("--pids-limit=50"))
bm = pod("--memory 64x")
check("verify_keys_forms: bad memory -> no key + warning",
      "verify_memory_max" not in bm and si.WARN_PREFIX in bm and "--memory" in bm)
check("verify_keys_forms: no limits -> no keys", "verify_" not in pod(""))
check("verify_keys_forms: args after the image are the container's",
      "verify_" not in pod("", " app --memory 64m"))
check("verify_keys_forms: not a Quadlet unit -> no keys",
      "verify_" not in hard("ExecStart=/usr/bin/podman run --memory 64m -d img\n"))
check("verify_keys_forms: non-podman exec -> no keys", "verify_" not in hard("ExecStart=/usr/bin/foo --memory 64m\n"))
check("verify: --cpus truncated like podman", "verify_cpu_max=28999 100000\n" in pod("--cpus=0.29"))
check("verify: zero means no limit", "verify_" not in pod("--memory=0 --cpus=0 --pids-limit=0"))
check("verify: attached -m512m", "verify_memory_max=536870912\n" in pod("-m512m"))
nf = si.unit_to_svc("p", si.parse_unit("[X-Container]\nImage=img\nExec=a b c d e f g\n[Service]\nType=notify\nExecStart=/usr/bin/podman run --memory 64m -d img\n"))
check("verify: image position unknown -> warning, not silence", "verify_" not in nf and "not verified" in nf)
nn = si.unit_to_svc("p", si.parse_unit("[X-Container]\nImage=img\n[Service]\nType=simple\nExecStart=/usr/bin/podman run --memory 64m img\n"))
check("verify: no keys without Type=notify", "verify_" not in nn)
# final review (I2-I4)
def quad(xc, args, image, tail=""):
    return si.unit_to_svc("q", si.parse_unit(
        "[X-Container]\n%s\n[Service]\nType=notify\nExecStart=/usr/bin/podman run --name x %s -d %s%s\n"
        "[Install]\nWantedBy=x\n" % (xc, args, image, tail)))
qh = quad("Image=nginx\nExec=app run", "--memory 1g --hostname nginx --cpus=1.5 --pids-limit 100", "nginx", " app run")
check("I3: image found by position, not first match",
      "verify_memory_max=1073741824\n" in qh and "verify_cpu_max=150000 100000\n" in qh and "verify_pids_max=100\n" in qh)
qi = quad("Image=web.image", "--memory 64m", "docker.io/library/nginx:latest")
check("I3: .image unit -> resolved image still found", "verify_memory_max=67108864\n" in qi)
for form, want in (("1.5g", 1610612736), ("512MiB", 536870912), ("64mb", 67108864), ("1t", 1 << 40), ("2048", 2048)):
    check("I4: memory %s" % form, "verify_memory_max=%d\n" % want in pod("--memory %s" % form))
check("I4: cpus .5", "verify_cpu_max=50000 100000\n" in pod("--cpus .5"))
check("I2: Quadlet unit without TimeoutStopSec -> stop grace past podman's 10 s",
      "stop_timeout_sec=15\n" in quad("Image=img", "", "img"))
check("I2: StopTimeout= honoured", "stop_timeout_sec=35\n" in quad("Image=img\nStopTimeout=30", "", "img"))
check("I2: TimeoutStopSec= still wins", "stop_timeout_sec=7\n" in si.unit_to_svc("q", si.parse_unit(
      "[X-Container]\nImage=img\n[Service]\nType=notify\nTimeoutStopSec=7\nExecStart=/usr/bin/podman run -d img\n")))
si.set_user_mode(None)
check("verify_keys_forms: system mode -> no keys", "verify_" not in pod("--memory=64m"))

# --- Limit*= -> limit_*= ---
hl = hard("LimitNOFILE=1024:524288\nLimitMEMLOCK=infinity\nLimitNICE=-19\nLimitRTPRIO=70\nLimitCORE=8M\n")
check("Limit*= translated",
      all(x in hl for x in ("limit_nofile=1024:524288\n", "limit_memlock=infinity\n",
                            "limit_nice=-19\n", "limit_rtprio=70\n", "limit_core=8M\n")))
check("Limit*= -> no warning", si.WARN_PREFIX not in hl)
hb = hard("LimitCPU=30s\nLimitNOFILE=-5\nLimitAS=1P\n")
check("unsupported Limit values dropped + warned",
      "limit_" not in hb and hb.count(si.WARN_PREFIX) == 3)
hr = hard("LimitNOFILE=infinity:4096\nLimitSTACK=8192:1024\nLimitNICE=-25\nLimitAS=99999999999999T\n")
check("Limit values PID 1 rejects are dropped + warned",
      "limit_" not in hr and hr.count(si.WARN_PREFIX) == 4)
check("Limit last assignment wins", "limit_nofile=2048\n" in hard("LimitNOFILE=1024\nLimitNOFILE=2048\n"))

# --- ratholes -> Skip ---
def skipped(name, txt):
    try:
        si.unit_to_svc(name, si.parse_unit(txt))
        return False
    except si.Skip:
        return True
bn = si.unit_to_svc("n", si.parse_unit("[Service]\nType=notify\nExecStart=/bin/n\n"))
check("Type=notify -> notify=1", "notify=1\n" in bn and "oneshot=1" not in bn)
bnr = si.unit_to_svc("nr", si.parse_unit("[Service]\nType=notify-reload\nExecStart=/bin/nr\n"))
check("Type=notify-reload -> notify=1", "notify=1\n" in bnr)
bw = si.unit_to_svc("w", si.parse_unit("[Service]\nType=notify\nWatchdogSec=3min\nExecStart=/bin/w\n"))
check("WatchdogSec=3min -> watchdog_sec=180", "watchdog_sec=180\n" in bw)
bw = si.unit_to_svc("w", si.parse_unit("[Service]\nType=notify\nWatchdogSec=1min 30s\nExecStart=/bin/w\n"))
check("WatchdogSec=1min 30s -> 90", "watchdog_sec=90\n" in bw)
bw = si.unit_to_svc("w", si.parse_unit("[Service]\nType=notify\nWatchdogSec=500ms\nExecStart=/bin/w\n"))
check("WatchdogSec=500ms rounds up to 1", "watchdog_sec=1\n" in bw)
bw = si.unit_to_svc("w", si.parse_unit("[Service]\nType=notify\nWatchdogSec=0\nExecStart=/bin/w\n"))
check("WatchdogSec=0 -> off", "watchdog_sec" not in bw)
bw = si.unit_to_svc("w", si.parse_unit("[Service]\nType=notify\nWatchdogSec=30bogus\nExecStart=/bin/w\n"))
check("WatchdogSec bad unit -> warn, off", "watchdog_sec=" not in bw and "WatchdogSec=30bogus unrecognised" in bw)
bw = si.unit_to_svc("w", si.parse_unit("[Service]\nWatchdogSec=30\nExecStart=/bin/w\n"))
check("WatchdogSec without notify -> dropped w/ warn", "watchdog_sec=" not in bw and "without Type=notify" in bw)
def stopsvc(extra):
    return si.unit_to_svc("t", si.parse_unit("[Service]\n" + extra + "ExecStart=/bin/t\n"))
check("TimeoutStopSec=30 -> stop_timeout_sec=30", "stop_timeout_sec=30\n" in stopsvc("TimeoutStopSec=30\n"))
check("TimeoutStopSec=2min -> 120", "stop_timeout_sec=120\n" in stopsvc("TimeoutStopSec=2min\n"))
check("TimeoutSec= sets the stop grace too", "stop_timeout_sec=45\n" in stopsvc("TimeoutSec=45\n"))
check("later TimeoutStopSec wins over TimeoutSec", "stop_timeout_sec=20\n" in stopsvc("TimeoutSec=45\nTimeoutStopSec=20\n"))
check("later TimeoutSec wins over TimeoutStopSec", "stop_timeout_sec=120\n" in stopsvc("TimeoutStopSec=30\nTimeoutSec=120\n"))
b = stopsvc("TimeoutStopSec=infinity\n")
check("TimeoutStopSec=infinity -> 300 w/ warn", "stop_timeout_sec=300\n" in b and "no limit" in b)
check("TimeoutStopSec=0 -> 300 w/ warn", "stop_timeout_sec=300\n" in stopsvc("TimeoutStopSec=0\n"))
check("TimeoutSec=0 warning names TimeoutSec", "TimeoutSec=0 (no limit)" in stopsvc("TimeoutSec=0\n"))
b = stopsvc("TimeoutStopSec=10min\n")
check("TimeoutStopSec=10min capped at 300 w/ warn", "stop_timeout_sec=300\n" in b and "capped at 300" in b)
b = stopsvc("TimeoutStopSec=30bogus\n")
check("TimeoutStopSec bad unit -> warn, default", "stop_timeout_sec=" not in b and "TimeoutStopSec=30bogus unrecognised" in b)
check("no TimeoutStopSec -> default", "stop_timeout_sec=" not in stopsvc(""))
bs = si.unit_to_svc("s", si.parse_unit("[Service]\nType=simple\nExecStart=/bin/s\n"))
check("Type=simple has no notify", "notify=" not in bs)
check("Type=forking skipped", skipped("f", "[Service]\nType=forking\nExecStart=/bin/f\n"))
check("Type=forking with %t PIDFile skipped", skipped("f", "[Service]\nType=forking\nPIDFile=%t/f.pid\nExecStart=/bin/f\n"))
bf = si.unit_to_svc("f", si.parse_unit("[Service]\nType=forking\nPIDFile=/run/f.pid\nExecStart=/bin/f\n"))
check("Type=forking + PIDFile -> pid_file", "pid_file=/run/f.pid\n" in bf and "notify=" not in bf)
check("Type=dbus without BusName skipped", skipped("d", "[Service]\nType=dbus\nExecStart=/bin/d\n"))
bd = si.unit_to_svc("nm", si.parse_unit("[Service]\nType=dbus\nBusName=org.freedesktop.NetworkManager\nExecStart=/usr/sbin/NetworkManager --no-daemon\n"))
check("Type=dbus -> ready_bus_name", "ready_bus_name=org.freedesktop.NetworkManager\n" in bd and "notify=" not in bd)
check("template skipped", skipped("t@", "[Service]\nExecStart=/bin/t\n"))
check("no ExecStart skipped", skipped("e", "[Service]\nType=simple\n"))

# --- .timer translation ---
for cal, want in (("daily", "00:00"), ("weekly", "Mon 00:00"), ("monthly", "1 00:00"),
                  ("Sun *-*-* 01:00:00", "Sun 01:00"), ("*-*-15 03:30", "15 03:30"),
                  ("hourly", ("interval", 3600)), ("*-*-* *:00:00", ("interval", 3600)),
                  ("Sun *-*-1..7 1:00:00", None), ("Mon..Fri 09:00", None), ("Mon *-*-15 01:00", None)):
    check("calendar %r -> %r" % (cal, want), si._calendar(cal) == want)
svcu = si.parse_unit("[Service]\nType=oneshot\nExecStart=/usr/sbin/logrotate /etc/logrotate.conf\n")
bt = si.timer_to_svc("logrotate", si.parse_unit("[Timer]\nOnCalendar=daily\nRandomizedDelaySec=1h\nPersistent=true\n"), svcu)
check("timer -> on_calendar + persistent, no oneshot/no_restart, notes drop",
      "on_calendar=00:00\n" in bt and "persistent=1\n" in bt and "oneshot=" not in bt
      and "no_restart=" not in bt and "dropped RandomizedDelaySec" in bt and "no [Install]" not in bt)
bt = si.timer_to_svc("mc", si.parse_unit("[Timer]\nOnBootSec=10min\nOnUnitInactiveSec=3h\n"), svcu)
check("OnBootSec/OnUnitInactiveSec -> on_boot_sec/on_active_sec", "on_boot_sec=600\n" in bt and "on_active_sec=10800\n" in bt)
bt = si.timer_to_svc("mc", si.parse_unit("[Timer]\nOnUnitActiveSec=1d\n"), svcu)
check("OnUnitActiveSec alone also arms the first fire", "on_boot_sec=86400\n" in bt and "on_active_sec=86400\n" in bt)
try:
    si.timer_to_svc("x", si.parse_unit("[Timer]\nOnCalendar=Mon..Fri 09:00\n"), svcu); ok = False
except si.Skip:
    ok = True
check("inexpressible OnCalendar skipped", ok)
bb = si.unit_to_svc("b", si.parse_unit("[Service]\nExecStart=/bin/bash -c 'echo $X; run ${Y}'\nEnvironment=Y=1\n"))
check("$VAR inside a word left for the shell, ${VAR} expanded", "args=echo $X; run 1\n" in bb)

# --- .socket translation ---
sl, sn = si.socket_lines(si.parse_unit(
    "[Socket]\nListenStream=/run/pcscd/pcscd.comm\nListenFIFO=/run/x.fifo\nListenDatagram=514\n"
    "ListenStream=[::1]:631\nSocketMode=0666\nSocketUser=pcscd\nSocketGroup=pcscd\nRemoveOnStop=on\n"))
check("socket listeners + owner/mode", sl == ["listen=stream:/run/pcscd/pcscd.comm", "listen=fifo:/run/x.fifo",
      "listen=dgram:514", "listen=stream:[::1]:631", "socket_mode=0666", "socket_user=pcscd",
      "socket_group=pcscd"] and sn == ["dropped RemoveOnStop"])
sl, _ = si.socket_lines(si.parse_unit("[Socket]\nListenStream=/a\nListenStream=\nListenSequentialPacket=@ISCSI\nListenStream=%t/b.sock\n"))
check("empty Listen resets, @abstract kept, %t -> /run", sl == ["listen=seqpacket:@ISCSI", "listen=stream:/run/b.sock"])
for bad in ("Accept=yes\nListenStream=22", "ListenNetlink=kobject-uevent 1", "ListenStream=%h/x",
            "ListenFIFO=@x", "SocketMode=0600", "ListenStream=" + "\nListenStream=".join("/%d" % i for i in range(5)),
            "ListenStream=localhost:80"):
    try:
        si.socket_lines(si.parse_unit("[Socket]\n" + bad + "\n")); ok = False
    except si.Skip:
        ok = True
    check("socket skipped: " + bad.split("\n")[0], ok)
cu = si.unit_to_svc("cups", si.parse_unit("[Service]\nExecStart=/usr/bin/cupsd -l\nType=notify\nRestart=on-failure\n"),
                    si.parse_unit("[Socket]\nListenStream=/run/cups/cups.sock\n"), lazy=True)
check("socket service: listen + lazy, no [Install] note", "listen=stream:/run/cups/cups.sock\n" in cu
      and "listen_lazy=1\n" in cu and "no [Install]" not in cu and "notify=1\n" in cu)

# --- Condition*/Assert* ---
si._FACTS = {"virt": "none", "virt_kind": None, "security": {"audit"}, "cgroup_v2": True,
             "controllers": {"cpu", "memory", "io"}, "cpus": 4, "arch": "x86-64"}
def cond(txt):
    return si.condition_lines(si.parse_unit("[Unit]\n" + txt + "\n"))
def cond_skip(txt):
    try:
        cond(txt); return False
    except si.Skip:
        return True
check("runtime conditions translated with | and !",
      cond("ConditionPathExists=!/etc/plasma-setup-done\nConditionKernelCommandLine=!rd.live.image\n"
           "AssertPathExists=/etc/mdadm.conf\nConditionDirectoryNotEmpty=|/etc/sssd/conf.d/\nConditionACPower=true")
      == ["condition=path_exists:!/etc/plasma-setup-done", "condition=kernel_cmdline:!rd.live.image",
          "condition=path_exists:/etc/mdadm.conf", "condition=dir_not_empty:|/etc/sssd/conf.d/",
          "condition=ac_power:true"])
check("empty Condition resets", cond("ConditionPathExists=/a\nConditionPathExists=\nConditionPathExists=/b")
      == ["condition=path_exists:/b"])
check("static true dropped", cond("ConditionVirtualization=no\nConditionCPUs=>1\nConditionVirtualization=!container\n"
      "ConditionControlGroupController=v2\nConditionCapability=CAP_SYS_ADMIN\nConditionSecurity=!selinux\n"
      "ConditionFirstBoot=no\nConditionArchitecture=x86-64") == [])
for t in ("ConditionVirtualization=vm", "ConditionVirtualization=yes", "ConditionVirtualization=kvm",
          "ConditionCPUs=>=8", "ConditionSecurity=selinux", "ConditionFirstBoot=yes",
          "ConditionControlGroupController=v1", "ConditionControlGroupController=cpu rdma",
          "ConditionNeedsUpdate=/etc", "ConditionSecurity=ima", "ConditionPathExists=%t/x",
          "ConditionArchitecture=arm64"):
    check("skipped: " + t, cond_skip(t))
check("static |-condition true satisfies the group",
      cond("ConditionVirtualization=|no\nConditionPathExists=|/nope") == [])
check("static |-condition false leaves runtime triggers",
      cond("ConditionVirtualization=|vm\nConditionPathExists=|/x") == ["condition=path_exists:|/x"])
check("only static |-conditions, none true -> skip", cond_skip("ConditionVirtualization=|vm\nConditionCPUs=|>8"))
check("BindsTo .device -> device path condition",
      cond("BindsTo=dev-virtio\\x2dports-org.qemu.guest_agent.0.device") ==
      ["condition=path_exists:/dev/virtio-ports/org.qemu.guest_agent.0"])
si._FACTS["virt"], si._FACTS["virt_kind"] = "kvm", "vm"
check("in a VM: Virtualization=vm/kvm hold, =no skips",
      cond("ConditionVirtualization=vm\nConditionVirtualization=kvm") == [] and cond_skip("ConditionVirtualization=no"))
si._FACTS["virt"], si._FACTS["virt_kind"] = "none", None
sm = si.unit_to_svc("smartd", si.parse_unit("[Unit]\nConditionVirtualization=no\nConditionPathExists=/etc/smartd.conf\n"
                                            "[Service]\nType=notify\nExecStart=/usr/sbin/smartd -n\n[Install]\nWantedBy=x\n"))
check("unit_to_svc carries condition lines", "condition=path_exists:/etc/smartd.conf\n" in sm)
tm = si.timer_to_svc("logrotate", si.parse_unit("[Unit]\nConditionACPower=true\n[Timer]\nOnCalendar=daily\n"),
                     si.parse_unit("[Unit]\nConditionPathExists=/etc/logrotate.conf\n[Service]\nType=oneshot\nExecStart=/usr/sbin/logrotate /etc/logrotate.conf\n"))
check("non-/dev device unit skipped", cond_skip("BindsTo=sys-subsystem-net-devices-wg0.device"))
try:
    si.timer_to_svc("t", si.parse_unit("[Unit]\nConditionPathExists=|/a\n[Timer]\nOnCalendar=daily\n"),
                    si.parse_unit("[Unit]\nConditionPathExists=|/b\nConditionPathExists=|/c\n[Service]\nExecStart=/bin/t\n")); ok = False
except si.Skip:
    ok = True
check("|-groups on both timer and service skipped", ok)
try:
    si.timer_to_svc("t", si.parse_unit("[Unit]\n" + "".join("ConditionPathExists=/t%d\n" % i for i in range(5)) + "[Timer]\nOnCalendar=daily\n"),
                    si.parse_unit("[Unit]\n" + "".join("ConditionPathExists=/s%d\n" % i for i in range(4)) + "[Service]\nExecStart=/bin/t\n")); ok = False
except si.Skip:
    ok = True
check("timer + service conditions over 8 skipped", ok)
check("timer + service conditions both kept", "condition=ac_power:true" in tm and "condition=path_exists:/etc/logrotate.conf" in tm)

# --- Requires=/After= -> dep= ---
known = {"dbus", "polkitd", "tuned", "auditd", "network-manager", "cups"}
dl, dn = si.dep_lines("tuned-ppd", si.parse_unit(
    "[Unit]\nRequires=tuned.service\nAfter=tuned.service network.target auditd.service\n[Service]\nType=dbus\nBusName=x\n"), known)
check("Type=dbus + Requires -> dbus, polkitd, then tuned; plain After= not a dep", dl == ["dep=dbus", "dep=polkitd", "dep=tuned"] and dn == [])
dl, _ = si.dep_lines("bd", si.parse_unit("[Service]\nType=dbus\nBusName=x\n"), {"dbus"})
check("Type=dbus without a polkitd.svc -> dbus only", dl == ["dep=dbus"])
fw = "[Unit]\nPartOf=dbus.service\n[Service]\nExecStart=/usr/bin/firewalld --nofork\n[Install]\nAlias=dbus-org.fedoraproject.FirewallD1.service\n"
dl, _ = si.dep_lines("firewalld", si.parse_unit(fw), known)
check("simple bus daemon (firewalld: PartOf=dbus + Alias=dbus-org.*) -> dbus, polkitd", dl == ["dep=dbus", "dep=polkitd"])
dl, _ = si.dep_lines("x", si.parse_unit("[Unit]\nPartOf=dbus.service\n[Service]\nExecStart=/bin/x\n"), known)
check("PartOf=dbus.service alone -> bus daemon", dl == ["dep=dbus", "dep=polkitd"])
# DefaultDependencies=yes = after local-fs.target: crond/auditd died before a
# separate /var subvol mounted. Only when the box has the barrier.
kf = known | {"mount-fstab"}
dl, _ = si.dep_lines("crond", si.parse_unit("[Service]\nExecStart=/usr/sbin/crond -n\n"), kf)
check("default deps -> dep=mount-fstab", dl == ["dep=mount-fstab"])
dl, _ = si.dep_lines("crond", si.parse_unit("[Service]\nExecStart=/usr/sbin/crond -n\n"), known)
check("no mount-fstab.svc -> no dangling dep", dl == [])
dl, _ = si.dep_lines("early", si.parse_unit("[Unit]\nDefaultDependencies=no\n[Service]\nExecStart=/bin/x\n"), kf)
check("DefaultDependencies=no -> no mount-fstab", dl == [])
for extra in ("After=local-fs.target systemd-tmpfiles-setup.service", "RequiresMountsFor=/var/log/audit"):
    dl, _ = si.dep_lines("auditd", si.parse_unit("[Unit]\nDefaultDependencies=no\n%s\n[Service]\nExecStart=/usr/bin/auditd\n" % extra), kf)
    check("DefaultDependencies=no + %s -> mount-fstab" % extra.split("=")[0], dl == ["dep=mount-fstab"])
many = "[Unit]\nRequires=" + " ".join("s%d.service" % i for i in range(9)) + "\n[Service]\nExecStart=/bin/x\n"
dl, _ = si.dep_lines("x", si.parse_unit(many), kf | {"s%d" % i for i in range(9)})
check("mount-fstab survives the 8-dep cap", dl[0] == "dep=mount-fstab" and len(dl) == 8)
dl, _ = si.dep_lines("x", si.parse_unit("[Service]\nBusName=org.x\nExecStart=/bin/x\n"), known)
check("BusName= without Type=dbus -> bus daemon", dl == ["dep=dbus", "dep=polkitd"])
dl, _ = si.dep_lines("x", si.parse_unit("[Service]\nExecStart=/bin/x\n[Install]\nAlias=x-alt.service\n"), known)
check("plain simple unit, non-dbus Alias -> no bus deps", dl == [])
dl, _ = si.dep_lines("polkit", si.parse_unit("[Service]\nBusName=org.freedesktop.PolicyKit1\nExecStart=/p\n"), known)
check("polkit never deps on itself via alias", dl == ["dep=dbus"])
dl, _ = si.dep_lines("x", si.parse_unit("[Unit]\nPartOf=dbus-broker.service\n[Service]\nExecStart=/bin/x\n"), known)
check("PartOf=dbus-broker.service -> bus daemon", dl == ["dep=dbus", "dep=polkitd"])
dl, _ = si.dep_lines("x", si.parse_unit("[Unit]\nPartOf=dbus.socket\n[Service]\nExecStart=/bin/x\n"), known)
check("PartOf=dbus.socket -> bus daemon", dl == ["dep=dbus", "dep=polkitd"])
dl, _ = si.dep_lines("firewalld", si.parse_unit(fw), {"dbus", "polkit", "firewalld"})
check("polkit queued under its raw name -> dep=polkit", dl == ["dep=dbus", "dep=polkit"])
dl, _ = si.dep_lines("polkit", si.parse_unit("[Service]\nType=dbus\nBusName=org.freedesktop.PolicyKit1\nExecStart=/p\n"), {"dbus", "polkit"})
check("raw-named polkit never deps on itself", dl == ["dep=dbus"])
dl, _ = si.dep_lines("tuned", si.parse_unit(
    "[Unit]\nRequires=dbus.service polkit.service NetworkManager.service ghost.service\n"
    "BindsTo=dbus.service\nWants=auditd.service\nRequisite=cups.socket getty@tty1.service tuned.service\n"), known)
check("aliases mapped, unknown/self/template/Wants dropped, deduped",
      dl == ["dep=dbus", "dep=polkitd", "dep=network-manager", "dep=cups"])
check("no known set -> no deps", si.dep_lines("x", si.parse_unit("[Unit]\nAfter=dbus.service\n"), None) == ([], []))
many = {"d%d" % i for i in range(10)}
dl, dn = si.dep_lines("x", si.parse_unit("[Unit]\nRequires=" + " ".join("d%d.service" % i for i in range(10)) + "\n"), many)
check("more than 8 deps -> first 8 + note", len(dl) == 8 and dn and "d8 d9" in dn[0])
tp = si.unit_to_svc("tuned-ppd", si.parse_unit("[Unit]\nRequires=tuned.service\n[Service]\nType=dbus\nBusName=net.hadess.PowerProfiles\n"
                    "ExecStart=/usr/sbin/tuned-ppd -l\n[Install]\nWantedBy=x\n"), known=known)
check("unit_to_svc emits dep= lines", "dep=dbus\ndep=polkitd\ndep=tuned\n" in tp)

# --- drain against a temp tree ---
tmp = tempfile.mkdtemp()
os.environ["MIGRATE_ROOT"] = tmp
os.environ["SCHEMA_STATE_DIR"] = os.path.join(tmp, "state")
os.environ["SCHEMA_SVC_DIR"] = os.path.join(tmp, "svc")
unitdir = os.path.join(tmp, "usr/lib/systemd/system")
os.makedirs(unitdir); os.makedirs(os.environ["SCHEMA_STATE_DIR"])
open(os.path.join(unitdir, "good.service"), "w").write(
    "[Service]\nExecStart=/usr/bin/good -D\n[Install]\nWantedBy=multi-user.target\n")
open(os.path.join(unitdir, "strict.service"), "w").write(
    "[Service]\nExecStart=/usr/bin/strict\nProtectSystem=strict\n[Install]\nWantedBy=x\n")
open(os.path.join(unitdir, "noisy.service"), "w").write(
    "[Service]\nType=forking\nExecStart=/usr/bin/noisy\n[Install]\nWantedBy=x\n")
open(os.path.join(unitdir, "tick.timer"), "w").write("[Timer]\nOnCalendar=weekly\nPersistent=true\nUnit=tock.service\n")
open(os.path.join(unitdir, "tock.service"), "w").write("[Service]\nType=oneshot\nExecStart=/usr/bin/tock\n")
open(os.path.join(unitdir, "bell.timer"), "w").write("[Timer]\nOnCalendar=daily\n")
open(os.path.join(unitdir, "bell.service"), "w").write("[Service]\nType=oneshot\nExecStart=/usr/bin/bell\n")
open(os.path.join(unitdir, "inst.timer"), "w").write("[Timer]\nOnCalendar=daily\nUnit=job@x.service\n")
open(si.queue_path(), "w").write("good\nnoisy\nghost\ntick.timer\nbell.service\nbell.timer\ninst.timer\n")

logged = []
counts = si.drain(units=["strict"], log=logged.append)
check("drain logs a WARN naming unit + directive",
      any(l.startswith("WARN    strict: ProtectSystem=strict") for l in logged))

counts = si.drain(log=lambda *_: None)
check("three imported (good, tick.timer, bell.timer)", counts["imported"] == 3)
bl = open(os.path.join(os.environ["SCHEMA_SVC_DIR"], "bell.svc")).read()
check("timer-driven bell.service skipped, bell.timer owns bell.svc", "on_calendar=00:00\n" in bl)
check("timer -> template instance skipped", os.path.exists(os.path.join(os.environ["SCHEMA_SVC_DIR"], "inst.svc.skipped")))
tk = open(os.path.join(os.environ["SCHEMA_SVC_DIR"], "tick.svc")).read()
check("tick.timer -> tick.svc running Unit= target weekly", "exec=/usr/bin/tock\n" in tk and "on_calendar=Mon 00:00\n" in tk)
check("three skipped (noisy, bell.service, inst.timer)", counts["skipped"] == 3)
check("one not-found", counts["not-found"] == 1)
check("good.svc written", os.path.exists(os.path.join(os.environ["SCHEMA_SVC_DIR"], "good.svc")))
check("noisy.svc NOT written", not os.path.exists(os.path.join(os.environ["SCHEMA_SVC_DIR"], "noisy.svc")))
stub = os.path.join(os.environ["SCHEMA_SVC_DIR"], "noisy.svc.skipped")
st = open(stub).read() if os.path.exists(stub) else ""
check("noisy.svc.skipped written with reason + commented unit",
      st.startswith("# schema-import skipped noisy: Type=forking") and "# ExecStart=/usr/bin/noisy\n" in st
      and all(l.startswith("#") for l in st.splitlines()))
remaining = [l.strip() for l in open(si.queue_path()) if l.strip()]
check("queue keeps only the transient miss", remaining == ["ghost"])

# re-drain the queue: only the transient miss remains
counts2 = si.drain(log=lambda *_: None)
check("re-drain: only ghost, still missing", counts2["not-found"] == 1 and counts2["imported"] == 0)
# direct re-import of an already-present unit hits the 'exists' path, writes nothing
counts3 = si.drain(units=["good"], log=lambda *_: None)
check("existing .svc not overwritten", counts3["exists"] == 1 and counts3["imported"] == 0)

# --- sockets through the drain ---
open(os.path.join(unitdir, "cupsd.socket"), "w").write("[Socket]\nListenStream=/run/cups/cups.sock\n[Install]\nWantedBy=sockets.target\n")
open(os.path.join(unitdir, "cupsd.service"), "w").write("[Service]\nExecStart=/usr/bin/cupsd -l\n[Install]\nWantedBy=multi-user.target\n")
open(os.path.join(unitdir, "mdns.socket"), "w").write("[Socket]\nListenStream=/run/mdns/socket\n")
open(os.path.join(unitdir, "mdns.service"), "w").write("[Service]\nExecStart=/usr/bin/mdnsd -s\n[Install]\nWantedBy=multi-user.target\n")
open(os.path.join(unitdir, "kcm.socket"), "w").write("[Socket]\nListenStream=/run/kcm\nService=kcm-responder.service\n")
open(os.path.join(unitdir, "kcm-responder.service"), "w").write("[Service]\nExecStart=/usr/libexec/kcm\n")
open(os.path.join(unitdir, "inetd.socket"), "w").write("[Socket]\nListenStream=2222\nAccept=yes\n")
open(os.path.join(unitdir, "inetd@.service"), "w").write("[Service]\nExecStart=/usr/bin/inetd\n")
open(os.path.join(unitdir, "dup.socket"), "w").write("[Socket]\nListenStream=/run/dup\n")
open(os.path.join(unitdir, "dup.service"), "w").write("[Service]\nExecStart=/usr/bin/good\n")
wants = os.path.join(tmp, "etc/systemd/system/multi-user.target.wants")
os.makedirs(wants)
os.symlink(os.path.join(unitdir, "cupsd.service"), os.path.join(wants, "cupsd.service"))
c4 = si.drain(units=["cupsd.socket", "mdns.socket", "mdns.service", "kcm.socket", "inetd.socket", "dup.socket"],
              log=lambda *_: None)
SD = os.environ["SCHEMA_SVC_DIR"]
cs = open(os.path.join(SD, "cupsd.svc")).read()
check("socket of a wants-enabled service -> eager", "listen=stream:/run/cups/cups.sock\n" in cs and "listen_lazy" not in cs)
md = open(os.path.join(SD, "mdns.svc")).read()
check("socket + its service queued together -> eager", "listen=stream:/run/mdns/socket\n" in md and "listen_lazy" not in md)
kc = open(os.path.join(SD, "kcm-responder.svc")).read()
check("Service= names the .svc; socket-only enable -> lazy", "exec=/usr/libexec/kcm\n" in kc and "listen_lazy=1\n" in kc)
check("Accept=yes socket skipped", os.path.exists(os.path.join(SD, "inetd.svc.skipped")))
check("binary already run by good.svc -> skipped, not a second copy",
      not os.path.exists(os.path.join(SD, "dup.svc")) and "already runs as good.svc" in open(os.path.join(SD, "dup.svc.skipped")).read())
check("socket drain counts", c4["imported"] == 3 and c4["skipped"] == 2 and c4["exists"] == 1)
os.makedirs(os.path.join(tmp, "usr/sbin"), exist_ok=True); os.makedirs(os.path.join(tmp, "usr/bin"), exist_ok=True)
open(os.path.join(tmp, "usr/bin/avahi-daemon"), "w").close()
os.symlink("../bin/avahi-daemon", os.path.join(tmp, "usr/sbin/avahi-daemon"))
open(os.path.join(SD, "avahi.svc"), "w").write("name=avahi\nexec=/usr/sbin/avahi-daemon\n")
open(os.path.join(unitdir, "avahi-daemon.service"), "w").write("[Service]\nExecStart=/usr/bin/avahi-daemon -s\n")
check("usr-merge alias counts as the same binary",
      si.import_one("avahi-daemon.socket")[0] == "not-found" and si.import_one("avahi-daemon")[0] == "skipped")

open(os.path.join(unitdir, "sshd.socket"), "w").write("[Socket]\nListenStream=22\nAccept=yes\n")
open(os.path.join(unitdir, "sshd.service"), "w").write("[Service]\nExecStart=/usr/sbin/sshd -D\n[Install]\nWantedBy=multi-user.target\n")
st, det = si.import_one("sshd")
check("an Accept=yes sibling socket does not block the service", st == "imported" and "listen=" not in det[1])
open(os.path.join(unitdir, "other.socket"), "w").write("[Socket]\nListenStream=/run/o\nService=elsewhere.service\n")
open(os.path.join(unitdir, "other.service"), "w").write("[Service]\nExecStart=/usr/bin/other\n")
st, det = si.import_one("other")
check("a same-name socket for another Service= is not attached", st == "imported" and "listen=" not in det[1])
# systemd only uses a socket that is enabled or pulled in (Debian ssh.socket ships disabled)
open(os.path.join(unitdir, "ssh.socket"), "w").write("[Socket]\nListenStream=22\nAccept=no\n[Install]\nWantedBy=sockets.target\n")
open(os.path.join(unitdir, "ssh.service"), "w").write("[Service]\nExecStart=/usr/sbin/sshd -D\n[Install]\nWantedBy=multi-user.target\n")
os.symlink(os.path.join(unitdir, "ssh.service"), os.path.join(wants, "ssh.service"))
st, det = si.import_one("ssh.service", force=True, queued=si.enabled_units())
check("--enabled: service enabled, its socket not -> no listen=", st == "imported" and "listen=" not in det[1])
st, det = si.import_one("ssh", force=True)
check("direct import: a disabled sibling socket is not attached", st == "imported" and "listen=" not in det[1])
st, det = si.import_one("ssh.service", force=True, queued=["ssh.service", "ssh.socket"])
check("socket queued in the same drain -> attached", "listen=stream:22\n" in det[1])
_sw = os.path.join(tmp, "etc/systemd/system/sockets.target.wants")
os.makedirs(_sw, exist_ok=True)
os.symlink(os.path.join(unitdir, "ssh.socket"), os.path.join(_sw, "ssh.socket"))
st, det = si.import_one("ssh.service", force=True, queued=si.enabled_units())
check("--enabled: socket enabled too -> listen= stays", st == "imported" and "listen=stream:22\n" in det[1])
os.unlink(os.path.join(_sw, "ssh.socket"))
open(os.path.join(unitdir, "ssh.service"), "w").write("[Unit]\nRequires=ssh.socket\n[Service]\nExecStart=/usr/sbin/sshd -D\n")
st, det = si.import_one("ssh", force=True)
check("Requires= pulls the socket in -> attached", "listen=stream:22\n" in det[1])
open(os.path.join(unitdir, "ssh.service"), "w").write("[Service]\nSockets=ssh.socket\nExecStart=/usr/sbin/sshd -D\n")
st, det = si.import_one("ssh", force=True)
check("[Service] Sockets= names the socket -> attached", "listen=stream:22\n" in det[1])
open(os.path.join(unitdir, "ssh.service"), "w").write("[Service]\nExecStart=/usr/sbin/sshd -D\n[Install]\nWantedBy=x\nAlso=foo.socket ssh.socket\n")
st, det = si.import_one("ssh", force=True)
check("[Install] Also= alone does not attach (enable-time only)", "listen=" not in det[1])
open(os.path.join(unitdir, "ssh.service"), "w").write("[Service]\nSockets=other.socket\nExecStart=/usr/sbin/sshd -D\n")
st, det = si.import_one("ssh", force=True)
check("Sockets= naming another socket -> same-name one not attached", "listen=" not in det[1])
os.unlink(os.path.join(wants, "ssh.service"))
open(os.path.join(unitdir, "goodcheck.service"), "w").write("[Service]\nType=oneshot\nExecStart=/usr/bin/good --check\n")
check("a oneshot sharing a daemon's binary still imports", si.import_one("goodcheck")[0] == "imported")

for n in ("rsyslog", "abrtd", "abrt-oops", "plasma-setup", "initial-setup"):
    open(os.path.join(unitdir, n + ".service"), "w").write("[Service]\nExecStart=/usr/sbin/%s -n\n[Install]\nWantedBy=x\n" % n)
    check("deny-listed: " + n, si.import_one(n)[0] == "skipped")
open(os.path.join(unitdir, "intel_lpmd.service"), "w").write("[Service]\nExecStart=/usr/bin/intel_lpmd --systemd\n[Install]\nWantedBy=x\n")
for vendor, flags, want in (("GenuineIntel", {"fpu"}, "skipped"), ("AuthenticAMD", {"hybrid_cpu"}, "skipped"),
                            ("GenuineIntel", {"fpu", "hybrid_cpu"}, "imported")):
    si._FACTS["cpu_vendor"], si._FACTS["cpu_flags"] = vendor, flags
    check("intel_lpmd quirk: %s %s -> %s" % (vendor, sorted(flags), want), si.import_one("intel_lpmd", force=True)[0] == want)
open(os.path.join(unitdir, "vmonly.service"), "w").write("[Unit]\nConditionVirtualization=vm\n[Service]\nExecStart=/usr/bin/vmonly\n[Install]\nWantedBy=x\n")
st, det = si.import_one("vmonly")
check("condition failing on this host -> skip stub", st == "skipped" and "does not hold" in det[0])

open(os.path.join(unitdir, "ppd.service"), "w").write("[Unit]\nRequires=pwr.service\nAfter=pwr.service\n[Service]\nExecStart=/usr/bin/ppd\n[Install]\nWantedBy=x\n")
open(os.path.join(unitdir, "pwr.service"), "w").write("[Service]\nExecStart=/usr/bin/pwr\n[Install]\nWantedBy=x\n")
si.drain(units=["ppd", "pwr"], log=lambda *_: None)
check("dep on a unit imported in the same drain", "dep=pwr\n" in open(os.path.join(SD, "ppd.svc")).read())
open(os.path.join(SD, "loopa.svc"), "w").write("name=loopa\nexec=/usr/bin/la\ndep=loopb\n")
open(os.path.join(SD, "mid.svc"), "w").write("name=mid\nexec=/usr/bin/mid\ndep=loopa\n")
open(os.path.join(unitdir, "loopb.service"), "w").write("[Unit]\nRequires=mid.service pwr.service\n[Service]\nExecStart=/usr/bin/lb\n[Install]\nWantedBy=x\n")
st, det = si.import_one("loopb")
check("dep closing a loop (loopb->mid->loopa->loopb) dropped, others kept",
      st == "imported" and "dep=mid" not in det[1] and "dep=pwr\n" in det[1] and "dependency loop" in det[1])
open(os.path.join(SD, "rot.svc"), "w").write("name=rot\nexec=/usr/sbin/rot\non_calendar=00:00\n")
check("a timer .svc is never a dep", "rot" not in si._known_svcs([]) and "pwr" in si._known_svcs([]))

for w, u in (("multi-user", "crond.service"), ("multi-user", "systemd-resolved.service"),
             ("timers", "fstrim.timer"), ("sockets", "dbus.socket"), ("sockets", "cups.socket"),
             ("getty", "getty@tty1.service"), ("network-online", "NetworkManager-wait-online.service"),
             ("graphical", "crond.service"), ("multi-user", "README")):
    d = os.path.join(tmp, "etc/systemd/system", w + ".target.wants")
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, u), "w").close()
os.makedirs(os.path.join(tmp, "etc/systemd/system/dev-x.device.wants"))
open(os.path.join(tmp, "etc/systemd/system/dev-x.device.wants/qga.service"), "w").close()
check("enabled_units: targets only, reclaimed/template/dup/non-unit dropped",
      si.enabled_units() == ["crond.service", "cupsd.service", "cups.socket", "fstrim.timer"])


act = os.path.join(tmp, "usr/share/dbus-1/system-services")
os.makedirs(act, exist_ok=True)
def actfile(fn, body): open(os.path.join(act, fn), "w").write(body)
actfile("org.x.Disks.service", "[D-BUS Service]\nName=org.x.Disks\nExec=/usr/libexec/disksd\nUser=root\nSystemdService=disksd.service\n")
actfile("org.x.Alias.service", "[D-BUS Service]\nName=org.x.Alias\nExec=/bin/false\nUser=root\nSystemdService=dbus-org.x.Alias.service\n")
actfile("org.x.Color.service", "[D-BUS Service]\nName=org.x.Color\nExec=/usr/libexec/colord\nUser=colord\nSystemdService=color.service\n")
actfile("org.x.Nounit.service", "[D-BUS Service]\nName=org.x.Nounit\nExec=/usr/libexec/n\nUser=root\nSystemdService=nounit.service\n")
actfile("org.x.Nosd.service", "[D-BUS Service]\nName=org.x.Nosd\nExec=/usr/libexec/k\nUser=root\n")
actfile("org.x.Pk.service", "[D-BUS Service]\nName=org.x.Pk\nExec=/usr/lib/polkit-1/polkitd\nUser=root\nSystemdService=polkit.service\n")
for u in ("disksd", "realalias", "color", "polkit"):
    open(os.path.join(unitdir, u + ".service"), "w").write("[Service]\nExecStart=/usr/bin/" + u + "\n")
os.symlink(os.path.join(unitdir, "realalias.service"), os.path.join(unitdir, "dbus-org.x.Alias.service"))
for n in ("disksd", "realalias", "color", "polkitd"):
    open(os.path.join(SD, n + ".svc"), "w").write("name=%s\nexec=/usr/bin/%s\n" % (n, n))
bd = os.path.join(tmp, "usr/local/share/dbus-1/system-services")
os.makedirs(bd, exist_ok=True)
actfile("org.x.Raw.service", "[D-BUS Service]\nName=org.x.Raw\nExec=/usr/bin/bluetoothd\nUser=root\nSystemdService=bluetooth.service\n")
open(os.path.join(unitdir, "bluetooth.service"), "w").write("[Service]\nExecStart=/usr/bin/bluetoothd\n")
open(os.path.join(SD, "bluetooth.svc"), "w").write("name=bluetooth\nexec=/usr/bin/bluetoothd\n")
actfile("org.x.NoUser.service", "[D-BUS Service]\nName=org.x.NoUser\nExec=/usr/libexec/disksd\nSystemdService=disksd.service\n")
actfile("org.x.NoUserFalse.service", "[D-BUS Service]\nName=org.x.NoUserFalse\nExec=/bin/false\nSystemdService=disksd.service\n")
open(os.path.join(act, "org.x.Latin.service"), "wb").write(b"[D-BUS Service]\n# caf\xe9\nName=org.x.Latin\n")
open(os.path.join(bd, "org.x.Pk.service"), "w").write("hand-made\n")
open(os.path.join(bd, "org.x.Gone.service"), "w").write(si.BUS_MARK + "\nName=org.x.Gone\n")
c = si.bus_overrides(log=lambda *_: None)
ov = lambda n: open(os.path.join(bd, n + ".service")).read()
check("bus override routes a .svc-backed name through PID 1",
      "Exec=%s disksd org.x.Disks /usr/libexec/disksd\n" % si.BUS_HELPER in ov("org.x.Disks")
      and "SystemdService=disksd.service\n" in ov("org.x.Disks") and ov("org.x.Disks").startswith(si.BUS_MARK + "\n"))
check("bus override follows a dbus-org alias to the real unit", " realalias org.x.Alias /bin/false\n" in ov("org.x.Alias"))
check("bus override skips non-root User, missing .svc, no SystemdService",
      not any(os.path.exists(os.path.join(bd, n + ".service")) for n in ("org.x.Color", "org.x.Nounit", "org.x.Nosd")))
check("bus override never touches a hand-made file", ov("org.x.Pk") == "hand-made\n" and c["skipped"] == 1)
check("stale generated override removed", not os.path.exists(os.path.join(bd, "org.x.Gone.service")) and c["removed"] == 1)
check("bus override uses the raw-stem .svc when it exists (bluetooth, not bluetoothd)", " bluetooth org.x.Raw " in ov("org.x.Raw"))
check("missing User= skipped unless the daemon is /bin/false",
      not os.path.exists(os.path.join(bd, "org.x.NoUser.service")) and "User=root\n" in ov("org.x.NoUserFalse"))
check("undecodable activation file skipped, pass still completes", not os.path.exists(os.path.join(bd, "org.x.Latin.service")))
check("no .tmp left behind", not any(f.endswith(".tmp") for f in os.listdir(bd)))
check("bus override rerun is a no-op", si.bus_overrides(log=lambda *_: None) == {"written": 0, "kept": 4, "removed": 0, "skipped": 1})
# schema-pi's trial run writes overrides to scratch on a live root: the bus must not be told to reload
_calls, _ud, _orig = [], si._unit_dirs(), (si._unit_dirs, si._root, si.act_dir, si.subprocess, si.bus_dir)
class _FakeSP:
    SubprocessError, DEVNULL = subprocess.SubprocessError, subprocess.DEVNULL
    @staticmethod
    def run(a, **k): _calls.append(a)
si._unit_dirs, si._root, si.act_dir, si.subprocess = (lambda: _ud), (lambda: "/"), (lambda: act), _FakeSP
os.environ["SCHEMA_BUS_OVERRIDE_DIR"] = tempfile.mkdtemp()
_c = si.bus_overrides(log=lambda *_: None)
os.environ.pop("SCHEMA_BUS_OVERRIDE_DIR")
check("redirected override dir on / -> written, no bus reload", _c["written"] > 0 and not _calls)
_bd = tempfile.mkdtemp()
si.bus_dir = lambda: _bd
si.bus_overrides(log=lambda *_: None)
check("default override dir on / -> bus reloaded", len(_calls) == 1 and "ReloadConfig" in " ".join(_calls[0]))
si._unit_dirs, si._root, si.act_dir, si.subprocess, si.bus_dir = _orig
os.remove(os.path.join(SD, "disksd.svc"))
si.bus_overrides(log=lambda *_: None)
check("override dropped when its .svc goes away", not os.path.exists(os.path.join(bd, "org.x.Disks.service")))

for unit, why in (("ExecStart=/bin/x " + " ".join(["a"] * 63), "arguments (limit 62)"),
                  ("ExecStartPre=/bin/p " + " ".join(["a"] * 32) + "\nExecStart=/bin/x", "words (limit 32)")):
    try:
        si.unit_to_svc("big", si.parse_unit("[Service]\n" + unit + "\n")); check("over-cap skipped: " + why, False)
    except si.Skip as e:
        check("over-cap skipped: " + why, why in str(e))
try:
    si.unit_to_svc("env", si.parse_unit("[Service]\nEnvironment=" + " ".join("K%d=v" % i for i in range(17)) + "\nExecStart=/bin/x\n"))
    check("over-cap skipped: 17 Environment= variables", False)
except si.Skip as e:
    check("over-cap skipped: 17 Environment= variables", "limit 16" in str(e))
check("repeated Environment= key counts once, last wins", si.unit_to_svc("env", si.parse_unit(
    "[Service]\nEnvironment=" + " ".join("K%d=v" % i for i in range(16)) + "\nEnvironment=K0=last\nExecStart=/bin/x\n")).count("\nenv=K") == 16)
check("16 Environment= variables kept", si.unit_to_svc("env", si.parse_unit(
    "[Service]\nEnvironment=" + " ".join("K%d=v" % i for i in range(16)) + "\nExecStart=/bin/x\n")).count("\nenv=K") == 16)
check("62 args kept", si.unit_to_svc("big", si.parse_unit("[Service]\nExecStart=/bin/x " + " ".join(["a"] * 62) + "\n")).count("args=a\n") == 62)

try:
    si.unit_to_svc("long", si.parse_unit("[Service]\nExecStart=/bin/x " + "a" * 600 + "\n")); check("over-long line skipped", False)
except si.Skip as e:
    check("over-long line skipped", "args= line is" in str(e))

_names = ["d%03d%s" % (i, "x" * 20) for i in range(40)]
_lb = si.unit_to_svc("deps", si.parse_unit("[Unit]\nRequires=" + " ".join(n + ".service" for n in _names)
                                           + "\n[Service]\nExecStart=/bin/x\n"), known=set(_names))
check("long notes comment does not skip the unit",
      "exec=/bin/x\n" in _lb and max(len(l) for l in _lb.splitlines()) > 511)

rc = tempfile.mkdtemp()
os.environ["MIGRATE_ROOT"] = rc
os.environ["SCHEMA_SVC_DIR"] = os.path.join(rc, "svc")
os.environ["SCHEMA_STATE_DIR"] = os.path.join(rc, "state")
for d in ("etc/init.d", "etc/rc3.d", "lib/systemd/system", "svc"):
    os.makedirs(os.path.join(rc, d))
open(os.path.join(rc, "etc/inittab"), "w").write("#id:5:initdefault:\nid:3:initdefault:\n")
open(os.path.join(rc, "svc/mount-fstab.svc"), "w").write("name=mount-fstab\nexec=/bin/true\noneshot=1\n")
_lsb = ("#!/bin/sh\n### BEGIN INIT INFO\n# Provides: %s\n# Required-Start:    $remote_fs $syslog %s\n"
        "# Default-Start: 2 3 4 5\n### END INIT INFO\nPIDFILE=/run/x.pid\n")
for n, extra in (("plain", "dbus"), ("masked", ""), ("withunit", ""), ("covered", ""), ("noexec", ""), ("udevonly", "")):
    open(os.path.join(rc, "etc/init.d", n), "w").write(_lsb % (n, extra))
    os.symlink("../init.d/" + n, os.path.join(rc, "etc/rc3.d", "S02" + n))
os.symlink("../init.d/plain", os.path.join(rc, "etc/rc3.d", "K01plain"))
os.symlink("/dev/null", os.path.join(rc, "lib/systemd/system/masked.service"))
open(os.path.join(rc, "lib/systemd/system/withunit.service"), "w").write("[Service]\nExecStart=/usr/bin/withunitd -f\n[Install]\nWantedBy=multi-user.target\n")
open(os.path.join(rc, "lib/systemd/system/udevonly.service"), "w").write("[Service]\nExecStart=/usr/bin/udevonlyd\n[Install]\nAlso=udevonly.socket\n")
open(os.path.join(rc, "lib/systemd/system/noexec.service"), "w").write("[Unit]\nDescription=x\n")
_skipf = os.path.join(rc, "skip")
open(_skipf, "w").write("# comment\ncovered other  # trailing\n")
check("read_skip: words, comments dropped", si.read_skip(_skipf) == {"covered", "other"})
check("runlevel from inittab initdefault", si._runlevel() == "3")
_u = si.rc_units(si.read_skip(_skipf))
check("rc_units: unit when boot starts it, script when masked/no ExecStart/no WantedBy, skip honored, K links ignored",
      _u == ["/etc/init.d/masked", "/etc/init.d/noexec", "/etc/init.d/plain", "/etc/init.d/udevonly", "withunit.service"])
si.drain(units=_u, log=lambda *_: None)
_p = open(os.path.join(rc, "svc/plain.svc")).read()
check("lsb: runs start once, like sysvinit", "exec=/etc/init.d/plain\nargs=start\noneshot=1\n" in _p)
check("lsb: no pid_file (an /etc/default-disabled script would restart-loop)", "pid_file" not in _p)
check("lsb: $remote_fs -> mount-fstab kept, unknown $syslog/dbus deps dropped",
      "dep=mount-fstab\n" in _p and "journal-sink" not in _p and "dep=dbus" not in _p)
check("lsb: runnable unit translated from the unit, not the script",
      "exec=/usr/bin/withunitd\n" in open(os.path.join(rc, "svc/withunit.svc")).read())
check("lsb: rerun leaves existing .svc", si.import_one("/etc/init.d/plain")[0] == "exists")
check("lsb: missing script -> not-found", si.import_one("/etc/init.d/gone")[0] == "not-found")
_ld = si.lsb_to_svc("many", _lsb % ("many", " ".join("s%d" % i for i in range(9))), {"s%d" % i for i in range(9)})
check("lsb: more than 8 deps capped at 8, the rest noted", _ld.count("\ndep=") == 8 and "dropped dep on s8 (limit 8)" in _ld)
check("lsb_header parses the INFO block only", si.lsb_header(_lsb % ("h", ""))["Provides"] == "h")
open(os.path.join(rc, "svc/udevd.svc"), "w").write("name=udevd\nexec=/sbin/udevd\n")
check("lsb: Devuan dep names mapped (udev -> udevd, bluetooth alias)",
      "dep=udevd\n" in si.lsb_to_svc("x", _lsb % ("x", "udev bluetooth"), {"udevd", "bluetoothd", "mount-fstab"})
      and "dep=bluetoothd\n" in si.lsb_to_svc("x", _lsb % ("x", "bluetooth"), {"bluetoothd"}))
open(os.path.join(rc, "etc/init.d/rsyslog"), "w").write(_lsb % ("rsyslog", ""))
check("lsb: denied script skipped with a stub", si.import_one("/etc/init.d/rsyslog")[0] == "skipped")
for n, unit in (("ttyd", "[Service]\nExecStart=/usr/bin/ttyd\nStandardInput=tty\n[Install]\nWantedBy=multi-user.target\n"),
                ("twin", "[Service]\nExecStart=/usr/bin/withunitd\n[Install]\nWantedBy=multi-user.target\n")):
    open(os.path.join(rc, "lib/systemd/system", n + ".service"), "w").write(unit)
    open(os.path.join(rc, "etc/init.d", n), "w").write(_lsb % (n, ""))
si.drain(units=["ttyd.service", "twin.service"], log=lambda *_: None, lsb_fallback={"ttyd.service", "twin.service"})
check("rc: unit the importer refuses falls back to its init script",
      "exec=/etc/init.d/ttyd\n" in open(os.path.join(rc, "svc/ttyd.svc")).read())
check("rc: no fallback when another .svc already runs the daemon (no second copy)",
      not os.path.exists(os.path.join(rc, "svc/twin.svc")) and os.path.exists(os.path.join(rc, "svc/twin.svc.skipped")))

rv = tempfile.mkdtemp()
os.environ["MIGRATE_ROOT"] = rv
os.environ["SCHEMA_SVC_DIR"] = os.path.join(rv, "svc")
os.makedirs(os.path.join(rv, "etc/runit/runsvdir/default"))
os.makedirs(os.path.join(rv, "svc"))
for n, run in (("dbus", "#!/bin/sh\nexec dbus-daemon --system --nofork\n"),
               ("elogind", "#!/bin/sh\nsv check dbus >/dev/null || exit 1\nexec /usr/libexec/elogind\n"),
               ("nm", "#!/bin/sh\nsv -w 30 check /var/service/dbus/ || exit 1\nsv start ghost || exit 1\nexec NetworkManager -n\n"),
               ("parked", "#!/bin/sh\nexec parkedd\n"), ("covered", "#!/bin/sh\nexec x\n")):
    os.makedirs(os.path.join(rv, "etc/sv", n))
    open(os.path.join(rv, "etc/sv", n, "run"), "w").write(run)
    os.symlink("/etc/sv/" + n, os.path.join(rv, "etc/runit/runsvdir/default", n))
os.symlink("/etc/sv/gone", os.path.join(rv, "etc/runit/runsvdir/default/gone"))
os.makedirs(os.path.join(rv, "etc/sv/held"))
open(os.path.join(rv, "etc/sv/held/down"), "w").close()
os.symlink("/etc/sv/held", os.path.join(rv, "etc/runit/runsvdir/default/held"))
os.symlink("../../../sv/parked", os.path.join(rv, "etc/runit/runsvdir/default/alias"))
os.symlink("/home/x/sv/mine", os.path.join(rv, "etc/runit/runsvdir/default/mine"))
_r = si.runit_units({"covered"})
check("runit_units: link targets in /etc/sv (relative too), skip honored, down file left out, outside /etc/sv refused",
      _r == ["/etc/sv/parked", "/etc/sv/dbus", "/etc/sv/elogind", "/etc/sv/gone", "/etc/sv/nm"])
si.runit_after = "void-boot"
_c = si.drain(units=_r, log=lambda *_: None)
si.runit_after = None
_e = open(os.path.join(rv, "svc/elogind.svc")).read()
check("runit: ./run from its own directory, supervised",
      "exec=/bin/sh\nargs=-c\nargs=cd /etc/sv/elogind && exec ./run\n" in _e and "oneshot" not in _e)
check("runit: sv check -> dep, after= first", "dep=void-boot\ndep=dbus\n" in _e)
_n = open(os.path.join(rv, "svc/nm.svc")).read()
check("runit: sv -w N check /path/ -> dep on the name; unknown service dropped",
      "dep=dbus\n" in _n and "ghost" not in _n)
_nine = ["s%d" % i for i in range(9)]
_rd = si.runit_to_svc("many", "".join("sv check %s\n" % d for d in _nine), set(_nine))
check("runit: more than 8 deps capped at 8, the rest noted", _rd.count("\ndep=") == 8 and "dropped dep on s8 (limit 8)" in _rd)
check("runit: after= not added to itself", "dep=" not in si.runit_to_svc("void-boot", "", set(), "void-boot"))
check("runit: missing run -> not-found, rest imported", _c["not-found"] == 1 and _c["imported"] == 4)
check("runit: no check script -> ready once running (stable_secs=0)", "stable_secs=0\n" in _e)
check("runit: dbus check -> its socket", "ready_path=/run/dbus/system_bus_socket\n" in si.runit_to_svc("dbus", "", set(), None, True)
      and "stable_secs" not in si.runit_to_svc("dbus", "", set(), None, True))
check("runit: unknown check -> 1 s, noted", "not translated" in si.runit_to_svc("pg", "", set(), None, True)
      and "stable_secs=1\n" in si.runit_to_svc("pg", "", set(), None, True))

# --- AmbientCapabilities= / RuntimeDirectory= / multi-hour OnCalendar= (Debian) ---
_amb = si.unit_to_svc("pihole-FTL", si.parse_unit(
    "[Service]\nUser=pihole\nAmbientCapabilities=CAP_NET_BIND_SERVICE CAP_NET_RAW\nExecStart=/usr/bin/pihole-FTL -f\n"))
check("AmbientCapabilities= + User= -> ambient_caps= comma list",
      "ambient_caps=CAP_NET_BIND_SERVICE,CAP_NET_RAW\n" in _amb and "user=pihole\n" in _amb)
check("AmbientCapabilities= never brings keep_caps=", "keep_caps" not in _amb)
_amb = si.unit_to_svc("a", si.parse_unit(
    "[Service]\nUser=a\nAmbientCapabilities=cap_net_raw\nAmbientCapabilities=CAP_SYS_TIME CAP_NET_RAW\nExecStart=/bin/a\n"))
check("AmbientCapabilities= lines merge, upper-cased, deduped", "ambient_caps=CAP_NET_RAW,CAP_SYS_TIME\n" in _amb)
_amb = si.unit_to_svc("a", si.parse_unit(
    "[Service]\nUser=a\nAmbientCapabilities=CAP_NET_RAW\nAmbientCapabilities=\nExecStart=/bin/a\n"))
check("AmbientCapabilities= empty resets", "ambient_caps" not in _amb)
_amb = si.unit_to_svc("a", si.parse_unit("[Service]\nAmbientCapabilities=CAP_NET_RAW\nExecStart=/bin/a\n"))
check("AmbientCapabilities= without User= -> no ambient_caps (root has them), noted",
      "ambient_caps" not in _amb and "dropped AmbientCapabilities" in _amb)
_amb = si.unit_to_svc("a", si.parse_unit("[Service]\nUser=a\nAmbientCapabilities=CAP_NET_RAW CAP_NET_RAWW CAP_FUTURE\nExecStart=/bin/a\n"))
check("AmbientCapabilities= unknown names dropped with a WARN, known kept",
      "ambient_caps=CAP_NET_RAW\n" in _amb and si.WARN_PREFIX + "AmbientCapabilities= CAP_NET_RAWW" in _amb
      and "CAP_FUTURE" in _amb.split("ambient_caps=")[0])
import re as _re
_engine_caps = set(_re.findall(r'\{ "(CAP_[A-Z_]+)",', open(os.path.join(REPO, "caps.c")).read()))
check("importer cap list matches caps.c cap_table", _engine_caps and set(si._ENGINE_CAPS) == _engine_caps)
_amb = si.unit_to_svc("a", si.parse_unit("[Service]\nUser=a\nAmbientCapabilities=~CAP_NET_RAW\nExecStart=/bin/a\n"))
check("AmbientCapabilities=~ -> not translated, warned", "ambient_caps" not in _amb and "AmbientCapabilities=~" in _amb)

_rt = si.unit_to_svc("ssh", si.parse_unit(
    "[Service]\nEnvironmentFile=-/etc/default/ssh\nExecStartPre=/usr/sbin/sshd -t\nExecStart=/usr/sbin/sshd -D $SSHD_OPTS\n"
    "Type=notify\nRuntimeDirectory=sshd\nRuntimeDirectoryMode=0755\n"))
check("RuntimeDirectory= -> root install -d exec_pre before ExecStartPre",
      "exec_pre=+/usr/bin/install -d -m 0755 /run/sshd\nexec_pre=/usr/sbin/sshd -t\n" in _rt)
check("RuntimeDirectory= adds no order-changed warning", "order changed" not in _rt)
_rt = si.unit_to_svc("r", si.parse_unit("[Service]\nUser=foo\nRuntimeDirectory=r\nExecStart=/bin/r\n"))
check("RuntimeDirectory= + User= -> owned by user, default mode 0755",
      "exec_pre=+/usr/bin/install -d -m 0755 -o foo -g foo /run/r\n" in _rt)
_pg = tempfile.mkdtemp(); os.makedirs(os.path.join(_pg, "etc"))
open(os.path.join(_pg, "etc/passwd"), "w").write("root:x:0:0::/root:/bin/sh\nsvcu:x:990:991::/:/sbin/nologin\norph:x:992:4242::/:/sbin/nologin\n")
open(os.path.join(_pg, "etc/group"), "w").write("root:x:0:\nsvcgrp:x:991:\n")
os.environ["MIGRATE_ROOT"] = _pg
_rt = si.unit_to_svc("r", si.parse_unit("[Service]\nUser=svcu\nRuntimeDirectory=r\nExecStart=/bin/r\n"))
check("RuntimeDirectory= owner group = User='s primary group from MIGRATE_ROOT passwd/group",
      "-o svcu -g svcgrp /run/r\n" in _rt)
_rt = si.unit_to_svc("r", si.parse_unit("[Service]\nUser=orph\nRuntimeDirectory=r\nExecStart=/bin/r\n"))
check("primary gid with no group name -> user name", "-o orph -g orph /run/r\n" in _rt)
os.environ.pop("MIGRATE_ROOT")
_rt = si.unit_to_svc("r", si.parse_unit("[Service]\nUser=foo\nGroup=bar\nRuntimeDirectory=r\nRuntimeDirectoryMode=750\nExecStart=/bin/r\n"))
check("RuntimeDirectory= + Group= -> -g group", "exec_pre=+/usr/bin/install -d -m 750 -o foo -g bar /run/r\n" in _rt)
_rt = si.unit_to_svc("r", si.parse_unit("[Service]\nRuntimeDirectory=a b\nRuntimeDirectory=%N\nExecStart=/bin/r\n"))
check("several RuntimeDirectory= entries, one line each, %N expanded",
      "exec_pre=+/usr/bin/install -d -m 0755 /run/a\nexec_pre=+/usr/bin/install -d -m 0755 /run/b\n"
      "exec_pre=+/usr/bin/install -d -m 0755 /run/r\n" in _rt)
# root install -d -o under a parent the user owns follows a symlink the user planted there
_rt = si.unit_to_svc("r", si.parse_unit("[Service]\nUser=foo\nRuntimeDirectory=a/b ok\nExecStart=/bin/r\n"))
check("RuntimeDirectory= nested entry dropped and noted",
      "/run/a" not in _rt and _rt.count("install -d") == 1 and "-o foo -g foo /run/ok\n" in _rt
      and "# schema-import:" in _rt and "dropped RuntimeDirectory=a/b" in _rt)
_rt = si.unit_to_svc("r", si.parse_unit("[Service]\nUser=foo\nRuntimeDirectory=. ..\nExecStart=/bin/r\n"))
check("RuntimeDirectory=. and .. dropped (would chown /run or /)", "install -d" not in _rt)
_rt = si.unit_to_svc("r", si.parse_unit("[Service]\nRuntimeDirectory=irqbalance/\nExecStart=/bin/r\n"))
check("RuntimeDirectory= trailing slash is not nesting (Fedora irqbalance)", "-m 0755 /run/irqbalance\n" in _rt)
_rt = si.unit_to_svc("r", si.parse_unit("[Service]\nRuntimeDirectory=a\nRuntimeDirectory=\nRuntimeDirectory=b\nExecStart=/bin/r\n"))
check("RuntimeDirectory= empty resets", "/run/a" not in _rt and "/run/b\n" in _rt)
_rt = si.unit_to_svc("r", si.parse_unit("[Service]\nRuntimeDirectory=../x /abs ok %Z\nExecStart=/bin/r\n"))
check("RuntimeDirectory= bad entries dropped and noted, good kept",
      _rt.count("install -d") == 1 and "/run/ok\n" in _rt and "dropped RuntimeDirectory=../x" in _rt
      and "dropped RuntimeDirectory=/abs" in _rt and "dropped RuntimeDirectory=%Z" in _rt)
_rt = si.unit_to_svc("r", si.parse_unit("[Service]\nRuntimeDirectory=r\nRuntimeDirectoryMode=bogus\nExecStart=/bin/r\n"))
check("RuntimeDirectoryMode= unrecognised -> 0755, warned", "-m 0755 /run/r\n" in _rt and "RuntimeDirectoryMode=bogus" in _rt)
check("RuntimeDirectory= counts toward the 8 exec_pre limit", skipped("r",
      "[Service]\nRuntimeDirectory=a b\n" + "".join("ExecStartPre=/bin/p%d\n" % i for i in range(7)) + "ExecStart=/bin/r\n"))

for cal, want in (("*-*-* 6,18:00", "6,18:00"), ("Mon 18,6:30", "Mon 6,18:30"),
                  ("*-*-15 0,12:00:00", "15 0,12:00"), ("*-*-* 6,24:00", None), ("*-*-* 6,,18:00", None)):
    check("calendar %r -> %r" % (cal, want), si._calendar(cal) == want)
_ad = si.timer_to_svc("apt-daily", si.parse_unit("[Timer]\nOnCalendar=*-*-* 6,18:00\nRandomizedDelaySec=12h\nPersistent=true\n"),
                      si.parse_unit("[Service]\nType=oneshot\nExecStart=/usr/lib/apt/apt.systemd.daily update\n"))
check("apt-daily.timer imports with on_calendar=6,18:00", "on_calendar=6,18:00\npersistent=1\n" in _ad)

# Fedora units that use none of the new keys: byte-identical to the output before they existed
_F_SSHD = ("[Unit]\nAfter=network.target sshd-keygen.target\nWants=sshd-keygen.target\n[Service]\nType=notify\nEnvironment=OPTIONS=\n"
           "EnvironmentFile=-/etc/sysconfig/sshd\nExecStart=/usr/sbin/sshd -D $OPTIONS\nExecReload=/bin/kill -HUP $MAINPID\n"
           "KillMode=process\nRestart=on-failure\nRestartSec=42s\n[Install]\nWantedBy=multi-user.target\n")
_F_CHRONY = ("[Service]\nType=notify\nPIDFile=/run/chrony/chronyd.pid\nEnvironment=\"OPTIONS=\"\nEnvironmentFile=-/etc/sysconfig/chronyd\n"
             "ExecStart=/usr/sbin/chronyd -n $OPTIONS\nCapabilityBoundingSet=~CAP_AUDIT_CONTROL CAP_AUDIT_READ\nNoNewPrivileges=yes\n"
             "PrivateTmp=yes\nProtectHome=yes\nProtectSystem=strict\nReadWritePaths=/run /var/lib/chrony -/var/log\nNoNewPrivileges=no\n"
             "[Install]\nWantedBy=multi-user.target\n")
_F_FSTRIM_T = "[Timer]\nOnCalendar=weekly\nAccuracySec=1h\nPersistent=true\nRandomizedDelaySec=100min\n[Install]\nWantedBy=timers.target\n"
_F_FSTRIM_S = ("[Service]\nType=oneshot\nExecStart=/usr/bin/fstrim --listed-in /etc/fstab:/proc/self/mountinfo --verbose "
               "--quiet-unsupported\nProtectKernelTunables=yes\n")
check("Fedora sshd.service output unchanged", si.unit_to_svc("sshd", si.parse_unit(_F_SSHD)) ==
      'name=sshd\nexec=/usr/sbin/sshd\nargs=-D\nargs=$OPTIONS\nenv=OPTIONS=\nenv_file=-/etc/sysconfig/sshd\nexpand_args=1\nnotify=1\n'
      'restart_sec=42\nneeds_root=1\ncritical=0\nno_new_privs=0\nprivate_tmp=0\nprotect_system=0\nprotect_home=0\n')
check("Fedora chronyd.service output unchanged", si.unit_to_svc("chronyd", si.parse_unit(_F_CHRONY)) ==
      '# schema-import WARN: ProtectSystem=strict mapped to protect_system=full (/ not read-only)\nname=chronyd\n'
      'exec=/usr/sbin/chronyd\nargs=-n\nargs=$OPTIONS\nenv=OPTIONS=\nenv_file=-/etc/sysconfig/chronyd\nexpand_args=1\nnotify=1\n'
      'no_restart=1\nneeds_root=1\ncritical=0\nno_new_privs=0\nprivate_tmp=1\nprotect_system=full\nprotect_home=1\n')
check("Fedora fstrim.timer output unchanged",
      si.timer_to_svc("fstrim", si.parse_unit(_F_FSTRIM_T), si.parse_unit(_F_FSTRIM_S)) ==
      '# schema-import: dropped RandomizedDelaySec; dropped AccuracySec\nname=fstrim\nexec=/usr/bin/fstrim\nargs=--listed-in\n'
      'args=/etc/fstab:/proc/self/mountinfo\nargs=--verbose\nargs=--quiet-unsupported\nneeds_root=1\ncritical=0\nno_new_privs=0\n'
      'private_tmp=0\nprotect_system=0\nprotect_home=0\non_calendar=Mon 00:00\npersistent=1\n')

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
