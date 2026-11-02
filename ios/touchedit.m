// Control layout editor + custom buttons — port of GDroid 0.1.31 TouchLayout/TouchLayoutActivity/CustomButtons.
// Edits <user>/cfg/<touch profile> (touch_addbutton lines), Documents/custom_buttons.json and
// <user>/cfg/gmod_touch_custom.cfg + materials/gmodvr_touch_custom/*.vtf (same formats as GDroid).
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <objc/runtime.h>
#include <zlib.h>

#pragma mark - model

@interface TLButton : NSObject
@property(nonatomic, copy) NSString *name, *texture, *command;
@property(nonatomic) float x1, y1, x2, y2, aspect;
@property(nonatomic) int r, g, b, a, flags;
@end
@implementation TLButton
- (instancetype)init { if ((self = [super init])) { _r = _g = _b = _a = 255; _aspect = 1; } return self; }
- (BOOL)isZone { return self.texture.length == 0; }
- (BOOL)hidden { return self.flags & 1; }
- (BOOL)moreButton { return [self.command caseInsensitiveCompare:@"gmod_touch_more"] == NSOrderedSame; }
- (BOOL)canBeInMore { return !(self.isZone || self.moreButton || [self.command isEqual:@"_move"] || [self.command isEqual:@"_look"] || (self.flags & 2)); }
- (BOOL)inMore { return (self.flags & 8192) && self.canBeInMore; }
- (float)w { return self.x2 - self.x1; }
- (float)h { return self.y2 - self.y1; }
- (UIColor *)color { return [UIColor colorWithRed:self.r / 255.0 green:self.g / 255.0 blue:self.b / 255.0 alpha:1]; }
- (NSString *)label {
  NSDictionary *m = @{@"attack": @"Fire", @"attack2": @"Secondary fire", @"spawnmenu": @"Spawn menu", @"contextmenu": @"Context menu",
    @"lastinv": @"Last weapon", @"duck": @"Crouch", @"edit": @"Layout editor", @"look": @"Look zone", @"more": @"⋯ menu",
    @"move": @"Move zone", @"score": @"Scoreboard", @"speed": @"Sprint", @"voice": @"Voice chat", @"thirdperson": @"Third person",
    @"invnext": @"Next weapon", @"invprev": @"Previous weapon"};
  if (m[self.name]) return m[self.name];
  if ([self.name hasPrefix:@"custom_"]) return [@"Custom button " stringByAppendingString:[self.name substringFromIndex:7]];
  return self.name.length ? [[[self.name substringToIndex:1] uppercaseString] stringByAppendingString:[self.name substringFromIndex:1]] : @"?";
}
@end

static NSString *tlquote(NSString *s) { return [NSString stringWithFormat:@"\"%@\"", [s stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""]]; }
static NSArray<NSString *> *tltokens(NSString *s) {
  NSMutableArray *o = [NSMutableArray new]; NSUInteger i = 0, n = s.length;
  while (i < n) {
    while (i < n && [NSCharacterSet.whitespaceCharacterSet characterIsMember:[s characterAtIndex:i]]) i++;
    if (i == n || (i + 1 < n && [s characterAtIndex:i] == '/' && [s characterAtIndex:i + 1] == '/')) break;
    NSMutableString *t = [NSMutableString new]; BOOL q = [s characterAtIndex:i] == '"'; if (q) i++;
    while (i < n) { unichar c = [s characterAtIndex:i++];
      if ((q && c == '"') || (!q && ([NSCharacterSet.whitespaceCharacterSet characterIsMember:c] || c == ';'))) break;
      if (q && c == '\\' && i < n && [s characterAtIndex:i] == '"') { c = '"'; i++; }
      [t appendFormat:@"%C", c]; }
    [o addObject:t];
  }
  return o;
}

@interface TLLayout : NSObject
@property(nonatomic) float screenAspect; @property(nonatomic) BOOL snap; @property(nonatomic) int gridCount;
@property(nonatomic, copy) NSString *moreCloseTexture;
@property(nonatomic, strong) NSMutableArray<TLButton *> *buttons;
@property(nonatomic, strong) NSMutableArray *lines;  // NSString (verbatim line) or TLButton
@end
@implementation TLLayout
+ (instancetype)parse:(NSString *)text aspect:(float)aspect error:(NSString **)err {
  TLLayout *L = [TLLayout new]; L.screenAspect = aspect; L.gridCount = 50; L.moreCloseTexture = @"vgui/touch/sb_xg";
  L.buttons = [NSMutableArray new]; L.lines = [NSMutableArray new];
  NSArray *rows = [text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet];
  for (NSString *ln in rows) { NSArray *t = tltokens(ln); if (t.count < 2) continue;
    if ([t[0] isEqual:@"touch_more_close_texture"]) L.moreCloseTexture = t[1];
    if ([t[0] isEqual:@"touch_grid_count"]) L.gridCount = MAX(2, [t[1] intValue]);
    if ([t[0] isEqual:@"touch_grid_enable"]) L.snap = [t[1] intValue] != 0; }
  for (NSString *ln in rows) {
    NSArray *t = tltokens(ln); TLButton *b = nil;
    if (t.count && [t[0] isEqual:@"touch_addbutton"]) {
      if (t.count < 8 || ![t[1] length]) { if (err) *err = @"Broken layout file (bad button)"; return nil; }
      b = [TLButton new]; b.name = t[1]; b.texture = t[2]; b.command = t[3];
      float x1 = [t[4] floatValue], y1 = [t[5] floatValue], x2 = [t[6] floatValue], y2 = [t[7] floatValue];
      if (x2 <= x1 || y2 <= y1) { if (err) *err = [NSString stringWithFormat:@"Invalid bounds for %@.", b.name]; return nil; }
      if (t.count >= 12) { b.r = MAX(0, MIN(255, [t[8] intValue])); b.g = MAX(0, MIN(255, [t[9] intValue])); b.b = MAX(0, MIN(255, [t[10] intValue])); b.a = MAX(0, MIN(255, [t[11] intValue])); }
      if (t.count >= 13) b.flags = [t[12] intValue] & ~19460;
      float asp = t.count >= 14 ? [t[13] floatValue] : 1; if (asp <= 0) asp = 1;
      if (!b.isZone) y2 = y1 + (x2 - x1) * aspect * asp;
      [L rect:b x:x1 y:y1 w:x2 - x1 h:y2 - y1];
      if ([L find:b.name]) { if (err) *err = [NSString stringWithFormat:@"%@ is in there twice", b.name]; return nil; }
      [L.buttons addObject:b];
    }
    [L.lines addObject:b ?: ln];
  }
  if (!L.buttons.count) { if (err) *err = @"Layout file is empty"; return nil; }
  [L migrateMore];
  return L;
}
- (void)migrateMore {
  TLButton *m = [self find:@"more"]; if (!m || !m.moreButton) return;
  if ([m.texture isEqual:@"vgui/touch/sb_xg"]) m.texture = @"vgui/touch/sb_menuplus";
  if (m.flags & 8192) return; m.flags |= 8192;
  NSArray *n = @[@"console", @"kill", @"cleanup", @"thirdperson"], *c = @[@"toggleconsole", @"kill", @"gmod_cleanup", @"gmod_touch_thirdperson"];
  float xs[] = {0.7f, 0.76f, 0.82f, 0.88f};
  for (int i = 0; i < 4; i++) { TLButton *b = [self find:n[i]];
    if (b && [b.command caseInsensitiveCompare:c[i]] == NSOrderedSame && (b.hidden || (fabsf(b.x1 - xs[i]) < 0.002f && b.y1 < 0.002f))) b.flags = (b.flags | 8192) & ~1; }
}
- (TLButton *)find:(NSString *)n { for (TLButton *b in self.buttons) if ([b.name isEqual:n]) return b; return nil; }
static float clampf(float v, float a, float b) { return MAX(a, MIN(b, v)); }
- (void)rect:(TLButton *)b x:(float)x y:(float)y w:(float)w h:(float)h {
  float mw = 2.0f / self.gridCount, mh = MIN(1.0f, self.screenAspect * mw);
  w = clampf(w, mw, 1); h = clampf(h, mh, 1); x = clampf(x, 0, 1 - w); y = clampf(y, 0, 1 - h);
  float x2 = x + w, y2 = y + h;
  if (self.snap) {
    int g = self.gridCount; x = roundf(x * g) / g; x2 = roundf(x2 * g) / g;
    float gy = g / self.screenAspect; y = roundf(y * gy) / gy; y2 = roundf(y2 * gy) / gy;
    if (y2 > 1) { float fl = floorf(gy) / gy; y -= y2 - fl; y2 = fl; }
    if (x2 > 1) { x -= x2 - 1; x2 = 1; }
  }
  b.x1 = MAX(0, x); b.y1 = MAX(0, y); b.x2 = x2; b.y2 = y2;
  b.aspect = b.h / (b.w * self.screenAspect);
}
- (TLButton *)moreAnchor { for (TLButton *b in self.buttons) if (b.moreButton && !b.hidden) return b; return nil; }
- (NSArray<TLButton *> *)arrangeMore:(TLButton *)except {
  NSMutableArray *out = [NSMutableArray new], *items = [NSMutableArray new];
  TLButton *m = self.moreAnchor; if (!m) return out;
  for (TLButton *b in self.buttons) if (b.inMore && !b.hidden && b != except) [items addObject:b];
  float w = m.w, h = m.h, fx = m.x1; BOOL right = m.x1 + m.x2 > 1; float fy = m.y1; BOOL top = m.y1 + m.y2 < 1;
  if (!right) fx = 1 - m.x2; if (top) fy = 1 - m.y2;
  int cols = (int)(fx / w + 0.001f), rows = (int)(fy / h + 0.001f) + 1; int n = (int)items.count;
  int per = MAX(1, MIN(cols, MAX(1, (int)(0.5f / w + 0.001f))));
  if (cols > 0 && (n + per - 1) / per > rows) per = MIN(cols, (n + rows - 1) / rows);
  for (int i = 0; i < n && cols > 0; i++) {
    int row = i / per, col = i % per, inrow = MIN(per, n - row * per); if (row >= rows) break;
    int k = right ? inrow - col : col + 1; TLButton *b = items[i];
    float x = right ? m.x1 - k * w : m.x2 + (k - 1) * w; float y = top ? m.y1 + row * h : m.y1 - row * h;
    b.x1 = x; b.y1 = y; b.x2 = x + w; b.y2 = y + h; b.aspect = m.aspect; [out addObject:b];
  }
  return out;
}
- (BOOL)coveredByMore:(TLButton *)b list:(NSArray *)list {
  if (b == self.moreAnchor || b.inMore || [b.command isEqual:@"_move"] || [b.command isEqual:@"_look"] || (b.flags & 2)) return NO;
  for (TLButton *o in list) if (b.x1 < o.x2 - 0.001f && b.x2 > o.x1 + 0.001f && b.y1 < o.y2 - 0.001f && b.y2 > o.y1 + 0.001f) return YES;
  return NO;
}
- (CGRect)moreArea:(NSArray *)list {
  TLButton *m = self.moreAnchor; if (!m) return CGRectNull;
  float a = m.x1, b = m.y1, c = m.x2, d = m.y2;
  for (TLButton *o in list) { a = MIN(a, o.x1); b = MIN(b, o.y1); c = MAX(c, o.x2); d = MAX(d, o.y2); }
  return CGRectMake(a, b, c - a, d - b);
}
- (BOOL)overMore:(TLButton *)b x:(float)x y:(float)y {
  TLButton *m = self.moreAnchor; return m && !b.inMore && b.canBeInMore && x > m.x1 && x < m.x2 && y > m.y1 && y < m.y2;
}
- (BOOL)setInMore:(TLButton *)b on:(BOOL)on {
  if (!b.canBeInMore) return NO;
  if (on && !(b.flags & 8192)) {
    b.flags = (b.flags | 8192) & ~1; [self.buttons removeObject:b]; [self.buttons addObject:b];
    [self.lines removeObject:b]; [self.lines addObject:b]; [self arrangeMore:nil]; return YES;
  }
  if (!on) b.flags &= ~8192;
  [self arrangeMore:nil]; return YES;
}
- (BOOL)dropMore:(TLButton *)b x:(float)x y:(float)y open:(BOOL)open {
  if ([self overMore:b x:x y:y]) return [self setInMore:b on:YES];
  if (!b.inMore) return NO;
  if (open) { CGRect r = [self moreArea:[self arrangeMore:b]];
    if (!CGRectIsNull(r) && x > r.origin.x && x < CGRectGetMaxX(r) && y > r.origin.y && y < CGRectGetMaxY(r)) { [self arrangeMore:nil]; return NO; } }
  return [self setInMore:b on:NO];
}
- (NSString *)cfgLine:(TLButton *)b {
  return [NSString stringWithFormat:@"touch_addbutton %@ %@ %@ %.6f %.6f %.6f %.6f %d %d %d %d %d %.6f", tlquote(b.name), tlquote(b.texture), tlquote(b.command),
          b.x1, b.y1, b.x2, b.y2, b.r, b.g, b.b, b.a, b.flags & ~19460, b.aspect];
}
- (NSString *)cfg:(NSString *)profile {
  NSMutableString *s = [NSMutableString stringWithString:@"// Saved by the launcher control layout editor.\n"];
  [s appendFormat:@"touch_config_file %@\ntouch_grid_count %d\ntouch_grid_enable %d\n", tlquote(profile), self.gridCount, self.snap ? 1 : 0];
  if (![self.moreCloseTexture isEqual:@"vgui/touch/sb_xg"]) [s appendFormat:@"touch_more_close_texture %@\n", tlquote(self.moreCloseTexture)];
  for (id l in self.lines) {
    if ([l isKindOfClass:TLButton.class]) { [s appendString:[self cfgLine:l]]; [s appendString:@"\n"]; continue; }
    NSArray *t = tltokens(l);
    if (t.count && [@[@"touch_config_file", @"touch_grid_count", @"touch_grid_enable", @"touch_more_close_texture"] containsObject:t[0]]) continue;
    [s appendString:l]; [s appendString:@"\n"];
  }
  return s;
}
@end

