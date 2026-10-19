// Repository loading through the git CLI.
//
// git's own C implementation (commit-graph, packfile caches, ...) is the fastest reader available, so
// DreamTree spawns it with machine-readable NUL-separated formats and parses the output while it
// streams. Refs, stashes, the commit log and the working-tree status are read concurrently.

#include "git.h"
#include "platform.h"
#include "text.h"
#include <cctype>
#include <cwchar>
#include <cwctype>

// ============================================================================ Oid

static inline int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool Oid::Parse(const char* s, size_t len, Oid& out) {
    if (len < 40) return false;
    for (int i = 0; i < 20; ++i) {
        int hi = HexVal(s[2 * i]), lo = HexVal(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.b[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

std::string Oid::HexA(int n) const {
    static const char* d = "0123456789abcdef";
    std::string s(40, '0');
    for (int i = 0; i < 20; ++i) { s[2 * i] = d[b[i] >> 4]; s[2 * i + 1] = d[b[i] & 15]; }
    s.resize((size_t)std::clamp(n, 0, 40));
    return s;
}

std::wstring Oid::Hex(int n) const {
    std::string a = HexA(n);
    return std::wstring(a.begin(), a.end());
}

bool Oid::IsZero() const {
    for (uint8_t x : b) if (x) return false;
    return true;
}

// ============================================================================ process runner

static int RunGit(const std::wstring& cwd, const std::wstring& args, std::string* out, std::string* err,
                  const Sink& sink = nullptr) {
    return RunProcess(GitPath(false), args, cwd, out, err, sink, nullptr);
}

GitOpResult RunGitOp(const std::wstring& root, const std::wstring& safeDir, const std::wstring& args,
                     const std::function<void(const std::string&)>& progress) {
    GitOpResult r;
    std::wstring cmd = L"--no-pager -c core.quotepath=false -c color.ui=false ";
    if (!safeDir.empty()) cmd += L"-c " + QuoteArg(L"safe.directory=" + safeDir) + L" ";
    cmd += args;
    std::string line;  // progress lines are terminated by \r (updates) or \n
    Sink errSink = [&](const char* p, size_t n) {
        if (!progress) return;
        for (size_t i = 0; i < n; ++i) {
            const char c = p[i];
            if (c == '\r' || c == '\n') {
                if (!line.empty()) progress(line);
                line.clear();
            } else if (line.size() < 400) {
                line.push_back(c);
            }
        }
    };
    r.code = RunProcess(GitPath(true), cmd, root, &r.out, &r.err, nullptr, errSink);
    return r;
}

// macOS ships /usr/bin/git as a stub that only works once the Command Line Tools are installed.
static const wchar_t kInstallTools[] = L"Git isn't installed yet: run \u201Cxcode-select --install\u201D in Terminal, or install Git with Homebrew.";
static bool MissingTools(const std::string& err) { return err.find("xcrun: error") != std::string::npos; }

std::wstring GitErrorText(const GitOpResult& r) {
    if (MissingTools(r.err)) return kInstallTools;
    const std::string& src = r.err.empty() ? r.out : r.err;
    std::string text;
    int lines = 0;
    size_t p = 0;
    while (p < src.size() && lines < 8) {
        size_t e = src.find('\n', p);
        if (e == std::string::npos) e = src.size();
        std::string_view line(src.data() + p, e - p);
        p = e + 1;
        if (size_t cr = line.rfind('\r'); cr != std::string_view::npos) {  // keep the final state of a progress line
            std::string_view tail = line.substr(cr + 1);
            line = tail.empty() ? line.substr(0, cr) : tail;
            if (size_t cr2 = line.rfind('\r'); cr2 != std::string_view::npos) line = line.substr(cr2 + 1);
        }
        while (!line.empty() && (unsigned char)line.front() <= ' ') line.remove_prefix(1);
        while (!line.empty() && (unsigned char)line.back() <= ' ') line.remove_suffix(1);
        if (line.empty() || line.substr(0, 5) == "hint:") continue;
        if (line.find("% (") != std::string_view::npos) continue;  // progress counters
        if (line.substr(0, 3) == "To " && line.find(' ', 3) == std::string_view::npos) continue;  // push destination URL
        for (const char* prefix : { "error: ", "fatal: ", "warning: " })
            if (line.substr(0, strlen(prefix)) == prefix) line.remove_prefix(strlen(prefix));
        if (!text.empty()) text += '\n';
        text.append(line);
        ++lines;
    }
    if (text.empty()) return L"git exited with code " + std::to_wstring(r.code) + L".";
    std::wstring w = Widen(text);
    if (!w.empty()) w[0] = (wchar_t)std::towupper((std::wint_t)w[0]);
    return w;
}

std::wstring CheckBranchName(const std::wstring& n) {
    if (n.empty()) return L"Enter a name for the new branch.";
    if (n[0] == L'-') return L"A branch name cannot start with '-'.";
    if (n == L"@" || n == L"HEAD") return L"'" + n + L"' is reserved by Git.";
    if (n.front() == L'/' || n.back() == L'/' || n.back() == L'.') return L"A branch name cannot start or end with '/' or end with '.'.";
    if (n.find(L"..") != std::wstring::npos || n.find(L"//") != std::wstring::npos || n.find(L"@{") != std::wstring::npos)
        return L"'..', '//' and '@{' are not allowed in branch names.";
    for (wchar_t c : n) {
        if (c < 32 || c == 127) return L"Control characters are not allowed.";
        if (wcschr(L" ~^:?*[\\", c)) return std::wstring(L"The character '") + c + L"' is not allowed in branch names.";
    }
    for (size_t p = 0;;) {
        const size_t s = n.find(L'/', p);
        const std::wstring_view comp = std::wstring_view(n).substr(p, s == std::wstring::npos ? std::wstring::npos : s - p);
        if (!comp.empty() && comp[0] == L'.') return L"No part of a branch name can start with '.'.";
        if (comp.size() >= 5 && comp.substr(comp.size() - 5) == L".lock") return L"No part of a branch name can end with '.lock'.";
        if (s == std::wstring::npos) break;
        p = s + 1;
    }
    return L"";
}

static std::wstring Git(const std::wstring& safeDir, std::wstring_view args);

std::wstring ReadHeadMessage(const std::wstring& root, const std::wstring& safeDir) {
    std::string out;
    if (RunGit(root, Git(safeDir, L"log -1 --format=%B HEAD --"), &out, nullptr) != 0) return L"";
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return Widen(out);
}

std::wstring ReadMergeMessage(const std::wstring& gitDir) {
    std::string data;
    if (!ReadWholeFile(PathJoin(gitDir, L"MERGE_MSG"), data, 1u << 16)) return L"";
    std::string kept;
    for (size_t p = 0; p < data.size();) {
        size_t e = data.find('\n', p);
        if (e == std::string::npos) e = data.size();
        std::string_view line(data.data() + p, e - p);
        p = e + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty() && line[0] == '#') continue;  // git's instructions, not part of the message
        kept.append(line);
        kept.push_back('\n');
    }
    while (!kept.empty() && (kept.back() == '\n' || kept.back() == ' ')) kept.pop_back();
    return Widen(kept);
}

static std::wstring Git(const std::wstring& safeDir, std::wstring_view args) {
    std::wstring s = L"--no-pager --no-optional-locks -c core.quotepath=false -c color.ui=false "
                     L"-c log.showSignature=false -c i18n.logOutputEncoding=UTF-8 ";
    if (!safeDir.empty()) s += L"-c " + QuoteArg(L"safe.directory=" + safeDir) + L" ";
    s += args;
    return s;
}

static std::wstring g_gitVersion;
static std::atomic<bool> g_gitVersionReady{ false };

std::wstring GitVersion() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::string out;
        if (RunGit(L"", L"--version", &out, nullptr) == 0) {
            // "git version 2.45.1.windows.1", "git version 2.39.3 (Apple Git-146)"
            size_t p = out.find("version ");
            std::string s = p == std::string::npos ? out : out.substr(p + 8);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            for (const char* suffix : { ".windows", " (" }) {
                const size_t w = s.find(suffix);
                if (w != std::string::npos) s.resize(w);
            }
            g_gitVersion = Widen(s);
        }
        g_gitVersionReady = true;
    });
    return g_gitVersion;
}

std::wstring GitVersionIfReady() { return g_gitVersionReady ? g_gitVersion : std::wstring(); }

