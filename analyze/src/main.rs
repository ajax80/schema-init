use std::ffi::{c_char, c_int, c_long};
use std::fs;
use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::time::Duration;

const SOCK: &str = "/run/schema-init.sock";
const DIR: &str = "/var/lib/schema-init/boots";
const KEEP: usize = 500;

#[derive(Debug, Clone, PartialEq)]
struct Svc {
    name: String,
    cost: Option<f64>,
    ready: f64,
}

#[derive(Debug, Clone, PartialEq, Default)]
struct Boot {
    id: String,
    boot_id: Option<String>,
    host: String,
    booted: String,
    kernel: String,
    kernel_s: Option<f64>,
    userspace_s: Option<f64>,
    last_ready: Option<String>,
    pid1_rss_kb: Option<u64>,
    svcs: Vec<Svc>,
}

impl Boot {
    fn svc(&self, name: &str) -> Option<&Svc> {
        self.svcs.iter().find(|s| s.name == name)
    }

    fn total(&self) -> Option<f64> {
        Some(self.kernel_s? + self.userspace_s?)
    }

    fn serialize(&self) -> String {
        let mut o = String::new();
        o += &format!("host {}\nbooted {}\nkernel {}\n", self.host, self.booted, self.kernel);
        if let Some(v) = &self.boot_id {
            o += &format!("boot_id {v}\n");
        }
        if let Some(v) = self.kernel_s {
            o += &format!("kernel_s {v:.3}\n");
        }
        if let Some(v) = self.userspace_s {
            o += &format!("userspace_s {v:.3}\n");
        }
        if let Some(v) = &self.last_ready {
            o += &format!("last_ready {v}\n");
        }
        if let Some(v) = self.pid1_rss_kb {
            o += &format!("pid1_rss_kb {v}\n");
        }
        for s in &self.svcs {
            let cost = s.cost.map_or("-".to_string(), |c| format!("{c:.3}"));
            o += &format!("svc {} {} {:.3}\n", s.name, cost, s.ready);
        }
        o
    }

    fn deserialize(id: &str, text: &str) -> Boot {
        let mut b = Boot { id: id.to_string(), ..Default::default() };
        for line in text.lines() {
            let (k, v) = line.split_once(' ').unwrap_or((line, ""));
            match k {
                "host" => b.host = v.to_string(),
                "boot_id" => b.boot_id = Some(v.to_string()),
                "booted" => b.booted = v.to_string(),
                "kernel" => b.kernel = v.to_string(),
                "kernel_s" => b.kernel_s = v.parse().ok(),
                "userspace_s" => b.userspace_s = v.parse().ok(),
                "last_ready" => b.last_ready = Some(v.to_string()),
                "pid1_rss_kb" => b.pid1_rss_kb = v.parse().ok(),
                "svc" => {
                    let f: Vec<&str> = v.split_whitespace().collect();
                    if let [name, cost, ready] = f[..] {
                        if let Ok(ready) = ready.parse() {
                            b.svcs.push(Svc { name: name.to_string(), cost: cost.parse().ok(), ready });
                        }
                    }
                }
                _ => {}
            }
        }
        b
    }
}

fn secs(s: &str) -> Option<f64> {
    s.strip_suffix('s')?.parse().ok()
}

fn parse_timing(text: &str) -> (Option<f64>, Vec<Svc>) {
    let mut k2p = None;
    let mut svcs = Vec::new();
    for line in text.lines() {
        if line.starts_with("kernel") {
            k2p = line.rsplit(' ').next().and_then(secs);
            continue;
        }
        if !line.starts_with("  ") {
            continue;
        }
        let f: Vec<&str> = line.split_whitespace().collect();
        match f[..] {
            ["service", ..] => {}
            [name, cost, ready] => {
                if let Some(ready) = secs(ready) {
                    svcs.push(Svc { name: name.to_string(), cost: secs(cost), ready });
                }
            }
            [name, ready] => {
                if let Some(ready) = secs(ready) {
                    svcs.push(Svc { name: name.to_string(), cost: None, ready });
                }
            }
            _ => {}
        }
    }
    (k2p, svcs)
}

