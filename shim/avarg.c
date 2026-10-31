// Variadic ABI bridge. Android (AAPCS64) callers pass varargs in x/q registers; Darwin arm64 variadic
// functions read them from the stack. So every variadic libc entry the Android libs call must be ours:
// an asm trampoline spills x0-x7/q0-q7, builds an AAPCS va_list and hands it to a C impl that formats
// with its own parser (each conversion is then printed with a correctly-typed Darwin snprintf call).
// v*-functions receive an AAPCS va_list by reference (32-byte struct), which is the same ava*.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <wchar.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <pthread.h>

typedef struct { char *stack; char *gr_top; char *vr_top; int gr_offs; int vr_offs; } ava;

static uint64_t gp(ava *a) {
  if (a->gr_offs < 0) { uint64_t v = *(uint64_t *)(a->gr_top + a->gr_offs); a->gr_offs += 8; return v; }
  uint64_t v = *(uint64_t *)a->stack; a->stack += 8; return v;
}
static double fp(ava *a) {
  if (a->vr_offs < 0) { double v = *(double *)(a->vr_top + a->vr_offs); a->vr_offs += 16; return v; }
  double v = *(double *)a->stack; a->stack += 8; return v;
}
static double fp_long(ava *a) { /* Android long double = binary128; take the slot, convert roughly */
  unsigned char q[16];
  if (a->vr_offs < 0) { memcpy(q, a->vr_top + a->vr_offs, 16); a->vr_offs += 16; }
  else { a->stack = (char *)(((uintptr_t)a->stack + 15) & ~(uintptr_t)15); memcpy(q, a->stack, 16); a->stack += 16; }
  uint64_t lo, hi; memcpy(&lo, q, 8); memcpy(&hi, q + 8, 8);
  int sign = hi >> 63; int e = (hi >> 48) & 0x7fff; uint64_t m = ((hi & 0xffffffffffffULL) << 4) | (lo >> 60);
  uint64_t d;
  if (e == 0) d = 0; else if (e == 0x7fff) d = 0x7ffULL << 52 | (m ? 1 : 0);
  else { int de = e - 16383 + 1023; if (de <= 0) d = 0; else if (de >= 0x7ff) d = 0x7ffULL << 52; else d = (uint64_t)de << 52 | m; }
  d |= (uint64_t)sign << 63; double r; memcpy(&r, &d, 8); return r;
}

typedef struct { char *b; size_t n, cap; } sb;
static void put(sb *s, const char *p, size_t n) {
  if (s->n + n + 1 > s->cap) { s->cap = (s->n + n + 1) * 2 + 64; s->b = realloc(s->b, s->cap); }
  memcpy(s->b + s->n, p, n); s->n += n; s->b[s->n] = 0;
}

