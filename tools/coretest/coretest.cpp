// Tests of the shared core (src/core) that run the same on Windows and on macOS: text and paths, command
// lines, the trees of the navigation pane, the texts both apps show, the settings file, and a real
// repository made in the temp folder and read through git.
//
//   coretest        (run.ps1 / run.sh build and run it)
#include "accounts.h"
#include "clone.h"
#include "git.h"
#include "navtree.h"
#include "platform.h"
#include "present.h"
#include "settings.h"
#include "text.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

static int g_checks = 0, g_failed = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failed;                                                            \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                          \
    } while (0)

static std::wstring Native(const std::wstring& path) {
    std::wstring s = path;
    for (wchar_t& c : s)
        if (c == L'/') c = kPathSep;
    return s;
}

// ---------------------------------------------------------------- text, paths, command lines

static void TestText() {
    const std::string mixed = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";  // a, e acute, euro sign, an emoji
    CHECK(Narrow(Widen(mixed)) == mixed);
    CHECK(Widen("\xF0\x9F\x98\x80").size() == (sizeof(wchar_t) == 2 ? 2u : 1u));  // UTF-16 pair or one UTF-32 unit
    CHECK(Widen("\xFF") == std::wstring(1, (wchar_t)0xFFFD));                      // invalid input
    std::wstring w = L"x";
    WidenAppend("yz", w);
    CHECK(w == L"xyz");

    CHECK(TrimW(L"  x y \n") == L"x y");
    CHECK(TrimSv("\t a \r\n") == "a");
    CHECK(FormatCount(1234567) == L"1.234.567");
    CHECK(Plural(1, L"file", L"files") == L"1 file");
    CHECK(Plural(1200, L"file", L"files") == L"1.200 files");
    CHECK(FormatRelative(UnixNow() - 30) == L"just now" || !FormatRelative(UnixNow() - 30).empty());

    CHECK(CompareNatural(L"v2", L"v10") < 0);
    CHECK(CompareNatural(L"feature/b", L"feature/a") > 0);
    CHECK(ContainsI(L"Fix the Parser", L"parser"));
    CHECK(!ContainsI(L"Fix the Parser", L"lexer"));
    CHECK(ToLower(L"ABC") == L"abc");
    CHECK(ToUpper(L"àb") == L"ÀB");

    for (const std::wstring& a : { std::wstring(L"simple"), std::wstring(L"with space"), std::wstring(L"quote\"inside"),
                                   std::wstring(L"trailing\\"), std::wstring(L"back\\\\slash \"q\""), std::wstring() }) {
        const std::vector<std::wstring> back = SplitCommandLine(L"git " + QuoteArg(a) + L" end");
        CHECK(back.size() == 3 && back[1] == a && back[2] == L"end");
    }

    CHECK(PathJoin(Native(L"/repo"), L"src/app.cpp") == Native(L"/repo/src/app.cpp"));
    CHECK(BaseName(Native(L"/work/mangrove-app")) == L"mangrove-app");
    CHECK(NormalizePath(Native(L"/work/repo/")) == Native(L"/work/repo"));
}

// ---------------------------------------------------------------- trees

static void TestTrees() {
    const TreeNode t = BuildPathTree({ { L"main", 0 }, { L"feature/b", 1 }, { L"feature/a", 2 }, { L"v10", 3 }, { L"v2", 4 } });
    CHECK(t.children.size() == 4);
    if (t.children.size() == 4) {
        CHECK(t.children[0].IsFolder() && t.children[0].name == L"feature" && t.children[0].count == 2);  // folders first
        CHECK(t.children[0].children.size() == 2 && t.children[0].children[0].name == L"a");
        CHECK(t.children[1].name == L"main" && t.children[2].name == L"v2" && t.children[3].name == L"v10");  // natural order
    }
    const TreeNode j = BuildPathTree({ { L"src/app/main.cpp", 0 }, { L"README.md", 1 } }, true);
    CHECK(j.children.size() == 2 && j.children[0].name == L"src/app" && j.children[0].children.size() == 1);
}

// ---------------------------------------------------------------- shared texts

static void TestTexts() {
    CHECK(StatusLetter(L'?') == L'U');
    CHECK(StatusLetter(L'U') == L'C');
    CHECK(StatusLetter(L'M') == L'M');
    CHECK(LineDelta(3, 1) == L"+3 −1");
    CHECK(LineDelta(0, 2) == L"−2");
    CHECK(LineDelta(0, 0).empty() && LineDelta(-1, -1).empty());
    CHECK(CommitButtonLabel(2, false) == L"Commit 2 files");
    CHECK(CommitButtonLabel(0, false) == L"Stage files to commit");
    CHECK(CommitButtonLabel(1, true) == L"Amend previous commit with 1 file");
    StashEntry s;
    s.name = L"stash@{0}";
    s.message = L"On main: wip";
    const Prompt p = DropStashPrompt(s);
    CHECK(p.buttons.size() == 2 && p.danger == 0 && p.message.find(L"stash@{0}") != std::wstring::npos);
    CHECK(PushRejectedPrompt().buttons.size() == 3);
}