fn parse_analyze_head(text: &str) -> Option<(f64, String)> {
    let line = text.lines().find(|l| l.starts_with("boot: "))?;
    let f: Vec<&str> = line.split_whitespace().collect();
    let i = f.iter().position(|w| *w == "userspace")?;
    let us = secs(f.get(i + 1)?)?;
    let last = line.split_once("(last ready: ")?.1.trim_end_matches(')').trim().to_string();
    Some((us, last))
}

fn parse_report(text: &str) -> Option<Boot> {
    let field = |key: &str| {
        text.lines()
            .find_map(|l| l.strip_prefix(key))
            .map(|v| v.trim().to_string())
    };
    let booted = field("booted at:")?;
    let section: String = text
        .lines()
        .skip_while(|l| !l.starts_with("### SCHEMA-INIT TIMING"))
        .skip(1)
        .take_while(|l| !l.starts_with("###"))
        .map(|l| format!("{l}\n"))
        .collect();
    let (kernel_s, svcs) = parse_timing(&section);
    if svcs.is_empty() {
        return None;
    }
    let rss = field("PID 1 RSS:").and_then(|v| v.split_whitespace().next()?.parse().ok());
    let host = field("hostname:").unwrap_or_default();
    Some(Boot {
        id: format!("{}-{}", stamp_of(&booted), host),
        boot_id: None,
        host,
        booted,
        kernel: field("kernel:").unwrap_or_default(),
        kernel_s,
        userspace_s: None,
        last_ready: None,
        pid1_rss_kb: rss,
        svcs,
    })
}

fn stamp_of(booted: &str) -> String {
    let d: String = booted.chars().filter(|c| c.is_ascii_digit()).collect();
    if d.len() >= 14 {
        format!("{}-{}", &d[..8], &d[8..14])
    } else {
        d
    }
}

#[repr(C)]
struct Tm {
    sec: c_int,
    min: c_int,
    hour: c_int,
    mday: c_int,
    mon: c_int,
    year: c_int,
    wday: c_int,
    yday: c_int,
    isdst: c_int,
    gmtoff: c_long,
    zone: *const c_char,
}

extern "C" {
    fn localtime_r(t: *const c_long, tm: *mut Tm) -> *mut Tm;
    fn signal(sig: c_int, handler: usize) -> usize;
}

fn local_time(epoch: i64) -> String {
    let t = epoch as c_long;
    let mut tm = Tm { sec: 0, min: 0, hour: 0, mday: 0, mon: 0, year: 0, wday: 0, yday: 0, isdst: 0, gmtoff: 0, zone: std::ptr::null() };
    if unsafe { localtime_r(&t, &mut tm) }.is_null() {
        return epoch.to_string();
    }
    format!(
        "{:04}-{:02}-{:02} {:02}:{:02}:{:02}",
        tm.year + 1900,
        tm.mon + 1,
        tm.mday,
        tm.hour,
        tm.min,
        tm.sec
    )
}

fn ctl(cmd: &str) -> Result<String, String> {
    let mut s = UnixStream::connect(SOCK).map_err(|e| format!("{SOCK}: {e}"))?;
    s.set_read_timeout(Some(Duration::from_secs(10))).map_err(|e| e.to_string())?;
    s.set_write_timeout(Some(Duration::from_secs(10))).map_err(|e| e.to_string())?;
    s.write_all(format!("{cmd}\n").as_bytes()).map_err(|e| e.to_string())?;
    let mut out = Vec::new();
    let mut buf = [0u8; 8192];
    loop {
        let n = s.read(&mut buf).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        out.extend_from_slice(&buf[..n]);
        if out.ends_with(b".\n") {
            out.truncate(out.len() - 2);
            break;
        }
    }
    let text = String::from_utf8_lossy(&out).into_owned();
    if text.starts_with("err:") {
        return Err(format!("{cmd}: {}", text.trim()));
    }
    Ok(text)
}

