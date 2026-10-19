// Windows implementation of the operating system services used by the shared core (platform.h).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#include "platform.h"
#include "text.h"
#include "watcher.h"

// ============================================================================ text

std::wstring ToLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

std::wstring ToUpper(std::wstring s) {
    if (!s.empty()) CharUpperBuffW(s.data(), (DWORD)s.size());
    return s;
}

bool ContainsI(std::wstring_view hay, std::wstring_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    return FindNLSStringEx(LOCALE_NAME_INVARIANT, FIND_FROMSTART | LINGUISTIC_IGNORECASE, hay.data(), (int)hay.size(), needle.data(),
                           (int)needle.size(), nullptr, nullptr, nullptr, 0) >= 0;
}

int CompareNatural(const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()); }

// ============================================================================ time

static bool ToSystemLocal(int64_t t, SYSTEMTIME& local) {
    if (t < 0) t = 0;
    ULARGE_INTEGER u;
    u.QuadPart = (uint64_t)(t + 11644473600LL) * 10000000ULL;
    FILETIME ft{ u.LowPart, u.HighPart };
    SYSTEMTIME utc;
    if (!FileTimeToSystemTime(&ft, &utc)) return false;
    return SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) != 0;
}

bool ToLocalTime(int64_t t, LocalTime& out) {
    SYSTEMTIME st;
    if (!ToSystemLocal(t, st)) return false;
    out = { st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute };
    return true;
}

std::wstring FormatClock(int64_t t) {
    SYSTEMTIME st;
    if (!ToSystemLocal(t, st)) return L"";
    wchar_t buf[80];
    if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, buf, 80)) return L"";
    return buf;
}

// ============================================================================ files

static uint64_t Stamp(const FILETIME& ft, DWORD sizeLow) {
    return ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) ^ ((uint64_t)sizeLow << 7);
}

static int64_t UnixTime(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)(u.QuadPart / 10000000ULL) - 11644473600LL;
}

bool DirExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool FileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool StatFile(const std::wstring& path, int64_t* mtime, uint64_t* stamp) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    if (mtime) *mtime = UnixTime(a.ftLastWriteTime);
    if (stamp) *stamp = Stamp(a.ftLastWriteTime, a.nFileSizeLow);
    return true;
}

void ListDir(const std::wstring& dir, const std::function<void(std::wstring_view, uint64_t, bool)>& f) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(PathJoin(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
        f(fd.cFileName, Stamp(fd.ftLastWriteTime, fd.nFileSizeLow), (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool ReadWholeFile(const std::wstring& path, std::string& out, size_t maxBytes, bool* cut) {
    out.clear();
    if (cut) *cut = false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::vector<char> buf(1 << 16);
    DWORD n = 0;
    while (ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr) && n) {
        if (out.size() + n >= maxBytes) {
            out.append(buf.data(), maxBytes - out.size());
            if (cut) *cut = true;
            break;
        }
        out.append(buf.data(), n);
    }
    CloseHandle(f);
    return true;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& bytes) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(f);
    if (!ok || written != bytes.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::wstring WriteTempFile(const std::string& bytes) {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"dtm", 0, path)) return L"";
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    DWORD written = 0;
    const BOOL ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(f);
    if (!ok || written != bytes.size()) {
        DeleteFileW(path);
        return L"";
    }
    return path;
}

void RemoveFile(const std::wstring& path) { DeleteFileW(path.c_str()); }

std::wstring AppDataDir() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) {
        dir = std::wstring(p) + L"\\DreamTree";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    if (p) CoTaskMemFree(p);
    return dir;
}

std::wstring EnvVar(const char* name) {
    const std::wstring w = Widen(name);
    wchar_t buf[2048];
    const DWORD n = GetEnvironmentVariableW(w.c_str(), buf, (DWORD)std::size(buf));
    return n > 0 && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}

// ============================================================================ processes

// git.exe as found on PATH. For Git for Windows this is the cmd\git.exe launcher, which prepares the
// same environment as a terminal (ssh, hooks, credential manager): used for commands that write.
static const std::wstring& GitLauncher() {
    static std::wstring exe;
    static std::once_flag once;
    std::call_once(once, [] {
        wchar_t buf[MAX_PATH * 2];
        const DWORD n = SearchPathW(nullptr, L"git.exe", nullptr, (DWORD)std::size(buf), buf, nullptr);
        std::wstring p = (n > 0 && n < std::size(buf)) ? std::wstring(buf, n) : std::wstring();
        if (p.empty()) {
            const wchar_t* candidates[] = { L"C:\\Program Files\\Git\\cmd\\git.exe", L"C:\\Program Files (x86)\\Git\\cmd\\git.exe" };
            for (const wchar_t* c : candidates)
                if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) { p = c; break; }
        }
        exe = p.empty() ? L"git.exe" : p;
    });
    return exe;
}

