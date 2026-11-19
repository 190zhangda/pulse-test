#pragma once
// File Explorer "快速预览" verb: right-click a file or folder and preview it
// with Quick Look without opening Pulse first (quick_look_mode.cpp handles the
// command line it launches).
//
// The verb lives in HKCU only and is owned by this application: Pulse installs
// it while Settings > 启动与集成 > 快速预览 is on and removes it again when the
// switch goes off or Pulse is uninstalled.
#include <windows.h>

#include <string>

namespace pulse::app {
struct QuickLookRegistryApi {
    decltype(&RegCreateKeyExW) create_key = ::RegCreateKeyExW;
    decltype(&RegSetValueExW) set_value = ::RegSetValueExW;
    decltype(&RegDeleteKeyExW) delete_key = ::RegDeleteKeyExW;
    decltype(&RegQueryValueExW) query_value = ::RegQueryValueExW;
    HKEY root = HKEY_CURRENT_USER;
};
// Writes both verbs (files and directories). exe is the stable entry point the
// verb launches; title is the menu text.
bool InstallQuickLookVerb(const std::wstring& exe, const std::wstring& title,
                          const QuickLookRegistryApi& api = {});
bool RemoveQuickLookVerb(const QuickLookRegistryApi& api = {});
// True when both verbs exist and already point at exe, so a periodic sync can
// skip the rewrite.
bool QuickLookVerbMatches(const std::wstring& exe, const QuickLookRegistryApi& api = {});
// Called from the UI timer (about twice a second): installs the verb while the
// setting is on, removes it when the setting goes off, and leaves the registry
// alone for a portable run that must not persist anything.
void TickQuickLookVerb(bool enabled, bool persist, ULONGLONG now);
} // namespace pulse::app
