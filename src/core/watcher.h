#pragma once
#include "base.h"

// Watches a working tree (recursively) and calls onChange, debounced, after files settle. Changes inside
// .git/objects are ignored: they always come together with ref or index updates. onChange runs on a
// background thread. Implemented per platform (ReadDirectoryChangesW on Windows, FSEvents on macOS, inotify on
// Linux).
class RepoWatcher {
public:
    RepoWatcher(const std::wstring& root, std::function<void()> onChange);
    ~RepoWatcher();
    RepoWatcher(const RepoWatcher&) = delete;
    RepoWatcher& operator=(const RepoWatcher&) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
};
