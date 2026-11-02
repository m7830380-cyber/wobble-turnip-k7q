// Steam account login (IAuthenticationService over HTTPS, same flow as Madeira's SwiftSteam)
// + Garry's Mod game content install/update/verify from SteamPipe (depot rules = GDroid SteamCatalog).
// Included at the end of sw.c (uses its protobuf/CM/CDN helpers).
#include <pthread.h>
#include <sys/statvfs.h>
#include <ctype.h>
#include <strings.h>

// ---------------- small helpers ----------------
static void b64enc(const uint8_t *p, size_t n, char *o) {
  static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i = 0, k = 0;
  for (; i + 2 < n; i += 3) { uint32_t v = p[i] << 16 | p[i + 1] << 8 | p[i + 2]; o[k++] = t[v >> 18]; o[k++] = t[v >> 12 & 63]; o[k++] = t[v >> 6 & 63]; o[k++] = t[v & 63]; }
  if (n - i == 1) { uint32_t v = p[i] << 16; o[k++] = t[v >> 18]; o[k++] = t[v >> 12 & 63]; o[k++] = '='; o[k++] = '='; }
  else if (n - i == 2) { uint32_t v = p[i] << 16 | p[i + 1] << 8; o[k++] = t[v >> 18]; o[k++] = t[v >> 12 & 63]; o[k++] = t[v >> 6 & 63]; o[k++] = '='; }
  o[k] = 0;
}
static void urlenc(const char *s, char *o) {
  static const char h[] = "0123456789ABCDEF"; size_t k = 0;
  for (; *s; s++) { unsigned char c = *s; if (isalnum(c)) o[k++] = c; else { o[k++] = '%'; o[k++] = h[c >> 4]; o[k++] = h[c & 15]; } }
  o[k] = 0;
}
static int hexdec(const char *s, uint8_t *o, size_t on) {
  size_t n = strlen(s); if (n % 2 || n / 2 > on) return -1;
  for (size_t i = 0; i < n / 2; i++) { unsigned v; if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1; o[i] = (uint8_t)v; }
  return (int)(n / 2);
}
static void sleep_cancel(int sec, volatile int *cancel) { for (int i = 0; i < sec * 4 && !(cancel && *cancel); i++) usleep(250000); }

// ---------------- IAuthenticationService ----------------
static uint64_t g_auth_client, g_auth_sid; static uint8_t g_auth_req[256]; static size_t g_auth_reqn; static float g_auth_iv = 5;

static int auth_call(const char *m, buf *b, int get, uint8_t **out, size_t *on, char *err, size_t errn) {
  size_t bn = b->n; char *b64 = malloc(bn * 2 + 8); b64enc(b->p ? b->p : (uint8_t *)"", bn, b64);
  char *enc = malloc(strlen(b64) * 3 + 1); urlenc(b64, enc); free(b64);
  size_t L = strlen(enc) + 256; char *url = malloc(L); int xe = 1, st;
  if (get) {
    snprintf(url, L, "https://api.steampowered.com/IAuthenticationService/%s/v1/?input_protobuf_encoded=%s", m, enc);
    st = sw_http_req("GET", url, NULL, 0, NULL, out, on, &xe);
  } else {
    snprintf(url, L, "https://api.steampowered.com/IAuthenticationService/%s/v1/", m);
    char *form = malloc(strlen(enc) + 40); sprintf(form, "input_protobuf_encoded=%s", enc);
    st = sw_http_req("POST", url, form, strlen(form), "application/x-www-form-urlencoded", out, on, &xe); free(form);
  }
  free(enc); free(url);
  sw_log("[steamauth] %s -> HTTP %d eresult %d", m, st, xe);
  if (st < 0) ERR("Can't reach Steam, check your internet");
  if (st == 429) { free(*out); ERR("Steam says chill, too many tries. Wait a bit"); }
  if (st != 200) { free(*out); ERR("Steam login is dead rn (HTTP %d), try later", st); }
  if (xe != 1) {
    free(*out);
    switch (xe) {
      case 5: case 18: ERR("Wrong username or password");
      case 65: case 88: ERR("Wrong code, try again (or wait for a new one)");
      case 84: case 87: ERR("Too many tries, Steam wants you to wait a bit");
      case 17: ERR("That account is banned/disabled rip");
      case 9: case 27: ERR("Login took too long, start again");
      default: ERR("Steam said no (code %d)", xe);
    }
  }
  return 0;
}

