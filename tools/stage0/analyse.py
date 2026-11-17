#!/usr/bin/env python3
"""Reads NUANCE_PERF logs (see docs/speed-stage0-results.md) and pools the report
windows that lie inside a window of game frames."""
import re, sys, json, math, glob, os
# Logs are read from $STAGE0_OUT/logs (default: ./stage0 under the current directory), as written by run.sh.
S = os.environ.get('STAGE0_OUT', os.path.join(os.getcwd(), 'stage0'))
ENDS = ['not running','interpreted packet','BIOS call','PE call','IL block','budget','chain off','branch pending','halted','icache invalidate','mid-block bail','interrupt','breakpoint','local memory dirty','no block']
INTR = ['delay slot of an interpreted branch','mid-block bail','newly decoded','marked compiled but no block','below the threshold','BIOS table or breakpoint','branch packet','control register','other']
NOBLK = ['at a BIOS or PE address','never decoded','never compilable','marked compiled but no block','below the threshold','block not native']
STATES = ['generated code','interpreter','asleep','dispatcher proper','BIOS and PE handlers','compiling','chain loop between blocks','round level','cache flush loops','overlay hashing','packet decode','media tick and loop between turns','NUANCE_PERF counting']

def num(s): return float(s)

def parse(path):
    wins = []; cur = None
    for line in open(path, errors='replace'):
        if 'perf: packets/s' in line:
            cur = {'mpe': [dict() for _ in range(4)]}
            m = re.search(r'MPE0 (\d+) MPE1 (\d+) MPE2 (\d+) MPE3 (\d+) total (\d+); game VidSync ([\d.]+)/s', line)
            for i in range(4): cur['mpe'][i]['packets'] = num(m.group(i+1))
            cur['total_packets'] = num(m.group(5)); cur['vidsync'] = num(m.group(6))
            continue
        if cur is None: continue
        if 'perf: emulation thread: ' in line:
            m = re.search(r'(\d+)% generated code, (\d+)% interpreter, (\d+)% dispatcher and the rest, (\d+)% asleep', line)
            cur['coarse'] = [num(x) for x in m.groups()]
        elif 'emulation thread, finer' in line:
            cur['samples'] = num(re.search(r'(\d+) samples', line).group(1))
            cur['states'] = {}
            for st in STATES:
                m = re.search(re.escape(st) + r' ([\d.]+)%', line)
                cur['states'][st] = num(m.group(1))
        elif 'perf: of its busy time' in line:
            cur['busy'] = [num(x) for x in re.findall(r'MPE\d (\d+)%', line)]
        elif 'perf: audio' in line:
            cur['audio'] = num(re.search(r'audio (\d+) frames', line).group(1))
        else:
            m = re.search(r'perf: MPE(\d) (.*)', line)
            if m:
                i = int(m.group(1)); t = m.group(2); d = cur['mpe'][i]
                if re.match(r'turns -?\d+/s', t):
                    g = re.match(r'turns (-?\d+)/s: (-?\d+) not running, (-?\d+) native, (-?\d+) interpreted, (-?\d+) BIOS, (-?\d+) PE; turns not given by the round (-?\d+)/s; chain steps (-?\d+)/s; polling turns (-?\d+)/s; packets (-?\d+)/s', t)
                    for k, v in zip(['turns','notrunning','native','interpreted','bios','pe','skipped','steps','polling','packets'], g.groups()): d[k] = num(v)
                elif t.startswith('turns ended by'):
                    body = t.split('turns ended by, per second:')[1].split('; the reasons sum')[0].replace('native turns:', ',').replace(';', ',')
                    parts = [p.strip() for p in body.split(',') if p.strip()]
                    vals = {}
                    for p in parts:
                        mm = re.match(r'(.*) (\d+)$', p)
                        vals[mm.group(1)] = num(mm.group(2))
                    d['ends'] = [vals.get(k, 0.0) for k in ENDS]
                    d['endsum'] = num(re.search(r'sum to (\d+) of (\d+) turns', t).group(1))
                elif t.startswith('native turns that ended for lack'):
                    body = t.split('per second:')[1]
                    d['noblock'] = [num(x) for x in re.findall(r' (\d+)(?:,|$)', body.strip())][:6]
                elif t.startswith('interpreted packets by reason'):
                    body = t.split('per second:')[1]
                    d['intr'] = [num(x) for x in re.findall(r' (\d+)(?:,|;|$)', body.strip())][:9]
                elif t.startswith('turns by packets run'):
                    d['hist'] = [num(x) for x in re.findall(r' (\d+)(?:,|$)', t.split('per second:')[1].strip())][:7]
                elif t.startswith('native cache flushes'):
                    g = re.findall(r'([\d.]+)', t.split('per second:')[1].replace('2-3',''))
                    d['flush'] = [num(x) for x in g]
                elif 'overlay lookups' in t:
                    g = re.match(r'per second: (\d+) overlay lookups, (\d+) native cache flushes, (\d+) interpreter cache flushes', t)
                    d['ovl'] = num(g.group(1)); d['icflush'] = num(g.group(3))
                elif 'native blocks (' in t:
                    g = re.match(r'per second: (\d+) native blocks \(([\d.]+) packets each\), (\d+) packets interpreted, (\d+) of them never', t)
                    d['blocks'] = num(g.group(1)); d['interpreted_old'] = num(g.group(3))
        if 'perf: window' in line:
            m = re.search(r'window ([\d.]+) s, game frames (\d+) to (\d+); rounds (\d+)/s, (\d+)/s with no packet run, (\d+)/s slept', line)
            cur['secs'] = num(m.group(1)); cur['f0'] = int(m.group(2)); cur['f1'] = int(m.group(3))
            cur['rounds'] = num(m.group(4)); cur['empty'] = num(m.group(5)); cur['idle'] = num(m.group(6))
            wins.append(cur); cur = None
    return wins

