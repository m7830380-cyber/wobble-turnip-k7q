#include <dlfcn.h>
#define _GNU_SOURCE
// Minimal arm64 ELF shared-object loader: Android bionic .so -> iOS (or Linux host for testing)
#include "elf_loader.h"
#include "elf_compat.h"
#include <stddef.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#endif

#define RA_ABS64 257
#define RA_GLOB_DAT 1025
#define RA_JUMP_SLOT 1026
#define RA_RELATIVE 1027
#define PG 0x4000UL
#define PDOWN(x) ((x) & ~(PG - 1))
#define PUP(x) PDOWN((x) + PG - 1)

static so_module *g_mods;
#ifdef __APPLE__
int so_jit_mode(void); int so_jit_traced(void);
#endif
/* TXM: executable ranges get a writable mirror; all writes into code go through so_wptr */
static struct { uint8_t *rx, *rw; size_t len; } g_mir[256]; static int g_nmir;
void *so_wptr(void *p) {
  for (int i = 0; i < g_nmir; i++) if ((uint8_t *)p >= g_mir[i].rx && (uint8_t *)p < g_mir[i].rx + g_mir[i].len)
    return g_mir[i].rw + ((uint8_t *)p - g_mir[i].rx);
  return p;
}
int so_is_mirrored(void *p) { return so_wptr(p) != p; }
#ifdef __APPLE__
extern void *so_jit_prepare(void *addr, size_t len, uint8_t **rw);
#endif
/* TXM: whole image comes from the debugger as RX (+RW mirror); data pages are then replaced by plain RW */
static uint8_t *g_img_rw;
static uint8_t *alloc_image(size_t len, int *txm) {
  *txm = 0; g_img_rw = NULL;
#ifdef __APPLE__
  if (so_jit_mode() == 2) {
    if (!so_jit_traced()) { fprintf(stderr, "[jit] TXM device but no debugger attached\n"); return MAP_FAILED; }
    uint8_t *rw = NULL, *rx = so_jit_prepare(NULL, len, &rw);
    if (!rx) return MAP_FAILED;
    *txm = 1; g_img_rw = rw; return rx;
  }
#endif
  return mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}
static void add_mirror(uint8_t *rx, size_t len, uint8_t *rw) {
  if (g_nmir < 256) { g_mir[g_nmir].rx = rx; g_mir[g_nmir].rw = rw; g_mir[g_nmir].len = len; g_nmir++; }
}

static so_resolver g_resolver, g_late;
void so_set_resolver(so_resolver r) { g_resolver = r; }
void so_set_late_resolver(so_resolver r) { g_late = r; }

static void *lookup_in(so_module *m, const char *name) {
  Elf64_Sym *syms = m->dynsym;
  for (size_t i = 1; i < m->nsyms; i++) {
    Elf64_Sym *s = &syms[i];
    if (s->st_shndx == SHN_UNDEF || !s->st_value) continue;
    if (strcmp(m->dynstr + s->st_name, name) == 0) return m->base + s->st_value;
  }
  return NULL;
}
void *so_sym(so_module *m, const char *name) { return lookup_in(m, name); }

static void *resolve(const char *name) {
  void *p = g_resolver ? g_resolver(name) : NULL;  // host shims take priority
  if (p) return p;
  for (so_module *m = g_mods; m; m = m->next)
    if ((p = lookup_in(m, name))) return p;
  return g_late ? g_late(name) : NULL;
}

static size_t count_syms(uint8_t *base, Elf64_Dyn *dyn) {
  uint32_t *gh = NULL, *h = NULL;
  for (Elf64_Dyn *d = dyn; d->d_tag; d++) {
    if (d->d_tag == DT_GNU_HASH) gh = (uint32_t *)(base + d->d_un.d_ptr);
    if (d->d_tag == DT_HASH) h = (uint32_t *)(base + d->d_un.d_ptr);
  }
  if (h) return h[1];
  if (!gh) return 0;
  uint32_t nb = gh[0], so = gh[1], bw = gh[2];
  uint32_t *buckets = gh + 4 + bw * 2, *chain = buckets + nb, last = 0;
  for (uint32_t i = 0; i < nb; i++) if (buckets[i] > last) last = buckets[i];
  if (last < so) return so;
  while (!(chain[last - so] & 1)) last++;
  return last + 1;
}


