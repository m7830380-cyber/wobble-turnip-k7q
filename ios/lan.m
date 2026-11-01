// LAN server discovery (Source A2S_INFO). Broadcast + unicast sweep of the local /24..
// (iOS blocks broadcast without the multicast entitlement, so the sweep is what normally finds servers.)
#import <Foundation/Foundation.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

NSString *lan_my_ip(void) {
  struct ifaddrs *ifa = NULL, *i; NSString *r = nil;
  if (getifaddrs(&ifa)) return nil;
  for (i = ifa; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
    char b[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, b, sizeof b);
    NSString *n = @(i->ifa_name), *s = @(b);
    if ([n isEqual:@"en0"]) { r = s; break; }                 // Wi-Fi
    if (!r && ([n hasPrefix:@"bridge"] || [n hasPrefix:@"en"])) r = s;  // personal hotspot / other
  }
  freeifaddrs(ifa); return r;
}

static NSString *rstr(const uint8_t *p, int n, int *o) {
  int s = *o; while (*o < n && p[*o]) (*o)++;
  NSString *r = [[NSString alloc] initWithBytes:p + s length:*o - s encoding:NSUTF8StringEncoding] ?: @"?";
  if (*o < n) (*o)++; return r;
}

static const uint8_t Q[] = "\xFF\xFF\xFF\xFFTSource Engine Query";  // + NUL from the literal

void lan_scan(void (^done)(NSArray<NSDictionary *> *servers, NSString *err)) {
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    NSMutableArray *out = [NSMutableArray new]; NSMutableSet *seen = [NSMutableSet new];
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { dispatch_async(dispatch_get_main_queue(), ^{ done(out, @"socket failed"); }); return; }
    int one = 1; setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    int big = 1 << 20; setsockopt(s, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    fcntl(s, F_SETFL, O_NONBLOCK);
    NSString *me = lan_my_ip();
    struct sockaddr_in a = {0}; a.sin_family = AF_INET;
    uint32_t mine = me ? ntohl(inet_addr(me.UTF8String)) : 0;
    void (^sendq)(uint32_t, int, const uint8_t *, size_t) = ^(uint32_t ip, int port, const uint8_t *q, size_t ql) {
      struct sockaddr_in d = a; d.sin_addr.s_addr = htonl(ip); d.sin_port = htons(port);
      sendto(s, q, ql, 0, (struct sockaddr *)&d, sizeof d);
    };
    void (^drain)(int) = ^(int ms) {
      struct pollfd pf = {s, POLLIN, 0};
      while (poll(&pf, 1, ms) > 0) {
        uint8_t b[1400]; struct sockaddr_in f; socklen_t fl = sizeof f;
        ssize_t n = recvfrom(s, b, sizeof b, 0, (struct sockaddr *)&f, &fl);
        if (n < 6 || *(int32_t *)b != -1) continue;
        if (b[4] == 0x41 && n >= 9) {  // challenge -> resend with it appended
          uint8_t q2[sizeof Q + 4]; memcpy(q2, Q, sizeof Q); memcpy(q2 + sizeof Q, b + 5, 4);
          sendto(s, q2, sizeof q2, 0, (struct sockaddr *)&f, fl); continue;
        }
        if (b[4] != 0x49) continue;
        int o = 6;
        NSString *name = rstr(b, (int)n, &o), *map = rstr(b, (int)n, &o), *folder = rstr(b, (int)n, &o), *game = rstr(b, (int)n, &o);
        o += 2; int pl = o < n ? b[o] : 0, mx = o + 1 < n ? b[o + 1] : 0, bots = o + 2 < n ? b[o + 2] : 0;
        o += 3; o += 2; int pw = o < n ? b[o] : 0; o += 2;  // type env | vis vac
        NSString *ver = rstr(b, (int)n, &o);
        int port = ntohs(f.sin_port);
        if (o < n && (b[o] & 0x80) && o + 2 < n) port = b[o + 1] | b[o + 2] << 8;
        char ipb[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &f.sin_addr, ipb, sizeof ipb);
        NSString *addr = [NSString stringWithFormat:@"%s:%d", ipb, port];
        if ([seen containsObject:addr]) continue; [seen addObject:addr];
        [out addObject:@{@"addr": addr, @"name": name, @"map": map, @"folder": folder, @"game": game,
                         @"players": @(pl - bots), @"max": @(mx), @"pw": @(pw), @"ver": ver}];
      }
    };
    for (int pass = 0; pass < 2; pass++) {
      for (int p = 27015; p <= 27020; p++) {
        sendq(0xFFFFFFFF, p, Q, sizeof Q);
        if (mine) sendq((mine & 0xFFFFFF00) | 0xFF, p, Q, sizeof Q);
      }
      if (mine)
        for (uint32_t h = 1; h < 255; h++) {
          for (int p = 27015; p <= 27020; p++) sendq((mine & 0xFFFFFF00) | h, p, Q, sizeof Q);
          if (h % 32 == 0) { drain(0); usleep(3000); }
        }
      drain(pass ? 1500 : 600);
    }
    close(s);
    NSString *err = me ? nil : @"Not connected to Wi-Fi.";
    dispatch_async(dispatch_get_main_queue(), ^{ done(out, err); });
  });
}

