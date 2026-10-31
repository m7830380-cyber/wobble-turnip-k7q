// bionic.c — Android bionic ABI shim for running GDroid's arm64 .so files on iOS (and Linux for host tests).
// Provides: bionic-only symbols (__sF, __errno, *_chk, __system_property_get, android_log...),
// struct translations where bionic layout != host layout (stat, dirent, FILE*), and the fake TLS block.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include "../loader/elf_loader.h"
#include "bionic.h"

#define LOG(...) do { fprintf(stderr, "[bionic] " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* ---------------- fake bionic thread block ----------------
 * bionic arm64 TLS slots (tp[i]): 0=DTV 1=THREAD_ID 2=APP 3=OPENGL 4=OPENGL_API 5=STACK_GUARD
 * 6=SANITIZER 7=ART_THREAD_SELF ... Code reads [tp,#40] (stack guard) everywhere.
 * On iOS tpidr_el0 is free for user code on arm64 (Darwin uses tpidrro_el0), so we own it.
 */
#define TCB_SIZE 0x4000
uintptr_t bionic_stack_guard = 0x5a17c0de5a17c0deULL;
static pthread_key_t g_tcb_key;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static inline void set_tp(void *p) { __asm__ volatile("msr tpidr_el0, %0" ::"r"(p)); }
static inline void *get_tp(void) { void *p; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(p)); return p; }

static void tcb_free(void *p) { munmap(p, TCB_SIZE); }
static void once(void) { pthread_key_create(&g_tcb_key, tcb_free); }

