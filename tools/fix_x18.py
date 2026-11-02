#!/usr/bin/env python3
# iOS zeroes x18 (platform register) on context switches. Android code uses it as a scratch register.
# For every function (eh_frame FDE) that references x18, rename x18 -> a caller-saved register r such that
# r is dead and unreferenced at every instruction where x18 is live/referenced (backward liveness over the CFG).
# Fallback order: a register unused in the whole function (fast path), else liveness-based choice.
import sys, struct, re
from elftools.elf.elffile import ELFFile
from elftools.dwarf.callframe import FDE
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

CANDS = [17, 16, 15, 14, 13, 12, 11, 10, 9]
ALL = (1 << 31) - 1
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
RX = re.compile(r'\b[xw](\d+)\b')

def regs_in(s):
    m = 0
    for g in RX.finditer(s): m |= 1 << int(g.group(1))
    return m

COND = ('cbz', 'cbnz', 'tbz', 'tbnz')
def analyse(insns):
    """returns per-insn (use, def, succ list, liveout_override)"""
    n = len(insns); idx = {i.address: k for k, i in enumerate(insns)}
    info = []
    for k, i in enumerate(insns):
        mn, op = i.mnemonic, i.op_str
        ops = [o.strip() for o in op.split(',')]
        refs = regs_in(op)
        # conservative: uses = every referenced reg except a pure destination; defs only for clear cases
        RMW = ('movk', 'bfi', 'bfxil', 'bfm', 'bfc', 'cas', 'swp', 'ldadd', 'ldclr', 'ldset', 'ldeor', 'ldsmax', 'ldsmin', 'ldumax', 'ldumin', 'csinc', 'ins', 'mov ')
        NODEF = ('st', 'cmp', 'cmn', 'tst', 'b', 'cb', 'tb', 'ret', 'prfm', 'nop', 'msr', 'dmb', 'dsb', 'isb', 'hint', 'bti', 'pac', 'aut', 'ccmp', 'ccmn', 'fcmp', 'brk', 'udf', 'hlt', 'sys', 'dc ', 'ic ')
        if mn.startswith(NODEF) and not mn.startswith(('bic', 'bif', 'bit', 'bsl')):
            d = 0; u = refs
        elif mn.startswith(('cas', 'swp', 'ldadd', 'ldclr', 'ldset', 'ldeor', 'ldsm', 'ldum', 'movk', 'bfi', 'bfxil', 'bfm', 'bfc')):
            d = 0; u = refs
        elif mn in ('ldp', 'ldnp', 'ldpsw', 'ldaxp', 'ldxp'):
            d = regs_in(ops[0]) | regs_in(ops[1]); u = regs_in(','.join(ops[2:]))
        elif ops and RX.fullmatch(ops[0]):
            d = regs_in(ops[0]); u = regs_in(','.join(ops[1:]))
        else:
            d = 0; u = refs
        if mn.startswith(('ld', 'st')):
            b = re.search(r'\[([xw]\d+|sp)', op)
            if b and ('!' in op or re.search(r'\],\s*#', op)):
                r = regs_in(b.group(1)); d &= ~r; u |= r  # writeback: base read+written, keep live
        succ = []; over = None
        nxt = k + 1 if k + 1 < n else None
        if mn in ('bl', 'blr'):
            u |= 0xFF | (regs_in(op) if mn == 'blr' else 0); d |= ((1 << 19) - 1) | (1 << 30)
            if nxt is not None: succ = [nxt]
        elif mn == 'b' or mn.startswith('b.') or mn in COND:
            t = int(ops[-1].lstrip('#'), 16) if ops[-1].startswith('#') else None
            tk = idx.get(t) if t is not None else None
            if mn == 'b':
                if tk is not None: succ = [tk]
                else: over = 0xFF | (1 << 30)  # tail call
            else:
                if tk is not None: succ.append(tk)
                else: over = ALL
                if nxt is not None: succ.append(nxt)
        elif mn in ('br', 'braa', 'brab', 'braaz', 'brabz'):
            over = ALL; u |= refs
        elif mn.startswith('ret') or mn in ('eret',):
            over = 0x3 | (1 << 30); u |= 1 << 30
        elif mn in ('brk', 'udf', 'hlt'):
            over = 0
        else:
            if nxt is not None: succ = [nxt]
            else: over = ALL
        info.append((u, d, succ, over))
    lin = [0] * n; lout = [0] * n
    changed = True
    while changed:
        changed = False
        for k in range(n - 1, -1, -1):
            u, d, succ, over = info[k]
            o = over or 0
            for s_ in succ: o |= lin[s_]
            i_ = u | (o & ~d)
            if o != lout[k] or i_ != lin[k]: lout[k] = o; lin[k] = i_; changed = True
    return info, lin, lout