static NSString *tl_profile_file(NSString *configCfg) {
  NSString *p = @"touch.cfg";
  for (NSString *ln in [configCfg componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
    NSArray *t = tltokens(ln); if (t.count >= 2 && [t[0] isEqual:@"touch_config_file"]) p = t[1]; }
  NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:@"^[A-Za-z0-9_.-]+\\.cfg$" options:0 error:nil];
  if (![re numberOfMatchesInString:p options:0 range:NSMakeRange(0, p.length)] || [p containsString:@".."]) return @"touch.cfg";
  return p;
}

#pragma mark - custom buttons (CustomButtons.java)

@interface CBEntry : NSObject
@property(nonatomic) int ident, slot;
@property(nonatomic, copy) NSString *key, *with, *consoleCommand, *label, *shape, *mode;  // shape circle|square|sharp, mode hold|toggle
@property(nonatomic, copy) NSString *imageEncoded;  // base64(zlib(raw RGBA 256x256)) or nil
@end
@implementation CBEntry
- (NSString *)buttonName { return [NSString stringWithFormat:@"custom_%d", self.ident]; }
- (BOOL)isCommand { return self.consoleCommand != nil; }
- (NSString *)keySpec { return self.with ? [NSString stringWithFormat:@"%@+%@", self.with, self.key] : self.key; }
- (BOOL)hasWheel { return [self.key hasPrefix:@"MOUSE_WHEEL_"] || [self.with hasPrefix:@"MOUSE_WHEEL_"]; }
@end

