// config.cpp — the /c settings dialog (native Win32), with a live D3D preview that
// updates as you change controls (parity with the macOS ConfigureView preview).
#include "config.h"
#include "settings_store.h"
#include "resource.h"
#include "log.h"
#include "renderer.h"

#include <commctrl.h>
#include <shellapi.h>

// Themed (v6) common controls so the dialog looks modern.
#pragma comment(linker, "\"/manifestdependency:type='win32' "                 \
                        "name='Microsoft.Windows.Common-Controls' "           \
                        "version='6.0.0.0' processorArchitecture='*' "        \
                        "publicKeyToken='6595b64144ccf1df' language='*'\"")

static MMSettings* s_set   = nullptr;   // settings being edited (modal, single-instance)
static bool        s_saved = false;
static HINSTANCE   g_inst  = nullptr;
static Renderer*   g_preview = nullptr; // live preview renderer
static HWND        g_previewWnd = nullptr;

static const wchar_t* kEncodingNames[] = {
    L"Matrix", L"Binary", L"Hexadecimal", L"Decimal", L"DNA", L"Unicode katakana"
};

// --- slider <-> normalized-double helpers (trackbars are integer 0..1000) -----
static void SetSlider(HWND dlg, int id, double v01)
{
    SendDlgItemMessage(dlg, id, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
    SendDlgItemMessage(dlg, id, TBM_SETPOS, TRUE, (LPARAM)(int)(v01 * 1000.0 + 0.5));
}
static double GetSlider(HWND dlg, int id)
{
    return (double)SendDlgItemMessage(dlg, id, TBM_GETPOS, 0, 0) / 1000.0;
}
static void SetCheck(HWND dlg, int id, int on) { CheckDlgButton(dlg, id, on ? BST_CHECKED : BST_UNCHECKED); }
static int  GetCheck(HWND dlg, int id)         { return IsDlgButtonChecked(dlg, id) == BST_CHECKED ? 1 : 0; }

static void ControlsFromSettings(HWND dlg, const MMSettings& s)
{
    SetSlider(dlg, IDC_DENSITY, s.density);
    SetSlider(dlg, IDC_SPEED, s.speed);
    SetSlider(dlg, IDC_GLOW, s.bloomIntensity);
    int enc = (s.encoding >= 0 && s.encoding < MM_ENCODING_COUNT) ? s.encoding : 0;
    SendDlgItemMessage(dlg, IDC_ENCODING, CB_SETCURSEL, enc, 0);
    SetCheck(dlg, IDC_FOG, s.fog);          SetCheck(dlg, IDC_WAVES, s.waves);
    SetCheck(dlg, IDC_PANNING, s.panning);  SetCheck(dlg, IDC_TEXTURED, s.textured);
    SetCheck(dlg, IDC_WIREFRAME, s.wireframe); SetCheck(dlg, IDC_SHOWFPS, s.showFPS);
    SetCheck(dlg, IDC_BLOOM, s.bloom);      SetCheck(dlg, IDC_HDR, s.hdr);
    EnableWindow(GetDlgItem(dlg, IDC_GLOW), s.bloom ? TRUE : FALSE);
}

static void SettingsFromControls(HWND dlg, MMSettings& s)
{
    s.density = GetSlider(dlg, IDC_DENSITY);
    s.speed   = GetSlider(dlg, IDC_SPEED);
    s.bloomIntensity = GetSlider(dlg, IDC_GLOW);
    LRESULT sel = SendDlgItemMessage(dlg, IDC_ENCODING, CB_GETCURSEL, 0, 0);
    if (sel != CB_ERR) s.encoding = (int)sel;
    s.fog = GetCheck(dlg, IDC_FOG);            s.waves = GetCheck(dlg, IDC_WAVES);
    s.panning = GetCheck(dlg, IDC_PANNING);    s.textured = GetCheck(dlg, IDC_TEXTURED);
    s.wireframe = GetCheck(dlg, IDC_WIREFRAME); s.showFPS = GetCheck(dlg, IDC_SHOWFPS);
    s.bloom = GetCheck(dlg, IDC_BLOOM);        s.hdr = GetCheck(dlg, IDC_HDR);
}

// --- live preview ------------------------------------------------------------
static LRESULT CALLBACK PreviewWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_ERASEBKGND) return 1;   // D3D owns this surface
    return DefWindowProcW(h, m, w, l);
}