// ---------------------------------------------------------------- settings

static void TestSettings(const std::filesystem::path& dir) {
    const std::wstring file = (dir / L"settings.ini").wstring();
#ifdef _WIN32
    _wputenv_s(L"MANGROVE_SETTINGS", file.c_str());
#else
    setenv("MANGROVE_SETTINGS", Narrow(file).c_str(), 1);
#endif
    Settings a;
    a.pullMode = 2;
    a.diffSplit = true;
    a.recent = { Native(L"/work/one"), Native(L"/work/two") };
    a.tabs = { Native(L"/work/two") };
    a.activeTab = 0;
    a.Set(L"custom", L"value = with equals");
    a.SetInt(L"number", 42);
    CHECK(a.Save());
    Settings b;
    b.Load();
    CHECK(b.pullMode == 2 && b.diffSplit);
    CHECK(b.recent == a.recent && b.tabs == a.tabs);
    CHECK(b.Get(L"custom") == L"value = with equals" && b.GetInt(L"number", 0) == 42);
    b.AddRecent(Native(L"/work/two"));
    CHECK(b.recent.size() == 2 && b.recent[0] == Native(L"/work/two"));
}

// ---------------------------------------------------------------- accounts

static void TestAccounts(const std::filesystem::path& dir) {
    CHECK(Base64("") == "" && Base64("M") == "TQ==" && Base64("Ma") == "TWE=" && Base64("Man") == "TWFu");
    CHECK(Base64("octocat:ghp_x") == "b2N0b2NhdDpnaHBfeA==");
    std::wstring why;
    CHECK(NormalizeServer(L" gitlab.example.com/ ", &why) == L"https://gitlab.example.com");
    CHECK(NormalizeServer(L"https://GitHub.com", &why) == L"https://github.com");
    CHECK(NormalizeServer(L"https://example.com/git/Lab/", &why) == L"https://example.com/git/Lab");
    CHECK(NormalizeServer(L"http://example.com", &why).empty() && !why.empty());
    CHECK(NormalizeServer(L"", &why).empty());
    CHECK(TokenPageUrl(Forge::GitHub, L"https://github.com").rfind(L"https://github.com/settings/tokens/new?", 0) == 0);
    CHECK(TokenPageUrl(Forge::GitLab, L"https://gitlab.com").find(L"scopes=read_user,read_api,read_repository,write_repository") != std::wstring::npos);
    const TokenCheck bad = CheckToken(Forge::GitHub, L"http://github.com", "x");
    CHECK(!bad.ok && !bad.error.empty());
    // Bitbucket is bitbucket.org; Azure DevOps needs an organization (the server is never asked: the address is wrong)
    CHECK(CheckToken(Forge::Bitbucket, L"https://example.com", "x").error.find(L"bitbucket.org") != std::wstring::npos);
    CHECK(CheckToken(Forge::AzureDevOps, L"https://dev.azure.com/", "x").error.find(L"organization") != std::wstring::npos);
    CHECK(CheckToken(Forge::AzureDevOps, L"https://example.com/org", "x").error.find(L"Azure DevOps") != std::wstring::npos);
    CHECK(TokenPageUrl(Forge::Bitbucket, L"https://bitbucket.org").rfind(L"https://id.atlassian.com/", 0) == 0);
    CHECK(Forges().size() == 5 && InfoOf(Forge::AzureDevOps).name == std::wstring(L"Azure DevOps"));

    // What the credential store keeps: a pasted token as it is, one that expires with what renews it.
    CHECK(StoredToken::Read(" glpat-x \n").access == "glpat-x" && StoredToken::Read("ghp_y").Text() == "ghp_y");
    StoredToken st;
    st.access = "a1";
    st.refresh = "r1";
    st.expires = 1793443529;
    st.client = "c1";
    const StoredToken back = StoredToken::Read(st.Text());
    CHECK(back.access == "a1" && back.refresh == "r1" && back.expires == 1793443529 && back.client == "c1");

    // Signing in with the browser: the answers of the device flow (RFC 8628, as GitHub sends them).
    CHECK(!BrowserSignInAvailable(Forge::GitLab, L"https://gitlab.example.com") && !BrowserSignInAvailable(Forge::GitHub, L"https://github.example.com"));
    CHECK(!BrowserSignInAvailable(Forge::Bitbucket, L"https://bitbucket.org") && !BrowserSignInAvailable(Forge::AzureDevOps, L"https://dev.azure.com/org"));
    const DevicePoll gitlab = ParseDevicePoll(200, R"({"access_token":"t0k","token_type":"Bearer","expires_in":7200,"refresh_token":"r3f","scope":"read_api","created_at":1793443529})", L"GitLab");
    CHECK(gitlab.state == DeviceState::Granted && gitlab.token == "t0k" && gitlab.refreshToken == "r3f" && gitlab.expiresIn == 7200);
    CHECK(ParseDevicePoll(400, R"({"error":"invalid_client"})", L"GitLab").error.find(L"GitLab") == 0);
    // gitlab.com's answers to Mangrove's app (2026-10-31): the code, then "not yet" with status 400.
    const DeviceCode glCode = ParseDeviceCode(200, R"({"device_code":"Hm8dR2xq0YbS","user_code":"SV96G9BW","verification_uri":"https://gitlab.com/oauth/device",
        "verification_uri_complete":"https://gitlab.com/oauth/device?user_code=SV96G9BW","expires_in":300,"interval":5})", L"GitLab");
    CHECK(glCode.error.empty() && glCode.userCode == L"SV96G9BW" && glCode.verifyUrl == L"https://gitlab.com/oauth/device" && glCode.expiresIn == 300);
    CHECK(ParseDevicePoll(400, R"({"error":"authorization_pending","error_description":"The authorization server encountered an unexpected condition which prevented it from fulfilling the request."})",
                          L"GitLab").state == DeviceState::Pending);
    const DeviceCode code = ParseDeviceCode(200, R"({"device_code":"3584d83530557fdd1f46af8289938c8ef79f9dc5","user_code":"WDJB-MJHT",
        "verification_uri":"https:\/\/github.com\/login\/device","expires_in":900,"interval":5})");
    CHECK(code.error.empty() && code.deviceCode == "3584d83530557fdd1f46af8289938c8ef79f9dc5" && code.userCode == L"WDJB-MJHT" &&
          code.verifyUrl == L"https://github.com/login/device" && code.expiresIn == 900 && code.interval == 5);
    CHECK(!ParseDeviceCode(200, R"({"error":"device_flow_disabled","error_description":"Device Flow must be explicitly enabled"})").error.empty());
    CHECK(!ParseDeviceCode(404, "Not Found").error.empty());
    CHECK(!ParseDeviceCode(200, "{}").error.empty());
    const DevicePoll granted = ParseDevicePoll(200, R"({"access_token":"gho_16C7e42F292c6912E7710c838347Ae178B4a","token_type":"bearer","scope":"repo,workflow,read:user"})");
    CHECK(granted.state == DeviceState::Granted && granted.token == "gho_16C7e42F292c6912E7710c838347Ae178B4a");
    CHECK(ParseDevicePoll(200, R"({"error":"authorization_pending","error_description":"..."})").state == DeviceState::Pending);
    const DevicePoll slow = ParseDevicePoll(200, R"({"error":"slow_down","error_description":"Too many requests.","interval":10})");
    CHECK(slow.state == DeviceState::SlowDown && slow.interval == 10);
    CHECK(ParseDevicePoll(400, R"({"error":"access_denied"})").state == DeviceState::Denied);
    CHECK(ParseDevicePoll(400, R"({"error":"expired_token"})").state == DeviceState::Expired);
    const DevicePoll failed = ParseDevicePoll(200, R"({"error":"incorrect_device_code"})");
    CHECK(failed.state == DeviceState::Failed && !failed.error.empty());
    CHECK(ParseDevicePoll(502, "<html>Bad gateway</html>").state == DeviceState::Failed);

    // The account list lives in the settings, the tokens don't.
    Settings& s = AppSettings();
    const std::vector<std::wstring> before = s.accounts;
    s.accounts = { L"gitlab\thttps://gitlab.example.com\tada\tAda Lovelace", L"nonsense", L"github\thttps://github.com\toctocat\t" };
    const std::vector<Account> list = LoadAccounts();
    CHECK(list.size() == 2);
    if (list.size() == 2) {
        CHECK(list[0].forge == Forge::GitLab && list[0].server == L"https://gitlab.example.com" && list[0].login == L"ada" &&
              list[0].name == L"Ada Lovelace");
        CHECK(list[1].forge == Forge::GitHub && list[1].Host() == L"github.com" && list[1].name.empty());
    }
    s.accounts = before;

    // git reads the authorization from the environment, for the server's addresses only.
    const std::wstring root = (dir / L"auth").wstring();
    std::filesystem::create_directories(root);
    const std::vector<std::string> env = { "GIT_CONFIG_COUNT=1", "GIT_CONFIG_KEY_0=http.https://example.com/.extraHeader",
                                           "GIT_CONFIG_VALUE_0=Authorization: Basic b2N0b2NhdDpnaHBfeA==" };
    const GitOpResult on = RunGitOp(root, L"", L"config --get-urlmatch http.extraheader https://example.com/team/repo.git", nullptr, env);
    CHECK(on.ok() && std::string(TrimSv(on.out)) == "Authorization: Basic b2N0b2NhdDpnaHBfeA==");
    const GitOpResult off = RunGitOp(root, L"", L"config --get-urlmatch http.extraheader https://other.example.org/repo.git", nullptr, env);
    CHECK(!off.ok() && TrimSv(off.out).empty());

    // An account's configuration: SSH addresses of its server become HTTPS ones, so they get the token too.
    const Account gh{ Forge::GitHub, L"https://github.com", L"octocat", L"" };
    std::vector<std::string> ghEnv;
    const auto config = GitAuthConfig(gh, "ghp_x");
    for (size_t i = 0; i < config.size(); ++i) {
        ghEnv.push_back("GIT_CONFIG_KEY_" + std::to_string(i) + "=" + config[i].first);
        ghEnv.push_back("GIT_CONFIG_VALUE_" + std::to_string(i) + "=" + config[i].second);
    }
    ghEnv.insert(ghEnv.begin(), "GIT_CONFIG_COUNT=" + std::to_string(config.size()));
    CHECK(RunGitOp(root, L"", L"init -q").ok());
    CHECK(RunGitOp(root, L"", L"remote add scp git@github.com:octo/hello.git").ok());
    CHECK(RunGitOp(root, L"", L"remote add ssh ssh://git@github.com/octo/hello.git").ok());
    CHECK(RunGitOp(root, L"", L"remote add other git@gitlab.com:octo/hello.git").ok());
    auto url = [&](const wchar_t* remote) { return std::string(TrimSv(RunGitOp(root, L"", std::wstring(L"remote get-url ") + remote, nullptr, ghEnv).out)); };
    CHECK(url(L"scp") == "https://github.com/octo/hello.git");
    CHECK(url(L"ssh") == "https://github.com/octo/hello.git");
    CHECK(url(L"other") == "git@gitlab.com:octo/hello.git");
    // GitLab takes "oauth2" with its OAuth tokens, Bitbucket a fixed user name and its own SSH host, Azure DevOps no user name
    const auto gl = GitAuthConfig({ Forge::GitLab, L"https://gitlab.com", L"ada", L"" }, "t");
    CHECK(gl.size() == 3 && gl[0].second == "Authorization: Basic " + Base64("oauth2:t"));
    const auto bb = GitAuthConfig({ Forge::Bitbucket, L"https://bitbucket.org", L"ada", L"" }, "t");
    CHECK(bb.size() == 5 && bb[0].second == "Authorization: Basic " + Base64("x-bitbucket-api-token-auth:t") && bb[3].second == "git@ssh.bitbucket.org:");
    const auto az = GitAuthConfig({ Forge::AzureDevOps, L"https://dev.azure.com/fabrikam", L"ada@example.com", L"" }, "t");
    CHECK(az.size() == 2 && az[0].first == "http.https://dev.azure.com/fabrikam/.extraHeader" && az[0].second == "Authorization: Basic " + Base64(":t") &&
          az[1].first == "http.https://fabrikam.visualstudio.com/.extraHeader");
    const GitOpResult header = RunGitOp(root, L"", L"config --get-urlmatch http.extraheader https://github.com/octo/hello.git", nullptr, ghEnv);
    CHECK(std::string(TrimSv(header.out)) == "Authorization: Basic " + Base64("octocat:ghp_x"));

    // SSH without a key: what to do instead of git's words.
    GitOpResult denied;
    denied.code = 128;
    denied.err = "git@github.com: Permission denied (publickey).\r\nfatal: Could not read from remote repository.\n";
    const std::wstring deniedText = GitErrorText(denied);
    CHECK(deniedText.rfind(L"github.com refused the SSH connection", 0) == 0 && deniedText.find(L"Integrations") != std::wstring::npos);

    // The credential store: Windows Credential Manager; on Linux the Secret Service when the run has one
    // (MANGROVE_TEST_SECRETS=1, as CI does); not the macOS keychain, which needs a logged-in user CI may not have.
