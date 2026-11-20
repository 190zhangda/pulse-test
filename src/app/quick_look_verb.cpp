#include "quick_look_verb.h"
#include "../common/app_runtime_paths.h"
#include "../common/localization.h"

#include <string>

namespace pulse::app {
namespace {
// The three background keys put the same verb on an empty patch of a folder, so
// Explorer and the desktop offer it on their backgrounds as well as on items.
constexpr const wchar_t* kVerbKeys[] = {
    L"Software\\Classes\\*\\shell\\PulseQuickLook",
    L"Software\\Classes\\Directory\\shell\\PulseQuickLook",
    L"Software\\Classes\\DesktopBackground\\shell\\PulseQuickLook",
    L"Software\\Classes\\Directory\\Background\\shell\\PulseQuickLook",
    L"Software\\Classes\\AllFilesystemObjects\\shell\\PulseQuickLook",
};
constexpr ULONGLONG kSyncPeriodMs = 2000;

std::wstring CommandValue(const std::wstring& exe) {
    return L"\"" + exe + L"\" --quick-look \"%1\"";
}

bool SetString(HKEY key, const wchar_t* name, const std::wstring& value, const QuickLookRegistryApi& api) {
    return api.set_value(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool CreateKeyWith(const std::wstring& path,
                   std::initializer_list<std::pair<const wchar_t*, std::wstring>> values,
                   const QuickLookRegistryApi& api) {
    HKEY h = nullptr;
    if (api.create_key(api.root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok = true;
    for (const auto& [name, value] : values) ok = SetString(h, name, value, api) && ok;
    RegCloseKey(h);
    return ok;
}

// Reads a verb's command default value; empty when the key is missing.
std::wstring ReadCommand(const std::wstring& path, const QuickLookRegistryApi& api) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(api.root, path.c_str(), 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) return {};
    std::wstring value(4096, L'\0');
    DWORD type = REG_SZ;
    DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    const LSTATUS status = api.query_value(h, L"", nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes);
    RegCloseKey(h);
    if (status != ERROR_SUCCESS || type != REG_SZ) return {};
    value.resize(bytes / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

bool InstallOne(const std::wstring& key, const std::wstring& exe, const std::wstring& title,
                const QuickLookRegistryApi& api) {
    const bool verb = CreateKeyWith(key,
        {{L"MUIVerb", title}, {L"Icon", L"\"" + exe + L"\",0"}, {L"MultiSelectModel", L"Player"}}, api);
    const bool command = CreateKeyWith(key + L"\\command", {{L"", CommandValue(exe)}}, api);
    return verb && command;
}

struct SyncState {
    ULONGLONG next_sync = 0;
    std::wstring synced_exe; // empty until a successful install
    bool removed_once = false;
};
SyncState& State() {
    static SyncState state;
    return state;
}
} // namespace

bool InstallQuickLookVerb(const std::wstring& exe, const std::wstring& title, const QuickLookRegistryApi& api) {
    if (exe.empty()) return false;
    bool ok = true;
    for (const wchar_t* key : kVerbKeys) ok = InstallOne(key, exe, title, api) && ok;
    return ok;
}

bool RemoveQuickLookVerb(const QuickLookRegistryApi& api) {
    bool ok = true;
    for (const wchar_t* key : kVerbKeys) {
        // The command subkey has to go first: SHDeleteKeyW refuses a tree that
        // still has children.
        api.delete_key(api.root, (std::wstring(key) + L"\\command").c_str(), 0, 0);
        const LSTATUS status = api.delete_key(api.root, key, 0, 0);
        // A missing key means the verb was never installed; that is success.
        ok = (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND) && ok;
    }
    return ok;
}

bool QuickLookVerbMatches(const std::wstring& exe, const QuickLookRegistryApi& api) {
    if (exe.empty()) return false;
    const std::wstring expected = CommandValue(exe);
    for (const wchar_t* key : kVerbKeys) {
        if (ReadCommand(std::wstring(key) + L"\\command", api) != expected) return false;
    }
    return true;
}

void TickQuickLookVerb(bool enabled, bool persist, ULONGLONG now) {
    SyncState& st = State();
    if (now < st.next_sync) return;
    st.next_sync = now + kSyncPeriodMs;
    if (!persist) return;
    if (!enabled) {
        if (!st.removed_once || !st.synced_exe.empty()) {
            if (RemoveQuickLookVerb()) {
                st.synced_exe.clear();
                st.removed_once = true;
            }
        }
        return;
    }
    const std::wstring exe = app_runtime_paths::StableEntryPath();
    if (exe.empty()) return;
    if (exe == st.synced_exe && QuickLookVerbMatches(exe)) return;
    if (InstallQuickLookVerb(exe, l10n::Get(l10n::StringId::QuickPreview))) {
        st.synced_exe = exe;
        st.removed_once = false;
    }
}
} // namespace pulse::app
