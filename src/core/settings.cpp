#include "settings.h"
#include "platform.h"
#include "text.h"
#include <cstdio>
#include <cstdlib>
#include <cwchar>

Settings& AppSettings() {
    static Settings s;
    return s;
}

static std::wstring SettingsPath() {
    const std::wstring custom = EnvVar("DREAMTREE_SETTINGS");  // separate profiles, UI tests
    if (!custom.empty()) return custom;
    return PathJoin(AppDataDir(), L"settings.ini");
}

void Settings::Load() {
    std::string raw;
    if (!ReadWholeFile(SettingsPath(), raw, 1u << 22)) return;
    if (raw.compare(0, 3, "\xEF\xBB\xBF") == 0) raw.erase(0, 3);  // saved by an editor that writes a UTF-8 BOM
    const std::wstring text = Widen(raw);
    recent.clear();
    trusted.clear();
    tabs.clear();
    accounts.clear();
    values.clear();
    for (size_t p = 0; p < text.size();) {
        size_t e = text.find(L'\n', p);
        if (e == std::wstring::npos) e = text.size();
        std::wstring line = text.substr(p, e - p);
        p = e + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == L"pullMode") pullMode = std::clamp((int)std::wcstol(v.c_str(), nullptr, 10), 0, 2);
        else if (k == L"diffSplit") diffSplit = v == L"1";
        else if (k == L"recent") { if (!v.empty()) recent.push_back(v); }
        else if (k == L"trusted") { if (!v.empty()) Trust(v); }
        else if (k == L"tab") { if (!v.empty()) tabs.push_back(v); }
        else if (k == L"active") activeTab = (int)std::wcstol(v.c_str(), nullptr, 10);
        else if (k == L"restoreTabs") restoreTabs = v != L"0";
        else if (k == L"account") { if (!v.empty()) accounts.push_back(v); }
        else Set(k, v);
    }
}

bool Settings::Save() const {
    std::wstring s;
    auto kv = [&](const std::wstring& k, const std::wstring& v) {
        s += k;
        s += L"=";
        s += v;
        s += L"\r\n";
    };
    for (const auto& [k, v] : values) kv(k, v);
    kv(L"pullMode", std::to_wstring(pullMode));
    kv(L"diffSplit", diffSplit ? L"1" : L"0");
    for (const auto& t : tabs) kv(L"tab", t);
    kv(L"active", std::to_wstring(activeTab));
    kv(L"restoreTabs", restoreTabs ? L"1" : L"0");
    for (const auto& a : accounts) kv(L"account", a);
    for (const auto& r : recent) kv(L"recent", r);
    for (const auto& t : trusted) kv(L"trusted", t);
    return WriteFileAtomic(SettingsPath(), Narrow(s));
}

std::wstring Settings::Get(std::wstring_view key, std::wstring_view def) const {
    for (const auto& [k, v] : values)
        if (k == key) return v;
    return std::wstring(def);
}

int Settings::GetInt(std::wstring_view key, int def) const {
    const std::wstring v = Get(key);
    return v.empty() ? def : (int)std::wcstol(v.c_str(), nullptr, 10);
}

float Settings::GetFloat(std::wstring_view key, float def) const {
    const std::wstring v = Get(key);
    return v.empty() ? def : (float)std::wcstod(v.c_str(), nullptr);
}

bool Settings::GetBool(std::wstring_view key, bool def) const {
    const std::wstring v = Get(key);
    return v.empty() ? def : v != L"0";
}

void Settings::Set(std::wstring_view key, const std::wstring& value) {
    for (auto& [k, v] : values)
        if (k == key) {
            v = value;
            return;
        }
    values.emplace_back(std::wstring(key), value);
}

void Settings::SetInt(std::wstring_view key, int v) { Set(key, std::to_wstring(v)); }

void Settings::SetFloat(std::wstring_view key, float v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, (double)v);  // always a '.' decimal point: the locale is "C"
    Set(key, Widen(buf));
}

void Settings::SetBool(std::wstring_view key, bool v) { Set(key, v ? L"1" : L"0"); }

void Settings::AddRecent(const std::wstring& path) {
    const std::wstring low = ToLower(path);
    recent.erase(std::remove_if(recent.begin(), recent.end(), [&](const std::wstring& r) { return ToLower(r) == low; }), recent.end());
    recent.insert(recent.begin(), path);
    if (recent.size() > 20) recent.resize(20);
}

void Settings::Trust(const std::wstring& dir) {
    if (std::find(trusted.begin(), trusted.end(), dir) == trusted.end()) trusted.push_back(dir);
}

// git reports the path with forward slashes; folders inside a trusted one are trusted too.
std::wstring Settings::SafeDirFor(const std::wstring& path) const {
    std::wstring fwd = path;
    for (auto& c : fwd) if (c == L'\\') c = L'/';
    const std::wstring low = ToLower(fwd);
    for (const auto& t : trusted) {
        const std::wstring lt = ToLower(t);
        if (low == lt || (low.size() > lt.size() && low.compare(0, lt.size(), lt) == 0 && low[lt.size()] == L'/')) return t;
    }
    return L"";
}