static void StampFile(const std::wstring& p, uint64_t& h) {
    uint64_t v = 0x9E3779B97F4A7C15ULL;
    FileInfo(p, nullptr, &v);
    h = (h ^ v) * 1099511628211ULL;
}

static void StampDir(const std::wstring& dir, uint64_t& h, int depth) {
    if (depth > 12) return;
    ListDir(dir, [&](std::wstring_view name, uint64_t stamp, bool isDir) {
        h = (h ^ Fnv1aW(name)) * 1099511628211ULL;
        h = (h ^ stamp) * 1099511628211ULL;
        if (isDir) StampDir(PathJoin(dir, name), h, depth + 1);
    });
}

uint64_t RefsStamp(const std::wstring& gitDir, const std::wstring& commonDir) {
    uint64_t h = 1469598103934665603ULL;
    StampFile(PathJoin(gitDir, L"HEAD"), h);
    StampFile(PathJoin(gitDir, L"logs/HEAD"), h);
    StampFile(PathJoin(commonDir, L"packed-refs"), h);
    StampDir(PathJoin(commonDir, L"refs"), h, 0);
    return h;
}

// ============================================================================ parsing helpers

namespace {

int64_t ParseI64(const char* s, size_t n) {
    int64_t v = 0;
    bool neg = false;
    size_t i = 0;
    while (i < n && s[i] == ' ') ++i;
    if (i < n && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; ++i; }
    for (; i < n && s[i] >= '0' && s[i] <= '9'; ++i) v = v * 10 + (s[i] - '0');
    return neg ? -v : v;
}

void Trim(const char*& s, size_t& n) {
    while (n && (unsigned char)*s <= ' ') { ++s; --n; }
    while (n && (unsigned char)s[n - 1] <= ' ') --n;
}

// First non-empty line of a commit body, capped to keep the arena small.
void FirstLine(const char* s, size_t n, const char*& o, size_t& on) {
    const char* e = s + n;
    while (s < e) {
        const char* nl = (const char*)memchr(s, '\n', (size_t)(e - s));
        const char* le = nl ? nl : e;
        const char* ls = s;
        size_t ll = (size_t)(le - s);
        Trim(ls, ll);
        if (ll) {
            if (ll > 240) {
                ll = 240;
                while (ll && ((unsigned char)ls[ll] & 0xC0) == 0x80) --ll;  // keep UTF-8 sequences whole
            }
            o = ls;
            on = ll;
            return;
        }
        if (!nl) break;
        s = nl + 1;
    }
    o = e;
    on = 0;
}

void ParseOidList(const char* p, size_t n, std::vector<Oid>& out, uint16_t& count) {
    const char* e = p + n;
    while (p < e) {
        while (p < e && (*p == ' ' || *p == '\n')) ++p;
        const char* s = p;
        while (p < e && *p != ' ' && *p != '\n') ++p;
        Oid o;
        if (p > s && Oid::Parse(s, (size_t)(p - s), o)) { out.push_back(o); ++count; }
    }
}

// Splits a stream into records of N NUL-terminated fields.
template <int N, typename F>
struct RecordStream {
    std::string carry;
    F onRecord;
    explicit RecordStream(F f) : onRecord(std::move(f)) {}
    void Feed(const char* data, size_t n) {
        carry.append(data, n);
        size_t used = Parse(false);
        carry.erase(0, used);
    }
    void Finish() {
        Parse(true);
        carry.clear();
    }
    size_t Parse(bool final) {
        const char* base = carry.data();
        size_t len = carry.size(), pos = 0;
        const char* f[N];
        size_t fl[N];
        while (pos < len) {
            size_t p = pos;
            int k = 0;
            while (k < N) {
                const void* z = memchr(base + p, 0, len - p);
                if (!z) break;
                size_t e = (size_t)((const char*)z - base);
                f[k] = base + p;
                fl[k] = e - p;
                ++k;
                p = e + 1;
            }
            if (k < N) {
                if (final && k == N - 1) { f[k] = base + p; fl[k] = len - p; ++k; p = len; }
                else break;
            }
            onRecord(f, fl);
            pos = p;
        }
        return pos;
    }
};

struct Builder {
    std::vector<Commit> commits;
    std::vector<Oid> parentOids;
    std::string text;
    std::vector<std::pair<std::string, std::string>> authorRaw;
    std::unordered_map<uint64_t, uint32_t> authorMap;

    uint32_t InternAuthor(const char* n, size_t nl, const char* e, size_t el) {
        uint64_t h = 1469598103934665603ULL;
        for (size_t i = 0; i < nl; ++i) { h ^= (uint8_t)n[i]; h *= 1099511628211ULL; }
        h ^= 0xff;
        h *= 1099511628211ULL;
        for (size_t i = 0; i < el; ++i) { h ^= (uint8_t)tolower((uint8_t)e[i]); h *= 1099511628211ULL; }
        auto [it, inserted] = authorMap.try_emplace(h, (uint32_t)authorRaw.size());
        if (inserted) authorRaw.emplace_back(std::string(n, nl), std::string(e, el));
        return it->second;
    }
    uint32_t AddText(const char* s, size_t n) {
        uint32_t off = (uint32_t)text.size();
        text.append(s, n);
        return off;
    }
};

struct StashRaw {
    Oid id, parent;
    bool hasParent = false;
    std::string name, author, email, message;
    int64_t at = 0, ct = 0;
};

std::wstring Initials(const std::wstring& name, const std::wstring& email) {
    std::vector<std::wstring_view> parts;
    std::wstring_view s = name;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == L' ' || s[i] == L'.' || s[i] == L'_' || s[i] == L'-' || s[i] == L'(')) ++i;
        size_t st = i;
        while (i < s.size() && !(s[i] == L' ' || s[i] == L'.' || s[i] == L'_' || s[i] == L'-' || s[i] == L'(')) ++i;
        if (i > st) parts.push_back(s.substr(st, i - st));
    }
    auto first = [](std::wstring_view w) {
        if (w.size() >= 2 && IsHighSurrogate(w[0])) return std::wstring(w.substr(0, 2));
        return std::wstring(1, w[0]);
    };
    std::wstring r;
    if (parts.empty()) r = email.empty() ? L"?" : first(email);
    else {
        r = first(parts.front());
        if (parts.size() > 1) r += first(parts.back());
    }
    return ToUpper(r);
}

uint32_t HslToRgb(float h, float s, float l) {
    auto f = [&](float n) {
        float k = std::fmod(n + h * 12.f, 12.f);
        float a = s * std::min(l, 1.f - l);
        float v = l - a * std::max(-1.f, std::min({ k - 3.f, 9.f - k, 1.f }));
        return (uint32_t)std::clamp((int)std::lround(v * 255.f), 0, 255);
    };
    return (f(0) << 16) | (f(8) << 8) | f(4);
}

uint32_t AvatarColor(const std::wstring& key) {
    uint32_t h = Fnv1aW(ToLower(key));
    float hue = (float)(h % 360) / 360.f;
    float sat = 0.45f + (float)((h >> 9) % 20) / 100.f;
    float lit = 0.38f + (float)((h >> 17) % 10) / 100.f;
    return HslToRgb(hue, sat, lit);
}

std::vector<std::wstring> ReadSubmodules(const std::wstring& root) {
    std::vector<std::wstring> out;
    std::string data;
    if (!ReadWholeFile(PathJoin(root, L".gitmodules"), data, 1u << 20)) return out;
    size_t p = 0;
    while (p < data.size()) {
        size_t e = data.find('\n', p);
        if (e == std::string::npos) e = data.size();
        std::string_view line = TrimSv(std::string_view(data).substr(p, e - p));
        if (line.size() > 4 && line.substr(0, 4) == "path") {
            size_t eq = line.find('=');
            if (eq != std::string_view::npos) out.push_back(Widen(TrimSv(line.substr(eq + 1))));
        }
        p = e + 1;
    }
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return CompareNatural(a, b) < 0; });
    return out;
}

std::wstring FirstErrorLine(const std::string& err) {
    if (MissingTools(err)) return kInstallTools;
    std::string_view s = err;
    size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find('\n', p);
        if (e == std::string_view::npos) e = s.size();
        std::string_view line = TrimSv(s.substr(p, e - p));
        if (!line.empty()) {
            if (line.substr(0, 7) == "fatal: ") line.remove_prefix(7);
            else if (line.substr(0, 7) == "error: ") line.remove_prefix(7);
            std::wstring w = Widen(line);
            if (!w.empty()) w[0] = (wchar_t)std::towupper((std::wint_t)w[0]);
            return w;
        }
        p = e + 1;
    }
    return L"Git failed to read this folder.";
}

