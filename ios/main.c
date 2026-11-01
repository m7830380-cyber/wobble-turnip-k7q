#include <sys/mman.h>
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
static int lib_private(const char *n) {
  // libraries statically linked into the .so files (FreeType etc.). Must bind to the module's own copy,
  // never to a same-named symbol that happens to exist in an iOS framework (different version/ABI -> hangs)
  static const char *pre[] = {"FT_", "FTC_", "TT_", "T1_", "ft_", "tt_", "af_", "ps_", "cff_", "hb_", "png_", "jpeg_", "ogg_", "vorbis_", "opus_", "speex_", "mpg123_", "lua_", "luaL_", "luaJIT_", "ZSTD_", "LZ4_", "XXH", 0};
  for (int i = 0; pre[i]; i++) if (!strncmp(n, pre[i], strlen(pre[i]))) return 1;
  return 0;
}
static int g_msgboxes;
static int my_ShowSimpleMessageBox(unsigned flags, const char *title, const char *msg, void *win) {
  (void)win; fprintf(stderr, "\n*** MESSAGEBOX [%u] %s: %s\n", flags, title ? title : "", msg ? msg : "");
  if (++g_msgboxes > 3) return 0;
  return SDL_ShowSimpleMessageBox(flags, title, msg, NULL);
}
static int not_debugging(void) { return 0; }
/* LuaJIT machine code crashes on iOS (debugger freezes the faulting thread) -> interpreter only */
static void *lua_real(const char *n) { so_module *m = so_find("liblua_shared.so"); return m ? so_sym(m, n) : NULL; }
static void lua_patch_once(void) {
  static int done; if (done) return; done = 1;
  so_module *m = so_find("liblua_shared.so"); if (!m) { fprintf(stderr, "[lua] patch: module not found\n"); return; }
  uint64_t *c = (uint64_t *)(m->base + 0x26cde0); uintptr_t pg = (uintptr_t)c & ~(uintptr_t)16383;
  int r = mprotect((void *)pg, 16384, PROT_READ | PROT_WRITE);
  fprintf(stderr, "[lua] ctr cache was %llx (mprotect %d)\n", (unsigned long long)*c, r); *c = 0x8444c004ULL;
}
static void *my_luaL_newstate(void) { lua_patch_once();
  void *(*f)(void) = lua_real("luaL_newstate"); void *L = f(); 
  int (*sm)(void *, int, int) = lua_real("luaJIT_setmode"); if (L && sm) fprintf(stderr, "[lua] JIT off %p: %d\n", L, sm(L, 0, 0));
  return L; }
static void *my_lua_newstate(void *a, void *ud) { lua_patch_once();
  void *(*f)(void *, void *) = lua_real("lua_newstate"); void *L = f(a, ud);
  int (*sm)(void *, int, int) = lua_real("luaJIT_setmode"); if (L && sm) fprintf(stderr, "[lua] JIT off %p: %d\n", L, sm(L, 0, 0));
  return L; }