fn read_trim(p: &str) -> String {
    fs::read_to_string(p).unwrap_or_default().trim().to_string()
}

fn snapshot() -> Result<Boot, String> {
    let timing = ctl("timing")?;
    let (kernel_s, svcs) = parse_timing(&timing);
    if svcs.is_empty() {
        return Err("PID 1 `timing` gave no service rows (output format changed?)".into());
    }
    let head = ctl("analyze").ok().and_then(|t| parse_analyze_head(&t));
    let btime: i64 = read_trim("/proc/stat")
        .lines()
        .find_map(|l| l.strip_prefix("btime ")?.trim().parse().ok())
        .ok_or("no btime in /proc/stat")?;
    let rss = read_trim("/proc/1/status")
        .lines()
        .find_map(|l| l.strip_prefix("VmRSS:")?.split_whitespace().next()?.parse().ok());
    let booted = local_time(btime);
    let host = read_trim("/proc/sys/kernel/hostname");
    Ok(Boot {
        id: format!("{}-{}", stamp_of(&booted), host),
        boot_id: Some(read_trim("/proc/sys/kernel/random/boot_id")),
        host,
        booted,
        kernel: read_trim("/proc/sys/kernel/osrelease"),
        kernel_s,
        userspace_s: head.as_ref().map(|h| h.0),
        last_ready: head.map(|h| h.1),
        pid1_rss_kb: rss,
        svcs,
    })
}

fn stamp_secs(booted: &str) -> Option<(&str, i64)> {
    let (date, time) = booted.split_once(' ')?;
    let t: Vec<i64> = time.split(':').map(|x| x.parse().ok()).collect::<Option<_>>()?;
    match t[..] {
        [h, m, sec] => Some((date, h * 3600 + m * 60 + sec)),
        _ => None,
    }
}

fn same_boot(a: &Boot, b: &Boot) -> bool {
    if a.id == b.id {
        return true;
    }
    if let (Some(x), Some(y)) = (&a.boot_id, &b.boot_id) {
        return x == y;
    }
    match (stamp_secs(&a.booted), stamp_secs(&b.booted)) {
        (Some((da, sa)), Some((db, sb))) => a.host == b.host && da == db && (sa - sb).abs() <= 2,
        _ => false,
    }
}

fn save(dir: &Path, b: &Boot, force: bool) -> Result<bool, String> {
    fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let dups: Vec<Boot> = load(dir).into_iter().filter(|o| same_boot(o, b)).collect();
    if !dups.is_empty() && !force {
        return Ok(false);
    }
    for d in dups.iter().filter(|d| d.id != b.id) {
        let _ = fs::remove_file(dir.join(format!("{}.boot", d.id)));
    }
    let path = dir.join(format!("{}.boot", b.id));
    let tmp = dir.join(format!(".{}.tmp", b.id));
    fs::write(&tmp, b.serialize()).map_err(|e| format!("{}: {e}", tmp.display()))?;
    fs::rename(&tmp, &path).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(true)
}

fn prune(dir: &Path) {
    let ids = list_ids(dir);
    if ids.len() > KEEP {
        for id in &ids[..ids.len() - KEEP] {
            let _ = fs::remove_file(dir.join(format!("{id}.boot")));
        }
    }
}

fn list_ids(dir: &Path) -> Vec<String> {
    let mut ids: Vec<String> = fs::read_dir(dir)
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|e| e.file_name().to_str()?.strip_suffix(".boot").map(String::from))
        .collect();
    ids.sort();
    ids
}

fn load(dir: &Path) -> Vec<Boot> {
    list_ids(dir)
        .into_iter()
        .filter_map(|id| {
            let text = fs::read_to_string(dir.join(format!("{id}.boot"))).ok()?;
            Some(Boot::deserialize(&id, &text))
        })
        .collect()
}