void bionic_thread_enter(void) {
  pthread_once(&g_once, once);
  if (pthread_getspecific(g_tcb_key)) return;
  uintptr_t *tcb = mmap(NULL, TCB_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  tcb[1] = (uintptr_t)pthread_self();
  tcb[5] = bionic_stack_guard;
  so_tls_init_block((uint8_t *)tcb);
  pthread_setspecific(g_tcb_key, tcb);
#ifndef BIONIC_HOST_TEST_NO_TP
  set_tp(tcb);
#endif
}

/* pthread_create wrapper: every thread the game spawns gets its own TCB */
struct tramp { void *(*fn)(void *); void *arg; };
static void *thread_tramp(void *p) {
  struct tramp t = *(struct tramp *)p; free(p);
  bionic_thread_enter();
  return t.fn(t.arg);
}
void bp_attr_to_host(const void *, pthread_attr_t *);
static int b_pthread_create(pthread_t *th, const void *a, void *(*fn)(void *), void *arg) {
  struct tramp *t = malloc(sizeof *t); t->fn = fn; t->arg = arg;
  pthread_attr_t ha; bp_attr_to_host(a, &ha);
  int r = pthread_create(th, &ha, thread_tramp, t);
  pthread_attr_destroy(&ha);
  if (r) free(t);
  return r;
}

/* ---------------- errno / stdio ---------------- */
static int *b_errno(void) { return &errno; }

/* bionic FILE __sF[3] is an array of 152-byte structs; code takes &__sF[1] for stdout.
 * We hand out a fake array and translate pointers in every FILE* function. */
#define BIONIC_FILE_SZ 152
uint8_t b_sF[3 * BIONIC_FILE_SZ];
static FILE *xf(FILE *f) {
  uint8_t *p = (uint8_t *)f;
  if (p == b_sF) return stdin;
  if (p == b_sF + BIONIC_FILE_SZ) return stdout;
  if (p == b_sF + 2 * BIONIC_FILE_SZ) return stderr;
  return f;
}
static FILE *b_stdin_p, *b_stdout_p, *b_stderr_p; /* bionic also exports stdin/stdout/stderr as FILE* vars */
static int b_fprintf(FILE *f, const char *fmt, ...) { va_list a; va_start(a, fmt); int r = vfprintf(xf(f), fmt, a); va_end(a); return r; }
static int b_vfprintf(FILE *f, const char *fmt, va_list a) { return vfprintf(xf(f), fmt, a); }
static int b_fputs(const char *s, FILE *f) { return fputs(s, xf(f)); }
static int b_fputc(int c, FILE *f) { return fputc(c, xf(f)); }
static size_t b_fwrite(const void *p, size_t s, size_t n, FILE *f) { return fwrite(p, s, n, xf(f)); }
static size_t b_fread(void *p, size_t s, size_t n, FILE *f) { return fread(p, s, n, xf(f)); }
static int b_fflush(FILE *f) { return fflush(f ? xf(f) : NULL); }
static int b_fileno(FILE *f) { return fileno(xf(f)); }
static int b_fgetc(FILE *f) { return fgetc(xf(f)); }
static int b_getc(FILE *f) { return getc(xf(f)); }
static char *b_fgets(char *s, int n, FILE *f) { return fgets(s, n, xf(f)); }
static int b_feof(FILE *f) { return feof(xf(f)); }
static int b_ferror(FILE *f) { return ferror(xf(f)); }
static FILE *b_freopen(const char *p, const char *m, FILE *f) { return freopen(p, m, xf(f)); }
static int b_isatty(int fd) { return isatty(fd); }

/* ---------------- fortify *_chk ---------------- */
static void chk_fail(const char *w) { LOG("FORTIFY: %s overflow", w); abort(); }
static void *b_memcpy_chk(void *d, const void *s, size_t n, size_t ds) { if (n > ds) chk_fail("memcpy"); return memcpy(d, s, n); }
static void *b_memmove_chk(void *d, const void *s, size_t n, size_t ds) { if (n > ds) chk_fail("memmove"); return memmove(d, s, n); }
static void *b_memset_chk(void *d, int c, size_t n, size_t ds) { if (n > ds) chk_fail("memset"); return memset(d, c, n); }
static size_t b_strlen_chk(const char *s, size_t ds) { size_t n = strlen(s); if (n >= ds) chk_fail("strlen"); return n; }
static char *b_strchr_chk(const char *s, int c, size_t ds) { (void)ds; return strchr(s, c); }
static char *b_strncpy_chk(char *d, const char *s, size_t n, size_t ds) { if (n > ds) chk_fail("strncpy"); return strncpy(d, s, n); }
static char *b_strncpy_chk2(char *d, const char *s, size_t n, size_t ds, size_t ss) { (void)ss; if (n > ds) chk_fail("strncpy2"); return strncpy(d, s, n); }
static char *b_strncat_chk(char *d, const char *s, size_t n, size_t ds) { (void)ds; return strncat(d, s, n); }
static int b_vsnprintf_chk(char *d, size_t n, int fl, size_t ds, const char *f, va_list a) { (void)fl; if (n > ds) chk_fail("vsnprintf"); return vsnprintf(d, n, f, a); }
static int b_vsprintf_chk(char *d, int fl, size_t ds, const char *f, va_list a) { (void)fl; int r = vsnprintf(d, ds, f, a); if ((size_t)r >= ds) chk_fail("vsprintf"); return r; }
static size_t b_fread_chk(void *p, size_t s, size_t n, FILE *f, size_t ds) { (void)ds; return fread(p, s, n, xf(f)); }
static size_t b_fwrite_chk(const void *p, size_t s, size_t n, FILE *f, size_t ds) { (void)ds; return fwrite(p, s, n, xf(f)); }
static ssize_t b_readlink_chk(const char *p, char *b, size_t n, size_t ds) { (void)ds; return readlink(p, b, n); }
static void b_FD_SET_chk(int fd, fd_set *s, size_t sz) { (void)sz; FD_SET(fd, s); }
static int b_FD_ISSET_chk(int fd, const fd_set *s, size_t sz) { (void)sz; return FD_ISSET(fd, s); }
static int b_open_2(const char *p, int fl) { return open(p, bionic_oflags(fl)); }
static void b_stack_chk_fail(void) { LOG("stack smashing detected"); abort(); }

/* ---------------- open flags: Linux/bionic values -> host ---------------- */
int bionic_oflags(int f) {
#ifdef __APPLE__
  int r = f & 3;
  if (f & 0100) r |= O_CREAT;  if (f & 0200) r |= O_EXCL;  if (f & 01000) r |= O_TRUNC;
  if (f & 02000) r |= O_APPEND; if (f & 04000) r |= O_NONBLOCK; if (f & 0200000) r |= O_DIRECTORY;
  if (f & 0400000) r |= O_NOFOLLOW; if (f & 02000000) r |= O_CLOEXEC;
  return r;
#else
  return f;
#endif
}
static int b_open(const char *p, int fl, ...) { va_list a; va_start(a, fl); int m = va_arg(a, int); va_end(a); return open(bionic_path(p), bionic_oflags(fl), m); }
static int b_openat(int d, const char *p, int fl, ...) { va_list a; va_start(a, fl); int m = va_arg(a, int); va_end(a); return openat(d, bionic_path(p), bionic_oflags(fl), m); }

/* ---------------- struct stat (bionic arm64 layout, 128 bytes) ---------------- */
struct bstat {
  uint64_t st_dev, st_ino; uint32_t st_mode, st_nlink, st_uid, st_gid;
  uint64_t st_rdev, __pad1; int64_t st_size; int32_t st_blksize, __pad2; int64_t st_blocks;
  struct timespec st_atim, st_mtim, st_ctim; uint32_t __unused4, __unused5;
};
static void cvt_stat(const struct stat *h, struct bstat *b) {
  memset(b, 0, sizeof *b);
  b->st_dev = h->st_dev; b->st_ino = h->st_ino; b->st_mode = h->st_mode; b->st_nlink = h->st_nlink;
  b->st_uid = h->st_uid; b->st_gid = h->st_gid; b->st_rdev = h->st_rdev; b->st_size = h->st_size;
  b->st_blksize = h->st_blksize; b->st_blocks = h->st_blocks;
#ifdef __APPLE__
  b->st_atim = h->st_atimespec; b->st_mtim = h->st_mtimespec; b->st_ctim = h->st_ctimespec;
#else
  b->st_atim = h->st_atim; b->st_mtim = h->st_mtim; b->st_ctim = h->st_ctim;
#endif
}
static int b_stat(const char *p, struct bstat *b) { struct stat h; int r = stat(bionic_path(p), &h); if (!r) cvt_stat(&h, b); return r; }
static int b_lstat(const char *p, struct bstat *b) { struct stat h; int r = lstat(bionic_path(p), &h); if (!r) cvt_stat(&h, b); return r; }
static int b_fstat(int fd, struct bstat *b) { struct stat h; int r = fstat(fd, &h); if (!r) cvt_stat(&h, b); return r; }

/* ---------------- dirent (bionic: ino u64, off i64, reclen u16, type u8, name[256]) ---------------- */
struct bdirent { uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256]; };
static struct bdirent *b_readdir(DIR *d) {
  static __thread struct bdirent out;
  struct dirent *e = readdir(d);
  if (!e) return NULL;
  out.d_ino = e->d_ino; out.d_off = 0; out.d_reclen = sizeof out; out.d_type = e->d_type;
  snprintf(out.d_name, sizeof out.d_name, "%s", e->d_name);
  return &out;
}
static DIR *b_opendir(const char *p) { return opendir(bionic_path(p)); }

