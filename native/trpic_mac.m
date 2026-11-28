// The translation drawn into a copy of the picture off the main thread (as trpic_win.cpp): backgrounds from the ring
// round each block, words laid out by Core Text in the block's place, the place of every word kept for selecting.
#import "mac_platform.h"
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>

extern void mb_post(const char *, const char *, size_t, const uint8_t *, size_t);

typedef struct {
  double x, y, w, h, lh;
  int draw;
} TrBlockIn;

static void trFailed(int32_t id, NSString *why) {
  NSData *json = [NSJSONSerialization dataWithJSONObject:@{@"do" : @"tr-pic", @"id" : @(id), @"ok" : @0, @"why" : why}
                                                 options:0
                                                   error:nil];
  mb_post("ps", json.bytes, json.length, NULL, 0);
}

static BOOL trUnspaced(unichar c) {
  return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
         (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xAC00 && c <= 0xD7AF);
}

static BOOL trBlank(unichar c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x3000 || c == 0xA0; }

static int trMedian(NSMutableData *d) {
  NSUInteger n = d.length / sizeof(int);
  if (!n) return 255;
  int *v = (int *)d.mutableBytes;
  qsort_b(v, n, sizeof(int), ^int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; });
  return v[n / 2];
}

static CTFrameRef trFrame(NSString *text, CGFloat size, CGFloat w, int zh, CGColorRef ink, CGFloat *height) {
  CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, (__bridge CFStringRef)(zh ? @"zh-Hans" : @"en"));
  CGFloat lineH = size * 1.3;
  CTParagraphStyleSetting ps[] = {
      {kCTParagraphStyleSpecifierMinimumLineHeight, sizeof lineH, &lineH},
      {kCTParagraphStyleSpecifierMaximumLineHeight, sizeof lineH, &lineH},
  };
  CTParagraphStyleRef para = CTParagraphStyleCreate(ps, 2);
  NSDictionary *attrs = @{
    (id)kCTFontAttributeName : (__bridge id)font,
    (id)kCTParagraphStyleAttributeName : (__bridge id)para,
    (id)kCTForegroundColorAttributeName : (__bridge id)ink,
  };
  NSAttributedString *as = [[NSAttributedString alloc] initWithString:text attributes:attrs];
  CTFramesetterRef fs = CTFramesetterCreateWithAttributedString((__bridge CFAttributedStringRef)as);
  CGSize fit = CTFramesetterSuggestFrameSizeWithConstraints(fs, CFRangeMake(0, 0), NULL, CGSizeMake(w, CGFLOAT_MAX), NULL);
  *height = ceil(fit.height);
  CGPathRef path = CGPathCreateWithRect(CGRectMake(0, 0, w, *height + 2), NULL);
  CTFrameRef frame = CTFramesetterCreateFrame(fs, CFRangeMake(0, 0), path, NULL);
  CGPathRelease(path);
  CFRelease(fs);
  CFRelease(para);
  CFRelease(font);
  return frame;
}

