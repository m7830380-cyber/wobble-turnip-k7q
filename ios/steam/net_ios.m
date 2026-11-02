// iOS transport for the Steam client: NSURLSessionWebSocketTask, NSURLSession, CommonCrypto.
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>
#include <Security/Security.h>
#include "sw.h"

struct sw_ws { void *task; void *sess; };

sw_ws *sw_ws_open(const char *url, char *err, size_t errn) {
  NSURLSessionConfiguration *c = [NSURLSessionConfiguration ephemeralSessionConfiguration];
  c.timeoutIntervalForRequest = 30;
  NSURLSession *s = [NSURLSession sessionWithConfiguration:c];
  NSURLSessionWebSocketTask *t = [s webSocketTaskWithURL:[NSURL URLWithString:@(url)]];
  t.maximumMessageSize = 64 << 20;
  [t resume];
  // Probe the connection with a ping so failures surface here.
  dispatch_semaphore_t sem = dispatch_semaphore_create(0); __block NSError *pe = nil;
  [t sendPingWithPongReceiveHandler:^(NSError *e) { pe = e; dispatch_semaphore_signal(sem); }];
  if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC)) || pe) {
    snprintf(err, errn, "%s", pe ? pe.localizedDescription.UTF8String : "timeout");
    [t cancel]; [s invalidateAndCancel]; return NULL;
  }
  sw_ws *w = calloc(1, sizeof *w); w->task = (__bridge_retained void *)t; w->sess = (__bridge_retained void *)s; return w;
}
int sw_ws_send(sw_ws *w, const void *p, size_t n) {
  NSURLSessionWebSocketTask *t = (__bridge NSURLSessionWebSocketTask *)w->task;
  dispatch_semaphore_t sem = dispatch_semaphore_create(0); __block NSError *er = nil;
  [t sendMessage:[[NSURLSessionWebSocketMessage alloc] initWithData:[NSData dataWithBytes:p length:n]] completionHandler:^(NSError *e) { er = e; dispatch_semaphore_signal(sem); }];
  if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC))) return -1;
  return er ? -1 : 0;
}
// One outstanding receive is kept across calls so a timeout does not lose a message.
static NSMutableDictionary *g_pend;
int sw_ws_recv(sw_ws *w, uint8_t **out, size_t *n, int timeout_ms) {
  NSURLSessionWebSocketTask *t = (__bridge NSURLSessionWebSocketTask *)w->task;
  @synchronized ([NSObject class]) { if (!g_pend) g_pend = [NSMutableDictionary new]; }
  NSValue *k = [NSValue valueWithPointer:w];
  NSMutableDictionary *st;
  @synchronized (g_pend) {
    st = g_pend[k];
    if (!st) {
      st = [NSMutableDictionary new]; st[@"sem"] = dispatch_semaphore_create(0); g_pend[k] = st;
      NSMutableDictionary *cap = st;
      [t receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage *m, NSError *e) {
        @synchronized (cap) { if (m) cap[@"data"] = m.type == NSURLSessionWebSocketMessageTypeData ? m.data : [m.string dataUsingEncoding:NSUTF8StringEncoding]; if (e) cap[@"err"] = e; }
        dispatch_semaphore_signal(cap[@"sem"]);
      }];
    }
  }
  if (dispatch_semaphore_wait(st[@"sem"], dispatch_time(DISPATCH_TIME_NOW, (int64_t)timeout_ms * NSEC_PER_MSEC))) return 1;
  @synchronized (g_pend) { [g_pend removeObjectForKey:k]; }
  NSData *d; @synchronized (st) { d = st[@"data"]; }
  if (!d) return -1;
  *n = d.length; *out = malloc(d.length ? d.length : 1); memcpy(*out, d.bytes, d.length); return 0;
}
void sw_ws_close(sw_ws *w) {
  NSURLSessionWebSocketTask *t = (__bridge_transfer NSURLSessionWebSocketTask *)w->task;
  NSURLSession *s = (__bridge_transfer NSURLSession *)w->sess;
  @synchronized ([NSObject class]) { if (g_pend) @synchronized (g_pend) { [g_pend removeObjectForKey:[NSValue valueWithPointer:w]]; } }
  [t cancelWithCloseCode:NSURLSessionWebSocketCloseCodeNormalClosure reason:nil]; [s invalidateAndCancel]; free(w);
}
int sw_http_get(const char *url, uint8_t **out, size_t *n, volatile int *cancel, void (*prog)(void *, size_t), void *ud) {
  (void)prog; (void)ud;
  static NSURLSession *s; static dispatch_once_t once;
  dispatch_once(&once, ^{ NSURLSessionConfiguration *c = [NSURLSessionConfiguration defaultSessionConfiguration]; c.timeoutIntervalForRequest = 60; c.timeoutIntervalForResource = 1800; c.HTTPMaximumConnectionsPerHost = 8; s = [NSURLSession sessionWithConfiguration:c]; });
  dispatch_semaphore_t sem = dispatch_semaphore_create(0); __block NSData *res = nil; __block long code = -1;
  NSURLSessionDataTask *t = [s dataTaskWithURL:[NSURL URLWithString:@(url)] completionHandler:^(NSData *d, NSURLResponse *r, NSError *e) {
    if (!e) { res = d; code = [(NSHTTPURLResponse *)r statusCode]; } dispatch_semaphore_signal(sem); }];
  [t resume];
  while (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC))) if (cancel && *cancel) { [t cancel]; dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER); return -1; }
  if (code < 0) return -1;
  *n = res.length; *out = malloc(res.length ? res.length : 1); memcpy(*out, res.bytes, res.length); return (int)code;
}
int sw_aes_ecb_dec(const uint8_t key[32], const uint8_t in[16], uint8_t out[16]) {
  size_t m; return CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionECBMode, key, 32, NULL, in, 16, out, 16, &m) == kCCSuccess ? 0 : -1;
}
int sw_aes_cbc_dec(const uint8_t key[32], const uint8_t iv[16], const uint8_t *in, size_t n, uint8_t *out) {
  size_t m; return CCCrypt(kCCDecrypt, kCCAlgorithmAES, 0, key, 32, iv, in, n, out, n, &m) == kCCSuccess ? 0 : -1;
}

