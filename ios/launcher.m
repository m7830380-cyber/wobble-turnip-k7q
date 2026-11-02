// Native pre-game launcher (port of GDroid's LauncherActivity essentials).
#import <UIKit/UIKit.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static volatile int g_done = 0;
static NSMutableArray<NSString *> *g_args;

#define D [NSUserDefaults standardUserDefaults]

@interface GLauncher : UIViewController <UITextFieldDelegate>
@property(nonatomic, strong) NSString *docs;
@property(nonatomic, strong) UIStackView *stack;
@property(nonatomic, strong) NSArray<NSString *> *maps;
@property(nonatomic, strong) UIButton *mapBtn, *startBtn;
@property(nonatomic, strong) UITextField *argsField, *nameField;
@property(nonatomic, strong) NSMutableDictionary *labels;
@end

@implementation GLauncher

- (UIColor *)accent { return [UIColor colorWithRed:0.24 green:0.62 blue:1 alpha:1]; }

- (UILabel *)label:(NSString *)t size:(CGFloat)s bold:(BOOL)b {
  UILabel *l = [UILabel new]; l.text = t; l.textColor = UIColor.whiteColor; l.numberOfLines = 0;
  l.font = b ? [UIFont boldSystemFontOfSize:s] : [UIFont systemFontOfSize:s]; return l;
}

- (UIView *)card:(NSString *)title {
  UIStackView *c = [UIStackView new]; c.axis = UILayoutConstraintAxisVertical; c.spacing = 10;
  c.layoutMargins = UIEdgeInsetsMake(14, 16, 14, 16); c.layoutMarginsRelativeArrangement = YES;
  c.backgroundColor = [UIColor colorWithWhite:0.13 alpha:1]; c.layer.cornerRadius = 14;
  UILabel *h = [self label:title size:13 bold:YES]; h.textColor = [UIColor colorWithWhite:0.6 alpha:1];
  [c addArrangedSubview:h]; [self.stack addArrangedSubview:c]; return c;
}

- (void)row:(UIStackView *)card text:(NSString *)t control:(UIView *)v {
  UIStackView *r = [UIStackView new]; r.axis = UILayoutConstraintAxisHorizontal; r.spacing = 10; r.alignment = UIStackViewAlignmentCenter;
  UILabel *l = [self label:t size:16 bold:NO]; [l setContentHuggingPriority:1 forAxis:UILayoutConstraintAxisHorizontal];
  [r addArrangedSubview:l]; [r addArrangedSubview:v]; [card addArrangedSubview:r];
}

- (void)toggle:(UIStackView *)card text:(NSString *)t key:(NSString *)k def:(BOOL)def {
  UISwitch *s = [UISwitch new]; s.onTintColor = self.accent;
  s.on = [D objectForKey:k] ? [D boolForKey:k] : def;
  s.accessibilityIdentifier = k;
  [s addTarget:self action:@selector(sw:) forControlEvents:UIControlEventValueChanged];
  [self row:card text:t control:s];
}
- (void)sw:(UISwitch *)s { [D setBool:s.on forKey:s.accessibilityIdentifier]; }

- (void)slider:(UIStackView *)card text:(NSString *)t key:(NSString *)k min:(float)mn max:(float)mx def:(float)def step:(float)st fmt:(NSString *)fmt {
  float v = [D objectForKey:k] ? [D floatForKey:k] : def;
  UILabel *l = [self label:[NSString stringWithFormat:fmt, t, v] size:16 bold:NO];
  self.labels[k] = @[l, t, fmt, @(st)];
  UISlider *s = [UISlider new]; s.minimumValue = mn; s.maximumValue = mx; s.value = v; s.tintColor = self.accent;
  s.accessibilityIdentifier = k;
  [s addTarget:self action:@selector(sl:) forControlEvents:UIControlEventValueChanged];
  [card addArrangedSubview:l]; [card addArrangedSubview:s];
}
- (void)sl:(UISlider *)s {
  NSArray *a = self.labels[s.accessibilityIdentifier]; float st = [a[3] floatValue];
  float v = roundf(s.value / st) * st;
  [D setFloat:v forKey:s.accessibilityIdentifier];
  ((UILabel *)a[0]).text = [NSString stringWithFormat:a[2], a[1], v];
}