#import <UIKit/UIKit.h>
@interface GLanList : UITableViewController
@property(nonatomic, strong) NSArray<NSDictionary *> *servers;
@property(nonatomic, copy) void (^join)(NSString *);
@property(nonatomic, copy) NSString *status;
@end
@implementation GLanList
- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = @"LAN games";
  self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemClose target:self action:@selector(close)];
  self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh target:self action:@selector(scan)];
  self.refreshControl = [UIRefreshControl new];
  [self.refreshControl addTarget:self action:@selector(scan) forControlEvents:UIControlEventValueChanged];
  [self scan];
}
- (void)close { [self dismissViewControllerAnimated:YES completion:nil]; }
- (void)scan {
  self.status = @"Searching…"; [self.tableView reloadData];
  self.navigationItem.rightBarButtonItem.enabled = NO;
  lan_scan(^(NSArray *sv, NSString *err) {
    self.servers = sv; self.navigationItem.rightBarButtonItem.enabled = YES; [self.refreshControl endRefreshing];
    NSString *me = lan_my_ip();
    self.status = err ?: (sv.count ? [NSString stringWithFormat:@"Your IP: %@", me] :
      [NSString stringWithFormat:@"No games found. Your IP: %@\nIf you dont see the game you either didnt type sv_lan or didnt allow ios to have local network access, allow it. or youre a dumbo", me]);
    [self.tableView reloadData];
  });
}
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { return self.servers.count + 1; }
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)ip {
  UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
  c.textLabel.numberOfLines = 0; c.detailTextLabel.numberOfLines = 0;
  if (ip.row == (NSInteger)self.servers.count) {
    c.textLabel.text = @"Enter IP manually…"; c.textLabel.textColor = c.tintColor;
    c.detailTextLabel.text = self.status; c.detailTextLabel.textColor = UIColor.secondaryLabelColor;
    return c;
  }
  NSDictionary *d = self.servers[ip.row];
  c.textLabel.text = [NSString stringWithFormat:@"%@%@", [d[@"pw"] intValue] ? @"🔒 " : @"", d[@"name"]];
  c.detailTextLabel.text = [NSString stringWithFormat:@"%@  ·  %@/%@ players  ·  %@\n%@", d[@"map"], d[@"players"], d[@"max"], d[@"game"], d[@"addr"]];
  c.detailTextLabel.textColor = UIColor.secondaryLabelColor;
  c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  return c;
}
- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)ip {
  [t deselectRowAtIndexPath:ip animated:YES];
  if (ip.row == (NSInteger)self.servers.count) {
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Join by IP" message:nil preferredStyle:1];
    [a addTextFieldWithConfigurationHandler:^(UITextField *f) { f.placeholder = @"192.168.1.10:27015"; f.text = [NSUserDefaults.standardUserDefaults stringForKey:@"last_ip"]; f.keyboardType = UIKeyboardTypeNumbersAndPunctuation; }];
    [a addAction:[UIAlertAction actionWithTitle:@"Connect" style:0 handler:^(UIAlertAction *x) {
      NSString *s = [a.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
      if (s.length) [self go:s]; }]];
    [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:1 handler:nil]];
    [self presentViewController:a animated:YES completion:nil]; return;
  }
  [self go:self.servers[ip.row][@"addr"]];
}
- (void)go:(NSString *)addr {
  [NSUserDefaults.standardUserDefaults setObject:addr forKey:@"last_ip"];
  void (^j)(NSString *) = self.join;
  [self dismissViewControllerAnimated:YES completion:^{ j(addr); }];
}
@end

void lan_present(UIViewController *from, void (^join)(NSString *addr)) {
  GLanList *l = [[GLanList alloc] initWithStyle:UITableViewStyleInsetGrouped]; l.join = join;
  UINavigationController *n = [[UINavigationController alloc] initWithRootViewController:l];
  n.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  [from presentViewController:n animated:YES completion:nil];
}
