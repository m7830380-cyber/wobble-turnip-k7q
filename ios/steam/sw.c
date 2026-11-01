// Minimal Steam workshop client (anonymous CM over WebSocket + SteamPipe CDN).
// Protocol verified against ValvePython/steam + SteamKit2; install layout = GDroid GmodAddons (packed .gma).
#include "sw.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include "../dec/LzmaDec.h"
#define ZSTD_STATIC_LINKING_ONLY
size_t ZSTD_decompress(void *dst, size_t dstCapacity, const void *src, size_t compressedSize);
unsigned ZSTD_isError(size_t code);

#define ERR(...) do { snprintf(err, errn, __VA_ARGS__); sw_log("[steam] error: %s", err); return -1; } while (0)

void sw_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); }

// ---------------- protobuf ----------------
typedef struct { uint8_t *p; size_t n, cap; } buf;
static void bput(buf *b, const void *p, size_t n) {
  if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
  memcpy(b->p + b->n, p, n); b->n += n;
}
static void pvar(buf *b, uint64_t v) { uint8_t t[10]; int i = 0; do { t[i] = v & 0x7f; v >>= 7; if (v) t[i] |= 0x80; i++; } while (v); bput(b, t, i); }
static void fvar(buf *b, int f, uint64_t v) { pvar(b, (uint64_t)f << 3); pvar(b, v); }
static void ff64(buf *b, int f, uint64_t v) { pvar(b, (uint64_t)f << 3 | 1); bput(b, &v, 8); }
static void fbytes(buf *b, int f, const void *p, size_t n) { pvar(b, (uint64_t)f << 3 | 2); pvar(b, n); bput(b, p, n); }
static void fstr(buf *b, int f, const char *s) { fbytes(b, f, s, strlen(s)); }

typedef struct { const uint8_t *p, *e; int f, wt; uint64_t v; const uint8_t *d; size_t dn; } pbr;
static int rvar(pbr *r, uint64_t *v) { uint64_t x = 0; int s = 0; while (r->p < r->e) { uint8_t c = *r->p++; x |= (uint64_t)(c & 0x7f) << s; s += 7; if (!(c & 0x80)) { *v = x; return 1; } if (s > 63) return 0; } return 0; }
static void pb_init(pbr *r, const void *p, size_t n) { r->p = p; r->e = (const uint8_t *)p + n; }
static int pb_next(pbr *r) {
  uint64_t k; if (r->p >= r->e || !rvar(r, &k)) return 0;
  r->f = (int)(k >> 3); r->wt = k & 7; r->d = NULL; r->dn = 0; r->v = 0;
  switch (r->wt) {
    case 0: return rvar(r, &r->v);
    case 1: if (r->e - r->p < 8) return 0; memcpy(&r->v, r->p, 8); r->p += 8; return 1;
    case 5: { uint32_t t; if (r->e - r->p < 4) return 0; memcpy(&t, r->p, 4); r->v = t; r->p += 4; return 1; }
    case 2: { uint64_t n; if (!rvar(r, &n) || n > (uint64_t)(r->e - r->p)) return 0; r->d = r->p; r->dn = n; r->p += n; return 1; }
  }
  return 0;
}
static void pb_str(pbr *r, char *out, size_t on) { size_t n = r->dn < on - 1 ? r->dn : on - 1; memcpy(out, r->d, n); out[n] = 0; }

// ---------------- CM connection ----------------
typedef struct { sw_ws *ws; uint64_t sid; int32_t sess; uint64_t job; time_t last; } cm_t;
static cm_t g_cm;

typedef struct { uint32_t emsg; uint64_t jobt; int eres; uint8_t *body; size_t bn; uint8_t *raw; } cmmsg;
#define QMAX 64
static cmmsg g_q[QMAX]; static int g_qn;

static void cm_close(void) { if (g_cm.ws) sw_ws_close(g_cm.ws); memset(&g_cm, 0, sizeof g_cm); for (int i = 0; i < g_qn; i++) free(g_q[i].raw); g_qn = 0; }