fn pick<'a>(boots: &'a [Boot], sel: &str) -> Result<(usize, &'a Boot), String> {
    let hashed = sel.strip_prefix('#');
    let num = hashed.unwrap_or(sel).parse::<i64>().ok();
    if let Some(n) = num {
        let i = if n < 0 { boots.len() as i64 + n } else { n - 1 };
        if i >= 0 && (i as usize) < boots.len() {
            return Ok((i as usize, &boots[i as usize]));
        }
    }
    let hits: Vec<usize> = if hashed.is_some() || sel.starts_with('-') {
        Vec::new()
    } else {
        (0..boots.len()).filter(|&i| boots[i].id.starts_with(sel)).collect()
    };
    match hits[..] {
        [i] => Ok((i, &boots[i])),
        [] if num.is_some() => Err(format!("no boot #{} ({} recorded)", num.unwrap(), boots.len())),
        [] => Err(format!("no boot matches {sel}")),
        _ => Err(format!("{sel} matches {} boots", hits.len())),
    }
}

fn default_pair(boots: &[Boot]) -> Option<(usize, usize)> {
    let last = boots.len().checked_sub(1)?;
    let prev = (0..last).rev().find(|&i| boots[i].host == boots[last].host)?;
    Some((prev, last))
}

fn fmt_s(v: Option<f64>) -> String {
    v.map_or("-".to_string(), |v| format!("{v:.3}s"))
}

fn fmt_d(v: f64) -> String {
    format!("{v:+.3}s")
}

fn metric(a: &Svc, b: &Svc) -> (f64, f64, bool) {
    match (a.cost, b.cost) {
        (Some(x), Some(y)) => (x, y, true),
        _ => (a.ready, b.ready, false),
    }
}

fn median(mut v: Vec<f64>) -> f64 {
    v.sort_by(|a, b| a.total_cmp(b));
    let n = v.len();
    if n % 2 == 1 {
        v[n / 2]
    } else {
        (v[n / 2 - 1] + v[n / 2]) / 2.0
    }
}

fn cmd_history(boots: &[Boot], n: usize) {
    if boots.is_empty() {
        println!("no boots recorded");
        return;
    }
    println!(
        "  {:>4}  {:<19}  {:<14}  {:>8}  {:>9}  {:>8}  {:>9}  slowest",
        "#", "booted", "host", "k→PID1", "userspace", "total", "PID1 RSS"
    );
    for (i, b) in boots.iter().enumerate().skip(boots.len().saturating_sub(n)) {
        let slow = b
            .svcs
            .iter()
            .filter(|s| s.cost.is_some())
            .max_by(|x, y| x.cost.unwrap().total_cmp(&y.cost.unwrap()))
            .map_or(String::new(), |s| format!("{} {}", s.name, fmt_s(s.cost)));
        let rss = b.pid1_rss_kb.map_or("-".to_string(), |r| format!("{r} kB"));
        println!(
            "  {:>4}  {:<19}  {:<14.14}  {:>8}  {:>9}  {:>8}  {:>9}  {}",
            i + 1,
            b.booted,
            b.host,
            fmt_s(b.kernel_s),
            fmt_s(b.userspace_s),
            fmt_s(b.total()),
            rss,
            slow
        );
    }
}

