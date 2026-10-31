// iOS entry: content lives in Documents (user copies garrysmod/ sourceengine/ platform/ via Files).
// The app bundle only carries the Android .so code (lib/) and runtime assets (rt/).
#include "../loader/elf_loader.h"
#include "../shim/bionic.h"
#include "../shim/jni_fake.h"
#include "../shim/platform.h"
#include <SDL.h>
#include <SDL_main.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char *ios_bundle_path(void);
char *ios_documents_path(void);

static char g_bundle[1024], g_docs[1024];
static void *g_gles, *g_egl;
static void *gl_proc(const char *n) {
  char p[1200];
  if (!g_gles) { snprintf(p, sizeof p, "%s/Frameworks/libGLESv2.framework/libGLESv2", g_bundle); g_gles = dlopen(p, RTLD_NOW | RTLD_GLOBAL); }
  if (!g_egl) { snprintf(p, sizeof p, "%s/Frameworks/libEGL.framework/libEGL", g_bundle); g_egl = dlopen(p, RTLD_NOW | RTLD_GLOBAL); }
  void *r = NULL;
  if (g_gles) r = dlsym(g_gles, n);
  if (!r && g_egl) r = dlsym(g_egl, n);
  return r;
}
void *ios_gl_proc(const char *n) { return gl_proc(n); }
void *glctx_resolve(const char *n);
static void *host(const char *n) {
  { void *g = glctx_resolve(n); if (g) return g; }
  void *p = platform_resolve(n);
  if (!p && !strncmp(n, "SDL_", 4)) p = dlsym(RTLD_DEFAULT, n);
  if (!p && (!strncmp(n, "gl", 2) || !strncmp(n, "egl", 3))) p = gl_proc(n);
  if (!p) p = dlsym(RTLD_DEFAULT, n);
  return p;
}
static long stub0(void) { return 0; }
static void *late(const char *n) { fprintf(stderr, "stub %s\n", n); return (void *)stub0; }

static int exists(const char *a, const char *b) {
  char p[1200]; struct stat st; snprintf(p, sizeof p, "%s/%s", a, b); return stat(p, &st) == 0;
}

extern int csops(int pid, unsigned int ops, void *useraddr, size_t usersize);
static int jit_enabled(void) { unsigned int f = 0; csops(getpid(), 0, &f, sizeof f); return (f & 0x10000000) != 0; }
static void wait_for_jit(void) {
  while (!jit_enabled()) {
    const SDL_MessageBoxButtonData btn[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Retry"}, {0, 0, "Quit"}};
    SDL_MessageBoxData d = {SDL_MESSAGEBOX_WARNING, NULL, "GMod", "JIT is not enabled.\n\nLaunch with JIT: TrollStore -> long-press GMod -> Open with JIT, or use StikDebug.\nThen tap Retry.", 2, btn, NULL};
    int b = 0; SDL_ShowMessageBox(&d, &b); if (b == 0) exit(0);
  }
}

static void wait_for_content(void) {
  wait_for_jit();
  while (!exists(g_docs, "garrysmod/garrysmod_dir.vpk") && !exists(g_docs, "garrysmod/gameinfo.txt")) {
    char msg[2048];
    snprintf(msg, sizeof msg,
             "Game files not found.\n\nOpen the Files app -> On My iPhone -> this app, and copy these folders from your "
             "Garry's Mod install:\n  garrysmod\n  sourceengine\n  platform\n\nThen tap Retry.\n\n(%s)", g_docs);
    const SDL_MessageBoxButtonData btn[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Retry"}};
    SDL_MessageBoxData d = {SDL_MESSAGEBOX_INFORMATION, NULL, "GMod", msg, 1, btn, NULL};
    int b; SDL_ShowMessageBox(&d, &b);
  }
}

#include <signal.h>
#include <sys/ucontext.h>
void so_describe(const char *tag, void *a);
static void on_crash(int sig, siginfo_t *si, void *uc_) {
  ucontext_t *uc = uc_;
  fprintf(stderr, "\n*** signal %d addr %p\n", sig, si->si_addr);
  so_describe("pc", (void *)uc->uc_mcontext->__ss.__pc);
  so_describe("lr", (void *)uc->uc_mcontext->__ss.__lr);
  uint64_t *fp = (uint64_t *)uc->uc_mcontext->__ss.__fp;
  for (int i = 0; i < 24 && fp && ((uintptr_t)fp & 7) == 0; i++) { so_describe("bt", (void *)fp[1]); fp = (uint64_t *)fp[0]; }
  signal(sig, SIG_DFL); raise(sig);
}

int main(int argc, char **argv) {
  snprintf(g_bundle, sizeof g_bundle, "%s", ios_bundle_path());
  snprintf(g_docs, sizeof g_docs, "%s", ios_documents_path());
  char lib[1200], wr[1200], rt[1200], lnk[1200], log[1200];
  snprintf(log, sizeof log, "%s/gmod_log.txt", g_docs);
  freopen(log, "w", stderr); setvbuf(stderr, NULL, _IONBF, 0);
  setenv("SO_MAP", "1", 1); setenv("SO_VERBOSE", "1", 1);
  { struct sigaction sa = {0}; sa.sa_sigaction = on_crash; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    static char alt[65536]; stack_t ss = {alt, 0, sizeof alt}; sigaltstack(&ss, NULL);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); }
  SDL_Init(SDL_INIT_VIDEO);  // needed for message box; engine re-inits
  wait_for_content();

  snprintf(lib, sizeof lib, "%s/lib", g_bundle);
  snprintf(rt, sizeof rt, "%s/rt", g_bundle);
  snprintf(wr, sizeof wr, "%s/write", g_docs); mkdir(wr, 0755);
  snprintf(lnk, sizeof lnk, "%s/garrysmod/runtime", g_docs);
  unlink(lnk); symlink(rt, lnk);  // bundle path changes per install
  setenv("APP_LIB_PATH", lib, 1);
  setenv("APP_DATA_PATH", g_docs, 1);
  setenv("VALVE_GAME_PATH", g_docs, 1);
  setenv("SOURCEVR_SHARED_CONTENT_PATH", g_docs, 1);
  setenv("SOURCEVR_WRITE_GAME_PATH", wr, 1);
  setenv("SOURCEVR_GAME", "garrysmod", 1);
  bionic_set_root(g_docs);
  setenv("SO_MAP", "1", 1); setenv("SO_VERBOSE", "1", 1);

  platform_init();
  bionic_gl_proc = gl_proc;
  so_set_libdir(lib);
  so_skip("libSDL2.so");
  so_skip("libopenxr_loader.so");  // VR only; its ctor crashes on iOS
  bionic_init(host);
  so_set_late_resolver(late);
  so_set_resolver(bionic_resolve);
  char p[1300]; snprintf(p, sizeof p, "%s/liblauncher.so", lib);
  int u = 0; so_module *m = so_load(p, &u);
  if (!m) { fprintf(stderr, "load failed\n"); return 1; }
  fprintf(stderr, "loaded, unresolved=%d\n", u);
  bionic_thread_enter();
  so_init_all();
  int (*jni_onload)(void *, void *) = so_sym(m, "JNI_OnLoad");
  if (jni_onload) jni_onload(jni_vm(), NULL);
  int (*lma)(int, char **) = so_sym(m, "LauncherMainAndroid");
  char *av[] = {"gmod", "-game", "garrysmod", NULL};
  int r = lma(3, av);
  fprintf(stderr, "LauncherMainAndroid returned %d\n", r);
  return r;
}
