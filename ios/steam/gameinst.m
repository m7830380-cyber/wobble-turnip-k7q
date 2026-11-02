// Launcher side of "Install GMod": Steam login (password + Steam Guard), install/update/verify progress screen,
// status checks (memory entitlement, game content). Refresh token lives in the Keychain.
#import <UIKit/UIKit.h>
#import <Security/Security.h>
#include <os/proc.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include "sw.h"

// ---------------- keychain ----------------
static NSDictionary *kq(void) { return @{(id)kSecClass: (id)kSecClassGenericPassword, (id)kSecAttrService: @"gmod.steam", (id)kSecAttrAccount: @"refresh"}; }
// fallback when the keychain refuses (some sideload signers drop keychain-access-groups): protected file in Library
static NSString *tokfile(void) { return [NSSearchPathForDirectoriesInDomains(NSLibraryDirectory, NSUserDomainMask, YES).firstObject stringByAppendingPathComponent:@".steam_token"]; }
static NSString *kc_get(void) {
  NSMutableDictionary *q = kq().mutableCopy; q[(id)kSecReturnData] = @YES; q[(id)kSecMatchLimit] = (id)kSecMatchLimitOne;
  CFTypeRef r = NULL;
  if (SecItemCopyMatching((__bridge CFDictionaryRef)q, &r) == errSecSuccess && r) { NSData *d = (__bridge_transfer NSData *)r; return [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding]; }
  NSString *f = [NSString stringWithContentsOfFile:tokfile() encoding:NSUTF8StringEncoding error:nil]; return f.length ? f : nil;
}
static void kc_del(void) { SecItemDelete((__bridge CFDictionaryRef)kq()); [NSFileManager.defaultManager removeItemAtPath:tokfile() error:nil]; }
static void kc_set(NSString *v) {
  kc_del(); NSMutableDictionary *q = kq().mutableCopy; q[(id)kSecValueData] = [v dataUsingEncoding:NSUTF8StringEncoding];
  q[(id)kSecAttrAccessible] = (id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
  OSStatus st = SecItemAdd((__bridge CFDictionaryRef)q, NULL);
  if (st != errSecSuccess) {
    fprintf(stderr, "[steam] keychain refused (%d), using protected file\n", (int)st);
    [[v dataUsingEncoding:NSUTF8StringEncoding] writeToFile:tokfile() options:NSDataWritingFileProtectionComplete error:nil];
  }
}
#define UD [NSUserDefaults standardUserDefaults]
NSString *gi_steam_user(void) { return kc_get() ? [UD stringForKey:@"steam_account"] : nil; }
void gi_logout(void) { kc_del(); [UD removeObjectForKey:@"steam_account"]; }

// ---------------- status checks ----------------
#include <dlfcn.h>
static BOOL ent(NSString *name) {
  void *(*mk)(CFAllocatorRef) = dlsym(RTLD_DEFAULT, "SecTaskCreateFromSelf");
  CFTypeRef (*cp)(void *, CFStringRef, CFErrorRef *) = dlsym(RTLD_DEFAULT, "SecTaskCopyValueForEntitlement");
  if (!mk || !cp) return NO;
  void *t = mk(NULL); if (!t) return NO;
  CFTypeRef v = cp(t, (__bridge CFStringRef)name, NULL); CFRelease(t);
  BOOL on = v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(v); if (v) CFRelease(v); return on;
}
BOOL gi_mem_entitled(void) { return ent(@"com.apple.developer.kernel.increased-memory-limit"); }
// current process memory limit (footprint + what's left), MB
int gi_mem_limit_mb(void) {
  task_vm_info_data_t vi; mach_msg_type_number_t c = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &c)) return 0;
  return (int)((vi.phys_footprint + os_proc_available_memory()) >> 20);
}
// nil = content ok, else what's missing
NSString *gi_content_missing(NSString *docs) {
  NSFileManager *fm = NSFileManager.defaultManager; NSMutableArray *m = [NSMutableArray new];
  if (![fm fileExistsAtPath:[docs stringByAppendingPathComponent:@"garrysmod/garrysmod_dir.vpk"]] && ![fm fileExistsAtPath:[docs stringByAppendingPathComponent:@"garrysmod/gameinfo.txt"]]) [m addObject:@"garrysmod"];
  BOOL dir;
  if (![fm fileExistsAtPath:[docs stringByAppendingPathComponent:@"sourceengine"] isDirectory:&dir] || !dir) [m addObject:@"sourceengine"];
  if (![fm fileExistsAtPath:[docs stringByAppendingPathComponent:@"platform"] isDirectory:&dir] || !dir) [m addObject:@"platform"];
  return m.count ? [m componentsJoinedByString:@", "] : nil;
}