// Lines that must keep their own line when reflowing a commit body: lists, quotes, code,
// headings, indented text and trailers such as "Signed-off-by: ...".
bool StructuredLine(std::string_view l) {
    if (l.empty()) return true;
    const unsigned char c = (unsigned char)l[0];
    if (c == ' ' || c == '\t' || c == '>' || c == '|' || c == '`' || c == '=') return true;
    if ((c == '-' || c == '*' || c == '+' || c == '#') && l.size() > 1 && l[1] == ' ') return true;  // "- item", "# heading"
    if (l.size() >= 3 && c == 0xE2 && (unsigned char)l[1] == 0x80 && (unsigned char)l[2] == 0xA2) return true;  // bullet
    size_t i = 0;
    while (i < l.size() && isdigit((unsigned char)l[i])) ++i;
    if (i > 0 && i < l.size() && (l[i] == '.' || l[i] == ')')) return true;
    const size_t colon = l.find(": ");
    if (colon != std::string_view::npos && colon > 0 && colon < 40) {
        bool token = true;
        for (size_t k = 0; k < colon && token; ++k) token = isalnum((unsigned char)l[k]) || l[k] == '-';
        if (token) return true;
    }
    return false;
}

// Git bodies are hard-wrapped at ~72 columns; join wrapped prose so it flows in narrow panels.
std::string Reflow(std::string_view body) {
    std::string out;
    out.reserve(body.size());
    bool prevProse = false, blank = false;
    size_t p = 0;
    while (p <= body.size()) {
        size_t e = body.find('\n', p);
        if (e == std::string_view::npos) e = body.size();
        std::string_view line = body.substr(p, e - p);
        p = e + 1;
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.remove_suffix(1);
        if (line.empty()) {
            blank = !out.empty();
            prevProse = false;
            continue;
        }
        const bool structured = StructuredLine(line);
        if (!out.empty()) out += blank ? "\n\n" : (prevProse && !structured ? " " : "\n");
        out += line;
        blank = false;
        prevProse = !structured;
    }
    return out;
}

void BuildLabels(RepoData& d) {
    std::unordered_map<int32_t, std::vector<int>> byCommit;
    for (int i = 0; i < (int)d.refs.size(); ++i)
        if (d.refs[i].commit >= 0) byCommit[d.refs[i].commit].push_back(i);
    for (auto& [commit, list] : byCommit) {
        std::vector<Label> labels;
        std::vector<int> locals, remotes, tags;
        for (int i : list) {
            const Ref& r = d.refs[i];
            (r.kind == RefKind::Local ? locals : r.kind == RefKind::Remote ? remotes : tags).push_back(i);
        }
        std::stable_sort(locals.begin(), locals.end(), [&](int a, int b) { return d.refs[a].head > d.refs[b].head; });
        std::vector<bool> usedRemote(remotes.size(), false);
        for (int li : locals) {
            const Ref& r = d.refs[li];
            Label L;
            L.text = r.name;
            L.ref = li;
            L.local = true;
            L.head = r.head;
            // Merge with the tracked remote branch (or the same-named one) pointing at the same commit.
            int match = -1;
            for (size_t k = 0; k < remotes.size(); ++k)
                if (!usedRemote[k] && !r.upstream.empty() && d.refs[remotes[k]].name == r.upstream) { match = (int)k; break; }
            if (match < 0)
                for (size_t k = 0; k < remotes.size(); ++k)
                    if (!usedRemote[k] && d.refs[remotes[k]].branch == r.name) { match = (int)k; break; }
            if (match >= 0) {
                usedRemote[(size_t)match] = true;
                L.remote = true;
                L.remoteRef = remotes[(size_t)match];
            }
            labels.push_back(std::move(L));
        }
        for (size_t k = 0; k < remotes.size(); ++k) {
            if (usedRemote[k]) continue;
            Label L;
            L.text = d.refs[remotes[k]].name;
            L.ref = remotes[k];
            L.remote = true;
            labels.push_back(std::move(L));
        }
        for (int ti : tags) {
            Label L;
            L.text = d.refs[ti].name;
            L.ref = ti;
            L.tag = true;
            labels.push_back(std::move(L));
        }
        d.labels.emplace(commit, std::move(labels));
    }
}

}  // namespace

// ============================================================================ repository

