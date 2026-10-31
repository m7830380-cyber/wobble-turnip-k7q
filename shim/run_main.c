// Host runner: load the launcher with native SDL2, fake JNI, then call LauncherMainAndroid.
#define _GNU_SOURCE
#include "../loader/elf_loader.h"
#include "bionic.h"
#include "jni_fake.h"
#include "platform.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *g_sdl;
static void *gl_proc(const char *n);
static void *host(const char *n) {
  void *p = platform_resolve(n);
  if (!p && g_sdl && !strncmp(n, "SDL_", 4)) p = dlsym(g_sdl, n);
  if (!p && (!strncmp(n, "gl", 2) || !strncmp(n, "egl", 3))) p = gl_proc(n);
  if (!p) p = dlsym(RTLD_DEFAULT, n);
  return p;
}
static void *g_gles, *g_egl;
static void *gl_proc(const char *n) {
  void *p = NULL;
  if (!g_gles) g_gles = dlopen(getenv("GLES_LIB") ? getenv("GLES_LIB") : "libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
  if (!g_egl) g_egl = dlopen(getenv("EGL_LIB") ? getenv("EGL_LIB") : "libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
  if (g_gles) p = dlsym(g_gles, n);
  if (!p && g_egl) p = dlsym(g_egl, n);
  if (!p && g_sdl) { void *(*gp)(const char *) = dlsym(g_sdl, "SDL_GL_GetProcAddress"); if (gp) p = gp(n); }
  if (!p && getenv("GL_VERBOSE")) fprintf(stderr, "gl missing %s\n", n);
  return p;
}
static long stub0(void) { return 0; }
static void *late(const char *n) {
  if (getenv("SO_VERBOSE")) fprintf(stderr, "stub %s\n", n);
  return (void *)stub0;
}
int main(int argc, char **argv) {
  const char *L = getenv("LIBDIR") ? getenv("LIBDIR") : "/tmp/apk/lib/arm64-v8a";
  setenv("APP_LIB_PATH", L, 0);
  g_sdl = dlopen(getenv("SDL_LIB") ? getenv("SDL_LIB") : "libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
  if (!g_sdl) fprintf(stderr, "warning: native SDL2 not found: %s\n", dlerror());
  platform_init();
  bionic_gl_proc = gl_proc;
  so_set_libdir(L);
  so_skip("libSDL2.so");
  bionic_init(host);
  so_set_late_resolver(late);
  so_set_resolver(bionic_resolve);
  char p[1024]; snprintf(p, sizeof p, "%s/liblauncher.so", L);
  int u = 0; so_module *m = so_load(p, &u);
  if (!m) { fprintf(stderr, "load failed\n"); return 1; }
  bionic_thread_enter();
  so_init_all();
  int (*jni_onload)(void *, void *) = so_sym(m, "JNI_OnLoad");
  if (jni_onload) printf("JNI_OnLoad -> %x\n", jni_onload(jni_vm(), NULL));
  int (*lma)(int, char **) = so_sym(m, "LauncherMainAndroid");
  printf("calling LauncherMainAndroid\n"); fflush(stdout);
  char *av[64]; int ac = 0; av[ac++] = "gmod";
  for (int i = 1; i < argc && ac < 63; i++) av[ac++] = argv[i];
  av[ac] = NULL;
  int r = lma(ac, av);
  printf("LauncherMainAndroid returned %d\n", r);
  return r;
}