static void EnsurePreviewClass()
{
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = PreviewWndProc;
    wc.hInstance = g_inst;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"MMConfigPreview";
    RegisterClassExW(&wc);
    done = true;
}

static void StartPreview(HWND dlg)
{
    EnsurePreviewClass();
    HWND ph = GetDlgItem(dlg, IDC_PREVIEW);
    if (!ph) return;
    RECT r; GetWindowRect(ph, &r);
    MapWindowPoints(nullptr, dlg, (POINT*)&r, 2);
    ShowWindow(ph, SW_HIDE);
    g_previewWnd = CreateWindowExW(0, L"MMConfigPreview", L"", WS_CHILD | WS_VISIBLE,
                                   r.left, r.top, r.right - r.left, r.bottom - r.top,
                                   dlg, nullptr, g_inst, nullptr);
    if (!g_previewWnd) return;
    g_preview = new Renderer();
    if (g_preview->Init(g_previewWnd, *s_set)) {
        SetTimer(dlg, 1, 16, nullptr);          // ~60 fps
    } else {
        delete g_preview; g_preview = nullptr;
        DestroyWindow(g_previewWnd); g_previewWnd = nullptr;
    }
}

static void StopPreview(HWND dlg)
{
    KillTimer(dlg, 1);
    if (g_preview) { g_preview->Shutdown(); delete g_preview; g_preview = nullptr; }
    if (g_previewWnd) { DestroyWindow(g_previewWnd); g_previewWnd = nullptr; }
}

static void ApplyPreview(HWND dlg)
{
    if (!g_preview) return;
    MMSettings w = *s_set;
    SettingsFromControls(dlg, w);
    g_preview->Apply(w);
}

static INT_PTR CALLBACK DlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        for (const wchar_t* n : kEncodingNames)
            SendDlgItemMessage(dlg, IDC_ENCODING, CB_ADDSTRING, 0, (LPARAM)n);
        ControlsFromSettings(dlg, *s_set);
        StartPreview(dlg);
        return TRUE;

    case WM_TIMER:
        if (wp == 1 && g_preview) g_preview->RenderFrame(1.0f / 60.0f);
        return TRUE;

    case WM_HSCROLL:                 // any trackbar moved
        ApplyPreview(dlg);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BLOOM:
            EnableWindow(GetDlgItem(dlg, IDC_GLOW), GetCheck(dlg, IDC_BLOOM) ? TRUE : FALSE);
            ApplyPreview(dlg);
            return TRUE;
        case IDC_FOG: case IDC_WAVES: case IDC_PANNING: case IDC_TEXTURED:
        case IDC_WIREFRAME: case IDC_SHOWFPS: case IDC_HDR:
            ApplyPreview(dlg);
            return TRUE;
        case IDC_ENCODING:
            if (HIWORD(wp) == CBN_SELCHANGE) ApplyPreview(dlg);
            return TRUE;
        case IDC_RESET: {
            MMSettings d = mm_settings_default();
            ControlsFromSettings(dlg, d);
            ApplyPreview(dlg);
            return TRUE;
        }
        case IDOK:
            SettingsFromControls(dlg, *s_set);
            SaveSettings(*s_set);
            s_saved = true;
            StopPreview(dlg);
            EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            StopPreview(dlg);
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_NOTIFY: {
        LPNMHDR hdr = (LPNMHDR)lp;
        if (hdr->idFrom == IDC_LINK && (hdr->code == NM_CLICK || hdr->code == NM_RETURN)) {
            PNMLINK link = (PNMLINK)lp;
            ShellExecuteW(dlg, L"open", link->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        break;
    }

    case WM_DESTROY:
        StopPreview(dlg);
        break;
    }
    return FALSE;
}

bool ShowConfigDialog(HINSTANCE inst, HWND parent, MMSettings& settings)
{
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);

    g_inst = inst;
    s_set = &settings;
    s_saved = false;
    INT_PTR r = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_CONFIG), parent, DlgProc, 0);
    if (r == -1) MMLog("config: DialogBoxParam FAILED, GetLastError=%lu", (unsigned long)GetLastError());
    else         MMLog("config: dialog closed (result=%lld, saved=%d)", (long long)r, s_saved ? 1 : 0);
    return s_saved;
}