int sg_auth_begin(const char *user, const char *pass, int *guard, char *hint, size_t hn, char *err, size_t errn) {
  *guard = 0; if (hint && hn) hint[0] = 0;
  uint8_t *o; size_t on; buf b = {0}; fstr(&b, 1, user);
  if (auth_call("GetPasswordRSAPublicKey", &b, 1, &o, &on, err, errn)) { free(b.p); return -1; } free(b.p);
  char mod[1100] = "", ex[64] = ""; uint64_t ts = 0;
  { pbr r; pb_init(&r, o, on); while (pb_next(&r)) { if (r.f == 1 && r.d) pb_str(&r, mod, sizeof mod); else if (r.f == 2 && r.d) pb_str(&r, ex, sizeof ex); else if (r.f == 3) ts = r.v; } }
  free(o);
  if (!mod[0]) ERR("Wrong username or password");
  uint8_t mb[600], eb[32], cipher[600]; int ml = hexdec(mod, mb, sizeof mb), el = hexdec(ex, eb, sizeof eb);
  if (ml <= 0 || el <= 0) ERR("Steam sent something weird, try again");
  int cl = sw_rsa_pkcs1(mb, ml, eb, el, (const uint8_t *)pass, strlen(pass), cipher, sizeof cipher);
  if (cl <= 0) ERR("Couldn't encrypt the password, idk why");
  char cb64[1000]; b64enc(cipher, cl, cb64);
  b = (buf){0}; fstr(&b, 1, "GMod iOS"); fstr(&b, 2, user); fstr(&b, 3, cb64); fvar(&b, 4, ts); fvar(&b, 6, 1); fvar(&b, 7, 1); fstr(&b, 8, "Client");
  { buf dd = {0}; fstr(&dd, 1, "GMod iOS"); fvar(&dd, 2, 1); fbytes(&b, 9, dd.p, dd.n); free(dd.p); }
  if (auth_call("BeginAuthSessionViaCredentials", &b, 0, &o, &on, err, errn)) { free(b.p); return -1; } free(b.p);
  g_auth_client = 0; g_auth_sid = 0; g_auth_reqn = 0; g_auth_iv = 5;
  { pbr r; pb_init(&r, o, on);
    while (pb_next(&r)) {
      if (r.f == 1) g_auth_client = r.v;
      else if (r.f == 2 && r.d && r.dn <= sizeof g_auth_req) { memcpy(g_auth_req, r.d, r.dn); g_auth_reqn = r.dn; }
      else if (r.f == 3 && r.wt == 5) { uint32_t u = (uint32_t)r.v; float f; memcpy(&f, &u, 4); if (f > 0.5f && f < 60) g_auth_iv = f; }
      else if (r.f == 4 && r.d) {
        pbr s; pb_init(&s, r.d, r.dn); int ty = 0; char msg[256] = "";
        while (pb_next(&s)) { if (s.f == 1) ty = (int)s.v; else if (s.f == 2 && s.d) pb_str(&s, msg, sizeof msg); }
        if (ty == 3) *guard |= 1; else if (ty == 2) { *guard |= 2; if (hint && hn) snprintf(hint, hn, "%s", msg); } else if (ty == 4 || ty == 5) *guard |= 4;
      } else if (r.f == 5) g_auth_sid = r.v;
    } }
  free(o);
  if (!g_auth_client) ERR("Wrong username or password");
  sw_log("[steamauth] session started, guard %d", *guard);
  return 0;
}

int sg_auth_code(const char *code, int email, char *err, size_t errn) {
  char c[32]; size_t k = 0; for (const char *s = code; *s && k < sizeof c - 1; s++) if (!isspace((unsigned char)*s)) c[k++] = (char)toupper((unsigned char)*s); c[k] = 0;
  buf b = {0}; fvar(&b, 1, g_auth_client); ff64(&b, 2, g_auth_sid); fstr(&b, 3, c); fvar(&b, 4, email ? 2 : 3);
  uint8_t *o; size_t on; int r = auth_call("UpdateAuthSessionWithSteamGuardCode", &b, 0, &o, &on, err, errn); free(b.p);
  if (!r) free(o); return r;
}

