// Host test: load GDroid libs, resolve against host libc, report unresolved
#define _GNU_SOURCE
#include "elf_loader.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
static void *host(const char *n) { return dlsym(RTLD_DEFAULT, n); }
int main(int c, char **v) {
  so_set_resolver(host); so_set_libdir("/tmp/apk/lib/arm64-v8a");
  int total = 0;
  for (int i = 1; i < c; i++) {
    int u = 0; so_module *m = so_load(v[i], &u);
    printf("%-30s %s base=%p unresolved=%d\n", strrchr(v[i], '/') + 1, m ? "OK" : "FAIL", m ? (void *)m->base : 0, u);
    total += u;
  }
  printf("total unresolved %d\n", total);
}
