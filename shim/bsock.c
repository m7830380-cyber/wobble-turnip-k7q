// BSD sockets: Android (Linux ABI) -> Darwin. Differences: sockaddr header (u16 family vs u8 len + u8 family),
// AF_INET6 10 vs 30, SOL_SOCKET 1 vs 0xffff + option numbers, IP_* numbers, SOCK_NONBLOCK/CLOEXEC type flags,
// MSG_* flags, timeval for SO_*TIMEO, and errno numbers (engine checks EWOULDBLOCK == 11 etc).
#ifdef __APPLE__
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

static int lerr(int e) {
  switch (e) {
    case EAGAIN: return 11; case EINPROGRESS: return 115; case EALREADY: return 114; case ECONNREFUSED: return 111;
    case ECONNRESET: return 104; case EADDRINUSE: return 98; case EADDRNOTAVAIL: return 99; case ENOTCONN: return 107;
    case EISCONN: return 106; case ETIMEDOUT: return 110; case EMSGSIZE: return 90; case ENETUNREACH: return 101;
    case EHOSTUNREACH: return 113; case ENOTSOCK: return 88; case ENOPROTOOPT: return 92; case EAFNOSUPPORT: return 97;
    case ECONNABORTED: return 103; case ENETDOWN: return 100; case ENOBUFS: return 105; case EPROTONOSUPPORT: return 93;
    case EOPNOTSUPP: return 95; case EDESTADDRREQ: return 89; case EHOSTDOWN: return 112; case EPIPE: return 32;
    default: return e;
  }
}
#define RET(x) do { long _r = (x); if (_r < 0) errno = lerr(errno); return _r; } while (0)

static int fam_in(int f) { return f == 10 ? AF_INET6 : f; }
static int fam_out(int f) { return f == AF_INET6 ? 10 : f; }

// Android sockaddr -> Darwin (into buf). Returns new length.
static socklen_t sa_in(const void *a, socklen_t l, struct sockaddr_storage *o) {
  if (!a) return 0;
  if (l > sizeof *o) l = sizeof *o;
  memcpy(o, a, l);
  uint16_t f; memcpy(&f, a, 2);
  struct sockaddr *s = (struct sockaddr *)o;
  s->sa_family = fam_in(f); s->sa_len = l;
  if (s->sa_family == AF_INET && l > sizeof(struct sockaddr_in)) s->sa_len = l = sizeof(struct sockaddr_in);
  return l;
}
// Darwin sockaddr -> Android into user buffer with *ul capacity.
static void sa_out(const struct sockaddr_storage *s, socklen_t sl, void *u, socklen_t *ul) {
  if (!u || !ul) return;
  unsigned char t[sizeof *s]; memcpy(t, s, sl);
  uint16_t f = fam_out(((const struct sockaddr *)s)->sa_family); memcpy(t, &f, 2);
  socklen_t n = sl < *ul ? sl : *ul; memcpy(u, t, n); *ul = sl;
}

static int b_socket(int d, int t, int p) {
  int nb = t & 04000, ce = t & 02000000; t &= 0xf;
  int fd = socket(fam_in(d), t, p);
  if (fd < 0) { errno = lerr(errno); return fd; }
  int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
  if (nb) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  if (ce) fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
}
static int b_socketpair(int d, int t, int p, int sv[2]) {
  int nb = t & 04000; t &= 0xf; int r = socketpair(fam_in(d), t, p, sv);
  if (r < 0) { errno = lerr(errno); return r; }
  if (nb) for (int i = 0; i < 2; i++) fcntl(sv[i], F_SETFL, fcntl(sv[i], F_GETFL) | O_NONBLOCK);
  return 0;
}

