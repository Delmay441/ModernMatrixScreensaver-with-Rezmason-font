// log.h — tiny logging to %TEMP%\ModernMatrix.log (+ debugger output).
// A screensaver has no console, so this is how we see what went wrong.
#pragma once

void MMLog(const char* fmt, ...);

// Log a failed HRESULT with context and return it (for early-out chains).
long MMLogHR(const char* what, long hr);

#define MM_CHECK(expr) do {                              \
        long _hr = (long)(expr);                         \
        if (_hr < 0) return (MMLogHR(#expr, _hr), false);\
    } while (0)
