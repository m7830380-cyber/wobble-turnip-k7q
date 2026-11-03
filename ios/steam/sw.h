// Minimal Steam client: anonymous CM session over WebSocket + SteamPipe CDN workshop downloads.
// Mirrors GDroid's JavaSteam-based SteamDownloads/GmodAddons install layout.
#pragma once
#include <stddef.h>
#include <stdint.h>

// ---- platform transport (ios/steam/net_ios.m on device, net_curl.c for host tests) ----
typedef struct sw_ws sw_ws;
sw_ws *sw_ws_open(const char *url, char *err, size_t errn);
int sw_ws_send(sw_ws *w, const void *p, size_t n);
int sw_ws_recv(sw_ws *w, uint8_t **out, size_t *n, int timeout_ms); // 0 ok, malloc'd *out
void sw_ws_close(sw_ws *w);
// returns HTTP status (<0 transport error); *out malloc'd
int sw_http_get(const char *url, uint8_t **out, size_t *n, volatile int *cancel, void (*prog)(void *, size_t), void *ud);
int sw_aes_ecb_dec(const uint8_t key[32], const uint8_t in[16], uint8_t out[16]);
int sw_aes_cbc_dec(const uint8_t key[32], const uint8_t iv[16], const uint8_t *in, size_t n, uint8_t *out);

// ---- API ----
typedef struct {
  void (*status)(void *ud, const char *text, uint64_t done, uint64_t total);
  void *ud;
  volatile int *cancel;
} sw_progress;

typedef struct {
  uint64_t id; uint32_t app, time_updated, file_type; uint64_t hcontent, file_size;
  char title[256], file_url[1024], filename[256];
  char tags[16][64]; int ntags;
  uint64_t *children; int nchildren;
  int result;
} sw_details;

// Resolve ids/URLs/collections in `spec` (comma/space separated) to a list of item ids.
int sw_resolve(const char *spec, uint64_t **ids, int *n, sw_progress *p, char *err, size_t errn);
// Download + install one item. root = content root containing garrysmod/; join=1 -> cache/workshop_join
int sw_install(const char *root, uint64_t id, int join, sw_progress *p, char *err, size_t errn);
// Already installed with at least this time (and crc if nonzero)?
int sw_deps(const char *root, const uint64_t *ids, int n, uint64_t **deps, char (**titles)[128], int *nd, sw_progress *p, char *err, size_t errn);
int sw_installed(const char *root, uint64_t id, uint64_t time, uint64_t crc, int join);
void sw_log(const char *fmt, ...);

// ---- extra transport for account login / game download ----
// generic request; method "GET"/"POST"; body may be NULL. *xeres = x-eresult header (or 1 if absent)
int sw_http_req(const char *method, const char *url, const void *body, size_t bn, const char *ctype, uint8_t **out, size_t *n, int *xeres);
// RSA PKCS#1 v1.5 encrypt with (modulus, exponent) big-endian bytes. returns output length or -1
int sw_rsa_pkcs1(const uint8_t *mod, size_t mn, const uint8_t *ex, size_t en, const uint8_t *in, size_t inn, uint8_t *out, size_t outn);
void sw_sha1(const void *p, size_t n, uint8_t out[20]);

// ---- Steam account + game content (sg.inc, part of sw.c) ----
// begin password login. *guard bits: 1 = Steam app code, 2 = email code, 4 = approve in Steam app. 0 = nothing needed
int sg_auth_begin(const char *user, const char *pass, int *guard, char *hint, size_t hn, char *err, size_t errn);
int sg_auth_code(const char *code, int email, char *err, size_t errn);
// blocks until Steam hands out tokens (after code / app approval), up to 5 min
int sg_auth_poll(char *refresh, size_t rn, char *acct, size_t an, volatile int *cancel, char *err, size_t errn);
typedef struct { uint64_t total, checked, downloaded, bad_chunks, files; } sg_stats;
// install / update / verify GMod content (depots 4001+4002) into root/{garrysmod,sourceengine,platform}
// returns 0 ok, -1 error, -2 saved login expired (log in again)
int sg_game_sync(const char *root, const char *acct, const char *refresh, int verify, sw_progress *p, sg_stats *st, char *err, size_t errn);

// ---- Steam multiplayer session (smp.inc, part of sw.c) ----
typedef struct { uint64_t sid; char name[64]; int state; uint32_t app; char game[64]; char addr[64]; } sm_friend;
void sm_start(const char *acct, const char *refresh);   // NULL acct = anonymous; starts/keeps a background session
void sm_stop(void);
int sm_state(char *msg, size_t mn);                      // 0 off, 1 connecting, 2 online, -1 login expired
uint64_t sm_steamid(void);
void sm_set_playing(int on);
void sm_set_host(int port);
void sm_set_host_ip(uint32_t ip); // 0 = public ip (Tailscale host uses its 100.x ip)
void sm_set_server(const char *addr, int port);
int sm_server(char *o, size_t on);
void sm_auth_request(const char *addr, int port, uint64_t server_sid, int secure);
int sm_auth_state(void);
int sm_auth_error(char *o, size_t on);
uint64_t sm_auth_steamid(void);
int sm_auth_cookie(uint8_t *o, size_t on, uint32_t *crc);
void sm_auth_cancel(void);
int sm_dir_start(void);
int sm_dir_poll(int id, char **out, int max);
void sm_dir_cancel(int id);
int sm_persona_available(void);
void sm_persona_request(uint64_t sid, int full);
uint64_t sm_persona_serial(void);
int sm_persona_changed(uint64_t *o, int max);
int sm_persona_name(uint64_t sid, char *o, size_t on, int *ver);
int sm_relationship(uint64_t sid);
int sm_avatar(uint64_t sid, int size, uint8_t *out, int *ver);
int sm_friends(sm_friend *o, int max);
int sm_invite(uint64_t to, const char *connect);
int sm_take_invite(uint64_t *from, char *name, size_t nn, char *addr, size_t an, char *raw, size_t rn);
extern unsigned char *(*sm_decode_image)(const uint8_t *d, size_t n, int size); // RGBA size*size, malloc'd
