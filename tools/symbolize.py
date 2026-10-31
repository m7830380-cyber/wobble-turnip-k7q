import re, sys, subprocess, bisect, os
err, gdbf, libdir = sys.argv[1:4]
maps = []
for l in open(err, errors='replace'):
    m = re.match(r'MAP (\S+) (0x[0-9a-f]+) ([0-9a-f]+)', l)
    if m: maps.append((int(m.group(2), 16), int(m.group(3), 16), os.path.basename(m.group(1))))
syms = {}
def symtab(lib):
    if lib in syms: return syms[lib]
    out = subprocess.run(['llvm-nm', '-D', '--defined-only', os.path.join(libdir, lib)], capture_output=True, text=True).stdout
    t = sorted((int(a, 16), n) for a, _, n in (x.split(None, 2) for x in out.splitlines() if len(x.split()) == 3))
    syms[lib] = t; return t
def sym(a):
    for b, s, n in maps:
        if b <= a < b + s:
            off = a - b; t = symtab(n); i = bisect.bisect_right([x[0] for x in t], off) - 1
            f = f'{t[i][1]}+0x{off - t[i][0]:x}' if i >= 0 else '?'
            return f'{n}+0x{off:x} ({f})'
    return None
seen = set()
for l in open(gdbf, errors='replace'):
    for h in re.findall(r'0x[0-9a-f]{6,}', l):
        a = int(h, 16)
        if a in seen: continue
        r = sym(a)
        if r: seen.add(a); print(h, r)
