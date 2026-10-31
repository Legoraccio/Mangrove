#include "accounts.h"
#include "platform.h"
#include "settings.h"
#include "text.h"
#include <chrono>
#include <cstdlib>

namespace {

const std::vector<ForgeInfo> kForges = {
    { Forge::GitHub, L"GitHub", L"https://github.com", L"Change it for GitHub Enterprise Server.",
      L"A classic token with the repo scope (and workflow, to push workflow files), or a fine-grained token with "
      L"read and write access to the contents of the repositories." },
    { Forge::GitLab, L"GitLab", L"https://gitlab.com", L"Change it for a self-managed GitLab server.",
      L"A personal access token with the read_user, read_api (to list your projects), read_repository and write_repository scopes." },
    { Forge::Gitea, L"Gitea / Forgejo", L"https://codeberg.org", L"Codeberg, or the address of another Gitea or Forgejo server.",
      L"An access token with read and write permission on repositories and read permission on the user." },
    { Forge::Bitbucket, L"Bitbucket", L"https://bitbucket.org", L"Bitbucket Cloud.",
      L"An Atlassian API token with scopes, for Bitbucket: read:user, read:workspace and read:repository, and write:repository "
      L"to push." },
    { Forge::AzureDevOps, L"Azure DevOps", L"https://dev.azure.com/", L"The address of your organization: https://dev.azure.com/ and its name.",
      L"A personal access token of the organization with the Code scope, read and write." },
};

const wchar_t* ForgeId(Forge f) {
    switch (f) {
    case Forge::GitLab: return L"gitlab";
    case Forge::Gitea: return L"gitea";
    case Forge::Bitbucket: return L"bitbucket";
    case Forge::AzureDevOps: return L"azure";
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

bool RenewToken(const Account& a, StoredToken& t);

bool TokenOf(const Account& a, std::string& token) {
    std::lock_guard<std::mutex> lk(g_tokensMutex);
    const std::wstring key = a.Key();
    auto it = g_tokens.find(key);
    if (it == g_tokens.end()) {
        std::string t;
        if (!SecretLoad(key, t)) t.clear();
        it = g_tokens.emplace(key, std::move(t)).first;
    }
    // A token that expires is renewed a few minutes before, and the new one kept.
    StoredToken t = StoredToken::Read(it->second);
    if (t.expires && UnixNow() > t.expires - 300 && !t.refresh.empty() && RenewToken(a, t)) {
        it->second = t.Text();
        SecretSave(key, it->second, std::wstring(L"Mangrove: ") + InfoOf(a.forge).name + L" token of " + a.login + L" on " + a.Host());
    }
    token = t.access;
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

    // The elements of the array the text is, as written (read each one with a Json of its own).
    std::vector<std::string_view> Elements() {
        std::vector<std::string_view> out;
        p = 0;
        Space();
        if (!Eat('[')) return out;
        Space();
        if (Eat(']')) return out;
        for (;;) {
            Space();
            const size_t from = p;
            if (p < s.size() && s[p] == '"') {
                std::string ignored;
                if (!String(ignored)) return out;
            } else if (!Skip()) {
                return out;
            }
            out.push_back(TrimSv(s.substr(from, p - from)));
            Space();
            if (Eat(']') || !Eat(',')) return out;
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

// Mangrove's OAuth apps, see the README. On github.com: Device Flow on, user tokens that don't expire. On gitlab.com:
// not confidential, with the device authorization grant; its tokens expire after two hours and Mangrove renews them.
// The device flow needs no client secret, and a client ID is public. Empty: no signing in with the browser.
constexpr char kGitHubClientId[] = "Ov23li3hoJcbPe6kwo7l";
constexpr char kGitLabClientId[] = "c5d5f1708159374234c90369b3de3f492291b220d09eb6eebc37458e5fd35d67";

// The client ID in use: MANGROVE_GITHUB_CLIENT_ID or MANGROVE_GITLAB_CLIENT_ID (another OAuth app, to try it), or
// Mangrove's.
const std::string& ClientId(Forge f) {
    static const std::string github = [] {
        const std::wstring env = EnvVar("MANGROVE_GITHUB_CLIENT_ID");
        return env.empty() ? std::string(kGitHubClientId) : Narrow(env);
    }();
    static const std::string gitlab = [] {
        const std::wstring env = EnvVar("MANGROVE_GITLAB_CLIENT_ID");
        return env.empty() ? std::string(kGitLabClientId) : Narrow(env);
    }();
    static const std::string none;
    return f == Forge::GitHub ? github : f == Forge::GitLab ? gitlab : none;
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
std::wstring SignInError(int status, const std::string& code, const std::string& description, const std::wstring& service) {
    if (code == "device_flow_disabled") return L"Signing in with the browser isn't enabled for Mangrove's " + service + L" app.";
    if (code == "incorrect_client_credentials" || code == "unauthorized_client" || code == "invalid_client")
        return service + L" doesn't know Mangrove's app: use a token instead.";
    if (code == "incorrect_device_code" || code == "bad_verification_code") return service + L" didn't recognize the code: start again.";
    if (!code.empty())
        return service + L" refused the sign-in (" + Widen(code) + L")" + (description.empty() ? L"." : L": " + Widen(description));
    return service + L" answered with error " + std::to_wstring(status) + L".";
}

// A form's fields, encoded.
std::string Form(std::initializer_list<std::pair<const char*, std::string>> fields) {
    std::string out;
    for (const auto& [k, v] : fields) out += (out.empty() ? "" : "&") + std::string(k) + "=" + FormEncode(v);
    return out;
}

// An expiring token renewed with the one that renews it (GitLab's OAuth tokens). Blocking.
bool RenewToken(const Account& a, StoredToken& t) {
    const HttpResponse res = HttpPost(Narrow(a.server) + "/oauth/token", { "User-Agent: Mangrove", "Accept: application/json" },
                                      Form({ { "grant_type", "refresh_token" }, { "refresh_token", t.refresh }, { "client_id", t.client } }));
    if (res.status != 200) return false;
    Json json(res.body);
    const std::string access = json.Member("access_token"), refresh = json.Member("refresh_token");
    if (access.empty()) return false;
    t.access = access;
    if (!refresh.empty()) t.refresh = refresh;  // the old one stops working
    t.expires = UnixNow() + std::clamp(ToInt(json.Member("expires_in"), 7200), 60, 86400 * 30);
    return true;
}

// The headers of a request to a service's API with a token.
std::vector<std::string> ApiHeaders(Forge f, const std::string& token) {
    std::vector<std::string> h{ "User-Agent: Mangrove", "Accept: application/json" };
    switch (f) {
    case Forge::Gitea: h.push_back("Authorization: token " + token); break;
    case Forge::AzureDevOps: h.push_back("Authorization: Basic " + Base64(":" + token)); break;
    default: h.push_back("Authorization: Bearer " + token); break;  // GitHub, GitLab, Bitbucket
    }
    if (f == Forge::GitHub) h.push_back("X-GitHub-Api-Version: 2022-11-28");
    return h;
}

// The address of an account's server by the rules of its service; empty, with the reason, when it can't be one.
std::wstring ServerFor(Forge f, const std::wstring& input, std::wstring* error) {
    const std::wstring s = NormalizeServer(input, error);
    auto fail = [&](const wchar_t* why) {
        if (error) *error = why;
        return std::wstring();
    };
    if (s.empty()) return s;
    if (f == Forge::Bitbucket && s != L"https://bitbucket.org") return fail(L"Mangrove connects to Bitbucket Cloud: https://bitbucket.org.");
    if (f != Forge::AzureDevOps) return s;
    // An organization: https://dev.azure.com/<organization>, or its old address https://<organization>.visualstudio.com
    std::wstring host = s.substr(8), path;
    if (const size_t slash = host.find(L'/'); slash != std::wstring::npos) {
        path = host.substr(slash + 1);
        host.resize(slash);
    }
    std::wstring org = path.substr(0, path.find(L'/'));
    const std::wstring old = L".visualstudio.com";
    if (host.size() > old.size() && host.compare(host.size() - old.size(), old.size(), old) == 0) org = host.substr(0, host.size() - old.size());
    else if (host != L"dev.azure.com") return fail(L"This is not an Azure DevOps address: https://dev.azure.com/ and the name of your organization.");
    if (org.empty()) return fail(L"Add the name of your organization to the address: https://dev.azure.com/your-organization.");
    return L"https://dev.azure.com/" + org;
}

// "https://ada@host/x" -> "https://host/x": the token goes with the requests, not a user name in the address.
std::wstring WithoutUser(std::wstring url) {
    const size_t scheme = url.find(L"://");
    if (scheme == std::wstring::npos) return url;
    const size_t at = url.find(L'@', scheme + 3), slash = url.find(L'/', scheme + 3);
    if (at != std::wstring::npos && (slash == std::wstring::npos || at < slash)) url.erase(scheme + 3, at - scheme - 2);
    return url;
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
        return server + L"/-/user_settings/personal_access_tokens?name=Mangrove&scopes=read_user,read_api,read_repository,write_repository";
    case Forge::Gitea: return server + L"/user/settings/applications";
    case Forge::Bitbucket: return L"https://id.atlassian.com/manage-profile/security/api-tokens";
    case Forge::AzureDevOps: return server + L"/_usersSettings/tokens";
    default: return server + L"/settings/tokens/new?scopes=repo,workflow,read:user&description=Mangrove";
    }
}

TokenCheck CheckToken(Forge f, const std::wstring& serverInput, const std::string& tokenInput) {
    TokenCheck r;
    const std::string token(TrimSv(tokenInput));
    std::wstring why;
    const std::wstring server = ServerFor(f, serverInput, &why);
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
    switch (f) {
    case Forge::GitHub: url = server == L"https://github.com" ? "https://api.github.com/user" : base + "/api/v3/user"; break;  // or Enterprise Server
    case Forge::GitLab: url = base + "/api/v4/user"; break;
    case Forge::Gitea: url = base + "/api/v1/user"; break;
    case Forge::Bitbucket: url = "https://api.bitbucket.org/2.0/user"; break;
    case Forge::AzureDevOps: url = base + "/_apis/connectionData?api-version=7.1-preview.1"; break;
    }
    Account a;
    a.forge = f;
    a.server = server;
    const HttpResponse res = HttpGet(url, ApiHeaders(f, token));
    if (res.status == 0) {
        r.error = L"Couldn't reach " + a.Host() + L": " + Widen(res.error);
        return r;
    }
    Json json(res.body);
    if (res.status == 401 || res.status == 403 || res.status == 203) {  // Azure DevOps answers a refused token with its sign-in page
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
    auto text = [](Json& j, std::string_view key) {  // a null is no text
        std::string v = j.Member(key);
        return v == "null" ? std::wstring() : Widen(v);
    };
    switch (f) {
    case Forge::GitLab:
        a.login = text(json, "username");
        a.name = text(json, "name");
        break;
    case Forge::Gitea:
        a.login = text(json, "login");
        a.name = text(json, "full_name");
        break;
    case Forge::Bitbucket:
        a.login = text(json, "username");
        if (a.login.empty()) a.login = text(json, "nickname");
        a.name = text(json, "display_name");
        break;
    case Forge::AzureDevOps: {  // the user, and their sign-in address among the properties
        const std::string userText = json.Member("authenticatedUser");
        Json user(userText);
        a.name = text(user, "customDisplayName");
        if (a.name.empty()) a.name = text(user, "providerDisplayName");
        const std::string props = user.Member("properties");
        Json p(props);
        const std::string accountText = p.Member("Account");
        Json account(accountText);
        a.login = text(account, "$value");
        if (a.login.empty()) a.login = a.name;
        break;
    }
    default:
        a.login = text(json, "login");
        a.name = text(json, "name");
        break;
    }
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
    const std::wstring s = NormalizeServer(server, &why);
    return !ClientId(f).empty() && ((f == Forge::GitHub && s == L"https://github.com") || (f == Forge::GitLab && s == L"https://gitlab.com"));
}

DeviceCode ParseDeviceCode(int status, const std::string& body, const std::wstring& service) {
    DeviceCode d;
    Json json(body);
    const std::string error = json.Member("error");
    if (status != 200 || !error.empty()) {
        d.error = SignInError(status, error, json.Member("error_description"), service);
        return d;
    }
    d.deviceCode = json.Member("device_code");
    d.userCode = Widen(json.Member("user_code"));
    d.verifyUrl = Widen(json.Member("verification_uri"));
    d.interval = std::clamp(ToInt(json.Member("interval"), 5), 1, 60);
    d.expiresIn = std::clamp(ToInt(json.Member("expires_in"), 900), 60, 3600);
    if (d.deviceCode.empty() || d.userCode.empty()) d.error = service + L" gave an unexpected answer.";
    return d;
}

DevicePoll ParseDevicePoll(int status, const std::string& body, const std::wstring& service) {
    DevicePoll p;
    Json json(body);
    const std::string token = json.Member("access_token"), error = json.Member("error");
    if (!token.empty() && error.empty()) {
        p.state = DeviceState::Granted;
        p.token = token;
        p.refreshToken = json.Member("refresh_token");
        p.expiresIn = std::max(0, ToInt(json.Member("expires_in"), 0));
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
        p.error = SignInError(status, error, json.Member("error_description"), service);
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
    const std::wstring service = InfoOf(f).name;
    const bool gitlab = f == Forge::GitLab;
    const std::string base = Narrow(server), client = ClientId(f);
    const std::string codeUrl = gitlab ? base + "/oauth/authorize_device" : "https://github.com/login/device/code";
    const std::string tokenUrl = gitlab ? base + "/oauth/token" : "https://github.com/login/oauth/access_token";
    const std::string scope = gitlab ? "read_user read_api read_repository write_repository" : "repo workflow read:user";
    const std::vector<std::string> headers{ "User-Agent: Mangrove", "Accept: application/json" };
    HttpResponse res = HttpPost(codeUrl, headers, Form({ { "client_id", client }, { "scope", scope } }));
    if (res.status == 0) {
        r.error = L"Couldn't reach " + server.substr(8) + L": " + Widen(res.error);
        return r;
    }
    DeviceCode code = ParseDeviceCode(res.status, res.body, service);
    if (!code.error.empty()) {
        r.error = code.error;
        return r;
    }
    // The page that takes the code is the server's own: anything else isn't opened.
    if (code.verifyUrl.compare(0, server.size() + 1, server + L"/") != 0) code.verifyUrl = server + (gitlab ? L"/oauth/device" : L"/login/device");
    if (onCode) onCode(code);

    const std::string ask = Form({ { "client_id", client }, { "device_code", code.deviceCode }, { "grant_type", "urn:ietf:params:oauth:grant-type:device_code" } });
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
        res = HttpPost(tokenUrl, headers, ask);
        if (cancel) {
            r.error = L"Cancelled.";
            return r;
        }
        if (res.status == 0) continue;  // the network failed for a moment: ask again at the next turn
        const DevicePoll p = ParseDevicePoll(res.status, res.body, service);
        switch (p.state) {
        case DeviceState::Pending: break;
        case DeviceState::SlowDown: interval = std::max(interval + 5, p.interval); break;
        case DeviceState::Denied: r.error = L"Mangrove wasn't authorized on " + service + L"."; return r;
        case DeviceState::Expired: r.error = L"The code expired before it was used: start again."; return r;
        case DeviceState::Failed: r.error = p.error; return r;
        case DeviceState::Granted:
            r = CheckToken(f, server, p.token);
            if (r.ok && token) {
                StoredToken t;
                t.access = p.token;
                if (p.expiresIn > 0 && !p.refreshToken.empty()) {  // it expires: what renews it stays with it
                    t.refresh = p.refreshToken;
                    t.expires = UnixNow() + p.expiresIn;
                    t.client = client;
                }
                *token = t.Text();
            }
            return r;
        }
    }
}

std::vector<std::pair<std::string, std::string>> GitAuthConfig(const Account& a, const std::string& token) {
    const std::string server = Narrow(a.server), host = Narrow(a.Host().substr(0, a.Host().find(L'/')));  // "github.com"
    // The user name sent with the token, as git would after asking for a password: GitLab wants "oauth2" with its
    // OAuth tokens (and takes any with the others), Bitbucket a fixed one with API tokens, Azure DevOps none.
    const std::string user = a.forge == Forge::GitLab      ? std::string("oauth2")
                             : a.forge == Forge::Bitbucket  ? std::string("x-bitbucket-api-token-auth")
                             : a.forge == Forge::AzureDevOps ? std::string()
                                                             : Narrow(a.login);
    const std::string header = "Authorization: Basic " + Base64(user + ":" + std::string(TrimSv(token)));
    std::vector<std::pair<std::string, std::string>> c{ { "http." + server + "/.extraHeader", header } };
    if (a.forge == Forge::AzureDevOps) {  // the organization's old address too; its SSH addresses don't turn into HTTPS ones by their start
        c.push_back({ "http.https://" + server.substr(server.find_last_of('/') + 1) + ".visualstudio.com/.extraHeader", header });
        return c;
    }
    c.push_back({ "url." + server + "/.insteadOf", "git@" + host + ":" });
    c.push_back({ "url." + server + "/.insteadOf", "ssh://git@" + host + "/" });
    if (a.forge == Forge::Bitbucket) {  // and its SSH host of its own
        c.push_back({ "url." + server + "/.insteadOf", "git@ssh." + host + ":" });
        c.push_back({ "url." + server + "/.insteadOf", "ssh://git@ssh." + host + "/" });
    }
    return c;
}

// ---------------------------------------------------------------- the repositories of an account

int64_t ParseIsoTime(std::string_view s) {
    auto num = [&](size_t at, size_t n, int& out) {
        if (at + n > s.size()) return false;
        out = 0;
        for (size_t i = at; i < at + n; ++i) {
            if (s[i] < '0' || s[i] > '9') return false;
            out = out * 10 + (s[i] - '0');
        }
        return true;
    };
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
    if (s.size() < 19 || !num(0, 4, Y) || s[4] != '-' || !num(5, 2, M) || s[7] != '-' || !num(8, 2, D) || (s[10] != 'T' && s[10] != ' ') ||
        !num(11, 2, h) || s[13] != ':' || !num(14, 2, m) || s[16] != ':' || !num(17, 2, sec) || M < 1 || M > 12 || D < 1 || D > 31)
        return 0;
    size_t p = 19;
    if (p < s.size() && s[p] == '.')
        for (++p; p < s.size() && s[p] >= '0' && s[p] <= '9';) ++p;
    int offset = 0;  // seconds east of UTC: "+02:00", "+0200"
    if (p < s.size() && (s[p] == '+' || s[p] == '-')) {
        int oh = 0, om = 0;
        if (num(p + 1, 2, oh)) num(p + 3 < s.size() && s[p + 3] == ':' ? p + 4 : p + 3, 2, om);
        offset = (oh * 60 + om) * 60 * (s[p] == '-' ? -1 : 1);
    }
    // days since 1970-01-01 of a date of the proleptic Gregorian calendar
    const int y = Y - (M <= 2 ? 1 : 0);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = (int64_t)era * 146097 + doe - 719468;
    return days * 86400 + h * 3600 + m * 60 + sec - offset;
}

std::vector<RemoteRepository> ParseRepositories(Forge f, const std::string& body) {
    std::vector<RemoteRepository> out;
    auto text = [](Json& j, std::string_view key) {  // a null is no text
        std::string v = j.Member(key);
        return v == "null" ? std::wstring() : Widen(v);
    };
    // Bitbucket and Azure DevOps answer with an object that holds the list.
    std::string list = body;
    if (f == Forge::Bitbucket || f == Forge::AzureDevOps) {
        Json page(body);
        list = page.Member(f == Forge::Bitbucket ? "values" : "value");
    }
    Json all(list);
    for (const std::string_view e : all.Elements()) {
        Json j(e);
        RemoteRepository r;
        r.name = text(j, "name");
        r.description = text(j, "description");
        switch (f) {
        case Forge::GitLab:
            r.path = text(j, "path_with_namespace");
            r.url = text(j, "http_url_to_repo");
            r.isPrivate = j.Member("visibility") != "public";
            r.updated = ParseIsoTime(j.Member("last_activity_at"));
            break;
        case Forge::Gitea:
            r.path = text(j, "full_name");
            r.url = text(j, "clone_url");
            r.isPrivate = j.Member("private") == "true";
            r.updated = ParseIsoTime(j.Member("updated_at"));
            break;
        case Forge::Bitbucket: {  // the HTTPS one of its clone links
            r.path = text(j, "full_name");
            r.isPrivate = j.Member("is_private") == "true";
            r.updated = ParseIsoTime(j.Member("updated_on"));
            const std::string links = j.Member("links");
            Json l(links);
            const std::string clone = l.Member("clone");
            Json c(clone);
            for (const std::string_view x : c.Elements()) {
                Json link(x);
                if (link.Member("name") == "https") r.url = WithoutUser(text(link, "href"));
            }
            break;
        }
        case Forge::AzureDevOps: {
            if (j.Member("isDisabled") == "true") continue;
            const std::string project = j.Member("project");
            Json p(project);
            r.path = text(p, "name") + L"/" + r.name;
            r.isPrivate = p.Member("visibility") != "public";
            r.url = WithoutUser(text(j, "remoteUrl"));
            break;
        }
        default:  // GitHub
            r.path = text(j, "full_name");
            r.url = text(j, "clone_url");
            r.isPrivate = j.Member("private") == "true";
            r.updated = ParseIsoTime(j.Member("pushed_at"));
            if (!r.updated) r.updated = ParseIsoTime(j.Member("updated_at"));
            break;
        }
        if (!r.url.empty()) out.push_back(std::move(r));
    }
    return out;
}

RepositoryList ListRepositories(const Account& a) {
    RepositoryList out;
    std::string token;
    if (!TokenOf(a, token)) {
        out.error = L"Mangrove has no token for this account: connect it again in Settings.";
        return out;
    }
    const std::string base = Narrow(a.server);
    const std::wstring service = InfoOf(a.forge).name;
    const std::vector<std::string> headers = ApiHeaders(a.forge, token);
    auto failure = [&](const HttpResponse& res) -> std::wstring {
        if (res.status == 0) return L"Couldn't reach " + a.Host() + L": " + Widen(res.error);
        if (res.status == 401 || res.status == 203)
            return service + L" refused the account's token: it may have expired or been revoked. Connect the account again in Settings.";
        if (res.status == 403 && a.forge == Forge::GitLab)
            return L"The token can't list projects: it needs the read_api scope. Make a new one in Settings \u203A Integrations.";
        if (res.status == 403 && a.forge == Forge::Bitbucket)
            return L"The token can't list repositories: it needs the read:workspace and read:repository scopes.";
        Json json(res.body);
        const std::string message = json.Member("message");
        return a.Host() + L" answered with error " + std::to_wstring(res.status) + (message.empty() ? L"." : L": " + Widen(message));
    };
    // The first page of each list; the others follow with &page=N, or (Bitbucket) the "next" address each page gives.
    std::vector<std::string> lists;
    size_t perPage = 100;
    switch (a.forge) {
    case Forge::GitHub:
        lists.push_back((a.server == L"https://github.com" ? std::string("https://api.github.com") : base + "/api/v3") + "/user/repos?sort=pushed&per_page=100");
        break;
    case Forge::GitLab: lists.push_back(base + "/api/v4/projects?membership=true&simple=true&order_by=last_activity_at&per_page=100"); break;
    case Forge::Gitea:
        lists.push_back(base + "/api/v1/user/repos?limit=50");
        perPage = 50;
        break;
    case Forge::AzureDevOps: lists.push_back(base + "/_apis/git/repositories?api-version=7.1"); break;
    case Forge::Bitbucket: {  // no list of all of a user's repositories: those of each of their workspaces
        const HttpResponse res = HttpGet("https://api.bitbucket.org/2.0/user/workspaces?pagelen=100", headers);
        if (res.status != 200) {
            out.error = failure(res);
            return out;
        }
        Json page(res.body);
        const std::string values = page.Member("values");
        Json all(values);
        for (const std::string_view e : all.Elements()) {
            Json item(e);
            const std::string ws = item.Member("workspace");
            Json w(ws);
            const std::string slug = w.Member("slug");
            if (!slug.empty()) lists.push_back("https://api.bitbucket.org/2.0/repositories/" + FormEncode(slug) + "?role=member&pagelen=100&sort=-updated_on");
        }
        break;
    }
    }
    for (const std::string& first : lists) {
        std::string url = first;
        for (int page = 1; page <= 10 && !url.empty() && out.error.empty(); ++page) {
            const HttpResponse res = HttpGet(url, headers);
            if (res.status != 200) {
                out.error = failure(res);
                break;
            }
            std::vector<RemoteRepository> got = ParseRepositories(a.forge, res.body);
            const size_t n = got.size();
            out.repos.insert(out.repos.end(), std::make_move_iterator(got.begin()), std::make_move_iterator(got.end()));
            if (a.forge == Forge::Bitbucket) {
                Json j(res.body);
                url = j.Member("next");
                if (url == "null") url.clear();
            } else if (a.forge == Forge::AzureDevOps || n < perPage) {
                url.clear();
            } else {
                url = first + "&page=" + std::to_string(page + 1);
            }
        }
    }
    std::stable_sort(out.repos.begin(), out.repos.end(), [](const RemoteRepository& x, const RemoteRepository& y) { return x.updated > y.updated; });
    return out;
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

// ---------------------------------------------------------------- what the credential store keeps

std::string StoredToken::Text() const {
    if (refresh.empty()) return access;  // as it was pasted
    return "oauth\n" + access + "\n" + refresh + "\n" + std::to_string(expires) + "\n" + client;
}

StoredToken StoredToken::Read(const std::string& text) {
    StoredToken t;
    if (text.compare(0, 6, "oauth\n") != 0) {
        t.access = std::string(TrimSv(text));
        return t;
    }
    std::vector<std::string> f;
    for (size_t p = 6; p <= text.size();) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        f.push_back(text.substr(p, e - p));
        p = e + 1;
    }
    f.resize(4);
    t.access = f[0];
    t.refresh = f[1];
    t.expires = std::strtoll(f[2].c_str(), nullptr, 10);
    t.client = f[3];
    return t;
}
