// Fake JNI: enough of JavaVM/JNIEnv for SDL + GDroid launcher/engine/GameUI.
// Variadic Call*Method are implemented as fixed-register functions (x3..x7, d0..d7) and decoded via the
// method signature. That works identically on Linux and iOS (Darwin passes varargs on the stack, but the
// Android caller always puts them in registers).
#include "jni_fake.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define JLOG(...) do { if (getenv("JNI_VERBOSE")) fprintf(stderr, "[jni] " __VA_ARGS__); } while (0)

/* ---------- object model ---------- */
enum { K_CLASS = 1, K_STRING, K_ARRAY, K_OBJECT };
typedef struct jobj { uint32_t magic; int kind; const char *cls; char *str; int len, esz; void *data; } jobj;
#define MAGIC 0x4a4f424a
static jobj *mk(int kind, const char *cls) { jobj *o = calloc(1, sizeof *o); o->magic = MAGIC; o->kind = kind; o->cls = cls; return o; }
static jobj *O(void *p) { jobj *o = p; return (o && o->magic == MAGIC) ? o : NULL; }

jobj *jni_new_string(const char *s) { jobj *o = mk(K_STRING, "java/lang/String"); o->str = strdup(s ? s : ""); o->len = (int)strlen(o->str); return o; }
jobj *jni_new_array(int n, int esz, const char *cls) { jobj *o = mk(K_ARRAY, cls); o->len = n; o->esz = esz; o->data = calloc(n ? n : 1, esz); return o; }
const char *jni_str(void *p) { jobj *o = O(p); return o && o->kind == K_STRING ? o->str : NULL; }

static jobj *g_classes[256]; static int g_ncls;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static jobj *find_class(const char *name) {
  char n[256]; snprintf(n, sizeof n, "%s", name); for (char *c = n; *c; c++) if (*c == '.') *c = '/';
  pthread_mutex_lock(&g_mx);
  for (int i = 0; i < g_ncls; i++) if (!strcmp(g_classes[i]->cls, n)) { pthread_mutex_unlock(&g_mx); return g_classes[i]; }
  jobj *c = mk(K_CLASS, strdup(n)); if (g_ncls < 256) g_classes[g_ncls++] = c;
  pthread_mutex_unlock(&g_mx); return c;
}
jobj *g_activity, *g_context;
void *jni_find_class_pub(const char *n) { return find_class(n); }

/* ---------- methods ---------- */
typedef struct jmeth { const char *cls, *name, *sig; int is_static; jni_impl impl; } jmeth;
static jmeth g_meth[1024]; static int g_nmeth;
static jni_impl lookup_impl(const char *cls, const char *name, const char *sig);
static jmeth *get_method(void *clazz, const char *name, const char *sig, int st) {
  jobj *c = O(clazz); const char *cn = c ? c->cls : "?";
  pthread_mutex_lock(&g_mx);
  for (int i = 0; i < g_nmeth; i++) { jmeth *m = &g_meth[i];
    if (!strcmp(m->cls, cn) && !strcmp(m->name, name) && !strcmp(m->sig, sig)) { pthread_mutex_unlock(&g_mx); return m; } }
  jmeth *m = g_nmeth < 1024 ? &g_meth[g_nmeth++] : &g_meth[0];
  m->cls = cn; m->name = strdup(name); m->sig = strdup(sig); m->is_static = st; m->impl = lookup_impl(cn, name, sig);
  pthread_mutex_unlock(&g_mx);
  JLOG("method %s.%s%s%s\n", cn, name, sig, m->impl ? "" : " (unimplemented)");
  return m;
}