// The real git binary: reads skip the launcher and save a process per command.
static const std::wstring& GitBinary() {
    static std::wstring exe;
    static std::once_flag once;
    std::call_once(once, [] {
        std::wstring p = GitLauncher();
        const std::wstring low = ToLower(p);
        const size_t pos = low.rfind(L"\\cmd\\git.exe");
        if (pos != std::wstring::npos) {
            const std::wstring real = p.substr(0, pos) + L"\\mingw64\\bin\\git.exe";
            if (GetFileAttributesW(real.c_str()) != INVALID_FILE_ATTRIBUTES) p = real;
        }
        exe = p;
    });
    return exe;
}

std::wstring GitPath(bool forWrites) { return forWrites ? GitLauncher() : GitBinary(); }

// Environment for git: never wait for terminal input (credential prompts, merge message editor)
// and always report in English so messages can be recognized.
static const std::vector<wchar_t>& GitEnv() {
    static std::vector<wchar_t> block;
    static std::once_flag once;
    std::call_once(once, [] {
        const std::pair<const wchar_t*, const wchar_t*> extra[] = {
            { L"GIT_TERMINAL_PROMPT", L"0" }, { L"GIT_MERGE_AUTOEDIT", L"no" }, { L"LANG", L"en_US.UTF-8" }, { L"LC_ALL", L"en_US.UTF-8" } };
        if (LPWCH env = GetEnvironmentStringsW()) {
            for (LPCWSTR p = env; *p; p += wcslen(p) + 1) {
                const size_t len = wcslen(p);
                bool overridden = false;
                for (const auto& e : extra) {
                    const size_t k = wcslen(e.first);
                    if (len > k && p[k] == L'=' && _wcsnicmp(p, e.first, k) == 0) overridden = true;
                }
                if (!overridden) { block.insert(block.end(), p, p + len); block.push_back(0); }
            }
            FreeEnvironmentStringsW(env);
        }
        for (const auto& e : extra) {
            const std::wstring kv = std::wstring(e.first) + L"=" + e.second;
            block.insert(block.end(), kv.begin(), kv.end());
            block.push_back(0);
        }
        block.push_back(0);
    });
    return block;
}

int RunProcess(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd, std::string* out, std::string* err,
               const Sink& sink, const Sink& errSink) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&outR, &outW, &sa, 1 << 20)) return -1;
    if (!CreatePipe(&errR, &errW, &sa, 1 << 16)) { CloseHandle(outR); CloseHandle(outW); return -1; }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    // Only the three std handles are inherited (safe with concurrent spawns from other threads).
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    HANDLE inherit[3] = { nul, outW, errW };
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = outW;
    si.StartupInfo.hStdError = errW;
    si.lpAttributeList = attrs;

    std::wstring cmd = L"\"" + exe + L"\" " + args;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                                   (LPVOID)GitEnv().data(), cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    const DWORD startErr = ok ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(outW);
    CloseHandle(errW);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(outR);
        CloseHandle(errR);
        if (err) {
            *err = startErr == ERROR_DIRECTORY || startErr == ERROR_PATH_NOT_FOUND || startErr == ERROR_FILE_NOT_FOUND
                       ? (GetFileAttributesW(cwd.c_str()) == INVALID_FILE_ATTRIBUTES ? "The folder does not exist."
                                                                                     : "git.exe was not found. Install Git for Windows.")
                       : "Could not start git.exe.";
        }
        return -1;
    }

    std::string errBuf;
    std::thread errThread([&] {
        char b[4096];
        DWORD n = 0;
        while (ReadFile(errR, b, sizeof b, &n, nullptr) && n) {
            errBuf.append(b, n);
            if (errSink) errSink(b, n);
        }
    });
    std::vector<char> buf(1 << 16);
    DWORD n = 0;
    while (ReadFile(outR, buf.data(), (DWORD)buf.size(), &n, nullptr) && n) {
        if (sink) sink(buf.data(), n);
        else if (out) out->append(buf.data(), n);
    }
    errThread.join();
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(outR);
    CloseHandle(errR);
    if (err) *err = std::move(errBuf);
    return (int)code;
}

