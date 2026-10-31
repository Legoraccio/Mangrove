#pragma once
#include "accounts.h"

// Cloning, shared by the apps: the repositories of the connected accounts, the address and the folder of a clone,
// and the clone itself. The apps show a CloneModel and hand it what the user types and picks; it tells them when
// something changed (onChange, on the UI thread).

std::wstring CloneName(const std::wstring& address);  // the folder a clone gets: "repo" for ".../repo.git"
bool IsGitAddress(const std::wstring& text);          // https://, ssh://, git://, file:// or user@host:path
std::wstring DefaultCloneFolder();  // the folder of the last clone, else the one of the recent repositories, else home

class CloneModel : public std::enable_shared_from_this<CloneModel> {
public:
    std::function<void()> onChange;

    // ---- the repositories of each account, listed on worker threads
    struct Source {
        Account account;
        std::vector<RemoteRepository> repos;
        std::wstring error;
        bool loading = true;
    };
    std::vector<Source> sources;  // in the accounts' order
    int listed = 0;               // grows each time an account's list arrives (or fails): the apps refill theirs
    void Load();
    bool Matches(const RemoteRepository& r) const;  // the words of `text` are all in its path or description

    // ---- what is cloned, and where
    std::wstring text;    // the field: an address, or words that filter the repositories
    std::wstring picked;  // the address of the repository picked in the list
    std::wstring folder;  // where the clone goes (DefaultCloneFolder at first)
    std::wstring Address() const;  // the text when it is an address, else the picked repository's
    std::wstring Target() const;   // folder + the name of the address; empty without an address
    std::wstring Check() const;    // why it can't be cloned now; empty when it can

    // ---- the clone
    bool running = false;
    std::wstring progress;  // git's last progress line
    std::wstring error;     // why the last clone failed
    std::wstring cloned;    // the new repository's folder, once a clone succeeded
    bool Start();           // false, with the reason in error, when Check refuses
};
