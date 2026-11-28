// The translation drawn into a copy of the picture, off the UI thread: each block's background taken from the picture
// round it, its words laid out by DirectWrite in the block's place (smaller, then wider, then taller where they do not
// fit) and drawn; the place of every word kept so the pin can select them. The pin then shows this picture, so zoom
// costs nothing and no glyph goes through the UI's atlas.
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include "moonbit.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

extern "C" void mb_post(const char *channel, const char *json, size_t len, const uint8_t *bytes, size_t blen);

struct TrJob {
  int id;
  int zh;
  std::vector<uint8_t> pic;
  std::string spec;
};

struct Block {
  double x, y, w, h, lh;
  int draw;
  std::wstring text;
};

static std::wstring wide(const std::string &s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
  std::wstring w(n > 0 ? n : 0, L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

static std::string narrow(const wchar_t *s, int n) {
  int m = WideCharToMultiByte(CP_UTF8, 0, s, n, NULL, 0, NULL, NULL);
  std::string o(m > 0 ? m : 0, '\0');
  if (m > 0) WideCharToMultiByte(CP_UTF8, 0, s, n, &o[0], m, NULL, NULL);
  return o;
}

static std::vector<Block> parse_spec(const std::string &spec) {
  std::vector<Block> out;
  size_t at = 0;
  while (at < spec.size()) {
    size_t end = spec.find('\n', at);
    if (end == std::string::npos) end = spec.size();
    std::string row = spec.substr(at, end - at);
    at = end + 1;
    Block b = {};
    double v[5] = {0};
    int draw = 0;
    size_t p = 0;
    bool ok = true;
    for (int i = 0; i < 6 && ok; i++) {
      size_t t = row.find('\t', p);
      if (t == std::string::npos) {
        ok = false;
        break;
      }
      std::string f = row.substr(p, t - p);
      if (i < 5) v[i] = atof(f.c_str());
      else draw = atoi(f.c_str());
      p = t + 1;
    }
    if (!ok) continue;
    b.x = v[0], b.y = v[1], b.w = v[2], b.h = v[3], b.lh = v[4], b.draw = draw;
    b.text = wide(row.substr(p));
    out.push_back(b);
  }
  return out;
}

static bool unspaced(wchar_t c) {
  return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
         (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xAC00 && c <= 0xD7AF);
}

static bool blank(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == 0x3000 || c == 0xA0; }

static int channel_median(std::vector<int> &v) {
  if (v.empty()) return 255;
  size_t m = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + m, v.end());
  return v[m];
}

static void tell_failed(int id, const char *why) {
  char json[128];
  int n = snprintf(json, sizeof json, "{\"do\":\"tr-pic\",\"id\":%d,\"ok\":0,\"why\":\"%s\"}", id, why);
  mb_post("ps", json, (size_t)(n > 0 ? n : 0), NULL, 0);
}

static void tr_run(TrJob *job) {
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICStream> in;
  ComPtr<IWICBitmapDecoder> dec;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICBitmapSource> conv;
  ComPtr<IWICBitmap> bmp;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
      FAILED(wic->CreateStream(&in)) ||
      FAILED(in->InitializeFromMemory(job->pic.data(), (DWORD)job->pic.size())) ||
      FAILED(wic->CreateDecoderFromStream(in.Get(), NULL, WICDecodeMetadataCacheOnDemand, &dec)) ||
      FAILED(dec->GetFrame(0, &frame)) ||
      FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, frame.Get(), &conv)) ||
      FAILED(wic->CreateBitmapFromSource(conv.Get(), WICBitmapCacheOnLoad, &bmp))) {
    tell_failed(job->id, "decode");
    return;
  }
  UINT W = 0, H = 0;
  bmp->GetSize(&W, &H);
  if (W == 0 || H == 0) {
    tell_failed(job->id, "decode");
    return;
  }
  std::vector<Block> blocks = parse_spec(job->spec);

  // backgrounds: the median of the ring just outside each block (opaque pixels only)
  std::vector<uint32_t> bgs(blocks.size(), 0xFFFFFFFFu);
  bool solid = true;
  {
    WICRect all = {0, 0, (INT)W, (INT)H};
    ComPtr<IWICBitmapLock> lock;
    if (SUCCEEDED(bmp->Lock(&all, WICBitmapLockRead, &lock))) {
      UINT stride = 0, size = 0;
      BYTE *px = NULL;
      lock->GetStride(&stride);
      lock->GetDataPointer(&size, &px);
      for (UINT y = 0; y < H && solid; y += 7)
        for (UINT x = 0; x < W; x += 7)
          if (px[y * stride + x * 4 + 3] != 255) {
            solid = false;
            break;
          }
      for (size_t i = 0; i < blocks.size(); i++) {
        const Block &b = blocks[i];
        double pad = std::max(2.0, b.lh * H * 0.25);
        int x0 = std::max(0, (int)(b.x * W - pad)), y0 = std::max(0, (int)(b.y * H - pad));
        int x1 = std::min((int)W - 1, (int)((b.x + b.w) * W + pad)), y1 = std::min((int)H - 1, (int)((b.y + b.h) * H + pad));
        std::vector<int> r, g, bl;
        auto take = [&](int x, int y) {
          const BYTE *p = px + (size_t)y * stride + (size_t)x * 4;
          if (p[3] < 128) return;
          bl.push_back(p[0] * 255 / p[3]);
          g.push_back(p[1] * 255 / p[3]);
          r.push_back(p[2] * 255 / p[3]);
        };
        for (int x = x0; x <= x1; x += 2) take(x, y0), take(x, y1);
        for (int y = y0; y <= y1; y += 2) take(x0, y), take(x1, y);
        if (!r.empty())
          bgs[i] = ((uint32_t)channel_median(r) << 16) | ((uint32_t)channel_median(g) << 8) | (uint32_t)channel_median(bl);
        else
          bgs[i] = 0xFFFFFFu;
      }
    }
  }

  ComPtr<ID2D1Factory> d2;
  ComPtr<IDWriteFactory> dw;
  ComPtr<ID2D1RenderTarget> rt;
  D2D1_RENDER_TARGET_PROPERTIES props = {D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                         {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96.0f, 96.0f,
                                         D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT};
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), NULL, (void **)d2.GetAddressOf())) ||
      FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)dw.GetAddressOf())) ||
      FAILED(d2->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt))) {
    tell_failed(job->id, "draw");
    return;
  }
  rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  const wchar_t *family = job->zh ? L"Microsoft YaHei UI" : L"Segoe UI";
  const wchar_t *locale = job->zh ? L"zh-cn" : L"en-us";
  auto layout_at = [&](const std::wstring &t, float size, float w, float h, ComPtr<IDWriteTextLayout> &out) -> bool {
    ComPtr<IDWriteTextFormat> f;
    if (FAILED(dw->CreateTextFormat(family, NULL, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, size, locale, &f)))
      return false;
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    f->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * 1.3f, size * 1.02f);
    out.Reset();
    return SUCCEEDED(dw->CreateTextLayout(t.data(), (UINT32)t.size(), f.Get(), w, h, &out));
  };

  std::string words;
  int line_n = 0;
  rt->BeginDraw();
  for (size_t bi = 0; bi < blocks.size(); bi++) {
    const Block &b = blocks[bi];
    if (b.text.empty()) continue;
    float pad = (float)std::min(std::max(b.lh * H * 0.12, 1.0), 6.0);
    float x = (float)std::max(0.0, b.x * W - pad);
    float y = (float)std::max(0.0, b.y * H - pad);
    float w = (float)std::min((double)W - x, b.w * W + pad * 2.0);
    float h = (float)(b.h * H + pad * 2.0);
    // the room below it: down to the next block under it
    float room = (float)H - y;
    for (size_t j = 0; j < blocks.size(); j++) {
      if (j == bi) continue;
      const Block &o = blocks[j];
      float oy = (float)(o.y * H), ox0 = (float)(o.x * W), ox1 = (float)((o.x + o.w) * W);
      if (oy > y + 1.0f && ox1 > x && ox0 < x + w) room = std::min(room, oy - y - 1.0f);
    }
    room = std::max(room, h);
    float s0 = (float)std::min(std::max(b.lh * H, 7.0), 160.0);
    float size = s0;
    float most_w = (float)W - x - 1.0f;
    ComPtr<IDWriteTextLayout> lay;
    DWRITE_TEXT_METRICS m = {};
    bool fit = false;
    if (!b.draw) {
      fit = layout_at(b.text, size, w + size, 100000.0f, lay);
      if (fit) lay->GetMetrics(&m);
    } else {
      for (float s = s0; s >= std::max(7.0f, s0 * 0.8f); s -= std::max(0.5f, s0 * 0.04f)) {
        if (!layout_at(b.text, s, w, 100000.0f, lay)) break;
        lay->GetMetrics(&m);
        size = s;
        if (m.height <= h + 1.0f) {
          fit = true;
          break;
        }
      }
      if (!fit && lay) {
        float least = std::max(7.0f, s0 * 0.8f);
        for (float s = least; s >= std::max(7.0f, s0 * 0.6f); s -= std::max(0.5f, s0 * 0.04f)) {
          if (!layout_at(b.text, s, std::max(w, most_w), 100000.0f, lay)) break;
          lay->GetMetrics(&m);
          size = s;
          if (m.height <= room + 1.0f) break;
        }
        w = std::max(w, std::min(most_w, m.widthIncludingTrailingWhitespace + 4.0f));
        layout_at(b.text, size, w, 100000.0f, lay);
        lay->GetMetrics(&m);
        h = std::min(std::max(h, m.height + 2.0f), room);
      }
    }
    if (!lay) continue;
    float ty = y + std::max(0.0f, (h - m.height) / 2.0f);
    if (b.draw) {
      uint32_t c = bgs[bi];
      float r = ((c >> 16) & 255) / 255.0f, g = ((c >> 8) & 255) / 255.0f, bl = (c & 255) / 255.0f;
      float lum = 0.299f * r + 0.587f * g + 0.114f * bl;
      ComPtr<ID2D1SolidColorBrush> ground, ink;
      rt->CreateSolidColorBrush(D2D1::ColorF(r, g, bl, 1.0f), &ground);
      rt->CreateSolidColorBrush(lum > 0.55f ? D2D1::ColorF(0.08f, 0.08f, 0.08f, 1.0f) : D2D1::ColorF(0.96f, 0.96f, 0.96f, 1.0f), &ink);
      D2D1_RECT_F box = {x, y, x + w, y + h};
      float rad = std::min(4.0f, size * 0.25f);
      rt->FillRoundedRectangle(D2D1::RoundedRect(box, rad, rad), ground.Get());
      rt->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_ALIASED);
      rt->DrawTextLayout(D2D1::Point2F(x + 1.0f, ty), lay.Get(), ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
      rt->PopAxisAlignedClip();
    }
    // the words' places, for selecting them
    const std::wstring &t = b.text;
    size_t i = 0;
    float last_top = -1.0f;
    bool sp = false;
    while (i < t.size()) {
      if (blank(t[i])) {
        sp = true;
        i++;
        continue;
      }
      size_t j = i + 1;
      if (!unspaced(t[i]))
        while (j < t.size() && !blank(t[j]) && !unspaced(t[j])) j++;
      DWRITE_HIT_TEST_METRICS hm[4];
      UINT32 got = 0;
      if (SUCCEEDED(lay->HitTestTextRange((UINT32)i, (UINT32)(j - i), x + 1.0f, ty, hm, 4, &got)) && got > 0) {
        if (last_top >= 0.0f && hm[0].top > last_top + 0.5f) line_n++;
        last_top = hm[0].top;
        float ww = 0.0f;
        for (UINT32 k = 0; k < got; k++)
          if (hm[k].top == hm[0].top) ww += hm[k].width;
        if (hm[0].top + hm[0].height <= y + h + 1.0f) {
          char row[160];
          int n = snprintf(row, sizeof row, "%d\t%d\t%d\t%.5f\t%.5f\t%.5f\t%.5f\t", line_n, (int)bi, sp ? 1 : 0,
                           hm[0].left / W, hm[0].top / H, ww / W, hm[0].height / H);
          if (n > 0) words.append(row, (size_t)n);
          words += narrow(t.data() + i, (int)(j - i));
          words.push_back('\n');
        }
      }
      sp = false;
      i = j;
    }
    line_n++;
  }
  HRESULT hr = rt->EndDraw();
  rt.Reset();
  if (FAILED(hr)) {
    tell_failed(job->id, "draw");
    return;
  }

  std::vector<uint8_t> out;
  WICRect all = {0, 0, (INT)W, (INT)H};
  if (solid) {
    ComPtr<IWICBitmapLock> lock;
    if (SUCCEEDED(bmp->Lock(&all, WICBitmapLockRead, &lock))) {
      UINT stride = 0, size = 0;
      BYTE *px = NULL;
      lock->GetStride(&stride);
      lock->GetDataPointer(&size, &px);
      size_t n = (size_t)W * H * 4;
      out.resize(54 + n);
      uint8_t *o = out.data();
      auto le32 = [&](size_t at, uint32_t v) {
        o[at] = v & 255, o[at + 1] = (v >> 8) & 255, o[at + 2] = (v >> 16) & 255, o[at + 3] = (v >> 24) & 255;
      };
      o[0] = 'B', o[1] = 'M';
      le32(2, (uint32_t)(54 + n));
      le32(10, 54);
      le32(14, 40);
      le32(18, W);
      le32(22, (uint32_t)(-(int32_t)H));
      o[26] = 1, o[28] = 32;
      le32(34, (uint32_t)n);
      for (UINT y = 0; y < H; y++) {
        memcpy(o + 54 + (size_t)y * W * 4, px + (size_t)y * stride, (size_t)W * 4);
        for (UINT x = 0; x < W; x++) o[54 + ((size_t)y * W + x) * 4 + 3] = 255;
      }
    }
  } else {
    ComPtr<IStream> mem;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fe;
    if (SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &mem)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc)) &&
        SUCCEEDED(enc->Initialize(mem.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fe, NULL)) && SUCCEEDED(fe->Initialize(NULL)) &&
        SUCCEEDED(fe->SetSize(W, H))) {
      WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
      fe->SetPixelFormat(&pf);
      ComPtr<IWICBitmapSource> straight;
      if (SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, bmp.Get(), &straight)) &&
          SUCCEEDED(fe->WriteSource(straight.Get(), NULL)) && SUCCEEDED(fe->Commit()) && SUCCEEDED(enc->Commit())) {
        HGLOBAL hg = NULL;
        if (SUCCEEDED(GetHGlobalFromStream(mem.Get(), &hg))) {
          STATSTG st = {};
          mem->Stat(&st, STATFLAG_NONAME);
          void *p = GlobalLock(hg);
          if (p) {
            out.assign((uint8_t *)p, (uint8_t *)p + (size_t)st.cbSize.QuadPart);
            GlobalUnlock(hg);
          }
        }
      }
    }
  }
  if (out.empty()) {
    tell_failed(job->id, "encode");
    return;
  }
  // (the words first, then the picture: one channel, read in order)
  char json[128];
  int n = snprintf(json, sizeof json, "{\"do\":\"tr-words\",\"id\":%d}", job->id);
  mb_post("ps", json, (size_t)(n > 0 ? n : 0), (const uint8_t *)words.data(), words.size());
  n = snprintf(json, sizeof json, "{\"do\":\"tr-pic\",\"id\":%d,\"ok\":1,\"w\":%u,\"h\":%u}", job->id, W, H);
  mb_post("ps", json, (size_t)(n > 0 ? n : 0), out.data(), out.size());
}

static DWORD WINAPI tr_thread(LPVOID arg) {
  TrJob *job = (TrJob *)arg;
  HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  tr_run(job);
  if (SUCCEEDED(co)) CoUninitialize();
  delete job;
  return 0;
}

extern "C" int32_t ps_tr_render(int32_t id, moonbit_bytes_t pic, moonbit_bytes_t spec, int32_t zh) {
  int32_t n = (int32_t)Moonbit_array_length(pic);
  if (n <= 0) return 0;
  TrJob *job = new TrJob();
  job->id = id;
  job->zh = zh;
  job->pic.assign((const uint8_t *)pic, (const uint8_t *)pic + n);
  job->spec.assign((const char *)spec, (size_t)Moonbit_array_length(spec));
  HANDLE t = CreateThread(NULL, 0, tr_thread, job, 0, NULL);
  if (!t) {
    delete job;
    return 0;
  }
  CloseHandle(t);
  return 1;
}
