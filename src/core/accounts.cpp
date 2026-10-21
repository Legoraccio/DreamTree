#include "accounts.h"
#include "platform.h"
#include "settings.h"
#include "text.h"
#include <chrono>
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

// Reads the members at the top level of the JSON object a server answers with: strings decoded, other values
// (numbers, true, ...) as written.
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
            } else {
                const size_t from = p;
                if (!Skip()) return {};
                if (name == key) return std::string(TrimSv(s.substr(from, p - from)));
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

// Mangrove's OAuth app on github.com: Device Flow on, user tokens that don't expire (Mangrove doesn't renew them),
// see the README. The device flow needs no client secret, and a client ID is public. Empty: no signing in with the
// browser.
constexpr char kGitHubClientId[] = "Ov23li3hoJcbPe6kwo7l";

// The client ID in use: MANGROVE_GITHUB_CLIENT_ID (another OAuth app, to try it) or Mangrove's.
const std::string& GitHubClientId() {
    static const std::string id = [] {
        const std::wstring env = EnvVar("MANGROVE_GITHUB_CLIENT_ID");
        return env.empty() ? std::string(kGitHubClientId) : Narrow(env);
    }();
    return id;
}

std::string FormEncode(std::string_view s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : s) {
        const unsigned char c = (unsigned char)ch;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

int ToInt(const std::string& s, int def) {
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    return end && end != s.c_str() ? (int)std::clamp(v, -1000000L, 1000000L) : def;
}

// What the service's OAuth error codes mean for the user.
std::wstring SignInError(int status, const std::string& code, const std::string& description) {
    if (code == "device_flow_disabled") return L"Signing in with the browser isn't enabled for Mangrove's GitHub app.";
    if (code == "incorrect_client_credentials" || code == "unauthorized_client" || code == "invalid_client")
        return L"GitHub doesn't know Mangrove's app: use a token instead.";
    if (code == "incorrect_device_code" || code == "bad_verification_code") return L"GitHub didn't recognize the code: start again.";
    if (!code.empty())
        return L"GitHub refused the sign-in (" + Widen(code) + L")" + (description.empty() ? L"." : L": " + Widen(description));
    return L"GitHub answered with error " + std::to_wstring(status) + L".";
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
        return server + L"/-/user_settings/personal_access_tokens?name=Mangrove&scopes=read_user,read_repository,write_repository";
    case Forge::Gitea: return server + L"/user/settings/applications";
    default: return server + L"/settings/tokens/new?scopes=repo,workflow,read:user&description=Mangrove";
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
    std::vector<std::string> headers{ "User-Agent: Mangrove", "Accept: application/json" };
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
    const std::wstring label = std::wstring(L"Mangrove: ") + InfoOf(a.forge).name + L" token of " + a.login + L" on " + a.Host();
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

// ---------------------------------------------------------------- signing in with the browser

bool BrowserSignInAvailable(Forge f, const std::wstring& server) {
    std::wstring why;
    return f == Forge::GitHub && !GitHubClientId().empty() && NormalizeServer(server, &why) == L"https://github.com";
}

DeviceCode ParseDeviceCode(int status, const std::string& body) {
    DeviceCode d;
    Json json(body);
    const std::string error = json.Member("error");
    if (status != 200 || !error.empty()) {
        d.error = SignInError(status, error, json.Member("error_description"));
        return d;
    }
    d.deviceCode = json.Member("device_code");
    d.userCode = Widen(json.Member("user_code"));
    d.verifyUrl = Widen(json.Member("verification_uri"));
    d.interval = std::clamp(ToInt(json.Member("interval"), 5), 1, 60);
    d.expiresIn = std::clamp(ToInt(json.Member("expires_in"), 900), 60, 3600);
    if (d.deviceCode.empty() || d.userCode.empty()) d.error = L"GitHub gave an unexpected answer.";
    return d;
}

DevicePoll ParseDevicePoll(int status, const std::string& body) {
    DevicePoll p;
    Json json(body);
    const std::string token = json.Member("access_token"), error = json.Member("error");
    if (!token.empty() && error.empty()) {
        p.state = DeviceState::Granted;
        p.token = token;
    } else if (error == "authorization_pending") {
        p.state = DeviceState::Pending;
    } else if (error == "slow_down") {
        p.state = DeviceState::SlowDown;
        p.interval = std::clamp(ToInt(json.Member("interval"), 0), 0, 120);
    } else if (error == "access_denied") {
        p.state = DeviceState::Denied;
    } else if (error == "expired_token") {
        p.state = DeviceState::Expired;
    } else {
        p.error = SignInError(status, error, json.Member("error_description"));
    }
    return p;
}

TokenCheck BrowserSignIn(Forge f, const std::wstring& serverInput, const std::function<void(const DeviceCode&)>& onCode,
                         const std::atomic<bool>& cancel, std::string* token) {
    TokenCheck r;
    std::wstring why;
    const std::wstring server = NormalizeServer(serverInput, &why);
    if (!BrowserSignInAvailable(f, server)) {
        r.error = L"Signing in with the browser isn't available for this server: use a token.";
        return r;
    }
    const std::vector<std::string> headers{ "User-Agent: Mangrove", "Accept: application/json" };
    const std::string client = "client_id=" + FormEncode(GitHubClientId());
    HttpResponse res = HttpPost("https://github.com/login/device/code", headers, client + "&scope=" + FormEncode("repo workflow read:user"));
    if (res.status == 0) {
        r.error = L"Couldn't reach github.com: " + Widen(res.error);
        return r;
    }
    DeviceCode code = ParseDeviceCode(res.status, res.body);
    if (!code.error.empty()) {
        r.error = code.error;
        return r;
    }
    // The page that takes the code is the server's own: anything else isn't opened.
    if (code.verifyUrl.compare(0, server.size() + 1, server + L"/") != 0) code.verifyUrl = server + L"/login/device";
    if (onCode) onCode(code);

    const std::string ask = client + "&device_code=" + FormEncode(code.deviceCode) +
                            "&grant_type=" + FormEncode("urn:ietf:params:oauth:grant-type:device_code");
    const double deadline = NowMs() + code.expiresIn * 1000.0;
    int interval = code.interval;
    for (;;) {
        for (const double until = NowMs() + interval * 1000.0; NowMs() < until;) {  // waits, and stops at once on cancel
            if (cancel) {
                r.error = L"Cancelled.";
                return r;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (NowMs() > deadline) {
            r.error = L"The code expired before it was used: start again.";
            return r;
        }
        res = HttpPost("https://github.com/login/oauth/access_token", headers, ask);
        if (cancel) {
            r.error = L"Cancelled.";
            return r;
        }
        if (res.status == 0) continue;  // the network failed for a moment: ask again at the next turn
        const DevicePoll p = ParseDevicePoll(res.status, res.body);
        switch (p.state) {
        case DeviceState::Pending: break;
        case DeviceState::SlowDown: interval = std::max(interval + 5, p.interval); break;
        case DeviceState::Denied: r.error = L"Mangrove wasn't authorized on GitHub."; return r;
        case DeviceState::Expired: r.error = L"The code expired before it was used: start again."; return r;
        case DeviceState::Failed: r.error = p.error; return r;
        case DeviceState::Granted:
            r = CheckToken(f, server, p.token);
            if (r.ok && token) *token = p.token;
            return r;
        }
    }
}

std::vector<std::pair<std::string, std::string>> GitAuthConfig(const Account& a, const std::string& token) {
    const std::string server = Narrow(a.server), host = Narrow(a.Host().substr(0, a.Host().find(L'/')));  // "github.com"
    return {
        // Basic authorization with the user name and the token, as git would send it after asking for a password.
        { "http." + server + "/.extraHeader", "Authorization: Basic " + Base64(Narrow(a.login) + ":" + std::string(TrimSv(token))) },
        { "url." + server + "/.insteadOf", "git@" + host + ":" },
        { "url." + server + "/.insteadOf", "ssh://git@" + host + "/" },
    };
}

std::vector<std::string> GitAuthEnvironment(const std::vector<Account>& accounts) {
    std::vector<std::string> env;
    int n = 0;
    for (const Account& a : accounts) {
        std::string token;
        if (!TokenOf(a, token)) continue;
        for (const auto& [key, value] : GitAuthConfig(a, token)) {
            env.push_back("GIT_CONFIG_KEY_" + std::to_string(n) + "=" + key);
            env.push_back("GIT_CONFIG_VALUE_" + std::to_string(n) + "=" + value);
            ++n;
        }
    }
    if (n) env.insert(env.begin(), "GIT_CONFIG_COUNT=" + std::to_string(n));
    return env;
}
