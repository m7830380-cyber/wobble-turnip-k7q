// Host test: load GDroid libs, resolve against host libc, report unresolved.
// DUMP_RELOCS=lib.so -> print decoded APS2 relocs of that lib (offset type sym addend) for diffing vs llvm-readelf
#define _GNU_SOURCE
#include "elf_loader.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void *host(const char *n) { return dlsym(RTLD_DEFAULT, n); }
int main(int c, char **v) {
  so_set_resolver(host);
  so_set_libdir(getenv("LIBDIR") ? getenv("LIBDIR") : "/tmp/apk/lib/arm64-v8a");
  int total = 0;
  for (int i = 1; i < c; i++) {
    int u = 0; so_module *m = so_load(v[i], &u);
    const char *b = strrchr(v[i], '/');
    printf("%-30s %s base=%p unresolved=%d\n", b ? b + 1 : v[i], m ? "OK" : "FAIL", m ? (void *)m->base : 0, u);
    total += u;
  }
  printf("total unresolved %d\n", total);
}