static void say(UIViewController *vc, NSString *t, NSString *m) {
  UIAlertController *a = [UIAlertController alertControllerWithTitle:t message:m preferredStyle:1];
  [a addAction:[UIAlertAction actionWithTitle:@"OK" style:0 handler:nil]]; [vc presentViewController:a animated:YES completion:nil];
}

// ---------------- login ----------------
static volatile int g_lcancel;
static void finish_login(UIViewController *vc, void (^done)(BOOL)) {
  dispatch_async(dispatch_get_global_queue(0, 0), ^{
    char rt[4096] = "", acct[128] = "", err[512] = "";
    int r = sg_auth_poll(rt, sizeof rt, acct, sizeof acct, &g_lcancel, err, sizeof err);
    NSString *E = @(err), *RT = @(rt), *AC = @(acct); memset(rt, 0, sizeof rt);
    dispatch_async(dispatch_get_main_queue(), ^{
      if (vc.presentedViewController) [vc dismissViewControllerAnimated:NO completion:nil];
      if (r) { if (!g_lcancel) say(vc, @"Login failed", E); done(NO); return; }
      kc_set(RT); [UD setObject:AC forKey:@"steam_account"]; done(YES);
    });
  });
}
static void ask_code2(UIViewController *vc, int guard, NSString *hint, void (^done)(BOOL), BOOL poll, NSString *oops);
static void ask_code(UIViewController *vc, int guard, NSString *hint, void (^done)(BOOL)) { ask_code2(vc, guard, hint, done, YES, nil); }
static void ask_code2(UIViewController *vc, int guard, NSString *hint, void (^done)(BOOL), BOOL poll, NSString *oops) {
  BOOL email = !(guard & 1) && (guard & 2);
  NSString *msg = (guard & 3) ? (email ? [NSString stringWithFormat:@"Steam emailed you a code%@, check your inbox (and spam lol)", hint.length ? [NSString stringWithFormat:@" (%@)", hint] : @""]
                                       : @"Open the Steam app → Steam Guard, type the 5 letter code")
                              : @"Open the Steam app and hit approve, I'll wait";
  if ((guard & 4) && (guard & 3)) msg = [msg stringByAppendingString:@"\n(or just hit approve in the Steam app, works too)"];
  if (oops) msg = [NSString stringWithFormat:@"%@\n\n%@", oops, msg];
  UIAlertController *a = [UIAlertController alertControllerWithTitle:@"2FA moment" message:msg preferredStyle:1];
  if (guard & 3) {
    [a addTextFieldWithConfigurationHandler:^(UITextField *f) { f.placeholder = @"ABC12"; f.autocapitalizationType = UITextAutocapitalizationTypeAllCharacters; f.autocorrectionType = UITextAutocorrectionTypeNo; }];
    [a addAction:[UIAlertAction actionWithTitle:@"Send" style:0 handler:^(UIAlertAction *x) {
      NSString *code = a.textFields.firstObject.text ?: @"";
      UIAlertController *w = [UIAlertController alertControllerWithTitle:@"Checking the code…" message:nil preferredStyle:1];
      [w addAction:[UIAlertAction actionWithTitle:@"Cancel" style:1 handler:^(UIAlertAction *y) { g_lcancel = 1; }]];
      [vc presentViewController:w animated:YES completion:nil];
      dispatch_async(dispatch_get_global_queue(0, 0), ^{
        char err[512] = ""; int r = sg_auth_code(code.UTF8String, email, err, sizeof err);
        NSString *E = @(err);
        if (r) dispatch_async(dispatch_get_main_queue(), ^{ // same login stays alive, just ask again
          if (g_lcancel) return;
          [vc dismissViewControllerAnimated:NO completion:^{ ask_code2(vc, guard, hint, done, NO, E); }]; });
      });
    }]];
  }
  [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:1 handler:^(UIAlertAction *x) { g_lcancel = 1; }]];
  [vc presentViewController:a animated:YES completion:nil];
  if (poll) finish_login(vc, done); // polls in the background; picks up the code or the app approval
}
void gi_login(UIViewController *vc, void (^done)(BOOL)) {
  UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Steam login"
    message:@"The account that owns GMod. Password goes straight to Steam encrypted, I don't keep it. 2FA comes next" preferredStyle:1];
  [a addTextFieldWithConfigurationHandler:^(UITextField *f) { f.placeholder = @"Username"; f.text = [UD stringForKey:@"steam_account"]; f.autocapitalizationType = 0; f.autocorrectionType = 1; f.textContentType = UITextContentTypeUsername; }];
  [a addTextFieldWithConfigurationHandler:^(UITextField *f) { f.placeholder = @"Password"; f.secureTextEntry = YES; f.textContentType = UITextContentTypePassword; }];
  [a addAction:[UIAlertAction actionWithTitle:@"Go" style:0 handler:^(UIAlertAction *x) {
    NSString *u = [a.textFields[0].text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet], *p = a.textFields[1].text ?: @"";
    if (!u.length || !p.length) { done(NO); return; }
    g_lcancel = 0;
    UIAlertController *w = [UIAlertController alertControllerWithTitle:@"Talking to Steam…" message:nil preferredStyle:1];
    [vc presentViewController:w animated:YES completion:nil];
    dispatch_async(dispatch_get_global_queue(0, 0), ^{
      int guard = 0; char hint[256] = "", err[512] = "";
      int r = sg_auth_begin(u.UTF8String, p.UTF8String, &guard, hint, sizeof hint, err, sizeof err);
      NSString *E = @(err), *H = @(hint);
      dispatch_async(dispatch_get_main_queue(), ^{
        [vc dismissViewControllerAnimated:NO completion:^{
          if (r) { say(vc, @"Login failed", E); done(NO); return; }
          if (!guard) {
            UIAlertController *w2 = [UIAlertController alertControllerWithTitle:@"Logging in…" message:nil preferredStyle:1];
            [w2 addAction:[UIAlertAction actionWithTitle:@"Cancel" style:1 handler:^(UIAlertAction *y) { g_lcancel = 1; }]];
            [vc presentViewController:w2 animated:YES completion:nil]; finish_login(vc, done);
          } else ask_code(vc, guard, H, done);
        }];
      });
    });
  }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:1 handler:^(UIAlertAction *x) { done(NO); }]];
  [vc presentViewController:a animated:YES completion:nil];
}