/* decode arguments per signature into jvalue[] */
static int decode_args(const char *sig, jvalue *out, const uint64_t *gr, int ngr, const double *fr) {
  int gi = 0, fi = 0, n = 0; const char *p = sig + 1;
  while (*p && *p != ')' && n < 16) {
    char t = *p;
    if (t == '[') { while (*p == '[') p++; if (*p == 'L') p = strchr(p, ';'); p++; out[n++].l = (void *)(gi < ngr ? gr[gi++] : 0); continue; }
    if (t == 'L') { p = strchr(p, ';') + 1; out[n++].l = (void *)(gi < ngr ? gr[gi++] : 0); continue; }
    p++;
    switch (t) {
      case 'F': { double d = fi < 8 ? fr[fi++] : 0; float f; memcpy(&f, &d, 4); out[n++].f = f; break; }
      case 'D': out[n++].d = fi < 8 ? fr[fi++] : 0; break;
      case 'J': out[n++].j = gi < ngr ? (int64_t)gr[gi++] : 0; break;
      default: out[n++].i = gi < ngr ? (int32_t)gr[gi++] : 0; break;
    }
  }
  return n;
}
static jvalue invoke(void *env, void *obj, jmeth *m, jvalue *a) {
  jvalue r; memset(&r, 0, sizeof r);
  if (!m) return r;
  if (m->impl) return m->impl(env, obj, a);
  if (getenv("JNI_VERBOSE")) fprintf(stderr, "[jni] call unimplemented %s.%s%s\n", m->cls, m->name, m->sig);
  const char *ret = strchr(m->sig, ')') + 1;
  if (*ret == 'L' && !strcmp(ret, "Ljava/lang/String;")) r.l = jni_new_string("");
  return r;
}

/* variadic-safe entry points: (env, obj, mid, x3..x7) + (d0..d7) */
#define FR double f0, double f1, double f2, double f3, double f4, double f5, double f6, double f7
#define CALLV(env, obj, mid) do { \
    uint64_t gr[5] = {x3, x4, x5, x6, x7}; double fr[8] = {f0, f1, f2, f3, f4, f5, f6, f7}; \
    jvalue a[16]; decode_args(((jmeth *)mid)->sig, a, gr, 5, fr); r = invoke(env, obj, mid, a); } while (0)
/* non-virtual has clazz before mid: (env, obj, clazz, mid, x4..x7) */
static jvalue call_reg(void *env, void *obj, void *mid, uint64_t x3, uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7, FR) {
  jvalue r; CALLV(env, obj, mid); return r; }
static jvalue call_nv(void *env, void *obj, void *cls, void *mid, uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7, FR) {
  (void)cls; uint64_t x3 = x4; x4 = x5; x5 = x6; x6 = x7; x7 = 0; jvalue r; CALLV(env, obj, mid); return r; }
static jvalue call_a(void *env, void *obj, void *mid, const jvalue *args) {
  jvalue a[16]; memset(a, 0, sizeof a);
  if (args) { int n = 0; for (const char *p = ((jmeth *)mid)->sig + 1; *p && *p != ')'; n++) { while (*p == '[') p++; if (*p == 'L') p = strchr(p, ';'); p++; }
    memcpy(a, args, (n > 16 ? 16 : n) * sizeof(jvalue)); }
  return invoke(env, obj, mid, a);
}
/* va_list variants are only reachable from code compiled for the same ABI as us: we never let Android code
   hand us a va_list (AAPCS va_list struct != Darwin char*). On Linux arm64 they're the same, so support it. */
static jvalue call_v(void *env, void *obj, void *mid, va_list ap) {
  jvalue a[16]; int n = 0; const char *p = ((jmeth *)mid)->sig + 1;
  while (*p && *p != ')' && n < 16) { char t = *p;
    if (t == '[' || t == 'L') { while (*p == '[') p++; if (*p == 'L') p = strchr(p, ';'); p++; a[n++].l = va_arg(ap, void *); continue; }
    p++; if (t == 'F' || t == 'D') { double d = va_arg(ap, double); if (t == 'F') a[n++].f = (float)d; else a[n++].d = d; }
    else if (t == 'J') a[n++].j = va_arg(ap, int64_t); else a[n++].i = va_arg(ap, int); }
  return invoke(env, obj, mid, a);
}

#define TYPED(T, ctype, field) \
  static ctype Call##T##Method(void *e, void *o, void *m, uint64_t x3, uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7, FR) { return call_reg(e, o, m, x3, x4, x5, x6, x7, f0, f1, f2, f3, f4, f5, f6, f7).field; } \
  static ctype CallStatic##T##Method(void *e, void *o, void *m, uint64_t x3, uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7, FR) { return call_reg(e, o, m, x3, x4, x5, x6, x7, f0, f1, f2, f3, f4, f5, f6, f7).field; } \
  static ctype CallNonvirtual##T##Method(void *e, void *o, void *c, void *m, uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7, FR) { return call_nv(e, o, c, m, x4, x5, x6, x7, f0, f1, f2, f3, f4, f5, f6, f7).field; } \
  static ctype Call##T##MethodA(void *e, void *o, void *m, const jvalue *a) { return call_a(e, o, m, a).field; } \
  static ctype CallStatic##T##MethodA(void *e, void *o, void *m, const jvalue *a) { return call_a(e, o, m, a).field; } \
  static ctype CallNonvirtual##T##MethodA(void *e, void *o, void *c, void *m, const jvalue *a) { (void)c; return call_a(e, o, m, a).field; } \
  static ctype Call##T##MethodV(void *e, void *o, void *m, va_list ap) { return call_v(e, o, m, ap).field; } \
  static ctype CallStatic##T##MethodV(void *e, void *o, void *m, va_list ap) { return call_v(e, o, m, ap).field; } \
  static ctype CallNonvirtual##T##MethodV(void *e, void *o, void *c, void *m, va_list ap) { (void)c; return call_v(e, o, m, ap).field; }