fn cmd_compare(boots: &[Boot], a: &str, b: &str, all: bool) -> Result<(), String> {
    let (ia, a) = pick(boots, a)?;
    let (ib, b) = pick(boots, b)?;
    println!("A  #{:<4} {}  {}  {}", ia + 1, a.booted, a.host, a.kernel);
    println!("B  #{:<4} {}  {}  {}", ib + 1, b.booted, b.host, b.kernel);
    println!();
    println!("  {:<22} {:>10} {:>10} {:>10}", "", "A", "B", "Δ");
    for (label, x, y) in [
        ("kernel → PID 1", a.kernel_s, b.kernel_s),
        ("userspace", a.userspace_s, b.userspace_s),
        ("total", a.total(), b.total()),
    ] {
        let d = match (x, y) {
            (Some(x), Some(y)) => fmt_d(y - x),
            _ => "-".to_string(),
        };
        println!("  {:<22} {:>10} {:>10} {:>10}", label, fmt_s(x), fmt_s(y), d);
    }
    if let (Some(x), Some(y)) = (a.pid1_rss_kb, b.pid1_rss_kb) {
        println!(
            "  {:<22} {:>10} {:>10} {:>10}",
            "PID 1 RSS",
            format!("{x} kB"),
            format!("{y} kB"),
            format!("{:+} kB", y as i64 - x as i64)
        );
    }

    let mut rows: Vec<(&str, f64, f64, bool)> = b
        .svcs
        .iter()
        .filter_map(|sb| {
            let (x, y, by_cost) = metric(a.svc(&sb.name)?, sb);
            Some((sb.name.as_str(), x, y, by_cost))
        })
        .collect();
    rows.sort_by(|p, q| (q.2 - q.1).abs().total_cmp(&(p.2 - p.1).abs()));
    let shown = if all { rows.len() } else { rows.len().min(15) };
    println!();
    println!("  {:<24} {:>9} {:>9} {:>9}", "service", "A", "B", "Δ");
    for (name, x, y, by_cost) in &rows[..shown] {
        let tag = if *by_cost { "" } else { "  (ready@)" };
        println!("  {:<24} {:>9} {:>9} {:>9}{}", name, fmt_s(Some(*x)), fmt_s(Some(*y)), fmt_d(y - x), tag);
    }
    if shown < rows.len() {
        println!("  … {} more (--all)", rows.len() - shown);
    }
    let names = |from: &Boot, other: &Boot| -> Vec<String> {
        from.svcs.iter().filter(|s| other.svc(&s.name).is_none()).map(|s| s.name.clone()).collect()
    };
    let new = names(b, a);
    let gone = names(a, b);
    if !new.is_empty() {
        println!("\nonly in B ({}): {}", new.len(), new.join(", "));
    }
    if !gone.is_empty() {
        println!("only in A ({}): {}", gone.len(), gone.join(", "));
    }
    Ok(())
}

struct Hit {
    name: String,
    base: f64,
    now: f64,
    samples: usize,
    by_cost: bool,
}

fn regressions(boots: &[Boot], window: usize, min_delta: f64) -> Option<(&Boot, usize, Vec<Hit>)> {
    let (last, prev) = boots.split_last()?;
    let prev: Vec<&Boot> = prev.iter().rev().filter(|b| b.host == last.host).take(window).collect();
    let mut hits = Vec::new();
    let mut check = |name: &str, now: f64, vals: Vec<f64>, by_cost: bool| {
        if vals.len() < 3 {
            return;
        }
        let base = median(vals.clone());
        if now - base > min_delta.max(base * 0.25) {
            hits.push(Hit { name: name.to_string(), base, now, samples: vals.len(), by_cost });
        }
    };
    if let Some(now) = last.kernel_s {
        check("kernel → PID 1", now, prev.iter().filter_map(|b| b.kernel_s).collect(), true);
    }
    if let Some(now) = last.userspace_s {
        check("userspace", now, prev.iter().filter_map(|b| b.userspace_s).collect(), true);
    }
    for s in &last.svcs {
        let (now, by_cost) = s.cost.map_or((s.ready, false), |c| (c, true));
        let vals = prev
            .iter()
            .filter_map(|b| b.svc(&s.name))
            .filter_map(|p| if by_cost { p.cost } else { Some(p.ready) })
            .collect();
        check(&s.name, now, vals, by_cost);
    }
    hits.sort_by(|p, q| (q.now - q.base).total_cmp(&(p.now - p.base)));
    Some((last, prev.len(), hits))
}

fn cmd_regress(boots: &[Boot], window: usize, min_delta: f64) -> bool {
    let Some((last, n, hits)) = regressions(boots, window, min_delta) else {
        println!("no boots recorded");
        return false;
    };
    println!("latest {}  {}  vs median of previous {} boot(s)", last.booted, last.host, n);
    if hits.is_empty() {
        println!("no regressions (threshold: +{min_delta:.2}s and +25%)");
        return false;
    }
    println!("\n  {:<24} {:>9} {:>9} {:>9}  n", "", "median", "now", "Δ");
    for h in &hits {
        let tag = if h.by_cost { "" } else { "  (ready@)" };
        println!(
            "  {:<24} {:>9} {:>9} {:>9}  {}{}",
            h.name,
            fmt_s(Some(h.base)),
            fmt_s(Some(h.now)),
            fmt_d(h.now - h.base),
            h.samples,
            tag
        );
    }
    true
}