- (UIButton *)button:(NSString *)t action:(SEL)a {
  UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem]; [b setTitle:t forState:0];
  b.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold]; b.tintColor = self.accent;
  [b addTarget:self action:a forControlEvents:UIControlEventTouchUpInside]; return b;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.labels = [NSMutableDictionary new];
  self.view.backgroundColor = [UIColor colorWithWhite:0.06 alpha:1];
  UIScrollView *sv = [UIScrollView new]; sv.translatesAutoresizingMaskIntoConstraints = NO;
  sv.keyboardDismissMode = UIScrollViewKeyboardDismissModeOnDrag;
  [self.view addSubview:sv];
  self.stack = [UIStackView new]; self.stack.axis = UILayoutConstraintAxisVertical; self.stack.spacing = 14;
  self.stack.translatesAutoresizingMaskIntoConstraints = NO; [sv addSubview:self.stack];
  UILayoutGuide *g = self.view.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [sv.topAnchor constraintEqualToAnchor:g.topAnchor], [sv.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    [sv.leadingAnchor constraintEqualToAnchor:g.leadingAnchor], [sv.trailingAnchor constraintEqualToAnchor:g.trailingAnchor],
    [self.stack.topAnchor constraintEqualToAnchor:sv.contentLayoutGuide.topAnchor constant:16],
    [self.stack.bottomAnchor constraintEqualToAnchor:sv.contentLayoutGuide.bottomAnchor constant:-30],
    [self.stack.centerXAnchor constraintEqualToAnchor:sv.centerXAnchor],
    [self.stack.widthAnchor constraintEqualToConstant:MIN(560, MIN(self.view.bounds.size.width, self.view.bounds.size.height) * 1.6)],
  ]];
  NSLayoutConstraint *w = [self.stack.widthAnchor constraintEqualToAnchor:sv.widthAnchor constant:-32]; w.priority = 999;
  w.active = YES; [self.stack.widthAnchor constraintLessThanOrEqualToConstant:620].active = YES;

  UILabel *title = [self label:@"Garry's Mod" size:30 bold:YES]; [self.stack addArrangedSubview:title];
  UILabel *sub = [self label:[NSString stringWithFormat:@"GDroid iOS launcher · build %@ (%@)", NSBundle.mainBundle.infoDictionary[@"CFBundleVersion"], NSBundle.mainBundle.infoDictionary[@"GMBuildSHA"] ?: @"?"] size:14 bold:NO]; sub.textColor = [UIColor colorWithWhite:0.55 alpha:1];
  [self.stack addArrangedSubview:sub];

  UIButton *play = [UIButton buttonWithType:UIButtonTypeSystem];
  [play setTitle:@"PLAY" forState:0]; play.titleLabel.font = [UIFont systemFontOfSize:22 weight:UIFontWeightBold];
  play.backgroundColor = self.accent; play.tintColor = UIColor.whiteColor; play.layer.cornerRadius = 14;
  [play.heightAnchor constraintEqualToConstant:58].active = YES;
  [play addTarget:self action:@selector(play) forControlEvents:UIControlEventTouchUpInside];
  [self.stack addArrangedSubview:play];
  UIStackView *mp = [UIStackView new]; mp.axis = UILayoutConstraintAxisHorizontal; mp.distribution = UIStackViewDistributionFillEqually; mp.spacing = 10;
  for (NSArray *b in @[@[@"Host game", @"host"], @[@"LAN games", @"connect"], @[@"Load a save", @"load"]]) {
    UIButton *x = [self button:b[0] action:@selector(mpTap:)]; x.accessibilityIdentifier = b[1];
    x.backgroundColor = [UIColor colorWithWhite:0.16 alpha:1]; x.layer.cornerRadius = 12; [x.heightAnchor constraintEqualToConstant:46].active = YES;
    [mp addArrangedSubview:x];
  }
  [self.stack addArrangedSubview:mp];

  UIButton *ws = [self button:@"Steam Workshop (browser based for now)" action:@selector(openWorkshop)];
  ws.backgroundColor = [UIColor colorWithWhite:0.16 alpha:1]; ws.layer.cornerRadius = 12; [ws.heightAnchor constraintEqualToConstant:46].active = YES;
  [self.stack addArrangedSubview:ws];

  // Start
  UIStackView *c = (UIStackView *)[self card:@"START"];
  self.startBtn = [self button:@"" action:@selector(pickStart)];
  [self row:c text:@"Start at" control:self.startBtn];
  self.mapBtn = [self button:@"" action:@selector(pickMap)];
  [self row:c text:@"Map" control:self.mapBtn];
  [self refreshStart];

  // Player
  c = (UIStackView *)[self card:@"PLAYER"];
  self.nameField = [self field:[D stringForKey:@"player_name"] ?: @"" placeholder:@"Player name"];
  [c addArrangedSubview:self.nameField];

  // Graphics
  c = (UIStackView *)[self card:@"GRAPHICS"];
  [self slider:c text:@"Framerate limit" key:@"fps" min:30 max:120 def:60 step:5 fmt:@"%@: %.0f FPS"];
  [self slider:c text:@"Render height" key:@"height" min:480 max:1290 def:1290 step:10 fmt:@"%@: %.0fp"];
  [self slider:c text:@"UI scale" key:@"ui_scale_percent" min:50 max:200 def:100 step:5 fmt:@"%@: %.0f%%"];
  [self toggle:c text:@"Physgun halo (if shit happens)" key:@"physgun_halo" def:YES];
  [self toggle:c text:@"Simplified physgun glow (dont turn on unless issues)" key:@"fast_halo" def:NO];
  [self toggle:c text:@"sRGB fix (leave off unless HDR is off)" key:@"srgb_fix4" def:NO];

  // Controls
  c = (UIStackView *)[self card:@"CONTROLS"];
  [self toggle:c text:@"Touch controls" key:@"touch" def:YES];
  [self toggle:c text:@"Show joystick" key:@"touch_show_joystick" def:YES];
  [self toggle:c text:@"Tap to toggle sprint" key:@"touch_sprint_toggle" def:NO];
  { UIButton *cl = [self button:@"Control layout" action:@selector(openLayout)];
    cl.backgroundColor = [UIColor colorWithWhite:0.16 alpha:1]; cl.layer.cornerRadius = 10; [cl.heightAnchor constraintEqualToConstant:40].active = YES; [c addArrangedSubview:cl];
    UIButton *cb = [self button:@"Custom buttons keys o algo / commands and etc" action:@selector(openCustomButtons)];
    cb.backgroundColor = [UIColor colorWithWhite:0.16 alpha:1]; cb.layer.cornerRadius = 10; [cb.heightAnchor constraintEqualToConstant:40].active = YES; [c addArrangedSubview:cb]; }
  [self slider:c text:@"Touch opacity" key:@"touch_opacity" min:0 max:100 def:60 step:5 fmt:@"%@: %.0f%%"];
  [self toggle:c text:@"Inverted camera (why tho)" key:@"invert" def:NO];

  // Gameplay
  c = (UIStackView *)[self card:@"GAMEPLAY"];
  [self toggle:c text:@"Walk through allied NPCs" key:@"gmod_npc_walkthrough" def:NO];
  [self toggle:c text:@"Walk animation" key:@"gmod_walk_anim" def:NO];
  [self toggle:c text:@"Mute failed grab sound" key:@"gmod_mute_failed_grab" def:NO];
  [self toggle:c text:@"Dev console" key:@"console" def:YES];

  // Advanced
  c = (UIStackView *)[self card:@"LAUNCH ARGUMENTS"];
  self.argsField = [self field:[D stringForKey:@"launch_args"] ?: @"" placeholder:@"-novid +sv_cheats 1"];
  [c addArrangedSubview:self.argsField];
  UILabel *def = [self label:@"Always added: -game garrysmod -nouserclip -novid -hushasserts -mobile -heapsize 1024 -fullscreen -w/-h +exec android_settings.cfg" size:12 bold:NO];
  def.textColor = [UIColor colorWithWhite:0.5 alpha:1]; [c addArrangedSubview:def];
  [c addArrangedSubview:[self button:@"Reset launch arguments" action:@selector(resetArgs)]];
  c = (UIStackView *)[self card:@"FILES"];
  [c addArrangedSubview:[self button:@"Backup saves" action:@selector(backup)]];
  [c addArrangedSubview:[self button:@"Share logs" action:@selector(shareLogs)]];
  [c addArrangedSubview:[self button:@"How to add game files" action:@selector(help)]];
}