// Android packed relocations (APS2): SLEB128 stream, grouped
static int64_t sleb(const uint8_t **pp) {
  const uint8_t *p = *pp; int64_t r = 0; int sh = 0; uint8_t b;
  do { b = *p++; r |= (int64_t)(b & 0x7f) << sh; sh += 7; } while (b & 0x80);
  if (sh < 64 && (b & 0x40)) r |= -((int64_t)1 << sh);
  *pp = p; return r;
}
static Elf64_Rela *aps2_decode(const uint8_t *p, size_t sz, size_t *out_n) {
  if (sz < 4 || memcmp(p, "APS2", 4)) { *out_n = 0; return NULL; }
  p += 4;
  size_t n = (size_t)sleb(&p); uint64_t off = (uint64_t)sleb(&p);
  Elf64_Rela *v = malloc(n * sizeof *v), r = {0};
  size_t k = 0;
  while (k < n) {
    size_t gs = (size_t)sleb(&p); int64_t fl = sleb(&p), gdelta = 0;
    if (fl & 2) gdelta = sleb(&p);
    if (fl & 1) r.r_info = (uint64_t)sleb(&p);
    if ((fl & 8) && (fl & 4)) r.r_addend += sleb(&p);
    else if (!(fl & 8)) r.r_addend = 0;
    for (size_t i = 0; i < gs && k < n; i++) {
      off += (fl & 2) ? gdelta : sleb(&p);
      r.r_offset = off;
      if (!(fl & 1)) r.r_info = (uint64_t)sleb(&p);
      if ((fl & 8) && !(fl & 4)) r.r_addend += sleb(&p);
      v[k++] = r;
    }
  }
  *out_n = n; return v;
}
// TLSDESC: all module TLS lives at a fixed offset from the thread pointer (inside our fake bionic TCB)
int so_tp_slot = 0;
size_t so_tls_static_off = 0x200;   // where the first module's TLS begins, relative to tp
size_t so_tls_static_used = 0;      // bytes allocated so far
__asm__(".text\n.globl so_tlsdesc_static\nso_tlsdesc_static:\n ldr x0, [x0, #8]\n ret\n");
extern char so_tlsdesc_static[] __asm__("so_tlsdesc_static");
static char g_libdir[512];
static const char *g_skip[32]; static int g_nskip;
void so_skip(const char *name) { if (g_nskip < 32) g_skip[g_nskip++] = name; }
int so_is_skipped(const char *n) { for (int i = 0; i < g_nskip; i++) if (!strcmp(g_skip[i], n)) return 1; return 0; }
void so_set_libdir(const char *d) { snprintf(g_libdir, sizeof g_libdir, "%s", d); }
so_module *so_find(const char *name) {
  for (so_module *m = g_mods; m; m = m->next) if (!strcmp(m->name, name)) return m;
  return NULL;
}
so_module *so_load(const char *path, int *unresolved) {
  { const char *b = strrchr(path, '/'); so_module *e = so_find(b ? b + 1 : path); if (e) { if (unresolved) *unresolved = 0; return e; } }
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;
  struct stat st; fstat(fd, &st);
  uint8_t *f = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  Elf64_Ehdr *eh = (Elf64_Ehdr *)f;
  if (memcmp(eh->e_ident, ELFMAG, 4) || eh->e_machine != EM_AARCH64) return NULL;
  Elf64_Phdr *ph = (Elf64_Phdr *)(f + eh->e_phoff);
  uint64_t hi = 0;
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
  size_t sz = PUP(hi), nsite = 0;
#ifdef __APPLE__
  /* XNU owns tpidr_el0 (cpu number), so rewrite every "mrs xN, tpidr_el0" into a branch to a
     thunk that loads our per-thread TCB from Darwin TSD slot so_tp_slot (tpidrro_el0). */
  for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_X)) {
    uint32_t *c = (uint32_t *)(f + ph[i].p_offset);
    for (size_t k = 0; k < ph[i].p_filesz / 4; k++) if ((c[k] & 0xFFFFFFE0u) == 0xD53BD040u) nsite++;
  }