static int cm_send(uint32_t emsg, buf *body, buf *xh) {
  buf h = {0}; ff64(&h, 1, g_cm.sid); fvar(&h, 2, (uint64_t)(int64_t)g_cm.sess); if (xh && xh->n) bput(&h, xh->p, xh->n);
  buf m = {0}; uint32_t hd[2] = {emsg | 0x80000000u, (uint32_t)h.n}; bput(&m, hd, 8); bput(&m, h.p, h.n); if (body && body->n) bput(&m, body->p, body->n);
  int r = sw_ws_send(g_cm.ws, m.p, m.n); free(h.p); free(m.p); g_cm.last = time(NULL); return r;
}
static void cm_dispatch(const uint8_t *d, size_t n, int depth) {
  if (n < 8 || depth > 3) return;
  uint32_t e, hl; memcpy(&e, d, 4); memcpy(&hl, d + 4, 4);
  if (!(e & 0x80000000u) || 8 + (size_t)hl > n) return;
  e &= 0x7fffffff;
  const uint8_t *body = d + 8 + hl; size_t bn = n - 8 - hl;
  if (e == 1) { // Multi
    pbr r; pb_init(&r, body, bn); uint64_t unz = 0; const uint8_t *mb = NULL; size_t mn = 0;
    while (pb_next(&r)) { if (r.f == 1) unz = r.v; else if (r.f == 2) { mb = r.d; mn = r.dn; } }
    if (!mb) return; uint8_t *tmp = NULL;
    if (unz) {
      tmp = malloc(unz); z_stream z = {0}; inflateInit2(&z, 16 + MAX_WBITS);
      z.next_in = (Bytef *)mb; z.avail_in = (uInt)mn; z.next_out = tmp; z.avail_out = (uInt)unz;
      inflate(&z, Z_FINISH); mn = z.total_out; inflateEnd(&z); mb = tmp;
    }
    for (size_t i = 0; i + 4 <= mn;) { uint32_t l; memcpy(&l, mb + i, 4); if (i + 4 + l > mn) break; cm_dispatch(mb + i + 4, l, depth + 1); i += 4 + l; }
    free(tmp); return;
  }
  cmmsg m = {0}; m.emsg = e; m.eres = 1;
  pbr r; pb_init(&r, d + 8, hl);
  while (pb_next(&r)) {
    if (r.f == 1 && e == 751) g_cm.sid = r.v;
    else if (r.f == 2 && e == 751) g_cm.sess = (int32_t)r.v;
    else if (r.f == 11) m.jobt = r.v;
    else if (r.f == 13) m.eres = (int)(int32_t)r.v;
  }
  if (g_qn >= QMAX) { free(g_q[0].raw); memmove(g_q, g_q + 1, sizeof(cmmsg) * (QMAX - 1)); g_qn--; }
  m.raw = malloc(bn + 1); memcpy(m.raw, body, bn); m.body = m.raw; m.bn = bn; g_q[g_qn++] = m;
}
static int cm_wait(uint32_t emsg, uint64_t job, cmmsg *out, int timeout_s, volatile int *cancel) {
  time_t t0 = time(NULL);
  for (;;) {
    for (int i = 0; i < g_qn; i++)
      if (g_q[i].emsg == emsg && (!job || g_q[i].jobt == job)) { *out = g_q[i]; memmove(g_q + i, g_q + i + 1, sizeof(cmmsg) * (g_qn - i - 1)); g_qn--; return 0; }
    for (int i = 0; i < g_qn; i++) if (g_q[i].emsg == 757) return -2; // ClientLoggedOff
    if (cancel && *cancel) return -3;
    if (time(NULL) - t0 > timeout_s) return -1;
    uint8_t *d; size_t n; int r = sw_ws_recv(g_cm.ws, &d, &n, 1000);
    if (r < 0) return -4;
    if (r == 0) { cm_dispatch(d, n, 0); free(d); }
  }
}

static int cm_connect(char *err, size_t errn, volatile int *cancel) {
  if (g_cm.ws && time(NULL) - g_cm.last < 20) return 0;
  cm_close();
  uint8_t *j; size_t jn;
  int st = sw_http_get("https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&cmtype=websockets", &j, &jn, cancel, 0, 0);
  if (st != 200) ERR("Steam directory unreachable (HTTP %d). Check the internet connection.", st);
  char *s = realloc(j, jn + 1); s[jn] = 0; char *p = s; int tries = 0; char e2[256] = "";
  while ((p = strstr(p, "\"endpoint\":\"")) && tries < 5) {
    p += 12; char *q = strchr(p, '"'); if (!q) break;
    char url[300]; snprintf(url, sizeof url, "wss://%.*s/cmsocket/", (int)(q - p), p); p = q; tries++;
    g_cm.ws = sw_ws_open(url, e2, sizeof e2);
    if (!g_cm.ws) { sw_log("[steam] %s: %s", url, e2); continue; }
    g_cm.sid = (1ULL << 56) | (10ULL << 52); g_cm.sess = 0; g_cm.job = 1;
    buf b = {0}; fvar(&b, 1, 65580); fvar(&b, 7, 16); fvar(&b, 5, 1771); // protocol, os, package
    cm_send(5514, &b, NULL); free(b.p);
    cmmsg m; int r = cm_wait(751, 0, &m, 20, cancel);
    if (r) { cm_close(); snprintf(e2, sizeof e2, "logon timeout"); continue; }
    pbr rr; pb_init(&rr, m.body, m.bn); int eres = 2; while (pb_next(&rr)) if (rr.f == 1) eres = (int)rr.v;
    free(m.raw);
    if (eres != 1) { cm_close(); snprintf(e2, sizeof e2, "anonymous logon refused (EResult %d)", eres); continue; }
    sw_log("[steam] connected %s anon sid %llx", url, (unsigned long long)g_cm.sid); free(s); return 0;
  }
  free(s); ERR("Could not connect to Steam: %s", e2[0] ? e2 : "no servers");
}
static int cm_call(const char *method, buf *body, cmmsg *out, char *err, size_t errn, volatile int *cancel) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (cm_connect(err, errn, cancel)) return -1;
    uint64_t job = g_cm.job++; buf h = {0}; ff64(&h, 10, job); fstr(&h, 12, method);
    int r = cm_send(151, body, &h); free(h.p);
    if (!r) r = cm_wait(147, job, out, 30, cancel);
    if (!r) { if (out->eres != 1) { int e = out->eres; free(out->raw); ERR("Steam %s failed (EResult %d)", method, e); } return 0; }
    if (r == -3) ERR("cancelled");
    cm_close();
  }
  ERR("Steam connection lost during %s", method);
}

