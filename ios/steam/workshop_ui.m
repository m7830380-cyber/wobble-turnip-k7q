// Workshop manager + in-app Steam Workshop browser (port of GDroid ModManager/WorkshopBrowser essentials).
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#include "sw.h"

static NSString *g_root;
const char *ios_content_root(void) { return g_root ? g_root.fileSystemRepresentation : ""; }
void workshop_set_root(const char *r) { g_root = @(r); }

@interface GWorkshop : UIViewController <WKNavigationDelegate, UITableViewDataSource, UITableViewDelegate>
@property(nonatomic, strong) UITableView *table;
@property(nonatomic, strong) UILabel *status;
@property(nonatomic, strong) UIProgressView *bar;
@property(nonatomic, strong) NSMutableArray<NSDictionary *> *items;
@property(nonatomic, strong) WKWebView *web;
@property(nonatomic, strong) UIButton *dlBtn;
@property(nonatomic) BOOL busy;
@end

static volatile int g_cancel;

@implementation GWorkshop
- (NSString *)addons { return [g_root stringByAppendingPathComponent:@"garrysmod/addons"]; }
- (NSString *)disabled { return [g_root stringByAppendingPathComponent:@"garrysmod/addons_disabled"]; }

- (void)reload {
  NSMutableArray *a = [NSMutableArray new]; NSFileManager *fm = NSFileManager.defaultManager;
  for (NSString *base in @[[self addons], [self disabled]]) {
    BOOL en = base == [self addons] || [base isEqual:[self addons]];
    for (NSString *f in [fm contentsOfDirectoryAtPath:base error:nil]) {
      if ([f hasPrefix:@"."]) continue;
      NSString *p = [base stringByAppendingPathComponent:f]; NSDictionary *j = nil;
      NSData *d = [NSData dataWithContentsOfFile:[p stringByAppendingPathComponent:@"workshop.json"]];
      if (d) j = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
      BOOL dir = NO; [fm fileExistsAtPath:p isDirectory:&dir];
      if (!j && !(dir || [f.pathExtension.lowercaseString isEqual:@"gma"])) continue;
      NSString *title = [j[@"title"] description] ?: f;
      long long size = [j[@"size"] longLongValue];
      [a addObject:@{@"path": p, @"name": f, @"title": title, @"enabled": @(en), @"size": @(size), @"ws": @(j != nil)}];
    }
  }
  [a sortUsingComparator:^NSComparisonResult(NSDictionary *x, NSDictionary *y) { return [x[@"title"] localizedCaseInsensitiveCompare:y[@"title"]]; }];
  self.items = a; [self.table reloadData];
}

- (UIButton *)btn:(NSString *)t sel:(SEL)s {
  UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem]; [b setTitle:t forState:0];
  b.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold]; b.backgroundColor = [UIColor colorWithWhite:0.17 alpha:1];
  b.layer.cornerRadius = 10; b.tintColor = [UIColor colorWithRed:0.24 green:0.62 blue:1 alpha:1];
  [b addTarget:self action:s forControlEvents:UIControlEventTouchUpInside]; [b.heightAnchor constraintEqualToConstant:40].active = YES; return b;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor colorWithWhite:0.06 alpha:1];
  UILabel *h = [UILabel new]; h.text = @"Workshop"; h.font = [UIFont boldSystemFontOfSize:24]; h.textColor = UIColor.whiteColor;
  UIStackView *top = [[UIStackView alloc] initWithArrangedSubviews:@[h, [self btn:@"Browse" sel:@selector(browse)], [self btn:@"Add by ID / link" sel:@selector(addId)], [self btn:@"Presets" sel:@selector(presets)], [self btn:@"Done" sel:@selector(close)]]];
  top.spacing = 10; top.distribution = UIStackViewDistributionFill; [h setContentHuggingPriority:1 forAxis:0];
  for (UIView *v in top.arrangedSubviews) if ([v isKindOfClass:UIButton.class]) [v.widthAnchor constraintGreaterThanOrEqualToConstant:90].active = YES;
  self.status = [UILabel new]; self.status.textColor = [UIColor colorWithWhite:0.7 alpha:1]; self.status.font = [UIFont systemFontOfSize:13]; self.status.numberOfLines = 2;
  self.status.text = @"Addons here lol.";
  self.bar = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault]; self.bar.hidden = YES;
  self.table = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain];
  self.table.backgroundColor = UIColor.clearColor; self.table.dataSource = self; self.table.delegate = self; self.table.separatorColor = [UIColor colorWithWhite:0.2 alpha:1];
  UIStackView *s = [[UIStackView alloc] initWithArrangedSubviews:@[top, self.status, self.bar, self.table]];
  s.axis = UILayoutConstraintAxisVertical; s.spacing = 10; s.translatesAutoresizingMaskIntoConstraints = NO; [self.view addSubview:s];
  UILayoutGuide *g = self.view.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[[s.topAnchor constraintEqualToAnchor:g.topAnchor constant:12], [s.bottomAnchor constraintEqualToAnchor:g.bottomAnchor],
    [s.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:16], [s.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-16]]];
  [self reload];
}
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (void)close {
  if (self.busy) { g_cancel = 1; self.status.text = @"Cancelling…"; return; }
  [self dismissViewControllerAnimated:YES completion:nil];
}