static int opt_map(int *lvl, int *o) {
  if (*lvl == 1) { *lvl = SOL_SOCKET;
    switch (*o) {
      case 2: *o = SO_REUSEADDR; return 0; case 3: *o = SO_TYPE; return 0; case 4: *o = SO_ERROR; return 0;
      case 5: *o = SO_DONTROUTE; return 0; case 6: *o = SO_BROADCAST; return 0; case 7: *o = SO_SNDBUF; return 0;
      case 8: *o = SO_RCVBUF; return 0; case 9: *o = SO_KEEPALIVE; return 0; case 10: *o = SO_OOBINLINE; return 0;
      case 13: *o = SO_LINGER; return 0; case 15: *o = SO_REUSEPORT; return 0; case 18: *o = SO_RCVLOWAT; return 0;
      case 19: *o = SO_SNDLOWAT; return 0; case 20: *o = SO_RCVTIMEO; return 1; case 21: *o = SO_SNDTIMEO; return 1;
      case 32: *o = SO_SNDBUF; return 0; case 33: *o = SO_RCVBUF; return 0; /* *BUFFORCE */
      default: return -1; } }
  if (*lvl == IPPROTO_IP) {
    switch (*o) {
      case 1: *o = IP_TOS; return 0; case 2: *o = IP_TTL; return 0; case 3: *o = IP_HDRINCL; return 0;
      case 32: *o = IP_MULTICAST_IF; return 0; case 33: *o = IP_MULTICAST_TTL; return 0; case 34: *o = IP_MULTICAST_LOOP; return 0;
      case 35: *o = IP_ADD_MEMBERSHIP; return 0; case 36: *o = IP_DROP_MEMBERSHIP; return 0;
      case 10: return -1; /* IP_MTU_DISCOVER: no Darwin equivalent, ignore */
      default: return -1; } }
  if (*lvl == IPPROTO_IPV6) {
    switch (*o) { case 26: *o = IPV6_V6ONLY; return 0; case 16: *o = IPV6_UNICAST_HOPS; return 0;
      case 20: *o = IPV6_JOIN_GROUP; return 0; case 21: *o = IPV6_LEAVE_GROUP; return 0; default: return -1; } }
  if (*lvl == IPPROTO_TCP) { if (*o == 1) { *o = TCP_NODELAY; return 0; } if (*o == 4) { *o = TCP_KEEPALIVE; return 0; } return -1; }
  return 0;
}
static int b_setsockopt(int fd, int lvl, int o, const void *v, socklen_t l) {
  int k = opt_map(&lvl, &o);
  if (k < 0) return 0; /* unknown -> pretend success */
  if (k == 1 && v && l >= 16) { struct timeval t = {((const long *)v)[0], (int)((const long *)v)[1]}; RET(setsockopt(fd, lvl, o, &t, sizeof t)); }
  RET(setsockopt(fd, lvl, o, v, l));
}
static int b_getsockopt(int fd, int lvl, int o, void *v, socklen_t *l) {
  int k = opt_map(&lvl, &o);
  if (k < 0) { if (v && l && *l >= 4) { memset(v, 0, 4); *l = 4; } return 0; }
  if (k == 1 && v && l && *l >= 16) { struct timeval t; socklen_t tl = sizeof t; int r = getsockopt(fd, lvl, o, &t, &tl);
    if (r < 0) { errno = lerr(errno); return r; } ((long *)v)[0] = t.tv_sec; ((long *)v)[1] = t.tv_usec; *l = 16; return 0; }
  int r = getsockopt(fd, lvl, o, v, l);
  if (r < 0) { errno = lerr(errno); return r; }
  if (lvl == SOL_SOCKET && o == SO_ERROR && v) *(int *)v = lerr(*(int *)v);
  return 0;
}

static int msgf(int f) {
  int o = 0;
  if (f & 1) o |= MSG_OOB; if (f & 2) o |= MSG_PEEK; if (f & 4) o |= MSG_DONTROUTE; if (f & 0x20) o |= MSG_TRUNC;
  if (f & 0x40) o |= MSG_DONTWAIT; if (f & 0x100) o |= MSG_WAITALL; if (f & 0x80) o |= MSG_EOR;
  return o; /* MSG_NOSIGNAL (0x4000): SO_NOSIGPIPE set on every socket */
}

