#pragma once
#include "accounts.h"
#include "git.h"
#include "watcher.h"

// RepoSession: everything Mangrove knows and does for one open repository, shared by the Windows, the macOS
// and the Linux app. Loading and live refresh, the selection and its details, search, line counts, the git
// operations with their undo/redo stacks, and the diff of one file with its line selection.
//
// Methods are called on the UI thread; work runs on background threads and comes back to the UI thread
// (RunOnMain) where the session updates itself and tells the app through onEvent. The app decides how to
// show it and asks the user before destructive actions (Discard, DropStash, Revert, discarding patches).

// ---------------------------------------------------------------- operations

// An action Mangrove performed that can be reverted. HEAD positions are "b:<branch>" or "d:<sha>".
struct UndoEntry {
    enum class Kind { Checkout, CreateBranch, Stash, Commit, Revert } kind = Kind::Checkout;
    std::wstring label;        // e.g. "checkout main"
    std::wstring from, to;     // HEAD before / after
    std::wstring branch, sha;  // created branch and the commit it was created at
    std::wstring stash;        // stash commit created by the action
    std::wstring redoArgs;     // git arguments that perform the action again
};

// Context handed to a git operation running on a worker thread.
struct OpCtx {
    std::wstring root, safe;
    std::function<void(const std::string&)> progress;
    std::vector<Account> accounts;  // as when the operation started
    GitOpResult Run(const std::wstring& args) const { return RunGitOp(root, safe, args, progress); }
    // fetch, pull, push: HTTPS requests to the accounts' servers carry their tokens
    GitOpResult RunNet(const std::wstring& args) const { return RunGitOp(root, safe, args, progress, GitAuthEnvironment(accounts)); }
};

struct OpOutcome {
    bool ok = false;
    bool quiet = false;         // no notification (a follow-up explains the result)
    bool statusOnly = false;    // only the working tree changed: refresh the status, not the history
    bool pushRejected = false;  // the remote has commits the branch doesn't: the app offers pull or force push
    std::wstring title, message;
    bool hasUndo = false;
    UndoEntry undo;                // pushed on the undo stack when ok
    std::function<void()> then;    // runs on the UI thread once the operation finished
};

// Actions the apps enable and explain (command bar, toolbar, menus).
enum Action : int { ActUndo = 0, ActRedo, ActPull, ActPush, ActBranch, ActStash, ActPop, ActTerminal, ActFetch };
inline constexpr int kCommitOp = 100;  // the operation id of commit and amend (the commit button shows its progress)

enum class SessionEvent : uint8_t {
    Loading,       // a load started (spinners)
    Loaded,        // data and graph were replaced, or loading failed (error is set); see lastLoadRefresh
    Status,        // the working tree status changed; see wipRowDelta
    Details,       // the details of the selected commit changed (arrived or cleared)
    Diff,          // the diff was opened, loaded, reloaded or closed, or its line selection changed
    Search,        // search results changed
    LineStats,     // more line counts arrived
    Operation,     // an operation started, reported progress or ended
    Notice,        // a message for the user: notice
    Message,       // a commit message to offer: suggestion
    Committed,     // a commit or amend succeeded: the app clears its message fields
    PushRejected,  // push was rejected: offer to pull or to force push
};

struct Notice {
    std::wstring title, message;
    bool error = false;
};

// A commit message to put in the (empty) message fields: the one being amended, or a merge's prepared one.
struct MessageSuggestion {
    std::wstring summary, body;
    bool amend = false;
};

// ---------------------------------------------------------------- diff

struct DiffState {
    bool open = false;
    DiffRequest req;
    std::shared_ptr<FileDiff> diff;  // null while the first load runs
    uint64_t seq = 0;                // last load started
    uint64_t diffSeq = 0;            // load that produced `diff`
    uint64_t hunkOpSeq = 0;          // a hunk was (un)staged after this load: its lines are stale until a newer one
    double loadStart = 0;
    // Changed lines selected for line actions (uncommitted changes): flags by line index, empty when none.
    std::vector<uint8_t> sel;
    int32_t selAnchor = -1, selCount = 0;
    bool HunkBusy() const { return hunkOpSeq && diffSeq <= hunkOpSeq; }
    bool Selected(int32_t line) const { return line >= 0 && (size_t)line < sel.size() && sel[(size_t)line]; }
};

// A patch made from the open diff, applied to the index (stage, or unstage) or backwards to the working
// tree (discard: the app asks first).
struct PatchJob {
    std::string patch;  // empty: nothing to do
    bool discard = false, unstage = false;
    std::wstring what;  // "hunk", "line" or "lines"
};

// ---------------------------------------------------------------- session

class RepoSession : public std::enable_shared_from_this<RepoSession> {
public:
    virtual ~RepoSession() = default;
    std::function<void(SessionEvent)> onEvent;

    // ---- the repository
    std::wstring path, title;
    std::shared_ptr<RepoData> data;
    std::shared_ptr<Graph> graph;
    std::shared_ptr<WorkStatus> status;
    bool loading = false, refreshing = false;
    bool lastLoadRefresh = false;  // the last load kept the state of the previous one (a refresh)
    int wipRowDelta = 0;           // last Status: +1 the uncommitted changes row appeared, -1 it went away
    uint64_t seq = 0;
    std::wstring error, untrusted;
    double loadMs = 0, loadStart = 0;
    uint64_t refsStamp = 0;
    bool headOnly = false;  // only the history of the current branch
    bool IsStart() const { return path.empty(); }
    void StartLoad(bool refresh);
    void CheckForChanges();  // reloads when refs moved, else re-reads the working tree
    void Trust();            // trusts the folder git refused (untrusted) and loads again

