// JNI replacements for GDroid's Steam multiplayer classes (engine/client/GameUI/ServerBrowser call these):
//  com/sourcevrport/platform/GModSteamAuth      request(String,int,long,boolean) state() error() steamID() ticket() cancel() shutdown()
//  com/sourcevrport/platform/GModServerDirectory start() poll(int) cancel(int)
//  com/sourcevrport/platform/GModSteamPersona   available() request(long,bool) serial() changed() name(long) relationship(long) avatar(long,int)
// Backend: smp.inc (sm_*).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../shim/jni_fake.h"
#include "sw.h"

#define R0 jvalue r; memset(&r, 0, sizeof r)
#define J(n) static jvalue n(void *env, void *thiz, jvalue *a)

// ---- GModSteamAuth ----
J(au_request) { (void)env; (void)thiz; R0; const char *addr = jni_str(a[0].l);
  sm_auth_request(addr ? addr : "", a[1].i, (uint64_t)a[2].j, a[3].z); return r; }
J(au_state) { (void)env; (void)thiz; (void)a; R0; r.i = sm_auth_state(); return r; }
J(au_error) { (void)env; (void)thiz; (void)a; R0; char e[320]; sm_auth_error(e, sizeof e); r.l = jni_new_string(e); return r; }
J(au_steamid) { (void)env; (void)thiz; (void)a; R0; r.j = (int64_t)sm_auth_steamid(); return r; }
J(au_ticket) { (void)env; (void)thiz; (void)a; R0; static uint8_t b[2100]; int n = sm_auth_cookie(b, sizeof b, NULL);
  if (n > 0) { void *arr = jni_new_array(n, 1, "[B"); memcpy((void *)jni_array_bytes(arr, NULL), b, n); r.l = arr; } return r; }
J(au_cancel) { (void)env; (void)thiz; (void)a; R0; sm_auth_cancel(); return r; }
J(au_noop) { (void)env; (void)thiz; (void)a; R0; return r; }

// ---- GModServerDirectory ----
J(sd_start) { (void)env; (void)thiz; (void)a; R0; r.i = sm_dir_start(); return r; }
J(sd_poll) { (void)env; (void)thiz; R0; char *ev[256]; int n = sm_dir_poll(a[0].i, ev, 256);
  if (n < 0) { ev[0] = strdup("done"); n = 1; }
  void *arr = jni_new_array(n, 8, "[Ljava/lang/String;"); void **d = (void **)jni_array_bytes(arr, NULL);
  for (int i = 0; i < n; i++) { d[i] = jni_new_string(ev[i]); free(ev[i]); }
  r.l = arr; return r; }
J(sd_cancel) { (void)env; (void)thiz; R0; sm_dir_cancel(a[0].i); return r; }

// ---- GModSteamPersona ----
J(pe_available) { (void)env; (void)thiz; (void)a; R0; r.z = (uint8_t)sm_persona_available(); return r; }
J(pe_request) { (void)env; (void)thiz; R0; sm_persona_request((uint64_t)a[0].j, a[1].z); return r; }
J(pe_serial) { (void)env; (void)thiz; (void)a; R0; r.j = (int64_t)sm_persona_serial(); return r; }
J(pe_changed) { (void)env; (void)thiz; (void)a; R0; uint64_t c[256]; int n = sm_persona_changed(c, 256);
  void *arr = jni_new_array(n, 8, "[J"); memcpy((void *)jni_array_bytes(arr, NULL), c, sizeof(uint64_t) * n); r.l = arr; return r; }
J(pe_name) { (void)env; (void)thiz; R0; char n[64]; if (sm_persona_name((uint64_t)a[0].j, n, sizeof n, NULL)) r.l = jni_new_string(n); return r; }
J(pe_rel) { (void)env; (void)thiz; R0; r.i = sm_relationship((uint64_t)a[0].j); return r; }
J(pe_avatar) { (void)env; (void)thiz; R0; int sz = a[1].i; if (sz != 64 && sz != 184) return r;
  if (!sm_avatar((uint64_t)a[0].j, sz, NULL, NULL)) return r;
  void *arr = jni_new_array(sz * sz * 4, 1, "[B");
  if (!sm_avatar((uint64_t)a[0].j, sz, (uint8_t *)jni_array_bytes(arr, NULL), NULL)) return r;
  r.l = arr; return r; }
J(pe_reset) { (void)env; (void)thiz; (void)a; R0; return r; }

#define A "com/sourcevrport/platform/GModSteamAuth"
#define D "com/sourcevrport/platform/GModServerDirectory"
#define P "com/sourcevrport/platform/GModSteamPersona"
static jni_impl_entry g_mp[] = {
  {A, "request", 0, au_request}, {A, "state", 0, au_state}, {A, "error", 0, au_error}, {A, "steamID", 0, au_steamid},
  {A, "ticket", 0, au_ticket}, {A, "cancel", 0, au_cancel}, {A, "shutdown", 0, au_cancel}, {A, "initialize", 0, au_noop},
  {D, "start", 0, sd_start}, {D, "poll", 0, sd_poll}, {D, "cancel", 0, sd_cancel}, {D, "initialize", 0, au_noop}, {D, "shutdown", 0, au_noop},
  {P, "available", 0, pe_available}, {P, "request", 0, pe_request}, {P, "serial", 0, pe_serial}, {P, "changed", 0, pe_changed},
  {P, "name", 0, pe_name}, {P, "relationship", 0, pe_rel}, {P, "avatar", 0, pe_avatar}, {P, "reset", 0, pe_reset},
};
void mp_jni_init(void) { jni_add_impls(g_mp, sizeof g_mp / sizeof *g_mp); }
