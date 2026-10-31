#include "clone.h"
#include "git.h"
#include "platform.h"
#include "settings.h"
#include "text.h"
#include <cwchar>

std::wstring CloneName(const std::wstring& address) {
    std::wstring s = TrimW(address);
    while (!s.empty() && (s.back() == L'/' || s.back() == L'\\')) s.pop_back();
    const size_t cut = s.find_last_of(L"/\\:");
    std::wstring name = cut == std::wstring::npos ? s : s.substr(cut + 1);
    if (name.size() > 4 && ToLower(name.substr(name.size() - 4)) == L".git") name.resize(name.size() - 4);
    std::wstring out;
    for (const wchar_t c : name)
        if (c >= 32 && !wcschr(L"<>:\"/\\|?*", c)) out.push_back(c);
    while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) out.pop_back();  // Windows drops them
    return out;
}

bool IsGitAddress(const std::wstring& input) {
    const std::wstring t = TrimW(input);
    if (t.empty() || t.find_first_of(L" \t") != std::wstring::npos) return false;
    if (t.find(L"://") != std::wstring::npos) return true;
    const size_t at = t.find(L'@'), colon = t.find(L':'), slash = t.find(L'/');  // user@host:path, a ':' before any '/'
    return at != std::wstring::npos && colon != std::wstring::npos && at < colon && (slash == std::wstring::npos || colon < slash);
}

std::wstring DefaultCloneFolder() {
    const Settings& s = AppSettings();
    const std::wstring last = s.Get(L"cloneFolder");
    if (!last.empty() && DirExists(last)) return last;
    for (const std::wstring& repo : s.recent) {
        const size_t cut = repo.find_last_of(kPathSep);
        if (cut == std::wstring::npos || cut == 0) continue;
        std::wstring parent = repo.substr(0, cut);
        if (parent.size() == 2 && parent[1] == L':') parent += kPathSep;  // a drive's root
        if (DirExists(parent)) return parent;
    }
    return EnvVar(kPathSep == L'\\' ? "USERPROFILE" : "HOME");
}

// ---------------------------------------------------------------- the model

void CloneModel::Load() {
    sources.clear();
    std::weak_ptr<CloneModel> wk = shared_from_this();
    for (const Account& a : LoadAccounts()) {
        sources.push_back({ a, {}, L"", true });
        const size_t i = sources.size() - 1;
        std::thread([wk, a, i] {
            RepositoryList list = ListRepositories(a);
            RunOnMain([wk, i, key = a.Key(), list = std::move(list)]() mutable {
                auto self = wk.lock();
                if (!self || i >= self->sources.size() || self->sources[i].account.Key() != key) return;
                Source& s = self->sources[i];
                s.repos = std::move(list.repos);
                s.error = std::move(list.error);
                s.loading = false;
                ++self->listed;
                if (self->onChange) self->onChange();
            });
        }).detach();
    }
}

bool CloneModel::Matches(const RemoteRepository& r) const {
    const std::wstring t = TrimW(text);
    if (IsGitAddress(t)) return false;  // an address is what gets cloned
    for (size_t p = 0; p < t.size();) {
        const size_t e = std::min(t.find(L' ', p), t.size());
        const std::wstring word = t.substr(p, e - p);
        if (!word.empty() && !ContainsI(r.path, word) && !ContainsI(r.description, word)) return false;
        p = e + 1;
    }
    return true;
}

std::wstring CloneModel::Address() const { return IsGitAddress(text) ? TrimW(text) : picked; }

std::wstring CloneModel::Target() const {
    const std::wstring address = Address(), name = address.empty() ? L"" : CloneName(address);
    return name.empty() || folder.empty() ? std::wstring() : PathJoin(folder, name);
}

std::wstring CloneModel::Check() const {
    if (running) return L"A clone is running.";
    if (Address().empty()) return TrimW(text).empty() ? L"Paste the address of a repository, or pick one of yours." : L"Pick a repository, or paste its address.";
    if (folder.empty() || !DirExists(folder)) return L"Choose a folder that exists for the clone.";
    const std::wstring target = Target();
    if (target.empty()) return L"This address doesn't name a repository.";
    if (FileExists(target)) return L"A file named \u201C" + BaseName(target) + L"\u201D is already in this folder.";
    if (DirExists(target)) {
        bool empty = true;
        ListDir(target, [&](std::wstring_view, uint64_t, bool) { empty = false; });
        if (!empty) return L"\u201C" + BaseName(target) + L"\u201D is already in this folder: choose another folder.";
    }
    return L"";
}

bool CloneModel::Start() {
    error = Check();
    if (!error.empty()) {
        if (onChange) onChange();
        return false;
    }
    const std::wstring address = Address(), target = Target(), parent = folder;
    const std::vector<Account> accounts = LoadAccounts();
    running = true;
    progress.clear();
    cloned.clear();
    std::weak_ptr<CloneModel> wk = shared_from_this();
    auto lastPost = std::make_shared<std::atomic<double>>(0.0);
    std::thread([wk, address, target, parent, accounts, lastPost] {
        auto onProgress = [wk, lastPost](const std::string& line) {
            const double now = NowMs();
            if (now - lastPost->load() < 50) return;  // ~20 updates per second are plenty
            lastPost->store(now);
            RunOnMain([wk, w = Widen(line)] {
                if (auto self = wk.lock(); self && self->running) {
                    self->progress = w;
                    if (self->onChange) self->onChange();
                }
            });
        };
        const GitOpResult r = RunGitOp(parent, L"", L"clone --progress -- " + QuoteArg(address) + L" " + QuoteArg(target), onProgress,
                                       GitAuthEnvironment(accounts));
        const std::wstring why = r.ok() ? std::wstring() : GitErrorText(r);  // a server that asks who you are: the accounts
        RunOnMain([wk, ok = r.ok(), why, target, parent] {
            auto self = wk.lock();
            if (!self) return;
            self->running = false;
            self->progress.clear();
            if (ok) {
                self->cloned = target;
                AppSettings().Set(L"cloneFolder", parent);
                AppSettings().Save();
            } else {
                self->error = why;
            }
            if (self->onChange) self->onChange();
        });
    }).detach();
    if (onChange) onChange();
    return true;
}