static int b_bind(int fd, const void *a, socklen_t l) { struct sockaddr_storage s; l = sa_in(a, l, &s); RET(bind(fd, (struct sockaddr *)&s, l)); }
static int b_connect(int fd, const void *a, socklen_t l) { struct sockaddr_storage s; l = sa_in(a, l, &s); RET(connect(fd, (struct sockaddr *)&s, l)); }
static int b_listen(int fd, int n) { RET(listen(fd, n)); }
static int b_accept(int fd, void *a, socklen_t *l) {
  struct sockaddr_storage s; socklen_t sl = sizeof s; int r = accept(fd, (struct sockaddr *)&s, &sl);
  if (r < 0) { errno = lerr(errno); return r; }
  int one = 1; setsockopt(r, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
  sa_out(&s, sl, a, l); return r;
}
static int b_getsockname(int fd, void *a, socklen_t *l) {
  struct sockaddr_storage s; socklen_t sl = sizeof s; int r = getsockname(fd, (struct sockaddr *)&s, &sl);
  if (r < 0) { errno = lerr(errno); return r; } sa_out(&s, sl, a, l); return 0;
}
static int b_getpeername(int fd, void *a, socklen_t *l) {
  struct sockaddr_storage s; socklen_t sl = sizeof s; int r = getpeername(fd, (struct sockaddr *)&s, &sl);
  if (r < 0) { errno = lerr(errno); return r; } sa_out(&s, sl, a, l); return 0;
}
static long b_send(int fd, const void *b, size_t n, int f) { RET(send(fd, b, n, msgf(f))); }
static long b_recv(int fd, void *b, size_t n, int f) { RET(recv(fd, b, n, msgf(f))); }
#include <stdio.h>
#include <time.h>
// diag: UDP to/from internet (non-private) IPv4 peers -> "[net]" lines, rate limited
static int pub4(const struct sockaddr_storage *s, uint32_t *ip, int *port) {
  if (((const struct sockaddr *)s)->sa_family != AF_INET) return 0;
  const struct sockaddr_in *i = (const struct sockaddr_in *)s; uint32_t a = ntohl(i->sin_addr.s_addr);
  if ((a >> 24) == 10 || (a >> 24) == 127 || (a >> 16) == 0xc0a8 || (a >> 20) == 0xac1 || (a >> 16) == 0xa9fe || !a || a == 0xffffffff || (a >> 28) == 14) return 0;
  *ip = a; *port = ntohs(i->sin_port); return 1;
}
static void ndiag(const char *what, const struct sockaddr_storage *s, long r, int e) {
  static uint32_t lip[2]; static int cnt[2]; static time_t t0[2]; int k = what[0] == 'r';
  uint32_t ip; int port; if (!pub4(s, &ip, &port)) return;
  time_t now = time(NULL);
  if (ip != lip[k] || now - t0[k] >= 10) { if (cnt[k] > 1) fprintf(stderr, "[net] %s x%d more\n", what, cnt[k] - 1); lip[k] = ip; cnt[k] = 0; t0[k] = now; }
  if (cnt[k]++ == 0 || r < 0)
    fprintf(stderr, "[net] %s %u.%u.%u.%u:%d -> %ld%s%s\n", what, ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port, r, r < 0 ? " " : "", r < 0 ? strerror(e) : "");
}
static long b_sendto(int fd, const void *b, size_t n, int f, const void *a, socklen_t l) {
  struct sockaddr_storage s; if (a) l = sa_in(a, l, &s);
  long r = sendto(fd, b, n, msgf(f), a ? (struct sockaddr *)&s : NULL, a ? l : 0); int e = errno;
  if (a) ndiag("udp send", &s, r, e);
  if (r < 0) errno = lerr(e); return r;
}
static long b_recvfrom(int fd, void *b, size_t n, int f, void *a, socklen_t *l) {
  struct sockaddr_storage s; socklen_t sl = sizeof s;
  long r = recvfrom(fd, b, n, msgf(f), (struct sockaddr *)&s, &sl);
  if (r < 0) { errno = lerr(errno); return r; }
  if (sl) ndiag("udp recv", &s, r, 0);
  if (a && l) { if (sl) sa_out(&s, sl, a, l); else *l = 0; }
  return r;
}
static int b_shutdown(int fd, int h) { RET(shutdown(fd, h)); }

// bionic addrinfo = BSD layout (flags,family,socktype,protocol,addrlen,canonname,addr,next) - same field order as Darwin,
// but the sockaddr inside and the family values differ, so we deep-copy into our own allocation.
struct bai { int flags, family, socktype, protocol; socklen_t addrlen; char *canon; void *addr; struct bai *next; };
static int eai(int e) { /* Darwin EAI_* -> bionic (BSD numbering, identical) */ return e; }
static int b_getaddrinfo(const char *node, const char *svc, const struct bai *h, struct bai **res) {
  struct addrinfo hh, *r = NULL; memset(&hh, 0, sizeof hh);
  if (h) { hh.ai_flags = h->flags; hh.ai_family = fam_in(h->family); hh.ai_socktype = h->socktype & 0xf; hh.ai_protocol = h->protocol; }
  int e = getaddrinfo(node, svc, h ? &hh : NULL, &r);
  if (e) { *res = NULL; return eai(e); }
  struct bai *head = NULL, **tail = &head;
  for (struct addrinfo *p = r; p; p = p->ai_next) {
    struct bai *b = calloc(1, sizeof *b + 128);
    b->flags = p->ai_flags; b->family = fam_out(p->ai_family); b->socktype = p->ai_socktype; b->protocol = p->ai_protocol;
    b->addr = (char *)(b + 1); socklen_t ul = 128; sa_out((struct sockaddr_storage *)p->ai_addr, p->ai_addrlen, b->addr, &ul);
    b->addrlen = p->ai_addrlen; b->canon = p->ai_canonname ? strdup(p->ai_canonname) : NULL;
    *tail = b; tail = &b->next;
  }
  freeaddrinfo(r); *res = head; return 0;
}
static void b_freeaddrinfo(struct bai *a) { while (a) { struct bai *n = a->next; free(a->canon); free(a); a = n; } }
static const char *b_gai_strerror(int e) { return gai_strerror(e); }

typedef struct { const char *n; void *f; } bsym;
static const bsym g_s[] = {
  {"socket", b_socket}, {"socketpair", b_socketpair}, {"setsockopt", b_setsockopt}, {"getsockopt", b_getsockopt},
  {"bind", b_bind}, {"connect", b_connect}, {"listen", b_listen}, {"accept", b_accept},
  {"getsockname", b_getsockname}, {"getpeername", b_getpeername}, {"send", b_send}, {"recv", b_recv},
  {"sendto", b_sendto}, {"recvfrom", b_recvfrom}, {"shutdown", b_shutdown},
  {"getaddrinfo", b_getaddrinfo}, {"freeaddrinfo", b_freeaddrinfo}, {"gai_strerror", b_gai_strerror}, {0, 0}};
void *bsock_resolve(const char *n) { for (const bsym *s = g_s; s->n; s++) if (!strcmp(s->n, n)) return s->f; return NULL; }
#else
void *bsock_resolve(const char *n) { (void)n; return 0; }
#endif
