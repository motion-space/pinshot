#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "moonbit.h"

extern void mb_post(const char *channel, const char *json, size_t len, const uint8_t *bytes, size_t blen);

#define HTTP_MOST (4 * 1024 * 1024)

typedef struct {
  int id;
  int timeout;
  wchar_t *verb;
  wchar_t *url;
  wchar_t *headers;
  char *body;
  DWORD body_n;
} Ask;

static wchar_t *wide_of(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (n > 0 ? n : 1));
  if (!w) return NULL;
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  else w[0] = 0;
  return w;
}

static void ask_free(Ask *a) {
  free(a->verb);
  free(a->url);
  free(a->headers);
  free(a->body);
  free(a);
}

static const char *why_of(DWORD e) {
  switch (e) {
    case ERROR_WINHTTP_TIMEOUT: return "timeout";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "dns";
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR: return "connect";
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID: return "tls";
    default: return "network";
  }
}

// One session for every request, made at the first (WinHTTP's own proxy and resolver threads come with it, once).
static HINTERNET session;
static INIT_ONCE session_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK session_open(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
  (void)once, (void)arg, (void)ctx;
  session = WinHttpOpen(L"PinShot", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  return TRUE;
}

static DWORD WINAPI ask_work(void *arg) {
  Ask *a = (Ask *)arg;
  HINTERNET s = NULL, c = NULL, r = NULL;
  ULONGLONG deadline = GetTickCount64() + (ULONGLONG)a->timeout * 2;
  uint8_t *body = NULL;
  size_t body_n = 0, body_cap = 0;
  const char *why = "";
  DWORD status = 0, sz = sizeof status;
  uint8_t buf[16 * 1024];
  wchar_t host[256] = {0}, path[4096] = {0};
  URL_COMPONENTS u;
  memset(&u, 0, sizeof u);
  u.dwStructSize = sizeof u;
  u.lpszHostName = host;
  u.dwHostNameLength = 255;
  u.lpszUrlPath = path;
  u.dwUrlPathLength = 2047;
  u.lpszExtraInfo = path + 2048;
  u.dwExtraInfoLength = 2047;
  if (!WinHttpCrackUrl(a->url, 0, 0, &u) ||
      (u.nScheme != INTERNET_SCHEME_HTTPS && !(u.nScheme == INTERNET_SCHEME_HTTP &&
                                               (!_wcsicmp(host, L"127.0.0.1") || !_wcsicmp(host, L"localhost"))))) {
    why = "url";
    goto done;
  }
  if (u.dwExtraInfoLength) {
    size_t pl = wcslen(path);
    memmove(path + pl, path + 2048, (u.dwExtraInfoLength + 1) * sizeof(wchar_t));
  }
  InitOnceExecuteOnce(&session_once, session_open, NULL, NULL);
  s = session;
  if (!s) {
    why = "network";
    goto done;
  }
  c = WinHttpConnect(s, host, u.nPort, 0);
  r = c ? WinHttpOpenRequest(c, a->verb, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             WINHTTP_FLAG_REFRESH | (u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0))
        : NULL;
  if (!r) {
    why = why_of(GetLastError());
    goto done;
  }
  WinHttpSetTimeouts(r, a->timeout, a->timeout, a->timeout, a->timeout);
  {
    // (a key in the headers never goes on to where a redirect points)
    DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof never);
  }
  if (!WinHttpSendRequest(r, a->headers[0] ? a->headers : WINHTTP_NO_ADDITIONAL_HEADERS, a->headers[0] ? (DWORD)-1L : 0,
                          a->body_n ? a->body : WINHTTP_NO_REQUEST_DATA, a->body_n, a->body_n, 0) ||
      !WinHttpReceiveResponse(r, NULL)) {
    why = why_of(GetLastError());
    goto done;
  }
  WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                      WINHTTP_NO_HEADER_INDEX);
  for (;;) {
    DWORD n = 0;
    if (!WinHttpReadData(r, buf, sizeof buf, &n)) {
      why = why_of(GetLastError());
      status = 0;
      goto done;
    }
    if (n == 0) break;
    if (GetTickCount64() > deadline) {
      why = "timeout";
      status = 0;
      goto done;
    }
    if (body_n + n > HTTP_MOST) {
      why = "large";
      status = 0;
      goto done;
    }
    if (body_n + n > body_cap) {
      size_t cap = body_cap ? body_cap * 2 : 32 * 1024;
      while (cap < body_n + n) cap *= 2;
      uint8_t *grown = (uint8_t *)realloc(body, cap);
      if (!grown) {
        why = "memory";
        status = 0;
        goto done;
      }
      body = grown, body_cap = cap;
    }
    memcpy(body + body_n, buf, n);
    body_n += n;
  }

done:;
  char json[160];
  int jn = snprintf(json, sizeof json, "{\"do\":\"http\",\"id\":%d,\"status\":%lu,\"err\":\"%s\"}", a->id,
                    (unsigned long)status, why);
  if (jn < 0) jn = 0;
  mb_post("ps", json, (size_t)jn, body, status ? body_n : 0);
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  free(body);
  ask_free(a);
  return 0;
}

int32_t ps_http(int32_t id, moonbit_bytes_t verb, moonbit_bytes_t url, moonbit_bytes_t headers, moonbit_bytes_t body,
                int32_t timeout_ms) {
  Ask *a = (Ask *)calloc(1, sizeof(Ask));
  if (!a) return 0;
  a->id = id;
  a->timeout = timeout_ms > 0 ? timeout_ms : 15000;
  a->verb = wide_of((const char *)verb);
  a->url = wide_of((const char *)url);
  a->headers = wide_of((const char *)headers);
  a->body_n = (DWORD)Moonbit_array_length(body);
  a->body = (char *)malloc(a->body_n + 1);
  if (!a->verb || !a->url || !a->headers || !a->body) {
    ask_free(a);
    return 0;
  }
  memcpy(a->body, body, a->body_n);
  HANDLE t = CreateThread(NULL, 0, ask_work, a, 0, NULL);
  if (!t) {
    ask_free(a);
    return 0;
  }
  CloseHandle(t);
  return 1;
}
