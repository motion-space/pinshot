#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <psapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mmsystem.h>
#include <wtsapi32.h>
#include <wincodec.h>
#include "moonbit.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "wtsapi32.lib")

extern void mb_post(const char *channel, const char *json, size_t len, const uint8_t *bytes, size_t blen);
extern void *mb_app_hwnd_of(int wid);

static moonbit_bytes_t ps_bytes(const void *p, size_t n) {
  moonbit_bytes_t b = moonbit_make_bytes((int32_t)n, 0);
  if (n > 0 && p) memcpy(b, p, n);
  return b;
}

static moonbit_bytes_t ps_text(const char *s) { return ps_bytes(s, strlen(s)); }

static wchar_t *ps_wide(const char *utf8) {
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
  wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (n > 0 ? n : 1));
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
  else w[0] = 0;
  return w;
}

static moonbit_bytes_t ps_utf8_bytes(const wchar_t *w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  if (n <= 1) return moonbit_make_bytes(0, 0);
  moonbit_bytes_t b = moonbit_make_bytes(n - 1, 0);
  WideCharToMultiByte(CP_UTF8, 0, w, -1, (char *)b, n - 1, NULL, NULL);
  return b;
}

static void ps_tell(const char *json) { mb_post("ps", json, strlen(json), NULL, 0); }

static double ps_ms(void) {
  static LARGE_INTEGER f;
  LARGE_INTEGER t;
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

double ps_now_ms(void) { return ps_ms(); }

#define WM_PS_TAKEN (WM_APP + 42)
#define PS_FREE_TIMER 7
#define PS_WGC_TIMER 8
#define PS_IDLE_TRIM_TIMER 9
#define PS_TRIM_SOON_TIMER 10
#define IDLE_TRIM_MS 3000
static int wgc_off;
static const GUID display_state_guid = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

typedef struct {
  HBITMAP dib;
  void *staging;
  HANDLE shm;
  void *shm_view;
  uint8_t *bits;
  int w, h, pitch;
} Shot;

typedef struct {
  int32_t ok;
  int32_t w, h, pitch;
  double take_ms;
  double white;
  int32_t said_half;
  int32_t pad;
} WgcShmHeader;
int ps_wgc_restart(void);
int ps_wgc_borderless_ok(void);
int ps_wgc_dd_ok(void);
int ps_wgc_dd_black(void);
int ps_wgc_last_used_dd(void);
int ps_wgc_mon_hdr(HMONITOR mon, float *white);
void *ps_wgc_take_dd(HMONITOR mon, int w, int h, uint8_t **bits, int *pitch);
void *ps_wgc_take_now(HMONITOR mon, int w, int h, uint8_t **bits, int *pitch);
void ps_wgc_release(void *taken);
void ps_wgc_trim(void);
static HMONITOR cap_mon;
static int cap_used_helper;
static double helper_ms, helper_white;
static int helper_said_half;

static Shot cap;
static volatile LONG cap_busy;
static int cap_ok;
static int cap_x, cap_y, cap_w, cap_h, cap_dpi;
static int cap_wx, cap_wy, cap_ww, cap_wh;
static double cap_t0, cap_ms, cap_key_lag;
static int keep_gpu_warm = 1;
static char *win_list;
static size_t win_list_len;
static void windows_listed(void);

static UINT idle_trim_ms(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *e = getenv("PINSHOT_IDLE_TRIM_MS");
    int v = e ? atoi(e) : IDLE_TRIM_MS;
    cached = v >= 100 ? v : IDLE_TRIM_MS;
  }
  return (UINT)cached;
}

static UINT monitor_dpi(HMONITOR mon) {
  UINT dx = 96, dy = 96;
  typedef HRESULT(WINAPI * GetDpiForMonitorFn)(HMONITOR, int, UINT *, UINT *);
  static GetDpiForMonitorFn dpi_of;
  static int tried;
  if (!tried) {
    tried = 1;
    HMODULE sh = LoadLibraryW(L"shcore.dll");
    if (sh) dpi_of = (GetDpiForMonitorFn)GetProcAddress(sh, "GetDpiForMonitor");
  }
  if (dpi_of) dpi_of(mon, 0, &dx, &dy);
  return dx;
}

static void shot_free(Shot *s) {
  if (s->dib) DeleteObject(s->dib);
  if (s->staging) ps_wgc_release(s->staging);
  if (s->shm_view) UnmapViewOfFile(s->shm_view);
  if (s->shm) CloseHandle(s->shm);
  memset(s, 0, sizeof *s);
}

static HWND tray_hwnd;

void ps_bmp_header(uint8_t h[54], int32_t w, int32_t hh) {
  memset(h, 0, 54);
  uint32_t img = (uint32_t)((size_t)w * (size_t)hh * 4);
  uint32_t size = 54u + img, off = 54u, hsize = 40u;
  int32_t neg_h = -hh;
  uint16_t planes = 1, bits = 32;
  h[0] = 'B';
  h[1] = 'M';
  memcpy(h + 2, &size, 4);
  memcpy(h + 10, &off, 4);
  memcpy(h + 14, &hsize, 4);
  memcpy(h + 18, &w, 4);
  memcpy(h + 22, &neg_h, 4);
  memcpy(h + 26, &planes, 2);
  memcpy(h + 28, &bits, 2);
  memcpy(h + 34, &img, 4);
}

static moonbit_bytes_t ps_raw_bmp(const uint8_t *bits, int32_t w, int32_t hh, int32_t pitch) {
  if (!bits || w <= 0 || hh <= 0) return moonbit_make_bytes(0, 0);
  size_t row = (size_t)w * 4;
  if ((size_t)pitch < row) return moonbit_make_bytes(0, 0);
  size_t img = row * (size_t)hh;
  moonbit_bytes_t b = moonbit_make_bytes((int32_t)(54 + img), 0);
  ps_bmp_header(b, w, hh);
  if ((size_t)pitch == row) {
    memcpy(b + 54, bits, img);
  } else {
    for (int32_t y = 0; y < hh; y++) memcpy(b + 54 + (size_t)y * row, bits + (size_t)y * (size_t)pitch, row);
  }
  return b;
}

static void cap_drop(void) { shot_free(&cap); }

static int monitor_under_pointer(void) {
  static int aware;
  if (!aware) aware = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) || 1;
  POINT p;
  GetCursorPos(&p);
  HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof mi};
  if (!GetMonitorInfoW(mon, &mi)) return 0;
  int w = mi.rcMonitor.right - mi.rcMonitor.left, h = mi.rcMonitor.bottom - mi.rcMonitor.top;
  if (w <= 0 || h <= 0) return 0;
  cap_x = mi.rcMonitor.left, cap_y = mi.rcMonitor.top, cap_w = w, cap_h = h, cap_dpi = (int)monitor_dpi(mon);
  cap_wx = mi.rcWork.left - cap_x, cap_wy = mi.rcWork.top - cap_y;
  cap_ww = mi.rcWork.right - mi.rcWork.left, cap_wh = mi.rcWork.bottom - mi.rcWork.top;
  cap_mon = mon;
  return 1;
}

static int test_screen_into(uint8_t *bits, int w, int h, int pitch) {
  wchar_t path[MAX_PATH];
  if (!getenv("PINSHOT_TEST") || !GetEnvironmentVariableW(L"PINSHOT_TEST_SCREEN", path, MAX_PATH)) return 0;
  HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  IWICImagingFactory *f = NULL;
  IWICBitmapDecoder *dec = NULL;
  IWICBitmapFrameDecode *fr = NULL;
  IWICFormatConverter *cv = NULL;
  int ok = 0;
  memset(bits, 0, (size_t)pitch * h);
  if (SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                 (void **)&f)) &&
      SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, path, NULL, GENERIC_READ,
                                                             WICDecodeMetadataCacheOnDemand, &dec)) &&
      SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &cv)) &&
      SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &GUID_WICPixelFormat32bppBGR,
                                               WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) {
    UINT iw = 0, ih = 0;
    IWICFormatConverter_GetSize(cv, &iw, &ih);
    WICRect r = {0, 0, (INT)(iw < (UINT)w ? iw : (UINT)w), (INT)(ih < (UINT)h ? ih : (UINT)h)};
    ok = SUCCEEDED(IWICFormatConverter_CopyPixels(cv, &r, (UINT)pitch, (UINT)pitch * h, bits));
  }
  if (cv) IWICFormatConverter_Release(cv);
  if (fr) IWICBitmapFrameDecode_Release(fr);
  if (dec) IWICBitmapDecoder_Release(dec);
  if (f) IWICImagingFactory_Release(f);
  if (SUCCEEDED(co)) CoUninitialize();
  return ok;
}

static DWORD WINAPI cap_work(void *arg) {
  (void)arg;
  int ok = 0;
  if (cap.dib && test_screen_into(cap.bits, cap_w, cap_h, cap.pitch)) {
    cap_ok = 1;
    cap_ms = ps_ms() - cap_t0;
    InterlockedExchange(&cap_busy, 0);
    if (tray_hwnd) PostMessageW(tray_hwnd, WM_PS_TAKEN, 1, 0);
    return 0;
  }
  HDC screen = GetDC(NULL);
  HDC mem = CreateCompatibleDC(screen);
  if (cap.dib) {
    HGDIOBJ old = SelectObject(mem, cap.dib);
    ok = BitBlt(mem, 0, 0, cap_w, cap_h, screen, cap_x, cap_y, SRCCOPY) != 0;
    SelectObject(mem, old);
    GdiFlush();
  }
  DeleteDC(mem);
  ReleaseDC(NULL, screen);
  cap_ok = ok;
  cap_ms = ps_ms() - cap_t0;
  InterlockedExchange(&cap_busy, 0);
  if (tray_hwnd) PostMessageW(tray_hwnd, WM_PS_TAKEN, (WPARAM)ok, 0);
  return 0;
}

static int gdi_begin(void);

static char cap_helper_fail[48];

typedef struct {
  wchar_t shm_name[80];
  wchar_t ev_name[80];
  HANDLE ev;
  int x, y, w, h;
} WgcHelperArgs;

