// com/gmodvr/flat/JoinWorkshop replacement: download a server's required Workshop addons on join.
// Contract from GDroid JoinWorkshop.java: start(String reqs, long budget, long sharedMillis) -> Z,
// poll() -> "running|finished\tindex\tcount\tdone\ttotal\ttext\tready\tfailed\tchanged\tskipped\tallBytes", cancel().
// reqs = "id[:time:crc[:shared]],..."
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../shim/jni_fake.h"
#include "sw.h"

const char *ios_content_root(void);

typedef struct { uint64_t id, time, crc; int shared; } req_t;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
  int running, index, count; uint64_t done, total, all; char text[256];
  char ready[8192], failed[8192], changed[8192], skipped[8192];
  volatile int cancel; req_t *reqs; int nreq; uint64_t budget; pthread_t th; int has_th;
} J;

static void clean(char *s) { for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' '; }
static void app(char *dst, size_t dn, char sep, const char *s) { size_t l = strlen(dst); snprintf(dst + l, dn - l, "%s%s", l ? (char[]){sep, 0} : "", s); }
static void st(void *u, const char *t, uint64_t d, uint64_t tot) {
  (void)u; pthread_mutex_lock(&g_mu); snprintf(J.text, sizeof J.text, "%s", t); clean(J.text); J.done = d; J.total = tot; pthread_mutex_unlock(&g_mu);
}

static void *worker(void *a) {
  (void)a; const char *root = ios_content_root(); sw_progress p = {st, 0, &J.cancel};
  uint64_t used = 0;
  for (int i = 0; i < J.nreq && !J.cancel; i++) {
    req_t *r = &J.reqs[i]; char ids[32]; snprintf(ids, sizeof ids, "%llu", (unsigned long long)r->id);
    pthread_mutex_lock(&g_mu); J.index = i; pthread_mutex_unlock(&g_mu);
    if (sw_installed(root, r->id, r->time, r->crc, 1)) { pthread_mutex_lock(&g_mu); app(J.ready, sizeof J.ready, ',', ids); pthread_mutex_unlock(&g_mu); continue; }
    char err[512] = "";
    int rc = sw_install(root, r->id, 1, &p, err, sizeof err);
    if (!rc && r->crc && !sw_installed(root, r->id, 0, r->crc, 1)) { rc = -1; snprintf(err, sizeof err, "Downloaded addon does not match the server's archive; update the server's addon %s", ids); }
    pthread_mutex_lock(&g_mu);
    if (!rc) { app(J.ready, sizeof J.ready, ',', ids); app(J.changed, sizeof J.changed, ',', ids); used += J.total; }
    else { char e[700]; clean(err); for (char *c = err; *c; c++) if (*c == ';') *c = ','; snprintf(e, sizeof e, "%s:%.200s", ids, err);
      app(r->shared ? J.skipped : J.failed, sizeof J.failed, ';', e); }
    pthread_mutex_unlock(&g_mu);
    if (J.budget && used > J.budget) break;
  }
  pthread_mutex_lock(&g_mu); if (J.cancel) { for (int i = J.index; i < J.nreq; i++) { char e[64]; snprintf(e, sizeof e, "%llu:cancelled", (unsigned long long)J.reqs[i].id); if (!strstr(J.ready, e)) app(J.failed, sizeof J.failed, ';', e); } }
  J.running = 0; pthread_mutex_unlock(&g_mu);
  sw_log("[workshop] join sync finished ready=%s failed=%s", J.ready, J.failed);
  return NULL;
}

static jvalue j_start(void *env, void *thiz, jvalue *a) {
  (void)env; (void)thiz; jvalue r = {0}; const char *s = jni_str(a[0].l); if (!s) return r;
  sw_log("[workshop] JoinWorkshop.start %s", s);
  if (J.has_th) { J.cancel = 1; pthread_join(J.th, NULL); J.has_th = 0; }
  req_t *rq = NULL; int n = 0; char *dup = strdup(s), *save = NULL;
  for (char *t = strtok_r(dup, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
    req_t q = {0}; char *e; q.id = strtoull(t, &e, 10); if (!q.id) continue;
    if (*e == ':') { q.time = strtoull(e + 1, &e, 10); if (*e == ':') { q.crc = strtoull(e + 1, &e, 10); if (*e == ':') q.shared = e[1] == '1'; } }
    int d = 0; for (int i = 0; i < n; i++) d |= rq[i].id == q.id; if (d) continue;
    rq = realloc(rq, sizeof *rq * (n + 1)); rq[n++] = q;
  }
  free(dup); if (!n) { free(rq); return r; }
  pthread_mutex_lock(&g_mu); free(J.reqs); memset(&J, 0, sizeof J); J.reqs = rq; J.nreq = n; J.count = n; J.running = 1;
  J.budget = a[1].j > 0 ? (uint64_t)a[1].j : 0; snprintf(J.text, sizeof J.text, "Connecting to Steam…"); pthread_mutex_unlock(&g_mu);
  J.has_th = !pthread_create(&J.th, NULL, worker, NULL);
  r.z = (uint8_t)J.has_th; return r;
}
static jvalue j_poll(void *env, void *thiz, jvalue *a) {
  (void)env; (void)thiz; (void)a; static char b[40000]; pthread_mutex_lock(&g_mu);
  if (!J.reqs) snprintf(b, sizeof b, "finished\t0\t0\t0\t0\t\t\t");
  else { int fin = !J.running;
    snprintf(b, sizeof b, "%s\t%d\t%d\t%llu\t%llu\t%s\t%s\t%s\t%s\t%s\t%llu", fin ? "finished" : "running", J.index, J.count, (unsigned long long)J.done, (unsigned long long)J.total, J.text,
             fin ? J.ready : "", fin ? J.failed : "", fin ? J.changed : "", fin ? J.skipped : "", (unsigned long long)J.all); }
  pthread_mutex_unlock(&g_mu); jvalue r = {0}; r.l = jni_new_string(b); return r;
}
static jvalue j_cancel(void *env, void *thiz, jvalue *a) { (void)env; (void)thiz; (void)a; J.cancel = 1; jvalue r = {0}; return r; }

#define C "com/gmodvr/flat/JoinWorkshop"
static jni_impl_entry g_jw[] = {
  {C, "start", 0, j_start}, {C, "poll", 0, j_poll}, {C, "cancel", 0, j_cancel},
};
void workshop_jni_init(void) { jni_add_impls(g_jw, sizeof g_jw / sizeof *g_jw); }
