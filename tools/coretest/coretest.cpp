// Tests of the shared core (src/core) that run the same on Windows and on macOS: text and paths, command
// lines, the trees of the navigation pane, the texts both apps show, the settings file, and a real
// repository made in the temp folder and read through git.
//
//   coretest        (run.ps1 / run.sh build and run it)
#include "git.h"
#include "navtree.h"
#include "platform.h"
#include "present.h"
#include "settings.h"
#include "text.h"
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
    CHECK(BaseName(Native(L"/work/dream-tree")) == L"dream-tree");
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
    _wputenv_s(L"DREAMTREE_SETTINGS", file.c_str());
#else
    setenv("DREAMTREE_SETTINGS", Narrow(file).c_str(), 1);
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

static int Run() {
    TestText();
    TestTrees();
    TestTexts();
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / ("dreamtree-coretest-" + std::to_string(UnixNow()));
    std::filesystem::create_directories(dir, ec);
    TestSettings(dir);
    TestRepository(dir);
    std::filesystem::remove_all(dir, ec);
    std::printf("coretest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}

#ifdef _WIN32
int wmain() { return Run(); }
#else
int main() { return Run(); }
#endif
