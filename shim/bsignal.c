// bionic arm64 signal/setjmp ABI translation.
// bionic: sigset_t = 8 bytes (unsigned long), struct sigaction = {handler, sa_mask(8), int flags, restorer}
// jmp_buf = long[32] (256 bytes). Host (glibc/Darwin) layouts differ, so translate.
#define _GNU_SOURCE
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <pthread.h>

typedef unsigned long bsigset_t;
struct bsigaction { void *handler; bsigset_t mask; int flags; void *restorer; };

static void to_host(sigset_t *h, const bsigset_t *b) {
  sigemptyset(h); if (!b) return;
  for (int s = 1; s < 64; s++) if (*b & (1UL << (s - 1))) sigaddset(h, s);
}
static void to_bionic(bsigset_t *b, const sigset_t *h) {
  if (!b) return; *b = 0;
  for (int s = 1; s < 64; s++) if (sigismember(h, s) == 1) *b |= 1UL << (s - 1);
}
static int bad(int s) { return s <= 0 || s > 64; }

int bs_sigemptyset(bsigset_t *s) { if (!s) { errno = EINVAL; return -1; } *s = 0; return 0; }
int bs_sigfillset(bsigset_t *s) { if (!s) { errno = EINVAL; return -1; } *s = ~0UL; return 0; }
int bs_sigaddset(bsigset_t *s, int n) { if (!s || bad(n)) { errno = EINVAL; return -1; } *s |= 1UL << (n - 1); return 0; }
int bs_sigdelset(bsigset_t *s, int n) { if (!s || bad(n)) { errno = EINVAL; return -1; } *s &= ~(1UL << (n - 1)); return 0; }
int bs_sigismember(const bsigset_t *s, int n) { if (!s || bad(n)) { errno = EINVAL; return -1; } return (*s >> (n - 1)) & 1; }

// bionic SA_* values == Linux values. Darwin differs; map when building for Apple.
#ifdef __APPLE__
static int fl_to_host(int f) { int r = 0;
  if (f & 0x00000004) r |= SA_SIGINFO; if (f & 0x08000000) r |= SA_ONSTACK;
  if (f & 0x10000000) r |= SA_RESTART; if (f & 0x80000000) r |= SA_RESETHAND;
  if (f & 0x40000000) r |= SA_NODEFER; if (f & 1) r |= SA_NOCLDSTOP; return r; }
static int fl_to_b(int f) { int r = 0;
  if (f & SA_SIGINFO) r |= 4; if (f & SA_ONSTACK) r |= 0x08000000; if (f & SA_RESTART) r |= 0x10000000;
  if (f & SA_RESETHAND) r |= 0x80000000; if (f & SA_NODEFER) r |= 0x40000000; if (f & SA_NOCLDSTOP) r |= 1; return r; }
#else
static int fl_to_host(int f) { return f & ~0x04000000; /* drop SA_RESTORER */ }
static int fl_to_b(int f) { return f; }
#endif

int bs_sigaction(int sig, const struct bsigaction *a, struct bsigaction *o) {
  struct sigaction ha, ho; memset(&ha, 0, sizeof ha);
  if (a) { ha.sa_sigaction = (void (*)(int, siginfo_t *, void *))a->handler; to_host(&ha.sa_mask, &a->mask); ha.sa_flags = fl_to_host(a->flags); }
  int r = sigaction(sig, a ? &ha : NULL, &ho);
  if (r == 0 && o) { o->handler = (void *)ho.sa_sigaction; to_bionic(&o->mask, &ho.sa_mask); o->flags = fl_to_b(ho.sa_flags); o->restorer = 0; }
  return r;
}
int bs_sigprocmask(int how, const bsigset_t *s, bsigset_t *o) {
  sigset_t h, ho; to_host(&h, s); int r = sigprocmask(how, s ? &h : NULL, &ho); if (!r) to_bionic(o, &ho); return r;
}
int bs_pthread_sigmask(int how, const bsigset_t *s, bsigset_t *o) {
  sigset_t h, ho; to_host(&h, s); int r = pthread_sigmask(how, s ? &h : NULL, &ho); if (!r) to_bionic(o, &ho); return r;
}
int bs_sigsuspend(const bsigset_t *s) { sigset_t h; to_host(&h, s); return sigsuspend(&h); }
int bs_sigpending(bsigset_t *s) { sigset_t h; int r = sigpending(&h); if (!r) to_bionic(s, &h); return r; }