int sg_auth_poll(char *refresh, size_t rn, char *acct, size_t an, volatile int *cancel, char *err, size_t errn) {
  time_t t0 = time(NULL);
  while (time(NULL) - t0 < 300) {
    if (cancel && *cancel) ERR("cancelled");
    buf b = {0}; fvar(&b, 1, g_auth_client); fbytes(&b, 2, g_auth_req, g_auth_reqn);
    uint8_t *o; size_t on; int r = auth_call("PollAuthSessionStatus", &b, 0, &o, &on, err, errn); free(b.p);
    if (r) return -1;
    char rt[4096] = "", nm[128] = ""; pbr p; pb_init(&p, o, on);
    while (pb_next(&p)) { if (p.f == 1 && p.v) g_auth_client = p.v; else if (p.f == 3 && p.d) pb_str(&p, rt, sizeof rt); else if (p.f == 6 && p.d) pb_str(&p, nm, sizeof nm); }
    free(o);
    if (rt[0]) { snprintf(refresh, rn, "%s", rt); if (nm[0]) snprintf(acct, an, "%s", nm); sw_log("[steamauth] got tokens for %s", nm); return 0; }
    sleep_cancel((int)(g_auth_iv + 0.5f), cancel);
  }
  ERR("Login took too long, start again");
}

// ---------------- PICS app info ----------------
static int pics_appinfo(uint32_t app, char **kv, char *err, size_t errn, volatile int *cancel) {
  if (cm_connect(err, errn, cancel)) return -1;
  uint64_t tok = 0;
  { uint64_t job = g_cm.job++; buf b = {0}, h = {0}; fvar(&b, 2, app); ff64(&h, 10, job);
    cm_send(8905, &b, &h); free(b.p); free(h.p); cmmsg m;
    if (!cm_wait(8906, job, &m, 30, cancel)) {
      pbr r; pb_init(&r, m.body, m.bn);
      while (pb_next(&r)) if (r.f == 3 && r.d) { pbr s; pb_init(&s, r.d, r.dn); uint64_t a = 0, t = 0; while (pb_next(&s)) { if (s.f == 1) a = s.v; else if (s.f == 2) t = s.v; } if (a == app) tok = t; }
      free(m.raw);
    } }
  uint64_t job = g_cm.job++; buf b = {0}, h = {0}, a = {0};
  fvar(&a, 1, app); if (tok) fvar(&a, 2, tok); fbytes(&b, 2, a.p, a.n); free(a.p); ff64(&h, 10, job);
  cm_send(8903, &b, &h); free(b.p); free(h.p);
  for (int k = 0; k < 6; k++) {
    cmmsg m; if (cm_wait(8904, job, &m, 30, cancel)) ERR("Steam ghosted the game info request, try again");
    pbr r; pb_init(&r, m.body, m.bn); int pending = 0;
    while (pb_next(&r)) {
      if (r.f == 1 && r.d) {
        pbr s; pb_init(&s, r.d, r.dn); uint64_t id = 0; const uint8_t *bb = NULL; size_t bl = 0;
        while (pb_next(&s)) { if (s.f == 1) id = s.v; else if (s.f == 5 && s.d) { bb = s.d; bl = s.dn; } }
        if (id == app && bb) { *kv = malloc(bl + 1); memcpy(*kv, bb, bl); (*kv)[bl] = 0; free(m.raw); return 0; }
      } else if (r.f == 6) pending = (int)r.v;
    }
    free(m.raw); if (!pending) break;
  }
  ERR("Steam has no GMod info for us (app %u), weird", app);
}

