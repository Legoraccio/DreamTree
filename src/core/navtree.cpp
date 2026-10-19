#include "navtree.h"
#include "platform.h"

namespace {

TreeNode* Folder(TreeNode& parent, std::wstring_view name) {
    for (TreeNode& c : parent.children)
        if (c.IsFolder() && c.name == name) return &c;
    parent.children.push_back({});
    parent.children.back().name = std::wstring(name);
    return &parent.children.back();
}

int Finish(TreeNode& n, bool joinSingle) {
    std::stable_sort(n.children.begin(), n.children.end(), [](const TreeNode& a, const TreeNode& b) {
        if (a.IsFolder() != b.IsFolder()) return a.IsFolder();
        return CompareNatural(a.name, b.name) < 0;
    });
    int leaves = 0;
    for (TreeNode& c : n.children) {
        if (!c.IsFolder()) {
            ++leaves;
            continue;
        }
        leaves += Finish(c, joinSingle);
        if (joinSingle)
            while (c.children.size() == 1 && c.children[0].IsFolder()) {  // "src" > "app" becomes "src/app"
                TreeNode only = std::move(c.children[0]);
                c.name += L"/" + only.name;
                c.children = std::move(only.children);
            }
    }
    if (n.IsFolder()) n.count = leaves;
    return leaves;
}

}  // namespace

TreeNode BuildPathTree(const std::vector<std::pair<std::wstring, int32_t>>& items, bool joinSingle) {
    TreeNode root;
    for (const auto& [path, index] : items) {
        TreeNode* n = &root;
        size_t p = 0;
        for (;;) {
            const size_t s = path.find(L'/', p);
            if (s == std::wstring::npos) break;
            n = Folder(*n, std::wstring_view(path).substr(p, s - p));
            p = s + 1;
        }
        TreeNode leaf;
        leaf.name = path.substr(p);
        leaf.index = index;
        n->children.push_back(std::move(leaf));
    }
    Finish(root, joinSingle);
    return root;
}

static TreeNode RefTree(const RepoData& d, RefKind kind) {
    std::vector<std::pair<std::wstring, int32_t>> items;
    for (size_t i = 0; i < d.refs.size(); ++i)
        if (d.refs[i].kind == kind) items.emplace_back(d.refs[i].name, (int32_t)i);
    return BuildPathTree(items);
}

TreeNode LocalBranchTree(const RepoData& d) { return RefTree(d, RefKind::Local); }
TreeNode TagTree(const RepoData& d) { return RefTree(d, RefKind::Tag); }

TreeNode RemoteTree(const RepoData& d) {
    TreeNode root;
    std::vector<std::wstring> order;
    std::unordered_map<std::wstring, std::vector<std::pair<std::wstring, int32_t>>> byRemote;
    for (size_t i = 0; i < d.refs.size(); ++i) {
        const Ref& r = d.refs[i];
        if (r.kind != RefKind::Remote) continue;
        auto [it, added] = byRemote.try_emplace(r.remote);
        if (added) order.push_back(r.remote);
        it->second.emplace_back(r.branch, (int32_t)i);
    }
    for (const std::wstring& name : d.remotes)  // remotes with nothing fetched yet
        if (byRemote.try_emplace(name).second) order.push_back(name);
    for (const std::wstring& name : order) {
        TreeNode n = BuildPathTree(byRemote[name]);
        n.name = name;
        root.children.push_back(std::move(n));
    }
    root.count = (int)order.size();
    return root;
}

std::vector<int> RecentBranches(const RepoData& d, size_t max) {
    std::vector<int> refs;
    for (size_t i = 0; i < d.refs.size(); ++i)
        if (d.refs[i].kind == RefKind::Local && d.refs[i].commit >= 0) refs.push_back((int)i);
    std::sort(refs.begin(), refs.end(), [&](int a, int b) {
        const Ref& ra = d.refs[(size_t)a];
        const Ref& rb = d.refs[(size_t)b];
        if (ra.head != rb.head) return ra.head;
        return d.commits[(size_t)ra.commit].commitTime > d.commits[(size_t)rb.commit].commitTime;
    });
    if (refs.size() > max) refs.resize(max);
    return refs;
}

std::wstring RefNote(const RepoData& d, const Label& L) {
    if (L.tag) return L"tag";
    const std::wstring head = L.head ? L"checked out \u00B7 " : L"";
    if (L.local && L.remote) {
        const std::wstring remote = L.remoteRef >= 0 ? d.refs[(size_t)L.remoteRef].remote : L"the remote";
        return head + L"here and on " + remote;
    }
    if (L.local) {  // its upstream, if any, is at another commit
        const Ref& r = d.refs[(size_t)L.ref];
        if (r.upstream.empty()) return head + L"local only";
        if (r.gone) return head + L"upstream deleted";
        std::wstring s;
        if (r.ahead) s = std::to_wstring(r.ahead) + L" to push";
        if (r.behind) s += (s.empty() ? L"" : L", ") + std::to_wstring(r.behind) + L" to pull";
        return head + (s.empty() ? L"tracks " + r.upstream : s);
    }
    return L"on " + d.refs[(size_t)L.ref].remote + L" only";
}

TreeNode FileTree(const std::vector<FileChange>& files) {
    std::vector<std::pair<std::wstring, int32_t>> items;
    items.reserve(files.size());
    for (size_t i = 0; i < files.size(); ++i) items.emplace_back(files[i].path, (int32_t)i);
    return BuildPathTree(items, true);
}
