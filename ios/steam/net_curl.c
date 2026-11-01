// Host test transport: libcurl websockets + OpenSSL AES.
#include "sw.h"
#include <curl/curl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct sw_ws { CURL *c; };
sw_ws *sw_ws_open(const char *url, char *err, size_t errn) {
  CURL *c = curl_easy_init(); curl_easy_setopt(c, CURLOPT_URL, url); curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L); curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
  CURLcode r = curl_easy_perform(c); if (r) { snprintf(err, errn, "%s", curl_easy_strerror(r)); curl_easy_cleanup(c); return NULL; }
  sw_ws *w = calloc(1, sizeof *w); w->c = c; return w;
}
int sw_ws_send(sw_ws *w, const void *p, size_t n) { size_t s; return curl_ws_send(w->c, p, n, &s, 0, CURLWS_BINARY) ? -1 : 0; }
int sw_ws_recv(sw_ws *w, uint8_t **out, size_t *n, int timeout_ms) {
  uint8_t *b = NULL; size_t bn = 0; time_t t0 = time(NULL);
  for (;;) {
    uint8_t tmp[65536]; size_t got; const struct curl_ws_frame *m;
    CURLcode r = curl_ws_recv(w->c, tmp, sizeof tmp, &got, &m);
    if (r == CURLE_AGAIN) { if (!b && (time(NULL) - t0) * 1000 > timeout_ms) return 1; usleep(5000); continue; }
    if (r) { free(b); return -1; }
    b = realloc(b, bn + got); memcpy(b + bn, tmp, got); bn += got;
    if (m->flags & CURLWS_CLOSE) { free(b); return -1; }
    if (!m->bytesleft && !(m->flags & CURLWS_CONT)) { *out = b; *n = bn; return 0; }
  }
}
void sw_ws_close(sw_ws *w) { curl_easy_cleanup(w->c); free(w); }
typedef struct { uint8_t *p; size_t n; volatile int *cancel; } hb;
static size_t wcb(char *d, size_t s, size_t k, void *u) { hb *h = u; if (h->cancel && *h->cancel) return 0; h->p = realloc(h->p, h->n + s * k); memcpy(h->p + h->n, d, s * k); h->n += s * k; return s * k; }
int sw_http_get(const char *url, uint8_t **out, size_t *n, volatile int *cancel, void (*prog)(void *, size_t), void *ud) {
  (void)prog; (void)ud; CURL *c = curl_easy_init(); hb h = {0}; h.cancel = cancel;
  curl_easy_setopt(c, CURLOPT_URL, url); curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb); curl_easy_setopt(c, CURLOPT_WRITEDATA, &h);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L); curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
  CURLcode r = curl_easy_perform(c); long st = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st); curl_easy_cleanup(c);
  if (r) { free(h.p); return -1; }
  *out = h.p ? h.p : malloc(1); *n = h.n; return (int)st;
}
int sw_aes_ecb_dec(const uint8_t key[32], const uint8_t in[16], uint8_t out[16]) {
  EVP_CIPHER_CTX *x = EVP_CIPHER_CTX_new(); int l, l2; EVP_DecryptInit_ex(x, EVP_aes_256_ecb(), 0, key, 0); EVP_CIPHER_CTX_set_padding(x, 0);
  int r = EVP_DecryptUpdate(x, out, &l, in, 16) && EVP_DecryptFinal_ex(x, out + l, &l2); EVP_CIPHER_CTX_free(x); return r ? 0 : -1;
}
int sw_aes_cbc_dec(const uint8_t key[32], const uint8_t iv[16], const uint8_t *in, size_t n, uint8_t *out) {
  EVP_CIPHER_CTX *x = EVP_CIPHER_CTX_new(); int l, l2; EVP_DecryptInit_ex(x, EVP_aes_256_cbc(), 0, key, iv); EVP_CIPHER_CTX_set_padding(x, 0);
  int r = EVP_DecryptUpdate(x, out, &l, in, (int)n) && EVP_DecryptFinal_ex(x, out + l, &l2); EVP_CIPHER_CTX_free(x); return r ? 0 : -1;
}
#ifdef SW_TEST_MAIN
static void st(void *u, const char *t, uint64_t d, uint64_t tot) { (void)u; static time_t last; if (time(NULL) != last || d == tot) { last = time(NULL); fprintf(stderr, "  %s %llu/%llu\n", t, (unsigned long long)d, (unsigned long long)tot); } }
int main(int argc, char **argv) {
  curl_global_init(CURL_GLOBAL_ALL); char err[512]; uint64_t *ids; int n; sw_progress p = {st, 0, 0};
  if (sw_resolve(argv[2], &ids, &n, &p, err, sizeof err)) return 1;
  for (int i = 0; i < n; i++) { fprintf(stderr, "item %llu\n", (unsigned long long)ids[i]); if (sw_install(argv[1], ids[i], 0, &p, err, sizeof err)) fprintf(stderr, "FAIL %s\n", err); }
  return 0;
}
#endif