// ---------------- details ----------------
static void parse_details(const uint8_t *d, size_t n, sw_details *o) {
  memset(o, 0, sizeof *o); pbr r; pb_init(&r, d, n);
  while (pb_next(&r)) switch (r.f) {
    case 1: o->result = (int)r.v; break;
    case 2: o->id = r.v; break;
    case 5: o->app = (uint32_t)r.v; break;
    case 7: pb_str(&r, o->filename, sizeof o->filename); break;
    case 8: o->file_size = r.v; break;
    case 10: pb_str(&r, o->file_url, sizeof o->file_url); break;
    case 14: o->hcontent = r.v; break;
    case 16: pb_str(&r, o->title, sizeof o->title); break;
    case 20: o->time_updated = (uint32_t)r.v; break;
    case 34: o->file_type = (uint32_t)r.v; break;
    case 52: if (o->ntags < 16 && r.d) { pbr t; pb_init(&t, r.d, r.dn); while (pb_next(&t)) if (t.f == 1) pb_str(&t, o->tags[o->ntags], 64); o->ntags++; } break;
    case 53: if (r.d) { pbr t; pb_init(&t, r.d, r.dn); while (pb_next(&t)) if (t.f == 1) { o->children = realloc(o->children, 8 * (o->nchildren + 1)); o->children[o->nchildren++] = t.v; } } break;
  }
}
static int get_details(const uint64_t *ids, int n, sw_details *out, char *err, size_t errn, volatile int *cancel) {
  buf b = {0}; for (int i = 0; i < n; i++) ff64(&b, 1, ids[i]);
  fvar(&b, 2, 1); fvar(&b, 4, 1); fvar(&b, 8, 1);
  cmmsg m; int r = cm_call("PublishedFile.GetDetails#1", &b, &m, err, errn, cancel); free(b.p); if (r) return -1;
  pbr rr; pb_init(&rr, m.body, m.bn); int k = 0;
  while (pb_next(&rr)) if (rr.f == 1 && k < n) parse_details(rr.d, rr.dn, &out[k++]);
  free(m.raw);
  for (; k < n; k++) { memset(&out[k], 0, sizeof out[k]); out[k].id = ids[k]; out[k].result = 9; }
  return 0;
}

int sw_resolve(const char *spec, uint64_t **ids, int *n, sw_progress *p, char *err, size_t errn) {
  uint64_t *in = NULL; int nin = 0;
  for (const char *s = spec; *s;) {
    const char *id = strstr(s, "id="); const char *d = s;
    while (*d && !(*d >= '0' && *d <= '9')) { if (id && d == id) break; d++; }
    if (id && id <= d) d = id + 3;
    if (!*d) break;
    char *e; unsigned long long v = strtoull(d, &e, 10); if (e == d) { s = d + 1; continue; }
    if (v > 1000) { in = realloc(in, 8 * (nin + 1)); in[nin++] = v; }
    s = e;
  }
  if (!nin) ERR("No Workshop IDs or links found");
  *ids = NULL; *n = 0;
  for (int depth = 0; depth < 3 && nin; depth++) {
    if (p && p->status) p->status(p->ud, "Reading Workshop details…", 0, 0);
    sw_details *det = calloc(nin, sizeof *det);
    uint64_t *next = NULL; int nn = 0;
    for (int i = 0; i < nin; i += 100) {
      int c = nin - i < 100 ? nin - i : 100;
      if (get_details(in + i, c, det + i, err, errn, p ? p->cancel : NULL)) { free(det); free(in); return -1; }
    }
    for (int i = 0; i < nin; i++) {
      if (det[i].file_type == 2) { for (int c = 0; c < det[i].nchildren; c++) { next = realloc(next, 8 * (nn + 1)); next[nn++] = det[i].children[c]; } }
      else { int dup = 0; for (int k = 0; k < *n; k++) dup |= (*ids)[k] == in[i]; if (!dup) { *ids = realloc(*ids, 8 * (*n + 1)); (*ids)[(*n)++] = in[i]; } }
      free(det[i].children);
    }
    free(det); free(in); in = next; nin = nn;
  }
  free(in);
  if (!*n) ERR("The collection has no items");
  return 0;
}

