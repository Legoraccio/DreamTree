// POSIX implementation of the operating system services used by the shared core (platform.h): macOS, with
// FSEvents for the file watcher; it also builds on other POSIX systems (without live refresh).

#include "platform.h"
#include "text.h"
#include "watcher.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <dirent.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wctype.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <Security/Security.h>
#include <curl/curl.h>
#include <dispatch/dispatch.h>
#include <xlocale.h>
#endif

extern char** environ;

namespace {

// File system paths are UTF-8.
std::string P(const std::wstring& w) { return Narrow(w); }

// A UTF-8 locale for case mapping, without touching the process locale (a GUI app usually runs in "C").
locale_t CaseLocale() {
    static locale_t loc = [] {
        for (const char* name : { "en_US.UTF-8", "C.UTF-8", "UTF-8" })
            if (locale_t l = newlocale(LC_CTYPE_MASK, name, (locale_t)0)) return l;
        return (locale_t)0;
    }();
    return loc;
}

wchar_t Lower(wchar_t c) {
    const locale_t l = CaseLocale();
    return l ? (wchar_t)towlower_l((wint_t)c, l) : (wchar_t)towlower((wint_t)c);
}

wchar_t Upper(wchar_t c) {
    const locale_t l = CaseLocale();
    return l ? (wchar_t)towupper_l((wint_t)c, l) : (wchar_t)towupper((wint_t)c);
}

bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

uint64_t StampOf(const struct stat& st) {
#ifdef __APPLE__
    const uint64_t ns = (uint64_t)st.st_mtimespec.tv_sec * 1000000000ULL + (uint64_t)st.st_mtimespec.tv_nsec;
#else
    const uint64_t ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ULL + (uint64_t)st.st_mtim.tv_nsec;
#endif
    return ns ^ ((uint64_t)st.st_size << 7);
}

bool WriteAll(int fd, const std::string& bytes) {
    size_t off = 0;
    while (off < bytes.size()) {
        const ssize_t n = write(fd, bytes.data() + off, bytes.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

void MakeDirs(const std::string& dir) {
    for (size_t p = 1; p <= dir.size(); ++p) {
        if (p == dir.size() || dir[p] == '/') mkdir(dir.substr(0, p).c_str(), 0755);
    }
}

#ifdef __APPLE__
std::wstring FromCF(CFStringRef s) {
    if (!s) return L"";
    const CFIndex len = CFStringGetLength(s);
    const CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string buf((size_t)max, '\0');
    if (!CFStringGetCString(s, buf.data(), max, kCFStringEncodingUTF8)) return L"";
    buf.resize(strlen(buf.c_str()));
    return Widen(buf);
}
#endif

}  // namespace

// ============================================================================ text

std::wstring ToLower(std::wstring s) {
    for (wchar_t& c : s) c = Lower(c);
    return s;
}

std::wstring ToUpper(std::wstring s) {
    for (wchar_t& c : s) c = Upper(c);
    return s;
}

bool ContainsI(std::wstring_view hay, std::wstring_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    return ToLower(std::wstring(hay)).find(ToLower(std::wstring(needle))) != std::wstring::npos;
}

// Digit runs compare by value ("v2" < "v10"), everything else by lower-case character.
int CompareNatural(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (IsDigit(a[i]) && IsDigit(b[j])) {
            size_t si = i, sj = j;
            while (si < a.size() && a[si] == L'0') ++si;
            while (sj < b.size() && b[sj] == L'0') ++sj;
            size_t ei = si, ej = sj;
            while (ei < a.size() && IsDigit(a[ei])) ++ei;
            while (ej < b.size() && IsDigit(b[ej])) ++ej;
            if (ei - si != ej - sj) return ei - si < ej - sj ? -1 : 1;
            for (size_t k = 0; k < ei - si; ++k)
                if (a[si + k] != b[sj + k]) return a[si + k] < b[sj + k] ? -1 : 1;
            i = ei;
            j = ej;
            continue;
        }
        const wchar_t la = Lower(a[i]), lb = Lower(b[j]);
        if (la != lb) return la < lb ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

// ============================================================================ time

bool ToLocalTime(int64_t t, LocalTime& out) {
    const time_t tt = (time_t)t;
    struct tm tmv;
    if (!localtime_r(&tt, &tmv)) return false;
    out.year = tmv.tm_year + 1900;
    out.month = tmv.tm_mon + 1;
    out.day = tmv.tm_mday;
    out.hour = tmv.tm_hour;
    out.minute = tmv.tm_min;
    return true;
}

std::wstring FormatClock(int64_t t) {
#ifdef __APPLE__
    // 12 or 24 hours as chosen in the system settings
    CFLocaleRef loc = CFLocaleCopyCurrent();
    CFDateFormatterRef f = CFDateFormatterCreate(kCFAllocatorDefault, loc, kCFDateFormatterNoStyle, kCFDateFormatterShortStyle);
    CFStringRef s = CFDateFormatterCreateStringWithAbsoluteTime(kCFAllocatorDefault, f, (CFAbsoluteTime)t - kCFAbsoluteTimeIntervalSince1970);
    std::wstring r = FromCF(s);
    if (s) CFRelease(s);
    if (f) CFRelease(f);
    if (loc) CFRelease(loc);
    return r;
#else
    LocalTime lt;
    if (!ToLocalTime(t, lt)) return L"";
    wchar_t buf[16];
    swprintf(buf, 16, L"%02d:%02d", lt.hour, lt.minute);
    return buf;
#endif
}

// ============================================================================ files

bool DirExists(const std::wstring& p) {
    struct stat st;
    return stat(P(p).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool FileExists(const std::wstring& p) {
    struct stat st;
    return stat(P(p).c_str(), &st) == 0;
}

bool StatFile(const std::wstring& path, int64_t* mtime, uint64_t* stamp) {
    struct stat st;
    if (stat(P(path).c_str(), &st) != 0) return false;
    if (mtime) *mtime = (int64_t)st.st_mtime;
    if (stamp) *stamp = StampOf(st);
    return true;
}

void ListDir(const std::wstring& dir, const std::function<void(std::wstring_view, uint64_t, bool)>& f) {
    const std::string d = P(dir);
    DIR* h = opendir(d.c_str());
    if (!h) return;
    while (dirent* e = readdir(h)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st;
        const std::string full = d + "/" + e->d_name;
        if (lstat(full.c_str(), &st) != 0) continue;
        f(Widen(e->d_name), StampOf(st), S_ISDIR(st.st_mode));
    }
    closedir(h);
}

bool ReadWholeFile(const std::wstring& path, std::string& out, size_t maxBytes, bool* cut) {
    out.clear();
    if (cut) *cut = false;
    const int fd = open(P(path).c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::vector<char> buf(1 << 16);
    for (;;) {
        const ssize_t n = read(fd, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        if (out.size() + (size_t)n >= maxBytes) {
            out.append(buf.data(), maxBytes - out.size());
            if (cut) *cut = true;
            break;
        }
        out.append(buf.data(), (size_t)n);
    }
    close(fd);
    return true;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& bytes) {
    const std::string p = P(path), tmp = p + ".tmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = WriteAll(fd, bytes);
    if (close(fd) != 0) ok = false;
    if (!ok) {
        unlink(tmp.c_str());
        return false;
    }
    return rename(tmp.c_str(), p.c_str()) == 0;
}

std::wstring WriteTempFile(const std::string& bytes) {
    const char* t = getenv("TMPDIR");
    std::string dir = t && *t ? t : "/tmp";
    if (dir.back() != '/') dir += '/';
    const std::string tmpl = dir + "mangrove-XXXXXX";
    std::vector<char> name(tmpl.begin(), tmpl.end());
    name.push_back('\0');
    const int fd = mkstemp(name.data());
    if (fd < 0) return L"";
    bool ok = WriteAll(fd, bytes);
    if (close(fd) != 0) ok = false;
    if (!ok) {
        unlink(name.data());
        return L"";
    }
    return Widen(name.data());
}

void RemoveFile(const std::wstring& path) { unlink(P(path).c_str()); }

static std::string ConfigDir(const char* name, const char* lower) {
    const char* home = getenv("HOME");
    const std::string base = home && *home ? home : "/tmp";
#ifdef __APPLE__
    (void)lower;
    return base + "/Library/Application Support/" + name;
#else
    (void)name;
    const char* xdg = getenv("XDG_CONFIG_HOME");
    return (xdg && *xdg ? std::string(xdg) : base + "/.config") + "/" + lower;
#endif
}

std::wstring AppDataDir() {
    const std::string dir = ConfigDir("Mangrove", "mangrove");
    MakeDirs(dir);
    return Widen(dir);
}

std::wstring OldAppDataDir() { return Widen(ConfigDir("DreamTree", "dreamtree")); }

std::wstring EnvVar(const char* name) {
    const char* v = getenv(name);
    return v ? Widen(v) : std::wstring();
}

// ============================================================================ processes

namespace {

// Apps started from the Finder get a minimal PATH, so the usual places are tried first: Homebrew (Apple
// silicon, then Intel), then Apple's git (it needs the Command Line Tools).
std::wstring FindGit() {
    for (const char* c : { "/opt/homebrew/bin/git", "/usr/local/bin/git", "/usr/bin/git" })
        if (access(c, X_OK) == 0) return Widen(c);
    if (const char* path = getenv("PATH")) {
        std::string s = path;
        for (size_t p = 0; p <= s.size();) {
            size_t e = s.find(':', p);
            if (e == std::string::npos) e = s.size();
            const std::string candidate = s.substr(p, e - p) + "/git";
            if (e > p && access(candidate.c_str(), X_OK) == 0) return Widen(candidate);
            p = e + 1;
        }
    }
    return L"/usr/bin/git";
}

// git's environment: no terminal prompts, no merge message editor, English messages; Homebrew's folders on
// PATH so that helpers installed there (credential helpers, git-lfs) are found.
const std::vector<std::string>& GitEnvironment() {
    static const std::vector<std::string> env = [] {
        std::vector<std::string> v;
        std::string path = "/opt/homebrew/bin:/usr/local/bin";
        bool hasPath = false;
        for (char** e = environ; e && *e; ++e) {
            const std::string kv = *e;
            const size_t eq = kv.find('=');
            const std::string key = kv.substr(0, eq);
            if (key == "GIT_TERMINAL_PROMPT" || key == "GIT_MERGE_AUTOEDIT" || key == "LANG" || key == "LC_ALL") continue;
            if (key == "PATH" && eq != std::string::npos) {
                path += ":" + kv.substr(eq + 1);
                hasPath = true;
                continue;
            }
            v.push_back(kv);
        }
        if (!hasPath) path += ":/usr/bin:/bin:/usr/sbin:/sbin";
        v.push_back("PATH=" + path);
        for (const char* o : { "GIT_TERMINAL_PROMPT=0", "GIT_MERGE_AUTOEDIT=no", "LANG=en_US.UTF-8", "LC_ALL=en_US.UTF-8" }) v.push_back(o);
        return v;
    }();
    return env;
}

}  // namespace

std::wstring GitPath(bool) {
    static const std::wstring git = FindGit();
    return git;
}

int RunProcess(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd, std::string* out, std::string* err,
               const Sink& sink, const Sink& errSink, const std::vector<std::string>& env) {
    if (!cwd.empty() && !DirExists(cwd)) {
        if (err) *err = "The folder does not exist.";
        return -1;
    }
    const std::string exe8 = Narrow(exe), cwd8 = Narrow(cwd);
    std::vector<std::string> argv8{ exe8 };
    for (const std::wstring& a : SplitCommandLine(args)) argv8.push_back(Narrow(a));
    std::vector<char*> argv;
    for (std::string& a : argv8) argv.push_back(a.data());
    argv.push_back(nullptr);
    // git's environment; `env` adds variables or replaces the ones of the same name
    auto nameOf = [](const std::string& kv) { return std::string_view(kv).substr(0, kv.find('=')); };
    std::vector<char*> envp;
    for (const std::string& e : GitEnvironment()) {
        bool replaced = false;
        for (const std::string& x : env) replaced |= nameOf(x) == nameOf(e);
        if (!replaced) envp.push_back(const_cast<char*>(e.c_str()));
    }
    for (const std::string& x : env) envp.push_back(const_cast<char*>(x.c_str()));
    envp.push_back(nullptr);

    int outPipe[2] = { -1, -1 }, errPipe[2] = { -1, -1 };
    if (pipe(outPipe) != 0) return -1;
    if (pipe(errPipe) != 0) {
        close(outPipe[0]);
        close(outPipe[1]);
        return -1;
    }
    // Not inherited by processes that other threads start at the same time.
    for (int fd : { outPipe[0], outPipe[1], errPipe[0], errPipe[1] }) fcntl(fd, F_SETFD, FD_CLOEXEC);

    pid_t pid = -1;
    int rc = 0;
#if defined(__APPLE__) || defined(__GLIBC__)
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, outPipe[1], 1);
    posix_spawn_file_actions_adddup2(&fa, errPipe[1], 2);
    if (!cwd8.empty()) posix_spawn_file_actions_addchdir_np(&fa, cwd8.c_str());
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef __APPLE__
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);  // only stdin, stdout and stderr are inherited
#endif
    rc = posix_spawn(&pid, exe8.c_str(), &fa, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
#else
    pid = fork();
    if (pid == 0) {  // child: only async-signal-safe calls until exec
        const int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, 0);
        dup2(outPipe[1], 1);
        dup2(errPipe[1], 2);
        if (!cwd8.empty() && chdir(cwd8.c_str()) != 0) _exit(127);
        execve(exe8.c_str(), argv.data(), envp.data());
        _exit(127);
    }
    rc = pid < 0 ? errno : 0;
#endif
    close(outPipe[1]);
    close(errPipe[1]);
    if (rc != 0) {
        close(outPipe[0]);
        close(errPipe[0]);
        if (err)
            *err = rc == ENOENT ? "Git was not found. Install the Xcode Command Line Tools (xcode-select --install) or Git from Homebrew."
                                : "Could not start git.";
        return -1;
    }

    std::string errBuf;
    std::vector<char> buf(1 << 16);
    struct pollfd fds[2] = { { outPipe[0], POLLIN, 0 }, { errPipe[0], POLLIN, 0 } };
    int live = 2;
    while (live > 0) {
        const int pr = poll(fds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int k = 0; k < 2; ++k) {
            if (fds[k].fd < 0 || !(fds[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const ssize_t n = read(fds[k].fd, buf.data(), buf.size());
            if (n > 0) {
                if (k == 0) {
                    if (sink) sink(buf.data(), (size_t)n);
                    else if (out) out->append(buf.data(), (size_t)n);
                } else {
                    errBuf.append(buf.data(), (size_t)n);
                    if (errSink) errSink(buf.data(), (size_t)n);
                }
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                close(fds[k].fd);
                fds[k].fd = -1;
                --live;
            }
        }
    }
    for (const pollfd& p : fds)
        if (p.fd >= 0) close(p.fd);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (err) *err = std::move(errBuf);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

// ============================================================================ network and secrets

#ifdef __APPLE__

static size_t CurlBody(char* p, size_t size, size_t n, void* response) {
    auto* r = static_cast<HttpResponse*>(response);
    r->body.append(p, size * n);
    return r->body.size() > (4u << 20) ? 0 : size * n;  // stops answers that are far too big
}

static size_t CurlHeader(char* p, size_t size, size_t n, void* response) {
    const std::string_view line(p, size * n);
    const size_t colon = line.find(':');
    if (colon != std::string_view::npos) {
        std::string name(TrimSv(line.substr(0, colon)));
        for (char& c : name) c = (char)std::tolower((unsigned char)c);
        static_cast<HttpResponse*>(response)->headers.emplace_back(std::move(name), std::string(TrimSv(line.substr(colon + 1))));
    }
    return size * n;
}

static HttpResponse CurlRequest(const std::string& url, const std::vector<std::string>& headers, const std::string* form) {
    HttpResponse r;
    if (url.compare(0, 8, "https://") != 0) {
        r.error = "only https:// addresses are supported";
        return r;
    }
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* c = curl_easy_init();
    if (!c) {
        r.error = "could not start the request";
        return r;
    }
    curl_slist* list = nullptr;
    for (const std::string& h : headers) list = curl_slist_append(list, h.c_str());
    if (form) {  // a POST of the form
        list = curl_slist_append(list, "Content-Type: application/x-www-form-urlencoded");
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, form->c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)form->size());
    }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, CurlBody);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, CurlHeader);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &r);
    const CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        r.status = (int)status;
    } else {
        r.error = curl_easy_strerror(rc);
    }
    curl_slist_free_all(list);
    curl_easy_cleanup(c);
    return r;
}

HttpResponse HttpGet(const std::string& url, const std::vector<std::string>& headers) { return CurlRequest(url, headers, nullptr); }

HttpResponse HttpPost(const std::string& url, const std::vector<std::string>& headers, const std::string& form) {
    return CurlRequest(url, headers, &form);
}

// The keychain: generic passwords of the service "Mangrove".
static CFMutableDictionaryRef KeychainQuery(const std::wstring& key) {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const std::string k = Narrow(key);
    CFStringRef account = CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8*)k.data(), (CFIndex)k.size(), kCFStringEncodingUTF8, false);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, CFSTR("Mangrove"));
    if (account) {
        CFDictionarySetValue(q, kSecAttrAccount, account);
        CFRelease(account);
    }
    return q;
}

bool SecretSave(const std::wstring& key, const std::string& secret, const std::wstring& label) {
    CFMutableDictionaryRef q = KeychainQuery(key);
    SecItemDelete(q);
    const std::string l = Narrow(label);
    CFStringRef cfLabel = CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8*)l.data(), (CFIndex)l.size(), kCFStringEncodingUTF8, false);
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, (const UInt8*)secret.data(), (CFIndex)secret.size());
    if (cfLabel) CFDictionarySetValue(q, kSecAttrLabel, cfLabel);
    if (data) CFDictionarySetValue(q, kSecValueData, data);
    const OSStatus st = data ? SecItemAdd(q, nullptr) : errSecParam;
    if (cfLabel) CFRelease(cfLabel);
    if (data) CFRelease(data);
    CFRelease(q);
    return st == errSecSuccess;
}