// ---- table ----
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { return self.items.count ?: 1; }
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)ip {
  UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
  c.backgroundColor = UIColor.clearColor; c.textLabel.textColor = UIColor.whiteColor; c.detailTextLabel.textColor = [UIColor colorWithWhite:0.55 alpha:1];
  if (!self.items.count) { c.textLabel.text = @"No addons yet"; c.detailTextLabel.text = @""; c.selectionStyle = 0; return c; }
  NSDictionary *it = self.items[ip.row];
  c.textLabel.text = it[@"title"];
  long long sz = [it[@"size"] longLongValue];
  c.detailTextLabel.text = [NSString stringWithFormat:@"%@%@%@", it[@"name"], sz ? [NSString stringWithFormat:@" · %.1f MB", sz / 1048576.0] : @"", [it[@"enabled"] boolValue] ? @"" : @" · disabled"];
  c.textLabel.alpha = [it[@"enabled"] boolValue] ? 1 : 0.45;
  UISwitch *sw = [UISwitch new]; sw.on = [it[@"enabled"] boolValue]; sw.tag = ip.row; [sw addTarget:self action:@selector(toggle:) forControlEvents:UIControlEventValueChanged];
  c.accessoryView = sw; return c;
}
- (void)toggle:(UISwitch *)s {
  NSDictionary *it = self.items[s.tag]; NSString *dst = [(s.on ? [self addons] : [self disabled]) stringByAppendingPathComponent:it[@"name"]];
  [NSFileManager.defaultManager createDirectoryAtPath:dst.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
  NSError *e; if (![NSFileManager.defaultManager moveItemAtPath:it[@"path"] toPath:dst error:&e]) self.status.text = e.localizedDescription;
  [self reload];
}
- (UISwipeActionsConfiguration *)tableView:(UITableView *)t trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)ip {
  if (!self.items.count) return nil; NSDictionary *it = self.items[ip.row];
  UIContextualAction *del = [UIContextualAction contextualActionWithStyle:UIContextualActionStyleDestructive title:@"Delete" handler:^(UIContextualAction *a, UIView *v, void (^done)(BOOL)) {
    [NSFileManager.defaultManager removeItemAtPath:it[@"path"] error:nil]; [self reload]; done(YES); }];
  NSMutableArray *acts = [NSMutableArray arrayWithObject:del];
  if ([it[@"ws"] boolValue] && [it[@"name"] longLongValue] > 0) {
    UIContextualAction *up = [UIContextualAction contextualActionWithStyle:UIContextualActionStyleNormal title:@"Update" handler:^(UIContextualAction *a, UIView *v, void (^done)(BOOL)) {
      [self download:it[@"name"]]; done(YES); }];
    up.backgroundColor = [UIColor colorWithRed:0.24 green:0.62 blue:1 alpha:1]; [acts addObject:up];
  }
  return [UISwipeActionsConfiguration configurationWithActions:acts];
}

