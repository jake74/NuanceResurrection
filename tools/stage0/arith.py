#!/usr/bin/env python3
"""Section 3.4 arithmetic for one set of runs (prefix) and window; prints every figure with its working."""
import sys
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import analyse as a

def main(prefix, f0, f1, c_pe=0.78, c_links=(1.0, 2.0)):
    rs = [r for r in a.runs(prefix, f0, f1) if r]
    n = len(rs)
    def mean(fn): return sum(fn(r) for r in rs) / n
    sh = {s: mean(lambda r, s=s: r['states'][s]) / 100.0 for s in a.STATES}   # share of all samples (wall time)
    asleep = sh['asleep']; cnt = sh['NUANCE_PERF counting']; busy = 1.0 - asleep - cnt  # busy time without NUANCE_PERF's own counting
    rounds = mean(lambda r: r['rounds'])
    steps = sum(mean(lambda r, i=i: r['mpe'][i]['steps']) for i in range(4))
    native_turns = sum(mean(lambda r, i=i: r['mpe'][i]['native']) for i in range(4))
    interp = sum(mean(lambda r, i=i: r['mpe'][i]['interpreted']) for i in range(4))
    running = sum(mean(lambda r, i=i: r['mpe'][i]['running']) for i in range(4))
    blocks = steps + native_turns
    out = {}
    print('runs %d, windows %s, fps %.1f' % (n, [r['windows'] for r in rs], mean(lambda r: r['fps'])))
    print('shares of wall time (measured, sampler): ' + ', '.join('%s %.2f%%' % (s, 100 * sh[s]) for s in a.STATES))
    print('busy fraction b = 1 - asleep - counting = %.4f (counting, the cost of NUANCE_PERF itself, is %.2f%% of wall time and is left out of every share of busy time below; the ns figures use the measured wall-time shares and event rates of the same run)' % (busy, 100*cnt))
    print('rounds/s %.3fM (measured); running turns/s %.3fM; native turns/s %.3fM; chain steps/s %.3fM; blocks/s %.3fM; interpreted packets/s %.3fM' % (rounds/1e6, running/1e6, native_turns/1e6, steps/1e6, blocks/1e6, interp/1e6))
    # 1. chain loop between blocks
    s6 = sh['chain loop between blocks']
    print('(1) block-to-block transition inside turns: s_6 = %.2f%% of wall time = %.2f%% of busy time (derived: s_6/b); per chain step %.2f ns (derived: s_6 x 1e9 / steps)' % (100*s6, 100*s6/busy, s6*1e9/steps))
    nat012 = sum(mean(lambda r, i=i: r['mpe'][i]['native']) for i in range(3))
    bud012 = sum(mean(lambda r, i=i: r['mpe'][i]['ends'][5]) for i in range(3))
    iters = steps + nat012 - bud012
    print('    state 6 also holds the last loop iteration of every chain that ends by a break (it runs the tests and then leaves): iterations/s = steps + native turns of MPE0-2 - turns ended by budget = %.3fM + %.3fM - %.3fM = %.3fM; per iteration %.2f ns (derived)' % (steps/1e6, nat012/1e6, bud012/1e6, iters/1e6, s6*1e9/iters))
    # 2. entry/exit
    ee = blocks * c_pe * 1e-9
    print('(2) block entry and exit: not separable by the sampler (the call, prologue and epilogue are in state 1 with the block body). Lower bound from the E4 micro-benchmark: %.2f ns per call, so %.2f%% of busy time (derived: blocks/s x c_pe / b)' % (c_pe, 100*ee/busy))
    # 3. interpreter plus turns
    s2 = sh['interpreter']
    s0 = sh['dispatcher proper']
    c_turn0 = s0 * 1e9 / running
    int_turn_cost = interp * c_turn0 * 1e-9
    print('(3) interpreter: s_2 = %.2f%% of wall time = %.2f%% of busy; the turns it ends: interpreted packets/s x c_turn where c_turn = s_0 x 1e9 / running turns = %.1f ns (derived), giving %.2f%% of wall time (%.2f%% of busy). Together %.2f%% of busy time' % (100*s2, 100*s2/busy, c_turn0, 100*int_turn_cost, 100*int_turn_cost/busy, 100*(s2+int_turn_cost)/busy))
    # 4. round
    print('(4) one round: busy time / rounds = %.1f ns (derived: b x 1e9 / rounds); wall time per round %.1f ns' % (busy*1e9/rounds, 1e9/rounds))
    for s in a.STATES:
        if s == 'asleep': continue
        print('      %-28s %6.1f ns per round (derived: share x 1e9 / rounds), %5.1f%% of busy' % (s, sh[s]*1e9/rounds, 100*sh[s]/busy))
    # D2, D1
    c_step = s6 * 1e9 / steps
    for c_link in c_links:
        d2 = s6 / busy + steps * (c_pe - c_link) * 1e-9 / busy
        print('D2 (plan 3.4) with c_pe %.2f ns (E4), c_link %.1f ns: s_6/b + steps x (c_pe - c_link)/b = %.2f%% + %.2f%% = %.2f%%' % (c_pe, c_link, 100*s6/busy, 100*steps*(c_pe-c_link)*1e-9/busy, 100*d2))
    # what in-situ c_pe would be needed for D2 of 15% with c_link 1.5
    need = (0.15 * busy - s6) / steps * 1e9 + 1.5
    print('     c_pe needed (in place of E4) for D2 = 15%% with c_link 1.5 ns: %.1f ns' % need)
    nonc = sum(mean(lambda r, i=i: r['mpe'][i]['noblock'][2]) for i in range(4))
    nonc_all = sum(mean(lambda r, i=i: r['mpe'][i]['ends'][14]) for i in range(4))
    d1 = (s2 * (2.0/3.0) + interp * c_turn0 * 1e-9 + nonc * (c_turn0 - c_step) * 1e-9) / busy
    print('D1 (plan 3.4) = [s_2 x 2/3 + n_int x c_turn + (turns ended by a never-compilable next packet = %.3fM/s) x (c_turn - c_step)] / b = [%.2f%% + %.2f%% + %.2f%%]/b = %.2f%%' % (nonc/1e6, 100*s2*2/3, 100*interp*c_turn0*1e-9, 100*nonc*(c_turn0-c_step)*1e-9, 100*d1))
    print('   (turns ended for lack of any block: %.3fM/s of which never compilable %.3fM/s)' % (nonc_all/1e6, nonc/1e6))
    tot = [sum(mean(lambda r, i=i, j=j: r['mpe'][i]['intr'][j]) for i in range(3)) for j in range(9)]   # MPE0-2, packets/s by reason
    print('interpreted packets/s by reason, MPE0-2 (measured): ' + ', '.join('%s %.3fM' % (nm, t/1e6) for nm, t in zip(a.INTR, tot)))
    addressable = tot[0] + tot[1] + tot[5] + tot[6] + tot[7] + tot[8]
    wait = tot[3]
    print('   of which Stage-1 addressable (delay slots, bails, never-compiled of any kind): %.3fM/s; marked-compiled-but-no-block (the R9 wait loop): %.3fM/s; newly decoded or below threshold: %.3fM/s' % (addressable/1e6, wait/1e6, (tot[2]+tot[4])/1e6))
    print('   interpreter and cache flush: interpreter cache flushes/s (MPE0-3, measured): ' + ', '.join('%.0f' % mean(lambda r, i=i: r['mpe'][i]['icflush']) for i in range(4)) + '; overlay lookups/s: ' + ', '.join('%.0f' % mean(lambda r, i=i: r['mpe'][i]['ovl']) for i in range(4)))
    frac_addr = addressable / sum(tot)
    d1a = (s2 * (2.0/3.0) * frac_addr + addressable * c_turn0 * 1e-9) / busy
    print('D1 restricted to the Stage-1-addressable interpreted packets (share of MPE0-2 interpreted packets %.0f%%): [s_2 x 2/3 x share + addressable x c_turn]/b = %.2f%%' % (100*frac_addr, 100*d1a))
    return

if __name__ == '__main__':
    main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
