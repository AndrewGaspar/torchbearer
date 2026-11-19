#include "core/log.h"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>

#include "core/module.h"

namespace tb {
namespace {
std::mutex g_mu;
FILE* g_file = nullptr;
LARGE_INTEGER g_t0, g_freq;
}  // namespace

void log_init() {
  std::lock_guard lk(g_mu);
  if (g_file) return;
  QueryPerformanceFrequency(&g_freq);
  QueryPerformanceCounter(&g_t0);
  std::wstring path;
  wchar_t buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"QUIVER_LAB_RUN_DIR", buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    path = std::wstring(buf, n) + L"\\torchbearer.log";
  } else {
    path = game_dir() + L"\\torchbearer.log";
  }
  g_file = _wfopen(path.c_str(), L"w");
}

void vlogf(const char* fmt, va_list ap) {
  char line[2048];
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  double t = double(now.QuadPart - g_t0.QuadPart) / double(g_freq.QuadPart ? g_freq.QuadPart : 1);
  int off = snprintf(line, sizeof line, "[%9.3f %5lu] ", t, GetCurrentThreadId());
  vsnprintf(line + off, sizeof line - off, fmt, ap);
  std::lock_guard lk(g_mu);
  if (g_file) {
    fputs(line, g_file);
    fputc('\n', g_file);
    fflush(g_file);
  }
  OutputDebugStringA(line);
}

void logf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vlogf(fmt, ap);
  va_end(ap);
}

}  // namespace tb