// ---- presets: named sets of enabled addons ----
- (NSString *)presetFile { return [g_root stringByAppendingPathComponent:@"garrysmod/ios_addon_presets.json"]; }
- (NSMutableDictionary *)loadPresets {
  NSData *d = [NSData dataWithContentsOfFile:[self presetFile]]; id j = d ? [NSJSONSerialization JSONObjectWithData:d options:NSJSONReadingMutableContainers error:nil] : nil;
  return [j isKindOfClass:NSDictionary.class] ? j : [NSMutableDictionary new];
}
- (void)savePresets:(NSDictionary *)p {
  NSData *d = [NSJSONSerialization dataWithJSONObject:p options:NSJSONWritingPrettyPrinted error:nil]; [d writeToFile:[self presetFile] atomically:YES];
}
- (void)presets {
  if (self.busy) return;
  NSMutableDictionary *ps = [self loadPresets];
  UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Addon presets" message:ps.count ? @"Tap one to load it: its addons get turned on, everything else off." : @"No presets yet. Save your current addon setup as one." preferredStyle:1];
  for (NSString *k in [ps.allKeys sortedArrayUsingSelector:@selector(localizedCaseInsensitiveCompare:)])
    [a addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"%@  (%lu)", k, (unsigned long)[ps[k] count]] style:0 handler:^(UIAlertAction *x) { [self presetMenu:k]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Save current setup as preset…" style:0 handler:^(UIAlertAction *x) { [self presetSave:nil]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:1 handler:nil]];
  [self presentViewController:a animated:YES completion:nil];
}
- (void)presetSave:(NSString *)over {
  NSMutableArray *on = [NSMutableArray new]; for (NSDictionary *it in self.items) if ([it[@"enabled"] boolValue]) [on addObject:it[@"name"]];
  void (^store)(NSString *) = ^(NSString *name) {
    name = [name stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]; if (!name.length) return;
    NSMutableDictionary *ps = [self loadPresets]; ps[name] = on; [self savePresets:ps];
    self.status.text = [NSString stringWithFormat:@"Saved preset \"%@\" (%lu addons on).", name, (unsigned long)on.count]; };
  if (over) { store(over); return; }
  UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Save preset" message:[NSString stringWithFormat:@"%lu addons are on right now. Name this setup:", (unsigned long)on.count] preferredStyle:1];
  [a addTextFieldWithConfigurationHandler:^(UITextField *f) { f.placeholder = @"e.g. horror maps, building, no lag"; }];
  [a addAction:[UIAlertAction actionWithTitle:@"Save" style:0 handler:^(UIAlertAction *x) { store(a.textFields.firstObject.text); }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:1 handler:nil]];
  [self presentViewController:a animated:YES completion:nil];
}
- (void)presetMenu:(NSString *)k {
  UIAlertController *a = [UIAlertController alertControllerWithTitle:k message:nil preferredStyle:1];
  [a addAction:[UIAlertAction actionWithTitle:@"Load it" style:0 handler:^(UIAlertAction *x) { [self presetApply:k]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Overwrite with current setup" style:0 handler:^(UIAlertAction *x) { [self presetSave:k]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Delete preset" style:2 handler:^(UIAlertAction *x) {
    NSMutableDictionary *ps = [self loadPresets]; [ps removeObjectForKey:k]; [self savePresets:ps]; self.status.text = [NSString stringWithFormat:@"Preset \"%@\" yeeted.", k]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:1 handler:nil]];
  [self presentViewController:a animated:YES completion:nil];
}
- (void)presetApply:(NSString *)k {
  NSArray *want = [self loadPresets][k]; if (![want isKindOfClass:NSArray.class]) return;
  NSSet *ws = [NSSet setWithArray:want]; NSMutableSet *have = [NSMutableSet new]; NSFileManager *fm = NSFileManager.defaultManager;
  [fm createDirectoryAtPath:[self disabled] withIntermediateDirectories:YES attributes:nil error:nil];
  [fm createDirectoryAtPath:[self addons] withIntermediateDirectories:YES attributes:nil error:nil];
  int moved = 0;
  for (NSDictionary *it in self.items) {
    BOOL on = [ws containsObject:it[@"name"]]; if (on) [have addObject:it[@"name"]];
    if (on == [it[@"enabled"] boolValue]) continue;
    NSString *dst = [(on ? [self addons] : [self disabled]) stringByAppendingPathComponent:it[@"name"]];
    if ([fm moveItemAtPath:it[@"path"] toPath:dst error:nil]) moved++;
  }
  [self reload];
  NSMutableArray *miss = [NSMutableArray new]; for (NSString *n in want) if (![have containsObject:n] && n.longLongValue > 0) [miss addObject:n];
  self.status.text = [NSString stringWithFormat:@"Loaded \"%@\" (%d switched).", k, moved];
  if (miss.count) {
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Some addons are missing" message:[NSString stringWithFormat:@"%lu addon(s) from this preset aren't installed anymore. Download them?", (unsigned long)miss.count] preferredStyle:1];
    [a addAction:[UIAlertAction actionWithTitle:@"Yeah, grab them" style:0 handler:^(UIAlertAction *x) { [self download:[miss componentsJoinedByString:@","]]; }]];
    [a addAction:[UIAlertAction actionWithTitle:@"Nah" style:1 handler:nil]];
    [self presentViewController:a animated:YES completion:nil];
  }
}

// ---- download ----
static void ui_status(void *ud, const char *t, uint64_t d, uint64_t tot) {
  GWorkshop *w = (__bridge GWorkshop *)ud; NSString *s = @(t);
  dispatch_async(dispatch_get_main_queue(), ^{
    w.status.text = tot ? [NSString stringWithFormat:@"%@  (%.1f / %.1f MB)", s, d / 1048576.0, tot / 1048576.0] : s;
    w.bar.hidden = !tot; if (tot) w.bar.progress = (float)d / tot; });
}
- (void)addId {
  UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Add Workshop items" message:@"Paste Workshop IDs, item links or collection links (comma separated)." preferredStyle:1];
  [a addTextFieldWithConfigurationHandler:^(UITextField *f) { f.placeholder = @"https://steamcommunity.com/sharedfiles/filedetails/?id=…"; f.text = UIPasteboard.generalPasteboard.hasStrings && [UIPasteboard.generalPasteboard.string containsString:@"id="] ? UIPasteboard.generalPasteboard.string : @""; }];
  [a addAction:[UIAlertAction actionWithTitle:@"Download" style:0 handler:^(UIAlertAction *x) { [self download:a.textFields.firstObject.text]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:1 handler:nil]]; [self presentViewController:a animated:YES completion:nil];
}
- (void)download:(NSString *)spec {
  if (self.busy || !spec.length) return; self.busy = YES; g_cancel = 0; self.dlBtn.enabled = NO;
  self.status.text = @"Connecting to Steam…"; self.bar.hidden = YES;
  NSString *root = g_root; const char *cs = strdup(spec.UTF8String);
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    sw_progress p = {ui_status, (__bridge void *)self, &g_cancel}; char err[512] = ""; uint64_t *ids = NULL; int n = 0, ok = 0, bad = 0;
    NSMutableString *fails = [NSMutableString new];
    if (sw_resolve(cs, &ids, &n, &p, err, sizeof err)) [fails appendFormat:@"%s", err];
    if (n > 0 && !g_cancel) {  // "Required items" on the Workshop page: offer to grab the ones not installed yet
      uint64_t *dp = NULL; char (*dt)[128] = NULL; int nd = 0; char e3[512] = "";
      if (sw_deps(root.fileSystemRepresentation, ids, n, &dp, &dt, &nd, &p, e3, sizeof e3)) sw_log("[workshop] dependency check failed: %s", e3);
      if (nd > 0) {
        NSMutableString *lst = [NSMutableString new];
        for (int i = 0; i < nd; i++) [lst appendFormat:@"%@• %s", i ? @"\n" : @"", dt[i]];
        dispatch_semaphore_t sem = dispatch_semaphore_create(0); __block int yes = 1;
        dispatch_async(dispatch_get_main_queue(), ^{
          UIAlertController *a = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"This addon needs %d other addon%s", nd, nd == 1 ? "" : "s"]
            message:[NSString stringWithFormat:@"Required items you don't have yet:\n\n%@\n\nGrab them too?", lst] preferredStyle:1];
          [a addAction:[UIAlertAction actionWithTitle:@"Yeah, download all" style:0 handler:^(UIAlertAction *x) { yes = 1; dispatch_semaphore_signal(sem); }]];
          [a addAction:[UIAlertAction actionWithTitle:@"Nah, just this one" style:1 handler:^(UIAlertAction *x) { yes = 0; dispatch_semaphore_signal(sem); }]];
          [self.presentedViewController ?: self presentViewController:a animated:YES completion:nil]; });
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        if (yes) { ids = realloc(ids, 8 * (n + nd)); memcpy(ids + n, dp, 8 * nd); n += nd; }
        sw_log("[workshop] %d required addon(s) %s", nd, yes ? "added" : "skipped");
      }
      free(dp); free(dt);
    }
    for (int i = 0; i < n && !g_cancel; i++) {
      char e2[512] = ""; ui_status((__bridge void *)self, [NSString stringWithFormat:@"Item %d/%d…", i + 1, n].UTF8String, 0, 0);
      if (sw_install(root.fileSystemRepresentation, ids[i], 0, &p, e2, sizeof e2)) { bad++; [fails appendFormat:@"%s%llu: %s", fails.length ? "\n" : "", (unsigned long long)ids[i], e2]; }
      else ok++;
    }
    free(ids); free((void *)cs);
    dispatch_async(dispatch_get_main_queue(), ^{
      self.busy = NO; self.bar.hidden = YES; self.dlBtn.enabled = YES;
      self.status.text = g_cancel ? @"Cancelled." : [NSString stringWithFormat:@"Installed %d item%s%@", ok, ok == 1 ? "" : "s", bad || (!ok && fails.length) ? [NSString stringWithFormat:@", %d failed", bad] : @"."];
      [self reload];
      if (fails.length && !g_cancel) { UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Some items failed" message:fails preferredStyle:1];
        [a addAction:[UIAlertAction actionWithTitle:@"OK" style:0 handler:nil]]; [self.presentedViewController ?: self presentViewController:a animated:YES completion:nil]; }
    });
  });
}

