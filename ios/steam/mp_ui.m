// Steam friends / invites UI: friends list (join friends in GMod, invite them to your server),
// incoming invite popups (launcher + in game), small in-game friends button, avatar decoding for the engine.
#import <UIKit/UIKit.h>
#include "sw.h"
#include "../../loader/elf_loader.h"

extern NSString *gi_steam_user(void);
extern NSString *gi_steam_token(void);
NSString *lan_my_ip(void);

// ---------- avatars: JPEG -> RGBA size x size ----------
static unsigned char *decode_img(const uint8_t *d, size_t n, int size) {
  @autoreleasepool {
    UIImage *im = [UIImage imageWithData:[NSData dataWithBytes:d length:n]]; if (!im.CGImage) return NULL;
    unsigned char *px = calloc((size_t)size * size, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(px, size, size, 8, size * 4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs); if (!c) { free(px); return NULL; }
    CGContextSetInterpolationQuality(c, kCGInterpolationHigh);
    CGContextDrawImage(c, CGRectMake(0, 0, size, size), im.CGImage); CGContextRelease(c);
    return px;
  }
}
static UIImage *avatar_img(uint64_t sid) {
  uint8_t *b = malloc(64 * 64 * 4); if (!sm_avatar(sid, 64, b, NULL)) { free(b); return nil; }
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGContextRef c = CGBitmapContextCreate(b, 64, 64, 8, 256, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
  CGImageRef ci = CGBitmapContextCreateImage(c); CGContextRelease(c); CGColorSpaceRelease(cs); free(b);
  UIImage *im = [UIImage imageWithCGImage:ci]; CGImageRelease(ci); return im;
}

// ---------- session boot ----------
static int g_ingame;
static void (^g_launcher_join)(NSString *addr);
void mp_boot(void) {
  sm_decode_image = decode_img;
  NSString *u = gi_steam_user(), *t = u ? gi_steam_token() : nil;
  static NSString *last;
  if (u && t) { NSString *k = [u stringByAppendingString:t]; if (![k isEqual:last]) { last = k; sm_start(u.UTF8String, t.UTF8String); } }
  else if (last) { last = nil; sm_stop(); }
}

// ---------- engine console ----------
static void engine_cmd(NSString *cmd) {
  so_module *m = so_find("libengine.so"); if (!m) return;
  uint32_t *f = (uint32_t *)(m->base + 0x29fc58); // Cbuf_AddText (0.1.31): stp x29,x30,[sp,#-48]!
  if (*f != 0xa9bd7bfd) { fprintf(stderr, "[mp] Cbuf_AddText not found (%08x)\n", *f); return; }
  const char *s = [[cmd stringByAppendingString:@"\n"] UTF8String];
  ((void (*)(const char *))f)(s);
  fprintf(stderr, "[mp] console: %s\n", cmd.UTF8String);
}
static void join_addr(NSString *addr) {
  if (!addr.length) return;
  if (g_ingame) engine_cmd([NSString stringWithFormat:@"connect %@", addr]);
  else if (g_launcher_join) g_launcher_join(addr);
}

static UIViewController *top_vc(void) {
  UIWindow *w = nil;
  for (UIScene *s in UIApplication.sharedApplication.connectedScenes) if ([s isKindOfClass:UIWindowScene.class]) for (UIWindow *x in ((UIWindowScene *)s).windows) if (x.isKeyWindow) w = x;
  if (!w) w = UIApplication.sharedApplication.windows.firstObject;
  UIViewController *v = w.rootViewController; while (v.presentedViewController) v = v.presentedViewController; return v;
}
static NSString *my_server(void) { char b[80]; return sm_server(b, sizeof b) ? @(b) : nil; }

// ---------- friends list ----------
@interface MPFriends : UITableViewController
@property(nonatomic, strong) NSArray<NSDictionary *> *rows; @property(nonatomic, strong) NSTimer *tm; @end
@implementation MPFriends
- (void)viewDidLoad {
  [super viewDidLoad]; self.title = @"Steam friends";
  self.tableView.rowHeight = 60;
  self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(done)];
  self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Join IP" style:0 target:self action:@selector(joinIP)];
  [self reload];
  self.tm = [NSTimer scheduledTimerWithTimeInterval:1.5 repeats:YES block:^(NSTimer *t) { [self reload]; }];
}
- (void)viewDidDisappear:(BOOL)a { [super viewDidDisappear:a]; [self.tm invalidate]; }
- (void)done { [self dismissViewControllerAnimated:YES completion:nil]; }
- (void)joinIP {
  UIAlertController *t = [UIAlertController alertControllerWithTitle:@"Join by IP" message:@"PC servers work too (non VAC ones)" preferredStyle:1];
  [t addTextFieldWithConfigurationHandler:^(UITextField *f) { f.placeholder = @"1.2.3.4:27015"; f.keyboardType = UIKeyboardTypeNumbersAndPunctuation; f.text = [NSUserDefaults.standardUserDefaults stringForKey:@"last_ip"]; }];
  [t addAction:[UIAlertAction actionWithTitle:@"Go" style:0 handler:^(UIAlertAction *x) {
    NSString *ip = [t.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if (!ip.length) return; [NSUserDefaults.standardUserDefaults setObject:ip forKey:@"last_ip"];
    [self dismissViewControllerAnimated:YES completion:^{ join_addr(ip); }]; }]];
  [t addAction:[UIAlertAction actionWithTitle:@"Nah" style:1 handler:nil]]; [self presentViewController:t animated:YES completion:nil];
}
- (void)reload {
  char msg[300]; int st = sm_state(msg, sizeof msg);
  sm_friend *f = calloc(2000, sizeof *f); int n = sm_friends(f, 2000);
  NSMutableArray *a = [NSMutableArray new];
  for (int i = 0; i < n; i++) {
    int gm = f[i].app == 4000, on = f[i].state > 0 || f[i].app;
    NSString *s = gm ? (f[i].addr[0] ? [NSString stringWithFormat:@"In GMod · %s", f[i].addr] : @"In GMod (menu / singleplayer)")
                : f[i].app ? [NSString stringWithFormat:@"Playing %s", f[i].game[0] ? f[i].game : "something"]
                : (f[i].state == 1 ? @"Online" : f[i].state == 3 ? @"Away" : f[i].state == 2 ? @"Busy" : f[i].state ? @"Online-ish" : @"Offline");
    [a addObject:@{@"sid": @(f[i].sid), @"name": @(f[i].name), @"sub": s, @"addr": @(f[i].addr), @"rank": @(gm ? 0 : on ? 1 : 2)}];
  }
  free(f);
  [a sortUsingComparator:^NSComparisonResult(NSDictionary *x, NSDictionary *y) {
    NSComparisonResult r = [x[@"rank"] compare:y[@"rank"]]; return r ? r : [x[@"name"] localizedCaseInsensitiveCompare:y[@"name"]]; }];
  self.rows = a;
  NSString *me = my_server();
  self.tableView.tableHeaderView = nil;
  UILabel *h = [UILabel new]; h.numberOfLines = 0; h.font = [UIFont systemFontOfSize:13]; h.textColor = UIColor.secondaryLabelColor;
  h.text = st == 2 ? (me ? [NSString stringWithFormat:@"  Your server: %@ (tap a friend to invite)", me] : @"  Join a server or host one to invite people")
         : st == -1 ? [NSString stringWithFormat:@"  %s", msg] : st == 1 ? @"  Connecting to Steam…" : @"  Not logged in. Launcher → Steam button";
  h.frame = CGRectMake(0, 0, 300, 34); self.tableView.tableHeaderView = h;
  [self.tableView reloadData];
}
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { return self.rows.count; }
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)ip {
  UITableViewCell *c = [t dequeueReusableCellWithIdentifier:@"f"] ?: [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"f"];
  NSDictionary *r = self.rows[ip.row]; uint64_t sid = [r[@"sid"] unsignedLongLongValue];
  c.textLabel.text = r[@"name"]; c.detailTextLabel.text = r[@"sub"];
  int rank = [r[@"rank"] intValue];
  c.detailTextLabel.textColor = rank == 0 ? [UIColor colorWithRed:0.56 green:0.79 blue:0.3 alpha:1] : rank == 1 ? UIColor.systemBlueColor : UIColor.secondaryLabelColor;
  c.imageView.image = avatar_img(sid) ?: [UIImage systemImageNamed:@"person.crop.square"];
  c.imageView.layer.cornerRadius = 6; c.imageView.clipsToBounds = YES;
  c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  return c;
}
- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)ip {
  [t deselectRowAtIndexPath:ip animated:YES];
  NSDictionary *r = self.rows[ip.row]; uint64_t sid = [r[@"sid"] unsignedLongLongValue]; NSString *addr = r[@"addr"], *me = my_server();
  UIAlertController *a = [UIAlertController alertControllerWithTitle:r[@"name"] message:r[@"sub"] preferredStyle:UIAlertControllerStyleActionSheet];
  if (addr.length) [a addAction:[UIAlertAction actionWithTitle:@"Join their server" style:0 handler:^(UIAlertAction *x) {
    [self dismissViewControllerAnimated:YES completion:^{ join_addr(addr); }]; }]];
  if (me) [a addAction:[UIAlertAction actionWithTitle:@"Invite to my server" style:0 handler:^(UIAlertAction *x) {
    int ok = sm_invite(sid, [NSString stringWithFormat:@"+connect %@", me].UTF8String) == 0;
    UIAlertController *d = [UIAlertController alertControllerWithTitle:ok ? @"Invite sent" : @"Invite failed" message:ok ? @"They get it in Steam chat" : @"Steam isn't connected rn" preferredStyle:1];
    [d addAction:[UIAlertAction actionWithTitle:@"OK" style:0 handler:nil]]; [self presentViewController:d animated:YES completion:nil]; }]];
  if (!addr.length && !me) a.message = [r[@"sub"] stringByAppendingString:@"\n\nNothing to join, and you're not on a server yet"];
  [a addAction:[UIAlertAction actionWithTitle:@"Back" style:1 handler:nil]];
  a.popoverPresentationController.sourceView = [t cellForRowAtIndexPath:ip];
  [self presentViewController:a animated:YES completion:nil];
}
@end