static DWORD WINAPI wgc_helper_work(void *arg) {
  WgcHelperArgs *a = (WgcHelperArgs *)arg;
  int ok = 0;
  wchar_t exe[MAX_PATH * 2] = {0};
  if (GetModuleFileNameW(NULL, exe, MAX_PATH * 2) > 0) {
    wchar_t cmd[MAX_PATH * 2 + 256];
    _snwprintf(cmd, MAX_PATH * 2 + 255, L"\"%ls\" --wgc-shot %ls %ls %d %d %d %d", exe, a->shm_name, a->ev_name, a->x,
               a->y, a->w, a->h);
    cmd[MAX_PATH * 2 + 255] = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
      HANDLE waits[2] = {a->ev, pi.hProcess};
      DWORD wr = WaitForMultipleObjects(2, waits, FALSE, 800);
      WgcShmHeader *hdr = (WgcShmHeader *)cap.shm_view;
      if (hdr && hdr->ok == 1 && hdr->w == a->w && hdr->h == a->h && hdr->pitch >= a->w * 4) {
        cap.bits = (uint8_t *)hdr + sizeof(WgcShmHeader);
        cap.w = hdr->w;
        cap.h = hdr->h;
        cap.pitch = hdr->pitch;
        helper_ms = hdr->take_ms;
        helper_white = hdr->white;
        helper_said_half = hdr->said_half;
        cap_used_helper = 1;
        ok = 1;
      } else if (wr == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 100);
        lstrcpynA(cap_helper_fail, "helper timeout 800ms", sizeof cap_helper_fail);
      } else {
        lstrcpynA(cap_helper_fail, "helper failed", sizeof cap_helper_fail);
      }
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
    } else {
      lstrcpynA(cap_helper_fail, "helper spawn failed", sizeof cap_helper_fail);
    }
  }
  CloseHandle(a->ev);
  free(a);
  if (!ok) {
    if (cap.shm_view) UnmapViewOfFile(cap.shm_view), cap.shm_view = NULL;
    if (cap.shm) CloseHandle(cap.shm), cap.shm = NULL;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = cap_w;
    bi.bmiHeader.biHeight = -cap_h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    cap.dib = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (cap.dib && bits) {
      cap.bits = (uint8_t *)bits;
      cap.w = cap_w, cap.h = cap_h, cap.pitch = cap_w * 4;
      HDC screen = GetDC(NULL);
      HDC mem = CreateCompatibleDC(screen);
      HGDIOBJ old = SelectObject(mem, cap.dib);
      ok = BitBlt(mem, 0, 0, cap_w, cap_h, screen, cap_x, cap_y, SRCCOPY) != 0;
      SelectObject(mem, old);
      GdiFlush();
      DeleteDC(mem);
      ReleaseDC(NULL, screen);
    }
  }
  cap_ok = ok;
  cap_ms = ps_ms() - cap_t0;
  InterlockedExchange(&cap_busy, 0);
  if (tray_hwnd) PostMessageW(tray_hwnd, WM_PS_TAKEN, (WPARAM)ok, 0);
  return 0;
}

static int wgc_helper_begin(void) {
  static volatile LONG seq;
  LONG id = InterlockedIncrement(&seq);
  WgcHelperArgs *a = (WgcHelperArgs *)calloc(1, sizeof *a);
  if (!a) return 0;
  DWORD pid = GetCurrentProcessId();
  _snwprintf(a->shm_name, 79, L"Local\\PinShot.Shot.%lu.%ld", pid, id);
  _snwprintf(a->ev_name, 79, L"Local\\PinShot.Shot.%lu.%ld.ev", pid, id);
  a->x = cap_x, a->y = cap_y, a->w = cap_w, a->h = cap_h;
  size_t total = sizeof(WgcShmHeader) + (size_t)cap_w * (size_t)cap_h * 4;
  cap.shm = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)(total >> 32), (DWORD)total, a->shm_name);
  if (!cap.shm) {
    free(a);
    return 0;
  }
  cap.shm_view = MapViewOfFile(cap.shm, FILE_MAP_ALL_ACCESS, 0, 0, total);
  if (!cap.shm_view) {
    CloseHandle(cap.shm);
    cap.shm = NULL;
    free(a);
    return 0;
  }
  memset(cap.shm_view, 0, sizeof(WgcShmHeader));
  a->ev = CreateEventW(NULL, TRUE, FALSE, a->ev_name);
  if (!a->ev) {
    UnmapViewOfFile(cap.shm_view);
    CloseHandle(cap.shm);
    cap.shm_view = NULL;
    cap.shm = NULL;
    free(a);
    return 0;
  }
  InterlockedExchange(&cap_busy, 1);
  HANDLE t = CreateThread(NULL, 0, wgc_helper_work, a, 0, NULL);
  if (!t) {
    InterlockedExchange(&cap_busy, 0);
    CloseHandle(a->ev);
    UnmapViewOfFile(cap.shm_view);
    CloseHandle(cap.shm);
    cap.shm_view = NULL;
    cap.shm = NULL;
    free(a);
    return 0;
  }
  CloseHandle(t);
  return 1;
}

static int cap_begin(void) {
  if (cap_busy) return 0;
  cap_t0 = ps_ms();
  if (!monitor_under_pointer()) return 0;
  cap_drop();
  windows_listed();
  cap_ok = 0;
  cap_used_helper = 0;
  cap_helper_fail[0] = 0;
  int test_screen = getenv("PINSHOT_TEST") && getenv("PINSHOT_TEST_SCREEN");
  if (!wgc_off && !test_screen && tray_hwnd) {
    if (ps_wgc_dd_ok()) {
      uint8_t *bits = NULL;
      int pitch = 0;
      void *t = ps_wgc_take_dd(cap_mon, cap_w, cap_h, &bits, &pitch);
      if (t) {
        cap.staging = t;
        cap.bits = bits;
        cap.w = cap_w, cap.h = cap_h, cap.pitch = pitch;
        cap_ok = 1;
        cap_ms = ps_ms() - cap_t0;
        PostMessageW(tray_hwnd, WM_PS_TAKEN, 1, 0);
        return 1;
      }
      // DXGI did not produce a usable frame (timeout, pointer-only storm, or all-black guard):
      // fall back to the out-of-process WGC helper so HDR stays accurate with 0 MB in-process WGC VRAM leak.
      if (ps_wgc_borderless_ok()) {
        float hdr_white = 1.0f;
        if (!keep_gpu_warm && !ps_wgc_mon_hdr(cap_mon, &hdr_white)) return gdi_begin();
        if (wgc_helper_begin()) return 1;
      }
      return gdi_begin();
    }
    if (!keep_gpu_warm && ps_wgc_borderless_ok()) {
      float hdr_white = 1.0f;
      if (!ps_wgc_mon_hdr(cap_mon, &hdr_white)) return gdi_begin();
      if (wgc_helper_begin()) return 1;
    }
    uint8_t *bits = NULL;
    int pitch = 0;
    void *t = ps_wgc_take_now(cap_mon, cap_w, cap_h, &bits, &pitch);
    if (t) {
      cap.staging = t;
      cap.bits = bits;
      cap.w = cap_w, cap.h = cap_h, cap.pitch = pitch;
      cap_ok = 1;
      cap_ms = ps_ms() - cap_t0;
      PostMessageW(tray_hwnd, WM_PS_TAKEN, 1, 0);
      return 1;
    }
  }
  return gdi_begin();
}

static int gdi_begin(void) {
  BITMAPINFO bi;
  memset(&bi, 0, sizeof bi);
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = cap_w;
  bi.bmiHeader.biHeight = -cap_h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void *bits = NULL;
  cap.dib = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
  if (!cap.dib || !bits) {
    shot_free(&cap);
    return 0;
  }
  cap.bits = (uint8_t *)bits;
  cap.w = cap_w, cap.h = cap_h, cap.pitch = cap_w * 4;
  InterlockedExchange(&cap_busy, 1);
  HANDLE t = CreateThread(NULL, 0, cap_work, NULL, 0, NULL);
  if (!t) {
    InterlockedExchange(&cap_busy, 0);
    shot_free(&cap);
    return 0;
  }
  CloseHandle(t);
  return 1;
}

static void tray_window_made(void);

int32_t ps_capture_now(void) {
  tray_window_made();
  if (!cap_begin()) return 0;
  MSG m;
  while (cap_busy) {
    if (PeekMessageW(&m, tray_hwnd, 0, 0, PM_REMOVE)) {
      if (m.message != WM_PS_TAKEN) DispatchMessageW(&m);
    } else {
      MsgWaitForMultipleObjects(0, NULL, FALSE, 1, QS_ALLINPUT);
    }
  }
  while (PeekMessageW(&m, tray_hwnd, WM_PS_TAKEN, WM_PS_TAKEN, PM_REMOVE)) {
  }
  return cap_ok;
}

void ps_wgc_timing(double *out);
void ps_wgc_state(char *out, int n);
moonbit_bytes_t ps_capture_info(void) {
  if (!cap.bits || cap_busy) return moonbit_make_bytes(0, 0);
  char s[320], lag[48] = {0}, blk[32] = {0};
  if (cap_key_lag >= 10.0) snprintf(lag, sizeof lag, "; key lag %.0f ms", cap_key_lag);
  if (ps_wgc_dd_black() > 0 && !ps_wgc_last_used_dd()) snprintf(blk, sizeof blk, "; dxgi black %d", ps_wgc_dd_black());
  if (cap_used_helper) {
    snprintf(s, sizeof s, "%d,%d,%d,%d,%d,%.2f,wgc helper %.1f; hdr white %.2f%s%s%s", cap_x, cap_y, cap_w, cap_h, cap_dpi,
             cap_ms, helper_ms, helper_white, helper_said_half > 0 ? " (copied first)" : "", blk, lag);
  } else if (cap.staging) {
    double t[5];
    ps_wgc_timing(t);
    snprintf(s, sizeof s, "%d,%d,%d,%d,%d,%.2f,%s copy %.1f; hdr white %.2f%s; frame %.1f s old%s%s%s", cap_x, cap_y, cap_w,
             cap_h, cap_dpi, cap_ms, ps_wgc_last_used_dd() ? "dxgi" : "wgc", t[0], t[1],
             t[2] > 0 ? " (copied first)" : "", t[3], t[4] > 0 ? " (session made again)" : "", blk, lag);
  } else {
    char st[96];
    ps_wgc_state(st, sizeof st);
    snprintf(s, sizeof s, "%d,%d,%d,%d,%d,%.2f,gdi (%s%s%s%s)", cap_x, cap_y, cap_w, cap_h, cap_dpi, cap_ms,
             cap_helper_fail[0] ? cap_helper_fail : "", cap_helper_fail[0] ? "; " : "", st, lag);
  }
  return ps_text(s);
}

moonbit_bytes_t ps_capture_take(void) {
  if (!cap.bits || cap_busy || !cap_ok) return moonbit_make_bytes(0, 0);
  return ps_raw_bmp(cap.bits, cap.w, cap.h, cap.pitch);
}

moonbit_bytes_t ps_capture_crop_rgba(int32_t x, int32_t y, int32_t w, int32_t h) {
  if (!cap.bits || cap_busy || !cap_ok || cap.w <= 0 || cap.h <= 0 || x < 0 || y < 0 || w <= 0 || h <= 0)
    return moonbit_make_bytes(0, 0);
  if (x > cap.w - w || y > cap.h - h)
    return moonbit_make_bytes(0, 0);
  size_t n = (size_t)w * (size_t)h * 4;
  if (n > 0x7FFFFFFFu)
    return moonbit_make_bytes(0, 0);
  moonbit_bytes_t out = moonbit_make_bytes((int32_t)n, 0);
  for (int32_t row = 0; row < h; row++) {
    const uint8_t *s = cap.bits + (size_t)(y + row) * (size_t)cap.pitch + (size_t)x * 4;
    uint8_t *d = (uint8_t *)out + (size_t)row * (size_t)w * 4;
    for (int32_t col = 0; col < w; col++, s += 4, d += 4) {
      d[0] = s[2];
      d[1] = s[1];
      d[2] = s[0];
      d[3] = 255;
    }
  }
  return out;
}

void ps_capture_release(void) {
  if ((!cap.dib && !cap.staging && !cap.shm) || cap_busy) return;
  cap_drop();
  if (tray_hwnd) SetTimer(tray_hwnd, PS_FREE_TIMER, 1500, NULL);
}

int32_t ps_pixel(int32_t x, int32_t y) {
  if (!cap.bits || cap_busy || x < 0 || y < 0 || x >= cap.w || y >= cap.h) return -1;
  const uint8_t *p = cap.bits + (size_t)y * cap.pitch + (size_t)x * 4;
  return (p[2] << 16) | (p[1] << 8) | p[0];
}