static NSArray<NSString *> *cb_parts(NSString *s) {
  NSMutableArray *o = [NSMutableArray new];
  for (NSString *p in [s ?: @"" componentsSeparatedByString:@";"]) { NSString *t = [p stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]; if (t.length) [o addObject:t]; }
  return o;
}
static BOOL cb_can_toggle(NSString *s) {
  NSArray *p = cb_parts(s); if (!p.count) return NO;
  for (NSString *x in p) if (x.length < 2 || [x characterAtIndex:0] != '+' || [x rangeOfCharacterFromSet:NSCharacterSet.whitespaceCharacterSet].location != NSNotFound) return NO;
  return YES;
}
static BOOL cb_toggle_effective(CBEntry *e) {
  if (![e.mode isEqual:@"toggle"]) return NO;
  return e.isCommand ? cb_can_toggle(e.consoleCommand) : !e.hasWheel;
}
static NSString *cb_command(CBEntry *e) {
  BOOL t = cb_toggle_effective(e);
  if (e.isCommand) return t ? [NSString stringWithFormat:@"gmod_touch_command_toggle %@ %@", e.buttonName, [cb_parts(e.consoleCommand) componentsJoinedByString:@" "]] : e.consoleCommand;
  return t ? [NSString stringWithFormat:@"gmod_touch_key_toggle %@ %@", e.keySpec, e.buttonName] : [@"+gmod_touch_key " stringByAppendingString:e.keySpec];
}
NSString *cb_command_error(NSString *s) {
  NSString *t = [s ?: @"" stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
  if (!cb_parts(t).count) return @"Type a command";
  if ([t containsString:@"\""]) return @"Quotation marks (\") can't be used in a button's command.";
  if ([t rangeOfCharacterFromSet:NSCharacterSet.newlineCharacterSet].location != NSNotFound) return @"One line only";
  if ([t lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 255) return @"Too long (255 max)";
  return nil;
}

// key list (CustomButtonKeys.java): name, label, group, title
static NSArray<NSArray<NSString *> *> *cb_keys(void) {
  static NSMutableArray *k; if (k) return k; k = [NSMutableArray new];
  for (char c = 'A'; c <= 'Z'; c++) [k addObject:@[[NSString stringWithFormat:@"KEY_%c", c], [NSString stringWithFormat:@"%c", c], @"Letters", [NSString stringWithFormat:@"%c", c]]];
  for (char c = '0'; c <= '9'; c++) [k addObject:@[[NSString stringWithFormat:@"KEY_%c", c], [NSString stringWithFormat:@"%c", c], @"Numbers", [NSString stringWithFormat:@"%c", c]]];
  for (int i = 1; i <= 12; i++) [k addObject:@[[NSString stringWithFormat:@"KEY_F%d", i], [NSString stringWithFormat:@"F%d", i], @"Function keys", [NSString stringWithFormat:@"F%d", i]]];
  NSArray *more = @[
    @[@"KEY_SPACE", @"SPC", @"Common keys", @"Space"], @[@"KEY_ENTER", @"ENT", @"Common keys", @"Enter"], @[@"KEY_TAB", @"TAB", @"Common keys", @"Tab"],
    @[@"KEY_BACKSPACE", @"BKSP", @"Common keys", @"Backspace"], @[@"KEY_ESCAPE", @"ESC", @"Common keys", @"Escape"], @[@"KEY_LSHIFT", @"SHFT", @"Common keys", @"Shift"],
    @[@"KEY_LCONTROL", @"CTRL", @"Common keys", @"Ctrl"], @[@"KEY_LALT", @"ALT", @"Common keys", @"Alt"], @[@"KEY_RSHIFT", @"RSHF", @"Common keys", @"Right Shift"],
    @[@"KEY_RCONTROL", @"RCTL", @"Common keys", @"Right Ctrl"], @[@"KEY_RALT", @"RALT", @"Common keys", @"Right Alt"], @[@"KEY_CAPSLOCK", @"CAPS", @"Common keys", @"Caps Lock"],
    @[@"KEY_INSERT", @"INS", @"Common keys", @"Insert"], @[@"KEY_DELETE", @"DEL", @"Common keys", @"Delete"], @[@"KEY_HOME", @"HOME", @"Common keys", @"Home"],
    @[@"KEY_END", @"END", @"Common keys", @"End"], @[@"KEY_PAGEUP", @"PGUP", @"Common keys", @"Page Up"], @[@"KEY_PAGEDOWN", @"PGDN", @"Common keys", @"Page Down"],
    @[@"KEY_UP", @"↑", @"Arrows", @"Up"], @[@"KEY_DOWN", @"↓", @"Arrows", @"Down"], @[@"KEY_LEFT", @"←", @"Arrows", @"Left"], @[@"KEY_RIGHT", @"→", @"Arrows", @"Right"]];
  [k addObjectsFromArray:more];
  for (int i = 0; i <= 9; i++) [k addObject:@[[NSString stringWithFormat:@"KEY_PAD_%d", i], [NSString stringWithFormat:@"N%d", i], @"Numpad", [NSString stringWithFormat:@"Numpad %d", i]]];
  [k addObjectsFromArray:@[
    @[@"KEY_PAD_DIVIDE", @"N/", @"Numpad", @"Numpad /"], @[@"KEY_PAD_MULTIPLY", @"N*", @"Numpad", @"Numpad *"], @[@"KEY_PAD_MINUS", @"N-", @"Numpad", @"Numpad -"],
    @[@"KEY_PAD_PLUS", @"N+", @"Numpad", @"Numpad +"], @[@"KEY_PAD_ENTER", @"NENT", @"Numpad", @"Numpad Enter"], @[@"KEY_PAD_DECIMAL", @"N.", @"Numpad", @"Numpad ."],
    @[@"KEY_LBRACKET", @"[", @"Symbols", @"["], @[@"KEY_RBRACKET", @"]", @"Symbols", @"]"], @[@"KEY_SEMICOLON", @";", @"Symbols", @";"], @[@"KEY_APOSTROPHE", @"'", @"Symbols", @"'"],
    @[@"KEY_BACKQUOTE", @"`", @"Symbols", @"`"], @[@"KEY_COMMA", @",", @"Symbols", @","], @[@"KEY_PERIOD", @".", @"Symbols", @"."], @[@"KEY_SLASH", @"/", @"Symbols", @"/"],
    @[@"KEY_BACKSLASH", @"\\", @"Symbols", @"\\"], @[@"KEY_MINUS", @"-", @"Symbols", @"-"], @[@"KEY_EQUAL", @"=", @"Symbols", @"="],
    @[@"MOUSE_MIDDLE", @"M3", @"Mouse", @"Mouse 3 (middle)"], @[@"MOUSE_4", @"M4", @"Mouse", @"Mouse 4"], @[@"MOUSE_5", @"M5", @"Mouse", @"Mouse 5"],
    @[@"MOUSE_WHEEL_UP", @"MW↑", @"Mouse", @"Wheel up"], @[@"MOUSE_WHEEL_DOWN", @"MW↓", @"Mouse", @"Wheel down"],
    @[@"MOUSE_LEFT", @"M1", @"Mouse", @"Mouse 1 (left)"], @[@"MOUSE_RIGHT", @"M2", @"Mouse", @"Mouse 2 (right)"]]];
  return k;
}
static NSArray<NSString *> *cb_key(NSString *n) { for (NSArray *k in cb_keys()) if ([k[0] isEqual:n]) return k; return nil; }
static NSString *cb_default_label(CBEntry *e) {
  if (e.isCommand) {
    NSArray *p = cb_parts(e.consoleCommand); NSString *w = p.count ? [p[0] componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet][0] : @"";
    while ([w hasPrefix:@"+"] || [w hasPrefix:@"-"]) w = [w substringFromIndex:1];
    w = w.uppercaseString; return w.length > 4 ? [w substringToIndex:4] : (w.length ? w : @"?");
  }
  NSArray *k = cb_key(e.key), *w = e.with ? cb_key(e.with) : nil; if (!k) return @"?";
  if (!w) return k[1];
  NSString *c = [NSString stringWithFormat:@"%@+%@", w[1], k[1]];
  return [c lengthOfBytesUsingEncoding:NSUTF32StringEncoding] / 4 <= 4 ? c : k[1];
}
static NSUInteger cpcount(NSString *s) { return [s lengthOfBytesUsingEncoding:NSUTF32LittleEndianStringEncoding] / 4; }
// returns nil if valid, else the error
NSString *cb_validate(CBEntry *e) {
  if (e.ident <= 0) return @"Bad custom button id.";
  if (e.isCommand) { NSString *err = cb_command_error(e.consoleCommand); if (err) return err;
    e.consoleCommand = [e.consoleCommand stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]; }
  else { if (!cb_key(e.key)) return @"Pick a key.";
    if (e.with && (!cb_key(e.with) || [e.with isEqual:e.key])) return @"Pick two different keys, or Nothing."; }
  if (e.slot < 0) return @"Bad slot.";
  NSString *l = [e.label ?: @"" stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
  if (!l.length) l = cb_default_label(e);
  if (cpcount(l) > 4) return @"Label has to be 1-4 letters";
  e.label = l;
  if (![@[@"circle", @"square", @"sharp"] containsObject:e.shape ?: @""]) e.shape = @"circle";
  if (![e.mode isEqual:@"toggle"]) e.mode = @"hold";
  return nil;
}
static NSMutableArray<CBEntry *> *cb_from_json(NSData *d) {
  NSMutableArray *o = [NSMutableArray new]; if (!d) return o;
  NSDictionary *j = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
  NSMutableSet *seen = [NSMutableSet new];
  for (NSDictionary *b in ([j isKindOfClass:NSDictionary.class] ? j[@"buttons"] : nil)) {
    if (![b isKindOfClass:NSDictionary.class]) continue;
    CBEntry *e = [CBEntry new]; e.ident = [b[@"id"] intValue]; e.slot = b[@"slot"] ? [b[@"slot"] intValue] : -1;
    if (b[@"command"]) e.consoleCommand = b[@"command"]; else { e.key = b[@"key"]; e.with = b[@"with"]; }
    e.label = b[@"label"]; e.shape = b[@"shape"]; e.mode = b[@"mode"];
    if ([b[@"image"] isKindOfClass:NSString.class]) e.imageEncoded = b[@"image"];
    if (cb_validate(e) || [seen containsObject:@(e.ident)]) continue;
    [seen addObject:@(e.ident)]; [o addObject:e];
  }
  return o;
}
static NSData *cb_to_json(NSArray<CBEntry *> *l) {
  NSMutableArray *a = [NSMutableArray new]; int ver = 1;
  for (CBEntry *e in l) {
    NSMutableDictionary *d = [@{@"id": @(e.ident), @"slot": @(e.slot)} mutableCopy];
    if (e.isCommand) { d[@"command"] = e.consoleCommand; ver = MAX(ver, 2); } else { d[@"key"] = e.key; if (e.with) d[@"with"] = e.with; }
    if (e.imageEncoded) { d[@"image"] = e.imageEncoded; ver = 3; }
    d[@"label"] = e.label; d[@"shape"] = e.shape; d[@"mode"] = e.mode; [a addObject:d];
  }
  return [NSJSONSerialization dataWithJSONObject:@{@"version": @(ver), @"buttons": a} options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:nil];
}
static int cb_last_slot(NSArray<CBEntry *> *l) { int m = 0; for (CBEntry *e in l) m = MAX(m, e.slot); return m; }
static int cb_next_id(NSArray<CBEntry *> *l) { int m = 0; for (CBEntry *e in l) m = MAX(m, e.ident); return m + 1; }
static int cb_next_slot(NSArray<CBEntry *> *l) { NSMutableSet *s = [NSMutableSet new]; for (CBEntry *e in l) [s addObject:@(e.slot)]; int i = 0; while ([s containsObject:@(i)]) i++; return i; }
static void cb_slot_rect(int i, int last, float out[4]) {
  if (i >= 13) {
    int mx = MAX(i, last); int cols = MAX(4, (int)ceil(sqrt(((mx - 12) * 0.26f * 2.2222223f) / 0.35f)));
    float mn = MIN(0.26f / cols, 0.35f / ((((mx - 13) / cols) + 1) * 2.2222223f));
    float sz = MAX(0.04f, MIN(0.055f, 0.84615386f * mn)); int j = i - 13;
    float x = MIN(0.26f - sz, (j % cols) * mn) + 0.42f, y = MIN(0.35f - sz * 2.2222223f, (j / cols) * mn * 2.2222223f) + 0.468f;
    out[0] = x; out[1] = y; out[2] = x + sz; out[3] = y + sz * 16 / 9; return;
  }
  int rowc[3] = {5, 4, 4}, m = MAX(0, i), r = 0;
  while (r < 2 && m >= rowc[r]) { m -= rowc[r]; r++; }
  out[0] = m * 0.059f + 0.405f; out[1] = r * 0.15f + 0.018f; out[2] = out[0] + 0.055f; out[3] = out[1] + 0.098f;
}

#pragma mark - pixels / VTF

static NSData *rgba_from_image(UIImage *img, BOOL fit) {  // 256x256 straight RGBA
  size_t W = 256; NSMutableData *d = [NSMutableData dataWithLength:W * W * 4];
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGContextRef c = CGBitmapContextCreate(d.mutableBytes, W, W, 8, W * 4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
  CGColorSpaceRelease(cs);
  CGSize s = img.size; float k = fit ? 256.0f / MAX(s.width, s.height) : 256.0f / s.width; float w = s.width * k, h = s.height * k;
  UIGraphicsPushContext(c); CGContextTranslateCTM(c, 0, W); CGContextScaleCTM(c, 1, -1);
  [img drawInRect:CGRectMake((256 - w) / 2, (256 - h) / 2, w, h)];
  UIGraphicsPopContext(); CGContextRelease(c);
  uint8_t *p = d.mutableBytes;
  for (size_t i = 0; i < W * W; i++) { uint8_t a = p[i * 4 + 3]; if (a && a < 255) for (int j = 0; j < 3; j++) p[i * 4 + j] = MIN(255, p[i * 4 + j] * 255 / a); }
  return d;
}
static UIImage *image_from_rgba(NSData *rgba) {
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGDataProviderRef pr = CGDataProviderCreateWithCFData((__bridge CFDataRef)rgba);
  CGImageRef im = CGImageCreate(256, 256, 8, 32, 1024, cs, kCGImageAlphaLast | kCGBitmapByteOrder32Big, pr, NULL, NO, kCGRenderingIntentDefault);
  UIImage *u = [UIImage imageWithCGImage:im]; CGImageRelease(im); CGDataProviderRelease(pr); CGColorSpaceRelease(cs); return u;
}
static NSString *encode_rgba(NSData *rgba) {
  uLongf n = compressBound(rgba.length); NSMutableData *z = [NSMutableData dataWithLength:n];
  compress2(z.mutableBytes, &n, rgba.bytes, rgba.length, 6); z.length = n; return [z base64EncodedStringWithOptions:0];
}
static NSData *decode_rgba(NSString *enc) {
  NSData *z = enc ? [[NSData alloc] initWithBase64EncodedString:enc options:0] : nil; if (!z) return nil;
  NSMutableData *d = [NSMutableData dataWithLength:262144]; uLongf n = 262144;
  if (uncompress(d.mutableBytes, &n, z.bytes, z.length) != Z_OK || n != 262144) return nil; return d;
}
static NSData *vtf_rgba8888(NSData *rgba) {  // VtfWriter.rgba8888(256,256)
  NSMutableData *v = [NSMutableData dataWithLength:80]; uint8_t *h = v.mutableBytes;
  memcpy(h, "VTF\0", 4); *(uint32_t *)(h + 4) = 7; *(uint32_t *)(h + 8) = 2; *(uint32_t *)(h + 12) = 80;
  *(uint16_t *)(h + 16) = 256; *(uint16_t *)(h + 18) = 256; *(uint32_t *)(h + 20) = 8972; *(uint16_t *)(h + 24) = 1; *(uint16_t *)(h + 26) = 0;
  float half = 0.5f, one = 1.0f; memcpy(h + 32, &half, 4); memcpy(h + 36, &half, 4); memcpy(h + 40, &half, 4); memcpy(h + 48, &one, 4);
  *(int32_t *)(h + 52) = 0; h[56] = 1; *(int32_t *)(h + 57) = -1; h[61] = 0; h[62] = 0; *(uint16_t *)(h + 63) = 1;
  [v appendData:rgba]; return v;
}
static NSString *touch_vmt(NSString *tex) {
  return [NSString stringWithFormat:@"\"UnlitGeneric\"\n{\n\t\"$basetexture\" \"%@\"\n\t\"$translucent\" \"1\"\n\t\"$ignorez\" \"1\"\n\t\"$vertexcolor\" \"1\"\n\t\"$vertexalpha\" \"1\"\n}\n", tex];
}
static NSString *res_path(NSString *rel) { return [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:rel]; }
// CustomButtonIcons.compose: shape background + label in (170,255,0) at 75%
static NSData *cb_rgba(CBEntry *e) {
  NSData *img = decode_rgba(e.imageEncoded); if (img) return img;
  NSString *shape = [NSString stringWithFormat:@"touch/custom_%@.png", e.shape];
  UIGraphicsImageRendererFormat *f = [UIGraphicsImageRendererFormat preferredFormat]; f.scale = 1; f.opaque = NO;
  UIImage *im = [[[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(256, 256) format:f] imageWithActions:^(UIGraphicsImageRendererContext *ctx) {
    [[UIImage imageWithContentsOfFile:res_path(shape)] drawInRect:CGRectMake(0, 0, 256, 256)];
    UIFont *font = [UIFont systemFontOfSize:143.36 weight:UIFontWeightBlack];
    NSDictionary *at = @{NSFontAttributeName: font, NSForegroundColorAttributeName: [UIColor colorWithRed:170 / 255.0 green:1 blue:0 alpha:0.75]};
    CGRect r = [e.label boundingRectWithSize:CGSizeMake(4000, 4000) options:NSStringDrawingUsesLineFragmentOrigin attributes:at context:nil];
    float k = MIN(1, MIN(r.size.width > 0 ? 179.2 / r.size.width : 1, r.size.height > 0 ? 133.12 / (font.capHeight + 4) : 1));
    if (k < 1) { font = [UIFont systemFontOfSize:143.36 * k weight:UIFontWeightBlack]; at = @{NSFontAttributeName: font, NSForegroundColorAttributeName: at[NSForegroundColorAttributeName]};
      r = [e.label boundingRectWithSize:CGSizeMake(4000, 4000) options:NSStringDrawingUsesLineFragmentOrigin attributes:at context:nil]; }
    [e.label drawAtPoint:CGPointMake(128 - r.size.width / 2, 128 - font.lineHeight / 2) withAttributes:at];
  }];
  return rgba_from_image(im, NO);
}
static NSString *cb_texture_name(CBEntry *e, NSData *vtf) {
  return [NSString stringWithFormat:@"gmodvr_touch_custom/c%d_%08lx", e.ident, (unsigned long)crc32(0, vtf.bytes, (uInt)vtf.length)];
}
static BOOL atomic_write(NSString *path, NSData *d) {
  [NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
  return [d writeToFile:path atomically:YES];
}
NSString *cb_json_path(void) { return [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0] stringByAppendingPathComponent:@"custom_buttons.json"]; }
// CustomButtons.install(user): cfg/gmod_touch_custom.cfg + materials/gmodvr_touch_custom/*
void cb_install(NSString *user) {
  NSMutableArray *list = cb_from_json([NSData dataWithContentsOfFile:cb_json_path()]);
  NSFileManager *fm = NSFileManager.defaultManager;
  NSString *cfg = [user stringByAppendingPathComponent:@"cfg/gmod_touch_custom.cfg"], *mat = [user stringByAppendingPathComponent:@"materials/gmodvr_touch_custom"];
  if (!list.count) { [fm removeItemAtPath:cfg error:nil]; [fm removeItemAtPath:mat error:nil]; return; }
  [fm createDirectoryAtPath:mat withIntermediateDirectories:YES attributes:nil error:nil];
  NSMutableSet *keep = [NSMutableSet new];
  NSMutableString *s = [NSMutableString stringWithString:@"// Generated by the GDroid launcher (Custom buttons) before every launch; edits are lost.\n// custom_* buttons the touch editor saved in touch.cfg keep their place and color.\ntouch_custom_begin\n"];
  int last = cb_last_slot(list);
  for (CBEntry *e in list) {
    NSData *vtf = vtf_rgba8888(cb_rgba(e)); NSString *tex = cb_texture_name(e, vtf), *base = [tex substringFromIndex:20];
    NSString *vp = [mat stringByAppendingPathComponent:[base stringByAppendingString:@".vtf"]];
    if ([[fm attributesOfItemAtPath:vp error:nil] fileSize] != vtf.length) atomic_write(vp, vtf);
    atomic_write([mat stringByAppendingPathComponent:[base stringByAppendingString:@".vmt"]], [touch_vmt(tex) dataUsingEncoding:NSUTF8StringEncoding]);
    [keep addObject:[base stringByAppendingString:@".vtf"]]; [keep addObject:[base stringByAppendingString:@".vmt"]];
    float r[4]; cb_slot_rect(e.slot, last, r);
    [s appendFormat:@"touch_custom_button \"%@\" \"%@\" \"%@\" %.3f %.3f %.3f %.3f 255 255 255 155 0 1\n", e.buttonName, tex, cb_command(e), r[0], r[1], r[2], r[3]];
  }
  for (NSString *f in [fm contentsOfDirectoryAtPath:mat error:nil]) if (![keep containsObject:f]) [fm removeItemAtPath:[mat stringByAppendingPathComponent:f] error:nil];
  [s appendString:@"touch_custom_end\n"];
  atomic_write(cfg, [s dataUsingEncoding:NSUTF8StringEncoding]);
}

#pragma mark - artwork

#import <CommonCrypto/CommonDigest.h>
static NSString *icon_name(NSData *rgba) {
  uint8_t h[32]; CC_SHA256(rgba.bytes, (CC_LONG)rgba.length, h);
  NSMutableString *s = [NSMutableString stringWithString:@"gmodvr_touch_icons/"]; for (int i = 0; i < 32; i++) [s appendFormat:@"%02x", h[i]]; return s;
}
static UIImage *stock_icon(NSString *tex) {  // vgui/touch/foo -> bundle touch/foo.png
  if (![tex hasPrefix:@"vgui/touch/"]) return nil;
  return [UIImage imageWithContentsOfFile:res_path([NSString stringWithFormat:@"touch/%@.png", [tex substringFromIndex:11]])];
}
static UIImage *neutral(UIImage *im) {
  if (!im) return nil; NSData *d = rgba_from_image(im, NO); uint8_t *p = (uint8_t *)d.bytes;
  for (int i = 0; i < 65536; i++) { uint8_t m = MAX(p[i * 4], MAX(p[i * 4 + 1], p[i * 4 + 2])); p[i * 4] = p[i * 4 + 1] = p[i * 4 + 2] = m; }
  return image_from_rgba(d);
}
static UIImage *tint(UIImage *im, UIColor *c) {
  UIGraphicsImageRendererFormat *f = [UIGraphicsImageRendererFormat preferredFormat]; f.scale = im.scale;
  return [[[UIGraphicsImageRenderer alloc] initWithSize:im.size format:f] imageWithActions:^(UIGraphicsImageRendererContext *x) {
    CGRect r = {CGPointZero, im.size}; [im drawInRect:r];
    [c setFill]; UIRectFillUsingBlendMode(r, kCGBlendModeMultiply);
    [im drawInRect:r blendMode:kCGBlendModeDestinationIn alpha:1]; }];
}

#pragma mark - editor
void cb_present(UIViewController *from, void (^done)(void));

@class TLCanvas;
@interface TLEditor : UIViewController <UIColorPickerViewControllerDelegate, UIDocumentPickerDelegate, UIImagePickerControllerDelegate, UINavigationControllerDelegate>
@property(nonatomic, strong) TLLayout *layout; @property(nonatomic, strong) TLButton *selected;
@property(nonatomic, strong) NSMutableArray<CBEntry *> *custom; @property(nonatomic) BOOL customChanged, changed, moreOpen, showZones, showHidden, inGame;
@property(nonatomic, copy) NSString *user, *profile, *file, *original, *defaults;
@property(nonatomic, strong) NSMutableDictionary<NSString *, UIImage *> *icons;      // name or texture -> image
@property(nonatomic, strong) NSMutableDictionary<NSString *, NSData *> *uploaded;    // gmodvr_touch_icons/<sha> -> rgba
@property(nonatomic, strong) NSCache *tintCache;
@property(nonatomic, strong) TLCanvas *canvas; @property(nonatomic, strong) UIStackView *insp;
@property(nonatomic, strong) UIButton *picker, *colorBtn, *imgBtn, *restoreBtn, *closeImgBtn, *closeRestoreBtn;
@property(nonatomic, strong) NSArray<UISlider *> *sliders; @property(nonatomic, strong) NSArray<UILabel *> *sliderLabels;
@property(nonatomic, strong) UISwitch *visSw, *moreSw, *snapSw; @property(nonatomic, strong) UIButton *rmBtn;
@property(nonatomic) BOOL updating, imageForClose; @property(nonatomic, copy) NSString *pendingExport;
@property(nonatomic, copy) void (^onClose)(BOOL saved);
- (void)select:(TLButton *)b; - (void)edited;
@end

@interface TLCanvas : UIView
@property(nonatomic, weak) TLEditor *ed; @property(nonatomic) CGRect screen;
@property(nonatomic) BOOL resizing, moved; @property(nonatomic) CGPoint down, drag, finger; @property(nonatomic, strong) NSArray *menu;
@end
@implementation TLCanvas
- (CGRect)bounds:(TLButton *)b { CGRect s = self.screen; return CGRectMake(s.origin.x + b.x1 * s.size.width, s.origin.y + b.y1 * s.size.height, b.w * s.size.width, b.h * s.size.height); }
- (BOOL)dragging { return self.moved && self.ed.selected; }
- (BOOL)menuOpen { return self.ed.moreOpen && self.ed.layout.moreAnchor; }
- (void)arrange { self.menu = [self.ed.layout arrangeMore:self.moved && !self.resizing ? self.ed.selected : nil]; }
- (BOOL)stick:(TLButton *)b { return [[NSUserDefaults.standardUserDefaults objectForKey:@"touch_show_joystick"] ?: @YES boolValue] && [b.command isEqual:@"_move"] && !b.hidden; }
- (BOOL)stickOnly:(TLButton *)b { return [self stick:b] && b != self.ed.selected && !self.ed.showZones; }
- (BOOL)showing:(TLButton *)b {
  if (b == self.ed.selected) return YES;
  if (b.inMore) return self.menuOpen && [self.menu containsObject:b];
  if (self.menuOpen && [self.ed.layout coveredByMore:b list:self.menu]) return NO;
  if ([self stick:b]) return YES;
  return (!b.hidden || self.ed.showHidden) && (!b.isZone || self.ed.showZones);
}
- (CGRect)stickBounds:(CGRect)r {
  CGRect s = self.screen; float W = s.size.width, H = s.size.height, mn = MIN(W, H);
  float l = r.origin.x - s.origin.x, t = r.origin.y - s.origin.y, rr = CGRectGetMaxX(r) - s.origin.x, bb = CGRectGetMaxY(r) - s.origin.y;
  float cx = (l + rr) / 2, a = cx < W / 2 ? MIN(l + 0.36f * mn, cx) : MAX(rr - 0.36f * mn, cx);
  float b = MAX(bb - mn * 0.38f, (t + bb) / 2), rad = MIN(0.15f * mn, MIN(rr - l, bb - t) * 0.48f);
  return CGRectMake(s.origin.x + a - rad, s.origin.y + b - rad, 2 * rad, 2 * rad);
}
- (UIImage *)art:(TLButton *)b {
  UIImage *im = self.ed.icons[b.name] ?: self.ed.icons[b.texture];
  if (self.menuOpen && b.moreButton && self.ed.icons[self.ed.layout.moreCloseTexture]) im = self.ed.icons[self.ed.layout.moreCloseTexture];
  if (!im) return nil;
  NSString *k = [NSString stringWithFormat:@"%p-%d-%d%d%d", im, (b.flags & 4096) != 0, b.r, b.g, b.b];
  UIImage *c = [self.ed.tintCache objectForKey:k];
  if (!c) { c = tint((b.flags & 4096) ? neutral(im) : im, b.color); [self.ed.tintCache setObject:c forKey:k]; }
  return c;
}
- (void)drawStick:(TLButton *)b rect:(CGRect)r {
  CGRect sb = [self stickBounds:r]; float kn = sb.size.width / 2 * 0.362f;
  UIImage *base = self.ed.icons[@"vgui/touch/joystick_base"], *knob = self.ed.icons[@"vgui/touch/joystick_knob"];
  if (base) [base drawInRect:sb]; else { [[UIColor colorWithWhite:0 alpha:0.55] setFill]; [[UIBezierPath bezierPathWithOvalInRect:sb] fill]; }
  CGRect kr = CGRectMake(CGRectGetMidX(sb) - kn, CGRectGetMidY(sb) - kn, 2 * kn, 2 * kn);
  if (knob) [((b.flags & 4096) ? tint(neutral(knob), b.color) : knob) drawInRect:kr];
  else { [[UIColor colorWithWhite:1 alpha:0.75] setFill]; [[UIBezierPath bezierPathWithOvalInRect:kr] fill]; }
}
- (void)drawRect:(CGRect)rect {
  CGContextRef c = UIGraphicsGetCurrentContext(); float pad = 24, asp = self.ed.layout.screenAspect;
  float w = MIN(MAX(1, self.bounds.size.width - pad), MAX(1, self.bounds.size.height - pad) * asp), h = w / asp;
  self.screen = CGRectMake((self.bounds.size.width - w) / 2, (self.bounds.size.height - h) / 2, w, h);
  [[UIColor colorWithRed:0x18 / 255.0 green:0x25 / 255.0 blue:0x2e / 255.0 alpha:1] setFill]; UIRectFill(self.screen);
  [[UIColor colorWithRed:0x22 / 255.0 green:0x31 / 255.0 blue:0x3c / 255.0 alpha:1] setStroke];
  for (int i = 1; i < 10; i++) { UIBezierPath *p = [UIBezierPath bezierPath]; float x = self.screen.origin.x + w * i / 10, y = self.screen.origin.y + h * i / 10;
    [p moveToPoint:CGPointMake(x, self.screen.origin.y)]; [p addLineToPoint:CGPointMake(x, CGRectGetMaxY(self.screen))];
    [p moveToPoint:CGPointMake(self.screen.origin.x, y)]; [p addLineToPoint:CGPointMake(CGRectGetMaxX(self.screen), y)]; p.lineWidth = 1; [p stroke]; }
  CGContextSaveGState(c); UIRectClip(self.screen); [self arrange];
  TLEditor *ed = self.ed;
  for (int pass = 0; pass < 2; pass++) for (TLButton *b in ed.layout.buttons) {
    if (![self showing:b] || (pass == 0) != b.isZone) continue;
    CGRect r = [self bounds:b]; UIImage *im = b.isZone ? nil : [self art:b];
    if (im) [im drawInRect:r blendMode:kCGBlendModeNormal alpha:b.a / 255.0 * (b.hidden ? 0.4 : 1)];
    else if ([self stickOnly:b]) [self drawStick:b rect:r];
    else {
      [[UIColor colorWithRed:b.r / 255.0 green:b.g / 255.0 blue:b.b / 255.0 alpha:(b.isZone ? 32 : 112) / 255.0] setFill]; UIRectFill(r);
      if ([self stick:b]) [self drawStick:b rect:r];
      [b.color setStroke]; UIRectFrame(r);
      UIFont *f = [UIFont boldSystemFontOfSize:b.isZone ? 13 : 10]; NSString *t = b.label;
      CGSize ts = [t sizeWithAttributes:@{NSFontAttributeName: f}];
      [t drawAtPoint:CGPointMake(CGRectGetMidX(r) - ts.width / 2, CGRectGetMidY(r) - ts.height / 2) withAttributes:@{NSFontAttributeName: f, NSForegroundColorAttributeName: b.color}];
    }
    if (b == ed.selected) {
      [UIColor.systemOrangeColor set]; UIBezierPath *p = [UIBezierPath bezierPathWithRect:r]; p.lineWidth = 2; [p stroke];
      UIRectFill(CGRectMake(CGRectGetMaxX(r) - 6, CGRectGetMaxY(r) - 6, 12, 12));
    }
  }
  if (self.menuOpen) { CGRect a = [ed.layout moreArea:self.menu];
    if (!CGRectIsNull(a)) { [UIColor.systemOrangeColor setStroke]; UIRectFrame(CGRectMake(self.screen.origin.x + a.origin.x * w, self.screen.origin.y + a.origin.y * h, a.size.width * w, a.size.height * h)); } }
  if (self.dragging && !self.resizing && [ed.layout overMore:ed.selected x:self.finger.x y:self.finger.y]) {
    [[UIColor.systemOrangeColor colorWithAlphaComponent:0.45] setFill]; UIRectFill([self bounds:ed.layout.moreAnchor]); }
  CGContextRestoreGState(c);
  [[UIColor colorWithWhite:0.35 alpha:1] setStroke]; UIRectFrame(self.screen);
}
- (void)touchesBegan:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)ev {
  CGPoint p = [ts.anyObject locationInView:self]; TLEditor *ed = self.ed;
  self.moved = NO; self.resizing = NO; self.down = p; [self arrange];
  if (ed.selected && !ed.selected.inMore) { CGRect r = [self bounds:ed.selected];
    self.resizing = fabs(p.x - CGRectGetMaxX(r)) < 18 && fabs(p.y - CGRectGetMaxY(r)) < 18; }
  TLButton *hit = self.resizing ? ed.selected : nil;
  for (int pass = 0; pass < 2 && !hit; pass++)
    for (TLButton *b in ed.layout.buttons.reverseObjectEnumerator) {
      if (b.isZone != (pass == 1) || ![self showing:b]) continue;
      CGRect r = [self stickOnly:b] ? [self stickBounds:[self bounds:b]] : [self bounds:b];
      if (CGRectContainsPoint(r, p)) { hit = b; break; } }
  if (!hit) { self.down = CGPointMake(-1, -1); return; }
  [ed select:hit];
  self.drag = CGPointMake((p.x - self.screen.origin.x) / self.screen.size.width - hit.x1, (p.y - self.screen.origin.y) / self.screen.size.height - hit.y1);
}
- (void)touchesMoved:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)ev {
  TLEditor *ed = self.ed; if (self.down.x < 0 || !ed.selected) return;
  CGPoint p = [ts.anyObject locationInView:self];
  if (!self.moved && hypot(p.x - self.down.x, p.y - self.down.y) > 8) self.moved = YES;
  if (!self.moved) return;
  float x = (p.x - self.screen.origin.x) / self.screen.size.width, y = (p.y - self.screen.origin.y) / self.screen.size.height; self.finger = CGPointMake(x, y);
  TLButton *s = ed.selected;
  if (self.resizing) [ed.layout rect:s x:s.x1 y:s.y1 w:x - s.x1 h:s.isZone ? y - s.y1 : (x - s.x1) * ed.layout.screenAspect * s.aspect];
  else [ed.layout rect:s x:x - self.drag.x y:y - self.drag.y w:s.w h:s.h];
  [ed edited];
}
- (void)touchesEnded:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)ev {
  TLEditor *ed = self.ed; if (self.down.x < 0) return;
  BOOL drop = self.moved && !self.resizing, tap = !self.moved; self.moved = NO; self.down = CGPointMake(-1, -1);
  if (ed.selected && tap && ed.selected.moreButton) ed.moreOpen = !ed.moreOpen;
  if (ed.selected && drop) { [ed.layout dropMore:ed.selected x:self.finger.x y:self.finger.y open:ed.moreOpen]; if (ed.selected.inMore) ed.moreOpen = YES; [ed edited]; }
  [self setNeedsDisplay];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)ev { self.moved = NO; self.down = CGPointMake(-1, -1); [self setNeedsDisplay]; }
@end

@implementation TLEditor
- (UILabel *)lab:(NSString *)t size:(float)s { UILabel *l = [UILabel new]; l.text = t; l.font = [UIFont systemFontOfSize:s]; l.textColor = [UIColor colorWithWhite:0.6 alpha:1]; l.numberOfLines = 0; return l; }
- (UIButton *)btn:(NSString *)t sel:(SEL)s {
  UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem]; [b setTitle:t forState:0]; b.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightSemibold];
  b.backgroundColor = [UIColor colorWithWhite:0.18 alpha:1]; b.layer.cornerRadius = 8; [b.heightAnchor constraintEqualToConstant:34].active = YES;
  b.titleLabel.adjustsFontSizeToFitWidth = YES; b.contentEdgeInsets = UIEdgeInsetsMake(0, 6, 0, 6);
  if (s) [b addTarget:self action:s forControlEvents:UIControlEventTouchUpInside]; return b;
}
- (UISwitch *)sw:(NSString *)t sel:(SEL)s {
  UISwitch *w = [UISwitch new]; [w addTarget:self action:s forControlEvents:UIControlEventValueChanged];
  UIStackView *r = [[UIStackView alloc] initWithArrangedSubviews:@[[self lab:t size:13], w]]; r.spacing = 6; ((UILabel *)r.arrangedSubviews[0]).textColor = UIColor.whiteColor;
  [self.insp addArrangedSubview:r]; return w;
}
- (void)error:(NSString *)m { UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Control layout" message:m preferredStyle:1];
  [a addAction:[UIAlertAction actionWithTitle:@"OK" style:0 handler:nil]]; [self presentViewController:a animated:YES completion:nil]; }
