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
static Renderer* g_preview = nullptr; // live preview renderer
static HWND        g_previewWnd = nullptr;

// The two tab pages are separate child dialogs (not just hidden control groups)
// so that control IDs don't need to be juggled across pages: sliders live on
// page 1, everything else on page 2. IDC_TAB itself lives on the host dialog.
static HWND        g_page1 = nullptr;   // IDD_PAGE_SLIDERS  -- all sliders
static HWND        g_page2 = nullptr;   // IDD_PAGE_OPTIONS  -- checkboxes + link
static INT_PTR CALLBACK PageSlidersProc(HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK PageOptionsProc(HWND, UINT, WPARAM, LPARAM);
static void ApplyPreview();

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

// Reads/writes span both page dialogs: sliders are children of g_page1,
// checkboxes (+ easter-egg toggle) are children of g_page2.
static void ControlsFromSettings(const MMSettings& s)
{
    SetSlider(g_page1, IDC_DENSITY, s.density);
    SetSlider(g_page1, IDC_SPEED, s.speed);
    SetSlider(g_page1, IDC_GLOW, s.bloomIntensity);
	SetSlider(g_page1, IDC_GLYPH_SCALE, s.glyphScale);
    
    // Evaluate initial warning visibility based on saved settings
    HWND hWarning = GetDlgItem(g_page1, IDC_WARNING_GLYPH);
    if (hWarning) ShowWindow(hWarning, s.glyphScale <= 0.1f ? SW_SHOWNA : SW_HIDE);

	SetSlider(g_page1, IDC_LENGTH_BIAS, s.lengthBias);
	SetSlider(g_page1, IDC_DISTORTION, s.crtDistort);
	SetSlider(g_page1, IDC_DEPTH, s.depthAmount / 1.5);
	SetSlider(g_page1, IDC_CAMSPEED, s.cameraSpeed);
	SetSlider(g_page1, IDC_MUTATION, s.mutationRate);
    SetCheck(g_page2, IDC_TEXTURED, s.textured);
    SetCheck(g_page2, IDC_WIREFRAME, s.wireframe); SetCheck(g_page2, IDC_SHOWFPS, s.showFPS);
    SetCheck(g_page2, IDC_BLOOM, s.bloom);      SetCheck(g_page2, IDC_HDR, s.hdr);
    SetCheck(g_page2, IDC_EASTEREGGS, s.easterEggs);
    EnableWindow(GetDlgItem(g_page1, IDC_GLOW), s.bloom ? TRUE : FALSE);
}

static void SettingsFromControls(MMSettings& s)
{
    s.density = GetSlider(g_page1, IDC_DENSITY);
    s.speed   = GetSlider(g_page1, IDC_SPEED);
    s.bloomIntensity = GetSlider(g_page1, IDC_GLOW);
	s.glyphScale = (float)GetSlider(g_page1, IDC_GLYPH_SCALE);
	s.lengthBias = GetSlider(g_page1, IDC_LENGTH_BIAS);
	s.crtDistort = GetSlider(g_page1, IDC_DISTORTION);
	s.depthAmount = GetSlider(g_page1, IDC_DEPTH) * 1.5;
	s.cameraSpeed = GetSlider(g_page1, IDC_CAMSPEED);
	s.mutationRate = GetSlider(g_page1, IDC_MUTATION);
	s.textured = GetCheck(g_page2, IDC_TEXTURED);
    s.wireframe = GetCheck(g_page2, IDC_WIREFRAME); s.showFPS = GetCheck(g_page2, IDC_SHOWFPS);
    s.bloom = GetCheck(g_page2, IDC_BLOOM);        s.hdr = GetCheck(g_page2, IDC_HDR);
    s.easterEggs = GetCheck(g_page2, IDC_EASTEREGGS);
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

static void ApplyPreview()
{
    if (!g_preview) return;
    MMSettings w = *s_set;
    SettingsFromControls(w);
    g_preview->Apply(w);
}

// --- tab pages ---------------------------------------------------------------
// Both pages are created once and simply shown/hidden on tab selection; their
// window rect is set to fill the tab control's display area (inset for the
// tab's own header strip), computed here rather than baked into the .rc so it
// stays correct regardless of exact tab-header metrics on a given system.
static void PositionPages(HWND dlg)
{
    HWND tab = GetDlgItem(dlg, IDC_TAB);
    if (!tab) return;
    RECT r; GetWindowRect(tab, &r);
    MapWindowPoints(nullptr, dlg, (POINT*)&r, 2);
    r.left += 4; r.right -= 4; r.top += 22; r.bottom -= 6;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (g_page1) MoveWindow(g_page1, r.left, r.top, w, h, TRUE);
    if (g_page2) MoveWindow(g_page2, r.left, r.top, w, h, TRUE);
}

static void ShowPage(int index)
{
    ShowWindow(g_page1, index == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_page2, index == 1 ? SW_SHOW : SW_HIDE);
}

// Trackbars on the sliders page notify their immediate parent (g_page1), so
// this page needs its own WM_HSCROLL handler to keep the live preview in sync.
static INT_PTR CALLBACK PageSlidersProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_HSCROLL: {
        HWND hSlider = (HWND)lp;
        
        // Evaluate dynamic warning visibility during drag events
        if (GetDlgCtrlID(hSlider) == IDC_GLYPH_SCALE) {
            int pos = (int)SendMessage(hSlider, TBM_GETPOS, 0, 0);
            HWND hWarning = GetDlgItem(hwnd, IDC_WARNING_GLYPH);
            
            // Slider range is 0-1000, 200 equates to a 0.2 scale
            if (pos <= 100) { 
                ShowWindow(hWarning, SW_SHOWNA);
            } else {
                ShowWindow(hWarning, SW_HIDE);
            }
        }
        
        ApplyPreview();
        return TRUE;
    }
    
    // Native paint override to force the static text to render in red
    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wp;
        HWND hwndStatic = (HWND)lp;
        
        if (GetDlgCtrlID(hwndStatic) == IDC_WARNING_GLYPH) {
            SetTextColor(hdcStatic, RGB(220, 20, 20)); 
            SetBkMode(hdcStatic, TRANSPARENT);
            return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }
    }
    
    return FALSE;
}