fn usage() -> ExitCode {
    eprint!(
        "usage: schema-analyze [--dir DIR] COMMAND
  record [--force]        save this boot's timing from PID 1 (one file per boot)
  history [-n N]          one line per recorded boot (default last 20)
  compare [A [B]]         per-service change, A→B (default: previous vs latest)
                          A/B: #index from history, -1 = latest, or id prefix
          [--all]         every service, not just the 15 biggest moves
  regress [--window N] [--min S]
                          latest boot vs median of up to N earlier boots on the
                          same host (default 10, +0.25s and +25%); exit 1 on a hit
  import FILE...          backfill from boot-capture.sh reports
boots live in {DIR} (override: --dir or SCHEMA_ANALYZE_DIR)
"
    );
    ExitCode::from(2)
}

fn run(args: Vec<String>) -> Result<ExitCode, String> {
    let mut dir = PathBuf::from(std::env::var("SCHEMA_ANALYZE_DIR").unwrap_or_else(|_| DIR.to_string()));
    let mut rest = Vec::new();
    let mut it = args.into_iter();
    while let Some(a) = it.next() {
        if a == "--dir" {
            dir = PathBuf::from(it.next().ok_or("--dir needs a value")?);
        } else {
            rest.push(a);
        }
    }
    let Some((cmd, opts)) = rest.split_first() else { return Ok(usage()) };
    let flag = |f: &str| opts.iter().any(|o| o == f);
    let value = |f: &str| -> Result<Option<String>, String> {
        match opts.iter().position(|o| o == f) {
            Some(i) => opts.get(i + 1).cloned().map(Some).ok_or(format!("{f} needs a value")),
            None => Ok(None),
        }
    };
    let num = |f: &str, d: f64| -> Result<f64, String> {
        value(f)?.map_or(Ok(d), |v| v.parse().map_err(|_| format!("{f}: not a number: {v}")))
    };

    match cmd.as_str() {
        "record" => {
            let b = snapshot()?;
            if save(&dir, &b, flag("--force"))? {
                prune(&dir);
                println!("recorded {} ({} services)", b.id, b.svcs.len());
            } else {
                println!("{} already recorded (--force to overwrite)", b.id);
            }
        }
        "import" => {
            let (mut added, mut skipped) = (0, 0);
            for f in opts {
                let text = fs::read_to_string(f).map_err(|e| format!("{f}: {e}"))?;
                match parse_report(&text) {
                    Some(b) if save(&dir, &b, false)? => added += 1,
                    _ => skipped += 1,
                }
            }
            println!("imported {added}, skipped {skipped} (unparseable or already recorded)");
        }
        "history" => cmd_history(&load(&dir), num("-n", 20.0)? as usize),
        "compare" => {
            let boots = load(&dir);
            let sel: Vec<&str> = opts.iter().map(String::as_str).filter(|o| !o.starts_with("--")).collect();
            let (a, b) = match sel[..] {
                [] => {
                    let (i, b) = default_pair(&boots).ok_or("need two boots from the same host to compare")?;
                    (format!("#{}", i + 1), format!("#{}", b + 1))
                }
                [b] => {
                    let (ib, _) = pick(&boots, b)?;
                    let i = (0..ib).rev().find(|&i| boots[i].host == boots[ib].host).ok_or("no earlier boot from that host")?;
                    (format!("#{}", i + 1), format!("#{}", ib + 1))
                }
                [a, b, ..] => (a.to_string(), b.to_string()),
            };
            cmd_compare(&boots, &a, &b, flag("--all"))?;
        }
        "regress" => {
            if cmd_regress(&load(&dir), num("--window", 10.0)? as usize, num("--min", 0.25)?) {
                return Ok(ExitCode::from(1));
            }
        }
        _ => return Ok(usage()),
    }
    Ok(ExitCode::SUCCESS)
}