TYPED(Object, void *, l) TYPED(Boolean, uint8_t, z) TYPED(Byte, int8_t, b) TYPED(Char, uint16_t, c) TYPED(Short, int16_t, s)
TYPED(Int, int32_t, i) TYPED(Long, int64_t, j) TYPED(Float, float, f) TYPED(Double, double, d)
static void CallVoidMethod(void *e, void *o, void *m, uint64_t x3, uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7, FR) { call_reg(e, o, m, x3, x4, x5, x6, x7, f0, f1, f2, f3, f4, f5, f6, f7); }
static void CallNonvirtualVoidMethod(void *e, void *o, void *c, void *m, uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7, FR) { call_nv(e, o, c, m, x4, x5, x6, x7, f0, f1, f2, f3, f4, f5, f6, f7); }
static void CallVoidMethodA(void *e, void *o, void *m, const jvalue *a) { call_a(e, o, m, a); }
static void CallNonvirtualVoidMethodA(void *e, void *o, void *c, void *m, const jvalue *a) { (void)c; call_a(e, o, m, a); }
static void CallVoidMethodV(void *e, void *o, void *m, va_list ap) { call_v(e, o, m, ap); }
static void CallNonvirtualVoidMethodV(void *e, void *o, void *c, void *m, va_list ap) { (void)c; call_v(e, o, m, ap); }
#define CallStaticVoidMethod CallVoidMethod
#define CallStaticVoidMethodA CallVoidMethodA
#define CallStaticVoidMethodV CallVoidMethodV
static void *NewObject(void *e, void *c, void *m, uint64_t x3, uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7, FR) {
  jobj *o = mk(K_OBJECT, O(c) ? O(c)->cls : "?"); call_reg(e, o, m, x3, x4, x5, x6, x7, f0, f1, f2, f3, f4, f5, f6, f7); return o; }
static void *NewObjectA(void *e, void *c, void *m, const jvalue *a) { jobj *o = mk(K_OBJECT, O(c) ? O(c)->cls : "?"); call_a(e, o, m, a); return o; }
static void *NewObjectV(void *e, void *c, void *m, va_list ap) { jobj *o = mk(K_OBJECT, O(c) ? O(c)->cls : "?"); call_v(e, o, m, ap); return o; }

/* ---------- misc env functions ---------- */
static int32_t GetVersion(void *e) { (void)e; return 0x00010006; }
static void *FindClass(void *e, const char *n) { (void)e; JLOG("FindClass %s\n", n); return find_class(n); }
static void *GetObjectClass(void *e, void *o) { (void)e; jobj *j = O(o); return find_class(j ? j->cls : "java/lang/Object"); }
static void *GetSuperclass(void *e, void *c) { (void)e; (void)c; return find_class("java/lang/Object"); }
static uint8_t IsAssignableFrom(void *e, void *a, void *b) { (void)e; (void)a; (void)b; return 1; }
static uint8_t IsInstanceOf(void *e, void *o, void *c) { (void)e; jobj *j = O(o), *k = O(c); return j && k && !strcmp(j->cls, k->cls); }
static uint8_t IsSameObject(void *e, void *a, void *b) { (void)e; return a == b; }
static void *GetMethodID(void *e, void *c, const char *n, const char *s) { (void)e; return get_method(c, n, s, 0); }
static void *GetStaticMethodID(void *e, void *c, const char *n, const char *s) { (void)e; return get_method(c, n, s, 1); }
static void *GetFieldID(void *e, void *c, const char *n, const char *s) { (void)e; JLOG("field %s %s\n", n, s); return get_method(c, n, s, 0); }
static void *GetStaticFieldID(void *e, void *c, const char *n, const char *s) { (void)e; JLOG("sfield %s %s\n", n, s); return get_method(c, n, s, 1); }
static jvalue field_get(void *o, void *f) { jmeth *m = f; jvalue r; memset(&r, 0, sizeof r); if (m && m->impl) r = m->impl(NULL, o, NULL); return r; }
#define FGET(T, ctype, fld) static ctype Get##T##Field(void *e, void *o, void *f) { (void)e; return field_get(o, f).fld; } \
  static ctype GetStatic##T##Field(void *e, void *c, void *f) { (void)e; return field_get(c, f).fld; } \
  static void Set##T##Field(void *e, void *o, void *f, ctype v) { (void)e; (void)o; (void)f; (void)v; } \
  static void SetStatic##T##Field(void *e, void *o, void *f, ctype v) { (void)e; (void)o; (void)f; (void)v; }
