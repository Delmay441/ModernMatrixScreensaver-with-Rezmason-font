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
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        // [Existing read logic]
        s.density        = GetDouble(k, L"density", 0.9);
        s.speed          = GetDouble(k, L"speed", 0.1);
        s.bloomIntensity = GetDouble(k, L"bloomIntensity", 0.9);
        s.glyphScale     = (float)GetDouble(k, L"glyphScale", 0.3);             
        s.lengthBias     = GetDouble(k, L"lengthBias", 0.5); 
		s.crtDistort     = GetDouble(k, L"crtDistort", 0.0);
		s.depthAmount 	 = GetDouble(k, L"depthAmount", 0.0);
		s.cameraSpeed    = GetDouble(k, L"cameraSpeed", 1.0);
		s.mutationRate   = GetDouble(k, L"mutationRate", 0.3);

        // [Existing Dword reads]
        s.textured       = (int)GetDword(k, L"textured", s.textured);
        s.wireframe      = (int)GetDword(k, L"wireframe", s.wireframe);
        s.showFPS        = (int)GetDword(k, L"showFPS", s.showFPS);
        s.bloom          = (int)GetDword(k, L"bloom", s.bloom);
        s.hdr            = (int)GetDword(k, L"hdr", s.hdr);
        s.easterEggs     = (int)GetDword(k, L"easterEggs", 1);
        RegCloseKey(k);
    }

    // Clamp the normalized sliders defensively.
    auto clamp01 = [](double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); };
    s.density        = clamp01(s.density);
    s.speed          = clamp01(s.speed);
    s.bloomIntensity = clamp01(s.bloomIntensity);
    s.glyphScale     = (float)clamp01(s.glyphScale);
    s.lengthBias     = clamp01(s.lengthBias);
	s.depthAmount 	 = clamp01(s.depthAmount);
	s.cameraSpeed    = clamp01(s.cameraSpeed);
	s.mutationRate   = clamp01(s.mutationRate);  

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
    SetDouble(k, L"glyphScale", s.glyphScale);
    SetDouble(k, L"lengthBias", s.lengthBias);
	SetDouble(k, L"crtDistort", s.crtDistort);
	SetDouble(k, L"depthAmount", s.depthAmount);
	SetDouble(k, L"cameraSpeed", s.cameraSpeed);
	SetDouble(k, L"mutationRate", s.mutationRate);
    SetDword(k, L"textured", s.textured);
    SetDword(k, L"wireframe", s.wireframe);
    SetDword(k, L"showFPS", s.showFPS);
    SetDword(k, L"bloom", s.bloom);
    SetDword(k, L"hdr", s.hdr);
    SetDword(k, L"easterEggs", s.easterEggs);
    SetDword(k, L"encoding", s.encoding);

    RegCloseKey(k);
}