fn main() -> ExitCode {
    unsafe { signal(13, 0) };
    match run(std::env::args().skip(1).collect()) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("schema-analyze: {e}");
            ExitCode::from(2)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const TIMING: &str = "kernel → PID 1:            4.336s
  service                      cost    ready@
  wsdd                      10.139s   11.837s
  udevd                      4.259s    4.617s
  mount-efi                       -    1.157s
";

    const TIMING_OLD: &str = "kernel → PID 1:            2.536s
  dbus                     0.400s
  nordvpnd                 23.518s
";

    const ANALYZE: &str = "boot: kernel 4.336s + userspace 8.186s = 12.522s   (last ready: schema-bootok)

critical chain → schema-bootok
";

    fn boot(id: &str, host: &str, k: f64, svcs: &[(&str, f64)]) -> Boot {
        Boot {
            id: id.into(),
            host: host.into(),
            kernel_s: Some(k),
            svcs: svcs.iter().map(|(n, c)| Svc { name: (*n).into(), cost: Some(*c), ready: *c + 0.4 }).collect(),
            ..Default::default()
        }
    }

    #[test]
    fn timing_current_format() {
        let (k, s) = parse_timing(TIMING);
        assert_eq!(k, Some(4.336));
        assert_eq!(s.len(), 3);
        assert_eq!(s[0], Svc { name: "wsdd".into(), cost: Some(10.139), ready: 11.837 });
        assert_eq!(s[2].cost, None);
        assert_eq!(s[2].ready, 1.157);
    }

    #[test]
    fn timing_old_format_is_ready_only() {
        let (k, s) = parse_timing(TIMING_OLD);
        assert_eq!(k, Some(2.536));
        assert_eq!(s[1], Svc { name: "nordvpnd".into(), cost: None, ready: 23.518 });
    }

    #[test]
    fn analyze_head() {
        assert_eq!(parse_analyze_head(ANALYZE), Some((8.186, "schema-bootok".into())));
        assert_eq!(parse_analyze_head("err: no service has reached ready\n"), None);
    }

    #[test]
    fn round_trip() {
        let (kernel_s, svcs) = parse_timing(TIMING);
        let b = Boot {
            id: "20261011-185955-blakbox".into(),
            boot_id: Some("8f1c2e4a-0000-4000-8000-000000000001".into()),
            host: "blakbox".into(),
            booted: "2026-10-11 18:59:55".into(),
            kernel: "7.0.12-cachyos1-schema.fc44.x86_64".into(),
            kernel_s,
            userspace_s: Some(8.186),
            last_ready: Some("schema-bootok".into()),
            pid1_rss_kb: Some(4716),
            svcs,
        };
        assert_eq!(Boot::deserialize(&b.id, &b.serialize()), b);
    }

    #[test]
    fn report_import() {
        let r = format!(
            "### HOST\nhostname:    blakbox\nkernel:      7.0.12\nbooted at:   2026-10-11 18:59:55\n\
             PID 1 RSS:   4716 kB  (systemd-as-PID1 typically 10000-15000 kB)\n\n\
             ### SCHEMA-INIT TIMING (kernel→PID1, then per-service time-to-stable)\n{TIMING}\n\
             ### SCHEMA-INIT STATUS\n  bogus   1.000s 2.000s\n"
        );
        let b = parse_report(&r).unwrap();
        assert_eq!(b.id, "20261011-185955-blakbox");
        assert_eq!(b.pid1_rss_kb, Some(4716));
        assert_eq!(b.kernel_s, Some(4.336));
        assert_eq!(b.svcs.len(), 3);
        assert!(parse_report("booted at: 2026-10-11 18:59:55\n").is_none());
    }

    #[test]
    fn pick_selectors() {
        let boots: Vec<Boot> = ["20261001-1-a", "20261002-1-a", "20261003-1-a"]
            .iter()
            .map(|id| boot(id, "a", 1.0, &[]))
            .collect();
        assert_eq!(pick(&boots, "-1").unwrap().0, 2);
        assert_eq!(pick(&boots, "1").unwrap().0, 0);
        assert_eq!(pick(&boots, "#3").unwrap().0, 2);
        assert_eq!(pick(&boots, "20261002").unwrap().0, 1);
        assert!(pick(&boots, "2026100").is_err());
        assert!(pick(&boots, "2026").is_err());
        assert_eq!(pick(&boots, "20261003").unwrap().0, 2);
        assert!(pick(&boots, "4").is_err());
        assert!(pick(&boots, "-4").is_err());
    }

    #[test]
    fn regress_flags_only_real_slowdowns() {
        let mut boots: Vec<Boot> = (0..5)
            .map(|i| boot(&format!("2026100{i}"), "a", 4.0, &[("udevd", 4.0 + i as f64 * 0.01), ("dbus", 1.0)]))
            .collect();
        boots.push(boot("x", "b", 99.0, &[("udevd", 99.0)]));
        boots.push(boot("20261009", "a", 4.1, &[("udevd", 6.0), ("dbus", 1.2), ("newsvc", 50.0)]));
        let (_, n, hits) = regressions(&boots, 10, 0.25).unwrap();
        assert_eq!(n, 5);
        assert_eq!(hits.len(), 1);
        assert_eq!(hits[0].name, "udevd");
        assert_eq!(hits[0].base, 4.02);
    }

    #[test]
    fn short_prefix_matches() {
        let boots: Vec<Boot> = ["20261001-1-a", "20270101-1-a"].iter().map(|id| boot(id, "a", 1.0, &[])).collect();
        assert_eq!(pick(&boots, "2027").unwrap().0, 1);
        assert_eq!(pick(&boots, "#2").unwrap().0, 1);
        assert!(pick(&boots, "#3").is_err());
    }

    #[test]
    fn same_boot_rules() {
        let mut a = boot("20261011-185955-blakbox", "blakbox", 1.0, &[]);
        let mut b = boot("20261011-185956-blakbox", "blakbox", 1.0, &[]);
        a.booted = "2026-10-11 18:59:55".into();
        b.booted = "2026-10-11 18:59:56".into();
        assert!(same_boot(&a, &b));
        b.host = "eli".into();
        assert!(!same_boot(&a, &b));
        b.host = "blakbox".into();
        a.boot_id = Some("x".into());
        b.boot_id = Some("y".into());
        assert!(!same_boot(&a, &b));
        b.booted = "1970-01-01 00:00:09".into();
        b.boot_id = Some("x".into());
        assert!(same_boot(&a, &b));
    }

    #[test]
    fn save_dedupes_and_force_replaces() {
        let dir = std::env::temp_dir().join(format!("schema-analyze-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let mut a = boot("20261011-185956-h", "h", 1.0, &[("x", 1.0)]);
        a.booted = "2026-10-11 18:59:56".into();
        let mut b = boot("20261011-185955-h", "h", 2.0, &[("x", 1.0)]);
        b.booted = "2026-10-11 18:59:55".into();
        assert!(save(&dir, &a, false).unwrap());
        assert!(!save(&dir, &b, false).unwrap());
        assert!(save(&dir, &b, true).unwrap());
        assert_eq!(list_ids(&dir), vec!["20261011-185955-h".to_string()]);
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn default_pair_same_host() {
        let boots = vec![boot("1", "a", 1.0, &[]), boot("2", "b", 1.0, &[]), boot("3", "a", 1.0, &[])];
        assert_eq!(default_pair(&boots), Some((0, 2)));
        assert_eq!(default_pair(&boots[..2]), None);
    }

    #[test]
    fn median_even_odd() {
        assert_eq!(median(vec![3.0, 1.0, 2.0]), 2.0);
        assert_eq!(median(vec![4.0, 1.0, 2.0, 3.0]), 2.5);
    }
}