FGET(Object, void *, l) FGET(Boolean, uint8_t, z) FGET(Byte, int8_t, b) FGET(Char, uint16_t, c) FGET(Short, int16_t, s)
FGET(Int, int32_t, i) FGET(Long, int64_t, j) FGET(Float, float, f) FGET(Double, double, d)

static __thread void *t_exc;
static int32_t Throw(void *e, void *o) { (void)e; t_exc = o; return 0; }
static int32_t ThrowNew(void *e, void *c, const char *m) { (void)e; JLOG("ThrowNew %s: %s\n", O(c) ? O(c)->cls : "?", m); t_exc = mk(K_OBJECT, "java/lang/Throwable"); return 0; }
static void *ExceptionOccurred(void *e) { (void)e; return t_exc; }
static void ExceptionDescribe(void *e) { (void)e; }
static void ExceptionClear(void *e) { (void)e; t_exc = NULL; }
static uint8_t ExceptionCheck(void *e) { (void)e; return t_exc != NULL; }
static void FatalError(void *e, const char *m) { (void)e; fprintf(stderr, "JNI FatalError: %s\n", m); abort(); }
static int32_t PushLocalFrame(void *e, int32_t c) { (void)e; (void)c; return 0; }
static void *PopLocalFrame(void *e, void *r) { (void)e; return r; }
static void *NewRef(void *e, void *o) { (void)e; return o; }
static void DelRef(void *e, void *o) { (void)e; (void)o; }
static int32_t EnsureLocalCapacity(void *e, int32_t c) { (void)e; (void)c; return 0; }
static void *AllocObject(void *e, void *c) { (void)e; return mk(K_OBJECT, O(c) ? O(c)->cls : "?"); }
static int32_t GetObjectRefType(void *e, void *o) { (void)e; (void)o; return 1; }

/* strings */
static void *NewStringUTF(void *e, const char *s) { (void)e; return jni_new_string(s); }
static int32_t GetStringUTFLength(void *e, void *s) { (void)e; jobj *o = O(s); return o ? o->len : 0; }
static const char *GetStringUTFChars(void *e, void *s, uint8_t *c) { (void)e; if (c) *c = 0; jobj *o = O(s); return o && o->str ? o->str : ""; }
static void ReleaseStringUTFChars(void *e, void *s, const char *c) { (void)e; (void)s; (void)c; }
static int32_t GetStringLength(void *e, void *s) { return GetStringUTFLength(e, s); }
static const uint16_t *GetStringChars(void *e, void *s, uint8_t *c) {
  (void)e; if (c) *c = 1; jobj *o = O(s); int n = o ? o->len : 0; uint16_t *u = calloc(n + 1, 2);
  for (int i = 0; i < n; i++) u[i] = (uint8_t)o->str[i]; return u; }
static void ReleaseStringChars(void *e, void *s, const uint16_t *c) { (void)e; (void)s; free((void *)c); }
static void *NewString(void *e, const uint16_t *u, int32_t n) { (void)e; char *b = malloc(n + 1); for (int i = 0; i < n; i++) b[i] = u[i] < 128 ? (char)u[i] : '?'; b[n] = 0; void *r = jni_new_string(b); free(b); return r; }
static void GetStringUTFRegion(void *e, void *s, int32_t st, int32_t n, char *buf) { (void)e; jobj *o = O(s); memcpy(buf, o->str + st, n); buf[n] = 0; }
static void GetStringRegion(void *e, void *s, int32_t st, int32_t n, uint16_t *buf) { (void)e; jobj *o = O(s); for (int i = 0; i < n; i++) buf[i] = (uint8_t)o->str[st + i]; }