// ============================================================================ file watcher

struct RepoWatcher::Impl {
    std::wstring root;
    std::function<void()> onChange;
    HANDLE stopEvent = nullptr;
    std::thread thread;
    void Run();
};

RepoWatcher::RepoWatcher(const std::wstring& r, std::function<void()> cb) : impl(std::make_unique<Impl>()) {
    impl->root = r;
    impl->onChange = std::move(cb);
    impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Impl* p = impl.get();
    impl->thread = std::thread([p] { p->Run(); });
}

RepoWatcher::~RepoWatcher() {
    SetEvent(impl->stopEvent);
    if (impl->thread.joinable()) impl->thread.join();
    CloseHandle(impl->stopEvent);
}

static bool Relevant(const uint8_t* buf, DWORD bytes) {
    static const wchar_t kObjects[] = L".git\\objects\\";
    const size_t kLen = std::size(kObjects) - 1;
    for (DWORD off = 0; off < bytes;) {
        auto* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buf + off);
        const size_t n = fni->FileNameLength / sizeof(wchar_t);
        const bool objects = n >= kLen && _wcsnicmp(fni->FileName, kObjects, kLen) == 0;
        if (!objects) return true;
        if (!fni->NextEntryOffset) break;
        off += fni->NextEntryOffset;
    }
    return false;
}

void RepoWatcher::Impl::Run() {
    HANDLE dir = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir == INVALID_HANDLE_VALUE) return;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<DWORD> storage(64 * 1024 / sizeof(DWORD));  // DWORD-aligned, as the API requires
    uint8_t* buf = reinterpret_cast<uint8_t*>(storage.data());
    const DWORD bufSize = (DWORD)(storage.size() * sizeof(DWORD));
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                         FILE_NOTIFY_CHANGE_SIZE;
    bool issued = false, pending = false;
    HANDLE handles[2] = { ov.hEvent, stopEvent };
    for (;;) {
        if (!issued) {
            ResetEvent(ov.hEvent);
            if (!ReadDirectoryChangesW(dir, buf, bufSize, TRUE, filter, nullptr, &ov, nullptr)) break;
            issued = true;
        }
        const DWORD w = WaitForMultipleObjects(2, handles, FALSE, pending ? 350 : INFINITE);
        if (w == WAIT_OBJECT_0 + 1) break;  // stop
        if (w == WAIT_TIMEOUT) {           // quiet period elapsed: report once
            pending = false;
            onChange();
            continue;
        }
        if (w != WAIT_OBJECT_0) break;
        DWORD bytes = 0;
        const BOOL ok = GetOverlappedResult(dir, &ov, &bytes, FALSE);
        issued = false;
        if (!ok) break;
        if (bytes == 0 || Relevant(buf, bytes)) pending = true;  // 0 bytes = buffer overflow, assume changes
    }
    if (issued) {
        CancelIoEx(dir, &ov);
        DWORD bytes = 0;
        GetOverlappedResult(dir, &ov, &bytes, TRUE);
    }
    CloseHandle(ov.hEvent);
    CloseHandle(dir);
}
