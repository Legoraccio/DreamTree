#include "accounts.h"
#include "platform.h"
#include "settings.h"
#include "text.h"
#include <cstdlib>

namespace {

const std::vector<ForgeInfo> kForges = {
    { Forge::GitHub, L"GitHub", L"https://github.com",
      L"A classic token with the repo scope (and workflow, to push workflow files), or a fine-grained token with "
      L"read and write access to the contents of the repositories." },
    { Forge::GitLab, L"GitLab", L"https://gitlab.com",
      L"A personal access token with the read_user, read_repository and write_repository scopes." },
    { Forge::Gitea, L"Gitea / Forgejo", L"https://codeberg.org",
      L"An access token with read and write permission on repositories and read permission on the user." },
};

const wchar_t* ForgeId(Forge f) {
    switch (f) {
    case Forge::GitLab: return L"gitlab";
    case Forge::Gitea: return L"gitea";
    default: return L"github";
    }
}

bool ForgeFromId(std::wstring_view id, Forge& f) {
    for (const ForgeInfo& i : kForges)
        if (id == ForgeId(i.forge)) {
            f = i.forge;
            return true;
        }
    return false;
}

// The token of each account, read from the credential store once.
std::mutex g_tokensMutex;
std::unordered_map<std::wstring, std::string> g_tokens;

bool TokenOf(const Account& a, std::string& token) {
    std::lock_guard<std::mutex> lk(g_tokensMutex);
    const std::wstring key = a.Key();
    auto it = g_tokens.find(key);
    if (it == g_tokens.end()) {
        std::string t;
        if (!SecretLoad(key, t)) t.clear();
        it = g_tokens.emplace(key, std::move(t)).first;
    }
    token = it->second;
    return !token.empty();
}

void ForgetToken(const Account& a) {
    std::lock_guard<std::mutex> lk(g_tokensMutex);
    g_tokens.erase(a.Key());
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

// Reads the string members at the top level of the JSON object a server answers with.
class Json {
public:
    explicit Json(std::string_view text) : s(text) {}

    std::string Member(std::string_view key) {
        p = 0;
        Space();
        if (!Eat('{')) return {};
        for (;;) {
            Space();
            std::string name;
            if (!String(name)) return {};
            Space();
            if (!Eat(':')) return {};
            Space();
            if (p < s.size() && s[p] == '"') {
                std::string value;
                if (!String(value)) return {};
                if (name == key) return value;
            } else if (!Skip()) {
                return {};
            }
            Space();
            if (!Eat(',')) return {};
        }
    }

private:
    std::string_view s;
    size_t p = 0;

    void Space() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) ++p;
    }
    bool Eat(char c) {
        if (p >= s.size() || s[p] != c) return false;
        ++p;
        return true;
    }
    bool Hex4(uint32_t& v) {
        if (p + 4 > s.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s[p++];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return false;
            v = v * 16 + (uint32_t)d;
        }
        return true;
    }
    bool String(std::string& out) {
        if (!Eat('"')) return false;
        while (p < s.size()) {
            const char c = s[p++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (p >= s.size()) return false;
            const char e = s[p++];
            switch (e) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= s.size() && s[p] == '\\' && s[p + 1] == 'u') {
                    const size_t back = p;
                    p += 2;
                    uint32_t lo = 0;
                    if (Hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else p = back;
                }
                AppendUtf8(out, cp);
                break;
            }
            default: out.push_back(e);  // \" \\ \/
            }
        }
        return false;
    }
    bool Skip() {  // a number, true, false, null, an object or an array
        if (p >= s.size()) return false;
        if (s[p] == '{' || s[p] == '[') {
            int depth = 0;
            while (p < s.size()) {
                if (s[p] == '"') {
                    std::string ignored;
                    if (!String(ignored)) return false;
                    continue;
                }
                const char c = s[p++];
                if (c == '{' || c == '[') ++depth;
                else if ((c == '}' || c == ']') && --depth == 0) return true;
            }
            return false;
        }
        while (p < s.size() && s[p] != ',' && s[p] != '}' && s[p] != ']') ++p;
        return true;
    }
};

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s)
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
    return s;
}

}  // namespace

