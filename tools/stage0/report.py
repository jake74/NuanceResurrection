#!/usr/bin/env python3
import sys, math
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import analyse as a

WA = (790, 1550)   # level one opening, about 50 frames a second at the shipped limit
WB = (1800, 2320)  # the slower stretch after it, about 25 frames a second

def ms(xs, f='%.2f', scale=1.0):
    m, sd, lo, hi = a.stats([x * scale for x in xs])
    return (f + ' ± ' + f + ' (' + f + '–' + f + ')') % (m, sd, lo, hi)

def per_run(prefix, win, metric, f='%.2f', scale=1.0):
    rs = [r for r in a.runs(prefix, *win) if r]
    return [metric(r) for r in rs], rs

def grp(r, names):
    return None

def table_runs(prefix, win):
    rs = [r for r in a.runs(prefix, *win) if r]
    print('| run | windows (s) | game frames | fps | rounds/s (M) | asleep % |')
    print('|---|---|---|---|---|---|')
    for r in rs:
        print('| %s | %d (%.1f) | %d-%d | %.1f | %.2f | %.1f |' % (r['tag'], r['windows'], r['secs'], r['f0'], r['f1'], r['fps'], r['rounds'] / 1e6, r['states']['asleep']))
    print('| mean ± sd (min–max) | | | %s | %s | %s |' % (ms([r['fps'] for r in rs], '%.1f'), ms([r['rounds'] for r in rs], '%.2f', 1e-6), ms([r['states']['asleep'] for r in rs], '%.1f')))
    return rs

def table_mpe(rs, key, label, scale=1e-6, f='%.2f', per='mpe'):
    print('| %s | MPE0 | MPE1 | MPE2 | MPE3 |' % label)
    print('|---|---|---|---|---|')
    for r in rs:
        print('| %s | ' % r['tag'] + ' | '.join(f % (r['mpe'][i][key] * scale) for i in range(4)) + ' |')
    print('| mean ± sd | ' + ' | '.join((f + ' ± ' + f) % tuple(a.stats([r['mpe'][i][key] * scale for r in rs])[:2]) for i in range(4)) + ' |')

GROUPS = [('interpreted packet', [1]), ('BIOS or PE call', [2, 3]), ('budget', [5]), ('no block', [14]), ('mid-block bail', [10]), ('interrupt', [11]), ('other native (chain off, branch pending, halted, icache, breakpoint, local memory dirty)', [6, 7, 8, 9, 12, 13]), ('IL block', [4]), ('MPE not running', [0])]

def ends_pct(r, i, idxs):
    m = r['mpe'][i]
    return 100.0 * sum(m['ends'][j] for j in idxs) / m['turns'] if m['turns'] else 0.0

def table_ends(rs):
    print('| turn ended by (% of all turns) | ' + ' | '.join('MPE%d' % i for i in range(4)) + ' |')
    print('|---|---|---|---|---|')
    for name, idxs in GROUPS:
        cells = []
        for i in range(4):
            xs = [ends_pct(r, i, idxs) for r in rs]
            m, sd, lo, hi = a.stats(xs)
            cells.append('%.1f ± %.1f' % (m, sd))
        print('| %s | ' % name + ' | '.join(cells) + ' |')

def table_states(rs):
    print('| state | mean % of samples | sd | min | max |')
    print('|---|---|---|---|---|')
    for s in a.STATES:
        m, sd, lo, hi = a.stats([r['states'][s] for r in rs])
        print('| %s | %.2f | %.2f | %.2f | %.2f |' % (s, m, sd, lo, hi))

def table_intr(rs):
    print('| interpreted packets by reason (M/s) | MPE0 | MPE1 | MPE2 | MPE3 |')
    print('|---|---|---|---|---|')
    for j, name in enumerate(a.INTR):
        print('| %s | ' % name + ' | '.join('%.3f' % (a.stats([r['mpe'][i]['intr'][j] / 1e6 for r in rs])[0]) for i in range(4)) + ' |')

def table_noblock(rs):
    print('| native turns ended for lack of a block, next packet (M/s) | MPE0 | MPE1 | MPE2 | MPE3 |')
    print('|---|---|---|---|---|')
    for j, name in enumerate(a.NOBLK):
        print('| %s | ' % name + ' | '.join('%.3f' % (a.stats([r['mpe'][i]['noblock'][j] / 1e6 for r in rs])[0]) for i in range(4)) + ' |')

def table_hist(rs):
    names = ['0', '1', '2-3', '4-7', '8-15', '16-31', '32+']
    print('| turns by packets run (% of turns) | MPE0 | MPE1 | MPE2 | MPE3 |')
    print('|---|---|---|---|---|')
    for j, n in enumerate(names):
        print('| %s | ' % n + ' | '.join('%.1f' % (a.stats([100.0 * r['mpe'][i]['hist'][j] / r['mpe'][i]['turns'] for r in rs])[0]) for i in range(4)) + ' |')

