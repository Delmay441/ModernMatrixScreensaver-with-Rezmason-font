// host.cpp — the .scr host shell.
//
// A Windows screensaver is a normal Win32 PE (renamed .scr) that parses argv:
//   /s            run full-screen (the actual screensaver) — one window per monitor
//   /p <HWND>     preview inside the given parent window (the little box)
//   /c[:<HWND>]   configure (modal settings dialog)
//   (none)        configure
// Plus dev-only modes:
//   /w            run in a normal resizable window (development/screenshots)
//   /shot <png> [frames] [w] [h]   headless render to a PNG (verification)
//
// Full-screen mode covers every monitor, hides the cursor, and exits on any real
// input. Preview/windowed modes never exit on input.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <vector>
#include "renderer.h"
#include "config.h"
#include "settings_store.h"
#include "log.h"

enum class Mode { Config, Run, Preview, Windowed, Shot };

struct App {
    Mode      mode = Mode::Config;
    std::vector<HWND>      windows;
    std::vector<Renderer*> renderers;   // one per window (per monitor in Run mode)
    bool      gotFirstMouse = false;
    POINT     firstMouse{};
    bool      quitting = false;
};
static App g_app;

// Headless screenshot params (Mode::Shot).
static std::wstring g_shotPath = L"shot.png";
static UINT g_shotW = 1280, g_shotH = 720;
static int  g_shotFrames = 150;

static void RequestQuit() { if (!g_app.quitting) { g_app.quitting = true; PostQuitMessage(0); } }

static void MaybeQuitOnInput(int dx, int dy)
{
    if (g_app.mode != Mode::Run) return;          // only the real saver bails on input
    if (abs(dx) > 4 || abs(dy) > 4) RequestQuit();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            Renderer* r = (Renderer*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (r) r->Resize(LOWORD(lp), HIWORD(lp));
        }
        return 0;

    case WM_SETCURSOR:
        if (g_app.mode == Mode::Run) { SetCursor(nullptr); return TRUE; }
        break;

    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(hwnd, &p);                 // screen coords: consistent across monitors
        if (!g_app.gotFirstMouse) { g_app.gotFirstMouse = true; g_app.firstMouse = p; }
        else MaybeQuitOnInput(p.x - g_app.firstMouse.x, p.y - g_app.firstMouse.y);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_app.mode == Mode::Run) { RequestQuit(); return 0; }
        if (wp == VK_ESCAPE) { RequestQuit(); return 0; }     // windowed dev: ESC quits
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (g_app.mode == Mode::Run) { RequestQuit(); return 0; }
        break;

    case WM_CLOSE:
    case WM_DESTROY:
        RequestQuit();
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void EnableDpiAwareness()
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI *PFN)(DPI_AWARENESS_CONTEXT);
    if (u32) {
        auto p = (PFN)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (p && p(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }
    SetProcessDPIAware();
}

static const wchar_t* kClass = L"ModernMatrixWindow";

static void EnsureClass(HINSTANCE inst)
{
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    done = true;
}

static bool AttachRenderer(HWND hwnd, const MMSettings& s)
{
    Renderer* r = new Renderer();
    if (!r->Init(hwnd, s)) { delete r; return false; }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)r);
    g_app.windows.push_back(hwnd);
    g_app.renderers.push_back(r);
    return true;
}

static BOOL CALLBACK MonitorEnum(HMONITOR mon, HDC, LPRECT, LPARAM lp)
{
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfo(mon, &mi))
        ((std::vector<RECT>*)lp)->push_back(mi.rcMonitor);
    return TRUE;
}

// One borderless topmost window + renderer per physical monitor.
static bool CreateRunWindows(HINSTANCE inst, const MMSettings& s)
{
    std::vector<RECT> mons;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnum, (LPARAM)&mons);
    if (mons.empty()) {
        RECT r{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        mons.push_back(r);
    }
    for (const RECT& r : mons) {
        HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, kClass, L"Modern Matrix", WS_POPUP,
                                    r.left, r.top, r.right - r.left, r.bottom - r.top,
                                    nullptr, nullptr, inst, nullptr);
        if (!hwnd) continue;
        if (!AttachRenderer(hwnd, s)) { DestroyWindow(hwnd); continue; }
        ShowWindow(hwnd, SW_SHOW);
    }
    if (g_app.renderers.empty()) return false;
    ShowCursor(FALSE);
    SetForegroundWindow(g_app.windows.front());
    return true;
}