// ---------------- crypto / codecs ----------------
static int symdec(const uint8_t key[32], const uint8_t *d, size_t n, uint8_t **out, size_t *on) {
  if (n < 32 || (n - 16) % 16) return -1;
  uint8_t iv[16]; if (sw_aes_ecb_dec(key, d, iv)) return -1;
  uint8_t *o = malloc(n - 16); if (sw_aes_cbc_dec(key, iv, d + 16, n - 16, o)) { free(o); return -1; }
  size_t l = n - 16; uint8_t pad = o[l - 1]; if (!pad || pad > 16 || pad > l) { free(o); return -1; }
  *out = o; *on = l - pad; return 0;
}
static void *lz_alloc(ISzAllocPtr p, size_t s) { (void)p; return malloc(s); }
static void lz_free(ISzAllocPtr p, void *a) { (void)p; free(a); }
static const ISzAlloc g_lza = {lz_alloc, lz_free};
static int lzma_raw(const uint8_t *props, const uint8_t *src, size_t sn, uint8_t *dst, size_t dn) {
  SizeT dl = dn, sl = sn; ELzmaStatus st;
  SRes r = LzmaDecode(dst, &dl, src, &sl, props, 5, LZMA_FINISH_ANY, &st, &g_lza);
  return (r == SZ_OK && dl == dn) ? 0 : -1;
}
static int unzip1(const uint8_t *z, size_t n, uint8_t **out, size_t *on) {
  // central directory -> first entry
  if (n < 22) return -1; ssize_t e = -1;
  for (ssize_t i = (ssize_t)n - 22; i >= 0 && i >= (ssize_t)n - 65557; i--) if (!memcmp(z + i, "PK\x05\x06", 4)) { e = i; break; }
  if (e < 0) return -1;
  uint32_t cd; memcpy(&cd, z + e + 16, 4); if (cd + 46 > n || memcmp(z + cd, "PK\x01\x02", 4)) return -1;
  uint16_t meth; uint32_t cs, us, lo; uint16_t ln, le;
  memcpy(&meth, z + cd + 10, 2); memcpy(&cs, z + cd + 20, 4); memcpy(&us, z + cd + 24, 4); memcpy(&lo, z + cd + 42, 4);
  if (lo + 30 > n) return -1; memcpy(&ln, z + lo + 26, 2); memcpy(&le, z + lo + 28, 2);
  size_t ds = lo + 30 + ln + le; if (ds + cs > n) return -1;
  uint8_t *o = malloc(us ? us : 1);
  if (meth == 0) memcpy(o, z + ds, us);
  else if (meth == 8) {
    z_stream s = {0}; inflateInit2(&s, -MAX_WBITS); s.next_in = (Bytef *)z + ds; s.avail_in = cs; s.next_out = o; s.avail_out = us;
    int r = inflate(&s, Z_FINISH); inflateEnd(&s); if (r != Z_STREAM_END || s.total_out != us) { free(o); return -1; }
  } else if (meth == 14) {
    if (cs < 9 || lzma_raw(z + ds + 4, z + ds + 9, cs - 9, o, us)) { free(o); return -1; }
  } else { free(o); return -1; }
  *out = o; *on = us; return 0;
}
static int chunk_decode(const uint8_t *d, size_t n, uint8_t *dst, size_t dn) {
  if (n >= 23 && !memcmp(d, "VSZa", 4)) {
    uint32_t sz; memcpy(&sz, d + n - 11, 4); if (sz != dn) return -1;
    size_t r = ZSTD_decompress(dst, dn, d + 8, n - 23); return (ZSTD_isError(r) || r != dn) ? -1 : 0;
  }
  if (n >= 22 && !memcmp(d, "VZa", 3)) {
    uint32_t sz; memcpy(&sz, d + n - 6, 4); if (sz != dn) return -1;
    return lzma_raw(d + 7, d + 12, n - 22, dst, dn);
  }
  if (n >= 4 && !memcmp(d, "PK\x03\x04", 4)) {
    uint8_t *o; size_t on; if (unzip1(d, n, &o, &on)) return -1; int ok = on == dn; if (ok) memcpy(dst, o, dn); free(o); return ok ? 0 : -1;
  }
  return -1;
}
static int b64dec(const char *s, uint8_t *o) {
  int n = 0, v = 0, bits = 0;
  for (; *s; s++) {
    int c = *s; int x = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
    if (x < 0) continue; v = v << 6 | x; bits += 6; if (bits >= 8) { bits -= 8; o[n++] = (uint8_t)(v >> bits); }
  }
  return n;
}

// ---------------- filesystem helpers ----------------
static void mkdirs(const char *p) { char t[2048]; snprintf(t, sizeof t, "%s", p); for (char *s = t + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(t, 0755); *s = '/'; } mkdir(t, 0755); }
static void rmtree(const char *p) {
  struct stat st; if (lstat(p, &st)) return;
  if (S_ISDIR(st.st_mode)) { DIR *d = opendir(p); struct dirent *e; if (d) { while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; char c[2048]; snprintf(c, sizeof c, "%s/%s", p, e->d_name); rmtree(c); } closedir(d); } rmdir(p); }
  else unlink(p);
}
static void jesc(FILE *f, const char *s) {
  fputc('"', f);
  for (; *s; s++) { unsigned char c = *s; if (c == '"' || c == '\\') fprintf(f, "\\%c", c); else if (c < 0x20) fprintf(f, "\\u%04x", c); else fputc(c, f); }
  fputc('"', f);
}