int32_t ps_mean(int32_t x, int32_t y, int32_t cell) {
  if (!cap.bits || cap_busy) return 0x808080;
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = x + cell > cap.w ? cap.w : x + cell, y1 = y + cell > cap.h ? cap.h : y + cell;
  if (x1 <= x0 || y1 <= y0) return 0x808080;
  int step = cell >= 24 ? 4 : (cell >= 8 ? 2 : 1);
  unsigned r = 0, g = 0, b = 0, n = 0;
  for (int yy = y0; yy < y1; yy += step) {
    const uint8_t *p = cap.bits + (size_t)yy * cap.pitch + (size_t)x0 * 4;
    for (int xx = x0; xx < x1; xx += step, p += 4 * step) {
      b += p[0];
      g += p[1];
      r += p[2];
      n++;
    }
  }
  unsigned qr = r / n / 4 * 4 + 2, qg = g / n / 4 * 4 + 2, qb = b / n / 4 * 4 + 2;
  return (int32_t)((qr << 16) | (qg << 8) | qb);
}

typedef struct {
  char *out;
  size_t len, cap;
  int depth;
  HWND *hwnds;
  size_t n, hwnds_cap;
} WinList;

static HWND *win_hwnds;
static size_t win_hwnds_n;

static void list_put(WinList *l, RECT r, HWND h) {
  if (l->n + 1 > l->hwnds_cap) {
    l->hwnds_cap = (l->n + 1) * 2;
    l->hwnds = (HWND *)realloc(l->hwnds, l->hwnds_cap * sizeof(HWND));
  }
  l->hwnds[l->n++] = h;
  char s[200], cls[96] = {0};
  GetClassNameA(h, cls, sizeof cls);
  int n = snprintf(s, sizeof s, "%ld,%ld,%ld,%ld;%s\n", r.left - cap_x, r.top - cap_y, r.right - r.left, r.bottom - r.top, cls);
  if (l->len + n + 1 > l->cap) {
    l->cap = (l->len + n + 1) * 2;
    l->out = (char *)realloc(l->out, l->cap);
  }
  memcpy(l->out + l->len, s, (size_t)n);
  l->len += (size_t)n;
}

static int rect_on_monitor(RECT r) {
  RECT m = {cap_x, cap_y, cap_x + cap_w, cap_y + cap_h}, i;
  return IntersectRect(&i, &r, &m) && (i.right - i.left) >= 8 && (i.bottom - i.top) >= 8;
}

static BOOL CALLBACK child_found(HWND h, LPARAM lp) {
  WinList *l = (WinList *)lp;
  if (!IsWindowVisible(h)) return TRUE;
  RECT r;
  if (!GetWindowRect(h, &r)) return TRUE;
  if ((r.right - r.left) < 40 || (r.bottom - r.top) < 24 || !rect_on_monitor(r)) return TRUE;
  list_put(l, r, h);
  return TRUE;
}

static BOOL CALLBACK window_found(HWND h, LPARAM lp) {
  WinList *l = (WinList *)lp;
  if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  int is_pin = pid == GetCurrentProcessId() && GetPropW(h, L"PinShot.Pin") != NULL;
  if (pid == GetCurrentProcessId() && !is_pin) return TRUE;
  BOOL cloaked = FALSE;
  DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
  if (cloaked) return TRUE;
  LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
  if ((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED)) return TRUE;
  if (ex & WS_EX_LAYERED) {
    BYTE alpha = 255;
    DWORD flags = 0;
    if (GetLayeredWindowAttributes(h, NULL, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0) return TRUE;
  }
  RECT r;
  if (is_pin) {
    if (!GetWindowRect(h, &r)) return TRUE;
  } else if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)) && !GetWindowRect(h, &r)) {
    return TRUE;
  }
  if ((r.right - r.left) < 8 || (r.bottom - r.top) < 8 || !rect_on_monitor(r)) return TRUE;
  list_put(l, r, h);
  if (!is_pin) EnumChildWindows(h, child_found, lp);
  return TRUE;
}

static void windows_listed(void) {
  WinList l = {NULL, 0, 0, 0, NULL, 0, 0};
  EnumWindows(window_found, (LPARAM)&l);
  free(win_list);
  win_list = l.out;
  win_list_len = l.len;
  free(win_hwnds);
  win_hwnds = l.hwnds;
  win_hwnds_n = l.n;
}

int32_t ps_window_shows(int32_t of, int32_t i, int32_t x, int32_t y) {
  if (i < 0 || (size_t)i >= win_hwnds_n || of < 0 || (size_t)of >= win_hwnds_n) return 0;
  HWND h = win_hwnds[i];
  if (!IsWindow(h)) return 0;
  HWND cur = GetAncestor(h, GA_ROOT);
  if (!cur || cur == h || cur != win_hwnds[of]) return 0;
  POINT screen = {x + cap_x, y + cap_y};
  for (int depth = 0; depth < 32; depth++) {
    POINT pt = screen;
    ScreenToClient(cur, &pt);
    HWND next = ChildWindowFromPointEx(cur, pt, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
    if (!next || next == cur) break;
    cur = next;
    if (cur == h) return 1;
  }
  return cur == h || IsChild(h, cur) ? 1 : 0;
}

moonbit_bytes_t ps_windows(void) { return ps_bytes(win_list, win_list_len); }

#define PS_CLASS L"PinShot.Tray"
static int test_run(void) {
  static int t = -1;
  if (t < 0) t = getenv("PINSHOT_TEST") != NULL;
  return t;
}
static const wchar_t *test_id(void) {
  static wchar_t id[32];
  static int done;
  if (!done) {
    done = 1;
    if (!GetEnvironmentVariableW(L"PINSHOT_TEST_ID", id, 32)) id[0] = 0;
  }
  return id;
}
static const wchar_t *ps_class(void) {
  static wchar_t cls[64];
  if (!test_run()) return PS_CLASS;
  if (!cls[0]) _snwprintf(cls, 63, L"PinShot.TestTray%ls", test_id());
  return cls;
}
#define WM_PS_TRAY (WM_APP + 40)
#define WM_PS_AGAIN (WM_APP + 41)
static NOTIFYICONDATAW tray;
static int tray_on;
static UINT taskbar_created;
static HANDLE one_run;
static HPOWERNOTIFY power_note;
static int wts_note;

enum { HK_CAPTURE = 1, HK_PIN_CLIP = 2, HK_PINS = 3, HK_RESTORE = 4, HK_MOST = 5 };
static int hk_on[HK_MOST];

static HICON own_icon(int size) {
  HICON i = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
  return i ? i : LoadIcon(NULL, IDI_APPLICATION);
}

static void tray_put(void) {
  if (test_run()) return;
  if (tray.hIcon) DestroyIcon(tray.hIcon);
  memset(&tray, 0, sizeof tray);
  tray.cbSize = sizeof tray;
  tray.hWnd = tray_hwnd;
  tray.uID = 1;
  tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  tray.uCallbackMessage = WM_PS_TRAY;
  tray.hIcon = own_icon(GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(tray_hwnd)));
  lstrcpynW(tray.szTip, L"PinShot", 64);
  if (Shell_NotifyIconW(NIM_ADD, &tray)) {
    tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &tray);
    tray_on = 1;
  }
}

static void tray_gone(void) {
  if (tray_hwnd) Shell_NotifyIconW(NIM_DELETE, &tray);
  if (tray.hIcon) {
    DestroyIcon(tray.hIcon);
    tray.hIcon = NULL;
  }
}

#define MENU_MOST 16
static wchar_t *menu_words[MENU_MOST];
static char menu_keys[MENU_MOST][32];
static int menu_n;

void ps_set_menu(moonbit_bytes_t lines) {
  for (int i = 0; i < menu_n; i++) free(menu_words[i]);
  menu_n = 0;
  int n = (int)Moonbit_array_length(lines);
  const char *s = (const char *)lines;
  int at = 0;
  while (at < n && menu_n < MENU_MOST) {
    int end = at;
    while (end < n && s[end] != '\n') end++;
    int tab = at;
    while (tab < end && s[tab] != '\t') tab++;
    char key[32] = {0}, words[256] = {0};
    int kl = tab - at < 31 ? tab - at : 31;
    memcpy(key, s + at, (size_t)kl);
    if (tab < end) {
      int wl = end - tab - 1 < 255 ? end - tab - 1 : 255;
      memcpy(words, s + tab + 1, (size_t)wl);
    }
    lstrcpynA(menu_keys[menu_n], key, 32);
    menu_words[menu_n] = ps_wide(words);
    menu_n++;
    at = end + 1;
  }
}

void ps_quit(void);

static void tray_menu(int x, int y) {
  HMENU m = CreatePopupMenu();
  for (int i = 0; i < menu_n; i++) {
    if (!strcmp(menu_keys[i], "-")) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    else AppendMenuW(m, MF_STRING, (UINT_PTR)(i + 1), menu_words[i]);
  }
  SetForegroundWindow(tray_hwnd);
  int chose = (int)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, tray_hwnd, NULL);
  PostMessageW(tray_hwnd, WM_NULL, 0, 0);
  DestroyMenu(m);
  if (chose > 0 && chose <= menu_n) {
    if (!strcmp(menu_keys[chose - 1], "quit")) {
      ps_quit();
      return;
    }
    char json[96];
    snprintf(json, sizeof json, "{\"do\":\"menu\",\"item\":\"%s\"}", menu_keys[chose - 1]);
    ps_tell(json);
  }
}

static char cap_by[16];
static int capture_open;
static char ask_waiting[16];
#define WM_PS_ASK (WM_APP + 45)
#define WM_PS_MOVE (WM_APP + 46)
static void gpu_trim_now(int force_trim);
#define WM_PS_TRIM (WM_APP + 43)

static HWND own_window(int wid);

static void capture_asked(const char *by) {
  if (capture_open == 2) {
    lstrcpynA(ask_waiting, by, sizeof ask_waiting);
    return;
  }
  if (capture_open) return;
  if (tray_hwnd) KillTimer(tray_hwnd, PS_IDLE_TRIM_TIMER);
  if (strcmp(by, "key") != 0) cap_key_lag = 0.0;
  if (!cap_begin()) return;
  lstrcpynA(cap_by, by, sizeof cap_by);
  char json[256];
  snprintf(json, sizeof json,
           "{\"do\":\"prepare\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"wx\":%d,\"wy\":%d,\"ww\":%d,\"wh\":%d,\"dpi\":%d,\"t\":%.3f}",
           cap_x, cap_y, cap_w, cap_h, cap_wx, cap_wy, cap_ww, cap_wh, cap_dpi, cap_t0);
  ps_tell(json);
}

void ps_quit(void);