std::shared_ptr<RepoData> LoadRepository(const std::wstring& path, const std::wstring& safeDir, bool headOnly) {
    auto d = std::make_shared<RepoData>();
    const double t0 = NowMs();
    d->requested = path;
    d->safeDir = safeDir;
    d->headOnly = headOnly;

    // 1. Validate and locate the repository, resolve HEAD.
    std::string out, err;
    RunGit(path, Git(safeDir, L"rev-parse --path-format=absolute --show-toplevel --git-dir --git-common-dir --verify -q HEAD"),
           &out, &err);
    std::vector<std::string_view> lines;
    for (size_t p = 0; p < out.size();) {
        size_t e = out.find('\n', p);
        if (e == std::string::npos) e = out.size();
        std::string_view l = TrimSv(std::string_view(out).substr(p, e - p));
        if (!l.empty()) lines.push_back(l);
        p = e + 1;
    }
    if (lines.size() < 3) {
        size_t k = err.find("dubious ownership");
        if (k != std::string::npos) {
            size_t q1 = err.find('\'', k);
            size_t q2 = q1 == std::string::npos ? std::string::npos : err.find('\'', q1 + 1);
            if (q2 != std::string::npos) d->untrustedPath = Widen(std::string_view(err).substr(q1 + 1, q2 - q1 - 1));
            else d->untrustedPath = path;
            d->error = L"This repository belongs to another user account, so Git refuses to read it until you trust it.";
        } else {
            d->error = err.empty() ? L"This folder is not a Git repository." : FirstErrorLine(err);
        }
        d->msTotal = NowMs() - t0;
        return d;
    }
    d->root = NormalizePath(Widen(lines[0]));
    d->gitDir = NormalizePath(Widen(lines[1]));
    d->commonDir = NormalizePath(Widen(lines[2]));
    d->name = BaseName(d->root);
    bool headValid = lines.size() >= 4 && Oid::Parse(lines[3].data(), lines[3].size(), d->headOid);
    d->unborn = !headValid;
    d->refsStamp = RefsStamp(d->gitDir, d->commonDir);  // taken before reading, so later changes are noticed
    for (const std::wstring* dir : { &d->gitDir, &d->commonDir })  // the last fetch wrote FETCH_HEAD
        if (FileInfo(PathJoin(*dir, L"FETCH_HEAD"), &d->fetchTime, nullptr)) break;
    const std::wstring& root = d->root;

    // 2. Refs and stashes in parallel with the (streamed) commit log.
    std::string refsOut, stashOut;
    std::thread tRefs([&] {
        RunGit(root, Git(safeDir, L"for-each-ref --format=%(refname)%00%(objectname)%00%(objecttype)%00%(*objectname)%00"
                                  L"%(*objecttype)%00%(upstream:short)%00%(upstream:track,nobracket)%00%(HEAD)%00%(symref) "
                                  L"refs/heads refs/remotes refs/tags"),
               &refsOut, nullptr);
    });
    std::string remotesOut;
    std::thread tStash([&] {
        RunGit(root, Git(safeDir, L"log -g -z --format=%H%x00%P%x00%gd%x00%an%x00%ae%x00%at%x00%ct%x00%gs refs/stash --"),
               &stashOut, nullptr);
        RunGit(root, Git(safeDir, L"remote"), &remotesOut, nullptr);
    });

    Builder b;
    b.commits.reserve(4096);
    auto onCommit = [&](const char** f, const size_t* fl) {
        const char* h = f[0];
        size_t hl = fl[0];
        while (hl && (*h == '\n' || *h == '\r')) { ++h; --hl; }
        Commit c;
        if (!Oid::Parse(h, hl, c.id)) return;
        c.parentOff = (uint32_t)b.parentOids.size();
        ParseOidList(f[1], fl[1], b.parentOids, c.parentCount);
        c.author = b.InternAuthor(f[2], fl[2], f[3], fl[3]);
        c.authorTime = ParseI64(f[4], fl[4]);
        c.commitTime = ParseI64(f[5], fl[5]);
        const char* s = f[6];
        size_t sl = fl[6];
        Trim(s, sl);
        c.subjOff = b.AddText(s, sl);
        c.subjLen = (uint32_t)sl;
        const char* bl = nullptr;
        size_t bll = 0;
        FirstLine(f[7], fl[7], bl, bll);
        c.bodyOff = b.AddText(bl, bll);
        c.bodyLen = (uint32_t)bll;
        if (c.parentCount > 1) c.flags |= CF_MERGE;
        b.commits.push_back(c);
    };
    RecordStream<8, decltype(onCommit)> logStream(onCommit);
    std::wstring revs = headOnly && headValid ? L"" : L"--branches --remotes --tags";
    if (headValid) revs += L" HEAD";
    const double tLog = NowMs();
    RunGit(root, Git(safeDir, L"log -z --format=%H%x00%P%x00%an%x00%ae%x00%at%x00%ct%x00%s%x00%b " + revs + L" --"), nullptr,
           nullptr, [&](const char* p, size_t n) { logStream.Feed(p, n); });
    logStream.Finish();
    tRefs.join();
    tStash.join();
    d->msGit = NowMs() - tLog;
    const double tBuild = NowMs();

    // 3. Stashes: only the stash commit itself is shown, attached to its base (first parent).
    std::vector<StashRaw> stashes;
    {
        auto onStash = [&](const char** f, const size_t* fl) {
            StashRaw s;
            const char* h = f[0];
            size_t hl = fl[0];
            while (hl && (*h == '\n' || *h == '\r')) { ++h; --hl; }
            if (!Oid::Parse(h, hl, s.id)) return;
            std::vector<Oid> ps;
            uint16_t pc = 0;
            ParseOidList(f[1], fl[1], ps, pc);
            if (pc) { s.parent = ps[0]; s.hasParent = true; }
            s.name.assign(f[2], fl[2]);
            s.author.assign(f[3], fl[3]);
            s.email.assign(f[4], fl[4]);
            s.at = ParseI64(f[5], fl[5]);
            s.ct = ParseI64(f[6], fl[6]);
            const char* m = f[7];
            size_t ml = fl[7];
            Trim(m, ml);
            s.message.assign(m, ml);
            stashes.push_back(std::move(s));
        };
        RecordStream<8, decltype(onStash)> st(onStash);
        st.Feed(stashOut.data(), stashOut.size());
        st.Finish();
    }

    std::unordered_map<Oid, int32_t, OidHash> index;
    index.reserve(b.commits.size() + stashes.size() + 16);
    for (int32_t i = 0; i < (int32_t)b.commits.size(); ++i) index.emplace(b.commits[i].id, i);
    for (size_t si = 0; si < stashes.size(); ++si) {
        const StashRaw& s = stashes[si];
        if (index.count(s.id)) continue;
        Commit c;
        c.id = s.id;
        c.flags = CF_STASH;
        c.stash = (int32_t)si;
        c.parentOff = (uint32_t)b.parentOids.size();
        if (s.hasParent) { b.parentOids.push_back(s.parent); c.parentCount = 1; }
        c.author = b.InternAuthor(s.author.data(), s.author.size(), s.email.data(), s.email.size());
        c.authorTime = s.at;
        c.commitTime = s.ct;
        c.subjOff = b.AddText(s.message.data(), s.message.size());
        c.subjLen = (uint32_t)s.message.size();
        c.bodyOff = (uint32_t)b.text.size();
        index.emplace(c.id, (int32_t)b.commits.size());
        b.commits.push_back(c);
    }

    // 4. Resolve parents and order commits: children before parents, otherwise newest first
    //    (equivalent to `git log --date-order`).
    const int32_t n = (int32_t)b.commits.size();
    std::vector<int32_t> rawPar(b.parentOids.size());
    for (size_t k = 0; k < b.parentOids.size(); ++k) {
        auto it = index.find(b.parentOids[k]);
        rawPar[k] = it == index.end() ? -1 : it->second;
    }
    std::vector<int32_t> indeg((size_t)n, 0);
    for (const Commit& c : b.commits)
        for (uint16_t j = 0; j < c.parentCount; ++j)
            if (int32_t p = rawPar[c.parentOff + j]; p >= 0) ++indeg[(size_t)p];

    auto later = [&](int32_t x, int32_t y) {  // heap "less": lower priority first
        const Commit& A = b.commits[(size_t)x];
        const Commit& B = b.commits[(size_t)y];
        if (A.commitTime != B.commitTime) return A.commitTime < B.commitTime;
        return x > y;
    };
    std::vector<int32_t> heap;
    heap.reserve((size_t)n);
    for (int32_t i = 0; i < n; ++i) if (indeg[(size_t)i] == 0) heap.push_back(i);
    std::make_heap(heap.begin(), heap.end(), later);
    std::vector<int32_t> order;
    order.reserve((size_t)n);
    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), later);
        int32_t c = heap.back();
        heap.pop_back();
        order.push_back(c);
        const Commit& cc = b.commits[(size_t)c];
        for (uint16_t j = 0; j < cc.parentCount; ++j) {
            int32_t p = rawPar[cc.parentOff + j];
            if (p >= 0 && --indeg[(size_t)p] == 0) {
                heap.push_back(p);
                std::push_heap(heap.begin(), heap.end(), later);
            }
        }
    }
    if ((int32_t)order.size() < n) {  // defensive: never happens in a DAG
        std::vector<uint8_t> seen((size_t)n, 0);
        for (int32_t c : order) seen[(size_t)c] = 1;
        for (int32_t i = 0; i < n; ++i) if (!seen[(size_t)i]) order.push_back(i);
    }
    std::vector<int32_t> pos((size_t)n);
    for (int32_t k = 0; k < n; ++k) pos[(size_t)order[(size_t)k]] = k;

    d->commits.resize((size_t)n);
    d->parents.reserve(rawPar.size());
    for (int32_t k = 0; k < n; ++k) {
        const Commit& src = b.commits[(size_t)order[(size_t)k]];
        Commit c = src;
        c.parentOff = (uint32_t)d->parents.size();
        for (uint16_t j = 0; j < src.parentCount; ++j) {
            int32_t p = rawPar[src.parentOff + j];
            d->parents.push_back(p >= 0 ? pos[(size_t)p] : -1);
        }
        d->commits[(size_t)k] = c;
    }
    d->text = std::move(b.text);
    auto displayIndex = [&](const Oid& id) -> int32_t {
        auto it = index.find(id);
        return it == index.end() ? -1 : pos[(size_t)it->second];
    };

    // 5. Authors
    d->authors.resize(b.authorRaw.size());
    for (size_t i = 0; i < b.authorRaw.size(); ++i) {
        Author& a = d->authors[i];
        a.name = Widen(b.authorRaw[i].first);
        a.email = Widen(b.authorRaw[i].second);
        a.initials = Initials(a.name, a.email);
        a.color = AvatarColor(a.email.empty() ? a.name : a.email);
    }

    // 6. Stash list
    for (size_t si = 0; si < stashes.size(); ++si) {
        StashEntry e;
        e.commit = displayIndex(stashes[si].id);
        e.name = Widen(stashes[si].name);
        e.message = Widen(stashes[si].message);
        d->stashes.push_back(std::move(e));
    }

    // 7. Refs
    for (size_t p = 0; p < refsOut.size();) {
        size_t e = refsOut.find('\n', p);
        if (e == std::string::npos) e = refsOut.size();
        std::string_view line(refsOut.data() + p, e - p);
        p = e + 1;
        std::string_view f[9];
        int k = 0;
        size_t s = 0;
        while (k < 9) {
            size_t z = line.find('\0', s);
            if (z == std::string_view::npos) { f[k++] = line.substr(s); break; }
            f[k++] = line.substr(s, z - s);
            s = z + 1;
        }
        if (k < 9 || !f[8].empty()) continue;  // malformed or symbolic (e.g. origin/HEAD)
        Ref r;
        std::string_view name = f[0];
        if (name.substr(0, 11) == "refs/heads/") { r.kind = RefKind::Local; r.name = Widen(name.substr(11)); r.branch = r.name; }
        else if (name.substr(0, 13) == "refs/remotes/") {
            r.kind = RefKind::Remote;
            std::string_view rest = name.substr(13);
            size_t slash = rest.find('/');
            if (slash == std::string_view::npos) continue;
            r.remote = Widen(rest.substr(0, slash));
            r.branch = Widen(rest.substr(slash + 1));
            r.name = Widen(rest);
        } else if (name.substr(0, 10) == "refs/tags/") { r.kind = RefKind::Tag; r.name = Widen(name.substr(10)); r.branch = r.name; }
        else continue;
        r.full = Widen(name);
        // Peel annotated tags to the commit they point to.
        Oid target;
        if (!f[3].empty() && f[4] == "commit" && Oid::Parse(f[3].data(), f[3].size(), target)) r.target = target;
        else if (f[2] == "commit" && Oid::Parse(f[1].data(), f[1].size(), target)) r.target = target;
        else continue;
        r.commit = displayIndex(r.target);
        r.upstream = Widen(f[5]);
        std::string_view track = f[6];
        if (track == "gone") r.gone = true;
        if (size_t a = track.find("ahead "); a != std::string_view::npos) r.ahead = (int)ParseI64(track.data() + a + 6, track.size() - a - 6);
        if (size_t bh = track.find("behind "); bh != std::string_view::npos) r.behind = (int)ParseI64(track.data() + bh + 7, track.size() - bh - 7);
        r.head = r.kind == RefKind::Local && f[7] == "*";
        if (r.head) d->headBranch = r.name;
        d->refs.push_back(std::move(r));
    }
    std::stable_sort(d->refs.begin(), d->refs.end(), [](const Ref& a, const Ref& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        if (a.kind == RefKind::Remote && a.remote != b.remote) return CompareNatural(a.remote, b.remote) < 0;
        return CompareNatural(a.name, b.name) < 0;
    });
    d->detached = headValid && d->headBranch.empty();
    d->headCommit = headValid ? displayIndex(d->headOid) : -1;

    for (size_t p = 0; p < remotesOut.size();) {
        size_t e = remotesOut.find('\n', p);
        if (e == std::string::npos) e = remotesOut.size();
        std::string_view r = TrimSv(std::string_view(remotesOut).substr(p, e - p));
        if (!r.empty()) d->remotes.push_back(Widen(r));
        p = e + 1;
    }

    BuildLabels(*d);
    d->submodules = ReadSubmodules(root);
    d->msBuild = NowMs() - tBuild;
    d->msTotal = NowMs() - t0;
    return d;
}

