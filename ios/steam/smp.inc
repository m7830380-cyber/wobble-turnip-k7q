// Steam multiplayer: one persistent logged-in CM session for friends, persona/avatars, game invites,
// game-server auth tickets (join PC servers) and the internet server directory.
// Replaces GDroid's JavaSteam backends (GModSteamSession / GModSteamAuth / GModServerDirectory / GModSteamPersona).
// Protocol refs: SteamKit2 SteamAuthTicket + SteamApps + SteamFriends, SteamDatabase protobufs. Included by sw.c.
#include <pthread.h>

#define SM_APP 4000
#define SM_OS ((uint32_t)-102)
static void ff32(buf *b, int f, uint32_t v) { pvar(b, (uint64_t)f << 3 | 5); bput(b, &v, 4); }
static void fsub(buf *b, int f, buf *s) { fbytes(b, f, s->p ? s->p : (uint8_t *)"", s->n); free(s->p); s->p = NULL; s->n = s->cap = 0; }

typedef struct {
  uint64_t sid; int rel, is_friend, wanted, want_full, pstate, sent; time_t last_req;
  uint32_t app, ip; uint16_t port; char name[64], game[64], connect[160];
  uint8_t hash[20]; int has_hash; uint8_t *med, *full; int med_fail, full_fail, name_ver, av_ver;
} sm_ent;
typedef struct { uint64_t job; int used, done, eres; uint8_t *body; size_t bn; } sm_waiter;

