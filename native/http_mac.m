#import "mac_platform.h"

extern void mb_post(const char *, const char *, size_t, const uint8_t *, size_t);

#define HTTP_MOST (4 * 1024 * 1024)

static void tell(int32_t id, long status, NSString *why, NSData *body) {
  NSDictionary *head = @{@"do" : @"http", @"id" : @(id), @"status" : @(status), @"err" : why};
  NSData *json = [NSJSONSerialization dataWithJSONObject:head options:0 error:nil];
  mb_post("ps", json.bytes, json.length, body.bytes, body.length);
}

static NSString *whyOf(NSError *e) {
  if (![e.domain isEqualToString:NSURLErrorDomain]) return @"network";
  switch (e.code) {
    case NSURLErrorTimedOut: return @"timeout";
    case NSURLErrorCannotFindHost:
    case NSURLErrorDNSLookupFailed: return @"dns";
    case NSURLErrorCannotConnectToHost:
    case NSURLErrorNetworkConnectionLost:
    case NSURLErrorNotConnectedToInternet: return @"connect";
    case NSURLErrorSecureConnectionFailed:
    case NSURLErrorServerCertificateUntrusted:
    case NSURLErrorServerCertificateHasBadDate:
    case NSURLErrorServerCertificateHasUnknownRoot:
    case NSURLErrorServerCertificateNotYetValid: return @"tls";
    default: return @"network";
  }
}

int32_t ps_http(int32_t id, moonbit_bytes_t verb, moonbit_bytes_t url, moonbit_bytes_t headers, moonbit_bytes_t body,
                int32_t timeout_ms) {
  NSURL *u = [NSURL URLWithString:[NSString stringWithUTF8String:(const char *)url]];
  if (!u) return 0;
  NSString *scheme = u.scheme.lowercaseString;
  BOOL local = [u.host isEqualToString:@"127.0.0.1"] || [u.host isEqualToString:@"localhost"];
  if (!([scheme isEqualToString:@"https"] || ([scheme isEqualToString:@"http"] && local))) return 0;
  NSMutableURLRequest *r = [NSMutableURLRequest requestWithURL:u];
  r.HTTPMethod = [NSString stringWithUTF8String:(const char *)verb];
  r.timeoutInterval = (timeout_ms > 0 ? timeout_ms : 15000) / 1000.0;
  r.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
  NSString *hs = [NSString stringWithUTF8String:(const char *)headers];
  for (NSString *line in [hs componentsSeparatedByString:@"\r\n"]) {
    NSRange colon = [line rangeOfString:@":"];
    if (colon.location == NSNotFound) continue;
    NSString *k = [[line substringToIndex:colon.location] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    NSString *v =
        [[line substringFromIndex:colon.location + 1] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if (k.length) [r setValue:v forHTTPHeaderField:k];
  }
  int32_t bn = (int32_t)Moonbit_array_length(body);
  if (bn > 0) r.HTTPBody = [NSData dataWithBytes:body length:bn];
  NSURLSessionConfiguration *c = NSURLSessionConfiguration.ephemeralSessionConfiguration;
  c.timeoutIntervalForRequest = r.timeoutInterval;
  c.timeoutIntervalForResource = r.timeoutInterval * 2;
  NSURLSession *s = [NSURLSession sessionWithConfiguration:c];
  NSURLSessionDataTask *t = [s dataTaskWithRequest:r
                                 completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
                                   if (err) {
                                     tell(id, 0, whyOf(err), nil);
                                   } else if (data.length > HTTP_MOST) {
                                     tell(id, 0, @"large", nil);
                                   } else {
                                     long status = [resp isKindOfClass:NSHTTPURLResponse.class]
                                                       ? (long)((NSHTTPURLResponse *)resp).statusCode
                                                       : 0;
                                     tell(id, status, status ? @"" : @"network", data);
                                   }
                                   [s finishTasksAndInvalidate];
                                 }];
  [t resume];
  return 1;
}