// ---------------- text KeyValues (appinfo) ----------------
typedef struct { uint32_t id; char os[64], arch[16], lang[32], lowv[8], dlc[16], gid[32]; uint64_t size; } depinfo;
static int vdf_tok(const char **pp, char *o, size_t on) {
  const char *p = *pp;
  for (;;) {
    while (*p && isspace((unsigned char)*p)) p++;
    if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
    break;
  }
  if (!*p) { *pp = p; return 0; }
  if (*p == '{') { *pp = p + 1; return 2; }
  if (*p == '}') { *pp = p + 1; return 3; }
  size_t k = 0;
  if (*p == '"') {
    p++;
    while (*p && *p != '"') { char c = *p++; if (c == '\\' && *p) { c = *p++; if (c == 'n') c = '\n'; else if (c == 't') c = '\t'; } if (k + 1 < on) o[k++] = c; }
    if (*p == '"') p++;
  } else while (*p && !isspace((unsigned char)*p) && *p != '{' && *p != '}' && *p != '"') { if (k + 1 < on) o[k++] = *p; p++; }
  o[k] = 0; *pp = p; return 1;
}
static int is_num(const char *s) { if (!*s) return 0; for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0; return 1; }
static int parse_depots(const char *kv, depinfo *d, int max) {
  char stk[16][64]; int dep = 0, n = 0, havekey = 0; char key[256] = "", t[1024]; const char *p = kv; int ty;
  while ((ty = vdf_tok(&p, t, sizeof t))) {
    if (ty == 2) { if (dep < 16) snprintf(stk[dep], 64, "%s", havekey ? key : ""); dep++; havekey = 0; continue; }
    if (ty == 3) { if (dep > 0) dep--; havekey = 0; continue; }
    if (!havekey) { snprintf(key, sizeof key, "%s", t); havekey = 1; continue; }
    havekey = 0; // key = value at stack depth dep
    // find "depots" level
    int di = -1; for (int i = 0; i < dep && i < 16; i++) if (!strcasecmp(stk[i], "depots")) { di = i; break; }
    if (di < 0 || di + 1 >= dep || !is_num(stk[di + 1])) continue;
    uint32_t id = (uint32_t)strtoul(stk[di + 1], 0, 10); int k = -1;
    for (int i = 0; i < n; i++) if (d[i].id == id) k = i;
    if (k < 0) { if (n >= max) continue; k = n++; memset(&d[k], 0, sizeof d[k]); d[k].id = id; }
    int rel = dep - (di + 2); const char *l1 = rel >= 1 ? stk[di + 2] : "", *l2 = rel >= 2 ? stk[di + 3] : "";
    if (rel == 0 && !strcasecmp(key, "dlcappid")) snprintf(d[k].dlc, sizeof d[k].dlc, "%s", t);
    else if (rel == 1 && !strcasecmp(l1, "config")) {
      if (!strcasecmp(key, "oslist")) snprintf(d[k].os, sizeof d[k].os, "%s", t);
      else if (!strcasecmp(key, "osarch")) snprintf(d[k].arch, sizeof d[k].arch, "%s", t);
      else if (!strcasecmp(key, "language")) snprintf(d[k].lang, sizeof d[k].lang, "%s", t);
      else if (!strcasecmp(key, "lowviolence")) snprintf(d[k].lowv, sizeof d[k].lowv, "%s", t);
    } else if (rel == 1 && !strcasecmp(l1, "manifests") && !strcasecmp(key, "public")) snprintf(d[k].gid, sizeof d[k].gid, "%s", t);
    else if (rel == 2 && !strcasecmp(l1, "manifests") && !strcasecmp(l2, "public")) {
      if (!strcasecmp(key, "gid")) snprintf(d[k].gid, sizeof d[k].gid, "%s", t);
      else if (!strcasecmp(key, "size")) d[k].size = strtoull(t, 0, 10);
    }
  }
  return n;
}
// GDroid SteamCatalog.depotMatches
static int depot_wanted(const depinfo *d) {
  if (d->dlc[0] || !d->gid[0] || !strcmp(d->lowv, "1")) return 0;
  if (d->os[0]) { char o[64]; snprintf(o, sizeof o, ",%s,", d->os); if (!strstr(o, ",windows,")) return 0; }
  if (d->arch[0] && strcmp(d->arch, "32")) return 0;
  if (d->lang[0] && strcmp(d->lang, "english")) return 0;
  return 1;
}
// GDroid SteamCatalog.gamePath: only garrysmod/ sourceengine/ platform/, no binaries / user folders
static int game_path(const char *name) {
  const char *sl = strchr(name, '/'); if (!sl) return 0;
  size_t l = (size_t)(sl - name);
  if (!((l == 9 && !strncasecmp(name, "garrysmod", 9)) || (l == 12 && !strncasecmp(name, "sourceengine", 12)) || (l == 8 && !strncasecmp(name, "platform", 8)))) return 0;
  char lo[1024]; size_t i = 0; for (; name[i] && i < sizeof lo - 1; i++) lo[i] = (char)tolower((unsigned char)name[i]); lo[i] = 0; size_t n = i;
  if (strstr(lo, "/bin/") || strstr(lo, "/save/") || strstr(lo, "/custom/") || strstr(lo, "/download/")) return 0;
  const char *bad[] = {".exe", ".dll", ".so", ".dylib"};
  for (int k = 0; k < 4; k++) { size_t bl = strlen(bad[k]); if (n >= bl && !strcmp(lo + n - bl, bad[k])) return 0; }
  return 1;
}

// ---------------- account CM logon (hooked into cm_connect) ----------------