static LRESULT CALLBACK tray_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (taskbar_created && msg == taskbar_created) {
    tray_put();
    return 0;
  }
  switch (msg) {
    case WM_HOTKEY:
      AllowSetForegroundWindow(GetCurrentProcessId());
      if (wp == HK_CAPTURE) {
        cap_key_lag = (double)((DWORD)GetTickCount() - (DWORD)GetMessageTime());
        capture_asked("key");
      }
      else if (wp == HK_PIN_CLIP) ps_tell("{\"do\":\"pin-clipboard\"}");
      else if (wp == HK_PINS) ps_tell("{\"do\":\"pins\"}");
      else if (wp == HK_RESTORE) ps_tell("{\"do\":\"pin-restore\"}");
      return 0;
    case WM_PS_AGAIN:
      if (wp == 1) ps_tell("{\"do\":\"menu\",\"item\":\"settings\"}");
      else if (wp == 2) ps_quit();
      else if (wp == 3) ;
      else capture_asked("again");
      return 0;
    case WM_PS_TRIM:
      if (capture_open || cap_busy) return 0;
      gpu_trim_now(0);
      if (keep_gpu_warm) SetTimer(h, PS_IDLE_TRIM_TIMER, idle_trim_ms(), NULL);
      return 0;
    case WM_PS_ASK:
      if (ask_waiting[0] && !capture_open) {
        char by[16];
        lstrcpynA(by, ask_waiting, sizeof by);
        ask_waiting[0] = 0;
        capture_asked(by);
      }
      return 0;
    case WM_DISPLAYCHANGE:
      if (!wgc_off) SetTimer(h, PS_WGC_TIMER, 250, NULL);
      return 0;
    case WM_POWERBROADCAST:
      if (wp == PBT_APMRESUMEAUTOMATIC ||
          (wp == PBT_POWERSETTINGCHANGE && lp &&
           !memcmp(&((POWERBROADCAST_SETTING *)lp)->PowerSetting, &display_state_guid, sizeof(GUID)) &&
           ((POWERBROADCAST_SETTING *)lp)->DataLength >= sizeof(DWORD) && *(DWORD *)((POWERBROADCAST_SETTING *)lp)->Data == 1))
        SetTimer(h, PS_WGC_TIMER, 1500, NULL);
      return TRUE;
    case WM_WTSSESSION_CHANGE:
      if (wp == WTS_SESSION_UNLOCK || wp == WTS_CONSOLE_CONNECT || wp == WTS_REMOTE_CONNECT)
        SetTimer(h, PS_WGC_TIMER, 1500, NULL);
      return 0;
    case WM_TIMER:
      if (wp == PS_WGC_TIMER) {
        KillTimer(h, PS_WGC_TIMER);
        if (!wgc_off) ps_wgc_restart();
      }
      if (wp == PS_TRIM_SOON_TIMER) {
        KillTimer(h, PS_TRIM_SOON_TIMER);
        PostMessageW(h, WM_PS_TRIM, 0, 0);
      }
      if (wp == PS_FREE_TIMER) {
        KillTimer(h, PS_FREE_TIMER);
        if (keep_gpu_warm && !capture_open && !cap_busy) SetTimer(h, PS_IDLE_TRIM_TIMER, idle_trim_ms(), NULL);
      }
      if (wp == PS_IDLE_TRIM_TIMER) {
        KillTimer(h, PS_IDLE_TRIM_TIMER);
        if (!capture_open && !cap_busy) gpu_trim_now(1);
      }
      return 0;
    case WM_PS_TAKEN: {
      char json[128];
      snprintf(json, sizeof json, "{\"do\":\"capture\",\"ok\":%s,\"by\":\"%s\"}", wp ? "true" : "false", cap_by);
      ps_tell(json);
      return 0;
    }
    case WM_PS_TRAY:
      switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT: capture_asked("icon"); break;
        case WM_CONTEXTMENU: tray_menu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp)); break;
        case NIN_BALLOONUSERCLICK: ps_tell("{\"do\":\"menu\",\"item\":\"settings\"}"); break;
      }
      return 0;
    case WM_QUERYENDSESSION:
      return TRUE;
    case WM_ENDSESSION:
      if (wp) ps_quit();
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

void ps_capture_open(int32_t on) {
  capture_open = on;
  if (on && tray_hwnd) KillTimer(tray_hwnd, PS_IDLE_TRIM_TIMER);
  if (!on && ask_waiting[0] && tray_hwnd) PostMessageW(tray_hwnd, WM_PS_ASK, 0, 0);
}

static void tray_window_made(void) {
  if (tray_hwnd) return;
  WNDCLASSEXW wc = {sizeof wc};
  wc.lpfnWndProc = tray_proc;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.lpszClassName = ps_class();
  RegisterClassExW(&wc);
  // (a top-level window never shown, not one of HWND_MESSAGE: Explorer's TaskbarCreated is broadcast to top-level ones)
  tray_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, ps_class(), L"PinShot", WS_POPUP, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
}

void ps_capture_ask(void) { capture_asked("menu"); }

void ps_rec_sweep_temp(void);

extern void mb_render_set_atlas_size(int size);
extern void mb_render_set_video_support(int on);

int32_t ps_start(int32_t again) {
  mb_render_set_atlas_size(1024);
  mb_render_set_video_support(0);
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  wchar_t run_name[96];
  _snwprintf(run_name, 95, L"Local\\PinShot.TestRun%ls", test_id());
  run_name[95] = 0;
  one_run = CreateMutexW(NULL, TRUE, test_run() ? run_name : L"Local\\PinShot.OneRun");
  if (one_run && GetLastError() == ERROR_ALREADY_EXISTS) {
    HWND other = FindWindowW(ps_class(), NULL);
    if (other) {
      DWORD pid = 0;
      GetWindowThreadProcessId(other, &pid);
      AllowSetForegroundWindow(pid);
      PostMessageW(other, WM_PS_AGAIN, (WPARAM)again, 0);
    }
    return 0;
  }
  tray_window_made();
  if (!_wgetenv(L"MBUI_SHADERS")) {
    wchar_t local[MAX_PATH] = {0}, app_dir[MAX_PATH * 2] = {0}, sh_dir[MAX_PATH * 2] = {0};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n > 0 && n < MAX_PATH - 40) {
      _snwprintf(app_dir, MAX_PATH * 2 - 1, L"%ls\\PinShot", local);
      CreateDirectoryW(app_dir, NULL);
      _snwprintf(sh_dir, MAX_PATH * 2 - 1, L"%ls\\shaders", app_dir);
      CreateDirectoryW(sh_dir, NULL);
      _wputenv_s(L"MBUI_SHADERS", sh_dir);
    }
  }
  taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
  ChangeWindowMessageFilterEx(tray_hwnd, taskbar_created, MSGFLT_ALLOW, NULL);
  tray_put();
  atexit(tray_gone);
  if (!test_run()) {
    RegisterApplicationRestart(L"", RESTART_NO_CRASH | RESTART_NO_HANG);
  }
  ps_rec_sweep_temp();
  const char *how = getenv("PINSHOT_CAPTURE");
  wgc_off = how && !strcmp(how, "gdi");
  if (!wgc_off) {
    power_note = RegisterPowerSettingNotification(tray_hwnd, &display_state_guid, DEVICE_NOTIFY_WINDOW_HANDLE);
    wts_note = WTSRegisterSessionNotification(tray_hwnd, NOTIFY_FOR_THIS_SESSION) ? 1 : 0;
  }
  return 1;
}

extern void *mb_render_context(void);
extern void *mb_render_device(void);
extern void mb_render_trim(void);
void ps_wgc_set_keep(int keep);
void ps_gpu_flush(void);
int ps_capture_keep_on(void) { return keep_gpu_warm; }

void ps_set_capture_keep(int32_t keep) {
  keep_gpu_warm = keep ? 1 : 0;
  if (!wgc_off) ps_wgc_set_keep(keep);
  if (!keep && tray_hwnd) {
    KillTimer(tray_hwnd, PS_IDLE_TRIM_TIMER);
    PostMessageW(tray_hwnd, WM_PS_TRIM, 0, 0);
  }
}

void ps_tray_note(moonbit_bytes_t title, moonbit_bytes_t text) {
  if (!tray_on) return;
  NOTIFYICONDATAW n = tray;
  n.uFlags = NIF_INFO;
  n.dwInfoFlags = NIIF_INFO;
  wchar_t *t = ps_wide((const char *)title), *b = ps_wide((const char *)text);
  lstrcpynW(n.szInfoTitle, t, 64);
  lstrcpynW(n.szInfo, b, 256);
  free(t);
  free(b);
  Shell_NotifyIconW(NIM_MODIFY, &n);
}

int32_t ps_tray_on(void) { return tray_on; }

int32_t ps_hotkey(int32_t which, int32_t mods, int32_t vk) {
  if (which <= 0 || which >= HK_MOST || !tray_hwnd) return 0;
  if (hk_on[which]) UnregisterHotKey(tray_hwnd, which);
  hk_on[which] = 0;
  if (vk == 0 || test_run()) return 1;
  hk_on[which] = RegisterHotKey(tray_hwnd, which, (UINT)mods | MOD_NOREPEAT, (UINT)vk) ? 1 : 0;
  return hk_on[which];
}

static HHOOK key_hook;
static int key_mods;

static void key_record_end(const char *how, int mods, int vk) {
  if (key_hook) {
    UnhookWindowsHookEx(key_hook);
    key_hook = NULL;
  }
  key_mods = 0;
  char json[96];
  snprintf(json, sizeof json, "{\"do\":\"key-pressed\",\"how\":\"%s\",\"mods\":%d,\"vk\":%d}", how, mods, vk);
  ps_tell(json);
}

static int key_mod_of(DWORD vk) {
  switch (vk) {
    case VK_MENU: case VK_LMENU: case VK_RMENU: return MOD_ALT;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return MOD_CONTROL;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return MOD_SHIFT;
    case VK_LWIN: case VK_RWIN: return MOD_WIN;
  }
  return 0;
}

static LRESULT CALLBACK key_hook_proc(int code, WPARAM wp, LPARAM lp) {
  if (code != HC_ACTION || !key_hook) return CallNextHookEx(NULL, code, wp, lp);
  const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
  if (k->flags & LLKHF_INJECTED) return CallNextHookEx(NULL, code, wp, lp);
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  if (pid != GetCurrentProcessId()) {
    key_record_end("cancel", 0, 0);
    return CallNextHookEx(NULL, code, wp, lp);
  }
  int down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
  int mod = key_mod_of(k->vkCode);
  if (mod) {
    if (down) key_mods |= mod;
    else key_mods &= ~mod;
    return 1;
  }
  if (!down) return 1;
  int vk = (int)k->vkCode;
  if (vk == VK_ESCAPE && key_mods == 0) key_record_end("cancel", 0, 0);
  else if ((vk == VK_BACK || vk == VK_DELETE) && key_mods == 0) key_record_end("clear", 0, 0);
  else key_record_end("key", key_mods, vk);
  return 1;
}

int32_t ps_key_record(int32_t on) {
  if (!on) {
    if (key_hook) UnhookWindowsHookEx(key_hook);
    key_hook = NULL;
    key_mods = 0;
    return 1;
  }
  if (key_hook) return 1;
  key_mods = 0;
  if (GetAsyncKeyState(VK_MENU) & 0x8000) key_mods |= MOD_ALT;
  if (GetAsyncKeyState(VK_CONTROL) & 0x8000) key_mods |= MOD_CONTROL;
  if (GetAsyncKeyState(VK_SHIFT) & 0x8000) key_mods |= MOD_SHIFT;
  if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) key_mods |= MOD_WIN;
  key_hook = SetWindowsHookExW(WH_KEYBOARD_LL, key_hook_proc, GetModuleHandleW(NULL), 0);
  return key_hook ? 1 : 0;
}