const std::vector<ForgeInfo>& Forges() { return kForges; }

const ForgeInfo& InfoOf(Forge f) {
    for (const ForgeInfo& i : kForges)
        if (i.forge == f) return i;
    return kForges[0];
}

std::wstring Account::Host() const {
    std::wstring h = server;
    if (h.compare(0, 8, L"https://") == 0) h.erase(0, 8);
    return h;
}

std::wstring Account::Key() const { return std::wstring(ForgeId(forge)) + L":" + server; }

std::vector<Account> LoadAccounts() {
    std::vector<Account> out;
    for (const std::wstring& line : AppSettings().accounts) {
        // service \t server \t login \t name
        std::vector<std::wstring> f;
        for (size_t p = 0;;) {
            const size_t t = line.find(L'\t', p);
            f.push_back(line.substr(p, t == std::wstring::npos ? std::wstring::npos : t - p));
            if (t == std::wstring::npos) break;
            p = t + 1;
        }
        Account a;
        if (f.size() < 3 || !ForgeFromId(f[0], a.forge) || f[1].empty() || f[2].empty()) continue;
        a.server = f[1];
        a.login = f[2];
        if (f.size() > 3) a.name = f[3];
        out.push_back(std::move(a));
    }
    return out;
}

static void SaveAccounts(const std::vector<Account>& list) {
    Settings& s = AppSettings();
    s.accounts.clear();
    for (const Account& a : list)
        s.accounts.push_back(std::wstring(ForgeId(a.forge)) + L"\t" + a.server + L"\t" + a.login + L"\t" + a.name);
    s.Save();
}

std::wstring NormalizeServer(const std::wstring& input, std::wstring* error) {
    std::wstring s = TrimW(input);
    auto fail = [&](const wchar_t* why) {
        if (error) *error = why;
        return std::wstring();
    };
    if (s.empty()) return fail(L"Enter the address of the server.");
    const std::wstring low = Lower(s);
    if (low.compare(0, 7, L"http://") == 0) return fail(L"Use an https:// address: tokens are never sent without encryption.");
    if (low.compare(0, 8, L"https://") != 0) {
        if (low.find(L"://") != std::wstring::npos) return fail(L"Use an https:// address.");
        s = L"https://" + s;
    }
    while (s.size() > 8 && s.back() == L'/') s.pop_back();
    const size_t slash = s.find(L'/', 8);
    std::wstring host = s.substr(8, slash == std::wstring::npos ? std::wstring::npos : slash - 8);
    if (host.empty() || host.find_first_of(L" \t@?#") != std::wstring::npos) return fail(L"This is not a server address.");
    return L"https://" + Lower(host) + (slash == std::wstring::npos ? L"" : s.substr(slash));
}

std::wstring TokenPageUrl(Forge f, const std::wstring& server) {
    switch (f) {
    case Forge::GitLab:
        return server + L"/-/user_settings/personal_access_tokens?name=DreamTree&scopes=read_user,read_repository,write_repository";
    case Forge::Gitea: return server + L"/user/settings/applications";
    default: return server + L"/settings/tokens/new?scopes=repo,workflow,read:user&description=DreamTree";
    }
}