void mp_friends_present(UIViewController *from) {
  mp_boot();
  if (!gi_steam_user()) {
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Steam login needed" message:@"Hit the Steam button in the launcher first, then friends show up here" preferredStyle:1];
    [a addAction:[UIAlertAction actionWithTitle:@"OK" style:0 handler:nil]]; [from presentViewController:a animated:YES completion:nil]; return;
  }
  UINavigationController *n = [[UINavigationController alloc] initWithRootViewController:[[MPFriends alloc] initWithStyle:UITableViewStyleInsetGrouped]];
  n.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  [from presentViewController:n animated:YES completion:nil];
}

// ---------- incoming invites ----------
static BOOL g_inv_showing;
static void poll_invites(void) {
  if (g_inv_showing) return;
  uint64_t from; char name[64], addr[80], raw[200];
  if (!sm_take_invite(&from, name, sizeof name, addr, sizeof addr, raw, sizeof raw)) return;
  UIViewController *v = top_vc(); if (!v) return;
  NSString *ad = @(addr);
  UIAlertController *a = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"%s invited you", name]
    message:ad.length ? [NSString stringWithFormat:@"GMod server %@", ad] : [NSString stringWithFormat:@"Can't join this one from here (%s)", raw] preferredStyle:1];
  if (ad.length) [a addAction:[UIAlertAction actionWithTitle:@"Join" style:0 handler:^(UIAlertAction *x) { g_inv_showing = NO; join_addr(ad); }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Nah" style:1 handler:^(UIAlertAction *x) { g_inv_showing = NO; }]];
  g_inv_showing = YES;
  [v presentViewController:a animated:YES completion:nil];
}
static void start_invite_timer(void) {
  static NSTimer *t; if (t) return;
  t = [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *x) { poll_invites(); }];
  [NSRunLoop.mainRunLoop addTimer:t forMode:NSRunLoopCommonModes];
}
void mp_launcher_attach(void (^join)(NSString *addr)) { g_launcher_join = join; mp_boot(); start_invite_timer(); }

// ---------- in game: touch layout button "friends" runs gmod_ios_friends ----------
static void cmd_hook(const char *c) {
  if (strcmp(c, "gmod_ios_friends")) return;
  static double last; double now = CFAbsoluteTimeGetCurrent(); if (now - last < 0.6) return; last = now;
  dispatch_async(dispatch_get_main_queue(), ^{ UIViewController *v = top_vc(); if (v && ![v isKindOfClass:UINavigationController.class]) { extern void SDL_StopTextInput(void); SDL_StopTextInput(); mp_friends_present(v); } });
}
// called right before the engine starts
void mp_game_start(int host_port) {
  extern void (*bionic_cmd_hook)(const char *); bionic_cmd_hook = cmd_hook;
  g_ingame = 1; mp_boot();
  if (!gi_steam_user()) return;
  sm_set_playing(1);
  if (host_port > 0) sm_set_host(host_port);
  dispatch_async(dispatch_get_main_queue(), ^{ start_invite_timer(); });
}
