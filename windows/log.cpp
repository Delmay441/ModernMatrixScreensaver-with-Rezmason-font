#include "log.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdarg>

static void writeLine(const char* line)
{
    OutputDebugStringA(line);
    char path[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH) return;
    lstrcatA(path, "ModernMatrix.log");
    FILE* f = fopen(path, "a");
    if (f) { fputs(line, f); fclose(f); }
}

void MMLog(const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (k < 0) k = 0;
    if (k > (int)sizeof(buf) - 2) k = sizeof(buf) - 2;
    buf[k] = '\n';
    buf[k + 1] = '\0';
    writeLine(buf);
}

long MMLogHR(const char* what, long hr)
{
    MMLog("ERROR hr=0x%08lX  <- %s", (unsigned long)hr, what);
    return hr;
}
