#import <UIKit/UIKit.h>
@interface AD : UIResponder <UIApplicationDelegate>
@property UIWindow *window;
@end
@implementation AD
- (BOOL)application:(UIApplication *)a didFinishLaunchingWithOptions:(NSDictionary *)o {
  self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
  UIViewController *vc = [UIViewController new];
  vc.view.backgroundColor = UIColor.systemGreenColor;
  UILabel *l = [[UILabel alloc] initWithFrame:CGRectMake(60, 60, 700, 80)];
  l.text = @HELLO_TEXT; l.font = [UIFont boldSystemFontOfSize:36];
  [vc.view addSubview:l];
  self.window.rootViewController = vc; [self.window makeKeyAndVisible];
  return YES;
}
@end
int main(int c, char **v) { @autoreleasepool { return UIApplicationMain(c, v, nil, @"AD"); } }
