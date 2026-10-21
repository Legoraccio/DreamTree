#pragma once
#include "base.h"

// Mangrove's settings, shared by the apps: a key=value text file (settings.ini in AppDataDir(), or the
// file named by MANGROVE_SETTINGS). Preferences all the apps use have fields; anything else (window
// placement, zoom, ...) is kept by key for the app that owns it.
class Settings {
public:
    int pullMode = 0;         // 0 fast-forward if possible, 1 fast-forward only, 2 rebase
    bool diffSplit = false;   // side-by-side diffs
    std::vector<std::wstring> recent;   // repositories, most recent first
    std::vector<std::wstring> trusted;  // folders trusted despite another owner (safe.directory)
    std::vector<std::wstring> tabs;     // repositories open at exit
    int activeTab = 0;
    bool restoreTabs = true;            // reopen them at launch
    std::vector<std::wstring> accounts;  // see accounts.h (no secrets here)

    void Load();
    bool Save() const;

    std::wstring Get(std::wstring_view key, std::wstring_view def = L"") const;
    int GetInt(std::wstring_view key, int def) const;
    float GetFloat(std::wstring_view key, float def) const;
    bool GetBool(std::wstring_view key, bool def) const;
    void Set(std::wstring_view key, const std::wstring& value);
    void SetInt(std::wstring_view key, int v);
    void SetFloat(std::wstring_view key, float v, int decimals = 2);
    void SetBool(std::wstring_view key, bool v);

    void AddRecent(const std::wstring& path);
    void Trust(const std::wstring& dir);
    std::wstring SafeDirFor(const std::wstring& path) const;  // the trusted folder holding path, or empty

private:
    std::vector<std::pair<std::wstring, std::wstring>> values;
};

Settings& AppSettings();
std::wstring SettingsFile();  // where they are read from and saved: MANGROVE_SETTINGS, or settings.ini in AppDataDir()
