#pragma once
#include "base.h"

// Object id (SHA-1; SHA-256 repositories keep their first 20 bytes, which is an unambiguous prefix).
struct Oid {
    uint8_t b[20] = {};
    bool operator==(const Oid& o) const { return memcmp(b, o.b, 20) == 0; }
    bool operator!=(const Oid& o) const { return !(*this == o); }
    bool IsZero() const;
    std::string HexA(int n = 40) const;
    std::wstring Hex(int n = 40) const;
    static bool Parse(const char* s, size_t len, Oid& out);
};
struct OidHash {
    size_t operator()(const Oid& o) const noexcept { uint64_t v; memcpy(&v, o.b, 8); return (size_t)v; }
};

enum : uint8_t { CF_MERGE = 1, CF_STASH = 2 };

struct Commit {
    Oid id;
    uint32_t parentOff = 0;
    uint16_t parentCount = 0;
    uint8_t flags = 0;
    uint8_t pad = 0;
    uint32_t author = 0;
    int64_t authorTime = 0;
    int64_t commitTime = 0;
    uint32_t subjOff = 0, subjLen = 0;   // UTF-8 in RepoData::text
    uint32_t bodyOff = 0, bodyLen = 0;   // first line of the body
    int32_t stash = -1;                  // index in RepoData::stashes for stash nodes
};

struct Author {
    std::wstring name, email, initials;
    uint32_t color = 0;  // 0xRRGGBB avatar background
};

enum class RefKind : uint8_t { Local, Remote, Tag };

struct Ref {
    RefKind kind = RefKind::Local;
    std::wstring full;      // refs/heads/feature/x
    std::wstring name;      // feature/x | origin/feature/x | v1.0
    std::wstring branch;    // feature/x (remote refs without the remote prefix)
    std::wstring remote;    // origin
    Oid target;
    int32_t commit = -1;    // display index, -1 when not loaded
    bool head = false;
    std::wstring upstream;  // origin/feature/x
    int ahead = 0, behind = 0;
    bool gone = false;
};

// A branch or tag shown in the Branch / tag column (local + remote at the same commit are merged).
struct Label {
    std::wstring text;
    int32_t ref = -1;
    int32_t remoteRef = -1;  // the remote branch merged into a local label
    bool local = false, remote = false, tag = false, head = false;
    mutable float width = -1, widthScale = 0;  // measurement cache
};

struct StashEntry {
    int32_t commit = -1;
    std::wstring name;     // stash@{0}
    std::wstring message;
};

struct FileChange {
    wchar_t status = L'M';  // M A D R C T U ?
    std::wstring path, oldPath;
    int32_t added = -1, deleted = -1;  // changed lines (commit files; -1 = unknown or binary)
};

// Lines a commit adds and removes (against its first parent).
struct LineStat {
    int32_t added = 0, deleted = 0;
};

struct WorkStatus {
    bool valid = false;
    std::vector<FileChange> unstaged, staged;
    int modified = 0, added = 0, deleted = 0;
    bool Dirty() const { return !unstaged.empty() || !staged.empty(); }
    int ChangedFiles() const;  // distinct paths, staged or not
};

struct CommitDetails {
    bool ok = false;
    std::wstring error;
    Oid id;
    std::vector<Oid> parents;
    std::wstring author, authorEmail, committer, committerEmail;
    int64_t authorTime = 0, commitTime = 0;
    std::wstring summary, body;
    std::vector<FileChange> files;
    int32_t added = 0, deleted = 0;  // over all files
    double ms = 0;
};

// ---------------------------------------------------------------- file diff
struct DiffLine {
    enum Kind : uint8_t { Context, Added, Deleted, Hunk, Info };
    Kind kind = Context;
    bool noEol = false;                 // the last line of a file that does not end with a newline
    int32_t oldNo = -1, newNo = -1;
    uint32_t off = 0, len = 0;          // text in FileDiff::text (wide characters), without the +/-/space prefix
    int32_t hunk = -1;
    uint32_t hlStart = 0, hlLen = 0;    // changed words inside a modified line (len 0 = none)
    uint32_t rawOff = 0;                // the line in FileDiff::raw, from its prefix (git diffs only)
};

struct DiffHunk {
    int32_t line = -1;                  // header row in FileDiff::lines
    int32_t splitRow = -1;              // header row in FileDiff::split
    int32_t oldStart = 0, oldCount = 0, newStart = 0, newCount = 0;  // from the @@ header
    int added = 0, deleted = 0;
};

struct SplitRow {
    int32_t left = -1, right = -1;      // line indices; header/meta rows have left == right
};

struct FileDiff {
    bool ok = false;
    std::wstring error;
    bool binary = false, newFile = false, deletedFile = false, truncated = false, synthesized = false, combined = false;
    bool multiFile = false;             // git printed more than one file (changes can't be applied back)
    std::wstring oldPath, modeChange;
    std::string raw;                    // git's output
    size_t headerOff = 0, headerLen = 0;  // file header (diff --git ... +++) in raw
    std::wstring text;                  // arena for line text (UTF-16 on Windows, UTF-32 on macOS)
    std::vector<DiffLine> lines;
    std::vector<DiffHunk> hunks;
    std::vector<SplitRow> split;
    int added = 0, deleted = 0;
    size_t maxLen = 0;                  // widest line in columns (horizontal scrolling)
    int maxLineNo = 0;                  // widest line number (gutter)
    double ms = 0;
};

struct DiffRequest {
    int section = 0;                    // 0 file of a commit, 1 unstaged, 2 staged
    Oid commit;
    std::wstring path, oldPath;
    wchar_t status = L'M';
};