static struct {
  pthread_mutex_t mu, smu; pthread_cond_t cv; pthread_t th; int th_on, stop, reconnect;
  char acct[128], tok[4096]; int anon, have_id; uint32_t loginid;
  sw_ws *ws; uint64_t sid; int32_t sess; uint64_t job; int hb; uint32_t pubip; time_t up_at, last_hb;
  int state; char err[300]; int logon_eres, got_logon;
  int blocked, play_known, playing, adv_dirty; uint32_t srv_ip; uint16_t srv_port; uint64_t srv_sid; char srv[80];
  uint8_t *tq[32]; size_t tqn[32]; int ntok, keep;
  sm_waiter w[16];
  uint32_t tk_crc; uint8_t *tk; size_t tkn;   // the one active auth ticket we report to Steam
  int a_gen, a_state; char a_err[320], a_key[200]; uint8_t *a_cookie; size_t a_cn; uint32_t a_crc; uint64_t a_srvsid;
  sm_ent *e; int ne, ce; uint64_t serial; uint64_t chg[256]; int nchg; int av_on;
  struct { uint64_t from; char connect[160]; time_t at; } inv[8]; int ninv;
} SM = {.mu = PTHREAD_MUTEX_INITIALIZER, .smu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

unsigned char *(*sm_decode_image)(const uint8_t *d, size_t n, int size);

static int sm_individual(uint64_t s) { uint64_t d = s - 76561197960265728ULL; return s > 76561197960265728ULL && d < 2147483648ULL; }
static void sm_ipstr(uint32_t ip, uint16_t port, char *o, size_t on) { snprintf(o, on, "%u.%u.%u.%u:%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port); }
static uint32_t sm_parse_ip(const char *s) { unsigned a, b, c, d; char t; if (!s || sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &t) < 4 || a > 255 || b > 255 || c > 255 || d > 255) return 0; return a << 24 | b << 16 | c << 8 | d; }
static void sm_sleep(int ms) { struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&ts, NULL); }

// ---------------- wire ----------------
static int sm_send(uint32_t emsg, buf *body, buf *xh) {
  pthread_mutex_lock(&SM.smu);
  if (!SM.ws) { pthread_mutex_unlock(&SM.smu); return -1; }
  buf h = {0}; ff64(&h, 1, SM.sid); fvar(&h, 2, (uint64_t)(int64_t)SM.sess); if (xh && xh->n) bput(&h, xh->p, xh->n);
  buf m = {0}; uint32_t hd[2] = {emsg | 0x80000000u, (uint32_t)h.n}; bput(&m, hd, 8); bput(&m, h.p, h.n); if (body && body->n) bput(&m, body->p, body->n);
  int r = sw_ws_send(SM.ws, m.p, m.n); free(h.p); free(m.p);
  pthread_mutex_unlock(&SM.smu); return r;
}
static void sm_on(uint32_t e, uint64_t jt, int er, const char *jn, const uint8_t *b, size_t bn);
static void sm_parse(const uint8_t *d, size_t n, int depth) {
  if (n < 8 || depth > 3) return;
  uint32_t e, hl; memcpy(&e, d, 4); memcpy(&hl, d + 4, 4);
  if (!(e & 0x80000000u) || 8 + (size_t)hl > n) return;
  e &= 0x7fffffff;
  const uint8_t *body = d + 8 + hl; size_t bn = n - 8 - hl;
  if (e == 1) {
    pbr r; pb_init(&r, body, bn); uint64_t unz = 0; const uint8_t *mb = NULL; size_t mn = 0;
    while (pb_next(&r)) { if (r.f == 1) unz = r.v; else if (r.f == 2) { mb = r.d; mn = r.dn; } }
    if (!mb || unz > (64u << 20)) return; uint8_t *tmp = NULL;
    if (unz) {
      tmp = malloc(unz); z_stream z = {0}; inflateInit2(&z, 16 + MAX_WBITS);
      z.next_in = (Bytef *)mb; z.avail_in = (uInt)mn; z.next_out = tmp; z.avail_out = (uInt)unz;
      inflate(&z, Z_FINISH); mn = z.total_out; inflateEnd(&z); mb = tmp;
    }
    for (size_t i = 0; i + 4 <= mn;) { uint32_t l; memcpy(&l, mb + i, 4); if (i + 4 + l > mn) break; sm_parse(mb + i + 4, l, depth + 1); i += 4 + l; }
    free(tmp); return;
  }
  uint64_t jt = 0; int er = 1; char jn[128] = "";
  pbr r; pb_init(&r, d + 8, hl);
  while (pb_next(&r)) {
    if (r.f == 1 && e == 751) SM.sid = r.v;
    else if (r.f == 2 && e == 751) SM.sess = (int32_t)r.v;
    else if (r.f == 11) jt = r.v;
    else if (r.f == 13) er = (int)(int32_t)r.v;
    else if (r.f == 12 && r.d) pb_str(&r, jn, sizeof jn);
  }
  sm_on(e, jt, er, jn, body, bn);
}
static void sm_drop(void) {
  pthread_mutex_lock(&SM.smu); if (SM.ws) sw_ws_close(SM.ws); SM.ws = NULL; pthread_mutex_unlock(&SM.smu);
  pthread_mutex_lock(&SM.mu);
  for (int i = 0; i < SM.ntok; i++) free(SM.tq[i]); SM.ntok = 0;
  if (SM.state == 2) SM.state = 1;
  SM.play_known = 0; SM.blocked = 0;
  pthread_cond_broadcast(&SM.cv); pthread_mutex_unlock(&SM.mu);
}

// request/response by job id (emsg 151 service call -> 147, or plain msgs that echo jobid_target)
static int sm_call(uint32_t emsg, buf *body, const char *method, uint8_t **out, size_t *on, int *eres, int timeout_s) {
  *out = NULL; *on = 0; if (eres) *eres = 2;
  pthread_mutex_lock(&SM.mu);
  int k = -1; for (int i = 0; i < 16; i++) if (!SM.w[i].used) { k = i; break; }
  if (k < 0 || SM.state != 2) { pthread_mutex_unlock(&SM.mu); return -1; }
  uint64_t job = ++SM.job; SM.w[k] = (sm_waiter){.job = job, .used = 1};
  pthread_mutex_unlock(&SM.mu);
  buf h = {0}; ff64(&h, 10, job); if (method) fstr(&h, 12, method);
  int r = sm_send(emsg, body, &h); free(h.p);
  pthread_mutex_lock(&SM.mu);
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += timeout_s;
  while (!r && !SM.w[k].done && SM.state == 2) if (pthread_cond_timedwait(&SM.cv, &SM.mu, &ts)) break;
  int ok = SM.w[k].done; if (ok) { *out = SM.w[k].body; *on = SM.w[k].bn; if (eres) *eres = SM.w[k].eres; }
  SM.w[k].used = 0; pthread_mutex_unlock(&SM.mu);
  return ok ? 0 : -1;
}

// ---------------- persona store ----------------
static sm_ent *sm_find(uint64_t sid, int create) {
  for (int i = 0; i < SM.ne; i++) if (SM.e[i].sid == sid) return &SM.e[i];
  if (!create || SM.ne >= 4096) return NULL;
  if (SM.ne == SM.ce) { SM.ce = SM.ce ? SM.ce * 2 : 64; SM.e = realloc(SM.e, sizeof(sm_ent) * SM.ce); }
  sm_ent *x = &SM.e[SM.ne++]; memset(x, 0, sizeof *x); x->sid = sid; return x;
}
static void sm_changed(uint64_t sid) {
  SM.serial++;
  for (int i = 0; i < SM.nchg; i++) if (SM.chg[i] == sid) return;
  if (SM.nchg < 256) SM.chg[SM.nchg++] = sid;
}
static void sm_kv(const uint8_t *d, size_t n, char *k, size_t kn, char *v, size_t vn) {
  k[0] = v[0] = 0; pbr r; pb_init(&r, d, n);
  while (pb_next(&r)) { if (r.f == 1 && r.d) pb_str(&r, k, kn); else if (r.f == 2 && r.d) pb_str(&r, v, vn); }
}
static void sm_persona(const uint8_t *b, size_t bn) {
  uint32_t flags = 0; pbr r; pb_init(&r, b, bn);
  while (pb_next(&r)) if (r.f == 1) flags = (uint32_t)r.v;
  pb_init(&r, b, bn);
  while (pb_next(&r)) {
    if (r.f != 2 || !r.d) continue;
    uint64_t sid = 0; pbr q; pb_init(&q, r.d, r.dn); while (pb_next(&q)) if (q.f == 1) sid = q.v;
    if (!sid) continue;
    sm_ent *x = sm_find(sid, 0); if (!x) continue;
    int presence = (flags & 16) || !flags, chg = 0;
    if (presence) { x->app = 0; x->ip = 0; x->port = 0; x->game[0] = 0; x->connect[0] = 0; }
    pb_init(&q, r.d, r.dn);
    while (pb_next(&q)) {
      switch (q.f) {
        case 2: x->pstate = (int)q.v; break;
        case 3: x->app = (uint32_t)q.v; break;
        case 4: x->ip = (uint32_t)q.v; break;
        case 5: x->port = (uint16_t)q.v; break;
        case 15: if (q.d) { char nm[64]; pb_str(&q, nm, sizeof nm); if (nm[0] && strcmp(nm, x->name)) { snprintf(x->name, sizeof x->name, "%s", nm); x->name_ver++; chg = 1; } } break;
        case 31: if (q.d && q.dn == 20) { int z = 1; for (int i = 0; i < 20; i++) z &= !q.d[i];
                   uint8_t h[20]; if (z) memcpy(h, "\xfe\xf4\x9e\x7f\xa7\xe1\x99\x73\x10\xd7\x05\xb2\xa6\x15\x8f\xf8\xdc\x1c\xdf\xeb", 20); else memcpy(h, q.d, 20);
                   if (!x->has_hash || memcmp(h, x->hash, 20)) { memcpy(x->hash, h, 20); x->has_hash = 1; free(x->med); free(x->full); x->med = x->full = NULL; x->med_fail = x->full_fail = 0; chg = 1; } } break;
        case 55: if (q.d) pb_str(&q, x->game, sizeof x->game); break;
        case 71: if (q.d) { char k[64], v[160]; sm_kv(q.d, q.dn, k, sizeof k, v, sizeof v); if (!strcmp(k, "connect")) snprintf(x->connect, sizeof x->connect, "%s", v); } break;
      }
    }
    if (chg) sm_changed(sid);
  }
  pthread_cond_broadcast(&SM.cv);
}

// ---------------- invites ----------------
static int sm_addr_from(const char *s, char *o, size_t on) { // finds a.b.c.d[:port] anywhere in s
  for (const char *p = s; p && *p; p++) {
    if (*p < '0' || *p > '9' || (p > s && ((p[-1] >= '0' && p[-1] <= '9') || p[-1] == '.'))) continue;
    unsigned a, b, c, d, port = 27015; int n = 0;
    if (sscanf(p, "%u.%u.%u.%u%n", &a, &b, &c, &d, &n) == 4 && a < 256 && b < 256 && c < 256 && d < 256) {
      if (p[n] == ':') sscanf(p + n + 1, "%u", &port);
      snprintf(o, on, "%u.%u.%u.%u:%u", a, b, c, d, port & 0xffff); return 1;
    }
  }
  return 0;
}
static void sm_push_invite(uint64_t from, const char *connect) {
  time_t now = time(NULL);
  for (int i = 0; i < SM.ninv; i++) if (SM.inv[i].from == from && !strcmp(SM.inv[i].connect, connect) && now - SM.inv[i].at < 15) return;
  if (SM.ninv == 8) { memmove(SM.inv, SM.inv + 1, sizeof SM.inv[0] * 7); SM.ninv--; }
  SM.inv[SM.ninv].from = from; SM.inv[SM.ninv].at = now; snprintf(SM.inv[SM.ninv].connect, sizeof SM.inv[0].connect, "%s", connect); SM.ninv++;
  sm_ent *x = sm_find(from, 1); if (x && !x->name[0]) { x->wanted = 1; x->sent = 0; }
  sw_log("[mp] invite from %llu: %s", (unsigned long long)from, connect);
}
static void sm_chat_invite(uint64_t from, int type, const char *msg) {
  const char *g = strstr(msg, "[gameinvite");
  if (!g && type != 3) return;
  char con[160] = "";
  const char *c = g ? strstr(g, "connect=") : NULL;
  if (c) { c += 8; char q = (*c == '"' || *c == '\'') ? *c++ : 0; size_t i = 0;
    while (c[i] && i < sizeof con - 1 && (q ? c[i] != q : (c[i] != ' ' && c[i] != ']'))) { con[i] = c[i]; i++; } con[i] = 0; }
  else snprintf(con, sizeof con, "%s", msg);
  if (g) { const char *ap = strstr(g, "appid="); if (ap && atoi(ap + 6 + (ap[6] == '"')) != SM_APP) return; }
  sm_push_invite(from, con);
}

// ---------------- dispatcher (session thread) ----------------
static const char *sm_auth_msg(int r) {
  switch (r) {
    case 2: return "Steam says this account doesn't own GMod";
    case 3: return "VAC banned on GMod, secure servers say no";
    case 4: return "Steam says your account is in a game somewhere else. Close it there and rejoin";
    case 5: return "This server is VAC secured and VAC can't run on iPhone. Pick a non-VAC server";
    case 6: return "Steam canceled the auth ticket, rejoin";
    case 7: case 8: return "Steam says the ticket is bad, rejoin";
  }
  return NULL;
}
static void sm_on(uint32_t e, uint64_t jt, int er, const char *jn, const uint8_t *b, size_t bn) {
  pbr r; pthread_mutex_lock(&SM.mu);
  if (jt) for (int i = 0; i < 16; i++) if (SM.w[i].used && !SM.w[i].done && SM.w[i].job == jt) {
    SM.w[i].done = 1; SM.w[i].eres = er; SM.w[i].body = malloc(bn + 1); memcpy(SM.w[i].body, b, bn); SM.w[i].bn = bn;
    pthread_cond_broadcast(&SM.cv);
  }
  switch (e) {
    case 751: { int eres = 2, hb = 9; uint32_t ip = 0; pb_init(&r, b, bn);
      while (pb_next(&r)) { if (r.f == 1) eres = (int)r.v; else if (r.f == 3) hb = (int)r.v; else if (r.f == 4 && !ip) ip = (uint32_t)r.v;
        else if (r.f == 15 && r.d) { pbr q; pb_init(&q, r.d, r.dn); while (pb_next(&q)) if (q.f == 1) ip = (uint32_t)q.v; } }
      SM.logon_eres = eres; SM.got_logon = 1; SM.hb = hb > 0 && hb < 120 ? hb : 9; if (ip) SM.pubip = ip; break; }
    case 757: { int eres = 0; pb_init(&r, b, bn); while (pb_next(&r)) if (r.f == 1) eres = (int)r.v;
      sw_log("[mp] Steam logged us off (code %d)", eres);
      snprintf(SM.err, sizeof SM.err, eres == 34 ? "Steam kicked this device, the account logged in somewhere else" : "Steam logged us off (code %d)", eres);
      SM.reconnect = 1; break; }
    case 779: { int keep = 10; pb_init(&r, b, bn);
      while (pb_next(&r)) {
        if (r.f == 1) keep = (int)r.v;
        else if (r.f == 2 && r.d) { if (SM.ntok == 32) { free(SM.tq[0]); memmove(SM.tq, SM.tq + 1, sizeof SM.tq[0] * 31); memmove(SM.tqn, SM.tqn + 1, sizeof SM.tqn[0] * 31); SM.ntok--; }
          SM.tq[SM.ntok] = malloc(r.dn); memcpy(SM.tq[SM.ntok], r.d, r.dn); SM.tqn[SM.ntok++] = r.dn; }
      }
      while (SM.ntok > keep && SM.ntok > 0) { free(SM.tq[0]); memmove(SM.tq, SM.tq + 1, sizeof SM.tq[0] * (SM.ntok - 1)); memmove(SM.tqn, SM.tqn + 1, sizeof SM.tqn[0] * (SM.ntok - 1)); SM.ntok--; }
      SM.keep = keep; pthread_cond_broadcast(&SM.cv); break; }
    case 5429: { uint64_t sid = 0; int resp = 0; uint32_t crc = 0; pb_init(&r, b, bn);
      while (pb_next(&r)) { if (r.f == 1) sid = r.v; else if (r.f == 4) resp = (int)r.v; else if (r.f == 6) crc = (uint32_t)r.v; }
      sw_log("[mp] ticket auth complete crc %08x sid %llu resp %d", crc, (unsigned long long)sid, resp);
      if (SM.a_state >= 2 && crc == SM.a_crc && (!SM.a_srvsid || sid == SM.a_srvsid || sid == SM.sid)) {
        if (!resp) SM.a_state = 3;
        else { const char *m = sm_auth_msg(resp); if (m) snprintf(SM.a_err, sizeof SM.a_err, "%s", m); else snprintf(SM.a_err, sizeof SM.a_err, "Steam auth said no (code %d)", resp); SM.a_state = -1; }
      }
      break; }
    case 9600: { int bl = 0; pb_init(&r, b, bn); while (pb_next(&r)) if (r.f == 2) bl = (int)r.v; SM.blocked = bl; SM.play_known = 1; SM.adv_dirty = 1; break; }
    case 767: { int inc = 0; pb_init(&r, b, bn); while (pb_next(&r)) if (r.f == 1) inc = (int)r.v;
      if (!inc) for (int i = 0; i < SM.ne; i++) SM.e[i].is_friend = 0;
      pb_init(&r, b, bn);
      while (pb_next(&r)) {
        if (r.f != 2 || !r.d) continue; uint64_t sid = 0; int rel = 0; pbr q; pb_init(&q, r.d, r.dn);
        while (pb_next(&q)) { if (q.f == 1) sid = q.v; else if (q.f == 2) rel = (int)q.v; }
        if (!sm_individual(sid)) continue;
        sm_ent *x = sm_find(sid, rel != 0); if (!x) continue;
        x->rel = rel; x->is_friend = rel == 3; if (x->is_friend && !x->name[0]) x->sent = 0;
      }
      for (int i = 0; i < SM.ne; i++) if (SM.e[i].is_friend) sm_changed(SM.e[i].sid);
      break; }
    case 766: sm_persona(b, bn); break;
    case 7005: { uint64_t src = 0; char con[160] = ""; pb_init(&r, b, bn);
      while (pb_next(&r)) { if (r.f == 2) src = r.v; else if (r.f == 3 && r.d) pb_str(&r, con, sizeof con); }
      if (src && con[0]) sm_push_invite(src, con); break; }
    case 5427: { uint64_t from = 0; int type = 0; char msg[1024] = ""; pb_init(&r, b, bn);
      while (pb_next(&r)) { if (r.f == 1) from = r.v; else if (r.f == 2) type = (int)r.v; else if (r.f == 4 && r.d) pb_str(&r, msg, sizeof msg); }
      if (from) sm_chat_invite(from, type, msg); break; }
    case 146: if (!strncmp(jn, "FriendMessagesClient.IncomingMessage", 36)) {
      uint64_t from = 0; int type = 0, echo = 0; char msg[1024] = ""; pb_init(&r, b, bn);
      while (pb_next(&r)) { if (r.f == 1) from = r.v; else if (r.f == 2) type = (int)r.v; else if (r.f == 4 && r.d) pb_str(&r, msg, sizeof msg); else if (r.f == 7) echo = (int)r.v; }
      if (from && !echo) sm_chat_invite(from, type, msg);
    } break;
  }
  pthread_mutex_unlock(&SM.mu);
}

// ---------------- session thread ----------------
static void sm_send_played(void) {
  pthread_mutex_lock(&SM.mu);
  int play = SM.playing && !SM.blocked; uint32_t ip = SM.srv_ip; uint16_t port = SM.srv_port; uint64_t gs = SM.srv_sid; SM.adv_dirty = 0;
  if (!strncmp(SM.srv, "host:", 5) && SM.pubip) ip = SM.pubip; // hosting: show our server so friends can join
  pthread_mutex_unlock(&SM.mu);
  buf b = {0};
  if (play) { buf g = {0}; if (gs) fvar(&g, 1, gs); ff64(&g, 2, SM_APP);
    if (ip) { fvar(&g, 3, ip); fvar(&g, 4, port); buf a = {0}; ff32(&a, 1, ip); fsub(&g, 23, &a); }
    fsub(&b, 1, &g); }
  fvar(&b, 2, SM_OS);
  sm_send(5410, &b, NULL); free(b.p);
  { char a[40] = ""; if (ip) sm_ipstr(ip, port, a, sizeof a); sw_log("[mp] games played: %s%s%s", play ? "GMod" : "nothing", ip ? " on server " : "", a); }
}
static void sm_send_authlist(void) {
  pthread_mutex_lock(&SM.mu);
  buf b = {0}; fvar(&b, 1, SM.ntok);
  if (SM.tk) { buf t = {0}; ff64(&t, 4, SM_APP); fvar(&t, 6, SM.tk_crc); fbytes(&t, 7, SM.tk, SM.tkn); fsub(&b, 4, &t); fvar(&b, 5, SM_APP); }
  uint64_t job = ++SM.job; pthread_mutex_unlock(&SM.mu);
  buf h = {0}; ff64(&h, 10, job); sm_send(5432, &b, &h); free(h.p); free(b.p);
}
static int sm_connect(void) {
  uint8_t *j; size_t jn; char e2[256] = "";
  int st = sw_http_get("https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&cmtype=websockets", &j, &jn, NULL, 0, 0);
  if (st != 200) { snprintf(SM.err, sizeof SM.err, "Steam unreachable (HTTP %d), internet ok?", st); return -1; }
  char *s = realloc(j, jn + 1); s[jn] = 0; char *p = s; int tries = 0;
  while ((p = strstr(p, "\"endpoint\":\"")) && tries < 5 && !SM.stop) {
    p += 12; char *q = strchr(p, '"'); if (!q) break;
    char url[300]; snprintf(url, sizeof url, "wss://%.*s/cmsocket/", (int)(q - p), p); p = q; tries++;
    sw_ws *ws = sw_ws_open(url, e2, sizeof e2); if (!ws) continue;
    pthread_mutex_lock(&SM.mu); char acct[128], tok[4096]; int anon = SM.anon; snprintf(acct, sizeof acct, "%s", SM.acct); snprintf(tok, sizeof tok, "%s", SM.tok);
    SM.got_logon = 0; SM.logon_eres = 0; SM.reconnect = 0; pthread_mutex_unlock(&SM.mu);
    pthread_mutex_lock(&SM.smu); SM.ws = ws; SM.sess = 0;
    SM.sid = anon ? (1ULL << 56) | (10ULL << 52) : (1ULL << 56) | (1ULL << 52) | (1ULL << 32); pthread_mutex_unlock(&SM.smu);
    buf b = {0};
    if (!anon) {
      buf hl = {0}; fvar(&hl, 1, 65580); sm_send(4006, &hl, NULL); free(hl.p);
      fvar(&b, 1, 65580); fvar(&b, 5, 1771); fstr(&b, 6, "english"); fvar(&b, 7, SM_OS); fvar(&b, 8, 1);
      { buf ip = {0}; ff32(&ip, 1, SM.loginid); fsub(&b, 11, &ip); }
      fvar(&b, 33, 2); fstr(&b, 50, acct); fstr(&b, 96, "GMod iOS"); fvar(&b, 102, 1); fstr(&b, 108, tok);
    } else { fvar(&b, 1, 65580); fvar(&b, 7, 16); fvar(&b, 5, 1771); }
    sm_send(5514, &b, NULL); free(b.p); memset(tok, 0, sizeof tok);
    time_t t0 = time(NULL);
    while (time(NULL) - t0 < 20 && !SM.got_logon && !SM.stop) {
      uint8_t *d; size_t n; int rr = sw_ws_recv(ws, &d, &n, 1000);
      if (rr < 0) break; if (rr) continue;
      sm_parse(d, n, 0); free(d);
    }
    int eres = SM.got_logon ? SM.logon_eres : -1;
    if (eres == 1) {
      sw_log("[mp] %s session up via %s, steamid %llu", anon ? "anonymous" : "account", url, (unsigned long long)SM.sid);
      free(s); return 0;
    }
    pthread_mutex_lock(&SM.smu); sw_ws_close(ws); SM.ws = NULL; pthread_mutex_unlock(&SM.smu);
    if (!anon && (eres == 5 || eres == 15 || eres == 18 || eres == 84)) { free(s); snprintf(SM.err, sizeof SM.err, "Steam login expired, log in again (code %d)", eres); return -2; }
    snprintf(e2, sizeof e2, eres < 0 ? "Steam didn't answer the login" : "Steam refused the login (code %d)", eres);
  }
  free(s); snprintf(SM.err, sizeof SM.err, "%s", e2[0] ? e2 : "no Steam servers"); return -1;
}
static void sm_request_personas(void) {
  pthread_mutex_lock(&SM.mu); time_t now = time(NULL); buf b = {0}; int n = 0;
  fvar(&b, 1, 1 | 2 | 8 | 16 | 64 | 256 | 4096);
  for (int i = 0; i < SM.ne && n < 64; i++) { sm_ent *x = &SM.e[i];
    if (x->sent || !(x->wanted || x->is_friend)) continue;
    if (x->last_req && now - x->last_req < 20) continue;
    x->sent = 1; x->last_req = now; ff64(&b, 2, x->sid); n++; }
  pthread_mutex_unlock(&SM.mu);
  if (n) sm_send(815, &b, NULL);
  free(b.p);
}
static void *sm_thread(void *a) {
  (void)a; int backoff = 0;
  while (!SM.stop) {
    if (!SM.ws || SM.reconnect) {
      if (SM.ws) sm_drop();
      for (int i = 0; i < backoff * 10 && !SM.stop; i++) sm_sleep(100);
      if (SM.stop) break;
      pthread_mutex_lock(&SM.mu); SM.state = 1; pthread_mutex_unlock(&SM.mu);
      int r = sm_connect();
      if (r == -2) { pthread_mutex_lock(&SM.mu); SM.state = -1; pthread_cond_broadcast(&SM.cv); pthread_mutex_unlock(&SM.mu); sw_log("[mp] %s", SM.err); break; }
      if (r) { sw_log("[mp] connect failed: %s", SM.err); backoff = backoff ? (backoff * 2 > 60 ? 60 : backoff * 2) : 3; continue; }
      backoff = 0;
      pthread_mutex_lock(&SM.mu); SM.state = 2; SM.err[0] = 0; SM.up_at = SM.last_hb = time(NULL); SM.adv_dirty = 1; SM.job = SM.job ? SM.job : 1;
      for (int i = 0; i < SM.ne; i++) SM.e[i].sent = 0, SM.e[i].last_req = 0;
      pthread_cond_broadcast(&SM.cv); pthread_mutex_unlock(&SM.mu);
      if (!SM.anon) { buf b = {0}; fvar(&b, 1, 1); sm_send(716, &b, NULL); free(b.p); } // persona: Online
      if (SM.tk) sm_send_authlist();
    }
    uint8_t *d; size_t n; int rr = sw_ws_recv(SM.ws, &d, &n, 1000);
    if (rr < 0) { sw_log("[mp] connection dropped, reconnecting"); sm_drop(); backoff = 2; continue; }
    if (!rr) { sm_parse(d, n, 0); free(d); }
    time_t now = time(NULL);
    if (now - SM.last_hb >= SM.hb) { SM.last_hb = now; buf b = {0}; sm_send(703, &b, NULL); }
    if (!SM.anon && SM.adv_dirty && (SM.play_known || now - SM.up_at >= 2)) sm_send_played();
    if (!SM.anon) sm_request_personas();
  }
  sm_drop();
  pthread_mutex_lock(&SM.mu); if (SM.state != -1) SM.state = 0; SM.th_on = 0; pthread_cond_broadcast(&SM.cv); pthread_mutex_unlock(&SM.mu);
  return NULL;
}

// ---------------- avatars ----------------
static void *sm_av_thread(void *a) {
  (void)a;
  for (;;) {
    pthread_mutex_lock(&SM.mu);
    sm_ent *x = NULL; int full = 0;
    for (int i = 0; i < SM.ne && !x; i++) { sm_ent *y = &SM.e[i]; if (!y->has_hash || !y->wanted) continue;
      if (!y->med && !y->med_fail) { x = y; full = 0; } else if (y->want_full && !y->full && !y->full_fail) { x = y; full = 1; } }
    if (!x) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 3;
      int t = pthread_cond_timedwait(&SM.cv, &SM.mu, &ts); if (t && SM.stop) { SM.av_on = 0; pthread_mutex_unlock(&SM.mu); return NULL; } pthread_mutex_unlock(&SM.mu); continue; }
    uint64_t sid = x->sid; uint8_t h[20]; memcpy(h, x->hash, 20);
    if (!sm_decode_image) { if (full) x->full_fail = 1; else x->med_fail = 1; pthread_mutex_unlock(&SM.mu); continue; }
    pthread_mutex_unlock(&SM.mu);
    char url[160]; int o = snprintf(url, sizeof url, "https://avatars.steamstatic.com/");
    for (int i = 0; i < 20; i++) o += snprintf(url + o, sizeof url - o, "%02x", h[i]);
    snprintf(url + o, sizeof url - o, full ? "_full.jpg" : "_medium.jpg");
    uint8_t *d = NULL; size_t n = 0; int st = sw_http_get(url, &d, &n, NULL, 0, 0);
    int sz = full ? 184 : 64; uint8_t *px = (st == 200 && n && n <= 262144) ? sm_decode_image(d, n, sz) : NULL; free(d);
    pthread_mutex_lock(&SM.mu);
    x = sm_find(sid, 0);
    if (x && !memcmp(x->hash, h, 20)) {
      if (full) { if (px) { free(x->full); x->full = px; px = NULL; } else x->full_fail = 1; }
      else { if (px) { free(x->med); x->med = px; px = NULL; } else x->med_fail = 1; }
      x->av_ver++; sm_changed(sid);
    }
    free(px); pthread_mutex_unlock(&SM.mu);
  }
}
static void sm_av_kick(void) { // mu held
  if (!SM.av_on) { pthread_t t; if (!pthread_create(&t, NULL, sm_av_thread, NULL)) { pthread_detach(t); SM.av_on = 1; } }
  pthread_cond_broadcast(&SM.cv);
}