#endif
  size_t tsz = PUP(nsite * 16);
  int txm = 0;
  uint8_t *base = alloc_image(sz + tsz, &txm);
  if (base == MAP_FAILED) return NULL;
  so_module *m = calloc(1, sizeof *m);
  m->base = base; m->size = sz;
  const char *bn = strrchr(path, '/');
  snprintf(m->name, sizeof m->name, "%s", bn ? bn + 1 : path);
  if (txm) {
    uint8_t *mir = g_img_rw;
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD) {
      uint64_t a = PDOWN(ph[i].p_vaddr), e = PUP(ph[i].p_vaddr + ph[i].p_memsz);
      if (ph[i].p_flags & PF_X) add_mirror(base + a, e - a, mir + a);
    }
    if (tsz) add_mirror(base + sz, tsz, mir + sz);
    /* every page not covered by an exec segment becomes ordinary RW memory */
    for (size_t off = 0; off < sz; off += PG) {
      if (so_is_mirrored(base + off)) continue;
      if (mmap(base + off, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
        { fprintf(stderr, "[%s] data page remap failed at %zx\n", m->name, off); return NULL; }
    }
    fprintf(stderr, "[%s] TXM image rx %p\n", m->name, base);
  }
  Elf64_Dyn *dyn = NULL;
  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type == PT_LOAD) memcpy(so_wptr(base + ph[i].p_vaddr), f + ph[i].p_offset, ph[i].p_filesz);
    if (ph[i].p_type == PT_DYNAMIC) dyn = (Elf64_Dyn *)(base + ph[i].p_vaddr);
    if (ph[i].p_type == PT_TLS) { m->tls_init = base + ph[i].p_vaddr; m->tls_filesz = ph[i].p_filesz; m->tls_memsz = ph[i].p_memsz; }
  }
  m->dyn = dyn;
#ifdef __APPLE__
  if (nsite) {
    uint32_t *t = (uint32_t *)(base + sz); size_t n = 0;
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_X)) {
      uint32_t *c = (uint32_t *)(base + ph[i].p_vaddr);
      for (size_t k = 0; k < ph[i].p_filesz / 4; k++) if ((c[k] & 0xFFFFFFE0u) == 0xD53BD040u) {
        uint32_t r = c[k] & 31; uint32_t *th = t + n * 4, *tw = so_wptr(th); n++;
        tw[0] = 0xD53BD060u | r;                                         /* mrs  xr, tpidrro_el0 */
        tw[1] = 0x92400000u | (61u << 16) | (60u << 10) | (r << 5) | r;   /* and  xr, xr, #~7 */
        tw[2] = 0xF9400000u | ((uint32_t)so_tp_slot << 10) | (r << 5) | r; /* ldr  xr, [xr, #slot*8] */
        tw[3] = 0x14000000u | ((uint32_t)((c + k + 1) - (th + 3)) & 0x3FFFFFF); /* b back */
        *(uint32_t *)so_wptr(c + k) = 0x14000000u | ((uint32_t)(th - (c + k)) & 0x3FFFFFF);       /* b thunk */
      }
    }
    fprintf(stderr, "[%s] patched %zu tpidr_el0 reads\n", m->name, n);
  }
