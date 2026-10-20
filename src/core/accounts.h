#pragma once
#include "base.h"

// Accounts on code hosting services (GitHub, GitLab, Gitea / Forgejo), shared by the apps. DreamTree keeps
// the list (service, server, user name) in its settings and each access token in the user's credential store
// (Windows Credential Manager, the macOS keychain), never in a file. Fetch, pull and push over HTTPS to an
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

// git's environment for HTTPS fetch, pull and push: each account's token as the authorization of requests to
// its server (GIT_CONFIG_* variables: never on a command line or in a file). Empty without accounts. Reads
// the credential store the first time: call it on a worker thread.
std::vector<std::string> GitAuthEnvironment(const std::vector<Account>& accounts);
