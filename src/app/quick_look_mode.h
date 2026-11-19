#pragma once
// Desktop Quick Look: `pulse.exe --quick-look <path>` shows the Quick Look
// window for one file or folder without creating the main window, so a
// context-menu verb can preview something straight from File Explorer.
//
// Called after OLE and localization initialization, before main application
// state exists; returns the process exit code.

namespace pulse::app {
int RunQuickLookMode(const wchar_t* path);
}