- (UITextField *)field:(NSString *)v placeholder:(NSString *)p {
  UITextField *f = [UITextField new]; f.text = v; f.textColor = UIColor.whiteColor; f.delegate = self;
  f.attributedPlaceholder = [[NSAttributedString alloc] initWithString:p attributes:@{NSForegroundColorAttributeName: [UIColor colorWithWhite:0.4 alpha:1]}];
  f.backgroundColor = [UIColor colorWithWhite:0.2 alpha:1]; f.layer.cornerRadius = 8;
  f.leftView = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 10, 10)]; f.leftViewMode = UITextFieldViewModeAlways;
  f.autocorrectionType = UITextAutocorrectionTypeNo; f.autocapitalizationType = UITextAutocapitalizationTypeNone;
  f.returnKeyType = UIReturnKeyDone; [f.heightAnchor constraintEqualToConstant:40].active = YES; return f;
}
- (BOOL)textFieldShouldReturn:(UITextField *)t { [t resignFirstResponder]; return YES; }

- (void)refreshStart {
  BOOL map = [[D stringForKey:@"gmod_start_at"] isEqualToString:@"map"];
  [self.startBtn setTitle:map ? @"That map" : @"Just the menu" forState:0];
  [self.mapBtn setTitle:[D stringForKey:@"gmod_map"] ?: @"gm_construct" forState:0];
  self.mapBtn.enabled = map; self.mapBtn.alpha = map ? 1 : 0.4;
}