def fix(path):
    data = bytearray(open(path, 'rb').read())
    with open(path, 'rb') as f:
        e = ELFFile(f)
        segs = [(g['p_vaddr'], g['p_filesz'], g['p_offset']) for g in e.iter_segments() if g['p_type'] == 'PT_LOAD']
        def off(va):
            for v, fs, o in segs:
                if v <= va < v + fs: return va - v + o
        ranges = []
        if e.has_dwarf_info():
            d = e.get_dwarf_info()
            for x in (d.EH_CFI_entries() if d.has_EH_CFI() else []):
                if isinstance(x, FDE): ranges.append((x.header['initial_location'], x.header['address_range']))
    if not ranges: print(f'{path}: no FDEs'); return
    ranges.sort()
    nfun = nins = bad = live = 0
    for start, size in ranges:
        o = off(start)
        if o is None or size <= 0 or size > 4 << 20: continue
        insns = list(md.disasm(bytes(data[o:o + size]), start))
        txts = [f'{i.mnemonic} {i.op_str}' for i in insns]
        refs = [regs_in(i.op_str) for i in insns]
        if not any(r >> 18 & 1 for r in refs): continue
        used = 0
        for r in refs: used |= r
        new = next((c for c in CANDS if not used >> c & 1), None)
        if new is None:
            info, lin, lout = analyse(insns)
            busy = 0  # registers live or referenced wherever x18 is live/referenced
            for k in range(len(insns)):
                if (refs[k] | lout[k] | lin[k]) >> 18 & 1: busy |= refs[k] | lout[k] | lin[k] | info[k][1]  # incl. defs (e.g. calls clobbering caller-saved regs)
            new = next((c for c in CANDS + [30] if not busy >> c & 1), None)
            if new is None: bad += 1; print(f'  {path}: no free reg in {start:#x}'); continue
            live += 1
        nfun += 1
        for i, t, r in zip(insns, txts, refs):
            if not r >> 18 & 1: continue
            want = re.sub(r'\b([xw])18\b', lambda m: f'{m.group(1)}{new}', t)
            w = struct.unpack('<I', i.bytes)[0]; done = False
            for shifts in [(0,), (5,), (10,), (16,), (0, 5), (0, 10), (0, 16), (5, 10), (5, 16), (10, 16), (0, 5, 10), (0, 5, 16), (0, 10, 16), (5, 10, 16), (0, 5, 10, 16)]:
                if not all(((w >> sh) & 31) == 18 for sh in shifts): continue
                nw = w
                for sh in shifts: nw = (nw & ~(31 << sh)) | (new << sh)
                rr = list(md.disasm(struct.pack('<I', nw), i.address))
                if rr and f'{rr[0].mnemonic} {rr[0].op_str}' == want:
                    struct.pack_into('<I', data, off(i.address), nw); done = True; nins += 1; break
            if not done: bad += 1; print(f'  {path}: cannot patch {i.address:#x} {t}')
    open(path, 'wb').write(data)
    print(f'{path}: {nfun} functions ({live} via liveness), {nins} insns renamed, {bad} problems')

for p in sys.argv[1:]: fix(p)