// ---------------- public: session ----------------
static void sm_start_thread(void) { // mu held
  if (SM.th_on) return;
  SM.stop = 0; SM.state = 1;
  if (!SM.loginid) { uint32_t r = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)&r ^ 0x5bd1e995u; SM.loginid = (r & 0x7fffffff) | 1; }
  if (!pthread_create(&SM.th, NULL, sm_thread, NULL)) SM.th_on = 1; else SM.state = 0;
}
void sm_start(const char *acct, const char *refresh) {
  pthread_mutex_lock(&SM.mu);
  int anon = !acct || !*acct;
  int same = SM.have_id && SM.anon == anon && !strcmp(SM.acct, anon ? "" : acct) && !strcmp(SM.tok, anon ? "" : refresh);
  snprintf(SM.acct, sizeof SM.acct, "%s", anon ? "" : acct); snprintf(SM.tok, sizeof SM.tok, "%s", anon ? "" : refresh);
  SM.anon = anon; SM.have_id = 1;
  if (!same && SM.th_on) SM.reconnect = 1;
  if (SM.state == -1 && !same) SM.state = 0;
  if (SM.state != -1) sm_start_thread();
  pthread_mutex_unlock(&SM.mu);
}
void sm_stop(void) {
  pthread_mutex_lock(&SM.mu); int on = SM.th_on; SM.stop = 1; SM.have_id = 0; memset(SM.tok, 0, sizeof SM.tok); SM.acct[0] = 0;
  pthread_cond_broadcast(&SM.cv); pthread_mutex_unlock(&SM.mu);
  if (on) pthread_join(SM.th, NULL);
  pthread_mutex_lock(&SM.mu);
  SM.state = 0; SM.err[0] = 0;
  for (int i = 0; i < SM.ne; i++) { free(SM.e[i].med); free(SM.e[i].full); }
  SM.ne = 0; SM.nchg = 0; SM.serial++; SM.ninv = 0;
  free(SM.tk); SM.tk = NULL; free(SM.a_cookie); SM.a_cookie = NULL; SM.a_state = 0; SM.a_gen++;
  pthread_mutex_unlock(&SM.mu);
}
static void sm_ensure(void) { pthread_mutex_lock(&SM.mu); if (SM.have_id && SM.state != -1) sm_start_thread(); pthread_mutex_unlock(&SM.mu); }
int sm_state(char *msg, size_t mn) {
  pthread_mutex_lock(&SM.mu); int s = SM.have_id ? SM.state : 0; if (msg && mn) snprintf(msg, mn, "%s", SM.err); pthread_mutex_unlock(&SM.mu); return s;
}
uint64_t sm_steamid(void) { return SM.state == 2 && !SM.anon ? SM.sid : 0; }
// wait until online; returns 0 ok, else fills err
static int sm_wait_up(int secs, char *err, size_t errn) {
  sm_ensure();
  pthread_mutex_lock(&SM.mu);
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += secs;
  while (SM.have_id && SM.state != 2 && SM.state != -1) if (pthread_cond_timedwait(&SM.cv, &SM.mu, &ts)) break;
  int st = SM.have_id ? SM.state : 0;
  if (st != 2) snprintf(err, errn, "%s", !SM.have_id ? "Log into Steam in the launcher first (Steam button)" : SM.err[0] ? SM.err : "Steam is taking forever to connect, try again");
  pthread_mutex_unlock(&SM.mu);
  return st == 2 ? 0 : -1;
}
void sm_set_playing(int on) { pthread_mutex_lock(&SM.mu); SM.playing = on; SM.adv_dirty = 1; pthread_mutex_unlock(&SM.mu); }
void sm_set_host(int port) { // hosting a listen server: invites point to our public ip
  pthread_mutex_lock(&SM.mu);
  if (port > 0) { SM.srv_port = (uint16_t)port; SM.srv_ip = 0; SM.srv_sid = 0; snprintf(SM.srv, sizeof SM.srv, "host:%d", port); }
  else { SM.srv[0] = 0; SM.srv_ip = 0; SM.srv_port = 0; SM.srv_sid = 0; }
  SM.playing = 1; SM.adv_dirty = 1;
  pthread_mutex_unlock(&SM.mu);
}
void sm_set_server(const char *addr, int port) { // joined a remote server (seen in the engine log)
  pthread_mutex_lock(&SM.mu);
  if (addr && *addr) { SM.srv_ip = sm_parse_ip(addr); SM.srv_port = (uint16_t)port; SM.srv_sid = 0; snprintf(SM.srv, sizeof SM.srv, "%s:%d", addr, port); }
  else { SM.srv_ip = 0; SM.srv_port = 0; SM.srv_sid = 0; SM.srv[0] = 0; }
  SM.playing = 1; SM.adv_dirty = 1; pthread_mutex_unlock(&SM.mu);
}
int sm_server(char *o, size_t on) { // current "+connect" target for invites
  pthread_mutex_lock(&SM.mu); int ok = 0;
  if (!strncmp(SM.srv, "host:", 5)) { if (SM.pubip) { char a[40]; sm_ipstr(SM.pubip, SM.srv_port, a, sizeof a); snprintf(o, on, "%s", a); ok = 1; } }
  else if (SM.srv[0]) { snprintf(o, on, "%s", SM.srv); ok = 1; }
  pthread_mutex_unlock(&SM.mu); return ok;
}