/* arrays */
static int32_t GetArrayLength(void *e, void *a) { (void)e; jobj *o = O(a); return o ? o->len : 0; }
static void *NewObjectArray(void *e, int32_t n, void *c, void *init) { (void)e; (void)c; jobj *a = jni_new_array(n, 8, "[Ljava/lang/Object;"); for (int i = 0; i < n; i++) ((void **)a->data)[i] = init; return a; }
static void *GetObjectArrayElement(void *e, void *a, int32_t i) { (void)e; jobj *o = O(a); return o && i < o->len ? ((void **)o->data)[i] : NULL; }
static void SetObjectArrayElement(void *e, void *a, int32_t i, void *v) { (void)e; jobj *o = O(a); if (o && i < o->len) ((void **)o->data)[i] = v; }
#define PRIM(T, ctype, sig) \
  static void *New##T##Array(void *e, int32_t n) { (void)e; return jni_new_array(n, sizeof(ctype), sig); } \
  static ctype *Get##T##ArrayElements(void *e, void *a, uint8_t *c) { (void)e; if (c) *c = 0; jobj *o = O(a); return o ? o->data : NULL; } \
  static void Release##T##ArrayElements(void *e, void *a, ctype *p, int32_t m) { (void)e; (void)a; (void)p; (void)m; } \
  static void Get##T##ArrayRegion(void *e, void *a, int32_t s, int32_t n, ctype *b) { (void)e; jobj *o = O(a); if (o) memcpy(b, (ctype *)o->data + s, n * sizeof(ctype)); } \
  static void Set##T##ArrayRegion(void *e, void *a, int32_t s, int32_t n, const ctype *b) { (void)e; jobj *o = O(a); if (o) memcpy((ctype *)o->data + s, b, n * sizeof(ctype)); }
PRIM(Boolean, uint8_t, "[Z") PRIM(Byte, int8_t, "[B") PRIM(Char, uint16_t, "[C") PRIM(Short, int16_t, "[S")
PRIM(Int, int32_t, "[I") PRIM(Long, int64_t, "[J") PRIM(Float, float, "[F") PRIM(Double, double, "[D")
static void *GetPrimitiveArrayCritical(void *e, void *a, uint8_t *c) { (void)e; if (c) *c = 0; jobj *o = O(a); return o ? o->data : NULL; }
static void ReleasePrimitiveArrayCritical(void *e, void *a, void *p, int32_t m) { (void)e; (void)a; (void)p; (void)m; }
static const uint16_t *GetStringCritical(void *e, void *s, uint8_t *c) { return GetStringChars(e, s, c); }
static void ReleaseStringCritical(void *e, void *s, const uint16_t *c) { ReleaseStringChars(e, s, c); }

static int32_t RegisterNatives(void *e, void *c, const void *m, int32_t n) {
  (void)e; struct { const char *n, *s; void *f; } const *mm = m;
  for (int i = 0; i < n; i++) JLOG("RegisterNatives %s.%s%s\n", O(c) ? O(c)->cls : "?", mm[i].n, mm[i].s);
  jni_register_natives(O(c) ? O(c)->cls : "?", m, n); return 0; }
static int32_t UnregisterNatives(void *e, void *c) { (void)e; (void)c; return 0; }
static int32_t MonitorEnter(void *e, void *o) { (void)e; (void)o; return 0; }
static int32_t MonitorExit(void *e, void *o) { (void)e; (void)o; return 0; }
static int32_t GetJavaVM(void *e, void **vm) { (void)e; *vm = jni_vm(); return 0; }
static void *NewDirectByteBuffer(void *e, void *addr, int64_t cap) { (void)e; jobj *o = mk(K_OBJECT, "java/nio/ByteBuffer"); o->data = addr; o->len = (int)cap; return o; }
static void *GetDirectBufferAddress(void *e, void *b) { (void)e; jobj *o = O(b); return o ? o->data : NULL; }
static int64_t GetDirectBufferCapacity(void *e, void *b) { (void)e; jobj *o = O(b); return o ? o->len : -1; }
static void *FromReflected(void *e, void *m) { (void)e; return m; }
static void *ToReflected(void *e, void *c, void *m, uint8_t s) { (void)e; (void)c; (void)s; return m; }
static void *DefineClass(void *e, const char *n, void *l, const void *b, int32_t sz) { (void)e; (void)l; (void)b; (void)sz; return find_class(n ? n : "anon"); }

