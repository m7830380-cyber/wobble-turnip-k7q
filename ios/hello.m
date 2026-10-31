#import <UIKit/UIKit.h>
@interface AD : UIResponder <UIApplicationDelegate>
@property UIWindow *window;
@end
@implementation AD
- (BOOL)application:(UIApplication *)a didFinishLaunchingWithOptions:(NSDictionary *)o {
  self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
  UIViewController *vc = [UIViewController new];
  vc.view.backgroundColor = UIColor.systemGreenColor;
  UILabel *l = [[UILabel alloc] initWithFrame:CGRectMake(60, 60, 700, 60)];
  NSString *d = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
  [@"hello ok" writeToFile:[d stringByAppendingPathComponent:@"hello.txt"] atomically:YES encoding:4 error:nil];
  l.text = @"HELLO WORKS"; l.font = [UIFont boldSystemFontOfSize:30];
  [vc.view addSubview:l];
  self.window.rootViewController = vc; [self.window makeKeyAndVisible];
  return YES;
}
@end
int main(int c, char **v) { @autoreleasepool { return UIApplicationMain(c, v, nil, @"AD"); } }