#endif
  Elf64_Rela *rela = NULL, *jmp = NULL; size_t relasz = 0, jmpsz = 0; const uint8_t *aps = NULL; size_t apssz = 0;
  for (Elf64_Dyn *d = dyn; d->d_tag; d++) switch (d->d_tag) {
    case DT_SYMTAB: m->dynsym = base + d->d_un.d_ptr; break;
    case DT_STRTAB: m->dynstr = (char *)base + d->d_un.d_ptr; break;
    case DT_RELA: rela = (Elf64_Rela *)(base + d->d_un.d_ptr); break;
    case DT_RELASZ: relasz = d->d_un.d_val; break;
    case DT_JMPREL: jmp = (Elf64_Rela *)(base + d->d_un.d_ptr); break;
    case 0x60000011: aps = base + d->d_un.d_ptr; break;
    case 0x60000012: apssz = d->d_un.d_val; break;
    case DT_PLTRELSZ: jmpsz = d->d_un.d_val; break;
    case DT_INIT_ARRAY: m->init_array = (void (**)(void))(base + d->d_un.d_ptr); break;
    case DT_INIT_ARRAYSZ: m->init_count = d->d_un.d_val / 8; break;
  }
  m->nsyms = count_syms(base, dyn);
  if (m->tls_memsz) { so_tls_static_used = (so_tls_static_used + 15) & ~(size_t)15; m->tls_off = so_tls_static_off + so_tls_static_used; so_tls_static_used += m->tls_memsz; }
  for (Elf64_Dyn *d = dyn; d->d_tag; d++) if (d->d_tag == DT_NEEDED && g_libdir[0]) {
    char dp[700]; snprintf(dp, sizeof dp, "%s/%s", g_libdir, m->dynstr + d->d_un.d_val);
    if (!so_find(m->dynstr + d->d_un.d_val) && !so_is_skipped(m->dynstr + d->d_un.d_val) && access(dp, R_OK) == 0) {
      int u = 0; so_load(dp, &u);
      if (getenv("SO_VERBOSE")) fprintf(stderr, "  dep %s of %s (unres %d)\n", dp, m->name, u);
    }
  }
  m->next = g_mods; g_mods = m;
  int bad = 0;
  size_t apsn = 0; Elf64_Rela *apsr = aps ? aps2_decode(aps, apssz, &apsn) : NULL;
  if (getenv("SO_MAP")) fprintf(stderr, "MAP %s %p %zx\n", m->name, m->base, m->size);
  if (aps && !apsr) fprintf(stderr, "[%s] bad APS2 table\n", m->name);
  if (getenv("SO_VERBOSE")) fprintf(stderr, "[%s] rela %zu aps2 %zu jmp %zu\n", m->name, relasz / 24, apsn, jmpsz / 24);
  { const char *dm = getenv("DUMP_RELOCS");
    if (dm && !strcmp(dm, m->name) && apsr) {
      FILE *o = fopen(getenv("DUMP_OUT") ? getenv("DUMP_OUT") : "/dev/stdout", "w");
      for (size_t i = 0; i < apsn; i++) fprintf(o, "%llx %u %u %llx\n", (unsigned long long)apsr[i].r_offset,
        (unsigned)ELF64_R_TYPE(apsr[i].r_info), (unsigned)ELF64_R_SYM(apsr[i].r_info), (unsigned long long)apsr[i].r_addend);
      fclose(o);
    } }
  Elf64_Rela *tabs[3] = {rela, apsr, jmp};
  size_t cnt[3] = {relasz / sizeof(Elf64_Rela), apsn, jmpsz / sizeof(Elf64_Rela)};
  Elf64_Sym *syms = m->dynsym;
  for (int t = 0; t < 3; t++) for (size_t i = 0; tabs[t] && i < cnt[t]; i++) {
    Elf64_Rela *r = &tabs[t][i];
    uint64_t *where = (uint64_t *)so_wptr(base + r->r_offset);
    uint32_t type = ELF64_R_TYPE(r->r_info), si = ELF64_R_SYM(r->r_info);
    void *val = NULL;
    if (si) {
      Elf64_Sym *s = &syms[si];
      const char *nm = m->dynstr + s->st_name;
      val = resolve(nm);  // honors interposition (global lookup), like bionic
      if (!val && s->st_shndx != SHN_UNDEF) val = base + s->st_value;
      if (!val && ELF64_ST_BIND(s->st_info) != STB_WEAK) {
        bad++;
        if (getenv("SO_VERBOSE")) fprintf(stderr, "[%s] unresolved %s\n", m->name, nm);
      }
    }
    switch (type) {
      case RA_RELATIVE: *where = (uint64_t)base + r->r_addend; break;
      case RA_ABS64: case RA_GLOB_DAT: case RA_JUMP_SLOT: *where = (uint64_t)val + r->r_addend; break;
      case 1031: { /* TLSDESC */
        uint64_t symoff = si ? syms[si].st_value : 0;
        where[0] = (uint64_t)so_tlsdesc_static; where[1] = m->tls_off + symoff + r->r_addend; break; }
      case 1030: /* TPREL64 */ *where = m->tls_off + (si ? syms[si].st_value : 0) + r->r_addend; break;
      default: fprintf(stderr, "[%s] reloc type %u unsupported\n", m->name, type); bad++;
    }
  }
  for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD) {
    int pr = (ph[i].p_flags & PF_R ? PROT_READ : 0) | (ph[i].p_flags & PF_W ? PROT_WRITE : 0) |
             (ph[i].p_flags & PF_X ? PROT_EXEC : 0);
    uint64_t a = PDOWN(ph[i].p_vaddr), e = PUP(ph[i].p_vaddr + ph[i].p_memsz);
    if (pr & PROT_EXEC) pr &= ~PROT_WRITE;
    if ((pr & PROT_EXEC) && txm) continue;
    if (mprotect(base + a, e - a, pr)) fprintf(stderr, "[%s] mprotect %llx %d failed\n", m->name, (unsigned long long)a, pr);
  }
  if (tsz && !txm) mprotect(base + sz, tsz, PROT_READ | PROT_EXEC);