#if defined(_WIN32) || defined(__linux__)
#ifdef __linux__
    if (EnvVar("MANGROVE_TEST_SECRETS").empty()) return;
#endif
    const Account a{ Forge::Gitea, L"https://git.example.com", L"tester", L"" };
    const std::wstring key = L"coretest:" + std::to_wstring(UnixNow());
    CHECK(SecretSave(key, "s3cret", L"Mangrove core test"));
    std::string got;
    CHECK(SecretLoad(key, got) && got == "s3cret");
    SecretDelete(key);
    CHECK(!SecretLoad(key, got));
    std::wstring err;
    CHECK(AddAccount(a, "tok3n", &err));
    const std::vector<std::string> authEnv = GitAuthEnvironment({ a });
    CHECK(authEnv.size() == 7 && authEnv[0] == "GIT_CONFIG_COUNT=3" && authEnv[1] == "GIT_CONFIG_KEY_0=http.https://git.example.com/.extraHeader" &&
          authEnv[2] == "GIT_CONFIG_VALUE_0=Authorization: Basic " + Base64("tester:tok3n") &&
          authEnv[3] == "GIT_CONFIG_KEY_1=url.https://git.example.com/.insteadOf" && authEnv[4] == "GIT_CONFIG_VALUE_1=git@git.example.com:" &&
          authEnv[6] == "GIT_CONFIG_VALUE_2=ssh://git@git.example.com/");
    RemoveAccount(a);
    CHECK(GitAuthEnvironment({ a }).empty());
    CHECK(LoadAccounts().size() == before.size());