// bionic SIG_* numbers == Linux. Darwin numbering differs for some; translate when on Apple.
#ifdef __APPLE__
static int sig_to_host(int s) { switch (s) { case 7: return SIGBUS; case 10: return SIGUSR1; case 12: return SIGUSR2;
  case 17: return SIGCHLD; case 18: return SIGCONT; case 19: return SIGSTOP; case 20: return SIGTSTP; case 23: return SIGURG; default: return s; } }
#else
static int sig_to_host(int s) { return s; }
#endif
typedef void (*bsh)(int);
bsh bs_signal(int s, bsh h) { return signal(sig_to_host(s), h); }
int bs_raise(int s) { return raise(sig_to_host(s)); }
int bs_pthread_kill(pthread_t t, int s) { return pthread_kill(t, sig_to_host(s)); }
int bs_kill(int pid, int s) { return kill(pid, sig_to_host(s)); }

// setjmp/longjmp: own aarch64 implementation into bionic's 256-byte jmp_buf.
// layout: x19..x30 (12), sp, d8..d15 (8) = 21 slots; slot 24 = savemask flag, slot 25 = saved bionic mask.
__asm__(
  ".text\n.p2align 2\n"
  ".globl bs_setjmp\nbs_setjmp:\n"
  "  mov x1, #1\n  b bs_sigsetjmp\n"
  ".globl bs__setjmp\nbs__setjmp:\n"
  "  mov x1, #0\n"
  ".globl bs_sigsetjmp\nbs_sigsetjmp:\n"
  "  stp x19, x20, [x0, #0]\n  stp x21, x22, [x0, #16]\n  stp x23, x24, [x0, #32]\n"
  "  stp x25, x26, [x0, #48]\n  stp x27, x28, [x0, #64]\n  stp x29, x30, [x0, #80]\n"
  "  mov x2, sp\n  str x2, [x0, #96]\n"
  "  stp d8, d9, [x0, #104]\n  stp d10, d11, [x0, #120]\n  stp d12, d13, [x0, #136]\n  stp d14, d15, [x0, #152]\n"
  "  str x1, [x0, #192]\n"
  "  cbz x1, 1f\n"
  "  stp x0, x30, [sp, #-16]!\n"
  "  add x2, x0, #200\n  mov x1, #0\n  mov x0, #0\n  bl bs_sigprocmask\n"
  "  ldp x0, x30, [sp], #16\n"
  "1: mov x0, #0\n  ret\n"
  ".globl bs_longjmp\nbs_longjmp:\n"
  ".globl bs__longjmp\nbs__longjmp:\n"
  ".globl bs_siglongjmp\nbs_siglongjmp:\n"
  "  mov x19, x0\n  mov x20, x1\n"
  "  ldr x2, [x0, #192]\n  cbz x2, 2f\n"
  "  add x1, x0, #200\n  mov x2, #0\n  mov x0, #2\n  bl bs_sigprocmask\n"
  "2: mov x0, x19\n  mov x1, x20\n"
  "  ldp x19, x20, [x0, #0]\n  ldp x21, x22, [x0, #16]\n  ldp x23, x24, [x0, #32]\n"
  "  ldp x25, x26, [x0, #48]\n  ldp x27, x28, [x0, #64]\n  ldp x29, x30, [x0, #80]\n"
  "  ldr x2, [x0, #96]\n  mov sp, x2\n"
  "  ldp d8, d9, [x0, #104]\n  ldp d10, d11, [x0, #120]\n  ldp d12, d13, [x0, #136]\n  ldp d14, d15, [x0, #152]\n"
  "  cmp x1, #0\n  csinc x0, x1, xzr, ne\n  ret\n"
);
