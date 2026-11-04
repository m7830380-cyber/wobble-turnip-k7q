// Native-dylib entry (A8+): the engine comes from the owner's iOS dylibs in Frameworks/ (no ELF loader, no JIT).
// Our launcher runs first, then liblauncher.dylib LauncherMainIOS(argc, argv) runs the game on this thread.
#import <UIKit/UIKit.h>
#include <SDL.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <mach/mach.h>
#include <os/proc.h>
#include <dispatch/dispatch.h>

char *ios_bundle_path(void);
char *ios_documents_path(void);

// mp_ui.m sets these; nothing calls them in dylib mode yet (engine log is plain stdout/stderr)
void (*bionic_cmd_hook)(const char *);
void (*bionic_line_hook)(const char *);

int ios_jit_ok(void) { return 2; }  // 2 = not needed (native dylibs)

static void describe(const char *tag, void *a) {
  Dl_info i; if (dladdr(a, &i) && i.dli_fname) {
    const char *b = strrchr(i.dli_fname, '/'); b = b ? b + 1 : i.dli_fname;
    fprintf(stderr, "  %s %p = %s+0x%lx (%s)\n", tag, a, b, (unsigned long)((char *)a - (char *)i.dli_fbase), i.dli_sname ? i.dli_sname : "?");
  } else fprintf(stderr, "  %s %p\n", tag, a);
}
static void on_crash(int sig, siginfo_t *si, void *uc_) {
  ucontext_t *uc = uc_;
  fprintf(stderr, "\n*** signal %d addr %p\n", sig, si->si_addr);
  describe("pc", (void *)uc->uc_mcontext->__ss.__pc);
  describe("lr", (void *)uc->uc_mcontext->__ss.__lr);
  uint64_t *fp = (uint64_t *)uc->uc_mcontext->__ss.__fp;
  for (int i = 0; i < 24 && fp && ((uintptr_t)fp & 7) == 0; i++) { describe("bt", (void *)(fp[1] & 0x0000000fffffffffULL)); fp = (uint64_t *)fp[0]; }
  signal(sig, SIG_DFL); raise(sig);
}
static void *memwatch(void *a) {
  int peak = 0, last = -1000;
  for (;;) {
    task_vm_info_data_t vi; mach_msg_type_number_t c = TASK_VM_INFO_COUNT;
    if (!task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &c)) {
      int mb = (int)(vi.phys_footprint >> 20), av = (int)(os_proc_available_memory() >> 20); if (mb > peak) peak = mb;
      if (mb - last >= 100 || last - mb >= 100 || av < 300) { last = mb; fprintf(stderr, "[mem] footprint %d MB (peak %d), available before kill %d MB\n", mb, peak, av); }
    }
    sleep(2);
  }
  return a;
}

// ---- host functions the dylibs look up with dlsym(RTLD_DEFAULT) (all optional) ----
extern void ios_touch_open(const char *arg);
__attribute__((visibility("default"))) int GiOS_OpenTouchControls(const char *arg) { ios_touch_open(arg); return 1; }