bool SecretLoad(const std::wstring& key, std::string& secret) {
    CFMutableDictionaryRef q = KeychainQuery(key);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef found = nullptr;
    const OSStatus st = SecItemCopyMatching(q, &found);
    CFRelease(q);
    if (st != errSecSuccess || !found) return false;
    CFDataRef data = (CFDataRef)found;
    secret.assign((const char*)CFDataGetBytePtr(data), (size_t)CFDataGetLength(data));
    CFRelease(found);
    return true;
}

void SecretDelete(const std::wstring& key) {
    CFMutableDictionaryRef q = KeychainQuery(key);
    SecItemDelete(q);
    CFRelease(q);
}

std::wstring SecretStoreName() { return L"the macOS keychain"; }

#else  // other POSIX systems: no HTTPS client or credential store yet

HttpResponse HttpGet(const std::string&, const std::vector<std::string>&) {
    HttpResponse r;
    r.error = "not available on this system";
    return r;
}
HttpResponse HttpPost(const std::string&, const std::vector<std::string>&, const std::string&) {
    HttpResponse r;
    r.error = "not available on this system";
    return r;
}
bool SecretSave(const std::wstring&, const std::string&, const std::wstring&) { return false; }
bool SecretLoad(const std::wstring&, std::string&) { return false; }
void SecretDelete(const std::wstring&) {}
std::wstring SecretStoreName() { return L"the credential store"; }

