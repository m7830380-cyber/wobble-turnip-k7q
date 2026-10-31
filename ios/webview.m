// iOS implementation of com.sourcevrport.platform.GModWebView (GDroid's DHTML / main menu browser).
// Android renders a WebView into an ImageReader and hands the engine unpremultiplied RGBA frames via pixels(id).
// Here: offscreen WKWebView per page + takeSnapshot -> RGBA, assets served via a custom URL scheme.
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#include "../shim/jni_fake.h"
#include <stdatomic.h>

char *ios_documents_path(void);
static NSString *const kBase = @"gmodasset://g/";

@interface GPage : NSObject <WKScriptMessageHandler, WKNavigationDelegate, WKUIDelegate>
@property(nonatomic) int pid, width, height;
@property(nonatomic, strong) WKWebView *web;
@property(nonatomic, strong) NSMutableArray<NSString *> *events;
@property(nonatomic, strong) NSData *pixels;
@property(nonatomic, strong) NSMutableDictionary<NSString *, NSArray *> *bindings;
@property(nonatomic) BOOL snapping, closed;
@property(nonatomic) CFTimeInterval lastSnap;
@property(nonatomic) void *arr;  // reused jni byte array
@property(nonatomic) int arrLen;
@property(nonatomic) int mouseX, mouseY, buttons;
@end

@interface GScheme : NSObject <WKURLSchemeHandler>
@end

static NSMutableDictionary<NSNumber *, GPage *> *g_pages;
static NSLock *g_lock;
static atomic_int g_next = 1;
static GScheme *g_scheme;
static CADisplayLink *g_link;

static NSString *json(NSArray *a) {
  NSData *d = [NSJSONSerialization dataWithJSONObject:a options:0 error:nil];
  return d ? [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] : @"[]";
}
static NSString *jsq(NSString *s) { NSString *j = json(@[ s ?: @"" ]); return [j substringWithRange:NSMakeRange(1, j.length - 2)]; }

static NSString *assetURL(NSString *s) {
  if ([s hasPrefix:@"asset://"]) return [kBase stringByAppendingString:[s substringFromIndex:8]];
  if ([s hasPrefix:@"file://"]) {
    NSString *r = [s substringFromIndex:7]; while ([r hasPrefix:@"/"]) r = [r substringFromIndex:1];
    return [kBase stringByAppendingString:r];
  }
  if ([s hasPrefix:@"html/"] || [s hasPrefix:@"data/"] || [s hasPrefix:@"lua/"]) return [kBase stringByAppendingString:s];
  return s;
}