// ---------------- public: game server auth (GModSteamAuth) ----------------
typedef struct { int gen; char addr[100]; int port; uint64_t ssid; } sm_aj;
static int sm_auth_live(int gen) { return SM.a_gen == gen && !SM.stop; }
static int g_a_lan;
// LAN / local targets keep working like before (no ticket = state 0) when Steam auth fails
static void sm_auth_fail(int gen, const char *m) { pthread_mutex_lock(&SM.mu); if (SM.a_gen == gen) { snprintf(SM.a_err, sizeof SM.a_err, "%s", m); SM.a_state = g_a_lan ? 0 : -1; } pthread_mutex_unlock(&SM.mu); sw_log("[mp] auth: %s%s", m, g_a_lan ? " (LAN target, going on without Steam)" : ""); }
static int sm_private_ip(uint32_t ip) { return !ip || (ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 16) == 0xc0a8 || (ip >> 20) == 0xac1 || (ip >> 16) == 0xa9fe || (ip >> 22) == (100 << 2 | 1); }
static void *sm_auth_thread(void *a) {
  sm_aj j = *(sm_aj *)a; free(a); char err[300];
  if (sm_wait_up(60, err, sizeof err)) { sm_auth_fail(j.gen, err); return NULL; }
  if (SM.anon) { sm_auth_fail(j.gen, "Log into Steam in the launcher first (Steam button)"); return NULL; }
  sm_send_played(); // Steam wants us "in GMod" before it accepts tickets
  // app ownership ticket
  buf b = {0}; fvar(&b, 1, SM_APP); uint8_t *o; size_t on; int er;
  int r = sm_call(857, &b, NULL, &o, &on, &er, 25); free(b.p);
  if (r) { sm_auth_fail(j.gen, "Steam didn't hand out a game ticket, rejoin"); return NULL; }
  int eres = 2; uint8_t *at = NULL; size_t an = 0; pbr p; pb_init(&p, o, on);
  while (pb_next(&p)) { if (p.f == 1) eres = (int)p.v; else if (p.f == 3 && p.d) { at = malloc(p.dn); memcpy(at, p.d, p.dn); an = p.dn; } }
  free(o);
  if (eres != 1 || !at) { free(at); snprintf(err, sizeof err, eres == 15 || eres == 2 ? "Steam says this account doesn't own GMod (code %d)" : "Steam game ticket failed (code %d)", eres); sm_auth_fail(j.gen, err); return NULL; }
  // game connect token
  pthread_mutex_lock(&SM.mu);
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 10;
  while (!SM.ntok && sm_auth_live(j.gen) && SM.state == 2) if (pthread_cond_timedwait(&SM.cv, &SM.mu, &ts)) break;
  if (!SM.ntok || !sm_auth_live(j.gen)) { pthread_mutex_unlock(&SM.mu); free(at); if (sm_auth_live(j.gen)) sm_auth_fail(j.gen, "Steam gave no game tokens, rejoin in a sec"); return NULL; }
  uint8_t *tk = SM.tq[0]; size_t tn = SM.tqn[0]; memmove(SM.tq, SM.tq + 1, sizeof SM.tq[0] * (SM.ntok - 1)); memmove(SM.tqn, SM.tqn + 1, sizeof SM.tqn[0] * (SM.ntok - 1)); SM.ntok--;
  uint64_t me = SM.sid;
  pthread_mutex_unlock(&SM.mu);
  // auth session ticket = len|token|24|1|2|rand8|time|seq  (SteamKit BuildAuthTicket)
  static uint32_t seq; uint32_t u;
  buf t = {0}; u = (uint32_t)tn; bput(&t, &u, 4); bput(&t, tk, tn); free(tk);
  u = 24; bput(&t, &u, 4); u = 1; bput(&t, &u, 4); u = 2; bput(&t, &u, 4);
  { uint32_t rnd[2]; for (int i = 0; i < 2; i++) rnd[i] = (uint32_t)rand() ^ (uint32_t)clock() << 16; bput(&t, rnd, 8); }
  { struct timespec mt; clock_gettime(CLOCK_MONOTONIC, &mt); u = (uint32_t)(mt.tv_sec * 1000000ULL + mt.tv_nsec / 1000); bput(&t, &u, 4); }
  u = ++seq; bput(&t, &u, 4);
  uint32_t crc = (uint32_t)crc32(0, t.p, (uInt)t.n);
  pthread_mutex_lock(&SM.mu);
  if (!sm_auth_live(j.gen)) { pthread_mutex_unlock(&SM.mu); free(t.p); free(at); return NULL; }
  free(SM.tk); SM.tk = malloc(t.n); memcpy(SM.tk, t.p, t.n); SM.tkn = t.n; SM.tk_crc = crc;
  pthread_mutex_unlock(&SM.mu);
  sm_send_authlist();
  // full ticket = auth | len(app) | app ; cookie = steamid(LE) | full
  buf c = {0}; uint64_t sid = me; bput(&c, &sid, 8); bput(&c, t.p, t.n); u = (uint32_t)an; bput(&c, &u, 4); bput(&c, at, an); free(at); free(t.p);
  if (c.n - 8 > 2040) { free(c.p); sm_auth_fail(j.gen, "Steam ticket came out too big, weird"); return NULL; }
  pthread_mutex_lock(&SM.mu);
  if (sm_auth_live(j.gen)) { free(SM.a_cookie); SM.a_cookie = c.p; SM.a_cn = c.n; SM.a_crc = crc; SM.a_state = 2; c.p = NULL; }
  pthread_mutex_unlock(&SM.mu);
  free(c.p);
  sw_log("[mp] auth ticket ready for %s:%d (crc %08x, %zu bytes)", j.addr, j.port, crc, (size_t)SM.a_cn);
  return NULL;
}
void sm_auth_request(const char *addr, int port, uint64_t ssid, int secure) {
  char key[200]; snprintf(key, sizeof key, "%s:%d:%llu:%d", addr ? addr : "", port, (unsigned long long)ssid, secure);
  pthread_mutex_lock(&SM.mu);
  if (!SM.have_id || SM.anon) { SM.a_state = 0; pthread_mutex_unlock(&SM.mu); sw_log("[mp] auth request %s: no Steam login, joining without a ticket", key); return; }
  g_a_lan = sm_private_ip(sm_parse_ip(addr));
  if (SM.a_state > 0 && !strcmp(key, SM.a_key)) { pthread_mutex_unlock(&SM.mu); return; }
  SM.a_gen++; SM.a_state = 1; SM.a_err[0] = 0; snprintf(SM.a_key, sizeof SM.a_key, "%s", key); SM.a_srvsid = ssid;
  free(SM.a_cookie); SM.a_cookie = NULL; SM.a_cn = 0;
  if (!(strncmp(SM.srv, "host:", 5) == 0 && sm_private_ip(sm_parse_ip(addr)))) { // joining our own listen server keeps the host ad
  SM.srv_ip = sm_parse_ip(addr); SM.srv_port = (uint16_t)port; SM.srv_sid = ssid; SM.adv_dirty = 1; SM.playing = 1;
  if (addr && *addr) snprintf(SM.srv, sizeof SM.srv, "%s:%d", addr, port); }
  sm_aj *j = malloc(sizeof *j); j->gen = SM.a_gen; snprintf(j->addr, sizeof j->addr, "%s", addr ? addr : ""); j->port = port; j->ssid = ssid;
  pthread_mutex_unlock(&SM.mu);
  sw_log("[mp] auth request %s", key);
  pthread_t t; if (pthread_create(&t, NULL, sm_auth_thread, j)) { free(j); sm_auth_fail(SM.a_gen, "Couldn't start Steam auth"); } else pthread_detach(t);
}
int sm_auth_state(void) { return SM.a_state; }
int sm_auth_error(char *o, size_t on) { pthread_mutex_lock(&SM.mu); snprintf(o, on, "%s", SM.a_state == -1 ? SM.a_err : ""); pthread_mutex_unlock(&SM.mu); return 0; }
uint64_t sm_auth_steamid(void) { return SM.a_state != 0 && !SM.anon ? SM.sid : 0; }
int sm_auth_cookie(uint8_t *o, size_t on, uint32_t *crc) {
  pthread_mutex_lock(&SM.mu); int n = 0;
  if (SM.a_state >= 2 && SM.a_cookie && SM.a_cn <= on) { memcpy(o, SM.a_cookie, SM.a_cn); n = (int)SM.a_cn; if (crc) *crc = SM.a_crc; }
  pthread_mutex_unlock(&SM.mu); return n;
}
void sm_auth_cancel(void) {
  pthread_mutex_lock(&SM.mu);
  int had = SM.a_state != 0 || SM.tk;
  SM.a_gen++; SM.a_state = 0; SM.a_key[0] = 0; free(SM.a_cookie); SM.a_cookie = NULL; SM.a_cn = 0;
  free(SM.tk); SM.tk = NULL; SM.tkn = 0;
  if (strncmp(SM.srv, "host:", 5)) { SM.srv[0] = 0; SM.srv_ip = 0; SM.srv_port = 0; SM.srv_sid = 0; }
  SM.adv_dirty = 1;
  pthread_mutex_unlock(&SM.mu);
  if (had && SM.state == 2) sm_send_authlist();
}

