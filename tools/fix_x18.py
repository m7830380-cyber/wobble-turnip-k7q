#!/usr/bin/env python3
# iOS clobbers x18 (platform register) on context switches. Android code uses it as a scratch register,
# which causes random hangs/corruption. For every function (eh_frame FDE range) that touches x18/w18,
# pick a caller-saved register the function never references and rename x18 -> it, in place.
import sys, struct
from elftools.elf.elffile import ELFFile
from elftools.dwarf.callframe import FDE
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

CANDS = [17, 16, 15, 14, 13, 12, 11, 10, 9]
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)

def regs_used(txt):
    s = set()
    import re
    for m in re.finditer(r'\b[xw](\d+)\b', txt): s.add(int(m.group(1)))
    return s

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
            ents = d.EH_CFI_entries() if d.has_EH_CFI() else []
            for x in ents:
                if isinstance(x, FDE): ranges.append((x.header['initial_location'], x.header['address_range']))
    if not ranges:
        print(f'{path}: no FDEs'); return
    ranges.sort()
    nfun = nins = bad = 0
    for start, size in ranges:
        o = off(start)
        if o is None or size <= 0 or size > 4 << 20: continue
        code = bytes(data[o:o + size])
        insns = list(md.disasm(code, start))
        txts = [f'{i.mnemonic} {i.op_str}' for i in insns]
        if not any(18 in regs_used(t) for t in txts): continue
        used = set()
        for t in txts: used |= regs_used(t)
        new = next((r for r in CANDS if r not in used), None)
        if new is None: bad += 1; print(f'  {path}: no free reg in {start:#x}'); continue
        nfun += 1
        for i, t in zip(insns, txts):
            if 18 not in regs_used(t): continue
            import re
            want = re.sub(r'\b([xw])18\b', lambda m: f'{m.group(1)}{new}', t)
            w = struct.unpack('<I', i.bytes)[0]
            done = False
            for shifts in [(0,), (5,), (10,), (16,), (0, 5), (0, 10), (0, 16), (5, 10), (5, 16), (10, 16), (0, 5, 10), (0, 5, 16), (0, 10, 16), (5, 10, 16), (0, 5, 10, 16)]:
                if not all(((w >> sh) & 31) == 18 for sh in shifts): continue
                nw = w
                for sh in shifts: nw = (nw & ~(31 << sh)) | (new << sh)
                r = list(md.disasm(struct.pack('<I', nw), i.address))
                if r and f'{r[0].mnemonic} {r[0].op_str}' == want:
                    struct.pack_into('<I', data, off(i.address), nw); done = True; nins += 1; break
            if not done: bad += 1; print(f'  {path}: cannot patch {i.address:#x} {t}')
    open(path, 'wb').write(data)
    print(f'{path}: {nfun} functions, {nins} insns renamed, {bad} problems')

for p in sys.argv[1:]: fix(p)
