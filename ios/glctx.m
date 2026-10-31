// SDL_GL_* replacement for iOS: the engine asks SDL for a GLES3 context, but SDL's UIKit backend gives
// EAGL (or nothing). We create an ANGLE (Metal) EGL context on a CAMetalLayer instead, like source-engine-ios.
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include <SDL.h>
#include <SDL_metal.h>
#include <stdio.h>

typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface;
typedef int EGLint; typedef intptr_t EGLAttrib;
#define EGL_NONE 0x3038
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES3_BIT 0x40
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x4
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_RENDER_BUFFER 0x3086
#define EGL_BACK_BUFFER 0x3084
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#define EGL_POWER_PREFERENCE_ANGLE 0x3482
#define EGL_HIGH_POWER_ANGLE 0x0002

extern void *ios_gl_proc(const char *n);
#define F(ret, name, args) static ret (*p_##name) args;
F(EGLDisplay, eglGetPlatformDisplay, (int, void *, const EGLAttrib *))
F(int, eglInitialize, (EGLDisplay, EGLint *, EGLint *))
F(int, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))
F(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *))
F(EGLSurface, eglCreateWindowSurface, (EGLDisplay, EGLConfig, void *, const EGLint *))
F(int, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))
F(int, eglSwapBuffers, (EGLDisplay, EGLSurface))
F(int, eglSwapInterval, (EGLDisplay, EGLint))
F(int, eglDestroyContext, (EGLDisplay, EGLContext))
F(EGLContext, eglGetCurrentContext, (void))
F(EGLint, eglGetError, (void))

static EGLDisplay g_dpy; static EGLConfig g_cfg; static EGLSurface g_surf; static SDL_Window *g_win;
static int g_depth = 24, g_stencil = 8, g_alpha = 8;

static int load(void) {
  if (p_eglInitialize) return 1;
#define L(n) p_##n = ios_gl_proc(#n); if (!p_##n) { fprintf(stderr, "glctx: missing %s\n", #n); return 0; }
  L(eglGetPlatformDisplay) L(eglInitialize) L(eglChooseConfig) L(eglCreateContext) L(eglCreateWindowSurface)
  L(eglMakeCurrent) L(eglSwapBuffers) L(eglSwapInterval) L(eglDestroyContext) L(eglGetCurrentContext) L(eglGetError)
  return 1;
}

int gs_SDL_GL_SetAttribute(SDL_GLattr a, int v) {
  if (a == SDL_GL_DEPTH_SIZE) g_depth = v; else if (a == SDL_GL_STENCIL_SIZE) g_stencil = v; else if (a == SDL_GL_ALPHA_SIZE) g_alpha = v;
  return 0;
}
int gs_SDL_GL_LoadLibrary(const char *p) { (void)p; return load() ? 0 : -1; }
void gs_SDL_GL_UnloadLibrary(void) {}

static void cfg_layer(CAMetalLayer *layer) {
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm; layer.framebufferOnly = YES;
  CGFloat s = UIScreen.mainScreen.nativeScale; layer.contentsScale = s;
  CGSize pt = UIScreen.mainScreen.bounds.size;
  if ([layer.delegate isKindOfClass:UIView.class]) pt = ((UIView *)layer.delegate).bounds.size;
  layer.drawableSize = CGSizeMake(pt.width * s, pt.height * s);
}