def pool(wins, f0, f1):
    sel = [w for w in wins if w['f0'] >= f0 and w['f1'] <= f1 and 'rounds' in w]
    if not sel: return None
    T = sum(w['secs'] for w in sel)
    def wavg(get):
        return sum(get(w) * w['secs'] for w in sel) / T
    r = {'windows': len(sel), 'secs': T, 'frames': sum(w['f1'] - w['f0'] for w in sel), 'f0': sel[0]['f0'], 'f1': sel[-1]['f1']}
    r['fps'] = r['frames'] / T
    r['rounds'] = wavg(lambda w: w['rounds']); r['empty'] = wavg(lambda w: w['empty'])
    r['audio'] = wavg(lambda w: w['audio'])
    r['states'] = {s: wavg(lambda w, s=s: w['states'][s]) for s in STATES}
    r['coarse'] = [wavg(lambda w, k=k: w['coarse'][k]) for k in range(4)]
    r['mpe'] = []
    for i in range(4):
        m = {}
        for k in ['packets','turns','notrunning','native','interpreted','bios','pe','skipped','steps','polling','blocks']:
            m[k] = wavg(lambda w, k=k: w['mpe'][i][k])
        for k, n in [('ends', 15), ('noblock', 6), ('intr', 9), ('hist', 7)]:
            m[k] = [wavg(lambda w, k=k, j=j: w['mpe'][i][k][j]) for j in range(n)]
        m['flush'] = [wavg(lambda w, j=j: w['mpe'][i]['flush'][j]) for j in range(6)]
        m['ovl'] = wavg(lambda w: w['mpe'][i]['ovl']); m['icflush'] = wavg(lambda w: w['mpe'][i]['icflush'])
        m['running'] = m['turns'] - m['notrunning']
        m['ppt'] = m['packets'] / m['running'] if m['running'] else 0
        m['bpt'] = (m['native'] + m['steps']) / m['native'] if m['native'] else 0
        m['native_packets'] = m['packets'] - m['interpreted'] - m['bios'] - m['pe']
        r['mpe'].append(m)
    r['turns_all'] = sum(m['turns'] for m in r['mpe'])
    return r

def stats(xs):
    n = len(xs); mean = sum(xs) / n
    sd = math.sqrt(sum((x - mean) ** 2 for x in xs) / (n - 1)) if n > 1 else 0.0
    return mean, sd, min(xs), max(xs)

EXCLUDED = []
def disturbed(tag):
    """A run whose host loop fell behind (the process was held up by the machine for a while)."""
    try:
        txt = open(os.path.join(S, 'logs', tag + '.err'), errors='replace').read()
        m = re.findall(r'perf_host: (\d+) frames in ([\d.]+) s', txt)
        n, secs = int(m[-1][0]), float(m[-1][1])
        return n < 0.99 * secs * 60
    except Exception:
        return False

def runs(prefix, f0, f1):
    out = []
    for p in sorted(glob.glob(os.path.join(S, 'logs', prefix + '_[0-9]*.log')), key=lambda x: int(re.search(r'_(\d+)\.log$', x).group(1))):
        tag = os.path.basename(p)[:-4]
        if disturbed(tag):
            if tag not in EXCLUDED: EXCLUDED.append(tag)
            continue
        r = pool(parse(p), f0, f1)
        if r: r['tag'] = tag
        out.append(r)
    return out

def last_frame(path):
    """Last 'frame N' of a NUANCE_T3K_STATE line, for runs without NUANCE_PERF."""
    best = 0
    for line in open(path, errors='replace'):
        m = re.search(r't3k state: frame (\d+) ', line)
        if m: best = max(best, int(m.group(1)))
    return best

if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'dump':
        for r in runs(sys.argv[2], int(sys.argv[3]), int(sys.argv[4])):
            print(json.dumps(r)[:2000])
