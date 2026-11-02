#!/usr/bin/env python3
"""Check that the MPC port still matches the desktop plugin.

    python3 tools/check_mpc_sync.py [mpc/vst | mpc/haze]

The desktop plugin's parameters live in Source/PluginProcessor.cpp
(createLayout); each MPC port repeats them in three places by hand:
<port>/params.json (the list and its defaults), <port>/layout.conf (the
touchscreen pages) and its engine (mpc/src/radio_core.h, shared, plus the
port's own engine file: set_param/setParam, get_param/getParam). This reads
them all and reports anything that drifted. Exit status 1 on an error, 0 if
only warnings. mpc/build.sh runs it first. The default port is mpc/vst (SDR
Radio); mpc/haze is SDR Haze, the radio plus the Haze step sequencer, whose
keys exist only on the MPC.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Desktop parameters the MPC shows differently, on purpose.
SPLIT = {"freq": ["mhz", "khz"]}          # one MHz float -> two int knobs
# The desktop's band_* switches are one list on MPC: rows band_1..band_N
# (Bands.h order), each a tap-to-toggle tile.
ROWS = {"band_": "band_"}                 # desktop id prefix -> MPC row prefix
# MPC-only keys: buttons the engine acts on, readouts it reports, list rows
# (preset_1..8: the desktop keeps its presets in the editor), and the
# wrapper's project state.
MPC_ONLY_SET = {"tune_down", "tune_up", "scan_survey", "scan_skip", "store",
                "list_prev", "list_next", "list_add", "list_block", "list_clear", "list_view"}
MPC_ONLY_GET = {"freq_text", "info", "status", "signal", "scan_text", "list_text", "chan_state", "hint_text", "count_text"}
MPC_ONLY_ROWS = {"preset_": 8, "list_": 24}     # list_: one page of saved (or blocked) channels
# Option lists the MPC extends past the desktop's: the extra options go at the end.
MORE_OPTIONS = {"scan_src": 4}                  # All (the desktop's Band), Presets, Saved, New (MPC only)
ENGINE_ONLY = {"state"}

# Per port: its engine files, and the keys only it has.
CORE = "mpc/src/radio_core.h"
PORTS = {
    "mpc/vst":  dict(engine=[CORE, "mpc/src/sdr_mpc_engine.cpp"], set=set(), get=set(), rows={}),
    "mpc/haze": dict(engine=[CORE, "mpc/src/sdr_haze_engine.cpp"],
                     set={"run", "seq_rate", "seq_len", "lane", "clear_lane", "mix", "rp_rate", "stop_time", "pitch",
                          "flt", "ph_depth", "dl_time", "rv_size", "dist_drive"},
                     get={"lane_text"}, rows={"step_": 16}),
}
PORT = "mpc/vst"
# Option labels may be shortened on MPC; only the count must match.

errors, warnings = [], []
def err(msg):  errors.append(msg)
def warn(msg): warnings.append(msg)


def read(rel):
    with open(os.path.join(ROOT, rel)) as f:
        return f.read()


def num(s):
    return float(s.strip().rstrip("f"))


def desktop_params():
    """id -> {kind, min, max, default, options} from createLayout(); empty in a checkout without the desktop plugin
    (the public MPC repo), where only the MPC files are checked against each other."""
    if not os.path.exists(os.path.join(ROOT, "Source/PluginProcessor.cpp")):
        return {}
    src = read("Source/PluginProcessor.cpp")
    body = src[src.index("createLayout()"):]
    body = body[:body.index("return layout;")]

    # Helper lambdas (unit, bipolar, ...): name -> (min, max, default or None).
    helpers = {}
    for m in re.finditer(r"auto\s+(\w+)\s*=\s*\[&\][^{]*\{(.*?)\};", body, re.S):
        r = re.search(r"NormalisableRange<float>\s*\(([^,]+),([^,)]+)", m.group(2))
        d = re.search(r"\)\s*,\s*(-?[\d.]+f?)\s*\)\s*\)", m.group(2))
        if r:
            helpers[m.group(1)] = (num(r.group(1)), num(r.group(2)), num(d.group(1)) if d else None)
        elif "AudioParameterBool" in m.group(2):
            helpers[m.group(1)] = "bool"
        body = body.replace(m.group(0), "")

    out = {}
    for stmt in body.split(";"):
        m = re.search(r'ParameterID\s*\{\s*"(\w+)"', stmt)
        if m:
            pid = m.group(1)
            if "AudioParameterFloat" in stmt:
                r = re.search(r"NormalisableRange<float>\s*\(([^,]+),([^,)]+)", stmt)
                d = re.search(r"\)\s*,\s*([-\d.f]+)\s*\)\s*\)\s*$", stmt.strip())
                out[pid] = dict(kind="float", min=num(r.group(1)), max=num(r.group(2)),
                                default=num(d.group(1)) if d else None)
            elif "AudioParameterBool" in stmt:
                d = re.search(r"(true|false)\s*\)\s*\)\s*$", stmt.strip())
                out[pid] = dict(kind="bool", default=1 if d and d.group(1) == "true" else 0)
            elif "AudioParameterChoice" in stmt:
                opts = re.search(r"StringArray\s*\{([^}]*)\}\s*,\s*(\d+)", stmt)
                out[pid] = dict(kind="choice", options=re.findall(r'"([^"]*)"', opts.group(1)),
                                default=int(opts.group(2)))
            else:
                warn("desktop: can't tell the type of %r" % pid)
            continue
        m = re.search(r"\b(\w+)\s*\(\s*\"(\w+)\"\s*,\s*\"[^\"]*\"\s*,\s*(true|false)\s*\)\s*$", stmt.strip())
        if m and helpers.get(m.group(1)) == "bool":
            out[m.group(2)] = dict(kind="bool", default=1 if m.group(3) == "true" else 0)
            continue
        m = re.search(r"\b(\w+)\s*\(\s*\"(\w+)\"\s*,\s*\"[^\"]*\"\s*(?:,\s*([-\d.f]+))?\s*\)\s*$", stmt.strip())
        if m and m.group(1) in helpers and helpers[m.group(1)] != "bool":
            lo, hi, d = helpers[m.group(1)]
            out[m.group(2)] = dict(kind="float", min=lo, max=hi,
                                   default=num(m.group(3)) if m.group(3) else d)

    # The band switches come from a loop over bands::all(): one bool per band in Source/Bands.h.
    loop = re.search(r'for\s*\(const auto& b : bands::all\(\)\).*?==\s*"(\w+)"', body, re.S)
    if loop:
        for bid in re.findall(r'\{\s*"(band_\w+)"', read("Source/Bands.h")):
            out[bid] = dict(kind="bool", default=1 if bid == loop.group(1) else 0)
    return out


def engine_source():
    return "".join(read(f) for f in PORTS[PORT]["engine"])


def engine_keys():
    """Keys with an is ("...") case in the set and get functions of the port's engine files."""
    def keys_in(names):
        out = set()
        for f in PORTS[PORT]["engine"]:
            src = read(f)
            for fn in names:
                start = src.find("    " + fn + " (")
                if start < 0:
                    continue
                nxt = re.search(r"\n    (?:void|int|bool|float|double|static|template|const|std::|private:|public:)\b", src[start + 10:])
                chunk = src[start: start + 10 + nxt.start()] if nxt else src[start:]
                out |= set(re.findall(r'is\s*\(\s*"(\w+)"\s*\)', chunk))
        return out
    return keys_in(["void set_param", "bool setParam"]), keys_in(["int get_param", "int getParam"])


