#include "settings_store.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cwchar>

static const wchar_t* kKey = L"Software\\Chewie\\MatrixReflow";

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

// --- shared read/write of the settings blob (used for both the live
//     settings key and each profile subkey) --------------------------------

static void ReadSettingsBlob(HKEY k, MMSettings& s)
{
    s.density        = GetDouble(k, L"density", s.density);
    s.speed          = GetDouble(k, L"speed", s.speed);
    s.bloomIntensity = GetDouble(k, L"bloomIntensity", s.bloomIntensity);
    s.glyphScale     = (float)GetDouble(k, L"glyphScale", s.glyphScale);
    s.lengthBias     = GetDouble(k, L"lengthBias", s.lengthBias);
    s.crtDistort     = GetDouble(k, L"crtDistort", s.crtDistort);
    s.depthAmount    = GetDouble(k, L"depthAmount", s.depthAmount);
    s.cameraSpeed    = GetDouble(k, L"cameraSpeed", s.cameraSpeed);
    s.mutationRate   = GetDouble(k, L"mutationRate", s.mutationRate);

    s.mainColorR     = (float)GetDouble(k, L"mainColorR", s.mainColorR);
    s.mainColorG     = (float)GetDouble(k, L"mainColorG", s.mainColorG);
    s.mainColorB     = (float)GetDouble(k, L"mainColorB", s.mainColorB);
    s.glitchColorR   = (float)GetDouble(k, L"glitchColorR", s.glitchColorR);
    s.glitchColorG   = (float)GetDouble(k, L"glitchColorG", s.glitchColorG);
    s.glitchColorB   = (float)GetDouble(k, L"glitchColorB", s.glitchColorB);

    s.textured       = (int)GetDword(k, L"textured", s.textured);
    s.wireframe      = (int)GetDword(k, L"wireframe", s.wireframe);
    s.showFPS        = (int)GetDword(k, L"showFPS", s.showFPS);
    s.bloom          = (int)GetDword(k, L"bloom", s.bloom);
    s.hdr            = (int)GetDword(k, L"hdr", s.hdr);
    s.easterEggs     = (int)GetDword(k, L"easterEggs", s.easterEggs);
    s.batterySaver   = (int)GetDword(k, L"batterySaver", s.batterySaver);

    s.flipXEnabled   = (int)GetDword(k, L"flipXEnabled", s.flipXEnabled);
    s.flipYEnabled   = (int)GetDword(k, L"flipYEnabled", s.flipYEnabled);
    s.binaryMode     = (int)GetDword(k, L"binaryMode", s.binaryMode);
    s.columnGaps     = (int)GetDword(k, L"columnGaps", s.columnGaps);
    s.extraContrastHeads = (int)GetDword(k, L"extraContrastHeads", s.extraContrastHeads);
    s.crtEmulation = (int)GetDword(k, L"crtEmulation", s.crtEmulation);
}

static void WriteSettingsBlob(HKEY k, const MMSettings& s)
{
    SetDouble(k, L"density", s.density);
    SetDouble(k, L"speed", s.speed);
    SetDouble(k, L"bloomIntensity", s.bloomIntensity);
    SetDouble(k, L"glyphScale", s.glyphScale);
    SetDouble(k, L"lengthBias", s.lengthBias);
    SetDouble(k, L"crtDistort", s.crtDistort);
    SetDouble(k, L"depthAmount", s.depthAmount);
    SetDouble(k, L"cameraSpeed", s.cameraSpeed);
    SetDouble(k, L"mutationRate", s.mutationRate);

    SetDouble(k, L"mainColorR", s.mainColorR);
    SetDouble(k, L"mainColorG", s.mainColorG);
    SetDouble(k, L"mainColorB", s.mainColorB);
    SetDouble(k, L"glitchColorR", s.glitchColorR);
    SetDouble(k, L"glitchColorG", s.glitchColorG);
    SetDouble(k, L"glitchColorB", s.glitchColorB);

    SetDword(k, L"textured", s.textured);
    SetDword(k, L"wireframe", s.wireframe);
    SetDword(k, L"showFPS", s.showFPS);
    SetDword(k, L"bloom", s.bloom);
    SetDword(k, L"hdr", s.hdr);
    SetDword(k, L"easterEggs", s.easterEggs);
    SetDword(k, L"batterySaver", s.batterySaver);

    SetDword(k, L"flipXEnabled", s.flipXEnabled);
    SetDword(k, L"flipYEnabled", s.flipYEnabled);
    SetDword(k, L"binaryMode", s.binaryMode);
    SetDword(k, L"columnGaps", s.columnGaps);
    SetDword(k, L"extraContrastHeads", s.extraContrastHeads);
    SetDword(k, L"crtEmulation", s.crtEmulation);
}