// ---------------- public: internet server directory (GModServerDirectory) ----------------
typedef struct { int id, used, cancel, done; char **ev; int nev, cev, rd; } sm_dir;
static sm_dir SMD[8]; static int g_smd_next = 1;
static void smd_add(sm_dir *d, const char *fmt, ...) {
  char t[300]; va_list a; va_start(a, fmt); vsnprintf(t, sizeof t, fmt, a); va_end(a);
  pthread_mutex_lock(&SM.mu);
  if (d->nev == d->cev) { d->cev = d->cev ? d->cev * 2 : 256; d->ev = realloc(d->ev, sizeof(char *) * d->cev); }
  d->ev[d->nev++] = strdup(t); pthread_mutex_unlock(&SM.mu);
}
static void smd_server(const uint8_t *d, size_t n, uint32_t *ip, uint32_t *port) {
  pbr r; pb_init(&r, d, n);
  while (pb_next(&r)) {
    if (r.f == 1 && r.v) *ip = (uint32_t)r.v; else if (r.f == 2 && r.v) *port = (uint32_t)r.v;
    else if (r.f == 4 && r.d) { pbr q; pb_init(&q, r.d, r.dn); while (pb_next(&q)) if (q.f == 1 && q.v) *ip = (uint32_t)q.v; }
  }
}
static void *smd_thread(void *a) {
  sm_dir *d = a; char err[300];
  if (sm_wait_up(40, err, sizeof err)) { smd_add(d, "error=%s", err); goto done; }
  {
    buf q = {0}; fvar(&q, 1, SM_APP); fvar(&q, 3, 255); fstr(&q, 4, "\\appid\\4000"); fvar(&q, 5, 20000);
    buf b = {0}; fvar(&b, 1, SM_APP); fsub(&b, 2, &q);
    uint8_t *o; size_t on; int er;
    int r = sm_call(151, &b, "GameServers.MasterServerQuery#1", &o, &on, &er, 30); free(b.p);
    if (d->cancel) { free(o); goto done; }
    if (r) { smd_add(d, "error=Steam server list timed out, refresh"); goto done; }
    if (er != 1) { free(o); smd_add(d, er == 15 ? "error=Steam said access denied, log in again in the launcher" : "error=Steam server list failed (code %d)", er); goto done; }
    const uint8_t *resp = NULL; size_t rn = 0; pbr p; pb_init(&p, o, on);
    while (pb_next(&p)) if (p.f == 1 && p.d) { resp = p.d; rn = p.dn; }
    uint32_t dip = 0, dport = 0; char e2[200] = "";
    pb_init(&p, resp, rn);
    while (pb_next(&p)) { if (p.f == 3 && p.d) smd_server(p.d, p.dn, &dip, &dport); else if (p.f == 2 && p.d) pb_str(&p, e2, sizeof e2); }
    if (e2[0]) { free(o); smd_add(d, "error=Steam refused the server list: %s", e2); goto done; }
    int cnt = 0; pb_init(&p, resp, rn);
    while (pb_next(&p) && !d->cancel) {
      if (p.f != 1 || !p.d) continue;
      uint32_t ip = dip, port = dport; smd_server(p.d, p.dn, &ip, &port);
      if (!ip || !port || port > 65535) continue;
      char s[40]; sm_ipstr(ip, (uint16_t)port, s, sizeof s); smd_add(d, "address=%s", s); cnt++;
    }
    free(o);
    sw_log("[mp] server list: %d servers", cnt);
    if (!cnt) smd_add(d, "error=Steam returned no GMod servers");
  }
done:
  pthread_mutex_lock(&SM.mu); d->done = 1; pthread_mutex_unlock(&SM.mu);
  return NULL;
}
int sm_dir_start(void) {
  pthread_mutex_lock(&SM.mu); sm_dir *d = NULL;
  for (int i = 0; i < 8; i++) if (!SMD[i].used) { d = &SMD[i]; break; }
  if (!d) { pthread_mutex_unlock(&SM.mu); return 0; }
  memset(d, 0, sizeof *d); d->used = 1; d->id = g_smd_next++;
  pthread_mutex_unlock(&SM.mu);
  pthread_t t; if (pthread_create(&t, NULL, smd_thread, d)) { d->used = 0; return 0; }
  pthread_detach(t); return d->id;
}
// moves up to max events into out (caller frees each). returns count, -1 when the job is unknown (-> "done")
int sm_dir_poll(int id, char **out, int max) {
  pthread_mutex_lock(&SM.mu); sm_dir *d = NULL;
  for (int i = 0; i < 8; i++) if (SMD[i].used && SMD[i].id == id) d = &SMD[i];
  if (!d) { pthread_mutex_unlock(&SM.mu); return -1; }
  int n = 0; while (n < max && d->rd < d->nev) { out[n++] = d->ev[d->rd]; d->ev[d->rd++] = NULL; }
  if (n < max && d->done && d->rd == d->nev) { out[n++] = strdup("done"); free(d->ev); d->ev = NULL; d->nev = d->cev = d->rd = 0; d->used = 0; }
  pthread_mutex_unlock(&SM.mu); return n;
}
void sm_dir_cancel(int id) {
  pthread_mutex_lock(&SM.mu);
  for (int i = 0; i < 8; i++) if (SMD[i].used && SMD[i].id == id) { SMD[i].cancel = 1;
    if (SMD[i].done) { for (int k = SMD[i].rd; k < SMD[i].nev; k++) free(SMD[i].ev[k]); free(SMD[i].ev); SMD[i].ev = NULL; SMD[i].used = 0; } }
  pthread_mutex_unlock(&SM.mu);
}