static int depot_key(uint32_t app, uint32_t depot, uint8_t key[32], char *err, size_t errn, volatile int *cancel) {
  if (cm_connect(err, errn, cancel)) return -1;
  uint64_t job = g_cm.job++; buf b = {0}, h = {0}; fvar(&b, 1, depot); fvar(&b, 2, app); ff64(&h, 10, job);
  cm_send(5438, &b, &h); free(b.p); free(h.p); cmmsg m;
  if (cm_wait(5439, job, &m, 30, cancel)) ERR("Steam ghosted us (depot key), try again");
  pbr r; pb_init(&r, m.body, m.bn); int eres = 2, kl = 0;
  while (pb_next(&r)) { if (r.f == 1) eres = (int)r.v; else if (r.f == 3 && r.dn == 32) { memcpy(key, r.d, 32); kl = 32; } }
  free(m.raw);
  if (eres != 1 || kl != 32) ERR("Steam won't give depot %u (code %d). You sure this account owns GMod?", depot, eres);
  return 0;
}

typedef struct { uint32_t file; uint8_t sha[20]; uint32_t adl, ul; uint64_t off; uint8_t check; } cjob;
typedef struct { char path[700]; uint64_t size; } cfile;
typedef struct {
  cjob *j; size_t nj, next; cfile *f; char hosts[20][256]; int nh; uint8_t key[32]; uint32_t depot;
  pthread_mutex_t mu; sw_progress *p; sg_stats *st; uint64_t done, total; int failed; char why[256]; const char *label;
} cpool;

static void *chunk_worker(void *u) {
  cpool *c = u; volatile int *cancel = c->p ? c->p->cancel : NULL; uint8_t *buf2 = NULL; size_t bcap = 0; unsigned wid = (unsigned)((uintptr_t)pthread_self() >> 4);
  for (;;) {
    pthread_mutex_lock(&c->mu); size_t i = c->next < c->nj ? c->next++ : (size_t)-1; pthread_mutex_unlock(&c->mu);
    if (i == (size_t)-1 || (cancel && *cancel)) break;
    cjob *j = &c->j[i]; const cfile *f = &c->f[j->file];
    if (bcap < j->ul + 1) { free(buf2); bcap = j->ul + 1; buf2 = malloc(bcap); }
    int good = 0, fetched = 0;
    if (j->check) {
      int fd = open(f->path, O_RDONLY);
      if (fd >= 0) { if (pread(fd, buf2, j->ul, (off_t)j->off) == (ssize_t)j->ul) { uint8_t s[20]; sw_sha1(buf2, j->ul, s); good = !memcmp(s, j->sha, 20); } close(fd); }
    }
    char why[160] = "";
    if (!good) {
      char hex[41]; for (int k = 0; k < 20; k++) sprintf(hex + 2 * k, "%02x", j->sha[k]);
      for (int t = 0; t < 6 && !good; t++) {
        if (cancel && *cancel) break;
        char url[600]; snprintf(url, sizeof url, "https://%s/depot/%u/chunk/%s", c->hosts[(wid + (unsigned)t) % (unsigned)c->nh], c->depot, hex);
        uint8_t *raw; size_t rn; int s2 = sw_http_get(url, &raw, &rn, cancel, 0, 0);
        if (s2 != 200) { snprintf(why, sizeof why, "HTTP %d", s2); if (s2 >= 0) free(raw); sleep_cancel(t, cancel); continue; }
        uint8_t *pt; size_t ptn; if (symdec(c->key, raw, rn, &pt, &ptn)) { free(raw); snprintf(why, sizeof why, "decrypt"); continue; } free(raw);
        if (chunk_decode(pt, ptn, buf2, j->ul)) { snprintf(why, sizeof why, "decompress"); free(pt); continue; } free(pt);
        if ((uint32_t)adler32(0, buf2, j->ul) != j->adl) { snprintf(why, sizeof why, "checksum"); continue; }
        int fd = open(f->path, O_WRONLY);
        ssize_t w = fd >= 0 ? pwrite(fd, buf2, j->ul, (off_t)j->off) : -1; if (fd >= 0) close(fd);
        if (w != (ssize_t)j->ul) { snprintf(why, sizeof why, "write: %s", strerror(errno)); break; }
        good = 1; fetched = 1;
      }
    }
    pthread_mutex_lock(&c->mu);
    c->done += j->ul;
    if (c->st) { c->st->checked += j->check ? j->ul : 0; if (fetched) c->st->downloaded += j->ul; if (j->check && fetched) c->st->bad_chunks++; }
    if (!good && !(cancel && *cancel)) { sw_log("[game] chunk fail ul %u host %s: %s", j->ul, c->hosts[wid % (unsigned)c->nh], why); c->failed++; snprintf(c->why, sizeof c->why, "%s (%s)", f->path, why); }
    if (c->p && c->p->status) c->p->status(c->p->ud, c->label, c->done, c->total);
    pthread_mutex_unlock(&c->mu);
  }
  free(buf2); return NULL;
}

