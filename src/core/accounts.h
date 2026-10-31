#pragma once
#include "base.h"

// Accounts on code hosting services (GitHub, GitLab, Gitea / Forgejo, Bitbucket, Azure DevOps), shared by the
// apps. Mangrove keeps the list (service, server, user name) in its settings and each access token in the user's
// credential store (Windows Credential Manager, the macOS keychain, the Secret Service on Linux), never in a file.
// Fetch, pull, push and clone over HTTPS to an account's server use its token.

enum class Forge : uint8_t { GitHub, GitLab, Gitea, Bitbucket, AzureDevOps };

struct ForgeInfo {
    Forge forge;
    const wchar_t* name;        // "GitHub"
    const wchar_t* server;      // the public service, "https://github.com" (Azure DevOps: where an organization's address starts)
    const wchar_t* serverHelp;  // what the server is
    const wchar_t* tokenHelp;   // what kind of token to create
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
// Only where Mangrove has a registered OAuth app: github.com, and gitlab.com, whose tokens expire after two hours:
// Mangrove keeps the token that renews them too, and renews them when they are about to expire.
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
    std::wstring error;        // Failed: why
    std::string token;         // Granted
    std::string refreshToken;  // Granted, when the token expires: what renews it
    int expiresIn = 0;         // Granted: seconds the token lives, 0 when it doesn't expire
    int interval = 0;          // SlowDown: the interval the service asks for, when it says
};
// The service's answers (HTTP status and body), read. Separate for the tests.
DeviceCode ParseDeviceCode(int status, const std::string& body, const std::wstring& service = L"GitHub");
DevicePoll ParseDevicePoll(int status, const std::string& body, const std::wstring& service = L"GitHub");

// The whole sign-in, blocking (a worker thread): asks for a code and gives it to onCode (to show it), then
// asks every few seconds whether the user authorized Mangrove, until they did, refused, the code expired or
// cancel became true. Then checks the token like CheckToken; *token receives what to keep (AddAccount) when the
// check is ok: the token, or with one that expires, a StoredToken's text.
TokenCheck BrowserSignIn(Forge f, const std::wstring& server, const std::function<void(const DeviceCode&)>& onCode,
                         const std::atomic<bool>& cancel, std::string* token);

// What the credential store keeps for an account: a token as it was pasted, or one from signing in that expires,
// with the token that renews it (lines: "oauth", the token, the renewing one, when it expires, the OAuth app).
struct StoredToken {
    std::string access;            // what authorizes the requests
    std::string refresh, client;   // an expiring token: what renews it, and the OAuth app that does
    int64_t expires = 0;           // unix seconds; 0: it doesn't expire
    std::string Text() const;      // to store
    static StoredToken Read(const std::string& text);
};

// git's configuration for one account: its token as the authorization of HTTPS requests to its server, and the
// server's SSH addresses (git@host:path, ssh://git@host/path) read as HTTPS ones, so that remotes written either
// way use the account (no SSH key needed). Name and value pairs.
std::vector<std::pair<std::string, std::string>> GitAuthConfig(const Account& a, const std::string& token);

// git's environment for fetch, pull and push: GitAuthConfig of each account, as GIT_CONFIG_* variables (never on
// a command line or in a file). Empty without accounts. Reads the credential store the first time: call it on a
// worker thread.
std::vector<std::string> GitAuthEnvironment(const std::vector<Account>& accounts);

// ---- the repositories of an account, to clone them

struct RemoteRepository {
    std::wstring name;         // "hello-world"
    std::wstring path;         // "octocat/hello-world": the owner, group or project, and the name
    std::wstring url;          // the HTTPS address git clones
    std::wstring description;
    bool isPrivate = false;
    int64_t updated = 0;       // unix seconds of the last change, 0 when unknown
};
struct RepositoryList {
    std::vector<RemoteRepository> repos;  // the most recently changed first
    std::wstring error;                   // why they couldn't be listed; empty when they could
};
// Blocking: a worker thread. The first thousand or so; reads the credential store.
RepositoryList ListRepositories(const Account& a);
// The server's answers, read (separate for the tests): one page of repositories, as each service writes them.
std::vector<RemoteRepository> ParseRepositories(Forge f, const std::string& body);
int64_t ParseIsoTime(std::string_view s);  // "2026-10-30T12:34:56Z" (fractions and offsets too) -> unix seconds; 0 when it isn't one