// ---------------- public: persona (GModSteamPersona) + friends ----------------
int sm_persona_available(void) { return SM.state == 2 && !SM.anon; }
void sm_persona_request(uint64_t sid, int full) {
  if (!sm_individual(sid)) return;
  sm_ensure();
  pthread_mutex_lock(&SM.mu);
  sm_ent *x = sm_find(sid, 1);
  if (x) { x->wanted = 1; x->want_full |= full; x->med_fail = 0; if (full) x->full_fail = 0;
    if (x->name[0] && x->has_hash) sm_changed(sid);
    if (!x->last_req || time(NULL) - x->last_req > 60) x->sent = 0;
    sm_av_kick(); }
  pthread_mutex_unlock(&SM.mu);
}
uint64_t sm_persona_serial(void) { return SM.serial; }
int sm_persona_changed(uint64_t *o, int max) {
  pthread_mutex_lock(&SM.mu); int n = SM.nchg < max ? SM.nchg : max; memcpy(o, SM.chg, sizeof(uint64_t) * n); SM.nchg = 0; pthread_mutex_unlock(&SM.mu); return n;
}
int sm_persona_name(uint64_t sid, char *o, size_t on, int *ver) {
  pthread_mutex_lock(&SM.mu); sm_ent *x = sm_find(sid, 0); int ok = x && x->name[0];
  if (ok) { snprintf(o, on, "%s", x->name); if (ver) *ver = x->name_ver; } pthread_mutex_unlock(&SM.mu); return ok;
}
int sm_relationship(uint64_t sid) {
  if (SM.state != 2 || SM.anon || !sm_individual(sid)) return -1;
  pthread_mutex_lock(&SM.mu); sm_ent *x = sm_find(sid, 0); int r = x ? x->rel : 0; pthread_mutex_unlock(&SM.mu); return r;
}
// size 64 or 184. out NULL -> just report version. returns 1 if available
int sm_avatar(uint64_t sid, int size, uint8_t *out, int *ver) {
  pthread_mutex_lock(&SM.mu); sm_ent *x = sm_find(sid, 0); uint8_t *p = x ? (size == 184 ? x->full : size == 64 ? x->med : NULL) : NULL;
  if (p) { if (ver) *ver = x->av_ver; if (out) memcpy(out, p, (size_t)size * size * 4); }
  pthread_mutex_unlock(&SM.mu); return p != NULL;
}
int sm_friends(sm_friend *o, int max) {
  pthread_mutex_lock(&SM.mu); int n = 0;
  for (int i = 0; i < SM.ne && n < max; i++) { sm_ent *x = &SM.e[i]; if (!x->is_friend) continue;
    sm_friend *f = &o[n++]; memset(f, 0, sizeof *f); f->sid = x->sid; snprintf(f->name, sizeof f->name, "%s", x->name[0] ? x->name : "…");
    f->state = x->pstate; f->app = x->app; snprintf(f->game, sizeof f->game, "%s", x->game);
    f->addr[0] = 0;
    if (x->app == SM_APP) { if (x->ip && x->port) sm_ipstr(x->ip, x->port, f->addr, sizeof f->addr); else if (x->connect[0]) sm_addr_from(x->connect, f->addr, sizeof f->addr); }
    if (!x->wanted) { x->wanted = 1; } }
  sm_av_kick();
  pthread_mutex_unlock(&SM.mu); return n;
}
int sm_invite(uint64_t to, const char *connect) {
  if (SM.state != 2 || SM.anon) return -1;
  buf b = {0}; ff64(&b, 1, to); ff64(&b, 2, SM.sid); fstr(&b, 3, connect);
  buf h = {0}; fvar(&h, 3, SM_APP);
  int r = sm_send(7005, &b, &h); free(b.p); free(h.p);
  { // also as a Steam chat game invite (what the real client sends), shows up on PC/phone Steam too
    char m[300]; snprintf(m, sizeof m, "[gameinvite appid=\"%d\" connect=\"%s\"][/gameinvite]", SM_APP, connect);
    buf c = {0}; ff64(&c, 1, to); fvar(&c, 2, 1); fstr(&c, 3, m); fvar(&c, 4, 1);
    pthread_mutex_lock(&SM.mu); uint64_t job = ++SM.job; pthread_mutex_unlock(&SM.mu);
    buf ch = {0}; ff64(&ch, 10, job); fstr(&ch, 12, "FriendMessages.SendMessage#1");
    int r2 = sm_send(151, &c, &ch); free(c.p); free(ch.p);
    sw_log("[mp] chat invite -> %llu (%d)", (unsigned long long)to, r2);
    if (r) r = r2; }
  sw_log("[mp] invite -> %llu '%s' (%d)", (unsigned long long)to, connect, r);
  return r;
}
// pops one incoming invite. addr = "a.b.c.d:port" or "" when it isn't a plain server (lobby etc)
int sm_take_invite(uint64_t *from, char *name, size_t nn, char *addr, size_t an, char *raw, size_t rn) {
  pthread_mutex_lock(&SM.mu);
  if (!SM.ninv) { pthread_mutex_unlock(&SM.mu); return 0; }
  *from = SM.inv[0].from; snprintf(raw, rn, "%s", SM.inv[0].connect);
  if (!sm_addr_from(SM.inv[0].connect, addr, an)) addr[0] = 0;
  sm_ent *x = sm_find(*from, 0); snprintf(name, nn, "%s", x && x->name[0] ? x->name : "A Steam friend");
  memmove(SM.inv, SM.inv + 1, sizeof SM.inv[0] * (SM.ninv - 1)); SM.ninv--;
  pthread_mutex_unlock(&SM.mu); return 1;
}