static size_t format(sb *o, const char *f, ava *a) {
  int saved_errno = errno;
  if (!f) f = "(null fmt)";
  while (*f) {
    if (*f != '%') { const char *q = strchr(f, '%'); size_t n = q ? (size_t)(q - f) : strlen(f); put(o, f, n); f += n; continue; }
    if (f[1] == '%') { put(o, "%", 1); f += 2; continue; }
    char spec[64]; int k = 0; spec[k++] = *f++;
    while (*f && strchr("-+ #0'", *f) && k < 40) spec[k++] = *f++;
    if (*f == '*') { k += snprintf(spec + k, 12, "%d", (int)gp(a)); f++; } else while (*f >= '0' && *f <= '9' && k < 40) spec[k++] = *f++;
    if (*f == '.') { spec[k++] = *f++; if (*f == '*') { k += snprintf(spec + k, 12, "%d", (int)gp(a)); f++; } else while (*f >= '0' && *f <= '9' && k < 40) spec[k++] = *f++; }
    int len = 0; /* 0 int,1 hh,2 h,3 l,4 ll,5 j,6 z,7 t,8 L */
    for (;;) {
      if (*f == 'h') { len = len == 2 ? 1 : 2; f++; } else if (*f == 'l') { len = len == 3 ? 4 : 3; f++; }
      else if (*f == 'q') { len = 4; f++; } else if (*f == 'j') { len = 5; f++; } else if (*f == 'z') { len = 6; f++; }
      else if (*f == 't') { len = 7; f++; } else if (*f == 'L') { len = 8; f++; } else break;
    }
    char c = *f ? *f++ : 0; char tmp[512]; int n = 0;
    spec[k] = 0;
    switch (c) {
      case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'c': {
        uint64_t v = gp(a);
        if (c == 'c' && len == 3) { char mb[8] = {0}; wctomb(mb, (wchar_t)v); n = snprintf(tmp, sizeof tmp, (strcat(spec, "s"), spec), mb); break; }
        if (len >= 3 && len <= 7) { strcat(spec, "ll"); spec[strlen(spec) + 1] = 0; spec[strlen(spec)] = c; n = snprintf(tmp, sizeof tmp, spec, (long long)v); }
        else {
          if (len == 1) v = (c == 'd' || c == 'i') ? (uint64_t)(int64_t)(signed char)v : (unsigned char)v;
          else if (len == 2) v = (c == 'd' || c == 'i') ? (uint64_t)(int64_t)(short)v : (unsigned short)v;
          size_t L = strlen(spec); spec[L] = c; spec[L + 1] = 0; n = snprintf(tmp, sizeof tmp, spec, (int)v);
        }
        break; }
      case 'p': { void *v = (void *)gp(a); size_t L = strlen(spec); spec[L] = 'p'; spec[L + 1] = 0; n = snprintf(tmp, sizeof tmp, spec, v); break; }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
        double v = len == 8 ? fp_long(a) : fp(a); size_t L = strlen(spec); spec[L] = c; spec[L + 1] = 0; n = snprintf(tmp, sizeof tmp, spec, v); break; }
      case 's': case 'S': {
        const void *v = (const void *)gp(a);
        if (len == 3 || c == 'S') {
          const wchar_t *w = v ? v : L"(null)"; size_t L = strlen(spec); spec[L] = 'l'; spec[L + 1] = 's'; spec[L + 2] = 0;
          int need = snprintf(NULL, 0, spec, w); char *big = malloc(need + 1); snprintf(big, need + 1, spec, w); put(o, big, need); free(big);
        } else {
          size_t L = strlen(spec); spec[L] = 's'; spec[L + 1] = 0; const char *s = v ? v : "(null)";
          int need = snprintf(NULL, 0, spec, s); char *big = malloc(need + 1); snprintf(big, need + 1, spec, s); put(o, big, need); free(big);
        }
        continue; }
      case 'm': n = snprintf(tmp, sizeof tmp, "%s", strerror(saved_errno)); break;
      case 'n': { void *p = (void *)gp(a);
        if (len == 1) *(signed char *)p = o->n; else if (len == 2) *(short *)p = o->n; else if (len >= 3) *(long *)p = o->n; else *(int *)p = o->n;
        continue; }
      default: put(o, spec, strlen(spec)); if (c) put(o, &c, 1); continue;
    }
    if (n > (int)sizeof tmp - 1) n = sizeof tmp - 1;
    if (n > 0) put(o, tmp, n);
  }
  if (!o->b) put(o, "", 0);
  return o->n;
}

static int to_buf(char *d, size_t cap, const char *f, ava *a) {
  ava c = *a; sb o = {0}; size_t n = format(&o, f, &c);
  if (cap) { size_t m = n < cap - 1 ? n : cap - 1; memcpy(d, o.b, m); d[m] = 0; }
  free(o.b); return (int)n;
}
static int to_file(FILE *fp_, const char *f, ava *a) {
  ava c = *a; sb o = {0}; size_t n = format(&o, f, &c); fwrite(o.b, 1, n, fp_); free(o.b); return (int)n;
}

extern FILE *bionic_xf(FILE *f);
extern int bionic_log_write(int p, const char *tag, const char *msg);

#define IMPL(name) __asm__(#name)
int i_snprintf(char *d, size_t n, const char *f, ava *a) IMPL(bvi_snprintf);
int i_snprintf(char *d, size_t n, const char *f, ava *a) { return to_buf(d, n, f, a); }
int i_sprintf(char *d, const char *f, ava *a) IMPL(bvi_sprintf);
int i_sprintf(char *d, const char *f, ava *a) { return to_buf(d, (size_t)1 << 30, f, a); }
int i_printf(const char *f, ava *a) IMPL(bvi_printf);
int i_printf(const char *f, ava *a) { return to_file(stdout, f, a); }
int i_fprintf(FILE *fp_, const char *f, ava *a) IMPL(bvi_fprintf);
int i_fprintf(FILE *fp_, const char *f, ava *a) { return to_file(bionic_xf(fp_), f, a); }
void i_syslog(int p, const char *f, ava *a) IMPL(bvi_syslog);
void i_syslog(int p, const char *f, ava *a) { (void)p; to_file(stderr, f, a); fputc('\n', stderr); }
int i_logp(int p, const char *tag, const char *f, ava *a) IMPL(bvi_logp);
int i_logp(int p, const char *tag, const char *f, ava *a) { char b[4096]; to_buf(b, sizeof b, f, a); return bionic_log_write(p, tag, b); }
int i_asprintf(char **r, const char *f, ava *a) IMPL(bvi_asprintf);
int i_asprintf(char **r, const char *f, ava *a) { ava c = *a; sb o = {0}; size_t n = format(&o, f, &c); *r = o.b; return (int)n; }

