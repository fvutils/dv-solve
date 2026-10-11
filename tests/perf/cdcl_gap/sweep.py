"""CDCL construct-gap sweep (docs/cdcl_construct_plan.md phase 0).

    python tests/perf/cdcl_gap/sweep.py [files... | -]   # env: SWEEP_TO, SWEEP_J, OUT, BIN

Runs every .smt2 fixture under tests/ (the SMT-COMP corpus and build
directories excluded), or the files named on the command line (`-`: read the
list from stdin), twice: default routing, and pure CDCL (DV_NO_BITBLAST=1).
Both runs set DV_LOG, so
each records the `route:` line of its first check-sat, the `cdcl-uncompiled:`
lines and the `cdcl-unknown:` bail. Writes one JSON object per fixture to
$OUT/results.jsonl (default perf-out/cdcl_gap/). analyze.py reports on it."""
import json, os, re, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BIN = os.environ.get('BIN', str(ROOT / 'build' / 'dv-solve-smt2'))
TO = float(os.environ.get('SWEEP_TO', '20'))
OUT = Path(os.environ.get('OUT', ROOT / 'perf-out' / 'cdcl_gap'))
OPS = ['bvand', 'bvor', 'bvxor', 'bvnot', 'bvnand', 'bvnor', 'bvxnor', 'bvshl', 'bvlshr',
       'bvashr', 'bvadd', 'bvsub', 'bvneg', 'bvmul', 'bvudiv', 'bvurem', 'bvsdiv', 'bvsrem',
       'bvsmod', 'bvult', 'bvule', 'bvugt', 'bvuge', 'bvslt', 'bvsle', 'bvsgt', 'bvsge',
       'concat', 'extract', 'zero_extend', 'sign_extend', 'rotate_left', 'rotate_right',
       'repeat', 'ite', 'select', 'store', 'bvcomp', 'distinct']
EVIDENCE = ('route:', 'qfbv-route:', 'cdcl-unknown:', 'cdcl-compile-failed',
            'cdcl-pool-overflow', 'model-validation:')


def fixtures():
    out = []
    for p in sorted((ROOT / 'tests').rglob('*.smt2')):
        rel = p.relative_to(ROOT).as_posix()
        if 'smtcomp' in rel or '/obj/' in rel:
            continue
        out.append(rel)
    return out


def shape(expr):
    """Collapse an uncompiled-constraint dump to its shape: `v12[w64]` -> V64,
    integer constants -> C."""
    s = re.sub(r'v\d+\*?\[w(\d+)\]', r'V\1', expr)
    return re.sub(r'(?<![\w\[:])-?\d+(?![\w\]:>])', 'C', s)


def run(path, extra):
    env = dict(os.environ)
    env.pop('LD_LIBRARY_PATH', None)
    env.update(extra)
    t = time.monotonic()
    try:
        p = subprocess.run([BIN, str(ROOT / path)], capture_output=True, text=True,
                           timeout=TO, env=env)
        out, err, rc = p.stdout, p.stderr, p.returncode
    except subprocess.TimeoutExpired:
        return dict(ans=['timeout'], ms=TO * 1000, ev=[], routes=[], unc={})
    ms = (time.monotonic() - t) * 1000
    ans = [l.strip() for l in out.splitlines() if l.strip() in ('sat', 'unsat', 'unknown')]
    if rc < 0:
        ans.append('crash%d' % rc)
    ev, routes, unc = [], [], {}
    for l in err.splitlines():
        if l.startswith('route:'):
            routes.append(l[6:].strip())
        elif l.startswith('cdcl-uncompiled:'):
            k = shape(l[16:].strip())
            unc[k] = unc.get(k, 0) + 1
        elif l.startswith(EVIDENCE):
            ev.append(l[:200])
    return dict(ans=ans, ms=round(ms, 2), ev=ev, routes=routes, unc=unc)


def ophist(path):
    try:
        s = (ROOT / path).read_text(errors='replace')
    except OSError:
        return {}
    h = {}
    for op in OPS:
        n = len(re.findall(r'[\s(]' + re.escape(op) + r'[\s)]', s))
        if n:
            h[op] = n
    m = re.search(r'set-logic\s+(\S+?)\)', s)
    h['_logic'] = m.group(1) if m else ''
    ws = [int(w) for w in re.findall(r'\(_ BitVec (\d+)\)', s)]
    h['_maxw'] = max(ws) if ws else 0
    h['_checks'] = s.count('(check-sat')
    return h


def one(path):
    d = run(path, {'DV_LOG': '1'})
    c = run(path, {'DV_LOG': '1', 'DV_NO_BITBLAST': '1'})
    return dict(file=path, d_ans=d['ans'], d_ms=d['ms'], d_ev=d['ev'], d_routes=d['routes'],
                c_ans=c['ans'], c_ms=c['ms'], c_ev=c['ev'], c_unc=c['unc'], ops=ophist(path))


def main():
    files = sys.argv[1:] or fixtures()
    if files == ['-']:
        files = [l.strip() for l in sys.stdin if l.strip()]
    OUT.mkdir(parents=True, exist_ok=True)
    n = 0
    with ThreadPoolExecutor(int(os.environ.get('SWEEP_J', '16'))) as ex, \
            open(OUT / 'results.jsonl', 'w') as f:
        for r in ex.map(one, files):
            f.write(json.dumps(r) + '\n')
            f.flush()
            n += 1
    print('%d fixtures -> %s' % (n, OUT / 'results.jsonl'))


if __name__ == '__main__':
    main()