// ---------------------------------------------------------------- graph
enum : uint8_t { SEG_PASS = 0, SEG_IN = 1, SEG_OUT = 2 };
struct Seg {
    uint16_t lane;
    uint8_t type;
    uint8_t dashed;
};
struct GraphRow {
    int32_t commit;  // -1 = WIP pseudo-commit
    uint16_t lane;
    uint16_t segCount;
    uint32_t segOff;
};
struct Graph {
    std::vector<GraphRow> rows;
    std::vector<Seg> segs;
    std::vector<int32_t> rowOfCommit;
    int maxLanes = 1;
    bool hasWip = false;
    double ms = 0;
};

// ---------------------------------------------------------------- repository snapshot
struct RepoData {
    std::wstring requested, root, gitDir, commonDir, name;
    uint64_t refsStamp = 0;      // see RefsStamp()
    int64_t fetchTime = 0;       // last fetch (FETCH_HEAD written), unix seconds; 0 = never
    bool headOnly = false;       // only the history of HEAD was loaded
    std::wstring safeDir;        // passed as -c safe.directory when the user trusted the repo
    std::wstring error;          // non-empty when loading failed
    std::wstring untrustedPath;  // set when git refused because of "dubious ownership"

    std::vector<Commit> commits;  // display order (children before parents, newest first)
    std::vector<int32_t> parents; // flattened parent display indices (-1 = not loaded)
    std::string text;             // UTF-8 arena for subjects and body lines
    std::vector<Author> authors;
    std::vector<Ref> refs;
    std::vector<StashEntry> stashes;
    std::vector<std::wstring> submodules;
    std::vector<std::wstring> remotes;
    std::unordered_map<int32_t, std::vector<Label>> labels;

    int32_t headCommit = -1;
    std::wstring headBranch;  // empty when detached or unborn
    Oid headOid;
    bool detached = false, unborn = false;

    double msGit = 0, msBuild = 0, msTotal = 0;

    std::string_view Subject(const Commit& c) const { return std::string_view(text).substr(c.subjOff, c.subjLen); }
    std::string_view Body(const Commit& c) const { return std::string_view(text).substr(c.bodyOff, c.bodyLen); }
    const int32_t* Parents(const Commit& c) const { return parents.data() + c.parentOff; }
};

// headOnly: only the commits reachable from HEAD (plus stashes), instead of every branch and tag.
std::shared_ptr<RepoData> LoadRepository(const std::wstring& path, const std::wstring& safeDir, bool headOnly = false);
// Added and removed lines of each commit (merges are not asked for: they would count a whole branch).
std::vector<std::pair<Oid, LineStat>> LoadLineStats(const std::wstring& root, const std::wstring& safeDir, const std::vector<Oid>& ids);
std::shared_ptr<WorkStatus> LoadWorkStatus(const std::wstring& root, const std::wstring& safeDir);
std::shared_ptr<CommitDetails> LoadCommitDetails(const std::wstring& root, const std::wstring& safeDir, const Oid& id);
Graph BuildGraph(const RepoData& d, bool withWip);
std::wstring GitVersion();               // blocking on first call
std::wstring GitVersionIfReady();        // empty until GitVersion() completed once
// Cheap fingerprint of HEAD, refs, packed-refs and the stash (file timestamps): detects outside changes.
uint64_t RefsStamp(const std::wstring& gitDir, const std::wstring& commonDir);

// ---------------------------------------------------------------- write operations
struct GitOpResult {
    int code = -1;
    std::string out, err;
    bool ok() const { return code == 0; }
};
// Runs a git command that may change the repository, through the git launcher (so hooks, ssh and the
// credential manager behave as in a terminal). Terminal prompts are disabled. `progress` receives each
// progress line git writes on stderr (called on a worker thread).
GitOpResult RunGitOp(const std::wstring& root, const std::wstring& safeDir, const std::wstring& args,
                     const std::function<void(const std::string&)>& progress = nullptr, const std::vector<std::string>& env = {});
std::wstring GitErrorText(const GitOpResult& r);         // readable summary of git's error output
std::wstring CheckBranchName(const std::wstring& name);  // empty when valid (git check-ref-format rules)
std::wstring ReadHeadMessage(const std::wstring& root, const std::wstring& safeDir);  // full message of HEAD, as written
// Diff of one file: of a commit against its first parent, of the working tree, or of the index.
std::shared_ptr<FileDiff> LoadFileDiff(const std::wstring& root, const std::wstring& safeDir, const DiffRequest& req);
// A patch for `git apply` holding only the changed lines of `d` for which `chosen(line index)` is true;
// the other changes are left as they are. `reverse`: the patch will be applied with --reverse (to unstage or
// discard). Empty when nothing is chosen or the diff can't be applied back.
std::string BuildPatch(const FileDiff& d, bool reverse, const std::function<bool(int32_t)>& chosen);
bool CanApplyBack(const FileDiff& d);  // a single text file without conflict, rename or cut
inline bool IsChange(const DiffLine& L) { return L.kind == DiffLine::Added || L.kind == DiffLine::Deleted; }
// A file that was only added or only deleted has one side: it stays in a single column.
inline bool ShowSplit(bool preferred, const FileDiff& d) { return preferred && !d.newFile && !d.deletedFile; }
int SplitRowOfLine(const FileDiff& d, int line);  // the split view row that shows a line
std::wstring ReadMergeMessage(const std::wstring& gitDir);  // prepared message of a merge in progress (no comments), or empty
