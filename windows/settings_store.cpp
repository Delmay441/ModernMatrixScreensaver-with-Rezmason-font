#include "settings_store.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static const wchar_t* kKey = L"Software\\Chewie\\ModernMatrix";

// --- low-level helpers --------------------------------------------------------

static DWORD GetDword(HKEY k, const wchar_t* name, DWORD def)
{
    DWORD v = def, sz = sizeof(v), type = 0;
    if (RegQueryValueExW(k, name, nullptr, &type, (BYTE*)&v, &sz) != ERROR_SUCCESS || type != REG_DWORD)
        return def;
    return v;
}
static double GetDouble(HKEY k, const wchar_t* name, double def)
{
    double v = def; DWORD sz = sizeof(v), type = 0;
    if (RegQueryValueExW(k, name, nullptr, &type, (BYTE*)&v, &sz) != ERROR_SUCCESS ||
        type != REG_BINARY || sz != sizeof(double))
        return def;
    return v;
}
static void SetDword(HKEY k, const wchar_t* name, DWORD v)
{
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
}
static void SetDouble(HKEY k, const wchar_t* name, double v)
{
    RegSetValueExW(k, name, 0, REG_BINARY, (const BYTE*)&v, sizeof(v));
}

// --- public API ---------------------------------------------------------------

MMSettings LoadSettings()
{
    MMSettings s = mm_settings_default();
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return s;   // nothing saved yet -> defaults

    s.density        = GetDouble(k, L"density", s.density);
    s.speed          = GetDouble(k, L"speed", s.speed);
    s.bloomIntensity = GetDouble(k, L"bloomIntensity", s.bloomIntensity);
    s.encoding       = (int)GetDword(k, L"encoding", (DWORD)s.encoding);
    s.fog       = (int)GetDword(k, L"fog", s.fog);
    s.waves     = (int)GetDword(k, L"waves", s.waves);
    s.panning   = (int)GetDword(k, L"panning", s.panning);
    s.textured  = (int)GetDword(k, L"textured", s.textured);
    s.wireframe = (int)GetDword(k, L"wireframe", s.wireframe);
    s.showFPS   = (int)GetDword(k, L"showFPS", s.showFPS);
    s.bloom     = (int)GetDword(k, L"bloom", s.bloom);
    s.hdr       = (int)GetDword(k, L"hdr", s.hdr);
    RegCloseKey(k);

    // Clamp the normalized sliders defensively.
    auto clamp01 = [](double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); };
    s.density = clamp01(s.density);
    s.speed = clamp01(s.speed);
    s.bloomIntensity = clamp01(s.bloomIntensity);
    if (s.encoding < 0 || s.encoding >= MM_ENCODING_COUNT) s.encoding = MM_ENCODING_MATRIX;
    return s;
}

void SaveSettings(const MMSettings& s)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr)
        != ERROR_SUCCESS)
        return;
    SetDouble(k, L"density", s.density);
    SetDouble(k, L"speed", s.speed);
    SetDouble(k, L"bloomIntensity", s.bloomIntensity);
    SetDword(k, L"encoding", (DWORD)s.encoding);
    SetDword(k, L"fog", (DWORD)s.fog);
    SetDword(k, L"waves", (DWORD)s.waves);
    SetDword(k, L"panning", (DWORD)s.panning);
    SetDword(k, L"textured", (DWORD)s.textured);
    SetDword(k, L"wireframe", (DWORD)s.wireframe);
    SetDword(k, L"showFPS", (DWORD)s.showFPS);
    SetDword(k, L"bloom", (DWORD)s.bloom);
    SetDword(k, L"hdr", (DWORD)s.hdr);
    RegCloseKey(k);
}
