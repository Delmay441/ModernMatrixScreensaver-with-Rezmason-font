// config.cpp — the /c settings dialog (native Win32), with a live D3D preview that
// updates as you change controls (parity with the macOS ConfigureView preview).
#include "config.h"
#include "settings_store.h"
#include "resource.h"
#include "log.h"
#include "renderer.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <cmath>
#include <cstdio>

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
static HWND        g_page2 = nullptr;   // IDD_PAGE_OPTIONS  -- checkboxes + colors + profiles + link
static INT_PTR CALLBACK PageSlidersProc(HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK PageOptionsProc(HWND, UINT, WPARAM, LPARAM);
static void ApplyPreview();

// --- color presets -------------------------------------------------------------
// The last entry ("Custom") is a sentinel, never applied directly -- it's what
// the combo shows once the user hand-picks a color that doesn't match a preset.
struct ColorPreset { const wchar_t* name; float mainR, mainG, mainB; float glitchR, glitchG, glitchB; };
static const ColorPreset kPresets[] = {
    { L"Matrix Green",    0.05f, 0.85f, 0.25f,   0.85f, 0.05f, 0.05f },
    { L"Aqua Blue",       0.05f, 0.60f, 0.95f,   0.85f, 0.05f, 0.05f },
    { L"Mojave Amber",    0.95f, 0.55f, 0.05f,   0.85f, 0.05f, 0.05f },
    { L"Pure Crimson",    0.90f, 0.05f, 0.05f,   1.00f, 0.80f, 0.10f },
    { L"Amethyst Purple", 0.55f, 0.05f, 0.90f,   0.85f, 0.05f, 0.05f },
    { L"Custom",          0,0,0,   0,0,0 },
};
static const int kPresetCount = (int)(sizeof(kPresets) / sizeof(kPresets[0]));

static int MatchPresetIndex(const MMSettings& s)
{
    auto close = [](float a, float b) { return fabsf(a - b) < 0.004f; };
    for (int i = 0; i < kPresetCount - 1; ++i) {
        const ColorPreset& p = kPresets[i];
        if (close(s.mainColorR, p.mainR) && close(s.mainColorG, p.mainG) && close(s.mainColorB, p.mainB) &&
            close(s.glitchColorR, p.glitchR) && close(s.glitchColorG, p.glitchG) && close(s.glitchColorB, p.glitchB))
            return i;
    }
    return -1; // Custom
}

static void PopulatePresetCombo(HWND page2)
{
    if (SendDlgItemMessageW(page2, IDC_PRESET_COMBO, CB_GETCOUNT, 0, 0) > 0) return;
    for (int i = 0; i < kPresetCount; ++i)
        SendDlgItemMessageW(page2, IDC_PRESET_COMBO, CB_ADDSTRING, 0, (LPARAM)kPresets[i].name);
}

static void SelectPresetInCombo(HWND page2, int idx)
{
    if (idx < 0) idx = kPresetCount - 1; // "Custom"
    SendDlgItemMessageW(page2, IDC_PRESET_COMBO, CB_SETCURSEL, (WPARAM)idx, 0);
}

// --- profile combo helpers -------------------------------------------------

static void SyncProfileNameField(HWND page2, int slot1based)
{
    wchar_t name[64] = {0};
    MMSettings tmp;
    if (LoadProfile(slot1based, tmp, name, 64) && name[0])
        SetDlgItemTextW(page2, IDC_PROFILE_NAME, name);
    else {
        wchar_t def[32];
        swprintf(def, 32, L"Profile %d", slot1based);
        SetDlgItemTextW(page2, IDC_PROFILE_NAME, def);
    }
}

static void RefreshProfileCombo(HWND page2, int selectIndex /* -1 = keep/first */)
{
    int prevSel = (int)SendDlgItemMessageW(page2, IDC_PROFILE_COMBO, CB_GETCURSEL, 0, 0);
    SendDlgItemMessageW(page2, IDC_PROFILE_COMBO, CB_RESETCONTENT, 0, 0);
    for (int slot = 1; slot <= MM_MAX_PROFILES; ++slot) {
        wchar_t name[64] = {0};
        MMSettings tmp;
        wchar_t label[96];
        if (LoadProfile(slot, tmp, name, 64) && name[0])
            swprintf(label, 96, L"%d: %s", slot, name);
        else
            swprintf(label, 96, L"%d: (empty)", slot);
        SendDlgItemMessageW(page2, IDC_PROFILE_COMBO, CB_ADDSTRING, 0, (LPARAM)label);
    }
    int sel = (selectIndex >= 0) ? selectIndex : (prevSel >= 0 ? prevSel : 0);
    SendDlgItemMessageW(page2, IDC_PROFILE_COMBO, CB_SETCURSEL, (WPARAM)sel, 0);
    SyncProfileNameField(page2, sel + 1);
}

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
// checkboxes (+ easter-egg toggle, battery saver, colors, profiles) are
// children of g_page2.
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
    SetCheck(g_page2, IDC_BATTERYSAVER, s.batterySaver);
    SetCheck(g_page2, IDC_FLIPX, s.flipXEnabled);
    SetCheck(g_page2, IDC_FLIPY, s.flipYEnabled);
    SetCheck(g_page2, IDC_BINARYMODE, s.binaryMode);
    SetCheck(g_page2, IDC_COLUMN_GAPS, s.columnGaps);
    SetCheck(g_page2, IDC_EXTRA_CONTRAST_HEADS, s.extraContrastHeads);
    SetCheck(g_page2, IDC_CRT_EMULATION, s.crtEmulation);
    EnableWindow(GetDlgItem(g_page1, IDC_GLOW), s.bloom ? TRUE : FALSE);

    PopulatePresetCombo(g_page2);
    SelectPresetInCombo(g_page2, MatchPresetIndex(s));
    RefreshProfileCombo(g_page2, -1);
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
    s.batterySaver = GetCheck(g_page2, IDC_BATTERYSAVER);
    s.flipXEnabled = GetCheck(g_page2, IDC_FLIPX);
    s.flipYEnabled = GetCheck(g_page2, IDC_FLIPY);
    s.binaryMode = GetCheck(g_page2, IDC_BINARYMODE);
    s.columnGaps = GetCheck(g_page2, IDC_COLUMN_GAPS);
    s.extraContrastHeads = GetCheck(g_page2, IDC_EXTRA_CONTRAST_HEADS);
    s.crtEmulation = GetCheck(g_page2, IDC_CRT_EMULATION);
    // Note: mainColor*/glitchColor* are NOT read from a widget here -- they're
    // set directly on s_set by the color-picker/preset handlers below, and
    // this function must not clobber them.
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

// Checkboxes, combos, buttons and the GitHub SysLink notify their immediate
// parent (g_page2).
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
        case IDC_BATTERYSAVER:
        case IDC_FLIPX:
        case IDC_FLIPY:
        case IDC_BINARYMODE:
        case IDC_COLUMN_GAPS:
        case IDC_EXTRA_CONTRAST_HEADS:
        case IDC_CRT_EMULATION:
            ApplyPreview();
            return TRUE;

        case IDC_PRESET_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int idx = (int)SendDlgItemMessageW(h, IDC_PRESET_COMBO, CB_GETCURSEL, 0, 0);
                if (idx >= 0 && idx < kPresetCount - 1) {
                    const ColorPreset& p = kPresets[idx];
                    s_set->mainColorR = p.mainR; s_set->mainColorG = p.mainG; s_set->mainColorB = p.mainB;
                    s_set->glitchColorR = p.glitchR; s_set->glitchColorG = p.glitchG; s_set->glitchColorB = p.glitchB;
                    ApplyPreview();
                }
            }
            return TRUE;

        case IDC_COLOR_MAIN:
        case IDC_COLOR_GLITCH: {
            static COLORREF customColors[16] = { 0 };
            float *rr, *gg, *bb;
            if (LOWORD(wp) == IDC_COLOR_MAIN) {
                rr = &s_set->mainColorR; gg = &s_set->mainColorG; bb = &s_set->mainColorB;
            } else {
                rr = &s_set->glitchColorR; gg = &s_set->glitchColorG; bb = &s_set->glitchColorB;
            }
            CHOOSECOLORW cc{};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = h;
            cc.lpCustColors = customColors;
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            cc.rgbResult = RGB((BYTE)(*rr * 255.0f + 0.5f), (BYTE)(*gg * 255.0f + 0.5f), (BYTE)(*bb * 255.0f + 0.5f));
            if (ChooseColorW(&cc)) {
                *rr = GetRValue(cc.rgbResult) / 255.0f;
                *gg = GetGValue(cc.rgbResult) / 255.0f;
                *bb = GetBValue(cc.rgbResult) / 255.0f;
                SelectPresetInCombo(h, MatchPresetIndex(*s_set));
                ApplyPreview();
            }
            return TRUE;
        }

        case IDC_PROFILE_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int slot = (int)SendDlgItemMessageW(h, IDC_PROFILE_COMBO, CB_GETCURSEL, 0, 0) + 1;
                SyncProfileNameField(h, slot);
            }
            return TRUE;

        case IDC_PROFILE_SAVE: {
            int slot = (int)SendDlgItemMessageW(h, IDC_PROFILE_COMBO, CB_GETCURSEL, 0, 0) + 1;
            wchar_t name[64] = {0};
            GetDlgItemTextW(h, IDC_PROFILE_NAME, name, 64);
            if (name[0] == L'\0') swprintf(name, 64, L"Profile %d", slot);
            MMSettings w = *s_set;
            SettingsFromControls(w); // colors already live on s_set / carried by w = *s_set
            SaveProfile(slot, name, w);
            RefreshProfileCombo(h, slot - 1);
            return TRUE;
        }
        case IDC_PROFILE_LOAD: {
            int slot = (int)SendDlgItemMessageW(h, IDC_PROFILE_COMBO, CB_GETCURSEL, 0, 0) + 1;
            MMSettings loaded;
            wchar_t name[64] = {0};
            if (LoadProfile(slot, loaded, name, 64)) {
                *s_set = loaded;
                ControlsFromSettings(*s_set);
                if (name[0]) SetDlgItemTextW(h, IDC_PROFILE_NAME, name);
                ApplyPreview();
            } else {
                MessageBoxW(h, L"That profile slot is empty.", L"Load Profile", MB_OK | MB_ICONINFORMATION);
            }
            return TRUE;
        }
        case IDC_PROFILE_DELETE: {
            int slot = (int)SendDlgItemMessageW(h, IDC_PROFILE_COMBO, CB_GETCURSEL, 0, 0) + 1;
            DeleteProfile(slot);
            RefreshProfileCombo(h, slot - 1);
            return TRUE;
        }
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
        s_set->mainColorR = d.mainColorR; s_set->mainColorG = d.mainColorG; s_set->mainColorB = d.mainColorB;
        s_set->glitchColorR = d.glitchColorR; s_set->glitchColorG = d.glitchColorG; s_set->glitchColorB = d.glitchColorB;
        s_set->batterySaver = d.batterySaver;
        SelectPresetInCombo(g_page2, MatchPresetIndex(d));
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