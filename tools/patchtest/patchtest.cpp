// Regression test for BuildPatch (src/core/git.cpp): stages, unstages or discards hunks, single lines and
// random selections of lines with real git, then checks that exactly the chosen changes moved, at the right
// lines. It uses only the shared core, so it builds on Windows and on macOS.
//
//   patchtest <repository> <file> stage|unstage|discard <random cases> [seed]
//
// The repository must be a throwaway one: run.ps1 creates it. Each case is undone before the next one.
#include "git.h"
#include "platform.h"
#include "text.h"
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <random>
#include <set>

static std::wstring g_root;

static GitOpResult G(const std::wstring& args) { return RunGitOp(g_root, L"", L"-c core.quotepath=false " + args); }

struct Change {
    std::string text;
    int no;
    bool operator<(const Change& o) const { return text != o.text ? text < o.text : no < o.no; }
    bool operator==(const Change& o) const { return text == o.text && no == o.no; }
};
struct Changes {
    std::multiset<Change> del, add;  // del: old line numbers, add: new line numbers
};

// A line that only gained or lost its final line break shows as the same text removed and added: drop such pairs.
static void Cancel(Changes& c) {
    for (auto it = c.add.begin(); it != c.add.end();) {
        auto d = std::find_if(c.del.begin(), c.del.end(), [&](const Change& x) { return x.text == it->text; });
        if (d != c.del.end()) {
            c.del.erase(d);
            it = c.add.erase(it);
        } else {
            ++it;
        }
    }
}

static Changes ParseDiffOutput(const std::string& out) {
    Changes c;
    int o = 0, n = 0;
    bool in = false;
    for (size_t p = 0; p < out.size();) {
        size_t e = out.find('\n', p);
        if (e == std::string::npos) e = out.size();
        std::string line = out.substr(p, e - p);
        p = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("diff --git", 0) == 0) {
            in = false;
            continue;
        }
        if (line.rfind("@@", 0) == 0) {
            int a = 0, b = 0;  // "@@ -a[,n] +b[,m] @@"
            if (line.size() > 4) a = (int)std::strtol(line.c_str() + 4, nullptr, 10);
            if (const size_t plus = line.find(" +"); plus != std::string::npos) b = (int)std::strtol(line.c_str() + plus + 2, nullptr, 10);
            o = a;
            n = b;
            in = true;
            continue;
        }
        if (!in) continue;
        if (line.empty() || line[0] == ' ') {
            ++o;
            ++n;
        } else if (line[0] == '-') {
            c.del.insert({ line.substr(1), o++ });
        } else if (line[0] == '+') {
            c.add.insert({ line.substr(1), n++ });
        }
    }
    Cancel(c);
    return c;
}

static std::multiset<std::string> Texts(const std::multiset<Change>& s) {
    std::multiset<std::string> t;
    for (const Change& c : s) t.insert(c.text);
    return t;
}

static std::string Show(const std::multiset<Change>& s) {
    std::string r;
    for (const Change& c : s) r += "  [" + std::to_string(c.no) + "] " + c.text + "\n";
    return r;
}