static bool CreateSingleWindow(HINSTANCE inst, Mode mode, HWND parent, const MMSettings& s)
{
    HWND hwnd = nullptr;
    if (mode == Mode::Preview && parent) {
        RECT rc{}; GetClientRect(parent, &rc);
        hwnd = CreateWindowExW(0, kClass, L"Modern Matrix", WS_CHILD | WS_VISIBLE,
                               0, 0, rc.right - rc.left, rc.bottom - rc.top,
                               parent, nullptr, inst, nullptr);
    } else {
        hwnd = CreateWindowExW(0, kClass, L"Modern Matrix (dev)",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
                               nullptr, nullptr, inst, nullptr);
    }
    if (!hwnd) return false;
    return AttachRenderer(hwnd, s);
}

static int RunLoop()
{
    LARGE_INTEGER freq, prev;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    for (;;) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return (int)msg.wParam;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        float dt = (float)(now.QuadPart - prev.QuadPart) / (float)freq.QuadPart;
        prev = now;
        if (dt > 0.1f) dt = 0.1f;                  // clamp per PORTING.md §8
        for (Renderer* r : g_app.renderers)
            if (!r->RenderFrame(dt)) RequestQuit();  // device lost -> exit, Windows relaunches
    }
}

static void Cleanup()
{
    for (Renderer* r : g_app.renderers) { r->Shutdown(); delete r; }
    g_app.renderers.clear();
    for (HWND h : g_app.windows) if (IsWindow(h)) DestroyWindow(h);
    g_app.windows.clear();
}

static void ParseArgs(Mode& mode, HWND& parent)
{
    mode = Mode::Config; parent = nullptr;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a.size() < 2 || (a[0] != L'/' && a[0] != L'-')) continue;

        std::wstring la = a;
        for (auto& ch : la) ch = towlower(ch);
        if (la == L"/shot" || la == L"-shot") {
            mode = Mode::Shot;
            if (i + 1 < argc) g_shotPath = argv[++i];
            if (i + 1 < argc) { int n = _wtoi(argv[i + 1]); if (n > 0) { g_shotFrames = n; ++i; } }
            if (i + 1 < argc) { int w = _wtoi(argv[i + 1]); if (w > 0) { g_shotW = (UINT)w; ++i; } }
            if (i + 1 < argc) { int h = _wtoi(argv[i + 1]); if (h > 0) { g_shotH = (UINT)h; ++i; } }
            continue;
        }

        wchar_t c = towlower(a[1]);
        std::wstring rest = a.substr(2);
        if (c == L's') { mode = Mode::Run; }
        else if (c == L'w') { mode = Mode::Windowed; }
        else if (c == L'c') {
            mode = Mode::Config;
            if (!rest.empty() && rest[0] == L':')
                parent = (HWND)(uintptr_t)_wcstoui64(rest.c_str() + 1, nullptr, 10);
        }
        else if (c == L'p') {
            mode = Mode::Preview;
            std::wstring h;
            if (!rest.empty()) h = (rest[0] == L':') ? rest.substr(1) : rest;
            if (h.empty() && i + 1 < argc) h = argv[++i];
            parent = (HWND)(uintptr_t)_wcstoui64(h.c_str(), nullptr, 10);
        }
    }
    LocalFree(argv);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // WIC/Direct2D atlas + overlay need COM
    Mode mode; HWND parent;
    ParseArgs(mode, parent);
    g_app.mode = mode;

    MMSettings settings = LoadSettings();

    if (mode == Mode::Shot) {
        Renderer* r = new Renderer();
        bool ok = r->InitHeadless(g_shotW, g_shotH, settings);
        if (ok) {
            for (int i = 0; i < g_shotFrames; ++i) r->RenderFrame(1.0f / 60.0f);
            ok = r->SaveScreenshot(g_shotPath.c_str());
        }
        r->Shutdown(); delete r;
        MMLog("screenshot %s", ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }

    if (mode == Mode::Config)
        return ShowConfigDialog(inst, parent, settings) ? 0 : 0;

    EnsureClass(inst);
    bool ok = (mode == Mode::Run) ? CreateRunWindows(inst, settings)
                                  : CreateSingleWindow(inst, mode, parent, settings);
    if (!ok) { MMLog("window/renderer init failed (mode=%d)", (int)mode); Cleanup(); return 1; }

    int rc = RunLoop();
    Cleanup();
    if (g_app.mode == Mode::Run) ShowCursor(TRUE);
    return rc;
}