#ifdef SG_TEST_MAIN
int sm_test(const char *acct, const char *tok) {
  sm_start(acct, tok); char e[300];
  if (sm_wait_up(40, e, sizeof e)) { printf("UP FAIL %s\n", e); return 1; }
  printf("UP steamid %llu pubip %08x hb %d\n", (unsigned long long)SM.sid, SM.pubip, SM.hb);
  int id = sm_dir_start(); int total = 0; char *ev[256]; time_t t0 = time(NULL);
  for (int fin = 0; !fin && time(NULL) - t0 < 60;) { int n = sm_dir_poll(id, ev, 256); if (n < 0) break;
    for (int i = 0; i < n; i++) { if (!strcmp(ev[i], "done")) fin = 1; else if (!strncmp(ev[i], "error=", 6)) printf("%s\n", ev[i]); else if (total++ < 3) printf("%s\n", ev[i]); free(ev[i]); }
    if (!n) sm_sleep(200); }
  printf("servers %d\n", total);
  sm_sleep(12000); printf("still state %d after heartbeat\n", SM.state);
  if (!SM.anon) { sm_auth_request("1.2.3.4", 27015, 0, 0); for (int i = 0; i < 100 && sm_auth_state() == 1; i++) sm_sleep(200);
    char ae[300]; sm_auth_error(ae, sizeof ae); printf("auth state %d %s\n", sm_auth_state(), ae); }
  sm_stop(); return 0;
}
#endif
