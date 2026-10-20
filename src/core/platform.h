#pragma once
#include "base.h"

// Operating system services used by the shared core. platform_win.cpp implements them for Windows,
// platform_posix.cpp for macOS (and other POSIX systems).
//
// Text is std::wstring throughout the core: UTF-16 on Windows, UTF-32 on macOS. Paths use the native
// separator; repository-relative paths from git always use '/'.

#ifdef _WIN32
inline constexpr wchar_t kPathSep = L'\\';
#else
inline constexpr wchar_t kPathSep = L'/';
#endif

// ---------------------------------------------------------------- text that depends on the system's tables
std::wstring ToLower(std::wstring s);
std::wstring ToUpper(std::wstring s);
bool ContainsI(std::wstring_view hay, std::wstring_view needle);    // ignoring case
int CompareNatural(const std::wstring& a, const std::wstring& b);  // "v2" before "v10", ignoring case

// ---------------------------------------------------------------- time
struct LocalTime {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
};
bool ToLocalTime(int64_t unixSec, LocalTime& out);
std::wstring FormatClock(int64_t unixSec);  // hours and minutes, as the user's locale writes them

// ---------------------------------------------------------------- files
bool DirExists(const std::wstring& path);
bool FileExists(const std::wstring& path);
// Last write time (unix seconds) and a value that changes whenever the file does. False when missing.
bool StatFile(const std::wstring& path, int64_t* mtime, uint64_t* stamp);
// Entries of a folder (without "." and ".."), with a change stamp each.
void ListDir(const std::wstring& dir, const std::function<void(std::wstring_view name, uint64_t stamp, bool isDir)>& f);
bool ReadWholeFile(const std::wstring& path, std::string& out, size_t maxBytes = SIZE_MAX, bool* cut = nullptr);
bool WriteFileAtomic(const std::wstring& path, const std::string& bytes);  // replaces the file in one step
std::wstring WriteTempFile(const std::string& bytes);                     // new temporary file; empty on failure
void RemoveFile(const std::wstring& path);
std::wstring AppDataDir();             // per-user folder for DreamTree's settings (created on demand)
std::wstring EnvVar(const char* name);  // empty when unset

// ---------------------------------------------------------------- processes
using Sink = std::function<void(const char*, size_t)>;
// The git executable: for reads, and for commands that write (on Windows the cmd\git.exe launcher of Git for
// Windows, which prepares ssh, hooks and the credential manager like a terminal does).
std::wstring GitPath(bool forWrites);
// Runs `exe` with `args` (a command line quoted with QuoteArg) in `cwd`, with git's environment: no terminal
// prompts, English messages, plus the "NAME=value" entries of `env`. stdout goes to `outSink` when set, else to
// *out; stderr to *err and errSink. Returns the exit code, or -1 when the program could not start (the reason
// in *err).
int RunProcess(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd, std::string* out, std::string* err,
               const Sink& outSink = nullptr, const Sink& errSink = nullptr, const std::vector<std::string>& env = {});

// ---------------------------------------------------------------- network
// An HTTPS GET request, without following redirects. Blocking: call it on a worker thread.
struct HttpResponse {
    int status = 0;  // 0: no answer (error says why)
    std::string body, error;
    std::vector<std::pair<std::string, std::string>> headers;  // names in lower case
    bool Has(std::string_view name) const {
        for (const auto& h : headers)
            if (h.first == name) return true;
        return false;
    }
    std::string Header(std::string_view name) const {
        for (const auto& h : headers)
            if (h.first == name) return h.second;
        return {};
    }
};
HttpResponse HttpGet(const std::string& url, const std::vector<std::string>& headers);  // "Name: value" headers

// ---------------------------------------------------------------- secrets
// The user's credential store (Windows Credential Manager, the macOS keychain): secrets stay there, never in
// DreamTree's files. `key` names the secret, `label` describes it to the user.
bool SecretSave(const std::wstring& key, const std::string& secret, const std::wstring& label);
bool SecretLoad(const std::wstring& key, std::string& secret);
void SecretDelete(const std::wstring& key);
std::wstring SecretStoreName();  // "Windows Credential Manager", "the macOS keychain" (for the middle of a sentence)

// ---------------------------------------------------------------- threads
// Each app installs how a function is run on its UI thread; the core posts its results through it.
void SetMainDispatcher(std::function<void(std::function<void()>)> post);
void RunOnMain(std::function<void()> fn);
