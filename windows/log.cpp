#include "log.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>     // SHGetKnownFolderPath
#include <knownfolders.h>
#include <objbase.h>    // CoTaskMemFree
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <ctime>

// Resolves the full path to write ModernMatrix.log to, once per process.
//
// Previously this wrote next to the running exe (falling back to %TEMP%),
// which meant a screensaver installed under Program Files (no write access
// without elevation) or invoked from a locked-down System32 copy would
// either fail outright or scatter its log into %TEMP%, where a screensaver
// launched by Windows' idle-timeout can land in a different session/user
// context than an interactive shell checking for it afterward. It also left
// stray ModernMatrix.log files sitting in whatever folder the .scr happened
// to be run from -- not somewhere a user expects an app to leave a file.
//
// This now writes to %LOCALAPPDATA%\MatrixReflow\MatrixReflow.log, which is
// always writable by the current user regardless of where the .scr itself
// lives or how it was launched, and is the conventional place for an app's
// own logs/state on Windows. Falls back to %TEMP% only if LocalAppData is
// somehow unavailable (very rare -- a broken user profile).
//
// The log now also survives across runs (appended, not clobbered) with a
// session-start marker per run, and rotates to a single ".old" backup once
// it grows past ~1MB so a screensaver left running for months doesn't grow
// an unbounded file.
static const wchar_t kLogDirName[]  = L"MatrixReflow";
static const wchar_t kLogFileName[] = L"MatrixReflow.log";
static const long long kMaxLogBytes = 1 * 1024 * 1024; // rotate past ~1MB

static bool ResolveLogPath(wchar_t* out, size_t cap)
{
    PWSTR localAppData = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData);
    if (SUCCEEDED(hr) && localAppData) {
        wchar_t dir[MAX_PATH];
        bool dirBuilt =
            wcscpy_s(dir, MAX_PATH, localAppData) == 0 &&
            wcscat_s(dir, MAX_PATH, L"\\") == 0 &&
            wcscat_s(dir, MAX_PATH, kLogDirName) == 0;
        CoTaskMemFree(localAppData);

        if (dirBuilt && (CreateDirectoryW(dir, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)) {
            bool pathBuilt =
                wcscpy_s(out, cap, dir) == 0 &&
                wcscat_s(out, cap, L"\\") == 0 &&
                wcscat_s(out, cap, kLogFileName) == 0;

            if (pathBuilt) {
                // Confirm the file is actually writable right now (covers
                // the rare case of a permissions-restricted profile) --
                // if this fails, fall through to %TEMP% below.
                FILE* probe = nullptr;
                if (_wfopen_s(&probe, out, L"a") == 0 && probe) {
                    fclose(probe);
                    return true;
                }
            }
        }
    }

    wchar_t tempPath[MAX_PATH];
    DWORD tn = GetTempPathW(MAX_PATH, tempPath);
    if (tn == 0 || tn >= MAX_PATH || tn + (sizeof(kLogFileName) / sizeof(wchar_t)) > cap)
        return false;
    wmemcpy(out, tempPath, tn);
    wcscpy_s(out + tn, cap - tn, kLogFileName);
    return true;
}

// If the log has grown past kMaxLogBytes, move it aside to a ".old" file
// (replacing any previous ".old") so writeLine() below starts a fresh file.
// Best-effort: if rotation fails for any reason, we just keep appending to
// the existing file rather than losing logging entirely.
static void RotateIfNeeded(const wchar_t* path)
{
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attr))
        return; // doesn't exist yet -- nothing to rotate

    long long size = ((long long)attr.nFileSizeHigh << 32) | attr.nFileSizeLow;
    if (size < kMaxLogBytes)
        return;

    wchar_t backupPath[MAX_PATH];
    if (wcscpy_s(backupPath, MAX_PATH, path) != 0) return;
    if (wcscat_s(backupPath, MAX_PATH, L".old") != 0) return;

    MoveFileExW(path, backupPath, MOVEFILE_REPLACE_EXISTING);
}

static void writeLine(const char* line)
{
    OutputDebugStringA(line);

    static wchar_t path[MAX_PATH];
    static bool resolved = false;
    static bool valid = false;
    if (!resolved) {
        valid = ResolveLogPath(path, MAX_PATH);
        if (valid) {
            RotateIfNeeded(path);

            // Session-start marker -- since the log now persists across runs
            // instead of being cleared each time, this is what lets you tell
            // where one run ends and the next begins when reading it back.
            time_t now = time(nullptr);
            struct tm t{};
            localtime_s(&t, &now);
            char stamp[64];
            strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &t);

            FILE* f = nullptr;
            if (_wfopen_s(&f, path, L"a") == 0 && f) {
                fprintf(f, "\n=== session start %s ===\n", stamp);
                fclose(f);
            }
        }
        resolved = true;
    }
    if (!valid) return;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a") == 0 && f) {
        fputs(line, f);
        fclose(f);
    }
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