#endif

// ============================================================================ file watcher

#ifdef __APPLE__

struct RepoWatcher::Impl {
    std::function<void()> onChange;
    FSEventStreamRef stream = nullptr;
    dispatch_queue_t queue = nullptr;
};

static void OnFsEvents(ConstFSEventStreamRef, void* info, size_t count, void* paths, const FSEventStreamEventFlags*,
                       const FSEventStreamEventId*) {
    auto* impl = static_cast<RepoWatcher::Impl*>(info);
    char** list = static_cast<char**>(paths);
    for (size_t i = 0; i < count; ++i) {
        if (!strstr(list[i], "/.git/objects")) {  // objects always come with a ref or index change
            impl->onChange();
            return;
        }
    }
}

RepoWatcher::RepoWatcher(const std::wstring& root, std::function<void()> cb) : impl(std::make_unique<Impl>()) {
    impl->onChange = std::move(cb);
    const std::string r = P(root);
    CFStringRef path = CFStringCreateWithCString(kCFAllocatorDefault, r.c_str(), kCFStringEncodingUTF8);
    if (!path) return;
    const void* values[1] = { path };
    CFArrayRef paths = CFArrayCreate(kCFAllocatorDefault, values, 1, &kCFTypeArrayCallBacks);
    FSEventStreamContext ctx{ 0, impl.get(), nullptr, nullptr, nullptr };
    // Events are delivered 0.35 s after the first one of a burst: files have settled by then.
    impl->stream = FSEventStreamCreate(kCFAllocatorDefault, &OnFsEvents, &ctx, paths, kFSEventStreamEventIdSinceNow, 0.35,
                                       kFSEventStreamCreateFlagNone);
    CFRelease(paths);
    CFRelease(path);
    if (!impl->stream) return;
    impl->queue = dispatch_queue_create("mangrove.watcher", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(impl->stream, impl->queue);
    FSEventStreamStart(impl->stream);
}

RepoWatcher::~RepoWatcher() {
    if (impl->stream) {
        FSEventStreamStop(impl->stream);
        FSEventStreamInvalidate(impl->stream);
        FSEventStreamRelease(impl->stream);
    }
    if (impl->queue) {
        dispatch_sync_f(impl->queue, nullptr, [](void*) {});  // a callback still running has finished
        dispatch_release(impl->queue);
    }
}

#else

// Elsewhere the history refreshes on demand (window activation, F5).
struct RepoWatcher::Impl {};
RepoWatcher::RepoWatcher(const std::wstring&, std::function<void()>) : impl(std::make_unique<Impl>()) {}
RepoWatcher::~RepoWatcher() = default;

#endif