// GMA header info (GmaArchive.readInfo)
typedef struct { char title[256], author[256], desc[1024], type[64]; char tags[8][64]; int ntags; uint64_t ts; uint32_t crc; } gma_info;
static int rd_cstr(FILE *f, char *o, size_t on) { size_t i = 0; int c; while ((c = fgetc(f)) > 0) if (i + 1 < on) o[i++] = (char)c; o[i] = 0; return c == 0 ? 0 : -1; }
static void json_str(const char *j, const char *key, char *o, size_t on) {
  o[0] = 0; char k[80]; snprintf(k, sizeof k, "\"%s\"", key); const char *p = strstr(j, k); if (!p) return;
  p = strchr(p + strlen(k), '"'); if (!p) return; p++; size_t i = 0;
  while (*p && *p != '"' && i + 1 < on) { if (*p == '\\' && p[1]) { p++; o[i++] = *p == 'n' ? '\n' : *p; p++; continue; } o[i++] = *p++; }
  o[i] = 0;
}
static int gma_read(const char *path, gma_info *g) {
  memset(g, 0, sizeof *g); FILE *f = fopen(path, "rb"); if (!f) return -1;
  char mg[4]; uint8_t ver; uint64_t sid; char tmp[8192];
  if (fread(mg, 1, 4, f) != 4 || memcmp(mg, "GMAD", 4) || fread(&ver, 1, 1, f) != 1 || fread(&sid, 8, 1, f) != 1 || fread(&g->ts, 8, 1, f) != 1) { fclose(f); return -1; }
  if (ver > 1) while (!rd_cstr(f, tmp, sizeof tmp) && tmp[0]) {}
  rd_cstr(f, g->title, sizeof g->title); rd_cstr(f, tmp, sizeof tmp); rd_cstr(f, g->author, sizeof g->author);
  int32_t av; fread(&av, 4, 1, f);
  if (tmp[0] == '{') {
    json_str(tmp, "description", g->desc, sizeof g->desc); json_str(tmp, "type", g->type, sizeof g->type);
    const char *t = strstr(tmp, "\"tags\""); if (t && (t = strchr(t, '['))) {
      const char *e = strchr(t, ']');
      while (t && e && (t = strchr(t + 1, '"')) && t < e && g->ntags < 8) { const char *q = strchr(t + 1, '"'); if (!q) break; snprintf(g->tags[g->ntags++], 64, "%.*s", (int)(q - t - 1), t + 1); t = q; }
    }
    for (char *c = g->type; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
  } else snprintf(g->desc, sizeof g->desc, "%s", tmp);
  if (!strcasecmp(g->author, "author") || !strcasecmp(g->author, "author name") || !strcasecmp(g->author, "no author provided")) g->author[0] = 0;
  if (!strcasecmp(g->desc, "description") || !strcasecmp(g->desc, "no description provided")) g->desc[0] = 0;
  uint64_t dlen = 0;
  for (;;) { uint32_t idx; if (fread(&idx, 4, 1, f) != 1) { fclose(f); return -1; } if (!idx) break; rd_cstr(f, tmp, sizeof tmp); int64_t sz; uint32_t c; fread(&sz, 8, 1, f); fread(&c, 4, 1, f); dlen += (uint64_t)sz; }
  long ds = ftell(f); fseek(f, 0, SEEK_END); long fl = ftell(f);
  uint32_t stored = 0; if ((uint64_t)fl >= ds + dlen + 4) { fseek(f, (long)(ds + dlen), SEEK_SET); fread(&stored, 4, 1, f); }
  if (stored) g->crc = stored;
  else { fseek(f, 0, SEEK_SET); uLong c = crc32(0, 0, 0); uint64_t left = ds + dlen; while (left) { size_t k = fread(tmp, 1, left < sizeof tmp ? left : sizeof tmp, f); if (!k) break; c = crc32(c, (Bytef *)tmp, (uInt)k); left -= k; } g->crc = (uint32_t)c; }
  fclose(f); return 0;
}
static uint32_t file_crc(const char *p) { FILE *f = fopen(p, "rb"); if (!f) return 0; uLong c = crc32(0, 0, 0); char b[65536]; size_t k; while ((k = fread(b, 1, sizeof b, f))) c = crc32(c, (Bytef *)b, (uInt)k); fclose(f); return (uint32_t)c; }
static void be32(FILE *f, uint32_t v) { uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v}; fwrite(b, 1, 4, f); }
static void be64(FILE *f, uint64_t v) { be32(f, (uint32_t)(v >> 32)); be32(f, (uint32_t)v); }

// Decompress legacy LZMA-packed UGC (Source "LZMA" header or lzma-alone) into a .gma
static int unlzma_file(const char *src, const char *dst) {
  FILE *f = fopen(src, "rb"); if (!f) return -1; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *d = malloc(n); if (fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return -1; } fclose(f);
  const uint8_t *props, *data; uint64_t us; size_t dn;
  if (n > 17 && !memcmp(d, "LZMA", 4)) { uint32_t a; memcpy(&a, d + 4, 4); us = a; props = d + 12; data = d + 17; dn = n - 17; }
  else if (n > 13 && d[0] == 0x5d) { memcpy(&us, d + 5, 8); props = d; data = d + 13; dn = n - 13; }
  else { free(d); return -1; }
  if (us > (1ULL << 32)) { free(d); return -1; }
  uint8_t *o = malloc(us ? us : 1); int r = lzma_raw(props, data, dn, o, us);
  if (!r) { FILE *w = fopen(dst, "wb"); r = (w && fwrite(o, 1, us, w) == us) ? 0 : -1; if (w) fclose(w); }
  free(d); free(o); return r;
}

// ---------------- download ----------------
typedef struct { sw_progress *p; uint64_t base, total; const char *title; } dlprog;

static int cdn_hosts(char hosts[][256], int max, char *err, size_t errn, volatile int *cancel) {
  buf b = {0}; fvar(&b, 1, 0); fvar(&b, 2, 20); cmmsg m;
  if (cm_call("ContentServerDirectory.GetServersForSteamPipe#1", &b, &m, err, errn, cancel)) { free(b.p); return -1; } free(b.p);
  int n = 0; pbr r; pb_init(&r, m.body, m.bn);
  while (pb_next(&r)) if (r.f == 1 && r.d && n < max) {
    pbr s; pb_init(&s, r.d, r.dn); char host[256] = "", https[32] = "", type[32] = ""; int proxy = 0, china = 0;
    while (pb_next(&s)) { if (s.f == 8) pb_str(&s, host, sizeof host); else if (s.f == 12) pb_str(&s, https, sizeof https); else if (s.f == 1) pb_str(&s, type, sizeof type); else if (s.f == 10) proxy = (int)s.v; else if (s.f == 7) china = (int)s.v; }
    if (host[0] && !proxy && !china && (!strcmp(https, "mandatory") || !strcmp(https, "optional"))) snprintf(hosts[n++], 256, "%s", host);
  }
  free(m.raw);
  if (!n) ERR("No Steam content servers available");
  return 0;
}