static void *host(const char *n) {
  if (!strcmp(n, "Plat_IsInDebugSession")) return (void *)not_debugging;
  if (!strcmp(n, "SDL_ShowSimpleMessageBox")) return (void *)my_ShowSimpleMessageBox;
  if (!strcmp(n, "luaL_newstate") && lua_real(n)) return (void *)my_luaL_newstate;
  if (!strcmp(n, "lua_newstate") && lua_real(n)) return (void *)my_lua_newstate;
  if (lib_private(n)) return NULL;
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


#include <mach/mach.h>
#include <pthread.h>
static char g_logpath[1200];
static void dump_threads(void) {
  thread_act_array_t th; mach_msg_type_number_t n; thread_t self = mach_thread_self();
  if (task_threads(mach_task_self(), &th, &n)) return;
  fprintf(stderr, "\n=== WATCHDOG: log idle, %u threads ===\n", n);
  for (unsigned i = 0; i < n; i++) {
    if (th[i] == self) continue;
    thread_suspend(th[i]);
    arm_thread_state64_t st; mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT;
    if (!thread_get_state(th[i], ARM_THREAD_STATE64, (thread_state_t)&st, &c)) {
      fprintf(stderr, "--- thread %u\n", i);
      so_describe("pc", (void *)arm_thread_state64_get_pc(st));
      so_describe("lr", (void *)arm_thread_state64_get_lr(st));
      uint64_t *fp = (uint64_t *)arm_thread_state64_get_fp(st);
      for (int k = 0; k < 40 && fp && ((uintptr_t)fp & 7) == 0; k++) { so_describe("bt", (void *)fp[1]); fp = (uint64_t *)fp[0]; }
    }
    thread_resume(th[i]);
  }
}
static void *watchdog(void *a) {
  off_t last = -1; int idle = 0, dumps = 0;
  for (;;) {
    sleep(1); struct stat st; if (stat(g_logpath, &st)) continue;
    if (st.st_size != last) { last = st.st_size; idle = 0; continue; }
    if (++idle == 8 && dumps < 2) { dumps++; dump_threads(); idle = -20; }
  }
  return a;
}

int main(int argc, char **argv) {
  snprintf(g_bundle, sizeof g_bundle, "%s", ios_bundle_path());
  snprintf(g_docs, sizeof g_docs, "%s", ios_documents_path());
  char lib[1200], wr[1200], rt[1200], lnk[1200], log[1200];
  snprintf(log, sizeof log, "%s/gmod_log.txt", g_docs);
  freopen(log, "w", stderr); setvbuf(stderr, NULL, _IONBF, 0);
  snprintf(g_logpath, sizeof g_logpath, "%s", log);
  { pthread_t t; pthread_create(&t, NULL, watchdog, NULL); pthread_detach(t); }
  setenv("SO_MAP", "1", 1); setenv("SO_VERBOSE", "1", 1); setenv("JNI_VERBOSE", "1", 1); setenv("JNI_VERBOSE", "1", 1);
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
  { // Windows GMod has no linux_fonts; engine (FreeType) needs them under platform/resource/linux_fonts
    const char *fn[] = {"dejavusans.ttf", "dejavusans-bold.ttf", "dejavusans-oblique.ttf", "dejavusans-boldoblique.ttf", "liberationmono-regular.ttf"};
    char d[1300], a[1400], b[1400]; snprintf(d, sizeof d, "%s/platform/resource/linux_fonts", g_docs); mkdir(d, 0755);
    for (int i = 0; i < 5; i++) {
      snprintf(a, sizeof a, "%s/platform/resource/linux_fonts/%s", rt, fn[i]); snprintf(b, sizeof b, "%s/%s", d, fn[i]);
      struct stat st; if (!stat(b, &st) && st.st_size > 0) continue;
      FILE *in = fopen(a, "rb"), *out = in ? fopen(b, "wb") : NULL; char buf[65536]; size_t n;
      if (in && out) while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
      if (in) fclose(in); if (out) fclose(out);
      fprintf(stderr, "font %s -> %s\n", fn[i], out ? "copied" : "FAILED");
    }
  }
  setenv("APP_LIB_PATH", lib, 1);
  setenv("APP_DATA_PATH", g_docs, 1);
  setenv("VALVE_GAME_PATH", g_docs, 1);
  setenv("SOURCEVR_SHARED_CONTENT_PATH", g_docs, 1);
  setenv("SOURCEVR_WRITE_GAME_PATH", wr, 1);
  setenv("SOURCEVR_GAME", "garrysmod", 1);
  bionic_set_root(g_docs);
  setenv("SO_MAP", "1", 1); setenv("SO_VERBOSE", "1", 1);

  platform_init();
  { extern void webview_init(void); webview_init(); }
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
  { /* liblua's own internal PLT calls bypass the resolver: patch its GOT slots (APK 0.1.30 offsets) */
    so_module *m = so_find("liblua_shared.so");
    if (m) {
      /* __clear_cache reads CTR_EL0 (mrs) unless cached at .bss+0x26cde0; mrs ctr_el0 traps on iOS -> pre-fill */
      { uint64_t *c = (uint64_t *)(m->base + 0x26cde0); uintptr_t pg = (uintptr_t)c & ~(uintptr_t)16383;
        mprotect((void *)pg, 16384, PROT_READ | PROT_WRITE);
        fprintf(stderr, "[lua] ctr cache was %llx\n", (unsigned long long)*c); *c = 0x8444c004ULL; }
      struct { size_t off; const char *n; void *hook; } g[] = {
        {0x253e28, "luaL_newstate", (void *)my_luaL_newstate}, {0x253b20, "lua_newstate", (void *)my_lua_newstate}};
      for (int i = 0; i < 2; i++) {
        void **slot = (void **)(m->base + g[i].off); void *real = so_sym(m, g[i].n);
        uintptr_t pg = (uintptr_t)slot & ~(uintptr_t)16383;
        mprotect((void *)pg, 16384, PROT_READ | PROT_WRITE);
        fprintf(stderr, "[lua] GOT %s %p real %p -> %s\n", g[i].n, *slot, real, *slot == real ? "hooked" : "MISMATCH");
        if (*slot == real) *slot = g[i].hook;
      }
    }
  }
  so_init_all();
  int (*jni_onload)(void *, void *) = so_sym(m, "JNI_OnLoad");
  if (jni_onload) jni_onload(jni_vm(), NULL);
  int (*lma)(int, char **) = so_sym(m, "LauncherMainAndroid");
  char *av[] = {"gmod", "-game", "garrysmod", NULL};
  int r = lma(3, av);
  fprintf(stderr, "LauncherMainAndroid returned %d\n", r);
  return r;
}