def table_flush(rs):
    print('| flushes per second | MPE0 | MPE1 | MPE2 | MPE3 |')
    print('|---|---|---|---|---|')
    for j, n in enumerate(['overlay region', 'main and system bus ranges', 'whole cache, past threshold', 'whole cache, no room', 'FlushRegion calls', 'addresses walked']):
        print('| %s | ' % n + ' | '.join('%.1f' % (a.stats([r['mpe'][i]['flush'][j] for r in rs])[0]) for i in range(4)) + ' |')

def full(prefix, win):
    rs = table_runs(prefix, win); print()
    for key, label, scale, f in [('packets', 'packets/s (M)', 1e-6, '%.1f'), ('turns', 'turns/s (M), all', 1e-6, '%.2f'), ('running', 'running turns/s (M)', 1e-6, '%.2f'), ('native', 'native turns/s (M)', 1e-6, '%.2f'), ('steps', 'chain steps/s (M)', 1e-6, '%.2f'), ('interpreted', 'interpreted packets/s (M)', 1e-6, '%.2f'), ('ppt', 'packets per running turn', 1, '%.2f'), ('bpt', 'blocks per native turn', 1, '%.2f'), ('polling', 'polling turns/s (M)', 1e-6, '%.2f')]:
        table_mpe(rs, key, label, scale, f); print()
    table_ends(rs); print()
    table_states(rs); print()
    table_intr(rs); print()
    table_noblock(rs); print()
    table_hist(rs); print()
    table_flush(rs); print()
    return rs

def sweep(windows):
    for name, (f0, f1) in windows.items():
        print('\n### ' + name + '\n')
        print('| NUANCE_A64_CHAIN | runs | fps per run | fps mean ± sd | vs 32 | rounds/s M mean ± sd | running turns/s M | chain steps/s M | blocks/s M | packets/s M (MPE0-2) | busy % |')
        print('|---|---|---|---|---|---|---|---|---|---|---|')
        rows = []; base = None
        for c in [0, 8, 32, 128]:
            rs = [r for r in a.runs('chain%d' % c, f0, f1) if r]
            m, sd, lo, hi = a.stats([r['fps'] for r in rs])
            rows.append((c, rs, m, sd))
            if c == 32: base = m
        for c, rs, m, sd in rows:
            rd = a.stats([r['rounds'] / 1e6 for r in rs])
            def tot(key, n=4): return sum(a.stats([r['mpe'][i][key] for r in rs])[0] for i in range(n)) / 1e6
            busy = 100 - a.stats([r['states']['asleep'] for r in rs])[0]
            print('| %d | %d | %s | %.1f ± %.1f | %.2f | %.2f ± %.2f | %.2f | %.2f | %.2f | %.1f | %.0f |' % (c, len(rs), ', '.join('%.1f' % r['fps'] for r in rs), m, sd, m / base, rd[0], rd[1], tot('running'), tot('steps'), tot('native') + tot('steps'), tot('packets', 3), busy))

def overhead():
    import re, os
    def gf(tag):
        p = os.path.join(a.S, 'logs', tag + '.err')
        mm = re.findall(r'perf_host: game frames (\d+)', open(p).read()) if os.path.exists(p) else []
        return int(mm[-1]) if mm and not a.disturbed(tag) else None
    print('| configuration | runs | game frames in 80 s, per run | mean ± sd | frames a second (over the 80 s, boot included) | against the pristine core, same setting |')
    print('|---|---|---|---|---|---|')
    ref = {}
    for g, label, cat in [('o2_pr_off', 'pristine core (before this stage), NUANCE_PERF off, no limit', 'off'), ('o2_new_off', 'this stage, NUANCE_PERF off, no limit', 'off'), ('o2_pr_on', 'pristine core, NUANCE_PERF on, no limit', 'on'), ('o2_new_on', 'this stage, NUANCE_PERF on, no limit', 'on'),
                          ('lim_off', 'this stage, NUANCE_PERF off, shipped limit', None), ('lim_on', 'this stage, NUANCE_PERF on, shipped limit', None)]:
        v = [x for x in (gf('%s_%d' % (g, k)) for k in range(1, 9)) if x]
        if not v: continue
        m, sd, lo, hi = a.stats(v)
        if g.startswith('o2_pr'): ref[cat] = m
        rel = '' if g.startswith('o2_pr') or cat is None else '%+.1f%%' % (100 * (m / ref[cat] - 1))
        if g == 'lim_on': rel = '%+.1f%% against NUANCE_PERF off' % (100 * (m / lim_off_m - 1))
        if g == 'lim_off': lim_off_m = m
        print('| %s | %d | %s | %.0f ± %.0f | %.1f | %s |' % (label, len(v), ', '.join(str(x) for x in v), m, sd, m / 80.0, rel))