/* ---------------- paths: /sdcard, case-insensitivity hook ---------------- */
static char g_root[1024];
void bionic_set_root(const char *r) { snprintf(g_root, sizeof g_root, "%s", r); }
const char *bionic_path(const char *p) {
  static __thread char buf[2048];
  if (p && g_root[0] && !strncmp(p, "/sdcard/", 8)) { snprintf(buf, sizeof buf, "%s/%s", g_root, p + 8); return buf; }
  return p;
}
static FILE *b_fopen(const char *p, const char *m) { return fopen(bionic_path(p), m); }
static int b_access(const char *p, int m) { return access(bionic_path(p), m); }
static int b_mkdir(const char *p, mode_t m) { return mkdir(bionic_path(p), m); }

/* ---------------- system properties / logging / misc ---------------- */
static int b_system_property_get(const char *k, char *v) {
  const char *r = "";
  if (!strcmp(k, "ro.build.version.sdk")) r = "33";
  else if (!strcmp(k, "ro.product.model")) r = "iPhone";
  else if (!strcmp(k, "ro.product.manufacturer")) r = "Apple";
  else if (!strcmp(k, "ro.hardware")) r = "apple";
  snprintf(v, 92, "%s", r);
  return (int)strlen(v);
}
static const char *prio(int p) { static const char *n = "??VDIWEF"; static __thread char s[2]; s[0] = (p >= 0 && p < 8) ? n[p] : '?'; return s; }
static int b_log_write(int p, const char *tag, const char *msg) { fprintf(stderr, "%s/%s: %s\n", prio(p), tag ? tag : "", msg); return 1; }
static int b_log_print(int p, const char *tag, const char *fmt, ...) {
  char b[4096]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); return b_log_write(p, tag, b);
}
static int b_log_vprint(int p, const char *tag, const char *fmt, va_list a) { char b[4096]; vsnprintf(b, sizeof b, fmt, a); return b_log_write(p, tag, b); }
static void b_assert2(const char *f, int l, const char *fn, const char *e) { LOG("assert %s:%d %s: %s", f, l, fn, e); abort(); }
static void b_set_abort_message(const char *m) { LOG("abort: %s", m); }
static unsigned long b_getauxval(unsigned long t) {
  if (t == 16) return (1 << 1) | (1 << 3) | (1 << 7) | (1 << 8);  /* AT_HWCAP: ASIMD FP ATOMICS CRC32 */
  if (t == 6) return 16384;                                        /* AT_PAGESZ */
  return 0;
}
static size_t b_ctype_get_mb_cur_max(void) { return 4; }
static int b_register_atfork(void (*a)(void), void (*b)(void), void (*c)(void), void *d) { (void)d; return pthread_atfork(a, b, c); }
static long b_sysconf(int n) {
  /* bionic numbering for the few we care about */
  switch (n) {
    case 0x27: return 16384;                         /* _SC_PAGESIZE */
    case 0x60: case 0x61: return sysconf(_SC_NPROCESSORS_ONLN); /* NPROCESSORS_CONF/ONLN */
    case 0x6: return 100;                            /* CLK_TCK */
  }
  return sysconf(n);
}
static int b_prctl(int o, ...) { (void)o; return 0; }
static void *b_dlopen(const char *n, int f) { return bionic_dlopen(n, f); }
static void *b_dlsym(void *h, const char *s) { return bionic_dlsym(h, s); }
static int b_dlclose(void *h) { (void)h; return 0; }
static char *b_dlerror(void) { return NULL; }