moonbit_bytes_t ps_key_name(int32_t vk) {
  wchar_t name[64] = {0};
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
    name[0] = (wchar_t)vk;
  } else if (vk >= VK_F1 && vk <= VK_F24) {
    _snwprintf(name, 63, L"F%d", vk - VK_F1 + 1);
  } else {
    const wchar_t *fixed = NULL;
    switch (vk) {
      case VK_SNAPSHOT: fixed = L"PrtSc"; break;
      case VK_SPACE: fixed = L"Space"; break;
      case VK_RETURN: fixed = L"Enter"; break;
      case VK_TAB: fixed = L"Tab"; break;
      case VK_PAUSE: fixed = L"Pause"; break;
      case VK_INSERT: fixed = L"Insert"; break;
      case VK_HOME: fixed = L"Home"; break;
      case VK_END: fixed = L"End"; break;
      case VK_PRIOR: fixed = L"PgUp"; break;
      case VK_NEXT: fixed = L"PgDn"; break;
      case VK_LEFT: fixed = L"\x2190"; break;
      case VK_UP: fixed = L"\x2191"; break;
      case VK_RIGHT: fixed = L"\x2192"; break;
      case VK_DOWN: fixed = L"\x2193"; break;
    }
    if (fixed) {
      lstrcpynW(name, fixed, 64);
    } else {
      UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC_EX);
      LONG lp = (LONG)((sc & 0xFF) << 16);
      if (sc & 0xFF00) lp |= 1 << 24;
      if (!GetKeyNameTextW(lp, name, 63)) _snwprintf(name, 63, L"Key %d", vk);
    }
  }
  return ps_utf8_bytes(name);
}

static LRESULT CALLBACK own_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR wid) {
  (void)id;
  switch (msg) {
    case WM_QUERYENDSESSION:
      return TRUE;
    case WM_ENDSESSION:
      if (wp) ps_quit();
      return 0;
    case WM_DPICHANGED:
      InvalidateRect(h, NULL, FALSE);
      return 0;
    case WM_CLOSE: {
      if (GetPropW(h, L"PinShot.Closing")) break;
      char json[64];
      snprintf(json, sizeof json, "{\"do\":\"close\",\"wid\":%d}", (int)wid);
      ps_tell(json);
      return 0;
    }
    case WM_ACTIVATE: {
      int on = LOWORD(wp) != WA_INACTIVE;
      int other = 0;
      if (!on && lp) other = (int)(INT_PTR)GetPropW((HWND)lp, L"PinShot.Wid");
      char json[96];
      snprintf(json, sizeof json, "{\"do\":\"active\",\"wid\":%d,\"on\":%d,\"other\":%d}", (int)wid, on, other);
      ps_tell(json);
      if (wid == 0) {
        InvalidateRect(h, NULL, FALSE);
        break;
      }
      SetPropW(h, L"PinShot.Frames", (HANDLE)(INT_PTR)(on ? 30 : 2));
      SetTimer(h, 0x5053, 16, NULL);
      break;
    }
    case WM_PS_MOVE: {
      // (the system's move, as a title bar's, while the button is held; then the press let go of where it ends:
      // the move's loop takes the button's release, which mbui never hears)
      // (none where the button is up already: the press was a click, its release heard)
      if (GetKeyState(VK_LBUTTON) >= 0) return 0;
      POINT p;
      GetCursorPos(&p);
      ReleaseCapture();
      SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(p.x, p.y));
      GetCursorPos(&p);
      ScreenToClient(h, &p);
      PostMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(p.x, p.y));
      return 0;
    }
    case WM_TIMER:
      if (wp == 0x5053) {
        INT_PTR n = (INT_PTR)GetPropW(h, L"PinShot.Frames");
        char json[48];
        snprintf(json, sizeof json, "{\"do\":\"tick\",\"wid\":%d}", (int)wid);
        ps_tell(json);
        InvalidateRect(h, NULL, FALSE);
        if (n <= 1) {
          KillTimer(h, 0x5053);
          RemovePropW(h, L"PinShot.Frames");
        } else {
          SetPropW(h, L"PinShot.Frames", (HANDLE)(n - 1));
        }
        return 0;
      }
      break;
    case WM_USER + 0x51: {
      if (test_run()) {
        extern int mb_render_snap_win(int wid, const wchar_t *path);
        wchar_t path[MAX_PATH] = {0};
        if (!GetEnvironmentVariableW(L"PINSHOT_SNAP_PATH", path, MAX_PATH) || !path[0]) {
          wchar_t tmp[MAX_PATH] = {0};
          GetTempPathW(MAX_PATH, tmp);
          _snwprintf(path, MAX_PATH, L"%lspinshot-snap.png", tmp);
        }
        return mb_render_snap_win((int)wid, path);
      }
      break;
    }
    case WM_NCHITTEST: {
      UINT_PTR packed = (UINT_PTR)GetPropW(h, L"PinShot.HitInset");
      if (packed) {
        int il = (int)(packed & 0xFFFF);
        int it = (int)((packed >> 16) & 0xFFFF);
        int ir = (int)((packed >> 32) & 0xFFFF);
        int ib = (int)((packed >> 48) & 0xFFFF);
        RECT wr;
        GetWindowRect(h, &wr);
        int px = (int)(short)LOWORD(lp) - wr.left;
        int py = (int)(short)HIWORD(lp) - wr.top;
        if (px < il || py < it || px >= (wr.right - wr.left) - ir || py >= (wr.bottom - wr.top) - ib) {
          return HTTRANSPARENT;
        }
      }
      break;
    }
    case WM_NCDESTROY:
      KillTimer(h, 0x5053);
      RemovePropW(h, L"PinShot.Frames");
      RemovePropW(h, L"PinShot.Closing");
      RemovePropW(h, L"PinShot.Clear");
      RemovePropW(h, L"PinShot.Cloaked");
      RemovePropW(h, L"PinShot.Wid");
      RemovePropW(h, L"PinShot.Pin");
      RemovePropW(h, L"PinShot.HitInset");
      RemoveWindowSubclass(h, own_proc, 1);
      break;
  }
  return DefSubclassProc(h, msg, wp, lp);
}

static HWND own_window(int wid) { return (HWND)mb_app_hwnd_of(wid); }

void ps_move_window(int32_t wid) {
  HWND h = own_window(wid);
  if (h) PostMessageW(h, WM_PS_MOVE, 0, 0);
}

void ps_let_close(int32_t wid) {
  HWND h = own_window(wid);
  if (h) SetPropW(h, L"PinShot.Closing", (HANDLE)1);
}

void ps_hit_inset(int32_t wid, int32_t l, int32_t t, int32_t r, int32_t b) {
  HWND h = own_window(wid);
  if (!h) return;
  if (l <= 0 && t <= 0 && r <= 0 && b <= 0) {
    RemovePropW(h, L"PinShot.HitInset");
    return;
  }
  UINT_PTR packed = ((UINT_PTR)(l & 0xFFFF)) |
                    (((UINT_PTR)(t & 0xFFFF)) << 16) |
                    (((UINT_PTR)(r & 0xFFFF)) << 32) |
                    (((UINT_PTR)(b & 0xFFFF)) << 48);
  SetPropW(h, L"PinShot.HitInset", (HANDLE)packed);
}

extern void mb_app_window_keep_cloaked(int wid, int on);
extern void mb_render_set_clear(int wid, int on);

static void test_cloak(HWND h, int wid) {
  static int cloaked = -1;
  if (cloaked < 0) cloaked = getenv("PINSHOT_TEST") && getenv("PINSHOT_TEST_CLOAKED") ? 1 : 0;
  if (!cloaked || !h || wid == 0) return;
  BOOL b = TRUE;
  DwmSetWindowAttribute(h, DWMWA_CLOAK, &b, sizeof b);
  mb_app_window_keep_cloaked(wid, 1);
}

int32_t ps_own(int32_t wid, int32_t topmost) {
  HWND h = own_window(wid);
  if (!h) return 0;
  test_cloak(h, wid);
  LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
  style &= ~(LONG_PTR)(WS_OVERLAPPEDWINDOW);
  style |= WS_POPUP;
  SetWindowLongPtrW(h, GWL_STYLE, style);
  LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
  ex &= ~(LONG_PTR)WS_EX_APPWINDOW;
  ex |= WS_EX_TOOLWINDOW;
  if (topmost == 2) ex |= WS_EX_NOACTIVATE;
  SetWindowLongPtrW(h, GWL_EXSTYLE, ex);
  SetPropW(h, L"PinShot.Wid", (HANDLE)(INT_PTR)wid);
  DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
  DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
  COLORREF no_border = 0xFFFFFFFEu;
  DwmSetWindowAttribute(h, 34 /* DWMWA_BORDER_COLOR */, &no_border, sizeof no_border);
  BOOL no = TRUE;
  DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &no, sizeof no);
  SetWindowSubclass(h, own_proc, 1, (DWORD_PTR)wid);
  SetWindowPos(h, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  if (wid == 0 && !keep_gpu_warm) ps_gpu_flush();
  return 1;
}

// Window `wid` given DWM's shadow round it (a pin: it stands off what is under it). A popup has none of its own; DWM
// draws one round a window whose frame it renders and that reaches into the client by a pixel.
void ps_shadow(int32_t wid) {
  HWND h = own_window(wid);
  if (!h) return;
  SetPropW(h, L"PinShot.Pin", (HANDLE)1);
  enum DWMNCRENDERINGPOLICY p = DWMNCRP_ENABLED;
  DwmSetWindowAttribute(h, DWMWA_NCRENDERING_POLICY, &p, sizeof p);
  MARGINS m = {1, 1, 1, 1};
  DwmExtendFrameIntoClientArea(h, &m);
}