// Checkboxes and the GitHub SysLink notify their immediate parent (g_page2).
static INT_PTR CALLBACK PageOptionsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BLOOM:
            EnableWindow(GetDlgItem(g_page1, IDC_GLOW), GetCheck(h, IDC_BLOOM) ? TRUE : FALSE);
            ApplyPreview();
            return TRUE;
        case IDC_TEXTURED:
        case IDC_WIREFRAME:
        case IDC_SHOWFPS:
        case IDC_HDR:
        case IDC_EASTEREGGS:
            ApplyPreview();
            return TRUE;
        }
        break;

    case WM_NOTIFY: {
        LPNMHDR hdr = (LPNMHDR)lp;
        if (hdr->idFrom == IDC_LINK && (hdr->code == NM_CLICK || hdr->code == NM_RETURN)) {
            PNMLINK link = (PNMLINK)lp;
            ShellExecuteW(h, L"open", link->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

static INT_PTR CALLBACK DlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        HWND tab = GetDlgItem(dlg, IDC_TAB);
        TCITEMW ti{}; ti.mask = TCIF_TEXT;
        ti.pszText = (LPWSTR)L"Effects"; TabCtrl_InsertItem(tab, 0, &ti);
        ti.pszText = (LPWSTR)L"Options"; TabCtrl_InsertItem(tab, 1, &ti);

        g_page1 = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_PAGE_SLIDERS), dlg, PageSlidersProc, 0);
        g_page2 = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_PAGE_OPTIONS), dlg, PageOptionsProc, 0);
        PositionPages(dlg);
        ControlsFromSettings(*s_set);
        ShowPage(0);

        StartPreview(dlg);
        return TRUE;
    }

    case WM_TIMER:
        if (wp == 1 && g_preview) {
            g_preview->RenderFrame(1.0f / 60.0f);            
        }
        return TRUE;

    case WM_COMMAND:
    switch (LOWORD(wp)) {
    case IDC_RESET: {
        MMSettings d = mm_settings_default();
        ControlsFromSettings(d);
        ApplyPreview();
        return TRUE;
    }
    case IDOK:
        SettingsFromControls(*s_set);
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
        if (hdr->idFrom == IDC_TAB && hdr->code == TCN_SELCHANGE) {
            ShowPage(TabCtrl_GetCurSel(GetDlgItem(dlg, IDC_TAB)));
            return TRUE;
        }
        break;
    }

    case WM_DESTROY:
        StopPreview(dlg);
        g_page1 = nullptr;
        g_page2 = nullptr;
        break;
    }
    return FALSE;
}

bool ShowConfigDialog(HINSTANCE inst, HWND parent, MMSettings& settings)
{
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_LINK_CLASS | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);

    g_inst = inst;
    s_set = &settings;
    s_saved = false;
    INT_PTR r = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_CONFIG), parent, DlgProc, 0);
    if (r == -1) MMLog("config: DialogBoxParam FAILED, GetLastError=%lu", (unsigned long)GetLastError());
    else         MMLog("config: dialog closed (result=%lld, saved=%d)", (long long)r, s_saved ? 1 : 0);
    return s_saved;
}