#endif
}

// ---------------------------------------------------------------- a real repository

static bool Git(const std::wstring& root, const std::wstring& args) {
    const GitOpResult r = RunGitOp(root, L"", L"-c user.name=Test -c user.email=test@example.com -c core.autocrlf=false " + args);
    if (!r.ok()) std::printf("git %s failed: %s\n", Narrow(args).c_str(), r.err.c_str());
    return r.ok();
}

static void Put(const std::wstring& root, const wchar_t* name, const std::string& text) { WriteFileAtomic(PathJoin(root, name), text); }

static void TestRepository(const std::filesystem::path& dir) {
    const std::wstring root = (dir / L"repo").wstring();
    std::filesystem::create_directories(root);
    CHECK(!GitVersion().empty());
    if (!Git(root, L"init -q -b main")) return;
    Put(root, L"a.txt", "one\ntwo\nthree\n");
    Put(root, L"b.txt", "bee\n");
    Git(root, L"add -A");
    Git(root, L"commit -qm \"First commit\"");
    Git(root, L"tag v1.0");
    Put(root, L"a.txt", "one\n2\nthree\nfour\n");
    Git(root, L"commit -qam \"Second commit\" -m \"With a body.\"");
    Git(root, L"switch -q -c feature/x");
    Put(root, L"c.txt", "see\n");
    Git(root, L"add c.txt");
    Git(root, L"commit -qm \"On a branch\"");
    Git(root, L"switch -q main");
    Put(root, L"b.txt", "bee\nbuzz\n");  // uncommitted
    Put(root, L"new.txt", "fresh\n");     // untracked

    const std::shared_ptr<RepoData> d = LoadRepository(root, L"");
    CHECK(d && d->error.empty());
    if (!d || !d->error.empty()) return;
    CHECK(d->commits.size() == 3);
    CHECK(d->headBranch == L"main" && !d->detached && !d->unborn);
    int locals = 0, tags = 0;
    for (const Ref& r : d->refs) {
        locals += r.kind == RefKind::Local;
        tags += r.kind == RefKind::Tag;
    }
    CHECK(locals == 2 && tags == 1);
    CHECK(d->headCommit >= 0 && Widen(d->Subject(d->commits[(size_t)d->headCommit])) == L"Second commit");
    CHECK(HeadRef(*d) && HeadRef(*d)->name == L"main");
    const TreeNode branches = LocalBranchTree(*d);
    CHECK(branches.children.size() == 2 && branches.children[0].name == L"feature");
    CHECK(HistorySubtitle(*d).rfind(L"main · 3 commits", 0) == 0);

    const std::shared_ptr<WorkStatus> st = LoadWorkStatus(root, L"");
    CHECK(st && st->valid && st->Dirty());
    if (st) {
        CHECK(st->unstaged.size() == 2 && st->staged.empty() && st->ChangedFiles() == 2);
        CHECK(WipSummary(*d, st.get()) == L"2 unstaged · on main");
    }
    const Graph g = BuildGraph(*d, st && st->Dirty());
    CHECK(g.hasWip && g.rows.size() == 4 && g.rows[0].commit == -1);

    const std::shared_ptr<CommitDetails> det = LoadCommitDetails(root, L"", d->commits[(size_t)d->headCommit].id);
    CHECK(det && det->ok);
    if (det && det->ok) {
        CHECK(det->summary == L"Second commit" && det->body == L"With a body.");
        CHECK(det->files.size() == 1 && det->files[0].path == L"a.txt" && det->added == 2 && det->deleted == 1);
        CHECK(FilesSummary(*det) == L"1 file · +2 −1");
    }

    DiffRequest req;
    req.section = 1;
    req.path = L"b.txt";
    const std::shared_ptr<FileDiff> diff = LoadFileDiff(root, L"", req);
    CHECK(diff && diff->ok && diff->added == 1 && diff->deleted == 0 && diff->hunks.size() == 1);
    if (diff && diff->ok) CHECK(CanApplyBack(*diff) && !BuildPatch(*diff, false, [](int32_t) { return true; }).empty());

    const std::vector<std::pair<Oid, LineStat>> stats = LoadLineStats(root, L"", { d->commits[(size_t)d->headCommit].id });
    CHECK(stats.size() == 1 && stats[0].second.added == 2 && stats[0].second.deleted == 1);
}