int SDL_main(int argc, char **argv) {
  (void)argc; (void)argv;
  char bundle[1024], docs[1024], log[1200];
  snprintf(bundle, sizeof bundle, "%s", ios_bundle_path());
  snprintf(docs, sizeof docs, "%s", ios_documents_path());
  snprintf(log, sizeof log, "%s/gmod_log.txt", docs);
  freopen(log, "w", stderr); setvbuf(stderr, NULL, _IONBF, 0);
  dup2(fileno(stderr), fileno(stdout)); setvbuf(stdout, NULL, _IONBF, 0);
  fprintf(stderr, "[dyl] native dylib mode (no JIT), bundle %s\n", bundle);
  { struct sigaction sa = {0}; sa.sa_sigaction = on_crash; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    static char alt[65536]; stack_t ss = {alt, 0, sizeof alt}; sigaltstack(&ss, NULL);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGABRT, &sa, NULL); }
  { pthread_t t; pthread_create(&t, NULL, memwatch, NULL); pthread_detach(t); }
  SDL_Init(SDL_INIT_VIDEO);
  { extern void ios_kbfix_install(void); ios_kbfix_install(); }

  char fw[1200], rt[1200], wr[1200], ur[1200], lnk[1200], diag[1200];
  snprintf(fw, sizeof fw, "%s/Frameworks", bundle);
  snprintf(rt, sizeof rt, "%s/runtime", bundle);
  snprintf(ur, sizeof ur, "%s/../Library/user", docs); mkdir(ur, 0755); { char rp[1200]; if (realpath(ur, rp)) snprintf(ur, sizeof ur, "%s", rp); }
  snprintf(wr, sizeof wr, "%s/garrysmod", ur); mkdir(wr, 0755); { char rp[1200]; if (realpath(wr, rp)) snprintf(wr, sizeof wr, "%s", rp); }
  { extern void workshop_set_root(const char *); workshop_set_root(docs); }
  int ac = 0; char **av;
  { extern char **ios_run_launcher(const char *, const char *, int *); av = ios_run_launcher(docs, wr, &ac); }

  snprintf(lnk, sizeof lnk, "%s/garrysmod/runtime", docs); unlink(lnk); symlink(rt, lnk);
  { // engine (FreeType) wants platform/resource/linux_fonts; Windows GMod content has none
    const char *fn[] = {"dejavusans.ttf", "dejavusans-bold.ttf", "dejavusans-oblique.ttf", "dejavusans-boldoblique.ttf", "liberationmono-regular.ttf"};
    char d[1300], a[1400], b[1400]; snprintf(d, sizeof d, "%s/platform/resource/linux_fonts", docs); mkdir(d, 0755);
    for (int i = 0; i < 5; i++) {
      snprintf(a, sizeof a, "%s/platform/resource/linux_fonts/%s", rt, fn[i]); snprintf(b, sizeof b, "%s/%s", d, fn[i]);
      struct stat st; if (!stat(b, &st) && st.st_size > 0) continue;
      FILE *in = fopen(a, "rb"), *out = in ? fopen(b, "wb") : NULL; char buf[65536]; size_t n;
      if (in && out) while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
      if (in) fclose(in); if (out) fclose(out);
      fprintf(stderr, "font %s -> %s\n", fn[i], out ? "copied" : "FAILED");
    }
  }
  snprintf(diag, sizeof diag, "%s/diagnostics", docs); mkdir(diag, 0755);
  char cache[1300]; snprintf(cache, sizeof cache, "%s/../Library/Caches", docs);
  setenv("APP_LIB_PATH", fw, 1); setenv("APP_MOD_LIB", fw, 1);
  setenv("APP_DATA_PATH", docs, 1); setenv("VALVE_GAME_PATH", docs, 1);
  setenv("SOURCEVR_SHARED_CONTENT_PATH", docs, 1);
  { /* the engine refuses the sound cache when "<write path>/cache/sound/v2/<64 hex>.sound.cache" > 260 chars
       (LiveContainer nests containers) -> short symlink in the outer container */
    const char *ew = wr; static char sw[600];
    if (strlen(wr) + 92 >= 260) {
      const char *m = strstr(docs, "/Documents/Data/Application/");
      if (m) { snprintf(sw, sizeof sw, "%.*s/gmw", (int)(m - docs), docs);
        char cur[1200]; ssize_t n = readlink(sw, cur, sizeof cur - 1); if (n > 0) cur[n] = 0;
        if (n <= 0 || strcmp(cur, wr)) { unlink(sw); symlink(wr, sw); }
        struct stat st; if (!stat(sw, &st) && S_ISDIR(st.st_mode) && strlen(sw) + 92 < 260) ew = sw; }
      fprintf(stderr, "[snd] write path %zu chars too long for sound cache -> %s\n", strlen(wr), ew == wr ? "NO short path found" : ew);
    }
    setenv("SOURCEVR_WRITE_GAME_PATH", ew, 1); }
  setenv("SOURCEVR_GAME", "garrysmod", 1); setenv("SOURCE_GAME", "garrysmod", 1);
  setenv("EXTRAS_VPK_PATH", rt, 1); setenv("LANG", "en_US.UTF-8", 1);
  setenv("GMODVR_DIAG_DIR", diag, 1); setenv("SQLITE_TMPDIR", cache, 1);
  { char p[1400]; snprintf(p, sizeof p, "%s/flat_texture_cache", cache); setenv("GMOD_FLAT_TEXTURE_CACHE", p, 1);
    snprintf(p, sizeof p, "%s/flat_vulkan_cache", cache); setenv("GMOD_FLAT_VULKAN_CACHE", p, 1); }
  { NSString *v = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"]; setenv("GMODVR_APP_VERSION", v ? v.UTF8String : "0", 1); }

  char p[1300]; snprintf(p, sizeof p, "%s/liblauncher.dylib", fw);
  void *h = dlopen(p, RTLD_NOW | RTLD_GLOBAL);
  if (!h) { const char *e = dlerror(); fprintf(stderr, "[dyl] dlopen liblauncher failed: %s\n", e ? e : "?");
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "GMod", "Game libraries failed to load. Send Documents/gmod_log.txt.", NULL); return 1; }
  int (*lm)(int, char **) = (int (*)(int, char **))dlsym(h, "LauncherMainIOS");
  if (!lm) { fprintf(stderr, "[dyl] LauncherMainIOS missing\n"); return 1; }
  fprintf(stderr, "[dyl] LauncherMainIOS(%d args)\n", ac);
  int r = lm(ac, av);
  fprintf(stderr, "[dyl] LauncherMainIOS returned %d\n", r);
  exit(0);
}