static uint64_t state_gid(const char *root, uint32_t depot) {
  char p[1200]; snprintf(p, sizeof p, "%s/.gmod_content", root); FILE *f = fopen(p, "r"); if (!f) return 0;
  unsigned d; unsigned long long g, r = 0; while (fscanf(f, "%u %llu", &d, &g) == 2) if (d == depot) r = g; fclose(f); return r;
}
static void state_set(const char *root, uint32_t depot, uint64_t gid) {
  char p[1200], q[1300]; snprintf(p, sizeof p, "%s/.gmod_content", root); snprintf(q, sizeof q, "%s.tmp", p);
  FILE *f = fopen(p, "r"), *o = fopen(q, "w"); if (!o) { if (f) fclose(f); return; }
  unsigned d; unsigned long long g;
  if (f) { while (fscanf(f, "%u %llu", &d, &g) == 2) if (d != depot) fprintf(o, "%u %llu\n", d, g); fclose(f); }
  fprintf(o, "%u %llu\n", depot, (unsigned long long)gid); fclose(o); rename(q, p);
}

static int sync_depot(const char *root, uint32_t app, const depinfo *dp, int verify, sw_progress *p, sg_stats *st, char *err, size_t errn) {
  volatile int *cancel = p ? p->cancel : NULL; uint64_t gid = strtoull(dp->gid, 0, 10); uint32_t depot = dp->id;
  static cpool c; memset(&c, 0, sizeof c); c.depot = depot; c.p = p; c.st = st; pthread_mutex_init(&c.mu, NULL);
  char label[200]; snprintf(label, sizeof label, verify ? "Checking + fixing files (part %u)" : "Pulling GMod (part %u)", depot); c.label = label;
  if (p && p->status) p->status(p->ud, "Getting the file list…", 0, 0);
  if (depot_key(app, depot, c.key, err, errn, cancel)) return -1;
  uint64_t code = 0;
  { buf b = {0}; fvar(&b, 1, app); fvar(&b, 2, depot); fvar(&b, 3, gid); fstr(&b, 4, "public"); cmmsg m;
    if (cm_call("ContentServerDirectory.GetManifestRequestCode#1", &b, &m, err, errn, cancel)) { free(b.p); return -1; } free(b.p);
    pbr r; pb_init(&r, m.body, m.bn); while (pb_next(&r)) if (r.f == 1) code = r.v; free(m.raw); }
  if (cdn_hosts(c.hosts, 20, err, errn, cancel)) return -1;
  for (c.nh = 0; c.nh < 20 && c.hosts[c.nh][0]; c.nh++) {}
  uint8_t *z = NULL; size_t zn = 0; int sti = 0;
  for (int t = 0; t < 6; t++) {
    char url[600]; snprintf(url, sizeof url, "https://%s/depot/%u/manifest/%llu/5/%llu", c.hosts[t % c.nh], depot, (unsigned long long)gid, (unsigned long long)code);
    sti = sw_http_get(url, &z, &zn, cancel, 0, 0); if (sti == 200) break; if (sti >= 0) free(z); z = NULL;
  }
  if (!z) ERR("File list download broke (HTTP %d)", sti);
  uint8_t *m; size_t mn; if (unzip1(z, zn, &m, &mn)) { free(z); ERR("Steam sent a broken file list, try again"); } free(z);
  uint32_t mg, pl; memcpy(&mg, m, 4); memcpy(&pl, m + 4, 4);
  if (mn < 8 || mg != 0x71F617D0 || 8 + (size_t)pl > mn) { free(m); ERR("Unknown file list format"); }
  int enc = 0; if (16 + (size_t)pl <= mn) { uint32_t mg2, l2; memcpy(&mg2, m + 8 + pl, 4); memcpy(&l2, m + 12 + pl, 4); if (mg2 == 0x1F4812BE && 16 + pl + l2 <= mn) { pbr r; pb_init(&r, m + 16 + pl, l2); while (pb_next(&r)) if (r.f == 4) enc = (int)r.v; } }
  int same = state_gid(root, depot) == gid, deep = verify || !same;
  size_t fcap = 4096, jcap = 16384, nf = 0; c.f = malloc(fcap * sizeof(cfile)); c.j = malloc(jcap * sizeof(cjob));
  uint64_t need = 0;
  pbr r; pb_init(&r, m + 8, pl);
  while (pb_next(&r)) {
    if (r.f != 1) continue;
    char name[1024] = ""; uint64_t size = 0; uint32_t flags = 0; pbr f; pb_init(&f, r.d, r.dn);
    while (pb_next(&f)) { if (f.f == 1) pb_str(&f, name, sizeof name); else if (f.f == 2) size = f.v; else if (f.f == 3) flags = (uint32_t)f.v; }
    if (enc) { uint8_t eb[1100]; int bl = b64dec(name, eb); uint8_t *pn; size_t pnl; if (symdec(c.key, eb, bl, &pn, &pnl)) { free(m); ERR("Couldn't decrypt file names"); } snprintf(name, sizeof name, "%.*s", (int)pnl, pn); free(pn); }
    for (char *q = name; *q; q++) if (*q == '\\') *q = '/';
    if (strstr(name, "..") || name[0] == '/' || (!game_path(name) && !getenv("SG_NOFILTER"))) continue;
    char out[1100]; snprintf(out, sizeof out, "%s/%s", root, name);
    if (flags & 64) { mkdirs(out); continue; }
    if (st) { st->files++; st->total += size; }
    { char dir[1100]; snprintf(dir, sizeof dir, "%s", out); char *sl = strrchr(dir, '/'); if (sl) { *sl = 0; mkdirs(dir); } }
    struct stat sb; int exists = !stat(out, &sb) && S_ISREG(sb.st_mode), right = exists && (uint64_t)sb.st_size == size;
    if (right && !deep) continue; // quick mode: same manifest + right size = fine
    if (!right) {
      int fd = open(out, O_WRONLY | O_CREAT, 0644); if (fd < 0) { free(m); ERR("Can't write %s: %s", name, strerror(errno)); }
      if (ftruncate(fd, (off_t)size)) {} close(fd); need += size - (exists && (uint64_t)sb.st_size < size ? (uint64_t)sb.st_size : 0);
    }
    if (nf == fcap) { fcap *= 2; c.f = realloc(c.f, fcap * sizeof(cfile)); }
    snprintf(c.f[nf].path, sizeof c.f[nf].path, "%s", out); c.f[nf].size = size;
    pb_init(&f, r.d, r.dn);
    while (pb_next(&f)) {
      if (f.f != 6) continue;
      if (c.nj == jcap) { jcap *= 2; c.j = realloc(c.j, jcap * sizeof(cjob)); }
      cjob *j = &c.j[c.nj]; memset(j, 0, sizeof *j); j->file = (uint32_t)nf; j->check = (uint8_t)right;
      pbr ch; pb_init(&ch, f.d, f.dn);
      while (pb_next(&ch)) { if (ch.f == 1 && ch.dn == 20) memcpy(j->sha, ch.d, 20); else if (ch.f == 2) j->adl = (uint32_t)ch.v; else if (ch.f == 3) j->off = ch.v; else if (ch.f == 4) j->ul = (uint32_t)ch.v; }
      c.total += j->ul; c.nj++;
    }
    nf++;
  }
  free(m);
  sw_log("[game] depot %u gid %llu: %zu files to look at, %zu chunks, %.1f MB, deep %d", depot, (unsigned long long)gid, nf, c.nj, c.total / 1048576.0, deep);
  { struct statvfs vs; if (!statvfs(root, &vs)) { uint64_t fr = (uint64_t)vs.f_bavail * vs.f_frsize;
      if (need > fr) { free(c.f); free(c.j); ERR("No space bro: need %.1f GB more, you got %.1f GB free", need / 1e9, fr / 1e9); } } }
  pthread_t th[8]; int mt = getenv("SG_THREADS") ? atoi(getenv("SG_THREADS")) : 8; if (mt < 1 || mt > 8) mt = 8; int nt = c.nj < (size_t)mt ? (int)c.nj : mt;
  for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, chunk_worker, &c);
  for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
  free(c.f); free(c.j);
  if (cancel && *cancel) ERR("cancelled");
  if (c.failed) ERR("%d pieces wouldn't download (last: %s). Hit it again, it keeps what it got", c.failed, c.why);
  state_set(root, depot, gid);
  return 0;
}

