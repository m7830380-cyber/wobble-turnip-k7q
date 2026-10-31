// Platform glue: SDL Android API replacements + GameActivity Java methods the engine calls.
#define _GNU_SOURCE
#include "jni_fake.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *env_or(const char *k, const char *d) { const char *v = getenv(k); return v && *v ? v : d; }

/* SDL Android extras (bundled libSDL2.so is replaced by native SDL2) */
static void *p_SDL_AndroidGetJNIEnv(void) { return jni_env(); }
static void *p_SDL_AndroidGetActivity(void) { return jni_activity(); }
static const char *p_SDL_AndroidGetInternalStoragePath(void) { return env_or("APP_DATA_PATH", "."); }
static const char *p_SDL_AndroidGetExternalStoragePath(void) { return env_or("APP_DATA_PATH", "."); }
static int p_SDL_AndroidGetExternalStorageState(void) { return 3; }
static int p_SDL_AndroidRequestPermission(const char *p) { (void)p; return 1; }
static int p_SDL_AndroidShowToast(const char *m, int d, int g, int x, int y) { (void)d; (void)g; (void)x; (void)y; fprintf(stderr, "[toast] %s\n", m); return 0; }
static void p_SDL_AndroidBackButton(void) {}
static int p_SDL_IsAndroidTV(void) { return 0; }
static int p_SDL_IsChromebook(void) { return 0; }
static int p_SDL_IsDeXMode(void) { return 0; }
static int p_SDL_GetAndroidSDKVersion(void) { return 33; }

/* ---------- Java methods (GameActivity & friends) ---------- */
#define J(name) static jvalue name(void *env, void *thiz, jvalue *a)
#define RET_STR(s) do { jvalue r = {0}; r.l = jni_new_string(s); return r; } while (0)
#define RET_I(v) do { jvalue r = {0}; r.i = (v); return r; } while (0)
#define RET_Z(v) do { jvalue r = {0}; r.z = (v); return r; } while (0)
#define RET_L(v) do { jvalue r = {0}; r.l = (v); return r; } while (0)
#define UNUSED (void)env; (void)thiz; (void)a

J(j_getLanguage) { UNUSED; RET_STR(env_or("GDIOS_LANG", "english")); }
J(j_getDataPath) { UNUSED; RET_STR(env_or("APP_DATA_PATH", ".")); }
J(j_getGamePath) { UNUSED; RET_STR(env_or("VALVE_GAME_PATH", ".")); }
J(j_emptyStr) { UNUSED; RET_STR(""); }
J(j_true) { UNUSED; RET_Z(1); }
J(j_false) { UNUSED; RET_Z(0); }
J(j_zero) { UNUSED; RET_I(0); }
J(j_null) { UNUSED; RET_L(NULL); }
J(j_self) { UNUSED; RET_L(jni_activity()); }
J(j_newobj) { UNUSED; RET_L(jni_new_object("java/lang/Object")); }
J(j_getClassLoader) { UNUSED; RET_L(jni_new_object("java/lang/ClassLoader")); }
J(j_loadClass) { (void)env; (void)thiz;
  const char *n = jni_str(a[0].l); extern void *jni_find_class_pub(const char *); RET_L(jni_find_class_pub(n ? n : "?")); }
J(j_getResources) { UNUSED; RET_L(jni_new_object("android/content/res/Resources")); }
J(j_getConfiguration) { UNUSED; RET_L(jni_new_object("android/content/res/Configuration")); }
J(j_toString) { UNUSED; RET_STR(""); }
J(j_steamid) { UNUSED; jvalue r = {0}; r.j = 0; return r; }
J(j_emptyLongArr) { UNUSED; RET_L(jni_new_array(0, 8, "[J")); }
J(j_emptyByteArr) { UNUSED; RET_L(jni_new_array(0, 1, "[B")); }
J(j_emptyStrArr) { UNUSED; RET_L(jni_new_array(0, 8, "[Ljava/lang/String;")); }

static jni_impl_entry g_impls[] = {
  {0, "getClassLoader", 0, j_getClassLoader},
  {0, "loadClass", 0, j_loadClass},
  {0, "getResources", 0, j_getResources},
  {0, "getConfiguration", 0, j_getConfiguration},
  {0, "toString", "()Ljava/lang/String;", j_toString},
  {0, "getLanguage", 0, j_getLanguage},
  {0, "getDataPath", 0, j_getDataPath},
  {0, "getGamePath", 0, j_getGamePath},
  {0, "getContext", 0, j_self},
  {0, "getActivity", 0, j_self},
  {0, "isSteamLoggedIn", 0, j_false},
};

void platform_init(void) {
  jni_init();
  jni_set_impls(g_impls, sizeof g_impls / sizeof *g_impls);
}

/* catch-all for unknown method return types handled in jni.c (empty string / 0) */

#define S(n) {#n, (void *)p_##n}
static struct { const char *n; void *p; } g_tab[] = {
  S(SDL_AndroidGetJNIEnv), S(SDL_AndroidGetActivity), S(SDL_AndroidGetInternalStoragePath),
  S(SDL_AndroidGetExternalStoragePath), S(SDL_AndroidGetExternalStorageState), S(SDL_AndroidRequestPermission),
  S(SDL_AndroidShowToast), S(SDL_AndroidBackButton), S(SDL_IsAndroidTV), S(SDL_IsChromebook), S(SDL_IsDeXMode),
  S(SDL_GetAndroidSDKVersion), {0, 0}};
void *platform_resolve(const char *n) {
  for (int i = 0; g_tab[i].n; i++) if (!strcmp(g_tab[i].n, n)) return g_tab[i].p;
  return NULL;
}