void ps_toast_place(int32_t wid, int32_t w, int32_t h) {
  HWND hw = own_window(wid);
  if (!hw) return;
  test_cloak(hw, wid);
  mb_render_set_clear(wid, 1);
  LONG_PTR ex = GetWindowLongPtrW(hw, GWL_EXSTYLE);
  if ((ex & (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) != (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) {
    SetWindowLongPtrW(hw, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
  }
  DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
  DwmSetWindowAttribute(hw, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
  COLORREF no_border = 0xFFFFFFFEu;
  DwmSetWindowAttribute(hw, 34 /* DWMWA_BORDER_COLOR */, &no_border, sizeof no_border);
  POINT p;
  GetCursorPos(&p);
  HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof mi};
  GetMonitorInfoW(mon, &mi);
  UINT dx = monitor_dpi(mon);
  int ww = MulDiv(w, (int)dx, 96), wh = MulDiv(h, (int)dx, 96);
  RECT a = mi.rcWork;
  int x = p.x - ww / 2, y = p.y - MulDiv(4, (int)dx, 96);
  if (y + wh > a.bottom) y = p.y - wh + MulDiv(18, (int)dx, 96);
  if (x < a.left) x = a.left;
  if (x + ww > a.right) x = a.right - ww;
  if (y < a.top) y = a.top;
  if (y + wh > a.bottom) y = a.bottom - wh;
  SetWindowPos(hw, HWND_TOPMOST, x, y, ww, wh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void force_foreground(HWND hw) {
  SetForegroundWindow(hw);
  if (GetForegroundWindow() != hw) {
    HWND fg = GetForegroundWindow();
    DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    DWORD cur_tid = GetCurrentThreadId();
    if (fg_tid && fg_tid != cur_tid) {
      AttachThreadInput(cur_tid, fg_tid, TRUE);
      BringWindowToTop(hw);
      SetForegroundWindow(hw);
      SetFocus(hw);
      AttachThreadInput(cur_tid, fg_tid, FALSE);
      return;
    }
  }
  SetFocus(hw);
}

void ps_place(int32_t wid, int32_t x, int32_t y, int32_t w, int32_t h, int32_t show, int32_t focus) {
  HWND hw = own_window(wid);
  if (!hw) return;
  if (show) test_cloak(hw, wid);
  UINT f = SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_NOZORDER);
  HWND after = show && (GetWindowLongPtrW(hw, GWL_EXSTYLE) & WS_EX_TOPMOST) ? HWND_TOPMOST : (show ? HWND_TOP : NULL);
  SetWindowPos(hw, after, x, y, w, h, f);
  if (show && focus) {
    force_foreground(hw);
  }
}

void ps_center(int32_t wid, int32_t w, int32_t h) {
  HWND hw = own_window(wid);
  if (!hw) return;
  test_cloak(hw, wid);
  POINT p;
  GetCursorPos(&p);
  HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof mi};
  GetMonitorInfoW(mon, &mi);
  UINT dx = monitor_dpi(mon);
  BOOL dark = TRUE;
  DwmSetWindowAttribute(hw, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
  RECT r = {0, 0, MulDiv(w, (int)dx, 96), MulDiv(h, (int)dx, 96)};
  AdjustWindowRectExForDpi(&r, (DWORD)GetWindowLongPtrW(hw, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(hw, GWL_EXSTYLE), dx);
  int ww = r.right - r.left, wh = r.bottom - r.top;
  RECT a = mi.rcWork;
  if (wh > a.bottom - a.top - 24) wh = a.bottom - a.top - 24;
  int x = a.left + ((a.right - a.left) - ww) / 2, y = a.top + ((a.bottom - a.top) - wh) / 2;
  SetWindowPos(hw, NULL, x, y, ww, wh, SWP_NOZORDER | SWP_SHOWWINDOW);
  SetForegroundWindow(hw);
}

extern void mb_app_reveal_after_frame(int wid);
extern void mb_app_cancel_reveal(int wid);

void ps_hide(int32_t wid) {
  if (wid == 0) mb_app_cancel_reveal(0);
  HWND h = own_window(wid);
  if (h) ShowWindow(h, SW_HIDE);
}

void ps_cloak(int32_t wid, int32_t on) {
  HWND h = own_window(wid);
  if (!h) return;
  if (wid == 0 && !on) {
    mb_app_reveal_after_frame(0);
    return;
  }
  if (wid == 0) mb_app_cancel_reveal(0);
  BOOL b = on ? TRUE : FALSE;
  DwmSetWindowAttribute(h, DWMWA_CLOAK, &b, sizeof b);
  if (wid != 0) {
    mb_app_window_keep_cloaked(wid, on ? 1 : 0);
  }
}

// Whether window `wid` is on the screen: shown and uncloaked (Invar uncloaks a new window once the compositor has
// taken its first frame). A test run's windows stay cloaked: shown is enough there.
int32_t ps_win_revealed(int32_t wid) {
  HWND h = own_window(wid);
  if (!h || !IsWindowVisible(h)) return 0;
  const char *cl = getenv("PINSHOT_TEST_CLOAKED");
  if (cl && cl[0]) return 1;
  BOOL cloaked = FALSE;
  if (FAILED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked))) return 1;
  return cloaked ? 0 : 1;
}

void ps_topmost(int32_t wid, int32_t on, int32_t front) {
  HWND h = own_window(wid);
  if (!h) return;
  UINT flags = SWP_NOMOVE | SWP_NOSIZE | (front ? SWP_SHOWWINDOW : SWP_NOACTIVATE);
  SetWindowPos(h, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, flags);
  if (front) {
    if (!on) SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    force_foreground(h);
    SetPropW(h, L"PinShot.Frames", (HANDLE)(INT_PTR)30);
    SetTimer(h, 0x5053, 16, NULL);
  }
}

void ps_opacity(int32_t wid, int32_t alpha) {
  HWND h = own_window(wid);
  if (!h) return;
  LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
  if (alpha >= 255) {
    if (ex & WS_EX_LAYERED) SetWindowLongPtrW(h, GWL_EXSTYLE, ex & ~(LONG_PTR)WS_EX_LAYERED);
    return;
  }
  if (!(ex & WS_EX_LAYERED)) SetWindowLongPtrW(h, GWL_EXSTYLE, ex | WS_EX_LAYERED);
  SetLayeredWindowAttributes(h, 0, (BYTE)(alpha < 20 ? 20 : alpha), LWA_ALPHA);
}

moonbit_bytes_t ps_window_rect(int32_t wid) {
  HWND h = own_window(wid);
  RECT r = {0};
  if (!h || !GetWindowRect(h, &r)) return moonbit_make_bytes(0, 0);
  char s[96];
  snprintf(s, sizeof s, "%ld,%ld,%ld,%ld,%u", r.left, r.top, r.right - r.left, r.bottom - r.top, GetDpiForWindow(h));
  return ps_text(s);
}

int32_t ps_shift_down(void) { return GetKeyState(VK_SHIFT) < 0; }

int32_t ps_reduce_motion(void) {
  const char *env = getenv("PINSHOT_REDUCE_MOTION");
  if (env && env[0] && env[0] != '0') return 1;
  BOOL anim = TRUE;
  if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0) && !anim) return 1;
  return 0;
}

moonbit_bytes_t ps_pointer(void) {
  POINT p = {0, 0};
  GetCursorPos(&p);
  char s[48];
  snprintf(s, sizeof s, "%ld,%ld", p.x, p.y);
  return ps_text(s);
}

static void gpu_trim_now(int force_trim) {
  mb_render_trim();
  ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)mb_render_context();
  if (ctx) {
    ID3D11DeviceContext_ClearState(ctx);
    ID3D11DeviceContext_Flush(ctx);
  }
  if (!keep_gpu_warm || force_trim) {
    ID3D11Device *dev = (ID3D11Device *)mb_render_device();
    IDXGIDevice3 *xd = NULL;
    if (dev && SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice3, (void **)&xd))) {
      IDXGIDevice3_Trim(xd);
      IDXGIDevice3_Release(xd);
    }
    if (!wgc_off) ps_wgc_trim();
  }
}

void ps_gpu_flush(void) {
  if (tray_hwnd) SetTimer(tray_hwnd, PS_TRIM_SOON_TIMER, 300, NULL);
}

moonbit_bytes_t ps_memory(void) {
  PROCESS_MEMORY_COUNTERS_EX pm;
  memset(&pm, 0, sizeof pm);
  pm.cb = sizeof pm;
  typedef BOOL(WINAPI * GetPmi)(HANDLE, PROCESS_MEMORY_COUNTERS *, DWORD);
  static GetPmi get;
  if (!get) get = (GetPmi)GetProcAddress(LoadLibraryW(L"kernel32.dll"), "K32GetProcessMemoryInfo");
  if (get) get(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
  char s[64];
  snprintf(s, sizeof s, "%llu,%llu", (unsigned long long)pm.WorkingSetSize, (unsigned long long)pm.PrivateUsage);
  return ps_text(s);
}

moonbit_bytes_t ps_data_dir(void) {
  static wchar_t dir[MAX_PATH * 2];
  if (!dir[0]) {
    wchar_t *p = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &p))) {
      if (test_run()) _snwprintf(dir, MAX_PATH * 2 - 1, L"%ls\\PinShot-test%ls", p, test_id());
      else _snwprintf(dir, MAX_PATH * 2 - 1, L"%ls\\PinShot", p);
      CoTaskMemFree(p);
    }
    if (dir[0]) CreateDirectoryW(dir, NULL);
  }
  return ps_utf8_bytes(dir);
}

moonbit_bytes_t ps_wallpaper(void) {
  wchar_t path[MAX_PATH * 2] = {0};
  if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH * 2, path, 0) || !path[0] ||
      GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
    return moonbit_make_bytes(0, 0);
  return ps_utf8_bytes(path);
}

moonbit_bytes_t ps_pictures_dir(void) {
  wchar_t *p = NULL;
  wchar_t dir[MAX_PATH * 2] = {0};
  if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Pictures, 0, NULL, &p))) {
    _snwprintf(dir, MAX_PATH * 2 - 1, L"%ls\\PinShot", p);
    CoTaskMemFree(p);
  }
  return ps_utf8_bytes(dir);
}

moonbit_bytes_t ps_read_file(moonbit_bytes_t path) {
  wchar_t *w = ps_wide((const char *)path);
  HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  free(w);
  if (f == INVALID_HANDLE_VALUE) return moonbit_make_bytes(0, 0);
  LARGE_INTEGER sz;
  if (!GetFileSizeEx(f, &sz) || sz.QuadPart > 512LL * 1024 * 1024) {
    CloseHandle(f);
    return moonbit_make_bytes(0, 0);
  }
  moonbit_bytes_t b = moonbit_make_bytes((int32_t)sz.QuadPart, 0);
  DWORD got = 0;
  BOOL ok = ReadFile(f, b, (DWORD)sz.QuadPart, &got, NULL);
  CloseHandle(f);
  if (!ok || got != (DWORD)sz.QuadPart) return moonbit_make_bytes(0, 0);
  return b;
}

void ps_delete_file(moonbit_bytes_t path) {
  wchar_t *w = ps_wide((const char *)path);
  if (w) {
    DeleteFileW(w);
    free(w);
  }
}

int32_t ps_rect_shown(int32_t x, int32_t y, int32_t w, int32_t h) {
  RECT r = {x, y, x + w, y + h};
  if (w <= 0 || h <= 0) return 0;
  HMONITOR m = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
  if (!m) return 0;
  MONITORINFO mi = {sizeof mi};
  if (!GetMonitorInfoW(m, &mi)) return 0;
  RECT in;
  // (enough of it on a screen to take hold of: a quarter of its width and its top 24 pixels)
  if (!IntersectRect(&in, &r, &mi.rcWork)) return 0;
  return (in.right - in.left) * 4 >= w && in.top <= y + 24 && in.bottom - in.top >= (h < 24 ? h : 24);
}

int32_t ps_write_file(moonbit_bytes_t path, moonbit_bytes_t data) {
  wchar_t *w = ps_wide((const char *)path);
  size_t n = wcslen(w);
  for (size_t i = 0; i < n; i++)
    if (w[i] == L'\\' && i > 2) {
      w[i] = 0;
      CreateDirectoryW(w, NULL);
      w[i] = L'\\';
    }
  wchar_t *tmp = (wchar_t *)malloc((n + 8) * sizeof(wchar_t));
  _snwprintf(tmp, n + 8, L"%ls.part", w);
  HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
  int ok = 0;
  if (f != INVALID_HANDLE_VALUE) {
    DWORD put = 0;
    DWORD len = (DWORD)Moonbit_array_length(data);
    ok = WriteFile(f, data, len, &put, NULL) && put == len;
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp, w, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp);
  }
  free(tmp);
  free(w);
  return ok;
}

moonbit_bytes_t ps_save_dialog(int32_t wid, moonbit_bytes_t dir, moonbit_bytes_t name) {
  IFileSaveDialog *d = NULL;
  moonbit_bytes_t out = moonbit_make_bytes(0, 0);
  if (FAILED(CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileSaveDialog, (void **)&d)))
    return out;
  COMDLG_FILTERSPEC types[] = {{L"PNG (*.png)", L"*.png"}, {L"JPEG (*.jpg)", L"*.jpg;*.jpeg"}};
  IFileSaveDialog_SetFileTypes(d, 2, types);
  const char *ns = (const char *)name;
  size_t nl = strlen(ns);
  int is_jpg = nl >= 4 && _stricmp(ns + nl - 4, ".jpg") == 0;
  IFileSaveDialog_SetFileTypeIndex(d, is_jpg ? 2 : 1);
  IFileSaveDialog_SetDefaultExtension(d, is_jpg ? L"jpg" : L"png");
  wchar_t *wn = ps_wide((const char *)name);
  IFileSaveDialog_SetFileName(d, wn);
  free(wn);
  wchar_t *wd = ps_wide((const char *)dir);
  CreateDirectoryW(wd, NULL);
  IShellItem *folder = NULL;
  if (SUCCEEDED(SHCreateItemFromParsingName(wd, NULL, &IID_IShellItem, (void **)&folder))) {
    IFileSaveDialog_SetFolder(d, folder);
    IShellItem_Release(folder);
  }
  free(wd);
  HWND owner = wid >= 0 ? own_window(wid) : NULL;
  if (SUCCEEDED(IFileSaveDialog_Show(d, owner))) {
    IShellItem *item = NULL;
    if (SUCCEEDED(IFileSaveDialog_GetResult(d, &item))) {
      wchar_t *p = NULL;
      if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &p))) {
        out = ps_utf8_bytes(p);
        CoTaskMemFree(p);
      }
      IShellItem_Release(item);
    }
  }
  IFileSaveDialog_Release(d);
  return out;
}