// ---------------- install / verify screen ----------------
@interface GIProgress : UIViewController
@property(nonatomic, strong) UILabel *title2, *status, *detail; @property(nonatomic, strong) UIProgressView *bar; @property(nonatomic, strong) UIButton *cancel;
@end
@implementation GIProgress
- (void)viewDidLoad {
  [super viewDidLoad]; self.view.backgroundColor = [UIColor colorWithWhite:0.06 alpha:0.97];
  UIStackView *s = [UIStackView new]; s.axis = UILayoutConstraintAxisVertical; s.spacing = 14; s.translatesAutoresizingMaskIntoConstraints = NO;
  [self.view addSubview:s];
  [NSLayoutConstraint activateConstraints:@[[s.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor], [s.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
    [s.widthAnchor constraintEqualToConstant:MIN(520, self.view.bounds.size.width - 40)]]];
  self.title2 = [UILabel new]; self.title2.font = [UIFont boldSystemFontOfSize:22]; self.title2.textColor = UIColor.whiteColor;
  self.status = [UILabel new]; self.status.textColor = UIColor.whiteColor; self.status.numberOfLines = 0; self.status.font = [UIFont systemFontOfSize:15];
  self.bar = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault]; self.bar.progressTintColor = [UIColor colorWithRed:0.24 green:0.62 blue:1 alpha:1];
  self.detail = [UILabel new]; self.detail.textColor = [UIColor colorWithWhite:0.6 alpha:1]; self.detail.numberOfLines = 0; self.detail.font = [UIFont monospacedDigitSystemFontOfSize:13 weight:0];
  self.detail.text = @"Keep the app open, screen stays on by itself. Leave the app and iOS kills it";
  self.cancel = [UIButton buttonWithType:UIButtonTypeSystem]; [self.cancel setTitle:@"Cancel" forState:0];
  for (UIView *v in @[self.title2, self.status, self.bar, self.detail, self.cancel]) [s addArrangedSubview:v];
}
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
@end