static int download_manifest_item(sw_details *d, const char *rawdir, sw_progress *p, char *err, size_t errn) {
  volatile int *cancel = p ? p->cancel : NULL;
  uint32_t app = d->app, depot = d->app;
  // depot key
  uint8_t key[32]; {
    if (cm_connect(err, errn, cancel)) return -1;
    uint64_t job = g_cm.job++; buf b = {0}, h = {0}; fvar(&b, 1, depot); fvar(&b, 2, app); ff64(&h, 10, job);
    cm_send(5438, &b, &h); free(b.p); free(h.p); cmmsg m;
    if (cm_wait(5439, job, &m, 30, cancel)) ERR("Steam did not answer the depot key request");
    pbr r; pb_init(&r, m.body, m.bn); int eres = 2; size_t kl = 0;
    while (pb_next(&r)) { if (r.f == 1) eres = (int)r.v; else if (r.f == 3 && r.dn == 32) { memcpy(key, r.d, 32); kl = 32; } }
    free(m.raw);
    if (eres != 1 || kl != 32) ERR("Steam denied depot %u (EResult %d). This item may need a signed-in account.", depot, eres);
  }
  uint64_t code; {
    buf b = {0}; fvar(&b, 1, app); fvar(&b, 2, depot); fvar(&b, 3, d->hcontent); cmmsg m;
    if (cm_call("ContentServerDirectory.GetManifestRequestCode#1", &b, &m, err, errn, cancel)) { free(b.p); return -1; } free(b.p);
    pbr r; pb_init(&r, m.body, m.bn); code = 0; while (pb_next(&r)) if (r.f == 1) code = r.v; free(m.raw);
  }
  char hosts[20][256]; int nh = 0; { if (cdn_hosts(hosts, 20, err, errn, cancel)) return -1; for (nh = 0; nh < 20 && hosts[nh][0]; nh++) {} }
  // manifest
  uint8_t *z = NULL; size_t zn = 0; int hi = 0, st = 0;
  for (int t = 0; t < 4; t++) {
    char url[512]; snprintf(url, sizeof url, "https://%s/depot/%u/manifest/%llu/5/%llu", hosts[hi], depot, (unsigned long long)d->hcontent, (unsigned long long)code);
    st = sw_http_get(url, &z, &zn, cancel, 0, 0); if (st == 200) break; free(z); z = NULL; hi = (hi + 1) % nh;
  }
  if (!z) ERR("Manifest download failed (HTTP %d)", st);
  uint8_t *m; size_t mn; if (unzip1(z, zn, &m, &mn)) { free(z); ERR("Bad manifest archive"); } free(z);
  uint32_t mg, pl; if (mn < 8) { free(m); ERR("Short manifest"); } memcpy(&mg, m, 4); memcpy(&pl, m + 4, 4);
  if (mg != 0x71F617D0 || 8 + (size_t)pl > mn) { free(m); ERR("Unknown manifest format %08x", mg); }
  int enc = 0; if (16 + (size_t)pl <= mn) { uint32_t mg2, l2; memcpy(&mg2, m + 8 + pl, 4); memcpy(&l2, m + 12 + pl, 4); if (mg2 == 0x1F4812BE && 16 + pl + l2 <= mn) { pbr r; pb_init(&r, m + 16 + pl, l2); while (pb_next(&r)) if (r.f == 4) enc = (int)r.v; } }
  // totals
  uint64_t total = 0, done = 0; { pbr r; pb_init(&r, m + 8, pl); while (pb_next(&r)) if (r.f == 1) { pbr f; pb_init(&f, r.d, r.dn); while (pb_next(&f)) if (f.f == 2) total += f.v; } }
  pbr r; pb_init(&r, m + 8, pl); int nfiles = 0;
  while (pb_next(&r)) {
    if (r.f != 1) continue;
    char name[1024] = ""; uint64_t size = 0; uint32_t flags = 0;
    pbr f; pb_init(&f, r.d, r.dn);
    while (pb_next(&f)) { if (f.f == 1) pb_str(&f, name, sizeof name); else if (f.f == 2) size = f.v; else if (f.f == 3) flags = (uint32_t)f.v; }
    if (enc) { uint8_t eb[1024]; int bl = b64dec(name, eb); uint8_t *pn; size_t pnl; if (symdec(key, eb, bl, &pn, &pnl)) { free(m); ERR("Could not decrypt file names"); } snprintf(name, sizeof name, "%.*s", (int)pnl, pn); free(pn); }
    for (char *c = name; *c; c++) if (*c == '\\') *c = '/';
    if (flags & 64) continue; // directory
    if (strstr(name, "..") || name[0] == '/') { free(m); ERR("Unsafe path in Workshop item"); }
    char out[2048]; snprintf(out, sizeof out, "%s/%s", rawdir, name);
    char *sl = strrchr(out, '/'); *sl = 0; mkdirs(out); *sl = '/';
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) { free(m); ERR("Cannot write %s: %s", name, strerror(errno)); }
    if (ftruncate(fd, (off_t)size)) {}
    pb_init(&f, r.d, r.dn);
    while (pb_next(&f)) {
      if (f.f != 6) continue;
      uint8_t sha[20] = {0}; uint32_t adl = 0; uint64_t off = 0; uint32_t ul = 0;
      pbr c; pb_init(&c, f.d, f.dn);
      while (pb_next(&c)) { if (c.f == 1 && c.dn == 20) memcpy(sha, c.d, 20); else if (c.f == 2) adl = (uint32_t)c.v; else if (c.f == 3) off = c.v; else if (c.f == 4) ul = (uint32_t)c.v; }
      char hex[41]; for (int i = 0; i < 20; i++) sprintf(hex + 2 * i, "%02x", sha[i]);
      uint8_t *dst = malloc(ul ? ul : 1); int ok = 0; char why[160] = "";
      for (int t = 0; t < 5 && !ok; t++) {
        if (cancel && *cancel) { free(dst); close(fd); free(m); ERR("cancelled"); }
        char url[512]; snprintf(url, sizeof url, "https://%s/depot/%u/chunk/%s", hosts[hi], depot, hex);
        uint8_t *raw; size_t rn; int s2 = sw_http_get(url, &raw, &rn, cancel, 0, 0);
        if (s2 != 200) { snprintf(why, sizeof why, "HTTP %d", s2); if (s2 >= 0) free(raw); hi = (hi + 1) % nh; continue; }
        uint8_t *pt; size_t ptn; if (symdec(key, raw, rn, &pt, &ptn)) { free(raw); snprintf(why, sizeof why, "decrypt"); continue; } free(raw);
        if (chunk_decode(pt, ptn, dst, ul)) { snprintf(why, sizeof why, "decompress %.4s", pt); free(pt); continue; } free(pt);
        if ((uint32_t)adler32(0, dst, ul) != adl) { snprintf(why, sizeof why, "checksum"); continue; }
        ok = 1;
      }
      if (!ok) { free(dst); close(fd); free(m); ERR("Chunk %s failed: %s", hex, why); }
      if (pwrite(fd, dst, ul, (off_t)off) != (ssize_t)ul) { free(dst); close(fd); free(m); ERR("Write failed: %s", strerror(errno)); }
      free(dst); done += ul;
      if (p && p->status) { char t[400]; snprintf(t, sizeof t, "Downloading Workshop: %s", d->title); p->status(p->ud, t, done, total); }
    }
    close(fd); nfiles++;
  }
  free(m);
  if (!nfiles) ERR("Workshop item has no downloadable files");
  return 0;
}

