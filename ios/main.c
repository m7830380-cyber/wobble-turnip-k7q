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
static int g_sdiag = -1; /* starts after first blit */
#define SD(fmt, ...) do { if (g_sdiag >= 0 && g_sdiag < 120) { g_sdiag++; fprintf(stderr, "STDIAG " fmt "\n", __VA_ARGS__); } } while (0)
static void (*real_blit)(int,int,int,int,int,int,int,int,unsigned,unsigned);
static void fix_blit(int a,int b,int c,int d,int e,int f,int g,int h,unsigned mask,unsigned filt) {
  static int n, calls; unsigned o = filt;
  if (calls++ < 3) fprintf(stderr, "BLITCALL filter 0x%x mask 0x%x\n", filt, mask);
  if (filt != 0x2600 && filt != 0x2601) filt = (filt == 2) ? 0x2601 : 0x2600; /* D3DTEXF: 1=POINT 2=LINEAR */
  if (mask & 0x500) filt = 0x2600;
  if (o != filt && n++ < 10) fprintf(stderr, "BLITFIX filter 0x%x -> 0x%x mask 0x%x %dx%d->%dx%d\n", o, filt, mask, c-a, d-b, g-e, h-f);
  if (g_sdiag < 0) g_sdiag = 0;
  SD("blit %dx%d->%dx%d", c-a, d-b, g-e, h-f);
  real_blit(a,b,c,d,e,f,g,h,mask,filt);
}
void *ios_gl_proc(const char *n);
static void (*r_enable)(unsigned), (*r_disable)(unsigned), (*r_sfunc)(unsigned,int,unsigned), (*r_sop)(unsigned,unsigned,unsigned), (*r_smask)(unsigned), (*r_clear)(unsigned), (*r_cmask)(unsigned char,unsigned char,unsigned char,unsigned char);
static void w_enable(unsigned c) { if (c == 0xB90) SD("enable stencil%s", ""); r_enable(c); }
static void w_disable(unsigned c) { if (c == 0xB90) SD("disable stencil%s", ""); r_disable(c); }
static void w_sfunc(unsigned f, int r, unsigned m) { SD("func 0x%x ref %d mask 0x%x", f, r, m); r_sfunc(f, r, m); }
static void w_sop(unsigned a, unsigned b, unsigned c) { SD("op 0x%x 0x%x 0x%x", a, b, c); r_sop(a, b, c); }
static void w_smask(unsigned m) { SD("wmask 0x%x", m); r_smask(m); }
static void w_clear(unsigned m) { SD("clear 0x%x", m); r_clear(m); }
static void w_cmask(unsigned char a, unsigned char b, unsigned char c, unsigned char d) { SD("cmask %d%d%d%d", a, b, c, d); r_cmask(a, b, c, d); }
static void *my_eglGetProcAddress(const char *n) { return ios_gl_proc(n); }
void *ios_gl_proc(const char *n) {
  if (!strcmp(n, "eglGetProcAddress")) return (void*)my_eglGetProcAddress;
#define W(nm, r, w) if (!strcmp(n, nm)) { if (!r) r = gl_proc(nm); return r ? (void*)w : NULL; }
  W("glEnable", r_enable, w_enable) W("glDisable", r_disable, w_disable) W("glStencilFunc", r_sfunc, w_sfunc)
  W("glStencilOp", r_sop, w_sop) W("glStencilMask", r_smask, w_smask) W("glClear", r_clear, w_clear) W("glColorMask", r_cmask, w_cmask)
  if (!strcmp(n, "glBlitFramebuffer") || !strcmp(n, "glBlitFramebufferANGLE") || !strcmp(n, "glBlitFramebufferEXT")) {
    if (!real_blit) real_blit = gl_proc("glBlitFramebuffer");
    return real_blit ? (void*)fix_blit : NULL;
  }
  return gl_proc(n);
}
void *glctx_resolve(const char *n); void *glctx_resolve3(const char *n);
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
  { void *g = glctx_resolve(n); if (!g) g = glctx_resolve3(n); if (g) return g; }
  void *p = platform_resolve(n);
  if (!p && !strncmp(n, "SDL_", 4)) p = dlsym(RTLD_DEFAULT, n);
  if (!p && (!strncmp(n, "gl", 2) || !strncmp(n, "egl", 3))) p = ios_gl_proc(n);
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
  char ur[1200]; snprintf(ur, sizeof ur, "%s/../Library/user", g_docs); mkdir(ur, 0755); { char rp[1200]; if (realpath(ur, rp)) snprintf(ur, sizeof ur, "%s", rp); } snprintf(wr, sizeof wr, "%s/garrysmod", ur); mkdir(wr, 0755); { char rp[1200]; if (realpath(wr, rp)) snprintf(wr, sizeof wr, "%s", rp); }
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
  setenv("SOURCEVR_GAME", "garrysmod", 1); setenv("SOURCE_GAME", "garrysmod", 1); setenv("EXTRAS_VPK_PATH", rt, 1); setenv("APP_MOD_LIB", lib, 1); setenv("LANG", "en_US.UTF-8", 1);
  bionic_set_root(g_docs);
  setenv("SO_MAP", "1", 1); setenv("SO_VERBOSE", "1", 1);

  platform_init();
  { extern void webview_init(void); webview_init(); }
  bionic_gl_proc = ios_gl_proc;
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