def layout_keys():
    keys, qlinks = set(), set()
    for line in read(PORT + "/layout.conf").splitlines():
        line = line.split("#", 1)[0]
        m = re.search(r"\bkey=(\w+)", line)
        if m and line.split()[0] == "list":       # a list's rows are <key>_1 .. <key>_<cols*rows>
            n = int(re.search(r"\bcols=(\d+)", line).group(1)) * int(re.search(r"\brows=(\d+)", line).group(1))
            keys.update("%s_%d" % (m.group(1), i) for i in range(1, n + 1))
        elif m:
            keys.add(m.group(1))
        m = re.match(r"\s*qlinks\s+\"[^\"]*\"\s*=\s*(.*)", line)
        if m:
            qlinks.update(k.strip() for k in m.group(1).split(",") if k.strip() not in ("", "-"))
    return keys, qlinks


def row_keys(mpc, prefix):
    return [k for k in mpc if re.fullmatch(re.escape(prefix) + r"\d+", k)]


def main():
    global PORT
    if len(sys.argv) > 1:
        PORT = sys.argv[1].rstrip("/")
    if PORT not in PORTS or not os.path.isdir(os.path.join(ROOT, PORT)):
        print("error: unknown port %r (one of %s)" % (PORT, ", ".join(PORTS)))
        return 1
    only_set, only_get = MPC_ONLY_SET | PORTS[PORT]["set"], MPC_ONLY_GET | PORTS[PORT]["get"]
    only_rows = dict(MPC_ONLY_ROWS, **PORTS[PORT]["rows"])
    desk = desktop_params()
    mpc = {p["key"]: p for p in json.loads(read(PORT + "/params.json"))["params"]}
    sets, gets = engine_keys()
    shown, qlinks = layout_keys()

    # 0. Desktop switches that are list rows on MPC: one row each, same count.
    row_handled = set()
    for dprefix, mprefix in ROWS.items():
        ids = [pid for pid in desk if pid.startswith(dprefix)]
        rows = row_keys(mpc, mprefix)
        if desk and len(rows) != len(ids):
            err("params.json: %d %s* rows, desktop has %d %s* switches" % (len(rows), mprefix, len(ids), dprefix))
        row_handled.update(ids)
        row_handled.update(rows)
    for prefix, n in only_rows.items():
        rows = row_keys(mpc, prefix)
        if len(rows) != n:
            err("params.json: %d %s* rows, expected %d" % (len(rows), prefix, n))
        row_handled.update(rows)

    # 1. Every desktop parameter reaches the MPC, with the same range and default.
    for pid, d in desk.items():
        if pid in row_handled:
            continue
        if pid in SPLIT:
            for k in SPLIT[pid]:
                if k not in mpc:
                    err("params.json: %r is missing (it carries desktop %r)" % (k, pid))
            khz = round(d["default"] * 1000)
            for k, want in zip(SPLIT[pid], (khz // 1000, khz % 1000)):
                if k in mpc and int(mpc[k].get("default", 0)) != want:
                    err("params.json: %r default is %s, desktop %r %s gives %d"
                        % (k, mpc[k].get("default"), pid, d["default"], want))
            continue
        p = mpc.get(pid)
        if p is None:
            err("params.json: desktop parameter %r is missing" % pid)
            continue
        if d["kind"] == "float":
            if "options" in p:
                err("params.json: %r is a list on MPC but a float on desktop" % pid)
                continue
            for f in ("min", "max", "default"):
                if d.get(f) is not None and abs(float(p.get(f, 0)) - d[f]) > 1e-6:
                    err("params.json: %r %s is %s, desktop has %s" % (pid, f, p.get(f), d[f]))
        else:
            n = 2 if d["kind"] == "bool" else len(d["options"])
            if len(p.get("options", [])) != MORE_OPTIONS.get(pid, n):
                err("params.json: %r needs %d options (desktop %s), has %d"
                    % (pid, n, d["kind"], len(p.get("options", []))))
            if int(p.get("default", 0)) != d["default"]:
                err("params.json: %r default is %s, desktop has %s" % (pid, p.get("default"), d["default"]))

    known = set(desk) | {k for ks in SPLIT.values() for k in ks}
    for k, p in mpc.items():
        if not desk or k in known or k in only_set or k in only_get or k in row_handled:
            continue
        warn("params.json: %r is not on the desktop (add it to MPC_ONLY_* in this script if that's intended)" % k)

    # 2. The engine handles every key the MPC can send, and every readout.
    #    List rows go through rowOf ("<prefix>N"), not one case each.
    src = engine_source()
    for prefix in list(ROWS.values()) + list(only_rows):
        if 'rowOf (key, "%s"' % prefix not in src:
            err("engine: no rowOf case for the %s* rows" % prefix)
    for k, p in mpc.items():
        if k in row_handled:
            continue
        if p.get("display") == "string":
            if k not in gets:
                err("engine: readout %r has no case in get_param" % k)
        elif k not in sets:
            err("engine: %r has no case in set_param" % k)
    for k in (sets | gets) - set(mpc) - ENGINE_ONLY:
        err("engine: handles %r, which params.json doesn't have" % k)

    # 3. The pages only use real keys, and show everything.
    for k in sorted((shown | qlinks) - set(mpc)):
        err("layout.conf: key %r isn't in params.json" % k)
    for k in mpc:
        if k not in shown:
            warn("layout.conf: %r isn't on any page" % k)

    for w in warnings:
        print("warning: " + w)
    for e in errors:
        print("error: " + e)
    if not errors:
        print("mpc sync %s: OK (%s, %d MPC params)" % (PORT, "%d desktop params" % len(desk) if desk else "no desktop plugin here", len(mpc)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