static void trRun(int32_t id, NSData *pic, NSString *spec, int zh) {
  CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)pic, NULL);
  CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
  if (src) CFRelease(src);
  if (!img) {
    trFailed(id, @"decode");
    return;
  }
  size_t W = CGImageGetWidth(img), H = CGImageGetHeight(img);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(NULL, W, H, 8, W * 4, cs, kCGImageAlphaPremultipliedLast);
  CGColorSpaceRelease(cs);
  if (!ctx) {
    CGImageRelease(img);
    trFailed(id, @"draw");
    return;
  }
  CGContextDrawImage(ctx, CGRectMake(0, 0, W, H), img);
  CGImageRelease(img);
  uint8_t *px = CGBitmapContextGetData(ctx);
  size_t stride = CGBitmapContextGetBytesPerRow(ctx);

  NSMutableArray<NSValue *> *rows = [NSMutableArray array];
  NSMutableArray<NSString *> *texts = [NSMutableArray array];
  for (NSString *row in [spec componentsSeparatedByString:@"\n"]) {
    NSArray<NSString *> *f = [row componentsSeparatedByString:@"\t"];
    if (f.count < 7) continue;
    TrBlockIn b = {f[0].doubleValue, f[1].doubleValue, f[2].doubleValue, f[3].doubleValue, f[4].doubleValue, f[5].intValue};
    [rows addObject:[NSValue valueWithBytes:&b objCType:@encode(TrBlockIn)]];
    [texts addObject:[[f subarrayWithRange:NSMakeRange(6, f.count - 6)] componentsJoinedByString:@"\t"]];
  }
  NSUInteger count = rows.count;
  TrBlockIn *blocks = calloc(count ? count : 1, sizeof(TrBlockIn));
  for (NSUInteger i = 0; i < count; i++) [rows[i] getValue:&blocks[i]];

  // (the context's rows run from the picture's top: Core Graphics' y is flipped for drawing below)
  NSMutableString *words = [NSMutableString string];
  int lineN = 0;
  for (NSUInteger bi = 0; bi < count; bi++) {
    TrBlockIn b = blocks[bi];
    NSString *text = texts[bi];
    if (!text.length) continue;
    CGFloat pad = MIN(MAX(b.lh * H * 0.12, 1.0), 6.0);
    CGFloat x = MAX(0.0, b.x * W - pad), y = MAX(0.0, b.y * H - pad);
    CGFloat w = MIN((CGFloat)W - x, b.w * W + pad * 2.0), h = b.h * H + pad * 2.0;
    CGFloat room = (CGFloat)H - y;
    for (NSUInteger j = 0; j < count; j++) {
      if (j == bi) continue;
      CGFloat oy = blocks[j].y * H, ox0 = blocks[j].x * W, ox1 = (blocks[j].x + blocks[j].w) * W;
      if (oy > y + 1.0 && ox1 > x && ox0 < x + w) room = MIN(room, oy - y - 1.0);
    }
    room = MAX(room, h);
    int r = 255, g = 255, bl = 255;
    {
      NSMutableData *rs = [NSMutableData data], *gs = [NSMutableData data], *bs = [NSMutableData data];
      CGFloat rp = MAX(2.0, b.lh * H * 0.25);
      long x0 = MAX(0, (long)(b.x * W - rp)), y0 = MAX(0, (long)(b.y * H - rp));
      long x1 = MIN((long)W - 1, (long)((b.x + b.w) * W + rp)), y1 = MIN((long)H - 1, (long)((b.y + b.h) * H + rp));
      void (^take)(long, long) = ^(long xx, long yy) {
        const uint8_t *p = px + (size_t)yy * stride + (size_t)xx * 4;
        if (p[3] < 128) return;
        int v[3] = {p[0] * 255 / p[3], p[1] * 255 / p[3], p[2] * 255 / p[3]};
        [rs appendBytes:&v[0] length:sizeof(int)];
        [gs appendBytes:&v[1] length:sizeof(int)];
        [bs appendBytes:&v[2] length:sizeof(int)];
      };
      for (long xx = x0; xx <= x1; xx += 2) take(xx, y0), take(xx, y1);
      for (long yy = y0; yy <= y1; yy += 2) take(x0, yy), take(x1, yy);
      if (rs.length) r = trMedian(rs), g = trMedian(gs), bl = trMedian(bs);
    }
    CGFloat lum = (0.299 * r + 0.587 * g + 0.114 * bl) / 255.0;
    CGColorRef ink = lum > 0.55 ? CGColorCreateSRGB(0.08, 0.08, 0.08, 1) : CGColorCreateSRGB(0.96, 0.96, 0.96, 1);
    CGFloat s0 = MIN(MAX(b.lh * H, 7.0), 160.0), size = s0, th = 0;
    CGFloat mostW = (CGFloat)W - x - 1.0;
    CTFrameRef frame = NULL;
    if (!b.draw) {
      frame = trFrame(text, size, w + size, zh, ink, &th);
    } else {
      BOOL fit = NO;
      for (CGFloat s = s0; s >= MAX(7.0, s0 * 0.8); s -= MAX(0.5, s0 * 0.04)) {
        if (frame) CFRelease(frame);
        frame = trFrame(text, s, w, zh, ink, &th);
        size = s;
        if (th <= h + 1.0) {
          fit = YES;
          break;
        }
      }
      if (!fit) {
        for (CGFloat s = MAX(7.0, s0 * 0.8); s >= MAX(7.0, s0 * 0.6); s -= MAX(0.5, s0 * 0.04)) {
          if (frame) CFRelease(frame);
          frame = trFrame(text, s, mostW, zh, ink, &th);
          size = s;
          if (th <= room + 1.0) break;
        }
        CGFloat widest = 0;
        CFArrayRef ls = CTFrameGetLines(frame);
        for (CFIndex k = 0; k < CFArrayGetCount(ls); k++)
          widest = MAX(widest, CTLineGetTypographicBounds((CTLineRef)CFArrayGetValueAtIndex(ls, k), NULL, NULL, NULL));
        w = MAX(w, MIN(mostW, widest + 4.0));
        CFRelease(frame);
        frame = trFrame(text, size, w, zh, ink, &th);
        h = MIN(MAX(h, th + 2.0), room);
      }
    }
    CGFloat ty = y + MAX(0.0, (h - th) / 2.0);
    CFArrayRef lines = CTFrameGetLines(frame);
    CFIndex nl = CFArrayGetCount(lines);
    CGPoint *origins = calloc(nl ? nl : 1, sizeof(CGPoint));
    CTFrameGetLineOrigins(frame, CFRangeMake(0, 0), origins);
    CGFloat frameH = th + 2;
    if (b.draw) {
      CGContextSaveGState(ctx);
      CGRect box = CGRectMake(x, (CGFloat)H - y - h, w, h);
      CGFloat rad = MIN(4.0, size * 0.25);
      CGPathRef rr = CGPathCreateWithRoundedRect(box, rad, rad, NULL);
      CGContextSetRGBFillColor(ctx, r / 255.0, g / 255.0, bl / 255.0, 1);
      CGContextAddPath(ctx, rr);
      CGContextFillPath(ctx);
      CGPathRelease(rr);
      CGContextClipToRect(ctx, box);
      CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
      for (CFIndex k = 0; k < nl; k++) {
        CGContextSetTextPosition(ctx, x + 1.0 + origins[k].x, (CGFloat)H - ty - (frameH - origins[k].y));
        CTLineDraw((CTLineRef)CFArrayGetValueAtIndex(lines, k), ctx);
      }
      CGContextRestoreGState(ctx);
    }
    CGFloat lineH = size * 1.3;
    for (CFIndex k = 0; k < nl; k++) {
      CTLineRef line = (CTLineRef)CFArrayGetValueAtIndex(lines, k);
      CFRange rg = CTLineGetStringRange(line);
      CGFloat top = ty + (frameH - origins[k].y) - lineH * 0.8;
      if (top + lineH > y + h + 1.0) break;
      NSUInteger i = (NSUInteger)rg.location, end = (NSUInteger)(rg.location + rg.length);
      BOOL sp = k > 0 && i > 0 && trBlank([text characterAtIndex:i - 1]);
      while (i < end) {
        unichar c = [text characterAtIndex:i];
        if (trBlank(c)) {
          sp = YES;
          i++;
          continue;
        }
        NSUInteger j = i + 1;
        if (!trUnspaced(c))
          while (j < end && !trBlank([text characterAtIndex:j]) && !trUnspaced([text characterAtIndex:j])) j++;
        CGFloat a = CTLineGetOffsetForStringIndex(line, (CFIndex)i, NULL);
        CGFloat e = CTLineGetOffsetForStringIndex(line, (CFIndex)j, NULL);
        [words appendFormat:@"%d\t%lu\t%d\t%.5f\t%.5f\t%.5f\t%.5f\t%@\n", lineN, (unsigned long)bi, sp ? 1 : 0,
                            (x + 1.0 + origins[k].x + a) / W, top / H, MAX(1.0, e - a) / W, lineH / H,
                            [text substringWithRange:NSMakeRange(i, j - i)]];
        sp = NO;
        i = j;
      }
      lineN++;
    }
    free(origins);
    CFRelease(frame);
    CGColorRelease(ink);
  }
  free(blocks);

  CGImageRef done = CGBitmapContextCreateImage(ctx);
  CGContextRelease(ctx);
  NSMutableData *png = [NSMutableData data];
  CGImageDestinationRef dst =
      CGImageDestinationCreateWithData((__bridge CFMutableDataRef)png, (__bridge CFStringRef) @"public.png", 1, NULL);
  BOOL ok = NO;
  if (dst && done) {
    CGImageDestinationAddImage(dst, done, NULL);
    ok = CGImageDestinationFinalize(dst);
  }
  if (dst) CFRelease(dst);
  if (done) CGImageRelease(done);
  if (!ok) {
    trFailed(id, @"encode");
    return;
  }
  NSData *wd = [words dataUsingEncoding:NSUTF8StringEncoding];
  NSData *wj = [NSJSONSerialization dataWithJSONObject:@{@"do" : @"tr-words", @"id" : @(id)} options:0 error:nil];
  mb_post("ps", wj.bytes, wj.length, wd.bytes, wd.length);
  NSData *json = [NSJSONSerialization
      dataWithJSONObject:@{@"do" : @"tr-pic", @"id" : @(id), @"ok" : @1, @"w" : @(W), @"h" : @(H)}
                 options:0
                   error:nil];
  mb_post("ps", json.bytes, json.length, png.bytes, png.length);
}

int32_t ps_tr_render(int32_t id, moonbit_bytes_t pic, moonbit_bytes_t spec, int32_t zh) {
  int32_t n = (int32_t)Moonbit_array_length(pic);
  if (n <= 0) return 0;
  NSData *data = [NSData dataWithBytes:pic length:n];
  NSString *s = [[NSString alloc] initWithBytes:spec length:Moonbit_array_length(spec) encoding:NSUTF8StringEncoding] ?: @"";
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    @autoreleasepool {
      trRun(id, data, s, zh);
    }
  });
  return 1;
}