TokenCheck CheckToken(Forge f, const std::wstring& serverInput, const std::string& tokenInput) {
    TokenCheck r;
    const std::string token(TrimSv(tokenInput));
    std::wstring why;
    const std::wstring server = NormalizeServer(serverInput, &why);
    if (server.empty()) {
        r.error = why;
        return r;
    }
    if (token.empty()) {
        r.error = L"Paste the token.";
        return r;
    }
    if (token.find_first_of(" \t\r\n") != std::string::npos) {
        r.error = L"A token has no spaces: check what was pasted.";
        return r;
    }
    const std::wstring service = InfoOf(f).name;
    const std::string base = Narrow(server);
    std::string url;
    std::vector<std::string> headers{ "User-Agent: DreamTree", "Accept: application/json" };
    switch (f) {
    case Forge::GitHub:
        url = server == L"https://github.com" ? "https://api.github.com/user" : base + "/api/v3/user";  // GitHub Enterprise Server
        headers.push_back("Authorization: Bearer " + token);
        headers.push_back("X-GitHub-Api-Version: 2022-11-28");
        break;
    case Forge::GitLab:
        url = base + "/api/v4/user";
        headers.push_back("Authorization: Bearer " + token);
        break;
    case Forge::Gitea:
        url = base + "/api/v1/user";
        headers.push_back("Authorization: token " + token);
        break;
    }
    Account a;
    a.forge = f;
    a.server = server;
    const HttpResponse res = HttpGet(url, headers);
    if (res.status == 0) {
        r.error = L"Couldn't reach " + a.Host() + L": " + Widen(res.error);
        return r;
    }
    Json json(res.body);
    if (res.status == 401 || res.status == 403) {
        const std::string message = json.Member("message");
        r.error = service + L" refused the token: it may be mistyped, expired or revoked." +
                  (message.empty() ? std::wstring() : L" (" + Widen(message) + L")");
        return r;
    }
    if (res.status == 404) {
        r.error = a.Host() + L" doesn't answer like a " + service + L" server: check the address.";
        return r;
    }
    if (res.status != 200) {
        r.error = a.Host() + L" answered with error " + std::to_wstring(res.status) + L".";
        return r;
    }
    a.login = Widen(json.Member(f == Forge::GitLab ? "username" : "login"));
    a.name = Widen(json.Member(f == Forge::Gitea ? "full_name" : "name"));
    if (a.login.empty()) {
        r.error = a.Host() + L" gave an unexpected answer: is it a " + service + L" server?";
        return r;
    }
    // Classic GitHub tokens list their scopes (fine-grained ones don't): without repo, pushing to private
    // repositories fails.
    if (f == Forge::GitHub && res.Has("x-oauth-scopes")) {
        bool repo = false;
        const std::string scopes = res.Header("x-oauth-scopes");
        for (size_t p = 0; p <= scopes.size();) {
            size_t e = scopes.find(',', p);
            if (e == std::string::npos) e = scopes.size();
            repo |= TrimSv(std::string_view(scopes).substr(p, e - p)) == "repo";
            p = e + 1;
        }
        if (!repo) r.note = L"This token has no repo scope: pushing to private repositories will fail.";
    }
    r.ok = true;
    r.account = a;
    return r;
}

bool AddAccount(const Account& a, const std::string& token, std::wstring* error) {
    const std::wstring label = std::wstring(L"DreamTree: ") + InfoOf(a.forge).name + L" token of " + a.login + L" on " + a.Host();
    if (!SecretSave(a.Key(), std::string(TrimSv(token)), label)) {
        if (error) *error = L"Couldn't store the token in " + SecretStoreName() + L".";
        return false;
    }
    ForgetToken(a);
    std::vector<Account> list = LoadAccounts();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Account& x) { return x.Key() == a.Key(); }), list.end());
    list.push_back(a);
    SaveAccounts(list);
    return true;
}

void RemoveAccount(const Account& a) {
    SecretDelete(a.Key());
    ForgetToken(a);
    std::vector<Account> list = LoadAccounts();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Account& x) { return x.Key() == a.Key(); }), list.end());
    SaveAccounts(list);
}

std::vector<std::string> GitAuthEnvironment(const std::vector<Account>& accounts) {
    std::vector<std::string> env;
    int n = 0;
    for (const Account& a : accounts) {
        std::string token;
        if (!TokenOf(a, token)) continue;
        // Basic authorization with the user name and the token, as git would send it after asking for a password.
        env.push_back("GIT_CONFIG_KEY_" + std::to_string(n) + "=http." + Narrow(a.server) + "/.extraHeader");
        env.push_back("GIT_CONFIG_VALUE_" + std::to_string(n) + "=Authorization: Basic " + Base64(Narrow(a.login) + ":" + token));
        ++n;
    }
    if (n) env.insert(env.begin(), "GIT_CONFIG_COUNT=" + std::to_string(n));
    return env;
}
