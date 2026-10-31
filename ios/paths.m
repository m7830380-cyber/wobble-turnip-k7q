#import <Foundation/Foundation.h>
char *ios_bundle_path(void) { return strdup([[NSBundle mainBundle].bundlePath UTF8String]); }
char *ios_documents_path(void) {
  NSString *d = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
  return strdup([d UTF8String]);
}
#include <SDL.h>
extern int SDL_main(int, char **);
#undef main
int main(int argc, char **argv) { return SDL_UIKitRunApp(argc, argv, SDL_main); }
