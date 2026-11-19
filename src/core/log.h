#pragma once
#include <cstdarg>

namespace tb {

// One log file per run: %QUIVER_LAB_RUN_DIR%\torchbearer.log in the lab (a fresh folder per
// launch), else torchbearer.log next to the game exe. Thread-safe, flushed per line.
void log_init();
void logf(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void vlogf(const char* fmt, va_list ap);

}  // namespace tb

#define TB_LOG(...) ::tb::logf(__VA_ARGS__)