int sw_http_req(const char *method, const char *url, const void *body, size_t bn, const char *ctype, uint8_t **out, size_t *n, int *xeres) {
  NSMutableURLRequest *r = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:@(url)]];
  r.HTTPMethod = @(method); r.timeoutInterval = 25; r.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
  if (body) { r.HTTPBody = [NSData dataWithBytes:body length:bn]; if (ctype) [r setValue:@(ctype) forHTTPHeaderField:@"Content-Type"]; }
  dispatch_semaphore_t sem = dispatch_semaphore_create(0); __block NSData *res = nil; __block long code = -1; __block int xe = 1;
  [[NSURLSession.sharedSession dataTaskWithRequest:r completionHandler:^(NSData *d, NSURLResponse *rs, NSError *e) {
    if (!e) { NSHTTPURLResponse *h = (NSHTTPURLResponse *)rs; res = d; code = h.statusCode; NSString *x = [h valueForHTTPHeaderField:@"x-eresult"]; if (x) xe = x.intValue; }
    dispatch_semaphore_signal(sem); }] resume];
  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
  if (xeres) *xeres = xe;
  if (code < 0) return -1;
  *n = res.length; *out = malloc(res.length + 1); memcpy(*out, res.bytes, res.length); (*out)[res.length] = 0; return (int)code;
}
static NSData *der_int(const uint8_t *p, size_t n) {
  NSMutableData *d = [NSMutableData dataWithBytes:"\x02" length:1]; NSMutableData *v = [NSMutableData data];
  if (n && (p[0] & 0x80)) [v appendBytes:"\0" length:1]; [v appendBytes:p length:n];
  size_t l = v.length; uint8_t h[3]; int hl;
  if (l < 128) { h[0] = l; hl = 1; } else if (l < 256) { h[0] = 0x81; h[1] = l; hl = 2; } else { h[0] = 0x82; h[1] = l >> 8; h[2] = l; hl = 3; }
  [d appendBytes:h length:hl]; [d appendData:v]; return d;
}
int sw_rsa_pkcs1(const uint8_t *mod, size_t mn, const uint8_t *ex, size_t en, const uint8_t *in, size_t inn, uint8_t *out, size_t outn) {
  NSMutableData *body = [NSMutableData data]; [body appendData:der_int(mod, mn)]; [body appendData:der_int(ex, en)];
  NSMutableData *seq = [NSMutableData dataWithBytes:"\x30" length:1]; size_t l = body.length; uint8_t h[3]; int hl;
  if (l < 128) { h[0] = l; hl = 1; } else if (l < 256) { h[0] = 0x81; h[1] = l; hl = 2; } else { h[0] = 0x82; h[1] = l >> 8; h[2] = l; hl = 3; }
  [seq appendBytes:h length:hl]; [seq appendData:body];
  NSDictionary *a = @{(id)kSecAttrKeyType: (id)kSecAttrKeyTypeRSA, (id)kSecAttrKeyClass: (id)kSecAttrKeyClassPublic, (id)kSecAttrKeySizeInBits: @(mn * 8)};
  CFErrorRef e = NULL; SecKeyRef k = SecKeyCreateWithData((__bridge CFDataRef)seq, (__bridge CFDictionaryRef)a, &e);
  if (!k) return -1;
  CFDataRef c = SecKeyCreateEncryptedData(k, kSecKeyAlgorithmRSAEncryptionPKCS1, (__bridge CFDataRef)[NSData dataWithBytes:in length:inn], &e);
  CFRelease(k); if (!c) return -1;
  size_t cl = CFDataGetLength(c); int r = -1; if (cl <= outn) { memcpy(out, CFDataGetBytePtr(c), cl); r = (int)cl; } CFRelease(c); return r;
}
void sw_sha1(const void *p, size_t n, uint8_t out[20]) { CC_SHA1(p, (CC_LONG)n, out); }