SDL_GLContext gs_SDL_GL_CreateContext(SDL_Window *w) {
  if (!load()) return NULL;
  if (!g_dpy) {
    EGLAttrib da[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_POWER_PREFERENCE_ANGLE, EGL_HIGH_POWER_ANGLE, EGL_NONE};
    g_dpy = p_eglGetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, NULL, da);
    if (!g_dpy || !p_eglInitialize(g_dpy, NULL, NULL)) { fprintf(stderr, "glctx: egl init failed %x\n", p_eglGetError()); return NULL; }
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                   EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, g_alpha, EGL_DEPTH_SIZE, g_depth, EGL_STENCIL_SIZE, g_stencil, EGL_NONE};
    EGLint n = 0;
    if (!p_eglChooseConfig(g_dpy, ca, &g_cfg, 1, &n) || !n) { fprintf(stderr, "glctx: no config %x\n", p_eglGetError()); return NULL; }
  }
  EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  EGLContext c = p_eglCreateContext(g_dpy, g_cfg, NULL, xa);
  if (!c) { fprintf(stderr, "glctx: create context failed %x\n", p_eglGetError()); return NULL; }
  if (!g_surf) {
    g_win = w;
    SDL_MetalView v = SDL_Metal_CreateView(w);
    CAMetalLayer *layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(v);
    cfg_layer(layer);
    EGLint sa[] = {EGL_RENDER_BUFFER, EGL_BACK_BUFFER, EGL_NONE};
    g_surf = p_eglCreateWindowSurface(g_dpy, g_cfg, (__bridge void *)layer, sa);
    if (!g_surf) { fprintf(stderr, "glctx: surface failed %x\n", p_eglGetError()); return NULL; }
  }
  p_eglMakeCurrent(g_dpy, g_surf, g_surf, c);
  fprintf(stderr, "glctx: ANGLE Metal context %p ready\n", c);
  return c;
}
int gs_SDL_GL_MakeCurrent(SDL_Window *w, SDL_GLContext c) {
  (void)w; if (!g_dpy) return -1;
  return p_eglMakeCurrent(g_dpy, c ? g_surf : NULL, c ? g_surf : NULL, c) ? 0 : -1;
}
void gs_SDL_GL_SwapWindow(SDL_Window *w) { (void)w; if (g_dpy && g_surf) p_eglSwapBuffers(g_dpy, g_surf); }
int gs_SDL_GL_SetSwapInterval(int i) { return g_dpy && p_eglSwapInterval(g_dpy, i) ? 0 : -1; }
int gs_SDL_GL_GetSwapInterval(void) { return 1; }
SDL_GLContext gs_SDL_GL_GetCurrentContext(void) { return p_eglGetCurrentContext ? p_eglGetCurrentContext() : NULL; }
void gs_SDL_GL_DeleteContext(SDL_GLContext c) { if (g_dpy && c) p_eglDestroyContext(g_dpy, c); }
void gs_SDL_GL_GetDrawableSize(SDL_Window *w, int *x, int *y) { SDL_Metal_GetDrawableSize(w, x, y); }
int gs_SDL_GL_GetAttribute(SDL_GLattr a, int *v) {
  *v = a == SDL_GL_DEPTH_SIZE ? g_depth : a == SDL_GL_STENCIL_SIZE ? g_stencil : a == SDL_GL_CONTEXT_MAJOR_VERSION ? 3 :
       a == SDL_GL_CONTEXT_PROFILE_MASK ? SDL_GL_CONTEXT_PROFILE_ES : a == SDL_GL_RED_SIZE || a == SDL_GL_GREEN_SIZE || a == SDL_GL_BLUE_SIZE ? 8 :
       a == SDL_GL_ALPHA_SIZE ? g_alpha : a == SDL_GL_DOUBLEBUFFER ? 1 : 0;
  return 0;
}
void *gs_SDL_GL_GetProcAddress(const char *n) { return ios_gl_proc(n); }
int gs_SDL_GL_ExtensionSupported(const char *e) { (void)e; return 0; }

void *glctx_resolve(const char *n) {
#define R(x) if (!strcmp(n, #x)) return (void *)gs_##x;
  R(SDL_GL_SetAttribute) R(SDL_GL_GetAttribute) R(SDL_GL_LoadLibrary) R(SDL_GL_UnloadLibrary) R(SDL_GL_CreateContext)
  R(SDL_GL_MakeCurrent) R(SDL_GL_SwapWindow) R(SDL_GL_SetSwapInterval) R(SDL_GL_GetSwapInterval) R(SDL_GL_GetCurrentContext)
  R(SDL_GL_DeleteContext) R(SDL_GL_GetDrawableSize) R(SDL_GL_GetProcAddress) R(SDL_GL_ExtensionSupported)
  return NULL;
}
