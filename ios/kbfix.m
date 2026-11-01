// In-game keyboard fixes: SDL shifts the whole game view up (black screen) when the keyboard opens,
// and the iOS keyboard has no dismiss key. Keep the game in place and add a "Hide keyboard" bar.
#import <UIKit/UIKit.h>
#include <SDL.h>

static __weak UIResponder *g_fr;
@interface UIResponder (GMFR) - (void)gm_findFR:(id)s; @end
@implementation UIResponder (GMFR) - (void)gm_findFR:(id)s { g_fr = self; } @end

@interface GMKb : NSObject @end
@implementation GMKb
+ (void)hide {
  SDL_StopTextInput();
  [UIApplication.sharedApplication sendAction:@selector(resignFirstResponder) to:nil from:nil forEvent:nil];
}
+ (void)unshift {
  for (UIWindow *w in UIApplication.sharedApplication.windows) {
    UIView *v = w.rootViewController.view;  // SDL's updateKeyboard moves view.frame up by the keyboard overlap
    if (v && v.frame.origin.y != 0) { CGRect f = v.frame; f.origin = CGPointZero; v.frame = f; }
  }
}
+ (void)willShow:(NSNotification *)n {
  dispatch_async(dispatch_get_main_queue(), ^{ [self unshift]; });
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ [self unshift]; });
  g_fr = nil;
  [UIApplication.sharedApplication sendAction:@selector(gm_findFR:) to:nil from:nil forEvent:nil];
  UIResponder *fr = g_fr;
  if (![fr isKindOfClass:UITextField.class]) return;
  UITextField *tf = (UITextField *)fr;
  if (tf.inputAccessoryView) return;
  UIToolbar *bar = [[UIToolbar alloc] initWithFrame:CGRectMake(0, 0, 320, 44)];
  bar.barStyle = UIBarStyleBlack;
  bar.items = @[[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace target:nil action:nil],
                [[UIBarButtonItem alloc] initWithTitle:@"Hide keyboard" style:UIBarButtonItemStyleDone target:self action:@selector(hide)]];
  [bar sizeToFit];
  tf.inputAccessoryView = bar;
  [tf reloadInputViews];
}
+ (void)didShow:(NSNotification *)n { [self unshift]; }
@end

void ios_kbfix_install(void) {
  { SDL_Rect r = {0, 0, 1, 1}; SDL_SetTextInputRect(&r); }  // input rect at the top -> SDL never needs to shift
  NSNotificationCenter *c = NSNotificationCenter.defaultCenter;
  [c addObserver:GMKb.class selector:@selector(willShow:) name:UIKeyboardWillShowNotification object:nil];
  [c addObserver:GMKb.class selector:@selector(didShow:) name:UIKeyboardDidShowNotification object:nil];
  [c addObserver:GMKb.class selector:@selector(didShow:) name:UIKeyboardDidChangeFrameNotification object:nil];
}