// ============================================================================ working tree

int WorkStatus::ChangedFiles() const {
    std::unordered_set<std::wstring_view> paths;
    for (const FileChange& f : staged) paths.insert(f.path);
    for (const FileChange& f : unstaged) paths.insert(f.path);
    return (int)paths.size();
}

std::shared_ptr<WorkStatus> LoadWorkStatus(const std::wstring& root, const std::wstring& safeDir) {
    auto ws = std::make_shared<WorkStatus>();
    std::string out;
    int code = RunGit(root, Git(safeDir, L"status --porcelain=v2 -z --untracked-files=all --ignore-submodules=dirty"), &out, nullptr);
    if (code != 0) return ws;
    ws->valid = true;
    const char* p = out.data();
    const char* e = p + out.size();
    auto next = [&](std::string_view& tok) {
        if (p >= e) return false;
        const char* z = (const char*)memchr(p, 0, (size_t)(e - p));
        if (!z) z = e;
        tok = std::string_view(p, (size_t)(z - p));
        p = z < e ? z + 1 : e;
        return true;
    };
    std::string_view t;
    while (next(t)) {
        if (t.size() < 2) continue;
        char kind = t[0];
        if (kind == '?') {
            ws->unstaged.push_back({ L'?', Widen(t.substr(2)), L"" });
            continue;
        }
        int spaces = kind == '1' ? 8 : kind == '2' ? 9 : kind == 'u' ? 10 : -1;
        if (spaces < 0) continue;
        size_t at = 0;
        int sp = 0;
        while (at < t.size() && sp < spaces) { if (t[at] == ' ') ++sp; ++at; }
        if (sp < spaces) continue;
        std::wstring path = Widen(t.substr(at));
        std::wstring orig;
        if (kind == '2') {
            std::string_view o;
            if (next(o)) orig = Widen(o);
        }
        char X = t.size() > 2 ? t[2] : '.';
        char Y = t.size() > 3 ? t[3] : '.';
        if (kind == 'u') { ws->unstaged.push_back({ L'U', path, L"" }); continue; }
        if (X != '.') ws->staged.push_back({ (wchar_t)X, path, (X == 'R' || X == 'C') ? orig : std::wstring() });
        if (Y != '.') ws->unstaged.push_back({ (wchar_t)Y, path, L"" });
    }
    // WIP counters (a path changed both in the index and the worktree counts once)
    std::unordered_map<std::wstring, int> kinds;  // 0 modified, 1 added, 2 deleted
    auto classify = [&](const FileChange& f) {
        int k = (f.status == L'D') ? 2 : (f.status == L'A' || f.status == L'?') ? 1 : 0;
        auto [it, ins] = kinds.try_emplace(f.path, k);
        if (!ins) it->second = std::max(it->second, k);
    };
    for (auto& f : ws->staged) classify(f);
    for (auto& f : ws->unstaged) classify(f);
    for (auto& [_, k] : kinds) (k == 2 ? ws->deleted : k == 1 ? ws->added : ws->modified)++;
    return ws;
}

// ============================================================================ commit details

std::shared_ptr<CommitDetails> LoadCommitDetails(const std::wstring& root, const std::wstring& safeDir, const Oid& id) {
    auto r = std::make_shared<CommitDetails>();
    r->id = id;
    const double t0 = NowMs();
    std::string out, err;
    int code = RunGit(root,
                      Git(safeDir, L"show --no-color -z --raw --numstat -M --diff-merges=first-parent "
                                   L"--format=%H%x00%P%x00%an%x00%ae%x00%at%x00%cn%x00%ce%x00%ct%x00%B%x00%x1f%x1f%x1f " +
                                       id.Hex() + L" --"),
                      &out, &err);
    if (code != 0) {
        r->error = FirstErrorLine(err);
        return r;
    }
    const char* p = out.data();
    const char* e = p + out.size();
    std::string_view f[8];
    for (int k = 0; k < 8; ++k) {
        const char* z = (const char*)memchr(p, 0, (size_t)(e - p));
        if (!z) { r->error = L"Unexpected git output."; return r; }
        f[k] = std::string_view(p, (size_t)(z - p));
        p = z + 1;
    }
    const char* marker = nullptr;
    for (const char* q = p; q < e;) {
        const char* z = (const char*)memchr(q, 0, (size_t)(e - q));
        if (!z || z + 4 > e) break;
        if (z[1] == 0x1f && z[2] == 0x1f && z[3] == 0x1f) { marker = z; break; }
        q = z + 1;
    }
    std::string_view msg(p, (size_t)((marker ? marker : e) - p));

    uint16_t pc = 0;
    ParseOidList(f[1].data(), f[1].size(), r->parents, pc);
    r->author = Widen(f[2]);
    r->authorEmail = Widen(f[3]);
    r->authorTime = ParseI64(f[4].data(), f[4].size());
    r->committer = Widen(f[5]);
    r->committerEmail = Widen(f[6]);
    r->commitTime = ParseI64(f[7].data(), f[7].size());

    // Summary = first paragraph (lines joined), body = the rest.
    std::string_view m = TrimSv(msg);
    size_t para = m.find("\n\n");
    std::string summary(m.substr(0, para));
    for (auto& c : summary) if (c == '\n' || c == '\r') c = ' ';
    r->summary = Widen(TrimSv(summary));
    if (para != std::string_view::npos) r->body = Widen(Reflow(TrimSv(m.substr(para + 2))));

    if (marker) {
        const char* q = marker + 4;
        while (q < e && (*q == 0 || *q == '\n' || *q == '\r')) ++q;
        auto next = [&](std::string_view& tok) {
            if (q >= e) return false;
            const char* z = (const char*)memchr(q, 0, (size_t)(e - q));
            if (!z) z = e;
            tok = std::string_view(q, (size_t)(z - q));
            q = z < e ? z + 1 : e;
            return true;
        };
        // ":<modes> <oids> <status>" then the path (two for renames and copies), and the numstat records
        // "<added>\t<deleted>\t<path>" ("-" for binary files; an empty path is followed by old and new).
        std::unordered_map<std::string_view, std::pair<int32_t, int32_t>> counts;
        std::vector<std::string_view> rawPaths;  // as git wrote them (the keys of `counts`)
        std::string_view tok, a, b;
        while (next(tok)) {
            while (!tok.empty() && (tok.front() == '\n' || tok.front() == '\r')) tok.remove_prefix(1);
            if (tok.empty()) continue;
            if (tok.front() == ':') {
                const size_t sp = tok.find_last_of(' ');
                const std::string_view st = sp == std::string_view::npos ? tok : tok.substr(sp + 1);
                if (st.empty() || !next(a)) break;
                FileChange fc;
                fc.status = (wchar_t)st[0];
                if (st[0] == 'R' || st[0] == 'C') {
                    if (!next(b)) break;
                    fc.oldPath = Widen(a);
                    fc.path = Widen(b);
                    rawPaths.push_back(b);
                } else {
                    fc.path = Widen(a);
                    rawPaths.push_back(a);
                }
                r->files.push_back(std::move(fc));
            } else if (const size_t t1 = tok.find('\t'); t1 != std::string_view::npos) {
                const size_t t2 = tok.find('\t', t1 + 1);
                if (t2 == std::string_view::npos) continue;
                std::string_view path = tok.substr(t2 + 1);
                if (path.empty()) {  // a rename: old and new path follow
                    if (!next(a) || !next(b)) break;
                    path = b;
                }
                const bool binary = tok[0] == '-';
                counts[path] = { binary ? -1 : (int32_t)ParseI64(tok.data(), t1),
                                 binary ? -1 : (int32_t)ParseI64(tok.data() + t1 + 1, t2 - t1 - 1) };
            }
        }
        for (size_t i = 0; i < r->files.size(); ++i) {
            FileChange& fc = r->files[i];
            const auto it = counts.find(rawPaths[i]);
            if (it == counts.end()) continue;
            fc.added = it->second.first;
            fc.deleted = it->second.second;
            if (fc.added > 0) r->added += fc.added;
            if (fc.deleted > 0) r->deleted += fc.deleted;
        }
    }
    r->ok = true;
    r->ms = NowMs() - t0;
    return r;
}

