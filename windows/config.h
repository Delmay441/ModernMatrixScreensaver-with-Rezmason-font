// config.h — the /c settings dialog. Edits `settings` in place and persists on OK.
// Returns true if the user saved changes.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../core/mmcore.h"

bool ShowConfigDialog(HINSTANCE inst, HWND parent, MMSettings& settings);