static void unimpl_slot(void) __attribute__((unused));
static void unimpl_slot(void) { fprintf(stderr, "[jni] unimplemented JNIEnv slot called\n"); }

/* ---------- table (order = jni.h JNINativeInterface) ---------- */
static void *g_tab[233];
static void *g_env_ptr = g_tab;  // JNIEnv* points at a pointer to the table
static void build_table(void) {
  int i = 0;
  for (int k = 0; k < 233; k++) g_tab[k] = (void *)unimpl_slot;
  i = 4;
#define P(f) g_tab[i++] = (void *)(f)
  P(GetVersion); P(DefineClass); P(FindClass); P(FromReflected); P(FromReflected); P(ToReflected); P(GetSuperclass);
  P(IsAssignableFrom); P(ToReflected); P(Throw); P(ThrowNew); P(ExceptionOccurred); P(ExceptionDescribe); P(ExceptionClear);
  P(FatalError); P(PushLocalFrame); P(PopLocalFrame); P(NewRef); P(DelRef); P(DelRef); P(IsSameObject); P(NewRef);
  P(EnsureLocalCapacity); P(AllocObject); P(NewObject); P(NewObjectV); P(NewObjectA); P(GetObjectClass); P(IsInstanceOf);
  P(GetMethodID);
#define V3(T) P(Call##T##Method); P(Call##T##MethodV); P(Call##T##MethodA);
  V3(Object) V3(Boolean) V3(Byte) V3(Char) V3(Short) V3(Int) V3(Long) V3(Float) V3(Double) V3(Void)
#define N3(T) P(CallNonvirtual##T##Method); P(CallNonvirtual##T##MethodV); P(CallNonvirtual##T##MethodA);
  N3(Object) N3(Boolean) N3(Byte) N3(Char) N3(Short) N3(Int) N3(Long) N3(Float) N3(Double) N3(Void)
  P(GetFieldID);
  P(GetObjectField); P(GetBooleanField); P(GetByteField); P(GetCharField); P(GetShortField); P(GetIntField); P(GetLongField); P(GetFloatField); P(GetDoubleField);
  P(SetObjectField); P(SetBooleanField); P(SetByteField); P(SetCharField); P(SetShortField); P(SetIntField); P(SetLongField); P(SetFloatField); P(SetDoubleField);
  P(GetStaticMethodID);
#define S3(T) P(CallStatic##T##Method); P(CallStatic##T##MethodV); P(CallStatic##T##MethodA);
  S3(Object) S3(Boolean) S3(Byte) S3(Char) S3(Short) S3(Int) S3(Long) S3(Float) S3(Double) S3(Void)
  P(GetStaticFieldID);
  P(GetStaticObjectField); P(GetStaticBooleanField); P(GetStaticByteField); P(GetStaticCharField); P(GetStaticShortField); P(GetStaticIntField); P(GetStaticLongField); P(GetStaticFloatField); P(GetStaticDoubleField);
  P(SetStaticObjectField); P(SetStaticBooleanField); P(SetStaticByteField); P(SetStaticCharField); P(SetStaticShortField); P(SetStaticIntField); P(SetStaticLongField); P(SetStaticFloatField); P(SetStaticDoubleField);
  P(NewString); P(GetStringLength); P(GetStringChars); P(ReleaseStringChars);
  P(NewStringUTF); P(GetStringUTFLength); P(GetStringUTFChars); P(ReleaseStringUTFChars);
  P(GetArrayLength); P(NewObjectArray); P(GetObjectArrayElement); P(SetObjectArrayElement);
  P(NewBooleanArray); P(NewByteArray); P(NewCharArray); P(NewShortArray); P(NewIntArray); P(NewLongArray); P(NewFloatArray); P(NewDoubleArray);
  P(GetBooleanArrayElements); P(GetByteArrayElements); P(GetCharArrayElements); P(GetShortArrayElements); P(GetIntArrayElements); P(GetLongArrayElements); P(GetFloatArrayElements); P(GetDoubleArrayElements);
  P(ReleaseBooleanArrayElements); P(ReleaseByteArrayElements); P(ReleaseCharArrayElements); P(ReleaseShortArrayElements); P(ReleaseIntArrayElements); P(ReleaseLongArrayElements); P(ReleaseFloatArrayElements); P(ReleaseDoubleArrayElements);
  P(GetBooleanArrayRegion); P(GetByteArrayRegion); P(GetCharArrayRegion); P(GetShortArrayRegion); P(GetIntArrayRegion); P(GetLongArrayRegion); P(GetFloatArrayRegion); P(GetDoubleArrayRegion);
  P(SetBooleanArrayRegion); P(SetByteArrayRegion); P(SetCharArrayRegion); P(SetShortArrayRegion); P(SetIntArrayRegion); P(SetLongArrayRegion); P(SetFloatArrayRegion); P(SetDoubleArrayRegion);
  P(RegisterNatives); P(UnregisterNatives); P(MonitorEnter); P(MonitorExit); P(GetJavaVM);
  P(GetStringRegion); P(GetStringUTFRegion); P(GetPrimitiveArrayCritical); P(ReleasePrimitiveArrayCritical);
  P(GetStringCritical); P(ReleaseStringCritical); P(NewRef); P(DelRef); P(ExceptionCheck);
  P(NewDirectByteBuffer); P(GetDirectBufferAddress); P(GetDirectBufferCapacity); P(GetObjectRefType);
  if (i != 233) { fprintf(stderr, "jni table size %d != 233\n", i); abort(); }
}