moonbit_bytes_t ps_pick_folder(int32_t wid, moonbit_bytes_t dir) {
  IFileOpenDialog *d = NULL;
  moonbit_bytes_t out = moonbit_make_bytes(0, 0);
  if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
    return out;
  DWORD opts = 0;
  if (SUCCEEDED(IFileOpenDialog_GetOptions(d, &opts)))
    IFileOpenDialog_SetOptions(d, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  wchar_t *wd = ps_wide((const char *)dir);
  CreateDirectoryW(wd, NULL);
  IShellItem *folder = NULL;
  if (SUCCEEDED(SHCreateItemFromParsingName(wd, NULL, &IID_IShellItem, (void **)&folder))) {
    IFileOpenDialog_SetFolder(d, folder);
    IShellItem_Release(folder);
  }
  free(wd);
  HWND owner = wid >= 0 ? own_window(wid) : NULL;
  if (SUCCEEDED(IFileOpenDialog_Show(d, owner))) {
    IShellItem *item = NULL;
    if (SUCCEEDED(IFileOpenDialog_GetResult(d, &item))) {
      wchar_t *p = NULL;
      if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &p))) {
        out = ps_utf8_bytes(p);
        CoTaskMemFree(p);
      }
      IShellItem_Release(item);
    }
  }
  IFileOpenDialog_Release(d);
  return out;
}

void ps_reveal(moonbit_bytes_t path) {
  wchar_t *w = ps_wide((const char *)path);
  DWORD a = GetFileAttributesW(w);
  if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
    wchar_t args[MAX_PATH * 2 + 16];
    _snwprintf(args, MAX_PATH * 2 + 15, L"/select,\"%ls\"", w);
    ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
  } else {
    CreateDirectoryW(w, NULL);
    ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
  }
  free(w);
}

void ps_log(moonbit_bytes_t line) {
  static wchar_t path[MAX_PATH * 2];
  if (!path[0]) {
    wchar_t *p = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &p))) return;
    _snwprintf(path, MAX_PATH * 2 - 1, L"%ls\\PinShot", p);
    CoTaskMemFree(p);
    CreateDirectoryW(path, NULL);
    if (test_run()) {
      wchar_t name[64];
      _snwprintf(name, 63, L"\\pinshot-test%ls.log", test_id());
      name[63] = 0;
      wcscat_s(path, MAX_PATH * 2, name);
    } else {
      wcscat_s(path, MAX_PATH * 2, L"\\pinshot.log");
    }
  }
  static HANDLE f = INVALID_HANDLE_VALUE;
  static LONGLONG size = -1;
  if (f != INVALID_HANDLE_VALUE && size > 1024 * 1024) {
    CloseHandle(f);
    f = INVALID_HANDLE_VALUE;
    DeleteFileW(path);
  }
  if (f == INVALID_HANDLE_VALUE) {
    f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS, 0,
                    NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    size = GetFileSizeEx(f, &sz) ? sz.QuadPart : 0;
    if (size > 1024 * 1024) {
      CloseHandle(f);
      f = INVALID_HANDLE_VALUE;
      DeleteFileW(path);
      f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS,
                      0, NULL);
      if (f == INVALID_HANDLE_VALUE) return;
      size = 0;
    }
  }
  SYSTEMTIME t;
  GetLocalTime(&t);
  size_t ll = strlen((const char *)line);
  char stack[512];
  char *buf = ll + 64 <= sizeof stack ? stack : (char *)malloc(ll + 64);
  if (!buf) return;
  int n = snprintf(buf, 48, "%02d-%02d %02d:%02d:%02d.%03d ", t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                   t.wMilliseconds);
  memcpy(buf + n, line, ll);
  memcpy(buf + n + ll, "\r\n", 2);
  DWORD put = 0;
  WriteFile(f, buf, (DWORD)(n + ll + 2), &put, NULL);
  size += put;
  if (buf != stack) free(buf);
}

moonbit_bytes_t ps_stamp(void) {
  SYSTEMTIME t;
  GetLocalTime(&t);
  char s[32];
  snprintf(s, sizeof s, "%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  return ps_text(s);
}

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

void ps_wgc_wake(void) {
  if (tray_hwnd) PostMessageW(tray_hwnd, WM_DISPLAYCHANGE, 0, 0);
}

int ps_packaged(void);
int ps_startup_on(void);
int ps_startup_set(int on);

int32_t ps_autostart(void) {
  if (ps_packaged()) return ps_startup_on();
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
  DWORD type = 0, size = 0;
  LONG r = RegQueryValueExW(k, L"PinShot", NULL, &type, NULL, &size);
  RegCloseKey(k);
  return r == ERROR_SUCCESS && size > 0;
}

int32_t ps_set_autostart(int32_t on) {
  if (ps_packaged()) return ps_startup_set(on);
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return 0;
  LONG r;
  if (on) {
    wchar_t exe[MAX_PATH], v[MAX_PATH + 4];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    _snwprintf(v, MAX_PATH + 3, L"\"%ls\"", exe);
    v[MAX_PATH + 3] = 0;
    r = RegSetValueExW(k, L"PinShot", 0, REG_SZ, (const BYTE *)v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
  } else {
    r = RegDeleteValueW(k, L"PinShot");
    if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
  }
  RegCloseKey(k);
  return r == ERROR_SUCCESS;
}

moonbit_bytes_t ps_args(void) {
  int argc = 0;
  LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return moonbit_make_bytes(0, 0);
  size_t total = 0;
  for (int i = 1; i < argc; i++) total += wcslen(argv[i]) * 3 + 1;
  char *out = (char *)malloc(total + 1);
  size_t len = 0;
  for (int i = 1; i < argc; i++) {
    int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, out + len, (int)(total + 1 - len), NULL, NULL);
    if (n > 0) len += (size_t)n - 1;
    out[len++] = '\n';
  }
  LocalFree(argv);
  moonbit_bytes_t b = ps_bytes(out, len);
  free(out);
  return b;
}

moonbit_bytes_t ps_popup_menu(int32_t wid, moonbit_bytes_t lines) {
  HWND owner = own_window(wid);
  if (!owner) owner = tray_hwnd;
  HMENU m = CreatePopupMenu();
  char keys[32][32];
  int n = 0;
  int len = (int)Moonbit_array_length(lines);
  const char *s = (const char *)lines;
  int at = 0;
  while (at < len && n < 32) {
    int end = at;
    while (end < len && s[end] != '\n') end++;
    char line[512] = {0};
    int ll = end - at < 511 ? end - at : 511;
    memcpy(line, s + at, (size_t)ll);
    at = end + 1;
    if (!strcmp(line, "-")) {
      AppendMenuW(m, MF_SEPARATOR, 0, NULL);
      continue;
    }
    char *t1 = strchr(line, '\t');
    if (!t1) continue;
    *t1 = 0;
    char *t2 = strchr(t1 + 1, '\t');
    const char *flags = "";
    if (t2) {
      *t2 = 0;
      flags = t2 + 1;
    }
    lstrcpynA(keys[n], line, 32);
    wchar_t *w = ps_wide(t1 + 1);
    UINT f = MF_STRING;
    if (strchr(flags, 'c')) f |= MF_CHECKED;
    if (strchr(flags, 'g')) f |= MF_GRAYED;
    AppendMenuW(m, f, (UINT_PTR)(n + 1), w);
    free(w);
    n++;
  }
  POINT p;
  GetCursorPos(&p);
  SetForegroundWindow(owner);
  int chose = (int)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, p.x, p.y, owner, NULL);
  PostMessageW(owner, WM_NULL, 0, 0);
  DestroyMenu(m);
  if (chose > 0 && chose <= n) return ps_text(keys[chose - 1]);
  return moonbit_make_bytes(0, 0);
}

#pragma pack(push, 1)
typedef struct {
  uint16_t type;
  uint32_t size;
  uint32_t reserved;
  uint32_t offset;
} BmpFileHead;
#pragma pack(pop)