#ifdef __APPLE__
  if (txm) for (int i = 0; i < g_nmir; i++) sys_icache_invalidate(g_mir[i].rx, g_mir[i].len);
#endif
  munmap(f, st.st_size);
  if (unresolved) *unresolved = bad;
  return m;
}

void so_init(so_module *m) {
  if (m->inited) return;
  m->inited = 1;
  Elf64_Dyn *dyn = m->dyn;
  for (Elf64_Dyn *d = dyn; d->d_tag; d++) if (d->d_tag == DT_NEEDED) { so_module *dm = so_find(m->dynstr + d->d_un.d_val); if (dm) so_init(dm); }
  if (getenv("SO_VERBOSE")) fprintf(stderr, "init %s (%zu ctors)\n", m->name, m->init_count);
  for (size_t i = 0; i < m->init_count; i++)
    if (m->init_array[i] && (intptr_t)m->init_array[i] != -1) {
      if (getenv("SO_VERBOSE")) fprintf(stderr, "  ctor %s+0x%lx\n", m->name, (unsigned long)((char *)m->init_array[i] - (char *)m->base));
      m->init_array[i]();
    }
}

void so_tls_init_block(uint8_t *tp) {
  for (so_module *m = g_mods; m; m = m->next) if (m->tls_memsz) {
    memset(tp + m->tls_off, 0, m->tls_memsz);
    memcpy(tp + m->tls_off, m->tls_init, m->tls_filesz);
  }
}
void so_init_all(void) {
  so_module *v[256]; int n = 0;
  for (so_module *m = g_mods; m && n < 256; m = m->next) v[n++] = m;
  while (n--) so_init(v[n]);  /* load order */
}

#ifndef __APPLE__
#include <link.h>
#endif
// report loaded ELF modules (bionic dl_phdr_info layout == glibc on aarch64 for first 4 fields)
int so_iterate_phdr(int (*cb_)(void *, size_t, void *), void *data) {
  int (*cb)(struct dl_phdr_info *, size_t, void *) = (void *)cb_;
  for (so_module *m = g_mods; m; m = m->next) {
    Elf64_Ehdr *eh = (Elf64_Ehdr *)m->base;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG)) continue;
    struct dl_phdr_info i = {0};
    i.dlpi_addr = (Elf64_Addr)m->base; i.dlpi_name = m->name;
    i.dlpi_phdr = (const Elf64_Phdr *)(m->base + eh->e_phoff); i.dlpi_phnum = eh->e_phnum;
    int r = cb(&i, offsetof(struct dl_phdr_info, dlpi_adds), data);
    if (r) return r;
  }
#ifdef __APPLE__
  return 0;
#else
  return dl_iterate_phdr(cb, data);
#endif
}

void so_describe(const char *tag, void *a) {
  a = (void *)((uintptr_t)a & 0x0000000fffffffffULL);  /* strip PAC */
  for (so_module *m = g_mods; m; m = m->next)
    if ((uint8_t *)a >= (uint8_t *)m->base && (uint8_t *)a < (uint8_t *)m->base + m->size) {
      fprintf(stderr, "%s %p = %s+0x%lx\n", tag, a, m->name, (unsigned long)((uint8_t *)a - (uint8_t *)m->base)); return; }
#ifdef __APPLE__
  { Dl_info di; if (dladdr(a, &di) && di.dli_fname) {
      const char *b = strrchr(di.dli_fname, '/'); b = b ? b + 1 : di.dli_fname;
      fprintf(stderr, "%s %p = %s!%s+0x%lx\n", tag, a, b, di.dli_sname ? di.dli_sname : "?",
              (unsigned long)((uint8_t *)a - (uint8_t *)(di.dli_saddr ? di.dli_saddr : di.dli_fbase))); return; } }
#endif
  fprintf(stderr, "%s %p = ?\n", tag, a);
}