def compare():
    print('| scene | MPE | packets per running turn: shipped / saturated | blocks per native turn: shipped / saturated | turns ended by an interpreted packet %: shipped / saturated | by budget % | by no block % | largest relative difference |')
    print('|---|---|---|---|---|---|---|---|')
    for name, (f0, f1) in [('WA', (770, 1550)), ('WB', (1770, 2320))]:
        b = [r for r in a.runs('base', f0, f1) if r]; t = [r for r in a.runs('sat', f0, f1) if r]
        for i in range(4):
            def mean(rs, fn): return sum(fn(r) for r in rs) / len(rs)
            vals = []
            cells = []
            for fn in [lambda r: r['mpe'][i]['ppt'], lambda r: r['mpe'][i]['bpt'], lambda r: ends_pct(r, i, [1]), lambda r: ends_pct(r, i, [5]), lambda r: ends_pct(r, i, [14])]:
                x, y = mean(b, fn), mean(t, fn)
                cells.append('%.2f / %.2f' % (x, y))
                if x > 0.5: vals.append(abs(y - x) / x)
            print('| %s | MPE%d | %s | %.1f%% |' % (name, i, ' | '.join(cells), 100 * max(vals) if vals else 0))


def split():
    sets = [('shipped limit, WA', 'base', 770, 1550), ('shipped limit, WB', 'base', 1770, 2320), ('saturated, WA', 'sat', 770, 1550), ('saturated, WB', 'sat', 1770, 2320)]
    data = {n: [r for r in a.runs(p, f0, f1) if r] for n, p, f0, f1 in sets}
    print("| state (percentage of the thread's wall time, mean ± sd over runs) | " + ' | '.join(n for n, _, _, _ in sets) + ' |')
    print('|---|' + '---|' * len(sets))
    for st in a.STATES:
        print('| %s | %s |' % (st, ' | '.join('%.2f ± %.2f' % a.stats([r['states'][st] for r in data[n]])[:2] for n, _, _, _ in sets)))
    for label, extra in [('(the old "dispatcher and the rest": all but generated code, interpreter and sleep)', 0), ('(the same, without NUANCE_PERF counting)', 1)]:
        cells = []
        for n, _, _, _ in sets:
            xs = [100 - r['states']['generated code'] - r['states']['interpreter'] - r['states']['asleep'] - (r['states']['NUANCE_PERF counting'] if extra else 0) for r in data[n]]
            cells.append('%.1f ± %.1f' % a.stats(xs)[:2])
        print('| %s | %s |' % (label, ' | '.join(cells)))

def marginal():
    for name, (f0, f1) in [('WA', (770, 1550)), ('WB', (1770, 2320))]:
        rows = {}
        for c in [0, 8, 32, 128]:
            rs = [r for r in a.runs('chain%d' % c, f0, f1) if r]
            fps = sum(r['fps'] for r in rs) / len(rs)
            turns = sum(sum(m['running'] for m in r['mpe']) for r in rs) / len(rs)
            blocks = sum(sum(m['native'] + m['steps'] for m in r['mpe']) for r in rs) / len(rs)
            rows[c] = (fps, turns, blocks)
        print('\n**%s**\n' % name)
        print('| NUANCE_A64_CHAIN | frame time ms | running turns per frame (thousand) | blocks per frame (thousand) |')
        print('|---|---|---|---|')
        for c, (fps, turns, blocks) in rows.items():
            print('| %d | %.2f | %.0f | %.0f |' % (c, 1000 / fps, turns / fps / 1e3, blocks / fps / 1e3))
        print()
        print('| pair | extra frame time ms | extra turns per frame (thousand) | ns per extra turn (derived) |')
        print('|---|---|---|---|')
        for lo, hi in [(0, 32), (8, 32), (32, 128)]:
            dt = 1000 / rows[lo][0] - 1000 / rows[hi][0]
            dn = (rows[lo][1] / rows[lo][0] - rows[hi][1] / rows[hi][0]) / 1e3
            print('| %d vs %d | %.2f | %.0f | %.1f |' % (lo, hi, dt, dn, dt * 1e6 / (dn * 1e3)))

if __name__ == '__main__':
    if sys.argv[1] == 'split':
        split()
    elif sys.argv[1] == 'marginal':
        marginal()
    elif sys.argv[1] == 'compare':
        compare()
    elif sys.argv[1] == 'sweep':
        sweep({'WA (game frames 770-1550)': (770, 1550), 'WB (game frames 1770-2320)': (1770, 2320)})
    elif sys.argv[1] == 'overhead':
        overhead()
    else:
        full(sys.argv[1], (int(sys.argv[2]), int(sys.argv[3])))