    // ---- selection: a commit (display index) or the uncommitted changes
    int32_t selCommit = -1;
    bool selWip = false;
    int32_t SelectedRow() const;
    void Select(int32_t commit, bool wip);  // closes the diff, loads the details
    void SelectRow(int32_t row);

    // ---- details of the selected commit
    std::shared_ptr<CommitDetails> details;
    std::unordered_map<Oid, std::shared_ptr<CommitDetails>, OidHash> cache;
    bool inflight = false;
    void RequestDetails();

    // ---- search: commits whose message, author, id or branch names contain the text
    std::wstring searchText;          // as typed
    std::wstring searchFor;           // the text searchHits answer
    std::vector<uint8_t> searchHits;  // per commit, 1 = match
    int32_t searchCount = 0;
    uint64_t searchSeq = 0;
    void SetSearch(const std::wstring& text);
    bool Searching() const { return !searchFor.empty() && data && searchHits.size() == data->commits.size(); }
    int32_t FindMatch(int dir) const;   // row of the next (1) or previous (-1) match after the selection, -1 if none
    int32_t MatchOrdinal() const;       // position of the selected commit among the matches (1-based), 0 if not one

    // ---- lines added and removed by each commit, loaded on demand
    std::unordered_map<Oid, LineStat, OidHash> lineStats;
    bool statsInflight = false;
    void RequestLineStats(std::vector<Oid> ids);

    // ---- operations (one at a time; quick ones queue up)
    bool opBusy = false;
    int opButton = -1;  // the action showing progress (Action, or an app's own id)
    std::wstring opLabel, opProgress;
    double opStart = 0;
    struct PendingOp {
        int button;
        std::wstring label;
        std::function<OpOutcome(const OpCtx&)> work;
    };
    std::deque<PendingOp> opQueue;
    std::vector<UndoEntry> undo, redo;
    Notice notice;
    MessageSuggestion suggestion;
    bool mergeMsgShown = false;

    bool CanRun(int action, std::wstring* why) const;
    void RunOp(int button, const std::wstring& label, std::function<OpOutcome(const OpCtx&)> work, bool queue = false);
    void Fetch();
    void Pull();  // with the pull mode of the settings
    void Push(bool force);
    void Stash();
    void Pop(int stash, bool apply);
    void DropStash(int stash);  // the app asks first
    void CheckoutRef(int ref);
    void CheckoutCommit(int32_t commit);
    int CheckoutTarget(int32_t commit) const;  // the branch "Check out" switches to for a commit; -1: the commit itself
    bool CanCheckout(int32_t commit, std::wstring* tip) const;  // what "Check out" does for a commit, and whether it can
    std::wstring CheckNewBranch(const std::wstring& name) const;          // why a new branch can't have this name; empty when it can
    std::wstring CreateBranch(const std::wstring& name, int32_t commit);  // why it can't; empty once started
    void Undo();
    void Redo();
    void Stage(const std::vector<std::wstring>& paths, bool stage);  // paths empty = everything
    void Discard(const FileChange& f);                                // the app asks first
    enum CommitResult { CommitStarted, CommitNoSummary, CommitNothingStaged, CommitFailed };
    CommitResult Commit(const std::wstring& summary, const std::wstring& body, bool amend);
    std::wstring CanRevert(int32_t commit) const;  // why not; empty when it can
    void Revert(int32_t commit);                   // the app asks first
    void RequestHeadMessage();                     // the message of HEAD, for amending: comes as a Message event
    void PrefillMergeMessage();                    // a merge in progress offers its prepared message

    // ---- the diff of one file
    DiffState diff;
    bool OpenDiff(int section, const std::wstring& file);  // section: 0 the selected commit, 1 unstaged, 2 staged; true when another file opened
    void CloseDiff();
    void ReloadDiff();
    void SelectDiffLine(int32_t line, bool extend, bool toggle);  // click, Shift+click / drag, Ctrl/Cmd+click
    void SetDiffLines(const std::vector<int32_t>& lines);         // the selection as a list (changed lines only count)
    void ClearDiffSelection();
    PatchJob HunkJob(int hunk, bool discard) const;
    PatchJob LinesJob(bool discard) const;
    void ApplyPatch(const PatchJob& job);

protected:
    void Emit(SessionEvent e) {
        if (onEvent) onEvent(e);
    }
    void Note(const std::wstring& title, const std::wstring& message, bool error);

private:
    std::unique_ptr<RepoWatcher> watcher;
    std::wstring watchedRoot;
    void OnLoaded(std::shared_ptr<RepoData> d, std::shared_ptr<Graph> g, std::shared_ptr<WorkStatus> st, double ms);
    void OnStatus(std::shared_ptr<WorkStatus> st, std::shared_ptr<Graph> g);
    void RunSearch();
    void SyncDiff();
    void ApplyStageLocally(const std::vector<std::wstring>& paths, bool stage);
};