std::vector<std::pair<Oid, LineStat>> LoadLineStats(const std::wstring& root, const std::wstring& safeDir, const std::vector<Oid>& ids) {
    std::vector<std::pair<Oid, LineStat>> res;
    if (ids.empty()) return res;
    std::wstring args = L"show --no-color --shortstat --format=%x00%H";
    for (const Oid& id : ids) args += L" " + id.Hex();
    std::string out;
    RunGit(root, Git(safeDir, args + L" --"), &out, nullptr);
    // Each commit: NUL, its hash, then " N files changed, X insertions(+), Y deletions(-)" when it changes anything.
    auto number = [](std::string_view s, std::string_view word) {
        const size_t k = s.find(word);
        if (k == std::string_view::npos) return 0;
        size_t b = k;
        while (b > 0 && s[b - 1] == ' ') --b;
        size_t a = b;
        while (a > 0 && s[a - 1] >= '0' && s[a - 1] <= '9') --a;
        return (int)ParseI64(s.data() + a, b - a);
    };
    for (size_t p = out.find('\0'); p != std::string::npos;) {
        const size_t e = out.find('\0', p + 1);
        const std::string_view rec(out.data() + p + 1, (e == std::string::npos ? out.size() : e) - p - 1);
        p = e;
        const size_t nl = rec.find('\n');
        const std::string_view sha = TrimSv(rec.substr(0, nl));
        Oid id;
        if (!Oid::Parse(sha.data(), sha.size(), id)) continue;
        const std::string_view rest = nl == std::string_view::npos ? std::string_view() : rec.substr(nl);
        res.push_back({ id, LineStat{ number(rest, " insertion"), number(rest, " deletion") } });
    }
    return res;
}

// ============================================================================ file diff

namespace {

constexpr uint32_t kMaxShownChars = 10000;   // characters shown per line (minified files can be huge)
constexpr size_t kMaxLineBytes = 1u << 20;   // bytes of a line kept for finding its change
constexpr size_t kMaxDiffBytes = 96u << 20;  // larger diffs are cut at a line boundary

bool StartsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }

// Appends UTF-8 text to the wide-character arena without temporary strings.
uint32_t AppendWide(std::wstring& arena, std::string_view s, uint32_t& outLen) {
    const uint32_t off = (uint32_t)arena.size();
    WidenAppend(s, arena);
    outLen = (uint32_t)(arena.size() - off);
    return off;
}

void PushLine(FileDiff& d, DiffLine::Kind k, std::string_view text, int oldNo, int newNo, size_t rawOff = 0) {
    DiffLine L;
    L.kind = k;
    L.oldNo = oldNo;
    L.newNo = newNo;
    L.hunk = (int32_t)d.hunks.size() - 1;
    L.rawOff = (uint32_t)rawOff;
    if (text.size() > kMaxLineBytes) text = text.substr(0, kMaxLineBytes);
    L.off = AppendWide(d.text, text, L.len);
    d.lines.push_back(L);
}

// "12,5" or "12" (a count of 1): a line range of a hunk header, after its - or +.
void ParseRange(std::string_view s, int32_t& start, int32_t& count) {
    const size_t comma = s.find(',');
    start = (int32_t)ParseI64(s.data(), comma == std::string_view::npos ? s.size() : comma);
    count = comma == std::string_view::npos ? 1 : (int32_t)ParseI64(s.data() + comma + 1, s.size() - comma - 1);
}

void ParseDiff(FileDiff& d) {
    const std::string& s = d.raw;
    size_t p = 0, fileStart = 0;
    int oldNo = 0, newNo = 0;
    bool inHunk = false, sawFile = false;
    std::string oldMode;
    while (p < s.size()) {
        size_t e = s.find('\n', p);
        if (e == std::string::npos) e = s.size();
        const size_t start = p;
        const std::string_view line(s.data() + p, e - p);
        p = e + 1;
        std::string_view text = line;
        if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
        if (StartsWith(line, "diff --git ") || StartsWith(line, "diff --cc ") || StartsWith(line, "diff --combined ")) {
            if (sawFile) {  // a second file in the same output
                PushLine(d, DiffLine::Info, text, -1, -1);
                d.multiFile = true;
            }
            sawFile = true;
            inHunk = false;
            fileStart = start;
            d.combined = !StartsWith(line, "diff --git ");
            continue;
        }
        if (StartsWith(line, "@@")) {
            if (d.hunks.empty()) {
                d.headerOff = fileStart;
                d.headerLen = start - fileStart;
            }
            // "@@ -a,b +c,d @@ context"   or, for conflicts,   "@@@ -a,b -c,d +e,f @@@ context"
            DiffHunk h;
            h.line = (int32_t)d.lines.size();
            bool haveOld = false;
            size_t q = 0;
            while (q < text.size() && text[q] == '@') ++q;
            while (q < text.size()) {
                while (q < text.size() && text[q] == ' ') ++q;
                if (q >= text.size() || text[q] == '@') break;
                const size_t t0 = q;
                while (q < text.size() && text[q] != ' ') ++q;
                const std::string_view tok = text.substr(t0, q - t0);
                if (tok.size() > 1 && tok[0] == '-' && !haveOld) {
                    ParseRange(tok.substr(1), h.oldStart, h.oldCount);
                    haveOld = true;
                } else if (tok.size() > 1 && tok[0] == '+') {
                    ParseRange(tok.substr(1), h.newStart, h.newCount);
                }
            }
            d.hunks.push_back(h);
            PushLine(d, DiffLine::Hunk, text, -1, -1, start);
            oldNo = h.oldStart;
            newNo = h.newStart;
            inHunk = true;
            continue;
        }
        if (!inHunk) {
            if (StartsWith(line, "Binary files ") || StartsWith(line, "GIT binary patch")) d.binary = true;
            else if (StartsWith(line, "new file mode")) d.newFile = true;
            else if (StartsWith(line, "deleted file mode")) d.deletedFile = true;
            else if (StartsWith(line, "rename from ")) d.oldPath = Widen(text.substr(12));
            else if (StartsWith(line, "old mode ")) oldMode = std::string(text.substr(9));
            else if (StartsWith(line, "new mode ")) d.modeChange = Widen(oldMode) + L" → " + Widen(text.substr(9));
            continue;
        }
        const size_t pre = d.combined ? 2 : 1;
        const char c0 = line.empty() ? ' ' : line[0];
        const char c1 = d.combined && line.size() > 1 ? line[1] : ' ';
        const std::string_view body = text.size() >= pre ? text.substr(pre) : std::string_view();
        if (c0 == '\\') {  // "\ No newline at end of file": about the line before
            if (!d.lines.empty() && d.lines.back().kind != DiffLine::Hunk) d.lines.back().noEol = true;
        } else if (c0 == '+' || c1 == '+') {
            PushLine(d, DiffLine::Added, body, -1, newNo++, start);
            ++d.added;
            ++d.hunks.back().added;
        } else if (c0 == '-' || c1 == '-') {
            PushLine(d, DiffLine::Deleted, body, oldNo++, -1, start);
            ++d.deleted;
            ++d.hunks.back().deleted;
        } else if (c0 == ' ' || line.empty()) {
            PushLine(d, DiffLine::Context, body, oldNo++, newNo++, start);
        } else {
            inHunk = false;  // not part of a hunk: back to header parsing
        }
    }
    d.maxLineNo = std::max(oldNo, newNo);
}

