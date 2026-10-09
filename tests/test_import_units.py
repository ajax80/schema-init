#!/usr/bin/env python3
"""schema-import unit tests — pure translation + a temp-tree drain. No systemctl."""
import os, sys, tempfile, importlib.util
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
check("User=nobody -> user=, no needs_root", "user=nobody\n" in b2 and "needs_root" not in b2)
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
si._FACTS = {"virt": "none", "virt_kind": None, "security": {"selinux", "audit"}, "cgroup_v2": True,
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
      "ConditionControlGroupController=v2\nConditionCapability=CAP_SYS_ADMIN\nConditionSecurity=selinux\n"
      "ConditionFirstBoot=no\nConditionArchitecture=x86-64") == [])
for t in ("ConditionVirtualization=vm", "ConditionVirtualization=yes", "ConditionVirtualization=kvm",
          "ConditionCPUs=>=8", "ConditionSecurity=!selinux", "ConditionFirstBoot=yes",
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

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
