// bionic arm64 pthread sync object translation.
// bionic sizes: mutex 40, cond 48, rwlock 56, sem 16, attr 56, mutexattr/condattr 8 (long). Only 4-byte aligned.
// Host objects (glibc/Darwin) are larger and need 8-byte alignment, so each bionic object keeps a
// 32-bit handle (at byte offset 4) into a global table of heap-allocated host objects.
// Static initializers are all-zero except recursive/errorcheck mutex initializers (type in bits 14-15 of word 0).
#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif

#define L1 4096
#define L2 4096
static void **g_tab[L1];
static uint32_t g_next = 1;
static uint32_t *g_free; static size_t g_nfree, g_capfree;
static pthread_mutex_t g_tl = PTHREAD_MUTEX_INITIALIZER;

static void **slot(uint32_t h) { return &g_tab[h / L2][h % L2]; }
static uint32_t alloc_h(void *obj) {
  pthread_mutex_lock(&g_tl);
  uint32_t h = g_nfree ? g_free[--g_nfree] : g_next++;
  if (!g_tab[h / L2]) g_tab[h / L2] = calloc(L2, sizeof(void *));
  *slot(h) = obj;
  pthread_mutex_unlock(&g_tl);
  return h;
}
static void free_h(uint32_t h) {
  pthread_mutex_lock(&g_tl);
  *slot(h) = NULL;
  if (g_nfree == g_capfree) { g_capfree = g_capfree ? g_capfree * 2 : 256; g_free = realloc(g_free, g_capfree * 4); }
  g_free[g_nfree++] = h;
  pthread_mutex_unlock(&g_tl);
}
static inline void *get_h(void *b) {
  uint32_t h = __atomic_load_n((uint32_t *)b + 1, __ATOMIC_ACQUIRE);
  return h ? *slot(h) : NULL;
}
// lazily create; mk builds the host object from the bionic bytes
static void *lazy(void *b, void *(*mk)(void *)) {
  void *o = get_h(b); if (o) return o;
  o = mk(b);
  uint32_t h = alloc_h(o), exp = 0;
  if (!__atomic_compare_exchange_n((uint32_t *)b + 1, &exp, h, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { free_h(h); return NULL; }
  return o;
}
#define LAZY(b, mk, T) ({ T *_o = lazy(b, mk); if (!_o) _o = get_h(b); _o; })

/* ---------- mutex ---------- */
// bionic mutexattr: long; bits 0-1 type (0 normal,1 recursive,2 errorcheck), bit 2 pshared
static int host_type(int bt) { return bt == 1 ? PTHREAD_MUTEX_RECURSIVE : bt == 2 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL; }
static void *mk_mutex_type(int bt) {
  pthread_mutex_t *m = malloc(sizeof *m); pthread_mutexattr_t a; pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, host_type(bt)); pthread_mutex_init(m, &a); pthread_mutexattr_destroy(&a); return m;
}
static void *mk_mutex(void *b) { return mk_mutex_type((*(uint32_t *)b >> 14) & 3); }
#define MXH(b) LAZY(b, mk_mutex, pthread_mutex_t)

int bp_mutexattr_init(long *a) { *a = 0; return 0; }
int bp_mutexattr_destroy(long *a) { (void)a; return 0; }
int bp_mutexattr_settype(long *a, int t) { if (t < 0 || t > 2) return EINVAL; *a = (*a & ~3L) | t; return 0; }
int bp_mutexattr_gettype(const long *a, int *t) { *t = *a & 3; return 0; }
int bp_mutexattr_setpshared(long *a, int p) { (void)a; (void)p; return 0; }
int bp_mutex_init(void *b, const long *a) {
  int bt = a ? (int)(*a & 3) : 0;
  memset(b, 0, 40); *(uint32_t *)b = bt << 14;
  void *o = mk_mutex_type(bt); ((uint32_t *)b)[1] = alloc_h(o); return 0;
}
int bp_mutex_destroy(void *b) {
  uint32_t h = ((uint32_t *)b)[1]; if (!h) return 0;
  pthread_mutex_t *m = *slot(h); ((uint32_t *)b)[1] = 0; free_h(h);
  if (m) { pthread_mutex_destroy(m); free(m); } return 0;
}
int bp_mutex_lock(void *b) { return pthread_mutex_lock(MXH(b)); }
int bp_mutex_trylock(void *b) { return pthread_mutex_trylock(MXH(b)); }
int bp_mutex_unlock(void *b) { return pthread_mutex_unlock(MXH(b)); }
int bp_mutex_timedlock(void *b, const struct timespec *t) {
#ifdef __APPLE__
  for (;;) { int r = pthread_mutex_trylock(MXH(b)); if (r != EBUSY) return r;
    struct timespec n; clock_gettime(CLOCK_REALTIME, &n);
    if (n.tv_sec > t->tv_sec || (n.tv_sec == t->tv_sec && n.tv_nsec >= t->tv_nsec)) return ETIMEDOUT;
    struct timespec s = {0, 200000}; nanosleep(&s, NULL); }
#else
  return pthread_mutex_timedlock(MXH(b), t);
#endif
}

/* ---------- cond ---------- */
// bionic condattr: long; bit 0 pshared, bits 1+ clock id (stored as clock<<1)
typedef struct { pthread_cond_t c; int clock; } hcond;
static void *mk_cond_clock(int clk) {
  hcond *c = malloc(sizeof *c); c->clock = clk;
#ifdef __APPLE__
  c->clock = 0; pthread_cond_init(&c->c, NULL);
#else
  pthread_condattr_t a; pthread_condattr_init(&a);
  pthread_condattr_setclock(&a, clk == 1 ? CLOCK_MONOTONIC : CLOCK_REALTIME);
  pthread_cond_init(&c->c, &a); pthread_condattr_destroy(&a);
#endif
  return c;
}
static void *mk_cond(void *b) { (void)b; return mk_cond_clock(0); }
#define CVH(b) LAZY(b, mk_cond, hcond)
int bp_condattr_init(long *a) { *a = 0; return 0; }
int bp_condattr_destroy(long *a) { (void)a; return 0; }
int bp_condattr_setclock(long *a, int c) { *a = (*a & 1) | ((long)c << 1); return 0; }
int bp_condattr_getclock(const long *a, int *c) { *c = (int)(*a >> 1); return 0; }
int bp_condattr_setpshared(long *a, int p) { (void)a; (void)p; return 0; }
int bp_cond_init(void *b, const long *a) {
  memset(b, 0, 48); ((uint32_t *)b)[1] = alloc_h(mk_cond_clock(a ? (int)(*a >> 1) : 0)); return 0;
}
int bp_cond_destroy(void *b) {
  uint32_t h = ((uint32_t *)b)[1]; if (!h) return 0;
  hcond *c = *slot(h); ((uint32_t *)b)[1] = 0; free_h(h);
  if (c) { pthread_cond_destroy(&c->c); free(c); } return 0;
}
int bp_cond_signal(void *b) { return pthread_cond_signal(&CVH(b)->c); }
int bp_cond_broadcast(void *b) { return pthread_cond_broadcast(&CVH(b)->c); }
int bp_cond_wait(void *b, void *m) { return pthread_cond_wait(&CVH(b)->c, MXH(m)); }
static int cond_timed(hcond *c, pthread_mutex_t *m, const struct timespec *t, int clk) {
#if !defined(__GLIBC__)
  struct timespec abs = *t;
  if (clk != c->clock) { // convert monotonic absolute -> realtime absolute
    struct timespec mn, rt; clock_gettime(CLOCK_MONOTONIC, &mn); clock_gettime(CLOCK_REALTIME, &rt);
    long long d = (t->tv_sec - mn.tv_sec) * 1000000000LL + (t->tv_nsec - mn.tv_nsec);
    long long r = rt.tv_sec * 1000000000LL + rt.tv_nsec + (d > 0 ? d : 0);
    abs.tv_sec = r / 1000000000LL; abs.tv_nsec = r % 1000000000LL;
  }
  return pthread_cond_timedwait(&c->c, m, &abs);
#else
  if (clk == c->clock || (clk != 1 && c->clock != 1)) return pthread_cond_timedwait(&c->c, m, t);
  return pthread_cond_clockwait(&c->c, m, clk == 1 ? CLOCK_MONOTONIC : CLOCK_REALTIME, t);
#endif
}
int bp_cond_timedwait(void *b, void *m, const struct timespec *t) { hcond *c = CVH(b); return cond_timed(c, MXH(m), t, c->clock); }
int bp_cond_timedwait_monotonic_np(void *b, void *m, const struct timespec *t) { return cond_timed(CVH(b), MXH(m), t, 1); }
int bp_cond_timedwait_relative_np(void *b, void *m, const struct timespec *rel) {
  struct timespec n; clock_gettime(CLOCK_REALTIME, &n);
  n.tv_sec += rel->tv_sec; n.tv_nsec += rel->tv_nsec; if (n.tv_nsec >= 1000000000) { n.tv_sec++; n.tv_nsec -= 1000000000; }
  return cond_timed(CVH(b), MXH(m), &n, 0);
}
int bp_cond_clockwait(void *b, void *m, int clk, const struct timespec *t) { return cond_timed(CVH(b), MXH(m), t, clk == 1 || clk == 4 ? 1 : 0); }

/* ---------- rwlock ---------- */
static void *mk_rw(void *b) { (void)b; pthread_rwlock_t *r = malloc(sizeof *r); pthread_rwlock_init(r, NULL); return r; }
#define RWH(b) LAZY(b, mk_rw, pthread_rwlock_t)
int bp_rwlock_init(void *b, const void *a) { (void)a; memset(b, 0, 56); ((uint32_t *)b)[1] = alloc_h(mk_rw(b)); return 0; }
int bp_rwlock_destroy(void *b) {
  uint32_t h = ((uint32_t *)b)[1]; if (!h) return 0;
  pthread_rwlock_t *r = *slot(h); ((uint32_t *)b)[1] = 0; free_h(h);
  if (r) { pthread_rwlock_destroy(r); free(r); } return 0;
}
int bp_rwlock_rdlock(void *b) { return pthread_rwlock_rdlock(RWH(b)); }
int bp_rwlock_wrlock(void *b) { return pthread_rwlock_wrlock(RWH(b)); }
int bp_rwlock_tryrdlock(void *b) { return pthread_rwlock_tryrdlock(RWH(b)); }
int bp_rwlock_trywrlock(void *b) { return pthread_rwlock_trywrlock(RWH(b)); }
int bp_rwlock_unlock(void *b) { return pthread_rwlock_unlock(RWH(b)); }

/* ---------- semaphores (unnamed) ---------- */
// implemented on host mutex+cond so it works on Darwin (no unnamed sem_init there)
typedef struct { pthread_mutex_t m; pthread_cond_t c; unsigned v; } hsem;
static hsem *SEM(void *b) { return get_h(b); }
int bp_sem_init(void *b, int pshared, unsigned v) {
  (void)pshared; hsem *s = malloc(sizeof *s); pthread_mutex_init(&s->m, NULL); pthread_cond_init(&s->c, NULL); s->v = v;
  memset(b, 0, 16); ((uint32_t *)b)[1] = alloc_h(s); return 0;
}
int bp_sem_destroy(void *b) {
  uint32_t h = ((uint32_t *)b)[1]; if (!h) return 0; hsem *s = *slot(h); ((uint32_t *)b)[1] = 0; free_h(h);
  if (s) { pthread_mutex_destroy(&s->m); pthread_cond_destroy(&s->c); free(s); } return 0;
}
int bp_sem_post(void *b) { hsem *s = SEM(b); if (!s) { errno = EINVAL; return -1; }
  pthread_mutex_lock(&s->m); s->v++; pthread_cond_signal(&s->c); pthread_mutex_unlock(&s->m); return 0; }
int bp_sem_wait(void *b) { hsem *s = SEM(b); if (!s) { errno = EINVAL; return -1; }
  pthread_mutex_lock(&s->m); while (!s->v) pthread_cond_wait(&s->c, &s->m); s->v--; pthread_mutex_unlock(&s->m); return 0; }
int bp_sem_trywait(void *b) { hsem *s = SEM(b); if (!s) { errno = EINVAL; return -1; }
  pthread_mutex_lock(&s->m); int ok = s->v > 0; if (ok) s->v--; pthread_mutex_unlock(&s->m);
  if (!ok) { errno = EAGAIN; return -1; } return 0; }
int bp_sem_timedwait(void *b, const struct timespec *t) { hsem *s = SEM(b); if (!s) { errno = EINVAL; return -1; }
  int r = 0; pthread_mutex_lock(&s->m);
  while (!s->v && r != ETIMEDOUT) r = pthread_cond_timedwait(&s->c, &s->m, t);
  int ok = s->v > 0; if (ok) s->v--; pthread_mutex_unlock(&s->m);
  if (!ok) { errno = ETIMEDOUT; return -1; } return 0; }
int bp_sem_getvalue(void *b, int *v) { hsem *s = SEM(b); if (!s) { errno = EINVAL; return -1; } *v = (int)s->v; return 0; }

/* ---------- pthread_attr (bionic 56 bytes, 8-aligned): {u32 flags; void *stack_base; size_t stack_size; size_t guard; i32 policy; i32 prio} ---------- */
typedef struct { uint32_t flags; void *stack_base; size_t stack_size; size_t guard; int32_t policy, prio; } battr;
int bp_attr_init(battr *a) { memset(a, 0, sizeof *a); a->stack_size = 1 << 20; return 0; }
int bp_attr_destroy(battr *a) { (void)a; return 0; }
int bp_attr_setdetachstate(battr *a, int s) { if (s) a->flags |= 1; else a->flags &= ~1u; return 0; }
int bp_attr_getdetachstate(const battr *a, int *s) { *s = a->flags & 1; return 0; }
int bp_attr_setstacksize(battr *a, size_t s) { a->stack_size = s; return 0; }
int bp_attr_getstacksize(const battr *a, size_t *s) { *s = a->stack_size; return 0; }
int bp_attr_setguardsize(battr *a, size_t s) { a->guard = s; return 0; }
int bp_attr_setschedparam(battr *a, const void *p) { a->prio = *(const int *)p; return 0; }
int bp_attr_getschedparam(const battr *a, void *p) { *(int *)p = a->prio; return 0; }
int bp_attr_setschedpolicy(battr *a, int p) { a->policy = p; return 0; }
int bp_attr_setscope(battr *a, int s) { (void)a; (void)s; return 0; }
// convert for pthread_create
void bp_attr_to_host(const void *bv, pthread_attr_t *h) {
  const battr *b = bv; pthread_attr_init(h);
  if (!b) return;
  if (b->flags & 1) pthread_attr_setdetachstate(h, PTHREAD_CREATE_DETACHED);
  size_t s = b->stack_size; if (s < 256 * 1024) s = 256 * 1024; // engine threads assume Android 1MB-ish
  s = (s + 16383) & ~(size_t)16383;
  pthread_attr_setstacksize(h, s);
}

/* ---------- once (bionic pthread_once_t = int, 0 init) ---------- */
int bp_once(int *o, void (*fn)(void)) {
  // states: 0 init, 1 running, 2 done
  if (__atomic_load_n(o, __ATOMIC_ACQUIRE) == 2) return 0;
  int e = 0;
  if (__atomic_compare_exchange_n(o, &e, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { fn(); __atomic_store_n(o, 2, __ATOMIC_RELEASE); return 0; }
  while (__atomic_load_n(o, __ATOMIC_ACQUIRE) != 2) sched_yield();
  return 0;
}
