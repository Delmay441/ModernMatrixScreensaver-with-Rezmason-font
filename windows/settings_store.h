// settings_store.h — persist MMSettings to HKCU\Software\Chewie\MatrixReflow.
// No sandbox on Windows, so the saver and the config dialog share one registry key.
#pragma once
#include "../core/mmcore.h"

#define MM_MAX_PROFILES 5

MMSettings LoadSettings();
void       SaveSettings(const MMSettings& s);

// Up to MM_MAX_PROFILES (1-based slots) named configuration snapshots the
// user can switch between from the config dialog.
bool LoadProfile(int slot, MMSettings& out, wchar_t* nameOut, size_t nameCap);
void SaveProfile(int slot, const wchar_t* name, const MMSettings& s);
void DeleteProfile(int slot);