- (NSString *)text:(NSString *)p { return [NSString stringWithContentsOfFile:p encoding:NSUTF8StringEncoding error:nil] ?: @""; }
- (float)aspect { CGSize s = UIScreen.mainScreen.bounds.size; return MAX(s.width, s.height) / MAX(1, MIN(s.width, s.height)); }

- (BOOL)load:(NSString **)err {
  self.profile = tl_profile_file([self text:[self.user stringByAppendingPathComponent:@"cfg/config.cfg"]]);
  self.file = [self.user stringByAppendingPathComponent:[@"cfg/" stringByAppendingString:self.profile]];
  self.original = [self text:self.file];
  self.defaults = [self text:res_path(@"touch/touch_default.cfg")];
  NSString *src = [self.original stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].length ? self.original : self.defaults;
  self.layout = [TLLayout parse:src aspect:self.aspect error:err];
  if (!self.layout) return NO;
  self.custom = cb_from_json([NSData dataWithContentsOfFile:cb_json_path()]);
  self.icons = [NSMutableDictionary new]; self.uploaded = [NSMutableDictionary new]; self.tintCache = [NSCache new];
  [self mergeCustom];
  for (TLButton *b in self.layout.buttons) {   // uploaded artwork already installed in materials/
    if (![b.texture hasPrefix:@"gmodvr_touch_icons/"]) continue;
    NSData *v = [NSData dataWithContentsOfFile:[self.user stringByAppendingFormat:@"/materials/%@.vtf", b.texture]];
    if (v.length == 80 + 262144) { NSData *px = [v subdataWithRange:NSMakeRange(80, 262144)]; self.uploaded[b.texture] = px; }
  }
  if ([self.layout.moreCloseTexture hasPrefix:@"gmodvr_touch_icons/"]) {
    NSData *v = [NSData dataWithContentsOfFile:[self.user stringByAppendingFormat:@"/materials/%@.vtf", self.layout.moreCloseTexture]];
    if (v.length == 80 + 262144) self.uploaded[self.layout.moreCloseTexture] = [v subdataWithRange:NSMakeRange(80, 262144)]; }
  [self reloadArt];
  if ([NSUserDefaults.standardUserDefaults objectForKey:@"touch_show_joystick"] ? [NSUserDefaults.standardUserDefaults boolForKey:@"touch_show_joystick"] : YES) [self sprintForJoystick];
  return YES;
}
- (void)sprintForJoystick {  // TouchLayout.placeSprintForJoystick(true)
  TLButton *s = [self.layout find:@"speed"]; if (!s || [s.command caseInsensitiveCompare:@"+speed"] || s.hidden || s.inMore) return;
  if (!(fabsf(s.x1 - 0.72f) < 0.002f && fabsf(s.y1 - 0.589f) < 0.002f && fabsf(s.x2 - 0.8f) < 0.002f)) return;
  float f = 0.1f, f2 = 0.755556f, f3 = 0.18f, hh = (s.h * (f3 - f)) / s.w + f2;
  for (TLButton *b in self.layout.buttons) if (b != s && !b.isZone && !b.hidden && !b.inMore && b.x1 < f3 - 0.001f && b.x2 > f + 0.001f && b.y1 < hh - 0.001f && b.y2 > f2 + 0.001f) return;
  s.x1 = f; s.y1 = f2; s.x2 = f3; s.y2 = hh;
}
- (void)mergeCustom {
  NSMutableSet *names = [NSMutableSet new]; int last = cb_last_slot(self.custom);
  for (CBEntry *e in self.custom) {
    [names addObject:e.buttonName]; NSData *rgba = cb_rgba(e); NSString *tex = cb_texture_name(e, vtf_rgba8888(rgba));
    TLButton *b = [self.layout find:e.buttonName];
    if (!b) { b = [TLButton new]; b.name = e.buttonName; b.a = 155; b.texture = tex; b.command = cb_command(e);
      float r[4]; cb_slot_rect(e.slot, last, r); [self.layout rect:b x:r[0] y:r[1] w:r[2] - r[0] h:(r[2] - r[0]) * self.layout.screenAspect];
      [self.layout.buttons addObject:b]; [self.layout.lines addObject:b]; }
    b.command = cb_command(e); b.texture = tex; self.icons[b.name] = image_from_rgba(rgba);
  }
  for (TLButton *b in [self.layout.buttons copy]) if ([b.name hasPrefix:@"custom_"] && ![names containsObject:b.name]) { [self.layout.buttons removeObject:b]; [self.layout.lines removeObject:b]; }
}
- (void)reloadArt {
  NSMutableArray *texs = [NSMutableArray arrayWithObjects:@"vgui/touch/joystick_base", @"vgui/touch/joystick_knob", @"vgui/touch/sb_xg", self.layout.moreCloseTexture, nil];
  for (TLButton *b in self.layout.buttons) if (!b.isZone) [texs addObject:b.texture];
  for (NSString *t in texs) {
    if (self.icons[t]) continue;
    UIImage *im = self.uploaded[t] ? image_from_rgba(self.uploaded[t]) : stock_icon(t); if (im) self.icons[t] = im; }
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor colorWithWhite:0.07 alpha:1]; self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  UIStackView *top = [UIStackView new]; top.spacing = 8; top.translatesAutoresizingMaskIntoConstraints = NO;
  UILabel *title = [UILabel new]; title.text = @"Control layout"; title.font = [UIFont boldSystemFontOfSize:18]; title.textColor = UIColor.whiteColor;
  [title setContentHuggingPriority:1 forAxis:0];
  for (UIView *v in @[[self btn:@"Cancel" sel:@selector(cancel)], title, [self btn:@"Custom buttons" sel:@selector(openCustom)], [self btn:@"Import" sel:@selector(importControls)],
                      [self btn:@"Export" sel:@selector(exportControls)], [self btn:@"Reset" sel:@selector(reset)], [self btn:@"Save" sel:@selector(save)]]) [top addArrangedSubview:v];
  ((UIButton *)top.arrangedSubviews.lastObject).backgroundColor = UIColor.systemOrangeColor; [(UIButton *)top.arrangedSubviews.lastObject setTintColor:UIColor.whiteColor];
  [self.view addSubview:top];
  self.canvas = [TLCanvas new]; self.canvas.ed = self; self.canvas.backgroundColor = UIColor.clearColor; self.canvas.contentMode = UIViewContentModeRedraw;
  self.canvas.translatesAutoresizingMaskIntoConstraints = NO; [self.view addSubview:self.canvas];
  UIScrollView *sc = [UIScrollView new]; sc.translatesAutoresizingMaskIntoConstraints = NO; [self.view addSubview:sc];
  self.insp = [UIStackView new]; self.insp.axis = UILayoutConstraintAxisVertical; self.insp.spacing = 6; self.insp.translatesAutoresizingMaskIntoConstraints = NO;
  [sc addSubview:self.insp];
  UILayoutGuide *g = self.view.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [top.topAnchor constraintEqualToAnchor:g.topAnchor constant:6], [top.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:8], [top.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-8],
    [self.canvas.topAnchor constraintEqualToAnchor:top.bottomAnchor constant:4], [self.canvas.leadingAnchor constraintEqualToAnchor:g.leadingAnchor],
    [self.canvas.bottomAnchor constraintEqualToAnchor:g.bottomAnchor], [sc.leadingAnchor constraintEqualToAnchor:self.canvas.trailingAnchor],
    [sc.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-6], [sc.topAnchor constraintEqualToAnchor:self.canvas.topAnchor], [sc.bottomAnchor constraintEqualToAnchor:g.bottomAnchor],
    [sc.widthAnchor constraintEqualToConstant:250],
    [self.insp.topAnchor constraintEqualToAnchor:sc.contentLayoutGuide.topAnchor], [self.insp.bottomAnchor constraintEqualToAnchor:sc.contentLayoutGuide.bottomAnchor constant:-20],
    [self.insp.leadingAnchor constraintEqualToAnchor:sc.contentLayoutGuide.leadingAnchor], [self.insp.trailingAnchor constraintEqualToAnchor:sc.contentLayoutGuide.trailingAnchor],
    [self.insp.widthAnchor constraintEqualToAnchor:sc.frameLayoutGuide.widthAnchor]]];
  NSString *err = nil;
  if (![self load:&err]) { UILabel *l = [self lab:[@"Could not open the layout: " stringByAppendingString:err ?: @"?"] size:14]; [self.insp addArrangedSubview:l]; return; }
  [self buildInspector];
  TLButton *first = nil; for (TLButton *b in self.layout.buttons) if (!b.isZone && !b.hidden && !b.inMore) { first = b; break; }
  [self select:first ?: self.layout.buttons[0]];
}
- (void)buildInspector {
  self.picker = [self btn:@"" sel:nil]; self.picker.showsMenuAsPrimaryAction = YES; [self.insp addArrangedSubview:self.picker];
  self.rmBtn = [self btn:@"Hide it" sel:@selector(toggleRemove)]; [self.insp addArrangedSubview:self.rmBtn];
  self.colorBtn = [self btn:@"Color…" sel:@selector(pickColor)]; [self.insp addArrangedSubview:self.colorBtn];
  [self.insp addArrangedSubview:[self btn:@"Apply color to all buttons" sel:@selector(colorAll)]];
  self.imgBtn = [self btn:@"Custom icon" sel:@selector(chooseImage)]; self.restoreBtn = [self btn:@"Default icon" sel:@selector(restoreImage)];
  self.closeImgBtn = [self btn:@"Custom X icon" sel:@selector(chooseCloseImage)]; self.closeRestoreBtn = [self btn:@"Default X icon" sel:@selector(restoreCloseImage)];
  for (UIView *v in @[self.imgBtn, self.restoreBtn, self.closeImgBtn, self.closeRestoreBtn]) [self.insp addArrangedSubview:v];
  NSMutableArray *sl = [NSMutableArray new], *ll = [NSMutableArray new];
  for (int i = 0; i < 4; i++) {
    UILabel *l = [self lab:@"" size:12]; [self.insp addArrangedSubview:l]; [ll addObject:l];
    UISlider *s = [UISlider new]; s.minimumValue = 0; s.maximumValue = 1; s.tag = i; s.tintColor = UIColor.systemOrangeColor;
    [s addTarget:self action:@selector(slid:) forControlEvents:UIControlEventValueChanged]; [self.insp addArrangedSubview:s]; [sl addObject:s];
  }
  self.sliders = sl; self.sliderLabels = ll;
  self.visSw = [self sw:@"Show button" sel:@selector(visChanged:)];
  self.moreSw = [self sw:@"Inside the ... menu" sel:@selector(moreChanged:)];
  [self.insp addArrangedSubview:[self lab:@"Tap ... to open or close the menu. Drop a button on ... to add it or drag one out of the open menu to take it out of the menu o algo" size:11]];
  self.snapSw = [self sw:@"Snap to grid" sel:@selector(snapChanged:)]; self.snapSw.on = self.layout.snap;
  [self sw:@"Show move / look zones" sel:@selector(zonesChanged:)];
  [self sw:@"Show removed buttons" sel:@selector(hiddenChanged:)];
  [self.insp addArrangedSubview:[self lab:@"Removed buttons stay in this list (they marked \"removed\") so you can put them back if you did bad" size:11]];
  [self.insp addArrangedSubview:[self lab:[NSString stringWithFormat:@"Drag a button to move it, or onto ... to put it in the burger menu. Drag its corner to resize. %@",
    self.inGame ? @"Save, then the game reloads the controls o algo" : @"Changes apply the next time the game starts."] size:11]];
  [self rebuildPicker];
}
- (void)rebuildPicker {
  NSMutableArray *acts = [NSMutableArray new];
  for (TLButton *b in self.layout.buttons) {
    CBEntry *ce = nil; for (CBEntry *e in self.custom) if ([e.buttonName isEqual:b.name]) ce = e;
    NSString *t = ce ? [@"Custom: " stringByAppendingString:ce.label] : b.label; if (b.inMore) t = [t stringByAppendingString:@" (⋯ menu)"]; if (b.hidden) t = [t stringByAppendingString:@" (removed)"];
    __weak TLEditor *ws = self;
    [acts addObject:[UIAction actionWithTitle:t image:nil identifier:nil handler:^(UIAction *a) { if (b.moreButton) ws.moreOpen = YES; [ws select:b]; }]];
  }
  self.picker.menu = [UIMenu menuWithTitle:@"Select a control" children:acts];
}
- (void)select:(TLButton *)b {
  if (!b) return;
  BOOL ch = b != self.selected; self.selected = b;
  if (ch) [self reveal];
  if (!self.sliders) return;
  self.updating = YES;
  CBEntry *ce = nil; for (CBEntry *e in self.custom) if ([e.buttonName isEqual:b.name]) ce = e;
  [self.picker setTitle:[(ce ? [@"Custom: " stringByAppendingString:ce.label] : b.label) stringByAppendingString:@"  ▾"] forState:0];
  float v[4] = {b.x1, b.y1, b.w, b.h}; NSArray *n = @[@"X", @"Y", @"Width", @"Height"];
  for (int i = 0; i < 4; i++) { self.sliders[i].value = v[i]; self.sliders[i].enabled = !b.inMore;
    self.sliderLabels[i].text = [NSString stringWithFormat:@"%@: %.1f%%", n[i], v[i] * 100]; }
  [self.rmBtn setTitle:b.hidden ? @"Bring it back" : @"Hide it" forState:0];
  self.rmBtn.tintColor = b.hidden ? UIColor.systemGreenColor : UIColor.systemRedColor;
  self.visSw.on = !b.hidden; self.moreSw.on = b.inMore; self.moreSw.enabled = b.canBeInMore;
  self.colorBtn.tintColor = ([b.command isEqual:@"_move"] && !(b.flags & 4096)) ? [UIColor colorWithRed:0xaa / 255.0 green:0 blue:0 alpha:1] : b.color;
  self.imgBtn.enabled = !b.isZone; [self.imgBtn setTitle:b.moreButton ? @"Custom ... icon" : @"Custom icon" forState:0];
  [self.restoreBtn setTitle:b.moreButton ? @"Default ... icon" : @"Default icon" forState:0];
  TLButton *stock = [[TLLayout parse:self.defaults aspect:self.layout.screenAspect error:nil] find:b.name];
  self.restoreBtn.enabled = !b.isZone && (ce ? ce.imageEncoded != nil : (stock && ![b.texture isEqual:stock.texture]));
  self.closeImgBtn.hidden = self.closeRestoreBtn.hidden = !b.moreButton;
  self.closeRestoreBtn.enabled = ![self.layout.moreCloseTexture isEqual:@"vgui/touch/sb_xg"];
  self.updating = NO;
  [self.canvas setNeedsDisplay];
}
- (void)reveal {
  if (!self.selected) return;
  if (self.selected.inMore) self.moreOpen = YES;
  else if (self.moreOpen && [self.layout coveredByMore:self.selected list:[self.layout arrangeMore:nil]]) self.moreOpen = NO;
}
- (void)edited { self.changed = YES; [self select:self.selected]; }
- (void)slid:(UISlider *)s {
  if (self.updating || !self.selected) return; TLButton *b = self.selected; float v = s.value;
  [self.layout rect:b x:s.tag == 0 ? v : b.x1 y:s.tag == 1 ? v : b.y1 w:s.tag == 2 ? v : b.w h:s.tag == 3 ? v : b.h]; [self edited];
}
- (void)toggleRemove { if (!self.selected) return; self.selected.flags ^= 1; [self rebuildPicker]; [self edited]; }
- (void)visChanged:(UISwitch *)s { if (self.updating || !self.selected) return; self.selected.flags = s.on ? self.selected.flags & ~1 : self.selected.flags | 1; [self rebuildPicker]; [self edited]; }
- (void)moreChanged:(UISwitch *)s { if (self.updating || !self.selected || ![self.layout setInMore:self.selected on:s.on]) return; [self reveal]; [self rebuildPicker]; [self edited]; }
- (void)snapChanged:(UISwitch *)s { self.layout.snap = s.on; if (s.on) for (TLButton *b in self.layout.buttons) [self.layout rect:b x:b.x1 y:b.y1 w:b.w h:b.h]; [self edited]; }
- (void)zonesChanged:(UISwitch *)s { self.showZones = s.on; [self.canvas setNeedsDisplay]; }
- (void)hiddenChanged:(UISwitch *)s { self.showHidden = s.on; [self.canvas setNeedsDisplay]; }
- (void)pickColor {
  if (!self.selected) return;
  UIColorPickerViewController *p = [UIColorPickerViewController new]; p.supportsAlpha = NO; p.selectedColor = self.selected.color; p.delegate = self;
  [self presentViewController:p animated:YES completion:nil];
}
- (void)colorPickerViewControllerDidSelectColor:(UIColorPickerViewController *)vc {
  CGFloat r, g, b, a; [vc.selectedColor getRed:&r green:&g blue:&b alpha:&a];
  TLButton *s = self.selected; s.r = (int)lround(MAX(0, MIN(1, r)) * 255); s.g = (int)lround(MAX(0, MIN(1, g)) * 255); s.b = (int)lround(MAX(0, MIN(1, b)) * 255); s.flags |= 4096; [self edited];
}
- (void)colorAll {
  TLButton *s = self.selected; if (!s) return;
  for (TLButton *b in self.layout.buttons) if (!b.isZone || [b.command isEqual:@"_move"]) { b.r = s.r; b.g = s.g; b.b = s.b; b.flags |= 4096; }
  [self edited];
}
- (void)chooseImage { if (!self.selected || self.selected.isZone) return; self.imageForClose = NO; [self pickPhoto]; }
- (void)chooseCloseImage { if (!self.selected.moreButton) return; self.imageForClose = YES; [self pickPhoto]; }
- (void)pickPhoto {
  UIImagePickerController *p = [UIImagePickerController new]; p.sourceType = UIImagePickerControllerSourceTypePhotoLibrary; p.delegate = self;
  [self presentViewController:p animated:YES completion:nil];
}
- (void)imagePickerController:(UIImagePickerController *)p didFinishPickingMediaWithInfo:(NSDictionary *)info {
  UIImage *im = info[UIImagePickerControllerOriginalImage]; [p dismissViewControllerAnimated:YES completion:nil];
  if (!im) return; NSData *rgba = rgba_from_image(im, YES);
  if (self.imageForClose) { [self setCloseImage:rgba]; return; }
  [self setImage:rgba for:self.selected];
}
- (void)imagePickerControllerDidCancel:(UIImagePickerController *)p { [p dismissViewControllerAnimated:YES completion:nil]; }
- (void)setCloseImage:(NSData *)rgba {
  if (rgba) { NSString *n = icon_name(rgba); self.layout.moreCloseTexture = n; self.uploaded[n] = rgba; self.icons[n] = image_from_rgba(rgba); }
  else self.layout.moreCloseTexture = @"vgui/touch/sb_xg";
  self.moreOpen = YES; [self reloadArt]; [self edited];
}
- (void)restoreCloseImage { [self setCloseImage:nil]; }
- (void)restoreImage { if (self.selected && !self.selected.isZone) [self setImage:nil for:self.selected]; }
- (void)setImage:(NSData *)rgba for:(TLButton *)b {
  for (CBEntry *e in self.custom) if ([e.buttonName isEqual:b.name]) {
    e.imageEncoded = rgba ? encode_rgba(rgba) : nil; NSData *px = cb_rgba(e);
    b.texture = cb_texture_name(e, vtf_rgba8888(px)); self.icons[b.name] = image_from_rgba(px); self.customChanged = YES; [self edited]; return; }
  if (!rgba) { TLButton *st = [[TLLayout parse:self.defaults aspect:self.layout.screenAspect error:nil] find:b.name]; if (!st) return; b.texture = st.texture; }
  else { NSString *n = icon_name(rgba); self.uploaded[n] = rgba; self.icons[n] = image_from_rgba(rgba); b.texture = n; }
  [self reloadArt]; [self edited];
}
- (void)reset {
  UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Reset everything?" message:@"Buttons go back to default spots, sizes and colors. Your custom buttons stay. Hit Save to apply." preferredStyle:1];
  [a addAction:[UIAlertAction actionWithTitle:@"Reset" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *x) {
    TLLayout *d = [TLLayout parse:self.defaults aspect:self.layout.screenAspect error:nil]; if (!d) return;
    self.layout.moreCloseTexture = d.moreCloseTexture;
    [self.layout.lines filterUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(id o, NSDictionary *b) { return ![o isKindOfClass:TLButton.class]; }]];
    [self.layout.buttons removeAllObjects];
    for (TLButton *b in d.buttons) { [self.layout rect:b x:b.x1 y:b.y1 w:b.w h:b.h]; [self.layout.buttons addObject:b]; [self.layout.lines addObject:b]; }
    [self mergeCustom]; [self reloadArt]; [self rebuildPicker]; self.selected = nil;
    TLButton *f = nil; for (TLButton *b in self.layout.buttons) if (!b.isZone && !b.hidden && !b.inMore) { f = b; break; }
    [self select:f]; self.changed = YES; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:1 handler:nil]]; [self presentViewController:a animated:YES completion:nil];
}
- (void)save {
  if ([self text:self.file].length != self.original.length || ![[self text:self.file] isEqual:self.original]) { [self error:@"Layout changed somewhere else, reopen this o algo"]; return; }
  NSMutableSet *used = [NSMutableSet setWithObject:self.layout.moreCloseTexture]; for (TLButton *b in self.layout.buttons) [used addObject:b.texture];
  for (NSString *n in self.uploaded) if ([used containsObject:n]) {
    atomic_write([self.user stringByAppendingFormat:@"/materials/%@.vtf", n], vtf_rgba8888(self.uploaded[n]));
    atomic_write([self.user stringByAppendingFormat:@"/materials/%@.vmt", n], [touch_vmt(n) dataUsingEncoding:NSUTF8StringEncoding]); }
  if (self.customChanged) atomic_write(cb_json_path(), cb_to_json(self.custom));
  if (!atomic_write(self.file, [[self.layout cfg:self.profile] dataUsingEncoding:NSUTF8StringEncoding])) { [self error:@"Couldn't save, idk why"]; return; }
  cb_install(self.user);
  self.changed = NO; [self close:YES];
}
- (void)cancel {
  if (!self.changed && !self.customChanged) { [self close:NO]; return; }
  UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Discard layout changes?" message:nil preferredStyle:1];
  [a addAction:[UIAlertAction actionWithTitle:@"Discard" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *x) { [self close:NO]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Keep editing" style:1 handler:nil]]; [self presentViewController:a animated:YES completion:nil];
}
- (void)close:(BOOL)saved { void (^cb)(BOOL) = self.onClose; [self dismissViewControllerAnimated:YES completion:^{ if (cb) cb(saved); }]; }

// export / import: GDroid "gmodvr-control-layout" JSON
- (void)exportControls {
  NSMutableDictionary *imgs = [NSMutableDictionary new]; NSMutableSet *used = [NSMutableSet setWithObject:self.layout.moreCloseTexture];
  for (TLButton *b in self.layout.buttons) [used addObject:b.texture];
  for (NSString *n in self.uploaded) if ([used containsObject:n]) imgs[n] = encode_rgba(self.uploaded[n]);
  NSMutableDictionary *j = [@{@"format": @"gmodvr-control-layout", @"version": @(imgs.count ? 2 : 1), @"layout": [self.layout cfg:@"touch.cfg"],
    @"customButtons": [NSJSONSerialization JSONObjectWithData:cb_to_json(self.custom) options:0 error:nil]} mutableCopy];
  if (imgs.count) j[@"images"] = imgs;
  NSString *p = [NSTemporaryDirectory() stringByAppendingPathComponent:@"gmod-controls.json"];
  [[NSJSONSerialization dataWithJSONObject:j options:NSJSONWritingPrettyPrinted error:nil] writeToFile:p atomically:YES];
  UIDocumentPickerViewController *d = [[UIDocumentPickerViewController alloc] initForExportingURLs:@[[NSURL fileURLWithPath:p]] asCopy:YES];
  [self presentViewController:d animated:YES completion:nil];
}
- (void)importControls {
  UIDocumentPickerViewController *d = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeJSON, UTTypePlainText, UTTypeData] asCopy:YES];
  d.delegate = self; [self presentViewController:d animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController *)c didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
  NSData *d = [NSData dataWithContentsOfURL:urls.firstObject]; if (!d) return;
  NSString *s = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding]; if (!s) { [self error:@"That's not a layout file"]; return; }
  if ([s hasPrefix:@"\ufeff"]) s = [s substringFromIndex:1];
  NSString *layoutText = s; NSMutableArray *custom = nil; NSDictionary *imgs = nil;
  if ([[s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] hasPrefix:@"{"]) {
    NSDictionary *j = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
    if (![j isKindOfClass:NSDictionary.class] || ![j[@"format"] isEqual:@"gmodvr-control-layout"]) { [self error:@"That's not a layout file"]; return; }
    layoutText = j[@"layout"]; imgs = j[@"images"];
    custom = cb_from_json([NSJSONSerialization dataWithJSONObject:j[@"customButtons"] ?: @{} options:0 error:nil]);
  }
  NSString *err = nil; TLLayout *L = [TLLayout parse:layoutText aspect:self.layout.screenAspect error:&err];
  if (!L) { [self error:err]; return; }
  for (NSString *n in imgs) { NSData *px = decode_rgba(imgs[n]); if (px && [icon_name(px) isEqual:n]) self.uploaded[n] = px; }
  self.layout = L; if (custom) { self.custom = custom; self.customChanged = YES; }
  [self.icons removeAllObjects]; [self mergeCustom]; [self reloadArt]; [self rebuildPicker];
  self.selected = nil; TLButton *f = nil; for (TLButton *b in L.buttons) if (!b.isZone && !b.hidden && !b.inMore) { f = b; break; }
  self.snapSw.on = L.snap; [self select:f ?: L.buttons[0]]; self.changed = YES;
}
- (void)openCustom {
  
  if (self.changed) { [self error:@"Save or cancel first"]; return; }
  __weak TLEditor *ws = self;
  cb_present(self, ^{ ws.custom = cb_from_json([NSData dataWithContentsOfFile:cb_json_path()]); [ws.icons removeAllObjects]; [ws mergeCustom]; [ws reloadArt]; [ws rebuildPicker]; [ws select:ws.selected]; });
}
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (BOOL)prefersStatusBarHidden { return YES; }
@end