/* ---------- JavaVM ---------- */
static int32_t vm_ok(void *vm) { (void)vm; return 0; }
static int32_t vm_attach(void *vm, void **env, void *a) { (void)vm; (void)a; *env = &g_env_ptr; return 0; }
static int32_t vm_getenv(void *vm, void **env, int32_t v) { (void)vm; (void)v; *env = &g_env_ptr; return 0; }
static void *g_vmtab[8] = {0, 0, 0, (void *)vm_ok, (void *)vm_attach, (void *)vm_ok, (void *)vm_getenv, (void *)vm_attach};
static void *g_vm_ptr = g_vmtab;
void *jni_vm(void) { return &g_vm_ptr; }
void *jni_env(void) { return &g_env_ptr; }

/* ---------- natives registry + Java-side method implementations ---------- */
typedef struct { const char *cls, *name, *sig; void *fn; } jnative;
static jnative g_nat[512]; static int g_nnat;
void jni_register_natives(const char *cls, const void *m, int n) {
  struct { const char *n, *s; void *f; } const *mm = m;
  for (int i = 0; i < n && g_nnat < 512; i++) g_nat[g_nnat++] = (jnative){cls, mm[i].n, mm[i].s, mm[i].f};
}
void *jni_native(const char *cls, const char *name) {
  for (int i = 0; i < g_nnat; i++) if (!strcmp(g_nat[i].name, name) && (!cls || !strcmp(g_nat[i].cls, cls))) return g_nat[i].fn;
  return NULL;
}

static struct { jni_impl_entry *t; int n; } g_itabs[8]; static int g_nitabs;
void jni_set_impls(jni_impl_entry *t, int n) { g_itabs[0].t = t; g_itabs[0].n = n; if (!g_nitabs) g_nitabs = 1; }
void jni_add_impls(jni_impl_entry *t, int n) { if (!g_nitabs) g_nitabs = 1; if (g_nitabs < 8) { g_itabs[g_nitabs].t = t; g_itabs[g_nitabs].n = n; g_nitabs++; } }
static jni_impl lookup_impl(const char *cls, const char *name, const char *sig) {
  for (int k = g_nitabs - 1; k >= 0; k--) for (int i = 0; i < g_itabs[k].n; i++) { jni_impl_entry *e = &g_itabs[k].t[i];
    if ((!e->cls || !strcmp(e->cls, cls)) && !strcmp(e->name, name) && (!e->sig || !strcmp(e->sig, sig))) return e->fn; }
  return NULL;
}
const void *jni_array_bytes(void *a, int *len) { jobj *o = O(a); if (len) *len = o ? o->len * o->esz : 0; return o ? o->data : NULL; }
void *jni_new_object(const char *cls) { return mk(K_OBJECT, cls); }
const char *jni_class_of(void *o) { jobj *j = O(o); return j ? j->cls : NULL; }

void jni_init(void) {
  build_table();
  g_activity = mk(K_OBJECT, "com/gmodvr/flat/GameActivity");
  g_context = g_activity;
}
void *jni_activity(void) { return g_activity; }
