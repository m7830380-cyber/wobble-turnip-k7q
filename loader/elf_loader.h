#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct so_module {
  struct so_module *next;
  char name[128];
  uint8_t *base; size_t size;
  void *dyn, *dynsym; char *dynstr; size_t nsyms;
  void (**init_array)(void); size_t init_count;
  void *tls_init; size_t tls_filesz, tls_memsz, tls_off; int inited;
} so_module;
typedef void *(*so_resolver)(const char *name);
void so_set_resolver(so_resolver r);
void so_set_late_resolver(so_resolver r); // after module search (stubs)
so_module *so_load(const char *path, int *unresolved);
void so_set_libdir(const char *d);
so_module *so_find(const char *name);
void so_init(so_module *m);
void so_init_all(void);
void *so_sym(so_module *m, const char *name);
extern int so_tp_slot;
extern size_t so_tls_static_off, so_tls_static_used;
// copy every module's TLS image into a new thread block (tp = block base)
void so_tls_init_block(uint8_t *tp);
void so_skip(const char *name);  // never load this DT_NEEDED (symbols come from resolver: native replacement)
int so_is_skipped(const char *n);
int so_iterate_phdr(int (*cb)(void *, size_t, void *), void *data);
void so_jit_reserve_dir(const char *dir); // TXM: prepare all exec pages up front
