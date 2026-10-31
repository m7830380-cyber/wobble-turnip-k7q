// Host run test: loader + bionic shim, run constructors, then call a few tier0 exports.
#define _GNU_SOURCE
#include "../loader/elf_loader.h"
#include "bionic.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void *host(const char *n) { return dlsym(RTLD_DEFAULT, n); }
static void stub(void) {}
static void *late(const char *n) {
  void *p = NULL;
  if (getenv("STUB_MISSING")) { if (getenv("SO_VERBOSE")) fprintf(stderr, "stub %s\n", n); return (void *)stub; }
  return p;
}
int main(int c, char **v) {
  const char *L = getenv("LIBDIR") ? getenv("LIBDIR") : "/tmp/apk/lib/arm64-v8a";
  setenv("APP_LIB_PATH", L, 1);
  so_set_libdir(L);
  bionic_init(host);
  so_set_late_resolver(late);
  so_set_resolver(bionic_resolve);
  for (int i = 1; i < c; i++) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", L, v[i]);
    int u = 0; so_module *m = so_load(p, &u);
    printf("load %s -> %s unresolved=%d\n", v[i], m ? "OK" : "FAIL", u);
  }
  bionic_thread_enter();  // TLS block now includes all loaded modules
  so_init_all();
  printf("constructors done\n");
  so_module *t0 = so_find("libtier0.so");
  if (t0) {
    double (*ft)(void) = so_sym(t0, "Plat_FloatTime");
    void (*msg)(const char *, ...) = so_sym(t0, "Msg");
    if (ft) printf("Plat_FloatTime = %f\n", ft());
    if (msg) msg("hello from tier0 Msg %d\n", 42);
  }
  fflush(stdout);
  return 0;
}
