#pragma once
#include "base.h"

// Accounts on code hosting services (GitHub, GitLab, Gitea / Forgejo), shared by the apps. Mangrove keeps
// the list (service, server, user name) in its settings and each access token in the user's credential store
// (Windows Credential Manager, the macOS keychain, the Secret Service on Linux), never in a file. Fetch, pull and push over HTTPS to an
// account's server use its token.

enum class Forge : uint8_t { GitHub, GitLab, Gitea };

struct ForgeInfo {
    Forge forge;
    const wchar_t* name;       // "GitHub"
    const wchar_t* server;     // the public service, "https://github.com"
    const wchar_t* tokenHelp;  // what kind of token to create
};
const std::vector<ForgeInfo>& Forges();
const ForgeInfo& InfoOf(Forge f);

struct Account {
    Forge forge = Forge::GitHub;
    std::wstring server;  // "https://github.com", or a self-hosted server
    std::wstring login;   // the user name on the server
    std::wstring name;    // the display name, when there is one
    std::wstring Host() const;  // "github.com"
    std::wstring Key() const;   // the token's name in the credential store
};

std::vector<Account> LoadAccounts();  // from AppSettings(), in the order they were added
// "gitlab.example.com/" -> "https://gitlab.example.com". Empty, with the reason, when it isn't an HTTPS address.
std::wstring NormalizeServer(const std::wstring& input, std::wstring* error);
std::wstring TokenPageUrl(Forge f, const std::wstring& server);  // where to create a token (name and scopes filled in)

// Asks the server whom a token belongs to. Blocking: call it on a worker thread.
struct TokenCheck {
    bool ok = false;
    std::wstring error;  // why not
    std::wstring note;   // ok, but something to know (a token that can't push, ...)
    Account account;
};
TokenCheck CheckToken(Forge f, const std::wstring& server, const std::string& token);

// Stores the token in the credential store and the account in the settings, replacing the account of the
// same server. False, with the reason, when the credential store refuses it.
bool AddAccount(const Account& a, const std::string& token, std::wstring* error);
void RemoveAccount(const Account& a);

// ---- signing in with the browser (OAuth 2.0 device authorization grant, RFC 8628)
// Mangrove asks the service for a one-time code, the user enters it on the service's site and authorizes
// Mangrove there, and Mangrove receives a token for the account, then checks and keeps it like a pasted one.
// Only where Mangrove has a registered OAuth app: github.com (GitLab's OAuth tokens expire after two hours).
bool BrowserSignInAvailable(Forge f, const std::wstring& server);

struct DeviceCode {
    std::wstring error;      // why the service gave no code; empty when it did
    std::string deviceCode;  // what Mangrove asks for the token with
    std::wstring userCode;   // what the user enters on the site: "WDJB-MJHT"
    std::wstring verifyUrl;  // where: "https://github.com/login/device"
    int interval = 5;        // seconds between two questions
    int expiresIn = 900;     // seconds the code lives
};
enum class DeviceState : uint8_t { Pending, SlowDown, Granted, Denied, Expired, Failed };
struct DevicePoll {
    DeviceState state = DeviceState::Failed;
    std::wstring error;  // Failed: why
    std::string token;   // Granted
    int interval = 0;    // SlowDown: the interval the service asks for, when it says
};
// The service's answers (HTTP status and body), read. Separate for the tests.
DeviceCode ParseDeviceCode(int status, const std::string& body);
DevicePoll ParseDevicePoll(int status, const std::string& body);

// The whole sign-in, blocking (a worker thread): asks for a code and gives it to onCode (to show it), then
// asks every few seconds whether the user authorized Mangrove, until they did, refused, the code expired or
// cancel became true. Then checks the token like CheckToken; *token receives it when the check is ok.
TokenCheck BrowserSignIn(Forge f, const std::wstring& server, const std::function<void(const DeviceCode&)>& onCode,
                         const std::atomic<bool>& cancel, std::string* token);

// git's configuration for one account: its token as the authorization of HTTPS requests to its server, and the
// server's SSH addresses (git@host:path, ssh://git@host/path) read as HTTPS ones, so that remotes written either
// way use the account (no SSH key needed). Name and value pairs.
std::vector<std::pair<std::string, std::string>> GitAuthConfig(const Account& a, const std::string& token);

// git's environment for fetch, pull and push: GitAuthConfig of each account, as GIT_CONFIG_* variables (never on
// a command line or in a file). Empty without accounts. Reads the credential store the first time: call it on a
// worker thread.
std::vector<std::string> GitAuthEnvironment(const std::vector<Account>& accounts);