void touch_editor_present(UIViewController *from, NSString *user, BOOL inGame, void (^done)(BOOL saved)) {
  TLEditor *e = [TLEditor new]; e.user = user; e.inGame = inGame; e.onClose = done;
  e.modalPresentationStyle = UIModalPresentationFullScreen;
  [from presentViewController:e animated:YES completion:nil];
}

#pragma mark - custom buttons screen (CustomButtonsActivity)

@interface CBList : UITableViewController <UIImagePickerControllerDelegate, UINavigationControllerDelegate>
@property(nonatomic, strong) NSMutableArray<CBEntry *> *entries; @property(nonatomic, copy) void (^done)(void);
@property(nonatomic, strong) CBEntry *imageFor;
@end
@implementation CBList
- (void)viewDidLoad {
  [super viewDidLoad]; self.title = @"Custom buttons";
  self.entries = cb_from_json([NSData dataWithContentsOfFile:cb_json_path()]);
  self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemClose target:self action:@selector(close)];
  self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAdd target:self action:@selector(add)];
}
- (void)close { void (^d)(void) = self.done; [self dismissViewControllerAnimated:YES completion:^{ if (d) d(); }]; }
- (void)persist { atomic_write(cb_json_path(), cb_to_json(self.entries)); [self.tableView reloadData]; }
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { return MAX(1, (NSInteger)self.entries.count); }
- (NSString *)tableView:(UITableView *)t titleForFooterInSection:(NSInteger)s {
  return @"Touch buttons that press a key or run a console command for addons swipe to remove it, press to edit Changes apply the next time the game starts, usable in the layout editor.";
}
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)ip {
  UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
  if (!self.entries.count) { c.textLabel.text = @"No custom buttons yet"; c.textLabel.numberOfLines = 0; c.textLabel.textColor = UIColor.secondaryLabelColor; return c; }
  CBEntry *e = self.entries[ip.row];
  c.imageView.image = [UIImage imageWithCGImage:image_from_rgba(cb_rgba(e)).CGImage scale:256 / 40.0 orientation:UIImageOrientationUp];
  c.textLabel.text = e.label;
  NSString *what = e.isCommand ? e.consoleCommand : [NSString stringWithFormat:@"%@%@", e.with ? [cb_key(e.with)[3] stringByAppendingString:@" + "] : @"", cb_key(e.key)[3] ?: e.key];
  c.detailTextLabel.text = [NSString stringWithFormat:@"%@ · %@", what, cb_toggle_effective(e) ? @"Toggle" : (e.isCommand ? @"Runs on tap" : @"Hold")];
  c.detailTextLabel.textColor = UIColor.secondaryLabelColor; c.accessoryType = UITableViewCellAccessoryDisclosureIndicator; return c;
}
- (BOOL)tableView:(UITableView *)t canEditRowAtIndexPath:(NSIndexPath *)ip { return self.entries.count > 0; }
- (void)tableView:(UITableView *)t commitEditingStyle:(UITableViewCellEditingStyle)st forRowAtIndexPath:(NSIndexPath *)ip {
  if (st == UITableViewCellEditingStyleDelete) { [self.entries removeObjectAtIndex:ip.row]; [self persist]; }
}
- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)ip { [t deselectRowAtIndexPath:ip animated:YES]; if (self.entries.count) [self edit:self.entries[ip.row]]; }
- (void)add {
  UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Add a button" message:@"What should it do?" preferredStyle:UIAlertControllerStyleActionSheet];
  [a addAction:[UIAlertAction actionWithTitle:@"Press a key" style:0 handler:^(UIAlertAction *x) {
    CBEntry *e = [CBEntry new]; e.ident = cb_next_id(self.entries); e.slot = cb_next_slot(self.entries); e.key = @"KEY_G"; e.shape = @"circle"; e.mode = @"hold"; e.label = @"";
    [self edit:e]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Console command" style:0 handler:^(UIAlertAction *x) {
    CBEntry *e = [CBEntry new]; e.ident = cb_next_id(self.entries); e.slot = cb_next_slot(self.entries); e.consoleCommand = @""; e.shape = @"circle"; e.mode = @"hold"; e.label = @"";
    [self edit:e]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:1 handler:nil]];
  a.popoverPresentationController.barButtonItem = self.navigationItem.rightBarButtonItem;
  [self presentViewController:a animated:YES completion:nil];
}
- (void)edit:(CBEntry *)orig {
  CBEntry *e = [CBEntry new]; e.ident = orig.ident; e.slot = orig.slot; e.key = orig.key; e.with = orig.with; e.consoleCommand = orig.consoleCommand;
  e.label = orig.label; e.shape = orig.shape; e.mode = orig.mode; e.imageEncoded = orig.imageEncoded;
  [self edit2:e orig:orig];
}
- (void)edit2:(CBEntry *)e orig:(CBEntry *)orig {
  BOOL isNew = ![self.entries containsObject:orig];
  UIAlertController *a = [UIAlertController alertControllerWithTitle:isNew ? @"Add a button" : @"Change button" message:nil preferredStyle:1];
  if (e.isCommand) [a addTextFieldWithConfigurationHandler:^(UITextField *f) { f.placeholder = @"Console command (separate with ;)"; f.text = e.consoleCommand;
    f.autocapitalizationType = UITextAutocapitalizationTypeNone; f.autocorrectionType = UITextAutocorrectionTypeNo; }];
  [a addTextFieldWithConfigurationHandler:^(UITextField *f) { f.placeholder = @"1 to 4 characters (blank = auto)"; f.text = e.label; }];
  __weak UIAlertController *wa = a;
  void (^collect)(void) = ^{ NSArray *tf = wa.textFields; if (e.isCommand) { e.consoleCommand = [tf[0] text]; e.label = [tf[1] text]; } else e.label = [tf[0] text]; };
  if (!e.isCommand) {
    [a addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Key: %@", cb_key(e.key)[3]] style:0 handler:^(UIAlertAction *x) { collect(); [self pickKey:e with:NO then:^{ [self edit2:e orig:orig]; }]; }]];
    [a addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Together with: %@", e.with ? cb_key(e.with)[3] : @"Nothing"] style:0 handler:^(UIAlertAction *x) { collect(); [self pickKey:e with:YES then:^{ [self edit2:e orig:orig]; }]; }]];
  }
  NSDictionary *shapes = @{@"circle": @"Circle", @"square": @"Rounded", @"sharp": @"Square"};
  [a addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Shape: %@", shapes[e.shape]] style:0 handler:^(UIAlertAction *x) { collect();
    NSArray *o = @[@"circle", @"square", @"sharp"]; e.shape = o[([o indexOfObject:e.shape] + 1) % 3]; [self edit2:e orig:orig]; }]];
  NSString *mode = [e.mode isEqual:@"toggle"] ? (e.isCommand ? @"Toggle: tap to hold the + commands, tap again to let go" : @"Toggle: tap to press, tap again to release")
                                              : (e.isCommand ? @"Runs on tap (+ commands held while held)" : @"Held while you hold the button");
  [a addAction:[UIAlertAction actionWithTitle:mode style:0 handler:^(UIAlertAction *x) { collect(); e.mode = [e.mode isEqual:@"toggle"] ? @"hold" : @"toggle"; [self edit2:e orig:orig]; }]];
  [a addAction:[UIAlertAction actionWithTitle:e.imageEncoded ? @"Replace image…" : @"Choose image…" style:0 handler:^(UIAlertAction *x) { collect(); self.imageFor = e; objc_setAssociatedObject(e, "orig", orig, OBJC_ASSOCIATION_RETAIN);
    UIImagePickerController *p = [UIImagePickerController new]; p.delegate = self; [self presentViewController:p animated:YES completion:nil]; }]];
  if (e.imageEncoded) [a addAction:[UIAlertAction actionWithTitle:@"Use preset artwork" style:0 handler:^(UIAlertAction *x) { collect(); e.imageEncoded = nil; [self edit2:e orig:orig]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
    collect();
    if (e.isCommand && [e.mode isEqual:@"toggle"] && !cb_can_toggle(e.consoleCommand)) e.mode = @"hold";
    NSString *err = cb_validate(e);
    if (err) { UIAlertController *m = [UIAlertController alertControllerWithTitle:@"Couldn't save, idk why" message:err preferredStyle:1];
      [m addAction:[UIAlertAction actionWithTitle:@"OK" style:0 handler:^(UIAlertAction *y) { [self edit2:e orig:orig]; }]]; [self presentViewController:m animated:YES completion:nil]; return; }
    NSUInteger i = [self.entries indexOfObject:orig]; if (i == NSNotFound) [self.entries addObject:e]; else self.entries[i] = e;
    [self persist]; }]];
  if (!isNew) [a addAction:[UIAlertAction actionWithTitle:@"Remove" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *x) { [self.entries removeObject:orig]; [self persist]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:1 handler:nil]];
  [self presentViewController:a animated:YES completion:nil];
}
- (void)pickKey:(CBEntry *)e with:(BOOL)with then:(void (^)(void))then {
  UIAlertController *g = [UIAlertController alertControllerWithTitle:with ? @"Combo key (held first)" : @"The key is" message:nil preferredStyle:UIAlertControllerStyleActionSheet];
  if (with) [g addAction:[UIAlertAction actionWithTitle:@"No combo" style:0 handler:^(UIAlertAction *x) { e.with = nil; then(); }]];
  NSMutableArray *groups = [NSMutableArray new]; for (NSArray *k in cb_keys()) if (![groups containsObject:k[2]]) [groups addObject:k[2]];
  for (NSString *gr in groups) [g addAction:[UIAlertAction actionWithTitle:gr style:0 handler:^(UIAlertAction *x) {
    UIAlertController *kk = [UIAlertController alertControllerWithTitle:gr message:nil preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSArray *k in cb_keys()) if ([k[2] isEqual:gr]) [kk addAction:[UIAlertAction actionWithTitle:k[3] style:0 handler:^(UIAlertAction *y) { if (with) e.with = k[0]; else e.key = k[0]; then(); }]];
    [kk addAction:[UIAlertAction actionWithTitle:@"Cancel" style:1 handler:^(UIAlertAction *y) { then(); }]];
    kk.popoverPresentationController.sourceView = self.view; kk.popoverPresentationController.sourceRect = CGRectMake(self.view.bounds.size.width / 2, 60, 1, 1);
    [self presentViewController:kk animated:YES completion:nil]; }]];
  [g addAction:[UIAlertAction actionWithTitle:@"Cancel" style:1 handler:^(UIAlertAction *x) { then(); }]];
  g.popoverPresentationController.sourceView = self.view; g.popoverPresentationController.sourceRect = CGRectMake(self.view.bounds.size.width / 2, 60, 1, 1);
  [self presentViewController:g animated:YES completion:nil];
}
- (void)imagePickerController:(UIImagePickerController *)p didFinishPickingMediaWithInfo:(NSDictionary *)info {
  UIImage *im = info[UIImagePickerControllerOriginalImage]; CBEntry *e = self.imageFor; CBEntry *orig = objc_getAssociatedObject(e, "orig");
  [p dismissViewControllerAnimated:YES completion:^{ if (im) e.imageEncoded = encode_rgba(rgba_from_image(im, YES)); [self edit2:e orig:orig]; }];
}
- (void)imagePickerControllerDidCancel:(UIImagePickerController *)p {
  CBEntry *e = self.imageFor; CBEntry *orig = objc_getAssociatedObject(e, "orig");
  [p dismissViewControllerAnimated:YES completion:^{ [self edit2:e orig:orig]; }];
}
@end

void cb_present(UIViewController *from, void (^done)(void)) {
  CBList *l = [[CBList alloc] initWithStyle:UITableViewStyleInsetGrouped]; l.done = done;
  UINavigationController *n = [[UINavigationController alloc] initWithRootViewController:l];
  n.overrideUserInterfaceStyle = UIUserInterfaceStyleDark; n.modalPresentationStyle = UIModalPresentationFormSheet;
  [from presentViewController:n animated:YES completion:nil];
}

#pragma mark - in-game hook (GameActivity.openTouchControls)

#include "../shim/jni_fake.h"
#include "../loader/elf_loader.h"
static void notify_changed(void) {
  so_module *m = so_find("liblauncher.so");
  void (*f)(void *, void *) = m ? (void (*)(void *, void *))so_sym(m, "Java_com_gmodvr_flat_GameActivity_nativeTouchControlsChanged") : NULL;
  fprintf(stderr, "[touch] controls changed -> native %p\n", (void *)f);
  if (f) f(jni_env(), NULL);
}
static UIViewController *top_vc(void) {
  UIViewController *v = nil;
  for (UIScene *s in UIApplication.sharedApplication.connectedScenes) if ([s isKindOfClass:UIWindowScene.class])
    for (UIWindow *w in ((UIWindowScene *)s).windows) if (w.isKeyWindow || !v) v = w.rootViewController;
  while (v.presentedViewController) v = v.presentedViewController;
  return v;
}
static jvalue j_openTouchControls(void *env, void *thiz, jvalue *a) {
  (void)env; (void)thiz; jvalue r = {0};
  const char *s = a ? jni_str(a[0].l) : NULL; NSString *arg = s ? @(s) : @"";
  BOOL custom = [[arg componentsSeparatedByString:@"\n"].firstObject isEqual:@"custom"];
  const char *wr = getenv("SOURCEVR_WRITE_GAME_PATH"); NSString *user = wr ? @(wr) : nil;
  fprintf(stderr, "[touch] openTouchControls '%s'\n", s ? s : "");
  dispatch_async(dispatch_get_main_queue(), ^{
    UIViewController *vc = top_vc(); if (!vc || !user) return;
    extern void SDL_StopTextInput(void); SDL_StopTextInput();
    if (custom) cb_present(vc, ^{ cb_install(user); notify_changed(); });
    else touch_editor_present(vc, user, YES, ^(BOOL saved) { if (saved) notify_changed(); });
  });
  return r;
}
static jni_impl_entry g_touch[] = {{0, "openTouchControls", 0, j_openTouchControls}};
void touchedit_jni_init(void) { jni_add_impls(g_touch, 1); }