moonbit_bytes_t ps_clipboard_image(void) {
  moonbit_bytes_t out = moonbit_make_bytes(0, 0);
  if (!OpenClipboard(tray_hwnd)) return out;
  UINT png = RegisterClipboardFormatW(L"PNG");
  HANDLE h = GetClipboardData(png);
  if (h) {
    void *p = GlobalLock(h);
    if (p) {
      out = ps_bytes(p, GlobalSize(h));
      GlobalUnlock(h);
    }
  } else if ((h = GetClipboardData(CF_DIB)) != NULL) {
    BITMAPINFOHEADER *bi = (BITMAPINFOHEADER *)GlobalLock(h);
    if (bi) {
      size_t n = GlobalSize(h);
      if (n >= sizeof(BITMAPINFOHEADER) && bi->biSize >= sizeof(BITMAPINFOHEADER) && bi->biSize <= n &&
          n <= 0x7FFFFFFFu - sizeof(BmpFileHead) && bi->biClrUsed <= 256 && bi->biBitCount <= 64) {
        size_t colors = bi->biClrUsed ? bi->biClrUsed : (bi->biBitCount <= 8 ? (size_t)1 << bi->biBitCount : 0);
        size_t masks = (bi->biCompression == BI_BITFIELDS && bi->biSize == sizeof(BITMAPINFOHEADER)) ? 12 : 0;
        size_t off = sizeof(BmpFileHead) + bi->biSize + masks + colors * 4;
        if (off <= sizeof(BmpFileHead) + n) {
          out = moonbit_make_bytes((int32_t)(sizeof(BmpFileHead) + n), 0);
          BmpFileHead fh = {0x4D42, (uint32_t)(sizeof(BmpFileHead) + n), 0, (uint32_t)off};
          memcpy(out, &fh, sizeof fh);
          memcpy(out + sizeof fh, bi, n);
        }
      }
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return out;
}

void ps_run_handover(void) {
  tray_gone();
  tray_on = 0;
  for (int i = 1; i < HK_MOST; i++)
    if (hk_on[i]) {
      UnregisterHotKey(tray_hwnd, i);
      hk_on[i] = 0;
    }
  if (one_run) {
    ReleaseMutex(one_run);
    CloseHandle(one_run);
    one_run = NULL;
  }
  if (power_note) UnregisterPowerSettingNotification(power_note);
  power_note = NULL;
  if (wts_note && tray_hwnd) WTSUnRegisterSessionNotification(tray_hwnd);
  wts_note = 0;
  if (tray_hwnd) {
    DestroyWindow(tray_hwnd);
    tray_hwnd = NULL;
  }
}

void ps_rec_quit(void);
void ps_quit(void) {
  tray_gone();
  ps_rec_quit();
  fflush(stdout);
  TerminateProcess(GetCurrentProcess(), 0);
  ExitProcess(0);
}

void ps_shutter(void) {
  PlaySoundW(L"SHUTTER", GetModuleHandleW(NULL), SND_RESOURCE | SND_ASYNC | SND_NODEFAULT);
}

int32_t ps_ui_chinese(void) { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE; }

static inline float rf_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline float rf_aa(float d, float r, float feather) {
  float t = rf_clampf((r + feather - d) / (2.0f * feather), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static inline float ps_smooth_a(float t) {
  return 1.0000098f +
         t * (-0.00106380035f +
              t * (0.0390446602f +
                   t * (1.21491043f +
                        t * (-1.49650831f +
                             t * (1.90947795f + t * (-2.77580972f + t * (2.11536546f + t * -0.591224537f)))))));
}

static inline float ps_smooth_b(float t) {
  return 1.00001404f +
         t * (-0.00136262193f +
              t * (0.039570688f +
                   t * (0.648050719f +
                        t * (-0.725922772f +
                             t * (0.327888394f + t * (-0.914717465f + t * (1.13728689f + t * -0.403984265f)))))));
}

void ps_update_region_frame(HWND hwnd, int rx, int ry, int rw, int rh, COLORREF color, int dashed, int handles) {
  if (!hwnd || rw <= 0 || rh <= 0) return;
  UINT dpi = GetDpiForWindow(hwnd);
  if (dpi < 96) dpi = 96;
  float s = (float)dpi / 96.0f;
  int pad = (int)roundf(10.0f * s);
  if (pad < 8) pad = 8;

  RECT rc = {rx, ry, rx + rw, ry + rh};
  MONITORINFO mi = {sizeof mi};
  int have_mon = 0;
  HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
  if (mon && GetMonitorInfoW(mon, &mi)) have_mon = 1;

  int wx = rx - pad, wy = ry - pad;
  int fw = rw + pad * 2, fh = rh + pad * 2;

  float outset = 2.0f * s;
  float fx0 = (float)rx - outset;
  float fy0 = (float)ry - outset;
  float fx1 = (float)(rx + rw) + outset;
  float fy1 = (float)(ry + rh) + outset;
  if (have_mon) {
    float inset = 2.0f * s;
    if (fx0 < (float)mi.rcMonitor.left + inset) fx0 = (float)mi.rcMonitor.left + inset;
    if (fy0 < (float)mi.rcMonitor.top + inset) fy0 = (float)mi.rcMonitor.top + inset;
    if (fx1 > (float)mi.rcMonitor.right - inset) fx1 = (float)mi.rcMonitor.right - inset;
    if (fy1 > (float)mi.rcMonitor.bottom - inset) fy1 = (float)mi.rcMonitor.bottom - inset;
  }
  float lx0 = fx0 - (float)wx, ly0 = fy0 - (float)wy;
  float lx1 = fx1 - (float)wx, ly1 = fy1 - (float)wy;
  float cx = (lx0 + lx1) * 0.5f, cy = (ly0 + ly1) * 0.5f;
  float hx = fmaxf(4.0f, (lx1 - lx0) * 0.5f);
  float hy = fmaxf(4.0f, (ly1 - ly0) * 0.5f);
  float room = fminf(hx, hy);
  float rad = fminf(6.0f * s, room * 0.35f);
  float rr = fminf(rad * 1.25f, room);
  float blen = fminf(fminf((float)rw, (float)rh) * 0.16f, 16.0f * s);
  blen = fmaxf(blen, rr + 3.0f * s);
  blen = fminf(blen, room * 0.45f);
  int show_ph = handles && (float)rw >= 54.0f * s;
  int show_pv = handles && (float)rh >= 54.0f * s;
  float plen = 7.0f * s;
  float pthk = 1.75f * s;

  float cr = (float)GetRValue(color) / 255.0f;
  float cg = (float)GetGValue(color) / 255.0f;
  float cb = (float)GetBValue(color) / 255.0f;
  float hr = fminf(1.0f, cr + 0.28f);
  float hg = fminf(1.0f, cg + 0.28f);
  float hb = fminf(1.0f, cb + 0.28f);

  BITMAPINFO bi;
  memset(&bi, 0, sizeof bi);
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = fw;
  bi.bmiHeader.biHeight = -fh;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  HDC scr = GetDC(NULL);
  HDC mem = CreateCompatibleDC(scr);
  void *bits = NULL;
  HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
  if (!bmp || !bits) {
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, scr);
    return;
  }
  HGDIOBJ old = SelectObject(mem, bmp);
  uint32_t *pix = (uint32_t *)bits;

  float sigma = 3.0f * s;
  float inv2s2 = 1.0f / (2.0f * sigma * sigma);
  float half_w = handles ? (0.95f * s) : (1.1f * s);
  float brk_hw = 1.65f * s;
  float hit_reach = 7.0f * s;
  float dash_per = 13.0f * s;
  float dash_len = 7.0f * s;

  float inner_pad = handles ? (hit_reach + 2.0f * s) : (brk_hw + 3.0f * s);
  int band_l = (int)ceilf(lx0 + inner_pad);
  int band_t = (int)ceilf(ly0 + inner_pad);
  int band_r = (int)floorf(lx1 - inner_pad);
  int band_b = (int)floorf(ly1 - inner_pad);
  if (band_l < 0) band_l = 0;
  if (band_t < 0) band_t = 0;
  if (band_r > fw) band_r = fw;
  if (band_b > fh) band_b = fh;

  for (int py = 0; py < fh; py++) {
    int in_y = (py >= band_t && py < band_b);
    float ay = fabsf((float)py + 0.5f - cy);
    uint32_t *row = pix + (size_t)py * (size_t)fw;
    for (int px = 0; px < fw; px++) {
      if (in_y && px == band_l && band_r > band_l) {
        px = band_r - 1;
        continue;
      }
      float ax = fabsf((float)px + 0.5f - cx);
      float qx = ax - (hx - rr);
      float qy = ay - (hy - rr);
      float ox = qx > 0.0f ? qx : 0.0f;
      float oy = qy > 0.0f ? qy : 0.0f;
      float sd;
      if (ox > 0.0f && oy > 0.0f) {
        float hi = ox > oy ? ox : oy;
        float lo = ox > oy ? oy : ox;
        float t = lo / hi;
        sd = hi * ps_smooth_a(t) - rr * ps_smooth_b(t);
      } else {
        float mq = qx > qy ? qx : qy;
        sd = sqrtf(ox * ox + oy * oy) + (mq < 0.0f ? mq : 0.0f) - rr;
      }
      float d = fabsf(sd);

      float d_brk = 1e6f;
      float d_pill = 1e6f;
      if (handles) {
        if (ax >= hx - blen && ay >= hy - blen) {
          d_brk = d;
        } else if (ax < hx - blen && ay >= hy - blen) {
          float ex = ax - (hx - blen);
          float ey = ay - hy;
          d_brk = sqrtf(ex * ex + ey * ey);
        } else if (ay < hy - blen && ax >= hx - blen) {
          float ex = ax - hx;
          float ey = ay - (hy - blen);
          d_brk = sqrtf(ex * ex + ey * ey);
        }
        if (show_ph) {
          float pex = fmaxf(0.0f, ax - (plen - pthk));
          float pey = ay - hy;
          float dh = sqrtf(pex * pex + pey * pey);
          if (dh < d_pill) d_pill = dh;
        }
        if (show_pv) {
          float pex = ax - hx;
          float pey = fmaxf(0.0f, ay - (plen - pthk));
          float dv = sqrtf(pex * pex + pey * pey);
          if (dv < d_pill) d_pill = dv;
        }
      }

      float out_r = 0.0f, out_g = 0.0f, out_b = 0.0f, out_a = 0.0f;
      if (sd >= -0.8f * s && d < 3.2f * sigma) {
        float ga = 0.28f * expf(-(d * d) * inv2s2);
        out_r = cr;
        out_g = cg;
        out_b = cb;
        out_a = ga;
      }

      float da = 0.32f * rf_aa(d, half_w + 1.35f * s, 0.7f);
      if (handles) {
        float da2 = 0.42f * rf_aa(d_brk, brk_hw + 1.35f * s, 0.7f);
        float da3 = 0.44f * rf_aa(d_pill, pthk + 1.3f * s, 0.7f);
        if (da2 > da) da = da2;
        if (da3 > da) da = da3;
      }
      if (da > 0.001f) {
        float na = da + out_a * (1.0f - da);
        if (na > 0.0001f) {
          float keep = out_a * (1.0f - da) / na;
          out_r *= keep;
          out_g *= keep;
          out_b *= keep;
        }
        out_a = na;
      }

      float sa = 0.90f * rf_aa(d, half_w, 0.65f);
      if (dashed && sa > 0.001f) {
        float coord = (qx < qy) ? ax : ay;
        float ph = fmodf(coord, dash_per);
        sa *= rf_aa(fabsf(ph - dash_len * 0.5f), dash_len * 0.5f, 0.7f);
      }
      if (sa > 0.001f) {
        float na = sa + out_a * (1.0f - sa);
        if (na > 0.0001f) {
          float inv = 1.0f / na;
          out_r = (cr * sa + out_r * out_a * (1.0f - sa)) * inv;
          out_g = (cg * sa + out_g * out_a * (1.0f - sa)) * inv;
          out_b = (cb * sa + out_b * out_a * (1.0f - sa)) * inv;
        }
        out_a = na;
      }

      if (handles) {
        float ba = rf_aa(d_brk, brk_hw, 0.65f);
        if (ba > 0.001f) {
          float na = ba + out_a * (1.0f - ba);
          if (na > 0.0001f) {
            float inv = 1.0f / na;
            out_r = (hr * ba + out_r * out_a * (1.0f - ba)) * inv;
            out_g = (hg * ba + out_g * out_a * (1.0f - ba)) * inv;
            out_b = (hb * ba + out_b * out_a * (1.0f - ba)) * inv;
          }
          out_a = na;
        }
        float pa = rf_aa(d_pill, pthk, 0.65f);
        if (pa > 0.001f) {
          float na = pa + out_a * (1.0f - pa);
          if (na > 0.0001f) {
            float inv = 1.0f / na;
            out_r = (1.0f * pa + out_r * out_a * (1.0f - pa)) * inv;
            out_g = (1.0f * pa + out_g * out_a * (1.0f - pa)) * inv;
            out_b = (1.0f * pa + out_b * out_a * (1.0f - pa)) * inv;
          }
          out_a = na;
        }
      }

      if (out_a > 0.001f) {
        uint32_t a8 = (uint32_t)(out_a * 255.0f + 0.5f);
        uint32_t r8 = (uint32_t)(out_r * out_a * 255.0f + 0.5f);
        uint32_t g8 = (uint32_t)(out_g * out_a * 255.0f + 0.5f);
        uint32_t b8 = (uint32_t)(out_b * out_a * 255.0f + 0.5f);
        if (a8 > 255) a8 = 255;
        if (r8 > a8) r8 = a8;
        if (g8 > a8) g8 = a8;
        if (b8 > a8) b8 = a8;
        row[px] = (a8 << 24) | (r8 << 16) | (g8 << 8) | b8;
      } else if (handles && (d <= hit_reach || d_brk <= hit_reach || d_pill <= hit_reach)) {
        // Invisible alpha=1 hit ring along the border so hover/resize grabs effortlessly while interior stays click-through.
        row[px] = 0x01000000u;
      }
    }
  }

  POINT ptDst = {wx, wy};
  SIZE sz = {fw, fh};
  POINT ptSrc = {0, 0};
  BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(hwnd, scr, &ptDst, &sz, mem, &ptSrc, 0, &bf, ULW_ALPHA);

  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(NULL, scr);
}