// An untracked file has no diff in git: show it as a new file with every line added.
void SynthesizeNewFile(FileDiff& d, const std::wstring& file) {
    d.synthesized = true;
    d.newFile = true;
    std::string data;
    bool cut = false;
    if (!ReadWholeFile(file, data, kMaxDiffBytes, &cut)) {
        d.error = L"The file can't be read.";
        return;
    }
    d.truncated = cut;
    if (memchr(data.data(), 0, std::min<size_t>(data.size(), 8000))) {
        d.binary = true;
        return;
    }
    if (data.empty()) return;  // a new empty file: no lines
    size_t lines = 0;
    for (char c : data) lines += c == '\n';
    if (!data.empty() && data.back() != '\n') ++lines;
    d.hunks.push_back({});
    d.hunks[0].line = 0;
    d.hunks[0].newStart = 1;
    d.hunks[0].newCount = (int32_t)lines;
    const std::string header = "@@ -0,0 +1," + std::to_string(lines) + " @@";
    PushLine(d, DiffLine::Hunk, header, -1, -1);
    int no = 1;
    for (size_t p = 0; p < data.size();) {
        size_t e = data.find('\n', p);
        if (e == std::string::npos) e = data.size();
        std::string_view line(data.data() + p, e - p);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        PushLine(d, DiffLine::Added, line, -1, no++);
        p = e + 1;
    }
    if (data.back() != '\n' && !d.truncated) d.lines.back().noEol = true;
    d.added = (int)lines;
    d.hunks[0].added = (int)lines;
    d.maxLineNo = no;
}

bool IsWordChar(wchar_t c) { return std::iswalnum((std::wint_t)c) || c == L'_'; }

// Marks the changed part of a modified line: common prefix and suffix are cut, then widened to whole words.
void HighlightPair(FileDiff& d, DiffLine& a, DiffLine& b) {
    const wchar_t* x = d.text.data() + a.off;
    const wchar_t* y = d.text.data() + b.off;
    size_t pre = 0;
    while (pre < a.len && pre < b.len && x[pre] == y[pre]) ++pre;
    size_t suf = 0;
    while (suf < a.len - pre && suf < b.len - pre && x[a.len - 1 - suf] == y[b.len - 1 - suf]) ++suf;
    if (pre + suf == 0) return;  // nothing in common: the whole line changed
    while (pre > 0 && pre < a.len && IsWordChar(x[pre - 1]) && IsWordChar(x[pre])) --pre;
    while (suf > 0 && suf < a.len && IsWordChar(x[a.len - suf]) && IsWordChar(x[a.len - suf - 1])) --suf;
    if (pre + suf == 0) return;  // only parts of words in common: marking the whole line says nothing
    a.hlStart = b.hlStart = (uint32_t)pre;
    a.hlLen = (uint32_t)(a.len - std::min<size_t>(a.len, pre + suf));
    b.hlLen = (uint32_t)(b.len - std::min<size_t>(b.len, pre + suf));
}

// Minified code can have lines megabytes long: each line keeps at most kMaxShownChars, taken around its
// change when the change lies further in, with an ellipsis where text was left out. Then measures the
// widest line in columns for horizontal scrolling (a tab advances up to 4, wide characters take 2).
void FinishLines(FileDiff& d) {
    bool cut = false;
    for (const DiffLine& L : d.lines) cut |= L.len > kMaxShownChars;
    if (cut) {
        std::wstring arena;
        size_t total = 0;
        for (const DiffLine& L : d.lines) total += std::min(L.len, kMaxShownChars) + 2;
        arena.reserve(total);
        for (DiffLine& L : d.lines) {
            uint32_t start = 0, n = L.len;
            if (L.len > kMaxShownChars) {
                // Paired lines share hlStart (also when one side only lost or gained text), so both sides
                // of the split view start at the same column.
                if (L.hlStart + std::min(L.hlLen, 400u) > kMaxShownChars - 200) start = L.hlStart > 80 ? L.hlStart - 80 : 0;
                n = std::min(L.len - start, kMaxShownChars);
            }
            const uint32_t off = (uint32_t)arena.size(), lead = start > 0 ? 1 : 0;
            if (lead) arena.push_back(L'…');
            arena.append(d.text, L.off + start, n);
            if (start + n < L.len) arena.push_back(L'…');
            if (L.hlLen) {
                const uint32_t hs = std::max(L.hlStart, start), he = std::min(L.hlStart + L.hlLen, start + n);
                L.hlStart = he > hs ? hs - start + lead : 0;
                L.hlLen = he > hs ? he - hs : 0;
            }
            L.off = off;
            L.len = (uint32_t)arena.size() - off;
        }
        d.text = std::move(arena);
    }
    for (const DiffLine& L : d.lines) {
        size_t cols = L.len;
        const wchar_t* w = d.text.data() + L.off;
        for (uint32_t i = 0; i < L.len; ++i) cols += w[i] == L'\t' ? 3 : (w[i] >= 0x1100 ? 1 : 0);
        d.maxLen = std::max(d.maxLen, cols);
    }
}

void ComputeIntraLine(FileDiff& d) {
    const size_t n = d.lines.size();
    for (size_t i = 0; i < n;) {
        if (d.lines[i].kind != DiffLine::Deleted) { ++i; continue; }
        size_t j = i;
        while (j < n && d.lines[j].kind == DiffLine::Deleted) ++j;
        size_t k = j;
        while (k < n && d.lines[k].kind == DiffLine::Added) ++k;
        for (size_t m = 0; m < std::min(j - i, k - j); ++m) HighlightPair(d, d.lines[i + m], d.lines[j + m]);
        i = std::max(j, k);
    }
}

// Side-by-side rows: deletions on the left paired with the additions that replace them on the right.
void BuildSplit(FileDiff& d) {
    const size_t n = d.lines.size();
    d.split.reserve(n);
    for (size_t i = 0; i < n;) {
        const DiffLine::Kind k = d.lines[i].kind;
        if (k != DiffLine::Deleted && k != DiffLine::Added) {
            if (k == DiffLine::Hunk && d.lines[i].hunk >= 0) d.hunks[(size_t)d.lines[i].hunk].splitRow = (int32_t)d.split.size();
            d.split.push_back({ (int32_t)i, (int32_t)i });
            ++i;
            continue;
        }
        size_t j = i;
        while (j < n && d.lines[j].kind == DiffLine::Deleted) ++j;
        size_t e = j;
        while (e < n && d.lines[e].kind == DiffLine::Added) ++e;
        const size_t nd = j - i, na = e - j;
        for (size_t r = 0; r < std::max(nd, na); ++r)
            d.split.push_back({ r < nd ? (int32_t)(i + r) : -1, r < na ? (int32_t)(j + r) : -1 });
        i = e;
    }
}

}  // namespace

