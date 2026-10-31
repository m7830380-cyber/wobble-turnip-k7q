// Minimal arm64 ELF shared-object loader: Android bionic .so -> iOS (or Linux host for testing)
#include "elf_loader.h"
#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define RA_ABS64 257
#define RA_GLOB_DAT 1025
#define RA_JUMP_SLOT 1026
#define RA_RELATIVE 1027
#define PG 0x4000UL
#define PDOWN(x) ((x) & ~(PG - 1))
#define PUP(x) PDOWN((x) + PG - 1)

static so_module *g_mods;
static so_resolver g_resolver;
void so_set_resolver(so_resolver r) { g_resolver = r; }

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
  return NULL;
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

static char g_libdir[512];
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
  size_t sz = PUP(hi);
  uint8_t *base = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) return NULL;
  so_module *m = calloc(1, sizeof *m);
  m->base = base; m->size = sz;
  const char *bn = strrchr(path, '/');
  snprintf(m->name, sizeof m->name, "%s", bn ? bn + 1 : path);
  Elf64_Dyn *dyn = NULL;
  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type == PT_LOAD) memcpy(base + ph[i].p_vaddr, f + ph[i].p_offset, ph[i].p_filesz);
    if (ph[i].p_type == PT_DYNAMIC) dyn = (Elf64_Dyn *)(base + ph[i].p_vaddr);
    if (ph[i].p_type == PT_TLS) { m->tls_init = base + ph[i].p_vaddr; m->tls_filesz = ph[i].p_filesz; m->tls_memsz = ph[i].p_memsz; }
  }
  m->dyn = dyn;
  Elf64_Rela *rela = NULL, *jmp = NULL; size_t relasz = 0, jmpsz = 0;
  for (Elf64_Dyn *d = dyn; d->d_tag; d++) switch (d->d_tag) {
    case DT_SYMTAB: m->dynsym = base + d->d_un.d_ptr; break;
    case DT_STRTAB: m->dynstr = (char *)base + d->d_un.d_ptr; break;
    case DT_RELA: rela = (Elf64_Rela *)(base + d->d_un.d_ptr); break;
    case DT_RELASZ: relasz = d->d_un.d_val; break;
    case DT_JMPREL: jmp = (Elf64_Rela *)(base + d->d_un.d_ptr); break;
    case DT_PLTRELSZ: jmpsz = d->d_un.d_val; break;
    case DT_INIT_ARRAY: m->init_array = (void (**)(void))(base + d->d_un.d_ptr); break;
    case DT_INIT_ARRAYSZ: m->init_count = d->d_un.d_val / 8; break;
  }
  m->nsyms = count_syms(base, dyn);
  for (Elf64_Dyn *d = dyn; d->d_tag; d++) if (d->d_tag == DT_NEEDED && g_libdir[0]) {
    char dp[700]; snprintf(dp, sizeof dp, "%s/%s", g_libdir, m->dynstr + d->d_un.d_val);
    if (!so_find(m->dynstr + d->d_un.d_val) && access(dp, R_OK) == 0) {
      int u = 0; so_load(dp, &u);
      if (getenv("SO_VERBOSE")) fprintf(stderr, "  dep %s of %s (unres %d)\n", dp, m->name, u);
    }
  }
  m->next = g_mods; g_mods = m;
  int bad = 0;
  Elf64_Rela *tabs[2] = {rela, jmp};
  size_t cnt[2] = {relasz / sizeof(Elf64_Rela), jmpsz / sizeof(Elf64_Rela)};
  Elf64_Sym *syms = m->dynsym;
  for (int t = 0; t < 2; t++) for (size_t i = 0; tabs[t] && i < cnt[t]; i++) {
    Elf64_Rela *r = &tabs[t][i];
    uint64_t *where = (uint64_t *)(base + r->r_offset);
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
      default: fprintf(stderr, "[%s] reloc type %u unsupported\n", m->name, type); bad++;
    }
  }
  for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD) {
    int pr = (ph[i].p_flags & PF_R ? PROT_READ : 0) | (ph[i].p_flags & PF_W ? PROT_WRITE : 0) |
             (ph[i].p_flags & PF_X ? PROT_EXEC : 0);
    uint64_t a = PDOWN(ph[i].p_vaddr), e = PUP(ph[i].p_vaddr + ph[i].p_memsz);
    if (pr & PROT_EXEC) pr &= ~PROT_WRITE;
    mprotect(base + a, e - a, pr);
  }
  munmap(f, st.st_size);
  if (unresolved) *unresolved = bad;
  return m;
}

void so_init(so_module *m) {
  for (size_t i = 0; i < m->init_count; i++)
    if (m->init_array[i] && (intptr_t)m->init_array[i] != -1) m->init_array[i]();
}