- (void)pickStart {
  UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Start at" message:nil preferredStyle:UIAlertControllerStyleActionSheet];
  for (NSString *s in @[@"menu", @"map"])
    [a addAction:[UIAlertAction actionWithTitle:[s isEqualToString:@"map"] ? @"That map" : @"Just the menu" style:0 handler:^(UIAlertAction *x) {
      [D setObject:s forKey:@"gmod_start_at"]; [self refreshStart]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
  a.popoverPresentationController.sourceView = self.startBtn;
  [self presentViewController:a animated:YES completion:nil];
}

- (NSArray *)scanMaps {
  NSMutableSet *s = [NSMutableSet setWithArray:@[@"gm_construct", @"gm_flatgrass"]];
  for (NSString *dir in @[@"garrysmod/maps", @"garrysmod/download/maps"]) {
    NSString *p = [self.docs stringByAppendingPathComponent:dir];
    for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:p error:nil])
      if ([f.pathExtension.lowercaseString isEqualToString:@"bsp"]) [s addObject:f.stringByDeletingPathExtension];
  }
  NSString *addons = [self.docs stringByAppendingPathComponent:@"garrysmod/addons"];
  for (NSString *ad in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:addons error:nil]) {
    NSString *p = [[addons stringByAppendingPathComponent:ad] stringByAppendingPathComponent:@"maps"];
    for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:p error:nil])
      if ([f.pathExtension.lowercaseString isEqualToString:@"bsp"]) [s addObject:f.stringByDeletingPathExtension];
  }
  return [s.allObjects sortedArrayUsingSelector:@selector(caseInsensitiveCompare:)];
}

