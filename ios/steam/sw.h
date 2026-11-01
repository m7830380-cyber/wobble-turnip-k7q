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
int sw_installed(const char *root, uint64_t id, uint64_t time, uint64_t crc, int join);
void sw_log(const char *fmt, ...);
