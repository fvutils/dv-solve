"""Report on a CDCL construct-gap sweep (docs/cdcl_construct_plan.md phase 0).

    python tests/perf/cdcl_gap/analyze.py [-v] [results.jsonl]

Prints the plan's baseline table (pure-CDCL agreement, wrong verdicts, CDCL
start share, uncompiled constraints), pure-CDCL results by the route each
fixture takes today, failures by bail reason and the uncompiled-shape tally.
-v also lists every failing fixture."""
import collections, json, os, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
args = [a for a in sys.argv[1:] if not a.startswith('-')]
src = Path(args[0]) if args else Path(os.environ.get('OUT', ROOT / 'perf-out' / 'cdcl_gap')) / 'results.jsonl'
R = [json.loads(l) for l in open(src)]


def final(a):
    return a[-1] if a else 'none'


def route(r):
    """The rule that picks the engine for the fixture's first check-sat."""
    if not r.get('d_routes'):
        return '?'
    m = re.match(r'(\w+) nb=(\S+) qfbv=(\S+) logic=(\d)', r['d_routes'][0])
    if not m:
        return '?'
    eng, nb, qfbv, logic = m.groups()
    if nb != '-':
        return nb
    if qfbv != '-':
        return qfbv
    if logic != '0' and eng == 'bitblast':
        return 'logic route'
    return 'CDCL already' if eng == 'cdcl' else eng


def bail(r):
    for e in r['c_ev']:
        if e.startswith('cdcl-unknown'):
            b = re.sub(r'; vars=.*', '', e)[14:]
            b = re.sub(r'uncompiled-constraints=0\b', 'all compiled', b)
            return re.sub(r'uncompiled-constraints=\d+', 'some uncompiled', b)
    for e in r['c_ev']:
        if e.startswith('model-validation'):
            return 'bad model'
    return ''


definitive = [r for r in R if final(r['d_ans']) in ('sat', 'unsat')]
ok = [r for r in definitive if final(r['c_ans']) == final(r['d_ans'])]
wrong = [r for r in definitive if final(r['c_ans']) in ('sat', 'unsat')
         and final(r['c_ans']) != final(r['d_ans'])]
fails = [r for r in definitive if r not in ok]
starts = collections.Counter(rt.split()[0] for r in R for rt in r.get('d_routes', []))
unc = collections.Counter()
unc_files = collections.Counter()
for r in R:
    for k, v in r.get('c_unc', {}).items():
        unc[k] += v
        unc_files[k] += 1

print('| Metric | Value |\n|---|---|')
print('| Fixtures swept | %d, of which %d have a definitive default answer |' % (len(R), len(definitive)))
print('| Pure CDCL (`DV_NO_BITBLAST=1`) agrees with default | %d / %d |' % (len(ok), len(definitive)))
print('| Pure CDCL gives a wrong verdict | %d |' % len(wrong))
print('| Check-sats that start on CDCL (default routing) | %d / %d |'
      % (starts['cdcl'], sum(starts.values())))
print('| Constraints left uncompiled (pure CDCL) | %d in %d files |'
      % (sum(unc.values()), sum(1 for r in R if r.get('c_unc'))))
for r in wrong:
    print('WRONG: %s default=%s cdcl=%s' % (r['file'], final(r['d_ans']), final(r['c_ans'])))

print('\n| Route today | n | CDCL ok |\n|---|---|---|')
byroute = collections.defaultdict(lambda: [0, 0])
for r in definitive:
    b = byroute[route(r)]
    b[0] += 1
    b[1] += r in ok
for k, (n, good) in sorted(byroute.items(), key=lambda kv: -kv[1][0]):
    print('| `%s` | %d | %d |' % (k, n, good))

print('\n== pure-CDCL failures by bail reason ==')
for k, v in collections.Counter(bail(r) or '[%s]' % final(r['c_ans']) for r in fails).most_common():
    print('%5d  %s' % (v, k[:110]))

print('\n== uncompiled constraint shapes (count, files) ==')
for k, v in unc.most_common(40):
    print('%7d %4df  %s' % (v, unc_files[k], k[:120]))

print('\n== CDCL-ok, default not on CDCL, CDCL faster (>30 ms default) ==')
for r in ok:
    if route(r) != 'CDCL already' and r['c_ms'] < r['d_ms'] * 0.8 and r['d_ms'] > 30:
        print('  %-70s cdcl=%8.1f default=%8.1f %s' % (r['file'], r['c_ms'], r['d_ms'], route(r)))

if '-v' in sys.argv:
    print('\n== failures ==')
    for r in fails:
        print('%-72s %-8s %-60s %s' % (r['file'], final(r['c_ans']), bail(r)[:60], route(r)))