// ---------------------------------------------------------------- operations of a session

// The session's "UI thread" is the test's own: what the core posts waits here until Pump runs it.
static std::mutex g_postedMutex;
static std::deque<std::function<void()>> g_posted;

static bool Pump(const std::function<bool()>& done) {
    const double until = NowMs() + 30000;
    while (!done()) {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lk(g_postedMutex);
            if (!g_posted.empty()) {
                fn = std::move(g_posted.front());
                g_posted.pop_front();
            }
        }
        if (fn) fn();
        else if (NowMs() > until) return false;
        else std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

static const FileChange* Unstaged(const WorkStatus& st, const std::wstring& path) {
    for (const FileChange& f : st.unstaged)
        if (f.path == path) return &f;
    return nullptr;
}

// Tags, deleting branches (here and on a remote) with undo, and discarding a submodule and a nested repository.
static void TestSession(const std::filesystem::path& dir) {
    SetMainDispatcher([](std::function<void()> fn) {
        std::lock_guard<std::mutex> lk(g_postedMutex);
        g_posted.push_back(std::move(fn));
    });
    const std::wstring origin = (dir / L"origin.git").wstring(), lib = (dir / L"lib").wstring(), root = (dir / L"work").wstring();
    for (const std::wstring& p : { origin, lib, root }) std::filesystem::create_directories(p);
    if (!Git(origin, L"init -q --bare") || !Git(lib, L"init -q -b main") || !Git(root, L"init -q -b main")) return;
    Put(lib, L"lib.txt", "one\n");
    Git(lib, L"add -A");
    Git(lib, L"commit -qm \"Library\"");
    Put(lib, L"lib.txt", "two\n");
    Git(lib, L"commit -qam \"Library, again\"");
    Put(root, L"a.txt", "a\n");
    Git(root, L"add -A");
    Git(root, L"commit -qm \"First\"");
    Git(root, L"-c protocol.file.allow=always submodule add -q ../lib lib");
    Git(root, L"commit -qm \"Add the library\"");
    Git(root, L"remote add origin " + QuoteArg(origin));
    Git(root, L"push -q -u origin main");
    Git(root, L"switch -q -c feature");
    Put(root, L"f.txt", "feature\n");
    Git(root, L"add f.txt");
    Git(root, L"commit -qm \"Feature work\"");
    Git(root, L"push -q -u origin feature");
    Git(root, L"switch -q -c local-only main");
    Git(root, L"commit -q --allow-empty -m \"Not pushed\"");
    Git(root, L"switch -q main");

    auto s = std::make_shared<RepoSession>();
    s->path = root;
    auto idle = [&] { return !s->loading && !s->opBusy && s->opQueue.empty(); };
    s->StartLoad(false);
    CHECK(Pump(idle) && s->data);
    if (!s->data) return;

    // a tag, undone and done again
    CHECK(!s->CheckNewTag(L"").empty() && !s->CheckNewTag(L"v1 0").empty() && s->CheckNewTag(L"v1.0").empty());
    CHECK(s->CreateTag(L"v1.0", s->data->headCommit).empty() && Pump(idle));
    const int tag = FindRef(*s->data, RefKind::Tag, L"v1.0");
    CHECK(tag >= 0 && s->data->refs[(size_t)tag].commit == s->data->headCommit);
    CHECK(s->CheckNewTag(L"v1.0").find(L"already exists") != std::wstring::npos);
    CHECK(!s->undo.empty() && s->undo.back().kind == UndoEntry::Kind::CreateTag);
    s->Undo();
    CHECK(Pump(idle) && FindRef(*s->data, RefKind::Tag, L"v1.0") < 0);
    s->Redo();
    CHECK(Pump(idle) && FindRef(*s->data, RefKind::Tag, L"v1.0") >= 0);

    // the branches that go together, and the commits only they have
    {
        const RepoData& d = *s->data;
        const int mainRef = FindRef(d, RefKind::Local, L"main"), feature = FindRef(d, RefKind::Local, L"feature");
        const int originFeature = FindRef(d, RefKind::Remote, L"origin/feature"), localOnly = FindRef(d, RefKind::Local, L"local-only");
        CHECK(mainRef >= 0 && feature >= 0 && originFeature >= 0 && localOnly >= 0);
        CHECK(s->PairOf(feature).remote == originFeature && s->PairOf(originFeature).local == feature && s->PairOf(localOnly).remote < 0);
        CHECK(s->CommitsOnlyOn({ feature }) == 0 && s->CommitsOnlyOn({ feature, originFeature }) == 1 && s->CommitsOnlyOn({ localOnly }) == 1);
        const std::vector<DeleteChoice> mainChoices = DeleteChoices(*s, mainRef);
        CHECK(mainChoices.size() == 3 && !mainChoices[0].enabled && mainChoices[1].enabled && !mainChoices[2].enabled);
        CHECK(DeleteChoices(*s, localOnly).size() == 1 && DeleteChoices(*s, FindRef(d, RefKind::Tag, L"v1.0")).empty());
        CHECK(DeleteBranchPrompt(*s, L"local-only", L"").message.find(L"1 commit is on no other branch") != std::wstring::npos);
        CHECK(DeleteBranchPrompt(*s, L"feature", L"origin/feature").message.find(L"can't be undone") != std::wstring::npos);
        s->DeleteBranch(L"main", L"");  // checked out: refused before anything runs
        CHECK(!s->opBusy && s->notice.error);
        s->DeleteBranch(L"", L"origin/feature");
    }
    CHECK(Pump(idle));
    CHECK(FindRef(*s->data, RefKind::Remote, L"origin/feature") < 0 && FindRef(*s->data, RefKind::Local, L"feature") >= 0);
    CHECK(!RunGitOp(origin, L"", L"rev-parse -q --verify refs/heads/feature").ok());
    const int feature = FindRef(*s->data, RefKind::Local, L"feature");
    const std::wstring featureSha = feature >= 0 ? s->data->refs[(size_t)feature].target.Hex() : L"";
    s->DeleteBranch(L"feature", L"");
    CHECK(Pump(idle) && FindRef(*s->data, RefKind::Local, L"feature") < 0);
    s->Undo();
    CHECK(Pump(idle));
    const int back = FindRef(*s->data, RefKind::Local, L"feature");
    CHECK(back >= 0 && s->data->refs[(size_t)back].target.Hex() == featureSha);

    // a submodule that checked out another commit goes back to the one recorded for it
    const std::wstring sub = PathJoin(root, L"lib");
    const std::wstring recorded = Widen(TrimSv(RunGitOp(sub, L"", L"rev-parse HEAD").out));
    Git(sub, L"checkout -q HEAD~1");
    std::shared_ptr<WorkStatus> st = LoadWorkStatus(root, L"");
    const FileChange* moved = Unstaged(*st, L"lib");
    CHECK(moved && moved->submodule && moved->status == L'M' && !Unstaged(*st, L"a.txt"));
    if (moved) {
        CHECK(DiscardPrompt(*moved).message.find(L"submodule") != std::wstring::npos);
        s->Discard(*moved);
        CHECK(Pump(idle));
    }
    CHECK(Widen(TrimSv(RunGitOp(sub, L"", L"rev-parse HEAD").out)) == recorded);
    // a repository of its own inside the working tree is deleted
    const std::wstring nested = PathJoin(root, L"nested");
    std::filesystem::create_directories(nested);
    Git(nested, L"init -q");
    Put(nested, L"n.txt", "n\n");
    st = LoadWorkStatus(root, L"");
    const FileChange* repo = Unstaged(*st, L"nested/");
    CHECK(repo && repo->NestedRepository() && !repo->submodule);
    if (repo) {
        s->Discard(*repo);
        CHECK(Pump(idle));
    }
    CHECK(!DirExists(nested));
    st = LoadWorkStatus(root, L"");
    CHECK(st->valid && !st->Dirty());

    s.reset();
    const double until = NowMs() + 500;
    Pump([&] { return NowMs() > until; });  // what is still posted finds the session gone
}

// ---------------------------------------------------------------- cloning

static void TestClone(const std::filesystem::path& dir) {
    CHECK(CloneName(L"https://github.com/octocat/hello-world.git") == L"hello-world");
    CHECK(CloneName(L"git@gitlab.com:group/sub/project.git") == L"project" && CloneName(L"git@host:solo") == L"solo");
    CHECK(CloneName(L"https://dev.azure.com/org/project/_git/repo") == L"repo" && CloneName(L"https://example.com/a/b/") == L"b");
    CHECK(IsGitAddress(L" https://github.com/o/r ") && IsGitAddress(L"git@github.com:o/r.git") && IsGitAddress(L"ssh://git@host/o/r"));
    CHECK(!IsGitAddress(L"hello world") && !IsGitAddress(L"octocat") && !IsGitAddress(L"a/b:c@d"));
    CHECK(ParseIsoTime("1970-01-01T00:00:00Z") == 0 && ParseIsoTime("2026-10-31T10:45:29Z") == 1793443529);
    CHECK(ParseIsoTime("2026-10-31T12:45:29.123+02:00") == 1793443529 && ParseIsoTime("2026-10-31T10:45:29.5Z") == 1793443529);
    CHECK(ParseIsoTime("yesterday") == 0 && ParseIsoTime("") == 0);

    const std::vector<RemoteRepository> gh = ParseRepositories(Forge::GitHub, R"([
        {"id":1,"name":"hello-world","full_name":"octocat/hello-world","private":false,"owner":{"login":"octocat"},
         "description":"My first repository","clone_url":"https://github.com/octocat/hello-world.git","pushed_at":"2026-10-30T12:00:00Z"},
        {"name":"secret","full_name":"octocat/secret","private":true,"description":null,"clone_url":"https://github.com/octocat/secret.git",
         "pushed_at":null,"updated_at":"2026-01-02T03:04:05Z"}])");
    CHECK(gh.size() == 2 && gh[0].path == L"octocat/hello-world" && gh[0].url == L"https://github.com/octocat/hello-world.git" &&
          !gh[0].isPrivate && gh[0].description == L"My first repository" && gh[0].updated == ParseIsoTime("2026-10-30T12:00:00Z"));
    CHECK(gh.size() == 2 && gh[1].isPrivate && gh[1].description.empty() && gh[1].updated == ParseIsoTime("2026-01-02T03:04:05Z"));
    const std::vector<RemoteRepository> gl = ParseRepositories(Forge::GitLab, R"([{"id":7,"name":"Project","path_with_namespace":"group/project",
        "http_url_to_repo":"https://gitlab.com/group/project.git","visibility":"internal","description":"","last_activity_at":"2026-10-29T08:00:00.000Z"}])");
    CHECK(gl.size() == 1 && gl[0].path == L"group/project" && gl[0].isPrivate && gl[0].url == L"https://gitlab.com/group/project.git");
    const std::vector<RemoteRepository> gt = ParseRepositories(Forge::Gitea, R"([{"name":"r","full_name":"ada/r","private":false,
        "clone_url":"https://codeberg.org/ada/r.git","updated_at":"2026-10-28T10:00:00+02:00"}])");
    CHECK(gt.size() == 1 && gt[0].path == L"ada/r" && gt[0].updated == ParseIsoTime("2026-10-28T08:00:00Z"));
    CHECK(ParseRepositories(Forge::GitHub, "{\"message\":\"Bad credentials\"}").empty() && ParseRepositories(Forge::GitHub, "[]").empty());
    const std::vector<RemoteRepository> bb = ParseRepositories(Forge::Bitbucket, R"({"pagelen":100,"values":[{"name":"Repo","full_name":"team/repo",
        "is_private":true,"description":"","updated_on":"2026-10-27T09:00:00.123456+00:00","links":{"html":{"href":"https://bitbucket.org/team/repo"},
        "clone":[{"name":"https","href":"https://ada@bitbucket.org/team/repo.git"},{"name":"ssh","href":"git@bitbucket.org:team/repo.git"}]}}],
        "next":"https://api.bitbucket.org/2.0/repositories/team?page=2"})");
    CHECK(bb.size() == 1 && bb[0].path == L"team/repo" && bb[0].url == L"https://bitbucket.org/team/repo.git" && bb[0].isPrivate &&
          bb[0].updated == ParseIsoTime("2026-10-27T09:00:00Z"));
    const std::vector<RemoteRepository> az = ParseRepositories(Forge::AzureDevOps, R"({"count":2,"value":[
        {"id":"1","name":"Web","project":{"name":"Fabrikam","visibility":"private"},"remoteUrl":"https://fabrikam@dev.azure.com/fabrikam/Fabrikam/_git/Web","isDisabled":false},
        {"id":"2","name":"Old","project":{"name":"Fabrikam"},"remoteUrl":"https://dev.azure.com/fabrikam/Fabrikam/_git/Old","isDisabled":true}]})");
    CHECK(az.size() == 1 && az[0].path == L"Fabrikam/Web" && az[0].url == L"https://dev.azure.com/fabrikam/Fabrikam/_git/Web" && az[0].isPrivate);

    // a clone of a repository on this disk, as the apps make it
    SetMainDispatcher([](std::function<void()> fn) {
        std::lock_guard<std::mutex> lk(g_postedMutex);
        g_posted.push_back(std::move(fn));
    });
    const std::wstring source = (dir / L"source").wstring(), into = (dir / L"clones").wstring();
    std::filesystem::create_directories(source);
    std::filesystem::create_directories(into);
    if (!Git(source, L"init -q -b main")) return;
    Put(source, L"r.txt", "readme\n");
    Git(source, L"add -A");
    Git(source, L"commit -qm \"Start\"");
    std::wstring url = L"file://" + std::wstring(source[0] == L'/' ? L"" : L"/") + source;
    for (wchar_t& c : url) if (c == L'\\') c = L'/';
    auto m = std::make_shared<CloneModel>();
    m->folder = into;
    CHECK(!m->Check().empty());  // no address yet
    m->text = L"hello";
    m->picked.clear();
    CHECK(m->Address().empty() && !m->Check().empty());
    m->text = url;
    CHECK(m->Address() == url && m->Target() == PathJoin(into, L"source") && m->Check().empty());
    CHECK(m->Start() && m->running);
    CHECK(Pump([&] { return !m->running; }));
    CHECK(m->error.empty() && m->cloned == PathJoin(into, L"source") && FileExists(PathJoin(m->cloned, L"r.txt")));
    CHECK(m->Check().find(L"already in this folder") != std::wstring::npos);  // the same clone again
}

static int Run() {
    TestText();
    TestTrees();
    TestTexts();
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / ("mangrove-coretest-" + std::to_string(UnixNow()));
    std::filesystem::create_directories(dir, ec);
    TestSettings(dir);
    TestAccounts(dir);
    TestRepository(dir);
    TestSession(dir);
    TestClone(dir);
    std::filesystem::remove_all(dir, ec);
    std::printf("coretest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}

#ifdef _WIN32
int wmain() { return Run(); }
#else
int main() { return Run(); }
#endif