// ---- browser ----
- (void)browse {
  UIViewController *v = [UIViewController new]; v.view.backgroundColor = UIColor.blackColor; v.modalPresentationStyle = UIModalPresentationFullScreen;
  WKWebViewConfiguration *cfg = [WKWebViewConfiguration new];
  self.web = [[WKWebView alloc] initWithFrame:CGRectZero configuration:cfg]; self.web.navigationDelegate = self; self.web.allowsBackForwardNavigationGestures = YES;
  self.web.customUserAgent = @"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15";
  UIButton *back = [self btn:@"‹ Back" sel:@selector(webBack)], *close = [self btn:@"Close" sel:@selector(webClose)];
  self.dlBtn = [self btn:@"Download this item" sel:@selector(webDownload)]; self.dlBtn.enabled = NO;
  self.dlBtn.backgroundColor = [UIColor colorWithRed:0.24 green:0.62 blue:1 alpha:1]; self.dlBtn.tintColor = UIColor.whiteColor;
  UILabel *st = [UILabel new]; st.textColor = [UIColor colorWithWhite:0.75 alpha:1]; st.font = [UIFont systemFontOfSize:12]; st.numberOfLines = 1;
  UIStackView *bar = [[UIStackView alloc] initWithArrangedSubviews:@[back, self.dlBtn, close]]; bar.spacing = 8; bar.distribution = UIStackViewDistributionFill;
  [back.widthAnchor constraintEqualToConstant:80].active = YES; [close.widthAnchor constraintEqualToConstant:80].active = YES;
  UIStackView *s = [[UIStackView alloc] initWithArrangedSubviews:@[bar, self.web]]; s.axis = UILayoutConstraintAxisVertical; s.spacing = 6; s.translatesAutoresizingMaskIntoConstraints = NO;
  [v.view addSubview:s]; UILayoutGuide *g = v.view.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[[s.topAnchor constraintEqualToAnchor:g.topAnchor constant:6], [s.bottomAnchor constraintEqualToAnchor:v.view.bottomAnchor],
    [s.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:8], [s.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-8]]];
  [self.web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:@"https://steamcommunity.com/workshop/browse/?appid=4000&browsesort=trend&section=readytouseitems"]]];
  [self presentViewController:v animated:YES completion:nil];
}
- (NSString *)webItem {
  NSURLComponents *c = [NSURLComponents componentsWithURL:self.web.URL resolvingAgainstBaseURL:NO];
  if (![c.path containsString:@"/sharedfiles/filedetails"] && ![c.path containsString:@"/workshop/filedetails"]) return nil;
  for (NSURLQueryItem *q in c.queryItems) if ([q.name isEqual:@"id"] && q.value.longLongValue > 0) return q.value;
  return nil;
}
- (void)webView:(WKWebView *)w didFinishNavigation:(WKNavigation *)n { [self webRefresh]; }
- (void)webView:(WKWebView *)w didCommitNavigation:(WKNavigation *)n { [self webRefresh]; }
- (void)webRefresh {
  NSString *id_ = [self webItem]; self.dlBtn.enabled = id_ != nil && !self.busy;
  [self.dlBtn setTitle:id_ ? (self.busy ? @"Downloading…" : @"Download this item / collection") : @"Open an item to download it" forState:0];
}
- (void)webBack { if (self.web.canGoBack) [self.web goBack]; }
- (void)webClose { [self dismissViewControllerAnimated:YES completion:nil]; self.web = nil; self.dlBtn = nil; }
- (void)webDownload {
  NSString *id_ = [self webItem]; if (!id_) return;
  [self.dlBtn setTitle:@"Downloading… (see Workshop screen)" forState:0];
  [self download:id_];
}
@end

void workshop_present(UIViewController *from) {
  GWorkshop *w = [GWorkshop new]; w.modalPresentationStyle = UIModalPresentationFullScreen;
  [from presentViewController:w animated:YES completion:nil];
}