// --- public API: live settings --------------------------------------------

MMSettings LoadSettings()
{
    MMSettings s = mm_settings_default();
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        ReadSettingsBlob(k, s);
        RegCloseKey(k);
    }

    // Clamp the normalized sliders defensively.
    auto clamp01 = [](double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); };
    s.density        = clamp01(s.density);
    s.speed          = clamp01(s.speed);
    s.bloomIntensity = clamp01(s.bloomIntensity);
    s.glyphScale     = (float)clamp01(s.glyphScale);
    s.lengthBias     = clamp01(s.lengthBias);
    s.crtDistort     = clamp01(s.crtDistort);
    s.depthAmount    = clamp01(s.depthAmount);
    s.cameraSpeed    = clamp01(s.cameraSpeed);
    s.mutationRate   = clamp01(s.mutationRate);
    s.mainColorR     = (float)clamp01(s.mainColorR);
    s.mainColorG     = (float)clamp01(s.mainColorG);
    s.mainColorB     = (float)clamp01(s.mainColorB);
    s.glitchColorR   = (float)clamp01(s.glitchColorR);
    s.glitchColorG   = (float)clamp01(s.glitchColorG);
    s.glitchColorB   = (float)clamp01(s.glitchColorB);

    return s;
}

void SaveSettings(const MMSettings& s)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr)
        != ERROR_SUCCESS)
        return;
    WriteSettingsBlob(k, s);
    RegCloseKey(k);
}

// --- public API: profiles ---------------------------------------------------

static bool ProfileKeyPath(int slot, wchar_t* out, size_t cap)
{
    if (slot < 1 || slot > MM_MAX_PROFILES) return false;
    return swprintf(out, cap, L"%s\\Profiles\\%d", kKey, slot) > 0;
}

bool LoadProfile(int slot, MMSettings& out, wchar_t* nameOut, size_t nameCap)
{
    wchar_t path[256];
    if (!ProfileKeyPath(slot, path, 256)) return false;

    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;

    out = mm_settings_default();
    ReadSettingsBlob(k, out);

    if (nameOut && nameCap > 0) {
        DWORD sz = (DWORD)(nameCap * sizeof(wchar_t)), type = 0;
        if (RegQueryValueExW(k, L"Name", nullptr, &type, (BYTE*)nameOut, &sz) != ERROR_SUCCESS || type != REG_SZ)
            nameOut[0] = L'\0';
    }
    RegCloseKey(k);
    return true;
}

void SaveProfile(int slot, const wchar_t* name, const MMSettings& s)
{
    wchar_t path[256];
    if (!ProfileKeyPath(slot, path, 256)) return;

    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr)
        != ERROR_SUCCESS)
        return;

    WriteSettingsBlob(k, s);
    if (name)
        RegSetValueExW(k, L"Name", 0, REG_SZ, (const BYTE*)name, (DWORD)((wcslen(name) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

void DeleteProfile(int slot)
{
    wchar_t path[256];
    if (!ProfileKeyPath(slot, path, 256)) return;
    RegDeleteTreeW(HKEY_CURRENT_USER, path);
}