@implementation GPage
- (void)event:(NSArray *)a {
  NSString *s = json(a);
  [g_lock lock]; if (self.events.count < 1024) [self.events addObject:s]; [g_lock unlock];
}
- (NSString *)bindingJS {
  NSMutableString *sb = [NSMutableString stringWithString:
    @"(()=>{if(window.top!==window)return;window.__gmodPost=window.__gmodPost||((o,f,a,i)=>{window.webkit.messageHandlers.__gmodNative.postMessage(JSON.stringify([o,f,a,i]));});"
     "window.__gmodCallbacks=window.__gmodCallbacks||{};window.__gmodCallId=window.__gmodCallId||0;"
     "window.__gmodReply=(id,args)=>{let f=window.__gmodCallbacks[id];delete window.__gmodCallbacks[id];if(f)f.apply(null,args);};"];
  for (NSArray *b in self.bindings.allValues) {
    NSString *o = jsq(b[0]), *f = jsq(b[1]);
    if ([b[1] length] == 0) [sb appendFormat:@"window[%@]=window[%@]||{};", o, o];
    else [sb appendFormat:@"window[%@]=window[%@]||{};window[%@][%@]=function(...args){let id=++window.__gmodCallId;if(typeof args[args.length-1]==='function')window.__gmodCallbacks[id]=args.pop();window.__gmodPost(%@,%@,JSON.stringify(args),id);};", o, o, o, f, o, f];
  }
  [sb appendString:@"})()"];
  return sb;
}
- (void)installBindings {
  WKUserContentController *uc = self.web.configuration.userContentController;
  [uc removeAllUserScripts];
  [uc addUserScript:[[WKUserScript alloc] initWithSource:
    @"(()=>{const P=(l,a)=>{try{window.webkit.messageHandlers.__gmodConsole.postMessage(l+Array.from(a).map(String).join(' '))}catch(e){}};"
     "const c=console;['log','warn','error','info'].forEach(k=>{const o=c[k].bind(c);c[k]=(...a)=>{P('',a);o(...a)}});"
     "window.addEventListener('error',e=>P('JS error: ',[e.message+' @'+e.filename+':'+e.lineno]));})()"
                                          injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES]];
  NSString *js = [self bindingJS];
  [uc addUserScript:[[WKUserScript alloc] initWithSource:js injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES]];
  [self.web evaluateJavaScript:js completionHandler:nil];
}
- (void)userContentController:(WKUserContentController *)uc didReceiveScriptMessage:(WKScriptMessage *)m {
  if ([m.name isEqualToString:@"__gmodConsole"]) { [self event:@[ @"console", [m.body description] ]]; fprintf(stderr, "[web%d] %s\n", self.pid, [[m.body description] UTF8String]); return; }
  if (![m.body isKindOfClass:NSString.class]) return;
  NSArray *a = [NSJSONSerialization JSONObjectWithData:[m.body dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
  if (![a isKindOfClass:NSArray.class] || a.count < 4) { [self event:@[ @"error", @"Invalid Lua callback arguments" ]]; return; }
  id args = [NSJSONSerialization JSONObjectWithData:[a[2] dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil] ?: @[];
  [self event:@[ @"call", a[0], a[1], args, a[3] ]];
}
- (void)webView:(WKWebView *)w didStartProvisionalNavigation:(WKNavigation *)n {
  [self event:@[ @"begin", w.URL.absoluteString ?: @"about:blank" ]];
}
- (void)webView:(WKWebView *)w didFinishNavigation:(WKNavigation *)n {
  [self.web evaluateJavaScript:[self bindingJS] completionHandler:nil];
  [self event:@[ @"ready", w.URL.absoluteString ?: @"about:blank" ]];
  if (w.title.length) [self event:@[ @"title", w.title ]];
}
- (void)webView:(WKWebView *)w didFailProvisionalNavigation:(WKNavigation *)n withError:(NSError *)e {
  fprintf(stderr, "[web%d] load failed: %s\n", self.pid, e.localizedDescription.UTF8String);
}
- (WKWebView *)webView:(WKWebView *)w createWebViewWithConfiguration:(WKWebViewConfiguration *)c forNavigationAction:(WKNavigationAction *)a windowFeatures:(WKWindowFeatures *)f {
  [self event:@[ @"child", w.URL.absoluteString ?: @"", a.request.URL.absoluteString ?: @"", @YES ]];
  return nil;
}
- (void)snap {
  if (self.snapping || self.closed || !self.web) return;
  CFTimeInterval now = CACurrentMediaTime(); if (now - self.lastSnap < 0.033) return;
  self.snapping = YES; self.lastSnap = now;
  WKSnapshotConfiguration *c = [WKSnapshotConfiguration new];
  c.rect = self.web.bounds; c.afterScreenUpdates = NO;
  c.snapshotWidth = @(self.width / UIScreen.mainScreen.scale);
  int w = self.width, h = self.height;
  [self.web takeSnapshotWithConfiguration:c completionHandler:^(UIImage *img, NSError *err) {
    self.snapping = NO;
    if (!img || self.closed) return;
    NSMutableData *d = [NSMutableData dataWithLength:(size_t)w * h * 4];
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(d.mutableBytes, w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) return;
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img.CGImage);
    CGContextRelease(ctx);
    uint8_t *p = d.mutableBytes;
    for (size_t i = 0, n = (size_t)w * h * 4; i < n; i += 4) {
      unsigned a = p[i + 3];
      if (a != 255) for (int k = 0; k < 3; k++) p[i + k] = a ? (uint8_t)MIN(255u, (p[i + k] * 255u + a / 2) / a) : 0;
    }
    [g_lock lock]; self.pixels = d; [g_lock unlock];
  }];
}
@end

/* ---------- asset scheme: gmodasset://g/<path> -> Documents/garrysmod/<path> (or engine readAsset) ---------- */
static NSString *mimeFor(NSString *p) {
  NSString *e = p.pathExtension.lowercaseString;
  NSDictionary *m = @{@"html" : @"text/html", @"htm" : @"text/html", @"js" : @"application/javascript", @"css" : @"text/css",
                      @"png" : @"image/png", @"jpg" : @"image/jpeg", @"jpeg" : @"image/jpeg", @"gif" : @"image/gif", @"svg" : @"image/svg+xml",
                      @"json" : @"application/json", @"ttf" : @"font/ttf", @"woff" : @"font/woff", @"woff2" : @"font/woff2",
                      @"txt" : @"text/plain", @"ogg" : @"audio/ogg", @"mp3" : @"audio/mpeg", @"wav" : @"audio/wav", @"webm" : @"video/webm"};
  return m[e] ?: @"application/octet-stream";
}
static NSData *readFile(NSString *rel) {
  NSString *root = [NSString stringWithUTF8String:ios_documents_path()];
  NSArray *cands = @[ [root stringByAppendingPathComponent:[@"garrysmod" stringByAppendingPathComponent:rel]],
                      [root stringByAppendingPathComponent:[@"garrysmod" stringByAppendingPathComponent:rel.lowercaseString]],
                      [root stringByAppendingPathComponent:rel] ];
  for (NSString *p in cands) { NSData *d = [NSData dataWithContentsOfFile:p]; if (d) return d; }
  // engine filesystem (VPKs): GModWebView.readAsset is a native registered by libclient
  typedef void *(*ra_fn)(void *, void *, void *);
  ra_fn f = (ra_fn)jni_native("com/sourcevrport/platform/GModWebView", "readAsset");
  if (f) {
    extern void *jni_find_class_pub(const char *);
    void *arr = f(jni_env(), jni_find_class_pub("com/sourcevrport/platform/GModWebView"), jni_new_string(rel.UTF8String));
    int n = 0; const void *bytes = jni_array_bytes(arr, &n);
    if (bytes && n > 0) return [NSData dataWithBytes:bytes length:n];
  }
  return nil;
}
@implementation GScheme
- (void)webView:(WKWebView *)w startURLSchemeTask:(id<WKURLSchemeTask>)t {
  NSString *path = t.request.URL.path ?: @"";
  while ([path hasPrefix:@"/"]) path = [path substringFromIndex:1];
  path = [path stringByRemovingPercentEncoding] ?: path;
  if ([path hasPrefix:@"garrysmod/"]) path = [path substringFromIndex:10];
  NSData *d = nil;
  if (![path containsString:@".."]) {
    if ([path hasPrefix:@"mapimage/"]) {
      NSString *m = [path substringFromIndex:9];
      for (NSString *dir in @[ @"maps/thumb/", @"materials/maps/thumb/", @"materials/maps/" ])
        for (NSString *ext in @[ @".png", @".jpg", @".jpeg" ]) if (!d) d = readFile([NSString stringWithFormat:@"%@%@%@", dir, m, ext]);
      if (!d) { d = readFile(@"materials/gui/noicon.png"); path = @"x.png"; }
    } else d = readFile(path);
  }
  NSInteger code = d ? 200 : 404;
  NSHTTPURLResponse *r = [[NSHTTPURLResponse alloc] initWithURL:t.request.URL statusCode:code HTTPVersion:@"HTTP/1.1"
    headerFields:@{@"Content-Type" : mimeFor(path), @"Access-Control-Allow-Origin" : @"*", @"Content-Length" : [NSString stringWithFormat:@"%lu", (unsigned long)d.length]}];
  if (!d) fprintf(stderr, "[web] 404 %s\n", path.UTF8String);
  [t didReceiveResponse:r];
  if (d) [t didReceiveData:d];
  [t didFinish];
}
- (void)webView:(WKWebView *)w stopURLSchemeTask:(id<WKURLSchemeTask>)t {}
@end

@interface GTick : NSObject
@end
@implementation GTick
- (void)tick { [g_lock lock]; NSArray *ps = g_pages.allValues; [g_lock unlock]; for (GPage *p in ps) [p snap]; }
@end
static GTick *g_tick;

static UIView *hostView(void) {
  for (UIScene *s in UIApplication.sharedApplication.connectedScenes)
    if ([s isKindOfClass:UIWindowScene.class]) for (UIWindow *w in ((UIWindowScene *)s).windows) if (w.isKeyWindow || w) return w;
  return nil;
}

static GPage *page(int id) { [g_lock lock]; GPage *p = g_pages[@(id)]; [g_lock unlock]; return p; }
static void onMain(void (^b)(void)) { if (NSThread.isMainThread) b(); else dispatch_async(dispatch_get_main_queue(), b); }

static void doResize(GPage *p, int w, int h) {
  w = MAX(64, MIN(4096, w)); h = MAX(64, MIN(4096, h));
  p.width = w; p.height = h;
  [g_lock lock]; p.pixels = nil; [g_lock unlock];
  CGFloat s = UIScreen.mainScreen.scale;
  // CSS px == texture px (Android density 160): view is w x h points, snapshot downsized to w/s points * s
  p.web.frame = CGRectMake(-w - 64, 0, w, h);
  (void)s;
}

static int wv_create(int w, int h, BOOL bridge) {
  int id = atomic_fetch_add(&g_next, 1);
  GPage *p = [GPage new]; p.pid = id; p.events = [NSMutableArray new]; p.bindings = [NSMutableDictionary new];
  [g_lock lock]; g_pages[@(id)] = p; [g_lock unlock];
  onMain(^{
    WKWebViewConfiguration *c = [WKWebViewConfiguration new];
    [c setURLSchemeHandler:g_scheme forURLScheme:@"gmodasset"];
    c.allowsInlineMediaPlayback = YES; c.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
    c.preferences.javaScriptCanOpenWindowsAutomatically = YES;
    [c.userContentController addScriptMessageHandler:p name:@"__gmodNative"];
    [c.userContentController addScriptMessageHandler:p name:@"__gmodConsole"];
    WKWebView *web = [[WKWebView alloc] initWithFrame:CGRectMake(-4000, 0, w, h) configuration:c];
    web.opaque = NO; web.backgroundColor = UIColor.clearColor; web.scrollView.backgroundColor = UIColor.clearColor;
    web.scrollView.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
    web.customUserAgent = @"Mozilla/5.0 (Linux; Android 13) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Mobile Safari/537.36 GMod/13 GModVR";
    web.navigationDelegate = p; web.UIDelegate = p;
    if (@available(iOS 16.4, *)) web.inspectable = YES;
    p.web = web;
    doResize(p, w, h);
    [p installBindings];
    UIView *host = hostView();
    if (host) { [host addSubview:web]; [host sendSubviewToBack:web]; }
    if (!g_link) { g_tick = [GTick new]; g_link = [CADisplayLink displayLinkWithTarget:g_tick selector:@selector(tick)]; [g_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes]; }
    fprintf(stderr, "[web] page %d created %dx%d host=%p\n", id, w, h, (__bridge void *)host);
  });
  return id;
}

static void wv_js(int id, NSString *js) { onMain(^{ GPage *p = page(id); if (p.web) [p.web evaluateJavaScript:js completionHandler:nil]; }); }

static void wv_command(int id, int op, NSString *s, int a, int b) {
  onMain(^{
    GPage *p = page(id); if (!p.web) return;
    switch (op) {
      case 0: { NSString *u = assetURL(s); fprintf(stderr, "[web%d] navigate %s\n", id, u.UTF8String);
        NSURL *url = [NSURL URLWithString:u]; if (url) [p.web loadRequest:[NSURLRequest requestWithURL:url]]; break; }
      case 1: [p.web loadHTMLString:[s stringByReplacingOccurrencesOfString:@"asset://" withString:kBase] baseURL:[NSURL URLWithString:kBase]]; break;
      case 2: [p.web evaluateJavaScript:s completionHandler:nil]; break;
      case 3: doResize(p, a, b); break;
      case 4: [p.web stopLoading]; break;
      case 5: [p.web evaluateJavaScript:[NSString stringWithFormat:@"(()=>{let e=document.elementFromPoint(%d,%d);while(e&&e!==document.body&&!(e.scrollHeight>e.clientHeight&&getComputedStyle(e).overflowY!=='visible'))e=e.parentElement;(e||document.scrollingElement||document.body).scrollBy(0,%d);})()", p.mouseX, p.mouseY, -a * 48] completionHandler:nil]; break;
      case 6: break;  // raw Android key codes: text input goes through op 7
      case 7: [p.web evaluateJavaScript:[NSString stringWithFormat:@"document.execCommand('insertText',false,%@)", jsq(s)] completionHandler:nil]; break;
      case 8: [p.web evaluateJavaScript:[NSString stringWithFormat:@"(()=>{%@})()", s] completionHandler:nil]; break;
    }
  });
}

static void wv_pointer(int id, int action, int x, int y, int button) {
  onMain(^{
    GPage *p = page(id); if (!p.web) return;
    p.mouseX = x; p.mouseY = y;
    int bit = button == 1 ? 2 : button == 2 ? 4 : 1;
    if (action == 1) p.buttons |= bit; else if (action == 2) p.buttons &= ~bit;
    NSString *type = action == 1 ? @"mousedown" : action == 2 ? @"mouseup" : @"mousemove";
    int jsb = button == 1 ? 2 : button == 2 ? 1 : 0;
    NSString *extra = @"";
    if (action == 2 && (button == 0 || button == 3)) extra = @"e.dispatchEvent(new MouseEvent('click',o));if(e.focus)e.focus();";
    if (action == 2 && button == 1) extra = @"e.dispatchEvent(new MouseEvent('contextmenu',o));";
    NSString *js = [NSString stringWithFormat:@"(()=>{let e=document.elementFromPoint(%d,%d);if(e){let o={bubbles:true,cancelable:true,view:window,button:%d,buttons:%d,clientX:%d,clientY:%d};"
      "e.dispatchEvent(new PointerEvent('pointer%@',o));e.dispatchEvent(new MouseEvent('%@',o));%@}})()",
      x, y, jsb, p.buttons, x, y, [type substringFromIndex:5], type, extra];
    [p.web evaluateJavaScript:js completionHandler:nil];
  });
}

static void wv_destroy(int id) {
  GPage *p = page(id); if (!p) return;
  p.closed = YES;
  [g_lock lock]; [g_pages removeObjectForKey:@(id)]; [g_lock unlock];
  onMain(^{
    [p.web stopLoading];
    [p.web.configuration.userContentController removeAllScriptMessageHandlers];
    [p.web removeFromSuperview]; p.web = nil;
  });
}

/* ---------- JNI glue ---------- */
#define J(name) static jvalue name(void *env, void *thiz, jvalue *a)
#define S(i) ([NSString stringWithUTF8String:jni_str(a[i].l) ?: ""])
J(j_create2) { (void)env; (void)thiz; jvalue r = {0}; r.i = wv_create(a[0].i, a[1].i, YES); return r; }
J(j_create3) { (void)env; (void)thiz; jvalue r = {0}; r.i = wv_create(a[0].i, a[1].i, a[2].z); return r; }
J(j_create4) { (void)env; (void)thiz; jvalue r = {0}; r.i = wv_create(a[0].i, a[1].i, a[2].z); return r; }
J(j_command) { (void)env; (void)thiz; wv_command(a[0].i, a[1].i, S(2), a[3].i, a[4].i); jvalue r = {0}; return r; }
J(j_navigate) { (void)env; (void)thiz; wv_command(a[0].i, 0, S(1), 0, 0); jvalue r = {0}; return r; }
J(j_html) { (void)env; (void)thiz; wv_command(a[0].i, 1, S(1), 0, 0); jvalue r = {0}; return r; }
J(j_script) { (void)env; (void)thiz; wv_command(a[0].i, 2, S(1), 0, 0); jvalue r = {0}; return r; }
J(j_resize) { (void)env; (void)thiz; wv_command(a[0].i, 3, @"", a[1].i, a[2].i); jvalue r = {0}; return r; }
J(j_stop) { (void)env; (void)thiz; wv_command(a[0].i, 4, @"", 0, 0); jvalue r = {0}; return r; }
J(j_wheel) { (void)env; (void)thiz; wv_command(a[0].i, 5, @"", a[1].i, 0); jvalue r = {0}; return r; }
J(j_text) { (void)env; (void)thiz; wv_command(a[0].i, 7, S(1), 0, 0); jvalue r = {0}; return r; }
J(j_key) { (void)env; (void)thiz; (void)a; jvalue r = {0}; return r; }
J(j_destroy) { (void)env; (void)thiz; wv_destroy(a[0].i); jvalue r = {0}; return r; }
J(j_setForeground) { (void)env; (void)thiz; (void)a; jvalue r = {0}; return r; }
J(j_pointer) { (void)env; (void)thiz;
  // pointer(id, action, x, y, button)  [Java: lambda(i5=button, i2=action, x, y, id)]
  wv_pointer(a[0].i, a[1].i, a[2].i, a[3].i, a[4].i); jvalue r = {0}; return r; }
J(j_bind) { (void)env; (void)thiz;
  int id = a[0].i; NSString *o = S(1), *f = S(2);
  onMain(^{ GPage *p = page(id); if (!p) return; p.bindings[[NSString stringWithFormat:@"%@\x1f%@", o, f]] = @[ o, f ]; [p installBindings]; });
  jvalue r = {0}; return r; }
J(j_reply) { (void)env; (void)thiz;
  wv_js(a[0].i, [NSString stringWithFormat:@"window.__gmodReply&&window.__gmodReply(%d,%@);", a[1].i, S(2)]); jvalue r = {0}; return r; }
J(j_events) { (void)env; (void)thiz;
  GPage *p = page(a[0].i); NSArray *ev = @[];
  if (p) { [g_lock lock]; NSUInteger n = MIN((NSUInteger)128, p.events.count); ev = [p.events subarrayWithRange:NSMakeRange(0, n)];
    [p.events removeObjectsInRange:NSMakeRange(0, n)]; [g_lock unlock]; }
  jobj *arr = jni_new_array((int)ev.count, 8, "[Ljava/lang/String;");
  void **d = (void **)jni_array_bytes(arr, NULL);
  for (NSUInteger i = 0; i < ev.count; i++) d[i] = jni_new_string([ev[i] UTF8String]);
  jvalue r = {0}; r.l = arr; return r; }
J(j_pixels) { (void)env; (void)thiz;
  GPage *p = page(a[0].i); jvalue r = {0}; if (!p) return r;
  [g_lock lock]; NSData *d = p.pixels; p.pixels = nil; [g_lock unlock];
  if (!d) return r;
  if (!p.arr || p.arrLen != (int)d.length) { p.arr = jni_new_array((int)d.length, 1, "[B"); p.arrLen = (int)d.length; }  // reused: engine copies it out
  memcpy((void *)jni_array_bytes(p.arr, NULL), d.bytes, d.length);
  r.l = p.arr; return r; }

#define C "com/sourcevrport/platform/GModWebView"
static jni_impl_entry g_wv[] = {
  {C, "create", "(II)I", j_create2}, {C, "createReadOnly", "(II)I", j_create2}, {C, "create", "(IIZ)I", j_create3}, {C, "create", "(IIZZ)I", j_create4},
  {C, "command", 0, j_command}, {C, "navigate", 0, j_navigate}, {C, "html", 0, j_html}, {C, "script", 0, j_script},
  {C, "resize", 0, j_resize}, {C, "stop", 0, j_stop}, {C, "wheel", 0, j_wheel}, {C, "text", 0, j_text}, {C, "key", 0, j_key},
  {C, "destroy", 0, j_destroy}, {C, "setForeground", 0, j_setForeground}, {C, "pointer", 0, j_pointer},
  {C, "bind", 0, j_bind}, {C, "reply", 0, j_reply}, {C, "events", 0, j_events}, {C, "pixels", 0, j_pixels},
};
void webview_init(void) {
  g_pages = [NSMutableDictionary new]; g_lock = [NSLock new]; g_scheme = [GScheme new];
  jni_add_impls(g_wv, sizeof g_wv / sizeof *g_wv);
}