static int scan(const char *s, FILE *fp_, const char *f, ava *a) {
  ava c = *a; void *p[24]; for (int i = 0; i < 24; i++) p[i] = (void *)gp(&c);
#define P p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15],p[16],p[17],p[18],p[19],p[20],p[21],p[22],p[23]
  return s ? sscanf(s, f, P) : fscanf(fp_, f, P);
}
int i_sscanf(const char *s, const char *f, ava *a) IMPL(bvi_sscanf);
int i_sscanf(const char *s, const char *f, ava *a) { return scan(s, NULL, f, a); }
int i_fscanf(FILE *fp_, const char *f, ava *a) IMPL(bvi_fscanf);
int i_fscanf(FILE *fp_, const char *f, ava *a) { return scan(NULL, bionic_xf(fp_), f, a); }
int i_swprintf(wchar_t *d, size_t n, const wchar_t *f, ava *a) IMPL(bvi_swprintf);
int i_swprintf(wchar_t *d, size_t n, const wchar_t *f, ava *a) {
  /* narrow the format, format, widen back (good enough for libc++ number formatting) */
  char nf[512]; size_t i = 0; for (; f[i] && i < sizeof nf - 1; i++) nf[i] = (char)f[i]; nf[i] = 0;
  char b[1024]; int r = to_buf(b, sizeof b, nf, a);
  if (!n) return -1; size_t m = 0; for (; b[m] && m < n - 1; m++) d[m] = (unsigned char)b[m]; d[m] = 0;
  return (size_t)r >= n ? -1 : r;
}

/* v* variants: va_list arrives as ava* directly */
static int b_vsnprintf(char *d, size_t n, const char *f, ava *a) { return to_buf(d, n, f, a); }
static int b_vsprintf(char *d, const char *f, ava *a) { return to_buf(d, (size_t)1 << 30, f, a); }
static int b_vprintf(const char *f, ava *a) { return to_file(stdout, f, a); }
static int b_vfprintf(FILE *fp_, const char *f, ava *a) { return to_file(bionic_xf(fp_), f, a); }
static int b_vasprintf(char **r, const char *f, ava *a) { return i_asprintf(r, f, a); }
static int b_vsscanf(const char *s, const char *f, ava *a) { return scan(s, NULL, f, a); }
static int b_vswprintf(wchar_t *d, size_t n, const wchar_t *f, ava *a) { return i_swprintf(d, n, f, a); }
static int b_vsnprintf_chk(char *d, size_t n, int fl, size_t ds, const char *f, ava *a) { (void)fl; (void)ds; return to_buf(d, n, f, a); }
static int b_vsprintf_chk(char *d, int fl, size_t ds, const char *f, ava *a) { (void)fl; return to_buf(d, ds, f, a); }
static int b_log_vprint(int p, const char *tag, const char *f, ava *a) { char b[4096]; to_buf(b, sizeof b, f, a); return bionic_log_write(p, tag, b); }

/* fixed-register replacements for variadic syscalls-ish functions */
extern int bionic_oflags(int f);
extern const char *bionic_path(const char *p);
static int b_open(const char *p, int fl, int mode) { return open(bionic_path(p), bionic_oflags(fl), mode); }
static int b_openat(int d, const char *p, int fl, int mode) { return openat(d, bionic_path(p), bionic_oflags(fl), mode); }
static int b_fcntl(int fd, int cmd, long arg) {
  if (cmd == 4 /*F_SETFL*/) return fcntl(fd, F_SETFL, bionic_oflags((int)arg));
  if (cmd == 3) { int r = fcntl(fd, F_GETFL); if (r < 0) return r; int o = r & 3; if (r & O_NONBLOCK) o |= 04000; if (r & O_APPEND) o |= 02000; return o; }
  if (cmd == 1 || cmd == 2) return fcntl(fd, cmd, (int)arg);
  if (cmd == 0 /*F_DUPFD*/) return fcntl(fd, F_DUPFD, (int)arg);
  if (cmd == 1030 /*F_DUPFD_CLOEXEC*/) return fcntl(fd, F_DUPFD_CLOEXEC, (int)arg);
  return 0;
}
static int b_ioctl(int fd, unsigned long req, void *arg) {
  if (req == 0x541B /*FIONREAD*/) return ioctl(fd, FIONREAD, arg);
  if (req == 0x5421 /*FIONBIO*/) return ioctl(fd, FIONBIO, arg);
  errno = ENOTTY; return -1;
}
static int b_prctl(int o, long a, long b, long c, long d) { (void)o; (void)a; (void)b; (void)c; (void)d; return 0; }
static long b_syscall(long n, long a, long b, long c, long d, long e) {
  (void)c; (void)d; (void)e;
  if (n == 178) { uint64_t t; pthread_threadid_np(NULL, &t); return (long)t; } /* gettid */
  if (n == 278) { arc4random_buf((void *)a, (size_t)b); return b; }           /* getrandom */
  if (n == 172) return getpid();
  errno = ENOSYS; return -1;
}
static void *b_mremap(void *old, size_t os, size_t ns, int fl, void *na) {
  (void)na; if (ns <= os) return old; if (!(fl & 1)) { errno = ENOMEM; return MAP_FAILED; }
  void *p = mmap(NULL, ns, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) return p; memcpy(p, old, os); munmap(old, os); return p;
}
static int b_execlp(const char *f, const char *a) { (void)f; (void)a; errno = ENOSYS; return -1; }