static int Run(const std::vector<std::wstring>& argv) {
    if (argv.size() < 5) {
        fprintf(stderr, "usage: patchtest <repository> <file> stage|unstage|discard <random cases> [seed]\n");
        return 2;
    }
    g_root = argv[1];
    const std::wstring path = argv[2], mode = argv[3];
    const int randomCases = (int)std::wcstol(argv[4].c_str(), nullptr, 10);
    const unsigned seed = argv.size() > 5 ? (unsigned)std::wcstol(argv[5].c_str(), nullptr, 10) : 1u;
    const bool stage = mode == L"stage", unstage = mode == L"unstage", discard = mode == L"discard";
    if (!stage && !unstage && !discard) return 2;

    DiffRequest req;
    req.section = unstage ? 2 : 1;
    req.path = path;
    const std::shared_ptr<FileDiff> d = LoadFileDiff(g_root, L"", req);
    if (!d->ok || !CanApplyBack(*d)) {
        printf("%s %s: the diff can't be applied back\n", Narrow(mode).c_str(), Narrow(path).c_str());
        return 1;
    }
    std::vector<int32_t> changed;
    for (int32_t i = 0; i < (int32_t)d->lines.size(); ++i)
        if (d->lines[(size_t)i].kind == DiffLine::Added || d->lines[(size_t)i].kind == DiffLine::Deleted) changed.push_back(i);

    // Cases: every hunk, every line, random selections, everything.
    std::vector<std::vector<uint8_t>> cases;
    for (size_t h = 0; h < d->hunks.size(); ++h) {
        std::vector<uint8_t> s(d->lines.size(), 0);
        for (int32_t i : changed) s[(size_t)i] = d->lines[(size_t)i].hunk == (int32_t)h;
        cases.push_back(s);
    }
    for (int32_t i : changed) {
        std::vector<uint8_t> s(d->lines.size(), 0);
        s[(size_t)i] = 1;
        cases.push_back(s);
    }
    std::mt19937 rng(seed);
    for (int k = 0; k < randomCases; ++k) {
        std::vector<uint8_t> s(d->lines.size(), 0);
        bool any = false;
        for (int32_t i : changed) any |= (s[(size_t)i] = (uint8_t)(rng() & 1)) != 0;
        if (!any) s[(size_t)changed[rng() % changed.size()]] = 1;
        cases.push_back(s);
    }
    cases.emplace_back(d->lines.size(), 0);
    for (int32_t i : changed) cases.back()[(size_t)i] = 1;

    const std::wstring indexPath = PathJoin(g_root, L".git/index");
    const std::wstring file = PathJoin(g_root, path);
    std::string fileBak, indexBak;
    const bool fileExisted = ReadWholeFile(file, fileBak);
    ReadWholeFile(indexPath, indexBak);

    int failures = 0;
    const std::wstring diffArgs = L"--no-color --no-ext-diff --no-renames -U3 -- " + QuoteArg(path);
    for (size_t ci = 0; ci < cases.size(); ++ci) {
        const std::vector<uint8_t>& sel = cases[ci];
        bool all = true;
        for (int32_t i : changed) all &= sel[(size_t)i] != 0;
        const std::string patch = BuildPatch(*d, !stage, [&](int32_t i) { return sel[(size_t)i] != 0; });
        Changes chosen, rest;
        for (int32_t i : changed) {
            const DiffLine& L = d->lines[(size_t)i];
            const std::string text = Narrow(std::wstring_view(d->text).substr(L.off, L.len));
            Changes& c = sel[(size_t)i] ? chosen : rest;
            if (L.kind == DiffLine::Deleted) c.del.insert({ text, L.oldNo });
            else c.add.insert({ text, L.newNo });
        }
        Cancel(chosen);
        Cancel(rest);

        const std::wstring tmp = WriteTempFile(patch);
        const GitOpResult r = G(std::wstring(L"apply ") + (discard ? L"--reverse" : unstage ? L"--cached --reverse" : L"--cached") +
                                L" --recount --whitespace=nowarn " + QuoteArg(tmp));
        RemoveFile(tmp);
        std::string problem;
        if (!r.ok()) {
            problem = "git apply failed: " + r.err;
        } else {
            const Changes staged = ParseDiffOutput(G(L"diff --cached " + diffArgs).out);
            const Changes unstaged = ParseDiffOutput(G(L"diff " + diffArgs).out);
            // Line numbers are compared on the side that keeps its original numbering, text on the other.
            auto expect = [&](const char* what, const std::multiset<Change>& got, const std::multiset<Change>& want, bool numbers) {
                if (numbers ? got != want : Texts(got) != Texts(want))
                    problem += std::string(what) + " differ:\n got:\n" + Show(got) + " expected:\n" + Show(want);
            };
            if (stage) {
                expect("staged deletions", staged.del, chosen.del, true);
                expect("staged additions", staged.add, chosen.add, false);
                if (!(all && d->deletedFile)) {
                    expect("unstaged additions", unstaged.add, rest.add, true);
                    expect("unstaged deletions", unstaged.del, rest.del, false);
                }
            } else if (unstage) {
                expect("staged deletions", staged.del, rest.del, true);
                expect("staged additions", staged.add, rest.add, false);
                if (!(all && d->newFile)) {  // a new file unstaged entirely is untracked, which git diff does not show
                    expect("unstaged additions", unstaged.add, chosen.add, true);
                    expect("unstaged deletions", unstaged.del, chosen.del, false);
                }
            } else {
                expect("staged changes", staged.del, {}, false);
                expect("unstaged deletions", unstaged.del, rest.del, true);
                expect("unstaged additions", unstaged.add, rest.add, false);
            }
        }
        WriteFileAtomic(indexPath, indexBak);
        if (discard) {
            if (fileExisted) WriteFileAtomic(file, fileBak);
            else RemoveFile(file);
        }
        if (!problem.empty()) {
            ++failures;
            printf("---- case %zu FAILED\n%s---- patch:\n%s\n", ci, problem.c_str(), patch.c_str());
            if (failures >= 5) break;
        }
    }
    printf("%-8s %-6s %zu hunks, %zu changed lines: %zu cases, %d failed\n", Narrow(mode).c_str(), Narrow(path).c_str(), d->hunks.size(),
           changed.size(), cases.size(), failures);
    return failures ? 1 : 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) { return Run(std::vector<std::wstring>(argv, argv + argc)); }
#else
int main(int argc, char** argv) {
    std::vector<std::wstring> args;
    for (int i = 0; i < argc; ++i) args.push_back(Widen(argv[i]));
    return Run(args);
}
#endif