- (void)pickMap {
  UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Map" message:nil preferredStyle:UIAlertControllerStyleActionSheet];
  for (NSString *m in [self scanMaps])
    [a addAction:[UIAlertAction actionWithTitle:m style:0 handler:^(UIAlertAction *x) {
      [D setObject:m forKey:@"gmod_map"]; [self refreshStart]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Type a name…" style:0 handler:^(UIAlertAction *x) {
    UIAlertController *t = [UIAlertController alertControllerWithTitle:@"Map name" message:nil preferredStyle:UIAlertControllerStyleAlert];
    [t addTextFieldWithConfigurationHandler:^(UITextField *f) { f.placeholder = @"gm_bigcity"; f.autocapitalizationType = 0; }];
    [t addAction:[UIAlertAction actionWithTitle:@"OK" style:0 handler:^(UIAlertAction *y) {
      NSString *n = [t.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
      if (n.length) { [D setObject:n forKey:@"gmod_map"]; [self refreshStart]; } }]];
    [t addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:t animated:YES completion:nil]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
  a.popoverPresentationController.sourceView = self.mapBtn;
  [self presentViewController:a animated:YES completion:nil];
}

- (void)help {
  UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Game files"
    message:@"Open Files → On My iPhone → GMod and copy the garrysmod, sourceengine and platform folders from a PC Garry's Mod install."
    preferredStyle:UIAlertControllerStyleAlert];
  [a addAction:[UIAlertAction actionWithTitle:@"OK" style:0 handler:nil]];
  [self presentViewController:a animated:YES completion:nil];
}

static float fv(NSString *k, float d) { return [D objectForKey:k] ? [D floatForKey:k] : d; }
static BOOL bv(NSString *k, BOOL d) { return [D objectForKey:k] ? [D boolForKey:k] : d; }

- (void)writeCfg:(NSString *)wr {
  NSString *name = [self.nameField.text stringByReplacingOccurrencesOfString:@"\"" withString:@""];
  NSMutableString *s = [NSMutableString stringWithString:@"// Generated by the iOS launcher\n"];
  [s appendFormat:@"fps_max %.0f\n", fv(@"fps", 60)];
  [s appendFormat:@"touch_opacity %g\n", fv(@"touch_opacity", 60) / 100.0];
  [s appendFormat:@"touch_show_joystick %d\n", bv(@"touch_show_joystick", YES)];
  [s appendFormat:@"touch_sprint_toggle %d\n", bv(@"touch_sprint_toggle", NO)];
  [s appendFormat:@"touch_enable %d\n", bv(@"touch", YES)];
  [s appendFormat:@"vgui_ui_scale %g\n", fv(@"ui_scale_percent", 100) / 100.0];
  [s appendFormat:@"physgun_halo %d\n", bv(@"physgun_halo", YES)];
  [s appendFormat:@"gmod_physgun_fast_halo %d\n", bv(@"fast_halo", NO)];
  [s appendString:@"gmod_physgun_optimize_halo 1\ngl_program_binary_cache 0\n"];
  [s appendFormat:@"m_pitch %s\n", bv(@"invert", NO) ? "-0.022" : "0.022"];
  if (name.length) [s appendFormat:@"name \"%@\"\n", name];
  [s appendString:@"vr_mainmenu 0\n"];
  for (NSString *base in @[wr, [self.docs stringByAppendingPathComponent:@"garrysmod"]]) {
    NSString *d = [base stringByAppendingPathComponent:@"cfg"];
    [[NSFileManager defaultManager] createDirectoryAtPath:d withIntermediateDirectories:YES attributes:nil error:nil];
    [s writeToFile:[d stringByAppendingPathComponent:@"android_settings.cfg"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
  }
}

- (void)resetArgs { self.argsField.text = @""; [D setObject:@"" forKey:@"launch_args"]; }
- (void)share:(NSArray *)items from:(UIView *)v {
  UIActivityViewController *a = [[UIActivityViewController alloc] initWithActivityItems:items applicationActivities:nil];
  a.popoverPresentationController.sourceView = v ?: self.view; [self presentViewController:a animated:YES completion:nil];
}
- (void)shareLogs {
  NSMutableArray *f = [NSMutableArray new];
  for (NSString *n in @[@"gmod_log.txt", @"gmod_boot.txt"]) { NSString *p = [self.docs stringByAppendingPathComponent:n];
    if ([[NSFileManager defaultManager] fileExistsAtPath:p]) [f addObject:[NSURL fileURLWithPath:p]]; }
  [self share:f from:nil];
}
- (NSString *)writeRoot { return [[[self.docs stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"Library/user/garrysmod"] stringByStandardizingPath]; }
- (void)backup {
  NSFileManager *fm = NSFileManager.defaultManager; NSMutableArray *f = [NSMutableArray new];
  NSString *dst = [self.docs stringByAppendingPathComponent:[NSString stringWithFormat:@"saves_backup_%ld", (long)NSDate.date.timeIntervalSince1970]];
  for (NSString *base in @[[self writeRoot], [self.docs stringByAppendingPathComponent:@"garrysmod"]])
    for (NSString *d in @[@"saves", @"data", @"settings", @"cfg"]) {
      NSString *p = [base stringByAppendingPathComponent:d]; BOOL dir;
      if ([fm fileExistsAtPath:p isDirectory:&dir] && dir) {
        NSString *t = [dst stringByAppendingPathComponent:[[base.lastPathComponent stringByAppendingString:@"_"] stringByAppendingString:[base isEqual:[self writeRoot]] ? [@"user/" stringByAppendingString:d] : d]];
        [fm createDirectoryAtPath:[t stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:nil];
        if ([fm copyItemAtPath:p toPath:t error:nil]) [f addObject:d];
      }
    }
  UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Backup saves"
    message:f.count ? [NSString stringWithFormat:@"Copied to Files → GMod → %@", dst.lastPathComponent] : @"No saves yet, go play first" preferredStyle:1];
  [a addAction:[UIAlertAction actionWithTitle:@"OK" style:0 handler:nil]]; [self presentViewController:a animated:YES completion:nil];
}
- (void)mpTap:(UIButton *)b {
  NSString *act = b.accessibilityIdentifier;
  if ([act isEqual:@"host"]) {
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Host LAN game" message:@"Starts the selected map with 8 slots on LAN." preferredStyle:1];
    [a addAction:[UIAlertAction actionWithTitle:@"Host" style:0 handler:^(UIAlertAction *x) { [self launch:@"host" target:[D stringForKey:@"gmod_map"] ?: @"gm_construct"]; }]];
    [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:1 handler:nil]]; [self presentViewController:a animated:YES completion:nil]; return;
  }
  if ([act isEqual:@"load"]) {
    NSMutableArray *saves = [NSMutableArray new];
    for (NSString *base in @[[self writeRoot], [self.docs stringByAppendingPathComponent:@"garrysmod"]])
      for (NSString *f in [NSFileManager.defaultManager contentsOfDirectoryAtPath:[base stringByAppendingPathComponent:@"save"] error:nil])
        if ([f.pathExtension isEqual:@"sav"] && ![saves containsObject:f.stringByDeletingPathExtension]) [saves addObject:f.stringByDeletingPathExtension];
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Load a save" message:saves.count ? nil : @"No saves here" preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSString *sv in saves) [a addAction:[UIAlertAction actionWithTitle:sv style:0 handler:^(UIAlertAction *x) { [self launch:@"load" target:sv]; }]];
    [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:1 handler:nil]]; a.popoverPresentationController.sourceView = b;
    [self presentViewController:a animated:YES completion:nil]; return;
  }
  { extern void lan_present(UIViewController *, void (^)(NSString *));
    lan_present(self, ^(NSString *addr) { [self launch:@"connect" target:addr]; }); return; }
  UIAlertController *t = [UIAlertController alertControllerWithTitle:@"Join by IP" message:nil preferredStyle:1];
  [t addTextFieldWithConfigurationHandler:^(UITextField *f) { f.placeholder = @"192.168.1.10:27015"; f.text = [D stringForKey:@"last_ip"]; f.keyboardType = UIKeyboardTypeNumbersAndPunctuation; }];
  [t addAction:[UIAlertAction actionWithTitle:@"Connect" style:0 handler:^(UIAlertAction *x) {
    NSString *ip = [t.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if (ip.length) { [D setObject:ip forKey:@"last_ip"]; [self launch:@"connect" target:ip]; } }]];
  [t addAction:[UIAlertAction actionWithTitle:@"Cancel" style:1 handler:nil]]; [self presentViewController:t animated:YES completion:nil];
}
- (void)openLayout { extern void touch_editor_present(UIViewController *, NSString *, BOOL, void (^)(BOOL));
  [self ensureTouchDefaults]; touch_editor_present(self, [self writeRoot], NO, nil); }
- (void)openCustomButtons { extern void cb_present(UIViewController *, void (^)(void)); cb_present(self, nil); }
- (void)ensureTouchDefaults { [NSFileManager.defaultManager createDirectoryAtPath:[[self writeRoot] stringByAppendingPathComponent:@"cfg"] withIntermediateDirectories:YES attributes:nil error:nil]; }
- (void)openWorkshop { extern void workshop_present(UIViewController *); workshop_present(self); }
- (void)play {
  BOOL map = [[D stringForKey:@"gmod_start_at"] isEqualToString:@"map"];
  [self launch:map ? @"map" : @"menu" target:[D stringForKey:@"gmod_map"] ?: @"gm_construct"];
}
- (void)launch:(NSString *)action target:(NSString *)target {
  [self.view endEditing:YES];
  [D setObject:self.argsField.text ?: @"" forKey:@"launch_args"];
  [D setObject:self.nameField.text ?: @"" forKey:@"player_name"];
  [D synchronize];
  NSArray *user = [self parse:self.argsField.text ?: @""];
  if (!user) return;
  NSMutableArray *a = g_args;
  [a addObjectsFromArray:@[@"gmod", @"-game", @"garrysmod"]];
  for (NSString *x in @[@"-nouserclip", @"-novid", @"-hushasserts", @"-mobile"]) if (![user containsObject:x]) [a addObject:x];
  if (![user containsObject:@"-heapsize"]) [a addObjectsFromArray:@[@"-heapsize", @"1024"]];
  if (![user containsObject:@"-windowed"] && ![user containsObject:@"-window"] && ![user containsObject:@"-sw"] && ![user containsObject:@"-fullscreen"]) [a addObject:@"-fullscreen"];
  { CGSize sz = UIScreen.mainScreen.nativeBounds.size; int H = (int)MIN(sz.width, sz.height), W = (int)MAX(sz.width, sz.height);
    int h = MIN(H, (int)fv(@"height", H)); int w = MAX(640, (W * h / H) / 2 * 2);
    if (![user containsObject:@"-width"] && ![user containsObject:@"-w"]) [a addObjectsFromArray:@[@"-w", @(w).stringValue]];
    if (![user containsObject:@"-height"] && ![user containsObject:@"-h"]) [a addObjectsFromArray:@[@"-h", @(h).stringValue]]; }
  if (bv(@"console", YES) && ![user containsObject:@"-console"]) [a addObject:@"-console"];
  [a addObjectsFromArray:@[@"+exec", @"android_settings.cfg"]];
  [a addObjectsFromArray:@[@"+hlvr_disable_player_ally_collisions", bv(@"gmod_npc_walkthrough", NO) ? @"2" : @"0",
                           @"+gmodvr_walk_anim", bv(@"gmod_walk_anim", NO) ? @"1" : @"0",
                           @"+snd_flat_mute_use_deny", bv(@"gmod_mute_failed_grab", NO) ? @"1" : @"0"]];
  BOOL hasMap = [user containsObject:@"+map"] || [user containsObject:@"+load"] || [user containsObject:@"+connect"];
  if ([action isEqual:@"host"] && !hasMap) {
    if (![user containsObject:@"+maxplayers"]) [a addObjectsFromArray:@[@"+maxplayers", @"8"]];
    if (![user containsObject:@"+sv_lan"]) [a addObjectsFromArray:@[@"+sv_lan", @"1"]];
  }
  [a addObjectsFromArray:user];
  if (!hasMap) {
    if ([action isEqual:@"map"] || [action isEqual:@"host"]) [a addObjectsFromArray:@[@"+map", target]];
    else if ([action isEqual:@"load"]) [a addObjectsFromArray:@[@"+load", target]];
    else if ([action isEqual:@"connect"]) [a addObjectsFromArray:@[@"+connect", target]];
  }
  setenv("GMOD_SRGB_FIX", bv(@"srgb_fix4", NO) ? "1" : "0", 1);
  g_done = 1;
}

- (NSArray *)parse:(NSString *)s {
  NSMutableArray *out = [NSMutableArray new]; NSMutableString *cur = [NSMutableString new];
  unichar q = 0; BOOL have = NO;
  for (NSUInteger i = 0; i < s.length; i++) {
    unichar ch = [s characterAtIndex:i];
    if (ch == '"' || ch == '\'') { if (!q) { q = ch; have = YES; continue; } if (q == ch) { q = 0; continue; } }
    if (!q && [[NSCharacterSet whitespaceAndNewlineCharacterSet] characterIsMember:ch]) {
      if (have) { [out addObject:[cur copy]]; [cur setString:@""]; have = NO; }
    } else { [cur appendFormat:@"%C", ch]; have = YES; }
  }
  if (q) {
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Launch arguments" message:@"You forgot a closing \"" preferredStyle:1];
    [a addAction:[UIAlertAction actionWithTitle:@"OK" style:0 handler:nil]];
    [self presentViewController:a animated:YES completion:nil]; return nil;
  }
  if (have) [out addObject:cur];
  return out;
}
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (BOOL)prefersStatusBarHidden { return YES; }
@end

// Shows the launcher, blocks (spinning the run loop) until Play. Returns argv (NULL-terminated).
char **ios_run_launcher(const char *docs, const char *wr, int *argc) {
  g_args = [NSMutableArray new]; g_done = 0;
  UIWindow *win;
  @autoreleasepool {
    UIWindowScene *scene = nil;
    for (UIScene *s in UIApplication.sharedApplication.connectedScenes)
      if ([s isKindOfClass:UIWindowScene.class]) { scene = (UIWindowScene *)s; break; }
    win = scene ? [[UIWindow alloc] initWithWindowScene:scene] : [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    GLauncher *vc = [GLauncher new]; vc.docs = [NSString stringWithUTF8String:docs];
    win.rootViewController = vc; win.windowLevel = UIWindowLevelNormal + 1;
    [win makeKeyAndVisible];
    while (!g_done) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    [vc writeCfg:[NSString stringWithUTF8String:wr]];
    { extern void cb_install(NSString *); cb_install([NSString stringWithUTF8String:wr]); }
    win.hidden = YES; win.rootViewController = nil;
  }
  win = nil;
  int n = (int)g_args.count; char **av = calloc(n + 1, sizeof(char *));
  for (int i = 0; i < n; i++) av[i] = strdup(g_args[i].UTF8String);
  *argc = n;
  for (int i = 0; i < n; i++) fprintf(stderr, "arg[%d] %s\n", i, av[i]);
  return av;
}
