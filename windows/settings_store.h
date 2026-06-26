// settings_store.h — persist MMSettings to HKCU\Software\Chewie\ModernMatrix.
// No sandbox on Windows, so the saver and the config dialog share one registry key.
#pragma once
#include "../core/mmcore.h"

MMSettings LoadSettings();
void       SaveSettings(const MMSettings& s);