/* trampolines: K = number of fixed GP args before "..." */
#define TRAMP(name, K) \
  ".globl bv_" #name "\n.p2align 2\nbv_" #name ":\n" \
  " stp x29, x30, [sp, #-16]!\n mov x29, sp\n sub sp, sp, #224\n" \
  " stp x0, x1, [sp, #0]\n stp x2, x3, [sp, #16]\n stp x4, x5, [sp, #32]\n stp x6, x7, [sp, #48]\n" \
  " stp q0, q1, [sp, #64]\n stp q2, q3, [sp, #96]\n stp q4, q5, [sp, #128]\n stp q6, q7, [sp, #160]\n" \
  " add x9, x29, #16\n str x9, [sp, #192]\n add x9, sp, #64\n str x9, [sp, #200]\n add x9, sp, #192\n str x9, [sp, #208]\n" \
  " mov w9, #-" #K "\n add w9, w9, #8\n neg w9, w9\n lsl w9, w9, #3\n str w9, [sp, #216]\n mov w9, #-128\n str w9, [sp, #220]\n" \
  " add x" #K ", sp, #192\n bl bvi_" #name "\n mov sp, x29\n ldp x29, x30, [sp], #16\n ret\n"
__asm__(".text\n"
  TRAMP(snprintf, 3) TRAMP(sprintf, 2) TRAMP(printf, 1) TRAMP(fprintf, 2) TRAMP(syslog, 2)
  TRAMP(logp, 3) TRAMP(asprintf, 2) TRAMP(sscanf, 2) TRAMP(fscanf, 2) TRAMP(swprintf, 3));

#define EXT(n) extern char bv_##n[] __asm__("bv_" #n);
EXT(snprintf) EXT(sprintf) EXT(printf) EXT(fprintf) EXT(syslog) EXT(logp) EXT(asprintf) EXT(sscanf) EXT(fscanf) EXT(swprintf)

typedef struct { const char *n; void *f; } avsym;
static const avsym g_av[] = {
  {"snprintf", bv_snprintf}, {"sprintf", bv_sprintf}, {"printf", bv_printf}, {"fprintf", bv_fprintf},
  {"syslog", bv_syslog}, {"__android_log_print", bv_logp}, {"asprintf", bv_asprintf}, {"sscanf", bv_sscanf},
  {"fscanf", bv_fscanf}, {"swprintf", bv_swprintf},
  {"vsnprintf", b_vsnprintf}, {"vsprintf", b_vsprintf}, {"vprintf", b_vprintf}, {"vfprintf", b_vfprintf},
  {"vasprintf", b_vasprintf}, {"vsscanf", b_vsscanf}, {"vswprintf", b_vswprintf},
  {"__vsnprintf_chk", b_vsnprintf_chk}, {"__vsprintf_chk", b_vsprintf_chk}, {"__android_log_vprint", b_log_vprint},
  {"open", b_open}, {"open64", b_open}, {"openat", b_openat}, {"fcntl", b_fcntl}, {"ioctl", b_ioctl},
  {"prctl", b_prctl}, {"syscall", b_syscall}, {"mremap", b_mremap}, {"execlp", b_execlp},
  {0, 0}};
void *avarg_resolve(const char *name) {
  for (const avsym *s = g_av; s->n; s++) if (!strcmp(s->n, name)) return s->f;
  return NULL;
}
