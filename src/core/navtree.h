#pragma once
#include "git.h"

// Trees for the navigation pane and the file lists: names are split at '/' into folders, folders come first,
// then leaves, each in natural order ("v2" before "v10").
struct TreeNode {
    std::wstring name;   // a path segment; with joinSingle, a chain of single-child folders ("src/app/ui")
    int32_t index = -1;  // leaves: the item's index (ref, file...); folders: -1
    int count = 0;       // folders: leaves below
    std::vector<TreeNode> children;
    bool IsFolder() const { return index < 0; }
};

// items: (path, index). joinSingle merges folders that only hold another folder.
TreeNode BuildPathTree(const std::vector<std::pair<std::wstring, int32_t>>& items, bool joinSingle = false);

TreeNode LocalBranchTree(const RepoData& d);
TreeNode TagTree(const RepoData& d);
// One child per remote (index = -1, name = remote), each with its branches below.
TreeNode RemoteTree(const RepoData& d);
// The checked-out branch first, then the local branches whose last commit is most recent.
std::vector<int> RecentBranches(const RepoData& d, size_t max);
TreeNode FileTree(const std::vector<FileChange>& files);

// How a branch or tag of a commit relates to its remote: "checked out \u00B7 here and on origin", "local only",
// "1 to push, 2 to pull", "on origin only", "tag".
std::wstring RefNote(const RepoData& d, const Label& L);