int sg_game_sync(const char *root, const char *acct, const char *refresh, int verify, sw_progress *p, sg_stats *st, char *err, size_t errn) {
  volatile int *cancel = p ? p->cancel : NULL; const uint32_t app = 4000;
  if (st) memset(st, 0, sizeof *st);
  cm_close(); snprintf(g_user, sizeof g_user, "%s", acct); snprintf(g_utok, sizeof g_utok, "%s", refresh); g_logon_eres = 0;
  if (p && p->status) p->status(p->ud, "Logging into Steam…", 0, 0);
  int rc = -1; char *kv = NULL;
  if (cm_connect(err, errn, cancel)) { if (g_logon_eres == 5 || g_logon_eres == 18 || g_logon_eres == 15) rc = -2; goto out; }
  if (p && p->status) p->status(p->ud, "Asking Steam what GMod even is…", 0, 0);
  if (pics_appinfo(app, &kv, err, errn, cancel)) goto out;
  depinfo d[32]; int n = parse_depots(kv, d, 32), used = 0;
  for (int i = 0; i < n; i++) {
    if (!depot_wanted(&d[i])) { sw_log("[game] skip depot %u (os '%s' arch '%s' gid '%s')", d[i].id, d[i].os, d[i].arch, d[i].gid); continue; }
    used++; int ok = 0;
    for (int round = 0; round < 4 && !ok; round++) { // network hiccups: next round only refetches what's still broken
      if (!sync_depot(root, app, &d[i], verify || round > 0, p, st, err, errn)) ok = 1;
      else if (!strstr(err, "pieces failed") || (cancel && *cancel)) goto out;
      else { sw_log("[game] round %d: %s", round + 1, err); cm_close(); if (cm_connect(err, errn, cancel)) goto out; }
    }
    if (!ok) goto out;
  }
  if (!used) { snprintf(err, errn, "Steam listed no GMod depots, weird"); goto out; }
  rc = 0;
out:
  free(kv); cm_close(); memset(g_utok, 0, sizeof g_utok); g_user[0] = 0;
  return rc;
}
#ifdef SG_TEST_MAIN
static void st2_cb(void *u, const char *t, uint64_t d, uint64_t tot) { (void)u; static time_t last; if (time(NULL) != last) { last = time(NULL); fprintf(stderr, "  %s %llu/%llu\n", t, (unsigned long long)d, (unsigned long long)tot); } }
int sg_test_pics(char *err, size_t errn) {
  char *kv; if (pics_appinfo(4000, &kv, err, errn, NULL)) { printf("FAIL %s\n", err); return 1; }
  depinfo d[32]; int n = parse_depots(kv, d, 32);
  for (int i = 0; i < n; i++) printf("depot %u os '%s' arch '%s' lang '%s' gid %s size %llu -> %s\n", d[i].id, d[i].os, d[i].arch, d[i].lang, d[i].gid, (unsigned long long)d[i].size, depot_wanted(&d[i]) ? "WANT" : "skip");
  const char *t[] = {"garrysmod/garrysmod_dir.vpk", "bin/gmod.exe", "garrysmod/bin/client.dll", "sourceengine/hl2_misc_dir.vpk", "platform/platform_misc_dir.vpk", "garrysmod/download/x.txt", "gmod.exe", "garrysmod/lua/a.lua"};
  for (int i = 0; i < 8; i++) printf("path %s -> %d\n", t[i], game_path(t[i]));
  free(kv); return 0;
}
#endif
#ifdef SG_TEST_MAIN
int sg_test_dl(const char *root, int verify, char *err, size_t errn) {
  char *kv; if (pics_appinfo(4020, &kv, err, errn, NULL)) { printf("FAIL %s\n", err); return 1; }
  depinfo d[32]; int n = parse_depots(kv, d, 32); free(kv);
  for (int i = 0; i < n; i++) if (d[i].id == 4023) {
    sw_progress p = {st2_cb, 0, 0}; sg_stats s = {0}; int r = sync_depot(root, 4020, &d[i], verify, &p, &s, err, errn);
    printf("rc %d %s files %llu total %llu checked %llu downloaded %llu bad %llu\n", r, r ? err : "", (unsigned long long)s.files, (unsigned long long)s.total, (unsigned long long)s.checked, (unsigned long long)s.downloaded, (unsigned long long)s.bad_chunks); return r; }
  return 1;
}
#endif