std::shared_ptr<FileDiff> LoadFileDiff(const std::wstring& root, const std::wstring& safeDir, const DiffRequest& req) {
    auto d = std::make_shared<FileDiff>();
    const double t0 = NowMs();
    if (req.section == 1 && req.status == L'?') {
        SynthesizeNewFile(*d, PathJoin(root, req.path));
    } else {
        std::wstring paths = L" -- " + QuoteArg(req.path);
        if (!req.oldPath.empty() && req.oldPath != req.path) paths += L" " + QuoteArg(req.oldPath);  // lets git pair the rename
        // Fixed a/ b/ prefixes and three lines of context whatever diff.noprefix and diff.context say, so that
        // changes can be fed back to git apply (which needs context to place a hunk).
        const std::wstring opts = L"--no-color --no-ext-diff --no-textconv --submodule=short --src-prefix=a/ --dst-prefix=b/ --unified=3";
        std::wstring args;
        if (req.section == 0) args = L"show " + opts + L" --format= -M --diff-merges=first-parent " + req.commit.Hex() + paths;
        else if (req.section == 2) args = L"diff --cached " + opts + L" -M" + paths;
        else args = L"diff " + opts + paths;
        std::string err;
        bool cut = false;
        const int code = RunGit(root, Git(safeDir, L"--literal-pathspecs " + args), nullptr, &err, [&](const char* p, size_t n) {
            if (cut) return;
            if (d->raw.size() + n > kMaxDiffBytes) {
                n = kMaxDiffBytes - d->raw.size();
                cut = true;
            }
            d->raw.append(p, n);
        });
        if (code != 0 && d->raw.empty()) {
            d->error = FirstErrorLine(err);
            d->ms = NowMs() - t0;
            return d;
        }
        if (cut) {
            const size_t nl = d->raw.rfind('\n');
            d->raw.resize(nl == std::string::npos ? 0 : nl + 1);
            d->truncated = true;
        }
        ParseDiff(*d);
    }
    ComputeIntraLine(*d);
    FinishLines(*d);
    BuildSplit(*d);
    d->ok = d->error.empty();
    d->ms = NowMs() - t0;
    return d;
}

// Rows of the split view hold a deleted line, an added line, or both; the first of the two grows with
// the row, so a binary search finds the row of a unified line.
int SplitRowOfLine(const FileDiff& d, int line) {
    auto key = [](const SplitRow& r) { return r.left >= 0 ? r.left : r.right; };
    auto it = std::upper_bound(d.split.begin(), d.split.end(), line, [&](int l, const SplitRow& r) { return l < key(r); });
    return std::max(0, (int)(it - d.split.begin()) - 1);
}

bool CanApplyBack(const FileDiff& d) {
    return d.ok && !d.synthesized && !d.combined && !d.truncated && !d.multiFile && !d.binary && d.oldPath.empty() && d.headerLen &&
           !d.hunks.empty();
}

namespace {
// A diff line as git printed it, after its one-character prefix and without the line break.
std::string_view RawBody(const FileDiff& d, const DiffLine& L) {
    size_t e = d.raw.find('\n', L.rawOff);
    if (e == std::string::npos) e = d.raw.size();
    const size_t b = std::min<size_t>(L.rawOff + 1, e);  // an empty context line may have no prefix
    return std::string_view(d.raw.data() + b, e - b);
}

// "--- a/path" from "+++ b/path" and the other way round (also when git quoted the path).
std::string OtherSide(std::string_view rest, char side) {
    std::string s(rest);
    const size_t q = !s.empty() && s[0] == '"' ? 1 : 0;
    if (s.size() > q + 1 && s[q + 1] == '/') s[q] = side;
    return s;
}
}  // namespace

std::string BuildPatch(const FileDiff& d, bool reverse, const std::function<bool(int32_t)>& chosen) {
    if (!CanApplyBack(d)) return {};
    const int32_t n = (int32_t)d.lines.size();
    auto changed = [&](int32_t i) {
        const DiffLine::Kind k = d.lines[(size_t)i].kind;
        return k == DiffLine::Added || k == DiffLine::Deleted;
    };
    bool any = false, all = true;
    for (int32_t i = 0; i < n; ++i) {
        if (!changed(i)) continue;
        if (chosen(i)) any = true;
        else all = false;
    }
    if (!any) return {};

    // File header. Applying part of an added file backwards, or part of a deleted file forwards, leaves a file
    // with content: then the patch describes a modification instead of a deletion.
    std::string out;
    const std::string_view header(d.raw.data() + d.headerOff, d.headerLen);
    if (all || !(reverse ? d.newFile : d.deletedFile)) {
        out.assign(header);
    } else {
        std::string_view minus, plus;
        for (size_t p = 0; p < header.size();) {
            size_t e = header.find('\n', p);
            if (e == std::string_view::npos) e = header.size();
            const std::string_view line = header.substr(p, e - p);
            p = e + 1;
            if (StartsWith(line, "diff --git ")) out.append(line).push_back('\n');
            else if (StartsWith(line, "--- ")) minus = line.substr(4);
            else if (StartsWith(line, "+++ ")) plus = line.substr(4);
            // the "new file mode", "deleted file mode" and "index" lines are left out
        }
        out += "--- " + (d.newFile ? OtherSide(plus, 'a') : std::string(minus)) + "\n";
        out += "+++ " + (d.newFile ? std::string(plus) : OtherSide(minus, 'b')) + "\n";
    }

    // Hunks. Lines that are not chosen stay as they are: forward (staging), a deletion becomes context and an
    // addition is left out; backwards, the other way round. Hunks without chosen lines are skipped, and the
    // start lines of the following ones move by what the skipped and reduced hunks no longer change (git apply
    // looks for each hunk from there, which matters when the same lines appear more than once).
    int64_t delta = 0;
    for (size_t hi = 0; hi < d.hunks.size(); ++hi) {
        const DiffHunk& h = d.hunks[hi];
        const int32_t first = h.line + 1, end = hi + 1 < d.hunks.size() ? d.hunks[hi + 1].line : n;
        bool use = false;
        for (int32_t i = first; i < end && !use; ++i) use = changed(i) && chosen(i);
        if (!use) {
            delta += reverse ? h.newCount - h.oldCount : h.oldCount - h.newCount;
            continue;
        }
        std::string body;
        int32_t oc = 0, nc = 0;
        auto emit = [&](char prefix, int32_t i, bool noEol) {
            body.push_back(prefix);
            body.append(RawBody(d, d.lines[(size_t)i]));
            body.push_back('\n');
            if (noEol) body += "\\ No newline at end of file\n";
            if (prefix != '+') ++oc;
            if (prefix != '-') ++nc;
        };
        for (int32_t i = first; i < end;) {
            if (!changed(i)) {
                emit(' ', i, d.lines[(size_t)i].noEol);
                ++i;
                continue;
            }
            // A block: deleted lines [i, j) replaced by added lines [j, e). What remains of it is taken slot by
            // slot (a deleted line, then the added line in its place): forward the kept deletions and the chosen
            // additions, backwards the chosen deletions and the kept additions. Only the last remaining line of a
            // block that ends the file may lack its line break.
            int32_t j = i;
            while (j < end && d.lines[(size_t)j].kind == DiffLine::Deleted) ++j;
            int32_t e = j;
            while (e < end && d.lines[(size_t)e].kind == DiffLine::Added) ++e;
            const int32_t m = j - i, k = e - j;
            int32_t lastLeft = -1;
            for (int32_t s = 0; s < std::max(m, k); ++s) {
                if (s < m && chosen(i + s) == reverse) lastLeft = i + s;
                if (s < k && chosen(j + s) != reverse) lastLeft = j + s;
            }
            auto last = [&](int32_t x) { return e == end && x == lastLeft; };
            for (int32_t s = 0; s < std::max(m, k); ++s) {
                if (s < m) {
                    const int32_t x = i + s;
                    const bool noEol = d.lines[(size_t)x].noEol;
                    if (reverse) {
                        if (chosen(x)) emit('-', x, noEol && last(x));  // brought back
                    } else if (chosen(x)) {
                        emit('-', x, noEol);  // removed
                    } else if (noEol && !last(x)) {
                        emit('-', x, true);  // kept, but a line now follows it: it gains a line break
                        emit('+', x, false);
                    } else {
                        emit(' ', x, noEol);  // kept
                    }
                }
                if (s < k) {
                    const int32_t x = j + s;
                    const bool noEol = d.lines[(size_t)x].noEol;
                    if (!reverse) {
                        if (chosen(x)) emit('+', x, noEol && last(x));  // added
                    } else if (chosen(x)) {
                        emit('+', x, noEol);  // taken away
                    } else if (noEol && !last(x)) {
                        emit('-', x, false);  // kept, but a line now follows it: it gains a line break
                        emit('+', x, true);
                    } else {
                        emit(' ', x, noEol);  // kept
                    }
                }
            }
            i = e;
        }
        const int64_t oldStart = reverse ? h.oldStart + delta : h.oldStart;
        const int64_t newStart = reverse ? h.newStart : h.newStart + delta;
        out += "@@ -" + std::to_string(oldStart) + "," + std::to_string(oc) + " +" + std::to_string(newStart) + "," + std::to_string(nc) +
               " @@\n";
        out += body;
        delta += reverse ? oc - h.oldCount : nc - h.newCount;
    }
    return out;
}