/* clock ids are identical on Linux; Darwin differs */
static int b_clock_gettime(int id, struct timespec *t) {
#ifdef __APPLE__
  clockid_t c = id == 1 || id == 4 || id == 7 ? CLOCK_MONOTONIC : id == 2 ? CLOCK_PROCESS_CPUTIME_ID : id == 3 ? CLOCK_THREAD_CPUTIME_ID : CLOCK_REALTIME;
  return clock_gettime(c, t);
#else
  return clock_gettime(id, t);
#endif
}

/* bionic pthread_mutex_t is 40 bytes on arm64, Darwin's is 64 — game structs embed 40.
 * Store a pointer to a heap mutex in the first word (lazy init handles static initializers = zeros). */
#ifdef __APPLE__
#define MX(m) bmx((void *)(m))
static pthread_mutex_t *bmx(void *m) {
  pthread_mutex_t **pp = m, *p = __atomic_load_n(pp, __ATOMIC_ACQUIRE);
  if (p) return p;
  p = malloc(sizeof *p);
  pthread_mutexattr_t at; pthread_mutexattr_init(&at);
  if (((int32_t *)m)[2] & 0x4000) pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE); /* bionic recursive init flag */
  pthread_mutex_init(p, &at);
  pthread_mutex_t *exp = NULL;
  if (!__atomic_compare_exchange_n(pp, &exp, p, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { pthread_mutex_destroy(p); free(p); return exp; }
  return p;
}
#endif

extern char bp_mutexattr_init[];
extern char bp_mutexattr_destroy[];
extern char bp_mutexattr_settype[];
extern char bp_mutexattr_gettype[];
extern char bp_mutexattr_setpshared[];
extern char bp_mutex_init[];
extern char bp_mutex_destroy[];
extern char bp_mutex_lock[];
extern char bp_mutex_trylock[];
extern char bp_mutex_unlock[];
extern char bp_mutex_timedlock[];
extern char bp_condattr_init[];
extern char bp_condattr_destroy[];
extern char bp_condattr_setclock[];
extern char bp_condattr_getclock[];
extern char bp_condattr_setpshared[];
extern char bp_cond_init[];
extern char bp_cond_destroy[];
extern char bp_cond_signal[];
extern char bp_cond_broadcast[];
extern char bp_cond_wait[];
extern char bp_cond_timedwait[];
extern char bp_cond_timedwait_monotonic_np[];
extern char bp_cond_timedwait_relative_np[];
extern char bp_cond_clockwait[];
extern char bp_rwlock_init[];
extern char bp_rwlock_destroy[];
extern char bp_rwlock_rdlock[];
extern char bp_rwlock_wrlock[];
extern char bp_rwlock_tryrdlock[];
extern char bp_rwlock_trywrlock[];
extern char bp_rwlock_unlock[];
extern char bp_sem_init[];
extern char bp_sem_destroy[];
extern char bp_sem_post[];
extern char bp_sem_wait[];
extern char bp_sem_trywait[];
extern char bp_sem_timedwait[];
extern char bp_sem_getvalue[];
extern char bp_attr_init[];
extern char bp_attr_destroy[];
extern char bp_attr_setdetachstate[];
extern char bp_attr_getdetachstate[];
extern char bp_attr_setstacksize[];
extern char bp_attr_getstacksize[];
extern char bp_attr_setguardsize[];
extern char bp_attr_setschedparam[];
extern char bp_attr_getschedparam[];
extern char bp_attr_setschedpolicy[];
extern char bp_attr_setscope[];
extern char bp_once[];
void bp_attr_to_host(const void *, pthread_attr_t *);
extern char bs_sigemptyset[];
extern char bs_sigfillset[];
extern char bs_sigaddset[];
extern char bs_sigdelset[];
extern char bs_sigismember[];
extern char bs_sigaction[];
extern char bs_sigprocmask[];
extern char bs_pthread_sigmask[];
extern char bs_sigsuspend[];
extern char bs_sigpending[];
extern char bs_signal[];
extern char bs_raise[];
extern char bs_pthread_kill[];
extern char bs_kill[];
extern char bs_setjmp[] __asm__("bs_setjmp");
extern char bs__setjmp[] __asm__("bs__setjmp");
extern char bs_sigsetjmp[] __asm__("bs_sigsetjmp");
extern char bs_longjmp[] __asm__("bs_longjmp");
extern char bs__longjmp[] __asm__("bs__longjmp");
extern char bs_siglongjmp[] __asm__("bs_siglongjmp");
/* ---------------- symbol table ---------------- */
typedef struct { const char *n; void *p; } sym;
#define S(name, fn) {name, (void *)(fn)}
#ifdef __APPLE__
/* bionic(Linux) mmap flags -> Darwin */
static void *b_mmap(void *a, size_t l, int pr, int fl, int fd, off_t o) {
  int f = fl & 3;
  if (fl & 0x10) f |= MAP_FIXED;
  if (fl & 0x20) f |= MAP_ANON;
  if (fl & 0x4000) f |= MAP_NORESERVE;
  return mmap(a, l, pr, f, (fl & 0x20) ? -1 : fd, o);
}
static int b_madvise(void *a, size_t l, int adv) { (void)a; (void)l; (void)adv; return 0; }
#define APPLE_SYMS S("mmap", b_mmap), S("mmap64", b_mmap), S("madvise", b_madvise),
#else
#define APPLE_SYMS
#endif
static sym g_syms[] = {
  S("pthread_mutexattr_init", bp_mutexattr_init), S("pthread_mutexattr_destroy", bp_mutexattr_destroy), S("pthread_mutexattr_settype", bp_mutexattr_settype), S("pthread_mutexattr_gettype", bp_mutexattr_gettype), S("pthread_mutexattr_setpshared", bp_mutexattr_setpshared), S("pthread_mutex_init", bp_mutex_init), S("pthread_mutex_destroy", bp_mutex_destroy), S("pthread_mutex_lock", bp_mutex_lock), S("pthread_mutex_trylock", bp_mutex_trylock), S("pthread_mutex_unlock", bp_mutex_unlock), S("pthread_mutex_timedlock", bp_mutex_timedlock), S("pthread_condattr_init", bp_condattr_init), S("pthread_condattr_destroy", bp_condattr_destroy), S("pthread_condattr_setclock", bp_condattr_setclock), S("pthread_condattr_getclock", bp_condattr_getclock), S("pthread_condattr_setpshared", bp_condattr_setpshared), S("pthread_cond_init", bp_cond_init), S("pthread_cond_destroy", bp_cond_destroy), S("pthread_cond_signal", bp_cond_signal), S("pthread_cond_broadcast", bp_cond_broadcast), S("pthread_cond_wait", bp_cond_wait), S("pthread_cond_timedwait", bp_cond_timedwait), S("pthread_cond_timedwait_monotonic_np", bp_cond_timedwait_monotonic_np), S("pthread_cond_timedwait_relative_np", bp_cond_timedwait_relative_np), S("pthread_cond_clockwait", bp_cond_clockwait), S("pthread_rwlock_init", bp_rwlock_init), S("pthread_rwlock_destroy", bp_rwlock_destroy), S("pthread_rwlock_rdlock", bp_rwlock_rdlock), S("pthread_rwlock_wrlock", bp_rwlock_wrlock), S("pthread_rwlock_tryrdlock", bp_rwlock_tryrdlock), S("pthread_rwlock_trywrlock", bp_rwlock_trywrlock), S("pthread_rwlock_unlock", bp_rwlock_unlock), S("sem_init", bp_sem_init), S("sem_destroy", bp_sem_destroy), S("sem_post", bp_sem_post), S("sem_wait", bp_sem_wait), S("sem_trywait", bp_sem_trywait), S("sem_timedwait", bp_sem_timedwait), S("sem_getvalue", bp_sem_getvalue), S("pthread_attr_init", bp_attr_init), S("pthread_attr_destroy", bp_attr_destroy), S("pthread_attr_setdetachstate", bp_attr_setdetachstate), S("pthread_attr_getdetachstate", bp_attr_getdetachstate), S("pthread_attr_setstacksize", bp_attr_setstacksize), S("pthread_attr_getstacksize", bp_attr_getstacksize), S("pthread_attr_setguardsize", bp_attr_setguardsize), S("pthread_attr_setschedparam", bp_attr_setschedparam), S("pthread_attr_getschedparam", bp_attr_getschedparam), S("pthread_attr_setschedpolicy", bp_attr_setschedpolicy), S("pthread_attr_setscope", bp_attr_setscope), S("pthread_once", bp_once),
  S("sigemptyset", bs_sigemptyset), S("sigfillset", bs_sigfillset), S("sigaddset", bs_sigaddset), S("sigdelset", bs_sigdelset), S("sigismember", bs_sigismember), S("sigaction", bs_sigaction), S("sigprocmask", bs_sigprocmask), S("pthread_sigmask", bs_pthread_sigmask), S("sigsuspend", bs_sigsuspend), S("sigpending", bs_sigpending), S("signal", bs_signal), S("raise", bs_raise), S("pthread_kill", bs_pthread_kill), S("kill", bs_kill), S("setjmp", bs_setjmp), S("_setjmp", bs__setjmp), S("sigsetjmp", bs_sigsetjmp), S("longjmp", bs_longjmp), S("_longjmp", bs__longjmp), S("siglongjmp", bs_siglongjmp),
  S("__errno", b_errno), S("__sF", b_sF), S("stdin", &b_stdin_p), S("stdout", &b_stdout_p), S("stderr", &b_stderr_p),
  S("fprintf", b_fprintf), S("vfprintf", b_vfprintf), S("fputs", b_fputs), S("fputc", b_fputc), S("fwrite", b_fwrite),
  S("fread", b_fread), S("fflush", b_fflush), S("fileno", b_fileno), S("fgetc", b_fgetc), S("getc", b_getc),
  S("fgets", b_fgets), S("feof", b_feof), S("ferror", b_ferror), S("freopen", b_freopen), S("isatty", b_isatty),
  S("__memcpy_chk", b_memcpy_chk), S("__memmove_chk", b_memmove_chk), S("__memset_chk", b_memset_chk),
  S("__strlen_chk", b_strlen_chk), S("__strchr_chk", b_strchr_chk), S("__strncpy_chk", b_strncpy_chk),
  S("__strncpy_chk2", b_strncpy_chk2), S("__strncat_chk", b_strncat_chk), S("__vsnprintf_chk", b_vsnprintf_chk),
  S("__vsprintf_chk", b_vsprintf_chk), S("__fread_chk", b_fread_chk), S("__fwrite_chk", b_fwrite_chk),
  S("__readlink_chk", b_readlink_chk), S("__FD_SET_chk", b_FD_SET_chk), S("__FD_ISSET_chk", b_FD_ISSET_chk),
  S("__open_2", b_open_2), S("__stack_chk_fail", b_stack_chk_fail), S("__stack_chk_guard", &bionic_stack_guard),
  S("open", b_open), S("openat", b_openat), S("stat", b_stat), S("lstat", b_lstat), S("fstat", b_fstat),
  S("readdir", b_readdir), S("opendir", b_opendir), S("fopen", b_fopen), S("access", b_access), S("mkdir", b_mkdir),
  S("__system_property_get", b_system_property_get), S("__android_log_write", b_log_write),
  S("__android_log_print", b_log_print), S("__android_log_vprint", b_log_vprint), S("__assert2", b_assert2),
  S("android_set_abort_message", b_set_abort_message), S("getauxval", b_getauxval),
  S("__ctype_get_mb_cur_max", b_ctype_get_mb_cur_max), S("__register_atfork", b_register_atfork),
  S("sysconf", b_sysconf), S("prctl", b_prctl), S("pthread_create", b_pthread_create),
  S("dl_iterate_phdr", so_iterate_phdr), S("dlopen", b_dlopen), S("dlsym", b_dlsym), S("dlclose", b_dlclose), S("dlerror", b_dlerror),
  S("clock_gettime", b_clock_gettime),
  APPLE_SYMS
  {0, 0}};

static so_resolver g_next;
void *bionic_resolve(const char *n) {
  for (sym *s = g_syms; s->n; s++) if (!strcmp(s->n, n)) return s->p;
  void *p = g_next ? g_next(n) : NULL;
  return p;
}
void bionic_init(so_resolver next) {
  g_next = next;
  b_stdin_p = (FILE *)b_sF; b_stdout_p = (FILE *)(b_sF + BIONIC_FILE_SZ); b_stderr_p = (FILE *)(b_sF + 2 * BIONIC_FILE_SZ);
  bionic_thread_enter();
}

/* dlopen of a game lib -> our loader; system libs -> pseudo handle resolved through bionic_resolve */
#define SYSLIB_HANDLE ((void *)1)
#define GL_HANDLE ((void *)2)
void *bionic_dlopen(const char *n, int f) {
  (void)f;
  if (!n) return SYSLIB_HANDLE;
  const char *b = strrchr(n, '/'); b = b ? b + 1 : n;
  if (!strncmp(b, "libGLES", 7) || !strncmp(b, "libEGL", 6) || !strncmp(b, "libGL.", 6)) return GL_HANDLE;
  so_module *m = so_find(b);
  if (!m) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", getenv("APP_LIB_PATH") ? getenv("APP_LIB_PATH") : ".", b);
    if (access(p, R_OK) == 0) { int u; m = so_load(p, &u); if (m) so_init(m); }
  }
  if (m) return m;
  return SYSLIB_HANDLE; /* libc.so, libEGL.so, ... */
}
void *(*bionic_gl_proc)(const char *);
void *bionic_dlsym(void *h, const char *s) {
  if (h == GL_HANDLE || !strncmp(s, "gl", 2) || !strncmp(s, "egl", 3)) {
    void *p = bionic_gl_proc ? bionic_gl_proc(s) : NULL; if (p) return p;
    if (h == GL_HANDLE) return NULL;
  }
  if (h && h != SYSLIB_HANDLE && (uintptr_t)h > 16) { void *p = so_sym(h, s); if (p) return p; }
  return bionic_resolve(s);
}