static int collect_gmas(const char *dir, char list[][2048], int *n, int max) {
  DIR *d = opendir(dir); if (!d) return 0; struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue; char p[2048]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st; if (lstat(p, &st)) continue;
    if (S_ISDIR(st.st_mode)) { collect_gmas(p, list, n, max); continue; }
    if (!S_ISREG(st.st_mode) || *n >= max) continue;
    FILE *f = fopen(p, "rb"); uint8_t mg[4] = {0}; if (f) { fread(mg, 1, 4, f); fclose(f); }
    if (!memcmp(mg, "GMAD", 4)) snprintf(list[(*n)++], 2048, "%s", p);
    else if (!memcmp(mg, "LZMA", 4) || mg[0] == 0x5d) { char g[2060]; snprintf(g, sizeof g, "%s.unpacked", p); if (!unlzma_file(p, g)) { unlink(p); FILE *t = fopen(g, "rb"); uint8_t m2[4] = {0}; if (t) { fread(m2, 1, 4, t); fclose(t); } if (!memcmp(m2, "GMAD", 4)) snprintf(list[(*n)++], 2048, "%s", g); } }
  }
  closedir(d); return 0;
}

static void addon_dir(const char *root, uint64_t id, int join, char *o, size_t on) {
  if (join) snprintf(o, on, "%s/garrysmod/cache/workshop_join/%llu", root, (unsigned long long)id);
  else snprintf(o, on, "%s/garrysmod/addons/%llu", root, (unsigned long long)id);
}

int sw_installed(const char *root, uint64_t id, uint64_t want_time, uint64_t want_crc, int join) {
  for (int k = 0; k < 2; k++) {
    char d[2048], j[2100]; if (k == 0) addon_dir(root, id, 0, d, sizeof d); else if (join) addon_dir(root, id, 1, d, sizeof d); else break;
    snprintf(j, sizeof j, "%s/workshop.json", d); FILE *f = fopen(j, "rb"); if (!f) continue;
    char b[8192]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    const char *t = strstr(b, "\"time\""); const char *c = strstr(b, "\"gma_crc\"");
    uint64_t tv = t ? strtoull(strchr(t, ':') + 1, 0, 10) : 0, cv = c ? strtoull(strchr(c, ':') + 1, 0, 10) : 0;
    if (want_time && tv < want_time) continue;
    if (want_crc && cv && cv != want_crc) continue;
    return 1;
  }
  return 0;
}