static volatile int g_icancel;
typedef struct { void *vc; double t0, last, lastd; uint64_t d0; } gi_ud;
static void gi_status(void *u, const char *t, uint64_t done, uint64_t total) {
  gi_ud *g = u; double now = CFAbsoluteTimeGetCurrent();
  if (now - g->last < 0.25 && (!total || done < total)) return;
  if (g->lastd > 0 && done < g->d0) { g->t0 = now; g->d0 = done; } // new depot
  if (g->t0 == 0) { g->t0 = now; g->d0 = done; }
  g->last = now; g->lastd = 1;
  double rate = now - g->t0 > 1 ? (done - g->d0) / (now - g->t0) : 0;
  NSString *ts = @(t);
  NSString *det = total ? [NSString stringWithFormat:@"%.2f / %.2f GB   %.1f MB/s%@", done / 1e9, total / 1e9, rate / 1e6,
                           rate > 0 ? [NSString stringWithFormat:@"   ~%d min left", (int)((total - done) / rate / 60) + 1] : @""] : nil;
  float pr = total ? (float)((double)done / total) : 0;
  GIProgress *vc = (__bridge GIProgress *)g->vc;
  dispatch_async(dispatch_get_main_queue(), ^{ vc.status.text = ts; if (det) { vc.detail.text = det; [vc.bar setProgress:pr animated:NO]; } });
}

void gi_sync(UIViewController *parent, NSString *root, BOOL verify, void (^done)(BOOL)) {
  NSString *tok = kc_get(), *acct = [UD stringForKey:@"steam_account"];
  if (!tok || !acct) { gi_login(parent, ^(BOOL ok) { if (ok) gi_sync(parent, root, verify, done); else if (done) done(NO); }); return; }
  GIProgress *vc = [GIProgress new]; vc.modalPresentationStyle = UIModalPresentationOverFullScreen;
  [parent presentViewController:vc animated:YES completion:nil];
  vc.title2.text = verify ? @"Checking your files" : @"Getting GMod"; vc.status.text = @"Warming up…";
  g_icancel = 0;
  [vc.cancel addAction:[UIAction actionWithHandler:^(UIAction *a) { g_icancel = 1; vc.status.text = @"Ok ok stopping…"; }] forControlEvents:UIControlEventTouchUpInside];
  UIApplication.sharedApplication.idleTimerDisabled = YES;
  __block UIBackgroundTaskIdentifier bt = [UIApplication.sharedApplication beginBackgroundTaskWithExpirationHandler:^{ [UIApplication.sharedApplication endBackgroundTask:bt]; bt = UIBackgroundTaskInvalid; }];
  gi_ud *ud = calloc(1, sizeof *ud); ud->vc = (__bridge void *)vc;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    sw_progress p = {gi_status, ud, &g_icancel}; sg_stats st; char err[512] = "";
    int r = sg_game_sync(root.UTF8String, acct.UTF8String, tok.UTF8String, verify, &p, &st, err, sizeof err);
    NSString *E = @(err);
    dispatch_async(dispatch_get_main_queue(), ^{
      free(ud); UIApplication.sharedApplication.idleTimerDisabled = NO;
      if (bt != UIBackgroundTaskInvalid) { [UIApplication.sharedApplication endBackgroundTask:bt]; bt = UIBackgroundTaskInvalid; }
      [parent dismissViewControllerAnimated:YES completion:^{
        if (r == -2) { gi_logout(); say(parent, @"Steam logged you out", @"Log in again o algo"); if (done) done(NO); return; }
        if (r) { if (!g_icancel) say(parent, verify ? @"Check broke" : @"Download broke", E); if (done) done(NO); return; }
        NSString *m = verify ? [NSString stringWithFormat:@"%llu files checked, %llu broken bits fixed (%.1f MB redownloaded)", st.files, st.bad_chunks, st.downloaded / 1e6]
                             : [NSString stringWithFormat:@"%llu files, %.2f GB pulled. Go play", st.files, st.downloaded / 1e9];
        say(parent, verify ? @"All good fr" : @"GMod is in", m); if (done) done(YES);
      }];
    });
  });
}
