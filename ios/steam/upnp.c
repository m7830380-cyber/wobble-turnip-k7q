// Opens the host port on the home router so internet players can join (UPnP IGD, then NAT-PMP).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <ifaddrs.h>
#include <net/if.h>

void sw_log(const char *fmt, ...);

static uint32_t up_local_ip(char *ifn, size_t n) { // first private IPv4 on wifi (en*), else any non-loopback
  struct ifaddrs *a, *p; uint32_t best = 0;
  if (getifaddrs(&a)) return 0;
  for (int pass = 0; pass < 2 && !best; pass++)
    for (p = a; p; p = p->ifa_next) {
      if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
      if (pass == 0 && strncmp(p->ifa_name, "en", 2)) continue;
      uint32_t ip = ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr);
      if ((ip >> 24) == 10 || (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8) { best = ip; snprintf(ifn, n, "%s", p->ifa_name); break; }
    }
  freeifaddrs(a); return best;
}
static void ipstr(uint32_t ip, char *o) { sprintf(o, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); }

static int udp_sock(int ms) {
  int s = socket(AF_INET, SOCK_DGRAM, 0); if (s < 0) return -1;
  struct timeval tv = { ms / 1000, (ms % 1000) * 1000 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  return s;
}
// tiny HTTP client: returns malloc'd body (or full response) or NULL
static char *http(const char *host, int port, const char *req, size_t rn) {
  int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return NULL;
  struct timeval tv = {4, 0}; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(port); inet_pton(AF_INET, host, &a.sin_addr);
  if (connect(s, (void *)&a, sizeof a)) { close(s); return NULL; }
  if (send(s, req, rn, 0) != (ssize_t)rn) { close(s); return NULL; }
  size_t cap = 65536, n = 0; char *b = malloc(cap + 1); ssize_t r;
  while ((r = recv(s, b + n, cap - n, 0)) > 0) { n += r; if (n >= cap) break; }
  close(s); b[n] = 0; return b;
}
static int parse_url(const char *u, char *host, int *port, char *path) { // http://h:p/path
  if (strncmp(u, "http://", 7)) return 0; u += 7;
  int i = 0; while (*u && *u != ':' && *u != '/' && i < 63) host[i++] = *u++; host[i] = 0;
  *port = 80; if (*u == ':') { *port = atoi(++u); while (*u && *u != '/') u++; }
  snprintf(path, 256, "%s", *u ? u : "/"); return host[0] != 0;
}
static const char *ci_find(const char *h, const char *n) { size_t l = strlen(n); for (; *h; h++) if (!strncasecmp(h, n, l)) return h; return NULL; }

static int soap(const char *host, int port, const char *ctl, const char *svc, const char *act, const char *args, char **out) {
  char body[2048], req[4096];
  int bn = snprintf(body, sizeof body, "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>", act, svc, args, act);
  int rn = snprintf(req, sizeof req, "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"%s#%s\"\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", ctl, host, port, svc, act, bn, body);
  char *r = http(host, port, req, rn); if (!r) return -1;
  int code = 0; sscanf(r, "HTTP/%*s %d", &code);
  if (out) *out = r; else free(r);
  return code;
}

static int try_upnp(uint32_t lip, int port, uint32_t *gw_out) {
  char lips[20]; ipstr(lip, lips);
  int s = udp_sock(1200); if (s < 0) return -1;
  const char *sts[] = { "urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:device:InternetGatewayDevice:2", "ssdp:all" };
  // multicast + unicast to the usual gateway addresses (multicast may need an entitlement on iOS)
  uint32_t tg[4] = { 0xEFFFFFFA, (lip & 0xFFFFFF00) | 1, (lip & 0xFFFFFF00) | 254, (lip & 0xFFFF0000) | 1 };
  char loc[300] = ""; uint32_t from = 0;
  for (int round = 0; round < 3 && !loc[0]; round++) {
    for (int t = 0; t < 4; t++) for (int i = 0; i < 4; i++) {
      char m[300]; int n = snprintf(m, sizeof m, "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: %s\r\n\r\n", sts[i]);
      struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(1900); a.sin_addr.s_addr = htonl(tg[t]);
      if (sendto(s, m, n, 0, (void *)&a, sizeof a) < 0 && round == 0 && i == 0) sw_log("[upnp] M-SEARCH to %s failed: %s", t ? "gateway" : "multicast", strerror(errno));
    }
    for (;;) {
      char b[2048]; struct sockaddr_in f; socklen_t fl = sizeof f;
      ssize_t r = recvfrom(s, b, sizeof b - 1, 0, (void *)&f, &fl); if (r <= 0) break; b[r] = 0;
      if (!ci_find(b, "InternetGatewayDevice") && !ci_find(b, "WANIPConnection") && !ci_find(b, "WANPPPConnection")) continue;
      const char *l = ci_find(b, "\nLOCATION:"); if (!l) continue; l += 10; while (*l == ' ') l++;
      int i = 0; while (l[i] && l[i] != '\r' && l[i] != '\n' && i < 299) { loc[i] = l[i]; i++; } loc[i] = 0;
      from = ntohl(f.sin_addr.s_addr); break;
    }
  }
  close(s);
  if (gw_out && from) *gw_out = from;
  if (!loc[0]) { sw_log("[upnp] no router answered (UPnP off on the router?)"); return -1; }
  char host[64], path[256]; int hp;
  if (!parse_url(loc, host, &hp, path)) { sw_log("[upnp] bad location %s", loc); return -1; }
  char req[512]; int rn = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", path, host, hp);
  char *x = http(host, hp, req, rn); if (!x) { sw_log("[upnp] can't read %s", loc); return -1; }
  const char *svcs[] = { "urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANPPPConnection:1" };
  char ctl[256] = "", svc[100] = "";
  for (int i = 0; i < 3 && !ctl[0]; i++) {
    const char *p = strstr(x, svcs[i]); if (!p) continue;
    const char *c = ci_find(p, "<controlURL>"); if (!c) continue; c += 12;
    int k = 0; while (c[k] && c[k] != '<' && k < 255) { ctl[k] = c[k]; k++; } ctl[k] = 0; snprintf(svc, sizeof svc, "%s", svcs[i]);
  }
  free(x);
  if (!ctl[0]) { sw_log("[upnp] router at %s has no WAN connection service", host); return -1; }
  if (!strncmp(ctl, "http://", 7)) { char h2[64]; int p2; char pa[256]; if (parse_url(ctl, h2, &p2, pa)) { snprintf(host, sizeof host, "%s", h2); hp = p2; snprintf(ctl, sizeof ctl, "%s", pa); } }
  else if (ctl[0] != '/') { char t[256]; snprintf(t, sizeof t, "/%s", ctl); snprintf(ctl, sizeof ctl, "%s", t); }
  char *o = NULL; char ext[40] = "?";
  if (soap(host, hp, ctl, svc, "GetExternalIPAddress", "", &o) == 200 && o) { const char *e = ci_find(o, "<NewExternalIPAddress>"); if (e) { e += 22; int k = 0; while (e[k] && e[k] != '<' && k < 39) { ext[k] = e[k]; k++; } ext[k] = 0; } }
  free(o); o = NULL;
  char args[1024]; int ok = 0, code = 0;
  for (int lease = 0; lease < 2 && !ok; lease++) {
    snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>%d</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>GMod iOS</NewPortMappingDescription><NewLeaseDuration>%d</NewLeaseDuration>", port, port, lips, lease ? 0 : 7200);
    code = soap(host, hp, ctl, svc, "AddPortMapping", args, &o);
    if (code == 200) ok = 1;
    else if (o) { const char *e = ci_find(o, "<errorDescription>"); char d[100] = ""; if (e) { e += 18; int k = 0; while (e[k] && e[k] != '<' && k < 99) { d[k] = e[k]; k++; } d[k] = 0; } sw_log("[upnp] AddPortMapping refused: HTTP %d %s", code, d); }
    free(o); o = NULL;
  }
  if (!ok) return -1;
  sw_log("[upnp] router %s opened UDP %d -> %s:%d (router's internet IP %s)", host, port, lips, port, ext);
  { // a router internet IP that is private/CGNAT means the ISP blocks incoming connections anyway
    uint32_t e = 0; struct in_addr ia; if (inet_pton(AF_INET, ext, &ia) == 1) e = ntohl(ia.s_addr);
    if (e && ((e >> 24) == 10 || (e >> 20) == 0xac1 || (e >> 16) == 0xc0a8 || (e >> 22) == (0x64400000u >> 22)))
      sw_log("[upnp] WARNING: router's internet IP %s is private (ISP CGNAT) -> internet players still can't reach you", ext);
  }
  return 0;
}

static int try_natpmp(uint32_t gw, int port) {
  int s = udp_sock(1000); if (s < 0) return -1;
  struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(5351); a.sin_addr.s_addr = htonl(gw);
  uint8_t q[12] = {0, 1, 0, 0}; q[4] = port >> 8; q[5] = port; q[6] = port >> 8; q[7] = port; q[8] = 0; q[9] = 0; q[10] = 0x1c; q[11] = 0x20; // 7200 s
  int ok = 0;
  for (int i = 0; i < 3 && !ok; i++) {
    sendto(s, q, 12, 0, (void *)&a, sizeof a);
    uint8_t r[16]; ssize_t n = recv(s, r, sizeof r, 0);
    if (n >= 16 && r[1] == 129) { int rc = r[2] << 8 | r[3]; if (rc == 0) ok = 1; else { sw_log("[natpmp] router refused (code %d)", rc); break; } }
  }
  close(s);
  if (ok) { char g[20]; ipstr(gw, g); sw_log("[natpmp] router %s opened UDP %d", g, port); }
  return ok ? 0 : -1;
}

static volatile int g_up_busy; static int g_up_port;
static void *up_thread(void *arg) {
  int port = g_up_port; char ifn[16] = "";
  uint32_t lip = up_local_ip(ifn, sizeof ifn);
  if (!lip) { sw_log("[upnp] no Wi-Fi IPv4 address, can't open the router port (mobile data can't host)"); g_up_busy = 0; return NULL; }
  char l[20]; ipstr(lip, l); sw_log("[upnp] hosting on %s %s:%d, asking the router to open the port", ifn, l, port);
  uint32_t gw = 0;
  if (try_upnp(lip, port, &gw) && try_natpmp(gw ? gw : (lip & 0xFFFFFF00) | 1, port))
    sw_log("[upnp] couldn't open UDP %d automatically. Internet players can't join unless you forward UDP %d to %s in the router settings (or turn on UPnP)", port, port, l);
  g_up_busy = 0; return NULL;
}
void up_open_port(int port) {
  if (port <= 0 || g_up_busy) return;
  g_up_busy = 1; g_up_port = port;
  pthread_t t; if (pthread_create(&t, NULL, up_thread, NULL)) { g_up_busy = 0; return; } pthread_detach(t);
}

#ifdef UP_TEST_MAIN
#include <stdarg.h>
void sw_log(const char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); putchar('\n'); }
int main(int c, char **v) { up_open_port(c > 1 ? atoi(v[1]) : 27015); sleep(1); while (g_up_busy) sleep(1); return 0; }
#endif