int sw_install(const char *root, uint64_t id, int join, sw_progress *p, char *err, size_t errn) {
  volatile int *cancel = p ? p->cancel : NULL;
  sw_details d; uint64_t ids[1] = {id};
  if (get_details(ids, 1, &d, err, errn, cancel)) return -1;
  free(d.children);
  if (d.result != 1) ERR("Workshop item %llu is unavailable (removed, private or banned; result %d)", (unsigned long long)id, d.result);
  if (d.app != 4000) ERR("Workshop item %llu is not a Garry's Mod item (app %u)", (unsigned long long)id, d.app);
  if (d.file_type == 2) ERR("%llu is a collection", (unsigned long long)id);
  sw_log("[steam] item %llu '%s' size %llu hcontent %llu url %s", (unsigned long long)id, d.title, (unsigned long long)d.file_size, (unsigned long long)d.hcontent, d.file_url[0] ? "yes" : "no");
  char stage[2048], raw[2100], prep[2100], dst[2048], bak[2100];
  snprintf(stage, sizeof stage, "%s/garrysmod/cache/workshop_staging/%llu", root, (unsigned long long)id);
  rmtree(stage); snprintf(raw, sizeof raw, "%s/raw", stage); snprintf(prep, sizeof prep, "%s/prepared", stage); mkdirs(raw); mkdirs(prep);
  if (p && p->status) { char t[400]; snprintf(t, sizeof t, "Downloading Workshop: %s", d.title); p->status(p->ud, t, 0, d.file_size); }
  if (d.file_url[0]) {
    char out[2300]; const char *fn = d.filename[0] ? strrchr(d.filename, '/') ? strrchr(d.filename, '/') + 1 : d.filename : "content.bin";
    snprintf(out, sizeof out, "%s/%s", raw, fn);
    uint8_t *b; size_t bn; int st = sw_http_get(d.file_url, &b, &bn, cancel, 0, 0);
    if (st != 200) { rmtree(stage); ERR("Download failed (HTTP %d)", st); }
    FILE *f = fopen(out, "wb"); size_t w = f ? fwrite(b, 1, bn, f) : 0; if (f) fclose(f); free(b);
    if (w != bn) { rmtree(stage); ERR("Write failed"); }
  } else if (d.hcontent && d.hcontent != (uint64_t)-1) {
    if (download_manifest_item(&d, raw, p, err, errn)) { rmtree(stage); return -1; }
  } else { rmtree(stage); ERR("Workshop item has no downloadable content"); }
  if (p && p->status) { char t[400]; snprintf(t, sizeof t, "Preparing Workshop: %s", d.title); p->status(p->ud, t, 0, 0); }
  static char gl[32][2048]; int ng = 0; collect_gmas(raw, gl, &ng, 32);
  if (!ng) { rmtree(stage); ERR("Workshop item contains no Garry's Mod addon (.gma)"); }
  gma_info gi = {0}; int have_gi = 0; uint64_t total = 0;
  FILE *rc; char rcp[2200]; snprintf(rcp, sizeof rcp, "%s/.gmodvr-files", prep);
  char names[32][32];
  for (int i = 0; i < ng; i++) {
    gma_info g; if (!gma_read(gl[i], &g) && !have_gi) { gi = g; have_gi = 1; }
    snprintf(names[i], 32, "content_%d.gma", i + 1);
    char to[2300]; snprintf(to, sizeof to, "%s/%s", prep, names[i]);
    if (rename(gl[i], to)) { rmtree(stage); ERR("Could not prepare %s: %s", names[i], strerror(errno)); }
    struct stat st; stat(to, &st); total += st.st_size;
  }
  // receipt (WorkshopIntegrity: DataOutputStream, sorted names)
  if ((rc = fopen(rcp, "wb"))) {
    be32(rc, 1196902705); be32(rc, ng);
    for (int i = 0; i < ng; i++) { char to[2300]; snprintf(to, sizeof to, "%s/%s", prep, names[i]); struct stat st; stat(to, &st);
      uint16_t l = (uint16_t)strlen(names[i]); uint8_t lb[2] = {l >> 8, l}; fwrite(lb, 1, 2, rc); fwrite(names[i], 1, l, rc); be64(rc, st.st_size); be32(rc, file_crc(to)); }
    fclose(rc);
  }
  // workshop.json (GmodAddons.metadata)
  char jp[2200]; snprintf(jp, sizeof jp, "%s/workshop.json", prep); FILE *jf = fopen(jp, "wb");
  if (jf) {
    char idb[32]; snprintf(idb, sizeof idb, "%llu", (unsigned long long)id);
    fprintf(jf, "{\n \"id\": "); jesc(jf, idb); fprintf(jf, ",\n \"title\": "); jesc(jf, d.title[0] ? d.title : gi.title);
    fprintf(jf, ",\n \"author\": "); jesc(jf, gi.author); fprintf(jf, ",\n \"description\": "); jesc(jf, gi.desc);
    fprintf(jf, ",\n \"tags\": [");
    char seen[32][64]; int ns = 0;
    #define ADDTAG(T) do { const char *_t = (T); if (!_t[0]) break; int dup = 0; for (int q = 0; q < ns; q++) dup |= !strcasecmp(seen[q], _t); if (dup || ns >= 32) break; snprintf(seen[ns++], 64, "%s", _t); } while (0)
    for (int i = 0; i < d.ntags; i++) ADDTAG(d.tags[i]);
    for (int i = 0; i < gi.ntags; i++) ADDTAG(gi.tags[i]);
    ADDTAG(gi.type);
    for (int i = 0; i < ns; i++) { if (i) fputc(',', jf); jesc(jf, seen[i]); }
    fprintf(jf, "],\n \"time\": %llu,\n \"gma_crc\": %u,\n \"type\": ", (unsigned long long)(d.time_updated ? d.time_updated : gi.ts), gi.crc);
    jesc(jf, gi.type); fprintf(jf, ",\n \"size\": %llu,\n \"format\": \"gma\"\n}", (unsigned long long)total); fclose(jf);
  }
  // publish
  addon_dir(root, id, join, dst, sizeof dst);
  { char par[2048]; snprintf(par, sizeof par, "%s", dst); *strrchr(par, '/') = 0; mkdirs(par); }
  snprintf(bak, sizeof bak, "%s.old", dst); rmtree(bak);
  struct stat st; if (!lstat(dst, &st)) rename(dst, bak);
  if (rename(prep, dst)) { if (!lstat(bak, &st)) rename(bak, dst); rmtree(stage); ERR("Install failed: %s", strerror(errno)); }
  rmtree(bak); rmtree(stage);
  if (!join) { char jc[2048]; addon_dir(root, id, 1, jc, sizeof jc); rmtree(jc); }
  sw_log("[steam] installed %llu -> %s (%d gma, crc %u)", (unsigned long long)id, dst, ng, gi.crc);
  return 0;
}
