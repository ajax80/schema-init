import json, os, time

INSTALLED, R1_PENDING, R1_HEAL, R2_PENDING, DONE, ROLLED_BACK = (
    "INSTALLED", "R1_PENDING", "R1_HEAL", "R2_PENDING", "DONE", "ROLLED_BACK")
STAGE_PATH = "var/lib/schema-init/wizard-stage.json"
VALID = {
    INSTALLED: {R1_PENDING}, R1_PENDING: {R1_HEAL}, R1_HEAL: {R2_PENDING},
    R2_PENDING: {DONE, ROLLED_BACK}, DONE: set(), ROLLED_BACK: set(),
}

def _p(root):
    return os.path.join(root, STAGE_PATH.lstrip("/"))

def read_stage(root="/"):
    try:
        return json.load(open(_p(root)))["stage"]
    except (OSError, ValueError, KeyError):
        return INSTALLED

def write_stage(stage, root="/", extra=None):
    p = _p(root)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    d = {"stage": stage, "ts": int(time.time())}
    if extra:
        d.update(extra)
    with open(p, "w") as fh:
        json.dump(d, fh, indent=2)
    os.chmod(p, 0o644)
    return p

def transition(new, root="/", extra=None):
    cur = read_stage(root)
    if new not in VALID.get(cur, set()):
        raise ValueError("illegal stage transition %s -> %s" % (cur, new))
    return write_stage(new, root=root, extra=extra)
