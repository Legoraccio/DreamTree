#include "session.h"
#include "platform.h"
#include "present.h"
#include "settings.h"
#include "text.h"
#include <chrono>
#include <condition_variable>
#include <cwctype>

// ============================================================================ main thread

static std::function<void(std::function<void()>)>& Dispatcher() {
    static std::function<void(std::function<void()>)> post;
    return post;
}

void SetMainDispatcher(std::function<void(std::function<void()>)> post) { Dispatcher() = std::move(post); }

void RunOnMain(std::function<void()> fn) {
    if (Dispatcher()) Dispatcher()(std::move(fn));
}

namespace {

std::string TrimA(std::string_view s) { return std::string(TrimSv(s)); }

bool Contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

// HEAD as "b:<branch>" or "d:<sha>" (empty for an unborn branch).
std::wstring HeadSpec(const OpCtx& c) {
    GitOpResult b = c.Run(L"symbolic-ref -q --short HEAD");
    std::string name = TrimA(b.out);
    if (b.ok() && !name.empty()) return L"b:" + Widen(name);
    GitOpResult h = c.Run(L"rev-parse -q --verify HEAD");
    std::string sha = TrimA(h.out);
    return sha.empty() ? std::wstring() : L"d:" + Widen(sha);
}

std::wstring SwitchTo(const std::wstring& spec) {
    if (spec.compare(0, 2, L"b:") == 0) return L"switch " + QuoteArg(spec.substr(2));
    return L"switch --detach " + spec.substr(2);
}

std::wstring RevParse(const OpCtx& c, const std::wstring& rev) {
    GitOpResult r = c.Run(L"rev-parse -q --verify " + QuoteArg(rev));
    return r.ok() ? Widen(TrimA(r.out)) : std::wstring();
}

// "1 file changed, 2 insertions(+)" style summary of a pull.
std::wstring PullSummary(const std::string& out) {
    std::wstring last;
    size_t p = 0;
    while (p < out.size()) {
        size_t e = out.find('\n', p);
        if (e == std::string::npos) e = out.size();
        std::string line = TrimA(std::string_view(out).substr(p, e - p));
        p = e + 1;
        if (line.empty()) continue;
        if (line.find("changed") != std::string::npos || line.find("Successfully rebased") != std::string::npos) return Widen(line);
        last = Widen(line);
    }
    return last;
}

int32_t FindCommit(const RepoData& d, const Oid& id) {
    for (size_t i = 0; i < d.commits.size(); ++i)
        if (d.commits[i].id == id) return (int32_t)i;
    return -1;
}

// First line = summary, the rest (after the blank line) = description.
void SplitMessage(const std::wstring& msg, std::wstring& summary, std::wstring& body) {
    const size_t nl = msg.find(L'\n');
    summary = TrimW(msg.substr(0, nl));
    body.clear();
    if (nl != std::wstring::npos) body = TrimW(msg.substr(nl + 1));
}

// Case-insensitive search of an ASCII needle (already lower case) in UTF-8 text.
bool FindAsciiI(std::string_view hay, std::string_view n) {
    if (n.empty()) return true;
    if (n.size() > hay.size()) return false;
    auto low = [](char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; };
    for (size_t i = 0; i + n.size() <= hay.size(); ++i) {
        if (low(hay[i]) != n[0]) continue;
        size_t k = 1;
        while (k < n.size() && low(hay[i + k]) == n[k]) ++k;
        if (k == n.size()) return true;
    }
    return false;
}

// Commits whose subject, body line, author, id (a hex prefix of 4+ digits) or branch and tag names contain q.
std::vector<uint8_t> MatchCommits(const RepoData& d, const std::wstring& q, int32_t& count) {
    std::vector<uint8_t> hits(d.commits.size(), 0);
    const std::string utf8 = Narrow(q);
    bool ascii = true;
    for (unsigned char c : utf8) ascii &= c < 0x80;
    std::string needle = utf8;
    for (char& c : needle) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    std::wstring wide;
    auto has = [&](std::string_view hay) {
        if (ascii) return FindAsciiI(hay, needle);
        WidenInto(hay, wide);
        return ContainsI(wide, q);
    };
    std::vector<uint8_t> author(d.authors.size(), 0);
    for (size_t i = 0; i < d.authors.size(); ++i) author[i] = ContainsI(d.authors[i].name, q) || ContainsI(d.authors[i].email, q);
    uint8_t prefix[20] = {};
    size_t digits = 0;
    if (q.size() >= 4 && q.size() <= 40) {
        digits = q.size();
        for (size_t i = 0; i < q.size() && digits; ++i) {
            const wchar_t c = q[i];
            const int v = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
            if (v < 0) digits = 0;
            else prefix[i / 2] |= (uint8_t)(i % 2 ? v : v << 4);
        }
    }
    auto idMatch = [&](const Oid& id) {
        if (!digits) return false;
        if (memcmp(id.b, prefix, digits / 2) != 0) return false;
        return digits % 2 == 0 || (id.b[digits / 2] & 0xF0) == prefix[digits / 2];
    };
    for (const auto& [commit, labels] : d.labels)
        for (const Label& L : labels)
            if (commit >= 0 && commit < (int32_t)hits.size() && ContainsI(L.text, q)) hits[(size_t)commit] = 1;
    for (size_t i = 0; i < d.commits.size(); ++i) {
        const Commit& c = d.commits[i];
        if (!hits[i] && (author[c.author] || idMatch(c.id) || has(d.Subject(c)) || has(d.Body(c)))) hits[i] = 1;
    }
    count = 0;
    for (uint8_t h : hits) count += h;
    return hits;
}

void ClearSelection(DiffState& v) {
    v.sel.clear();
    v.selCount = 0;
    v.selAnchor = -1;
}

// Moves the line selection to a reloaded diff: a line stays selected when the new diff has it on the same side,
// at the same number and with the same text.
void RemapSelection(DiffState& v, const FileDiff& next) {
    if (!v.selCount || !v.diff) return ClearSelection(v);
    const FileDiff& prev = *v.diff;
    auto key = [](const DiffLine& L) { return (uint64_t)L.kind << 32 | (uint32_t)(L.kind == DiffLine::Deleted ? L.oldNo : L.newNo); };
    auto hash = [](const FileDiff& d, const DiffLine& L) { return Fnv1aW(std::wstring_view(d.text).substr(L.off, L.len)); };
    std::unordered_map<uint64_t, uint32_t> keep;
    for (size_t i = 0; i < v.sel.size() && i < prev.lines.size(); ++i)
        if (v.sel[i]) keep.emplace(key(prev.lines[i]), hash(prev, prev.lines[i]));
    std::vector<uint8_t> sel(next.lines.size(), 0);
    int32_t count = 0, firstSel = -1;
    for (size_t i = 0; i < next.lines.size(); ++i) {
        const DiffLine& L = next.lines[i];
        if (!IsChange(L)) continue;
        const auto it = keep.find(key(L));
        if (it == keep.end() || it->second != hash(next, L)) continue;
        sel[i] = 1;
        if (!count++) firstSel = (int32_t)i;
    }
    if (!count) return ClearSelection(v);
    v.sel = std::move(sel);
    v.selCount = count;
    v.selAnchor = firstSel;
}

}  // namespace

void RepoSession::Note(const std::wstring& t, const std::wstring& m, bool err) {
    if (t.empty() && m.empty()) return;
    notice = { t, m, err };
    Emit(SessionEvent::Notice);
}

// ============================================================================ loading

void RepoSession::StartLoad(bool refresh) {
    if (IsStart()) return;
    loading = true;
    refreshing = refresh && data != nullptr;
    if (!refreshing) {
        error.clear();
        untrusted.clear();
    }
    loadStart = NowMs();
    const uint64_t s = ++seq;
    const std::wstring p = path, safe = AppSettings().SafeDirFor(path);
    const bool head = headOnly;
    const double t0 = NowMs();
    std::weak_ptr<RepoSession> wk = shared_from_this();
    std::thread([wk, s, p, safe, head, t0] {
        // The working-tree status runs alongside the history load.
        std::shared_ptr<WorkStatus> st;
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        std::thread tst([&] {
            std::shared_ptr<WorkStatus> r = LoadWorkStatus(p, safe);
            {
                std::lock_guard<std::mutex> lk(m);
                st = std::move(r);
                done = true;
            }
            cv.notify_one();
        });
        std::shared_ptr<RepoData> data = LoadRepository(p, safe, head);
        std::shared_ptr<Graph> graph;
        bool withStatus = false;
        if (data->error.empty()) {
            {
                std::unique_lock<std::mutex> lk(m);
                withStatus = cv.wait_for(lk, std::chrono::milliseconds(200), [&] { return done; });
            }
            graph = std::make_shared<Graph>(BuildGraph(*data, withStatus && st && st->Dirty()));
        }
        std::shared_ptr<WorkStatus> stNow = withStatus ? st : nullptr;
        const double ms = NowMs() - t0;
        RunOnMain([wk, s, data, graph, stNow, ms] {
            if (auto self = wk.lock(); self && self->seq == s) self->OnLoaded(data, graph, stNow, ms);
        });
        tst.join();
        if (data->error.empty() && !withStatus) {
            std::shared_ptr<Graph> g2 = (st && st->Dirty()) ? std::make_shared<Graph>(BuildGraph(*data, true)) : nullptr;
            RunOnMain([wk, s, st, g2] {
                if (auto self = wk.lock(); self && self->seq == s) self->OnStatus(st, g2);
            });
        }
    }).detach();
    Emit(SessionEvent::Loading);
}

void RepoSession::OnLoaded(std::shared_ptr<RepoData> d, std::shared_ptr<Graph> g, std::shared_ptr<WorkStatus> st, double ms) {
    loading = false;
    const bool refresh = refreshing;
    refreshing = false;
    lastLoadRefresh = refresh;
    if (!d->error.empty()) {
        error = d->error;
        untrusted = d->untrustedPath;
        data.reset();
        graph.reset();
        Emit(SessionEvent::Loaded);
        return;
    }
    // Keep the selection (by object id) across refreshes.
    Oid selId;
    bool hadSel = false;
    const bool wasWip = selWip;
    if (refresh && data && selCommit >= 0 && selCommit < (int32_t)data->commits.size()) {
        selId = data->commits[(size_t)selCommit].id;
        hadSel = true;
    }
    data = d;
    graph = g;
    if (st) status = st;
    else if (!refresh) status.reset();
    error.clear();
    untrusted.clear();
    title = d->name;
    loadMs = ms;
    refsStamp = d->refsStamp;
    details.reset();
    if (refresh) {
        selWip = wasWip && g->hasWip;
        selCommit = hadSel ? FindCommit(*d, selId) : -1;
        // The selected commit is gone (undone or amended): its changes are back in the working tree.
        if (hadSel && selCommit < 0 && g->hasWip) selWip = true;
        if (!selWip && selCommit < 0) selCommit = d->headCommit;
    } else {
        selWip = g->hasWip;
        selCommit = g->hasWip ? -1 : (d->headCommit >= 0 ? d->headCommit : (d->commits.empty() ? -1 : 0));
    }
    PrefillMergeMessage();
    SyncDiff();
    if (!TrimW(searchText).empty()) RunSearch();  // commit indices changed
    else searchHits.clear();
    if (watchedRoot != d->root) {
        std::weak_ptr<RepoSession> wk = shared_from_this();
        watcher.reset();
        watcher = std::make_unique<RepoWatcher>(d->root, [wk] {
            RunOnMain([wk] {
                if (auto self = wk.lock()) self->CheckForChanges();
            });
        });
        watchedRoot = d->root;
    }
    AppSettings().AddRecent(d->root);
    RequestDetails();
    Emit(SessionEvent::Loaded);
}

void RepoSession::OnStatus(std::shared_ptr<WorkStatus> st, std::shared_ptr<Graph> g) {
    if (!data || !st) return;
    status = st;
    wipRowDelta = 0;
    PrefillMergeMessage();
    const bool wantWip = st->Dirty();
    if (graph && graph->hasWip != wantWip) {
        if (!g) g = std::make_shared<Graph>(BuildGraph(*data, wantWip));
        graph = g;
        wipRowDelta = wantWip ? 1 : -1;
        if (!wantWip && selWip) {  // committed or discarded: back to the checked-out commit
            selWip = false;
            selCommit = data->headCommit;
            RequestDetails();
        }
    }
    SyncDiff();
    Emit(SessionEvent::Status);
}

void RepoSession::CheckForChanges() {
    if (!data || loading || opBusy) return;  // an operation in progress reloads when it ends
    std::weak_ptr<RepoSession> wk = shared_from_this();
    std::shared_ptr<RepoData> d = data;
    const uint64_t s = seq, oldStamp = refsStamp;
    const bool hadWip = graph && graph->hasWip;
    std::thread([wk, d, s, oldStamp, hadWip] {
        if (RefsStamp(d->gitDir, d->commonDir) != oldStamp) {
            RunOnMain([wk, s] {
                if (auto self = wk.lock(); self && self->seq == s) self->StartLoad(true);
            });
            return;
        }
        std::shared_ptr<WorkStatus> st = LoadWorkStatus(d->root, d->safeDir);
        std::shared_ptr<Graph> g;
        if (st->valid && st->Dirty() != hadWip) g = std::make_shared<Graph>(BuildGraph(*d, st->Dirty()));
        RunOnMain([wk, s, st, g] {
            if (auto self = wk.lock(); self && self->seq == s && st->valid) self->OnStatus(st, g);
        });
    }).detach();
}

void RepoSession::Trust() {
    if (untrusted.empty()) return;
    AppSettings().Trust(untrusted);
    AppSettings().Save();
    StartLoad(false);
}

// ============================================================================ selection and details

int32_t RepoSession::SelectedRow() const {
    if (!graph) return -1;
    if (selWip) return graph->hasWip ? 0 : -1;
    if (selCommit >= 0 && selCommit < (int32_t)graph->rowOfCommit.size()) return graph->rowOfCommit[(size_t)selCommit];
    return -1;
}

void RepoSession::Select(int32_t commit, bool wip) {
    CloseDiff();  // picking a commit brings the history back
    selWip = wip;
    selCommit = wip ? -1 : commit;
    RequestDetails();
}

void RepoSession::SelectRow(int32_t row) {
    if (!graph || row < 0 || row >= (int32_t)graph->rows.size()) return;
    const int32_t c = graph->rows[(size_t)row].commit;
    Select(c, c < 0);
}

void RepoSession::RequestDetails() {
    if (!data || selWip || selCommit < 0 || selCommit >= (int32_t)data->commits.size()) {
        if (details) {
            details.reset();
            Emit(SessionEvent::Details);
        }
        return;
    }
    const Oid id = data->commits[(size_t)selCommit].id;
    if (details && details->id == id) return;
    if (auto it = cache.find(id); it != cache.end()) {
        details = it->second;
        Emit(SessionEvent::Details);
        return;
    }
    if (details) {
        details.reset();
        Emit(SessionEvent::Details);
    }
    if (inflight) return;  // the completion handler asks again for the current selection
    inflight = true;
    std::weak_ptr<RepoSession> wk = shared_from_this();
    const std::wstring root = data->root, safe = data->safeDir;
    std::thread([wk, root, safe, id] {
        std::shared_ptr<CommitDetails> det = LoadCommitDetails(root, safe, id);
        RunOnMain([wk, det, id] {
            auto self = wk.lock();
            if (!self) return;
            self->inflight = false;
            if (self->cache.size() > 600) self->cache.clear();
            self->cache[id] = det;
            self->RequestDetails();
        });
    }).detach();
}

// ============================================================================ search and line counts

void RepoSession::SetSearch(const std::wstring& text) {
    searchText = text;
    RunSearch();
}

void RepoSession::RunSearch() {
    const uint64_t s = ++searchSeq;
    const std::wstring q = TrimW(searchText);
    if (q.empty() || !data) {
        const bool had = !searchFor.empty() || !searchHits.empty();
        searchFor.clear();
        searchHits.clear();
        searchCount = 0;
        if (had) Emit(SessionEvent::Search);
        return;
    }
    std::shared_ptr<RepoData> d = data;
    std::weak_ptr<RepoSession> wk = shared_from_this();
    std::thread([wk, d, q, s] {
        int32_t count = 0;
        std::vector<uint8_t> hits = MatchCommits(*d, q, count);
        RunOnMain([wk, d, q, s, hits = std::move(hits), count]() mutable {
            auto self = wk.lock();
            if (!self || self->searchSeq != s || self->data != d) return;
            self->searchFor = q;
            self->searchHits = std::move(hits);
            self->searchCount = count;
            self->Emit(SessionEvent::Search);
        });
    }).detach();
}

int32_t RepoSession::FindMatch(int dir) const {
    if (!graph || !Searching() || searchCount == 0) return -1;
    const Graph& g = *graph;
    const int32_t n = (int32_t)g.rows.size();
    if (n == 0) return -1;
    const int32_t row = SelectedRow();
    const int32_t base = row >= 0 ? row : (dir > 0 ? -1 : n);
    for (int32_t k = 1; k <= n; ++k) {
        const int32_t r = ((base + dir * k) % n + n) % n;
        const int32_t c = g.rows[(size_t)r].commit;
        if (c >= 0 && searchHits[(size_t)c]) return r;
    }
    return -1;
}

int32_t RepoSession::MatchOrdinal() const {
    if (!graph || !Searching()) return 0;
    const int32_t sel = SelectedRow();
    if (sel < 0) return 0;
    const int32_t c = graph->rows[(size_t)sel].commit;
    if (c < 0 || !searchHits[(size_t)c]) return 0;
    int32_t k = 0;
    for (int32_t row = 0; row <= sel; ++row) {
        const int32_t cc = graph->rows[(size_t)row].commit;
        k += cc >= 0 && searchHits[(size_t)cc];
    }
    return k;
}

void RepoSession::RequestLineStats(std::vector<Oid> ids) {
    if (!data || statsInflight || ids.empty()) return;
    statsInflight = true;
    if (lineStats.size() > 200000) lineStats.clear();
    for (const Oid& id : ids) lineStats.emplace(id, LineStat{ -1, -1 });  // requested: not asked again
    std::weak_ptr<RepoSession> wk = shared_from_this();
    const std::wstring root = data->root, safe = data->safeDir;
    std::thread([wk, root, safe, ids = std::move(ids)] {
        std::vector<std::pair<Oid, LineStat>> res = LoadLineStats(root, safe, ids);
        RunOnMain([wk, res = std::move(res)] {
            auto self = wk.lock();
            if (!self) return;
            self->statsInflight = false;
            for (const auto& [id, st] : res) self->lineStats[id] = st;
            self->Emit(SessionEvent::LineStats);
        });
    }).detach();
}

// ============================================================================ operations

void RepoSession::RunOp(int button, const std::wstring& label, std::function<OpOutcome(const OpCtx&)> work, bool queue) {
    if (!data) return;
    if (opBusy) {
        if (queue) opQueue.push_back({ button, label, std::move(work) });
        return;
    }
    opBusy = true;
    opButton = button;
    opLabel = label;
    opProgress.clear();
    opStart = NowMs();
    std::weak_ptr<RepoSession> wk = shared_from_this();
    auto lastPost = std::make_shared<std::atomic<double>>(0.0);
    OpCtx ctx{ data->root, data->safeDir, nullptr };
    ctx.progress = [wk, lastPost](const std::string& line) {
        const double now = NowMs();
        if (now - lastPost->load() < 50) return;  // ~20 updates per second are plenty
        lastPost->store(now);
        std::wstring w = Widen(line);
        RunOnMain([wk, w] {
            if (auto self = wk.lock(); self && self->opBusy) {
                self->opProgress = w;
                self->Emit(SessionEvent::Operation);
            }
        });
    };
    std::thread([wk, ctx, work = std::move(work)] {
        OpOutcome res = work(ctx);
        RunOnMain([wk, res = std::move(res)] {
            auto self = wk.lock();
            if (!self) return;
            self->opBusy = false;
            self->opButton = -1;
            self->opProgress.clear();
            if (res.ok && res.hasUndo) {
                self->undo.push_back(res.undo);
                if (self->undo.size() > 50) self->undo.erase(self->undo.begin());
                self->redo.clear();
            }
            if (!res.quiet) self->Note(res.title, res.message, !res.ok);
            if (res.statusOnly) self->CheckForChanges();  // refs unchanged: just re-read the working tree
            else self->StartLoad(true);
            if (res.then) res.then();
            if (res.pushRejected) self->Emit(SessionEvent::PushRejected);
            if (!self->opQueue.empty()) {
                PendingOp next = std::move(self->opQueue.front());
                self->opQueue.pop_front();
                self->RunOp(next.button, next.label, std::move(next.work), true);
            }
            self->Emit(SessionEvent::Operation);
        });
    }).detach();
    Emit(SessionEvent::Operation);
}

bool RepoSession::CanRun(int b, std::wstring* tip) const {
    std::wstring scratch;
    std::wstring& tp = tip ? *tip : scratch;
    if (b == ActTerminal) {
        tp = L"Open a terminal in this repository";
        return data != nullptr;
    }
    if (!data) { tp.clear(); return false; }
    if (opBusy) { tp = opLabel + L"…"; return false; }
    const RepoData& d = *data;
    const Ref* head = HeadRef(d);
    const int pullMode = AppSettings().pullMode;
    switch (b) {
    case ActUndo:
        if (undo.empty()) { tp = L"Nothing to undo"; return false; }
        tp = L"Undo " + undo.back().label;
        return true;
    case ActRedo:
        if (redo.empty()) { tp = L"Nothing to redo"; return false; }
        tp = L"Redo " + redo.back().label;
        return true;
    case ActPull:
        if (d.remotes.empty()) { tp = L"This repository has no remotes"; return false; }
        if (!head) { tp = L"Check out a branch to pull"; return false; }
        if (head->upstream.empty()) { tp = L"'" + head->name + L"' has no upstream branch: push it first"; return false; }
        tp = pullMode == 1 ? L"Pull (fast-forward only)" : pullMode == 2 ? L"Pull (rebase)" : L"Pull (fast-forward if possible)";
        tp += L" from " + head->upstream;
        return true;
    case ActPush:
        if (d.remotes.empty()) { tp = L"This repository has no remotes"; return false; }
        if (!head) { tp = L"Check out a branch to push"; return false; }
        tp = head->upstream.empty() ? L"Push '" + head->name + L"' and set its upstream" : L"Push '" + head->name + L"' to " + head->upstream;
        return true;
    case ActBranch:
        if (d.commits.empty()) { tp = L"Commit something before creating branches"; return false; }
        tp = L"Create a branch at the selected commit";
        return true;
    case ActStash:
        if (!status || !status->Dirty()) { tp = L"No changes to stash"; return false; }
        tp = L"Stash all changes, including untracked files";
        return true;
    case ActPop:
        if (d.stashes.empty()) { tp = L"No stashes"; return false; }
        tp = L"Pop " + d.stashes[0].name + L"  " + d.stashes[0].message;
        return true;
    case ActFetch:
        if (d.remotes.empty()) { tp = L"This repository has no remotes"; return false; }
        tp = L"Fetch all remotes (branches deleted there are pruned)";
        return true;
    default:
        return false;
    }
}

// ---- remotes

void RepoSession::Fetch() {
    if (!data || opBusy) return;
    if (data->remotes.empty()) {
        Note(L"Nothing to fetch", L"This repository has no remotes.", true);
        return;
    }
    RunOp(ActFetch, L"Fetching", [](const OpCtx& c) {
        OpOutcome o;
        GitOpResult r = c.Run(L"fetch --all --prune --progress");
        o.ok = r.ok();
        o.title = o.ok ? L"Fetch complete" : L"Fetch failed";
        o.message = o.ok ? L"Remote branches and tags are up to date." : GitErrorText(r);
        return o;
    });
}

void RepoSession::Pull() {
    std::wstring why;
    if (!data || opBusy) return;
    if (!CanRun(ActPull, &why)) {
        Note(L"Can't pull", why, true);
        return;
    }
    const int mode = AppSettings().pullMode;
    const std::wstring args = mode == 1   ? L"pull --ff-only --autostash --progress"
                              : mode == 2 ? L"pull --rebase --autostash --progress"
                                          : L"pull --no-rebase --no-edit --autostash --progress";
    RunOp(ActPull, L"Pulling", [args](const OpCtx& c) {
        OpOutcome o;
        GitOpResult r = c.Run(args);
        o.ok = r.ok();
        if (o.ok) {
            o.title = L"Pull complete";
            o.message = Contains(r.out, "Already up to date") ? L"Already up to date." : PullSummary(r.out);
        } else {
            const bool conflict = Contains(r.out, "CONFLICT") || Contains(r.err, "CONFLICT");
            o.title = conflict ? L"Pull stopped on conflicts" : L"Pull failed";
            o.message = GitErrorText(r);
            if (conflict) o.message = L"Resolve the conflicts in your editor and commit the result.\n" + o.message;
        }
        return o;
    });
}

void RepoSession::Push(bool force) {
    std::wstring why;
    if (!data || opBusy) return;
    if (!CanRun(ActPush, &why)) {
        Note(L"Can't push", why, true);
        return;
    }
    const Ref* head = HeadRef(*data);
    const std::wstring branch = head->name, upstream = head->upstream;
    const std::vector<std::wstring> remotes = data->remotes;
    RunOp(ActPush, force ? L"Force pushing" : L"Pushing", [branch, upstream, remotes, force](const OpCtx& c) {
        OpOutcome o;
        std::wstring args, target;
        if (!upstream.empty()) {
            args = L"-c push.default=upstream push --progress";
            target = upstream;
        } else {
            const std::wstring remote =
                std::find(remotes.begin(), remotes.end(), L"origin") != remotes.end() ? std::wstring(L"origin") : remotes.front();
            args = L"push --progress -u " + QuoteArg(remote) + L" " + QuoteArg(L"HEAD:refs/heads/" + branch);
            target = remote + L"/" + branch;
        }
        if (force) args += L" --force-with-lease";
        GitOpResult r = c.Run(args);
        if (r.ok()) {
            o.ok = true;
            o.title = force ? L"Force push complete" : L"Push complete";
            o.message = Contains(r.err, "Everything up-to-date") ? target + L" was already up to date." : L"Pushed '" + branch + L"' to " + target + L".";
            if (upstream.empty()) o.message += L" It is now the upstream of '" + branch + L"'.";
            return o;
        }
        const bool rejected = Contains(r.err, "[rejected]") || Contains(r.err, "non-fast-forward") || Contains(r.err, "fetch first");
        if (rejected && !force) {
            o.quiet = true;
            o.pushRejected = true;
            return o;
        }
        if (force && Contains(r.err, "stale info")) {
            o.title = L"Force push stopped";
            o.message = L"The remote branch has commits you haven't fetched yet, and they would be lost. "
                        L"Fetch them, review them, then force push again if you still want to replace them.";
            return o;
        }
        o.title = force ? L"Force push failed" : L"Push failed";
        o.message = GitErrorText(r);
        return o;
    });
}

// ---- stash

void RepoSession::Stash() {
    if (!CanRun(ActStash, nullptr)) return;
    RunOp(ActStash, L"Stashing", [](const OpCtx& c) {
        OpOutcome o;
        const std::wstring args = L"stash push --include-untracked";
        GitOpResult r = c.Run(args);
        if (!r.ok()) {
            o.title = L"Stash failed";
            o.message = GitErrorText(r);
            return o;
        }
        o.ok = true;
        if (Contains(r.out, "No local changes to save")) {
            o.title = L"Nothing to stash";
            o.message = L"There are no local changes.";
            return o;
        }
        o.title = L"Changes stashed";
        o.message = PullSummary(r.out);
        const std::wstring sha = RevParse(c, L"refs/stash");
        if (!sha.empty()) {
            o.hasUndo = true;
            o.undo.kind = UndoEntry::Kind::Stash;
            o.undo.label = L"stash";
            o.undo.stash = sha;
            o.undo.redoArgs = args;
        }
        return o;
    });
}

void RepoSession::Pop(int index, bool apply) {
    if (!data || opBusy || index < 0 || index >= (int)data->stashes.size()) return;
    const std::wstring name = data->stashes[(size_t)index].name;
    RunOp(apply ? -1 : ActPop, apply ? L"Applying " + name : L"Popping " + name, [name, apply](const OpCtx& c) {
        OpOutcome o;
        GitOpResult r = c.Run((apply ? L"stash apply " : L"stash pop ") + QuoteArg(name));
        if (r.ok()) {
            o.ok = true;
            o.title = apply ? L"Stash applied" : L"Stash popped";
            o.message = name + (apply ? L" was applied to the working tree and kept." : L" was applied to the working tree.");
            return o;
        }
        const bool conflict = Contains(r.out, "CONFLICT") || Contains(r.err, "CONFLICT");
        o.title = conflict ? L"Stash applied with conflicts" : L"Could not apply the stash";
        o.message = GitErrorText(r);
        if (conflict && !apply) o.message += L"\nThe stash was kept.";
        return o;
    });
}

void RepoSession::DropStash(int index) {
    if (!data || opBusy || index < 0 || index >= (int)data->stashes.size()) return;
    const StashEntry s = data->stashes[(size_t)index];
    RunOp(-1, L"Deleting " + s.name, [s](const OpCtx& c) {
        OpOutcome o;
        GitOpResult r = c.Run(L"stash drop " + QuoteArg(s.name));
        o.ok = r.ok();
        o.title = o.ok ? L"Stash deleted" : L"Could not delete the stash";
        o.message = o.ok ? s.name + L" was deleted." : GitErrorText(r);
        return o;
    });
}

// ---- checkout and branches

void RepoSession::CheckoutRef(int ri) {
    if (!data || opBusy || ri < 0 || ri >= (int)data->refs.size()) return;
    const RepoData& d = *data;
    const Ref& r = d.refs[(size_t)ri];
    std::wstring args, label, created;
    switch (r.kind) {
    case RefKind::Local:
        if (r.head) return;
        args = L"switch " + QuoteArg(r.name);
        label = r.name;
        break;
    case RefKind::Remote: {
        const Ref* local = nullptr;
        for (const Ref& x : d.refs)
            if (x.kind == RefKind::Local && x.name == r.branch) local = &x;
        if (local) {
            if (local->head) return;
            args = L"switch " + QuoteArg(local->name);
            label = local->name;
        } else {
            args = L"switch --track -c " + QuoteArg(r.branch) + L" " + QuoteArg(r.name);
            label = r.branch;
            created = r.branch;
        }
        break;
    }
    case RefKind::Tag:
        args = L"switch --detach " + QuoteArg(L"refs/tags/" + r.name);
        label = r.name;
        break;
    }
    RunOp(-1, L"Checking out " + label, [args, label, created](const OpCtx& c) {
        OpOutcome o;
        const std::wstring from = HeadSpec(c);
        GitOpResult res = c.Run(args);
        if (!res.ok()) {
            o.title = L"Checkout failed";
            o.message = GitErrorText(res);
            return o;
        }
        o.ok = true;
        o.title = L"Checked out " + label;
        if (!created.empty()) o.message = L"Created the local branch '" + created + L"', tracking the remote branch.";
        o.hasUndo = !from.empty();
        o.undo.kind = created.empty() ? UndoEntry::Kind::Checkout : UndoEntry::Kind::CreateBranch;
        o.undo.label = L"checkout " + label;
        o.undo.from = from;
        o.undo.to = HeadSpec(c);
        o.undo.branch = created;
        o.undo.sha = RevParse(c, L"HEAD");
        o.undo.redoArgs = args;
        return o;
    });
}

void RepoSession::CheckoutCommit(int32_t commit) {
    if (!data || opBusy || commit < 0 || commit >= (int32_t)data->commits.size()) return;
    const std::wstring sha = data->commits[(size_t)commit].id.Hex();
    const std::wstring args = L"switch --detach " + sha;
    RunOp(-1, L"Checking out " + sha.substr(0, 7), [args, sha](const OpCtx& c) {
        OpOutcome o;
        const std::wstring from = HeadSpec(c);
        GitOpResult r = c.Run(args);
        if (!r.ok()) {
            o.title = L"Checkout failed";
            o.message = GitErrorText(r);
            return o;
        }
        o.ok = true;
        o.title = L"Checked out " + sha.substr(0, 7);
        o.message = L"HEAD is detached at this commit.";
        o.hasUndo = !from.empty();
        o.undo.kind = UndoEntry::Kind::Checkout;
        o.undo.label = L"checkout " + sha.substr(0, 7);
        o.undo.from = from;
        o.undo.to = L"d:" + sha;
        o.undo.redoArgs = args;
        return o;
    });
}

int RepoSession::CheckoutTarget(int32_t commit) const {
    if (!data) return -1;
    auto it = data->labels.find(commit);
    if (it == data->labels.end()) return -1;
    for (const Label& L : it->second)
        if (L.local && !L.head) return L.ref;
    for (const Label& L : it->second)
        if (!L.local && L.remote) return L.ref;
    return -1;
}

bool RepoSession::CanCheckout(int32_t commit, std::wstring* tip) const {
    std::wstring scratch;
    std::wstring& tp = tip ? *tip : scratch;
    if (!data || commit < 0 || commit >= (int32_t)data->commits.size()) { tp.clear(); return false; }
    const RepoData& d = *data;
    const int target = CheckoutTarget(commit);
    if (target >= 0) {
        const Ref& tr = d.refs[(size_t)target];
        bool hasLocal = false;
        for (const Ref& x : d.refs) hasLocal |= tr.kind == RefKind::Remote && x.kind == RefKind::Local && x.name == tr.branch;
        tp = tr.kind == RefKind::Local ? L"Switch to " + tr.name
             : hasLocal               ? L"Switch to " + tr.branch
                                      : L"Create " + tr.branch + L" tracking " + tr.name;
    } else if (commit == d.headCommit) {
        tp = d.detached ? L"HEAD is already at this commit" : L"Already checked out";
        return false;
    } else {
        tp = L"Check out this commit (detached HEAD)";
    }
    if (opBusy) {
        tp = opLabel + L"\u2026";
        return false;
    }
    return true;
}

static std::wstring BranchNameOf(const std::wstring& input) {
    std::wstring name = input;
    while (!name.empty() && name.back() == L'-') name.pop_back();  // a trailing space typed as '-'
    return name;
}

std::wstring RepoSession::CheckNewBranch(const std::wstring& input) const {
    const std::wstring name = BranchNameOf(input);
    std::wstring err = CheckBranchName(name);
    if (err.empty() && data)
        for (const Ref& r : data->refs)
            if (r.kind == RefKind::Local && r.name == name) return L"A branch named '" + name + L"' already exists.";
    return err;
}

std::wstring RepoSession::CreateBranch(const std::wstring& input, int32_t commit) {
    if (!data || opBusy) return L"Another operation is running.";
    const std::wstring name = BranchNameOf(input);
    if (std::wstring err = CheckNewBranch(name); !err.empty()) return err;
    const RepoData& d = *data;
    const int32_t at = commit >= 0 ? commit : d.headCommit;
    const std::wstring start = (at >= 0 && at < (int32_t)d.commits.size()) ? d.commits[(size_t)at].id.Hex() : std::wstring();
    const std::wstring args = L"switch -c " + QuoteArg(name) + (start.empty() ? L"" : L" " + start);
    RunOp(ActBranch, L"Creating " + name, [args, name](const OpCtx& c) {
        OpOutcome o;
        const std::wstring from = HeadSpec(c);
        GitOpResult r = c.Run(args);
        if (!r.ok()) {
            o.title = L"Couldn't create the branch";
            o.message = GitErrorText(r);
            return o;
        }
        o.ok = true;
        o.title = L"Created " + name;
        o.message = L"The new branch is checked out.";
        o.hasUndo = !from.empty();
        o.undo.kind = UndoEntry::Kind::CreateBranch;
        o.undo.label = L"create branch " + name;
        o.undo.branch = name;
        o.undo.sha = RevParse(c, L"HEAD");
        o.undo.from = from;
        o.undo.to = L"b:" + name;
        o.undo.redoArgs = args;
        return o;
    });
    return L"";
}

// ---- undo / redo

void RepoSession::Undo() {
    if (!data || opBusy || undo.empty()) return;
    const UndoEntry e = undo.back();
    std::weak_ptr<RepoSession> wk = shared_from_this();
    RunOp(ActUndo, L"Undoing " + e.label, [e, wk](const OpCtx& c) {
        OpOutcome o;
        GitOpResult r;
        std::wstring note;
        auto dropEntry = [wk] {
            if (auto self = wk.lock(); self && !self->undo.empty()) self->undo.pop_back();
        };
        switch (e.kind) {
        case UndoEntry::Kind::Checkout:
            r = c.Run(SwitchTo(e.from));
            break;
        case UndoEntry::Kind::CreateBranch:
            r = c.Run(SwitchTo(e.from));
            // Delete the branch only while it still points where it was created: nothing can be lost.
            if (r.ok() && !e.branch.empty()) {
                if (RevParse(c, L"refs/heads/" + e.branch) == e.sha) {
                    GitOpResult del = c.Run(L"branch -D " + QuoteArg(e.branch));
                    if (!del.ok()) r = del;
                } else {
                    note = L"'" + e.branch + L"' has new commits, so it was kept.";
                }
            }
            break;
        case UndoEntry::Kind::Stash:
            if (RevParse(c, L"refs/stash") != e.stash) {
                o.title = L"Can't undo the stash";
                o.message = L"The stash it created is no longer the most recent one.";
                o.then = dropEntry;
                return o;
            }
            r = c.Run(L"stash pop");
            break;
        case UndoEntry::Kind::Commit:
        case UndoEntry::Kind::Revert: {
            // Move HEAD back, only while nobody else can have the commit. A commit's changes stay staged; a
            // revert is dropped with its changes (--keep stops if that would touch local changes).
            const std::wstring sha = e.to.substr(2), prev = e.from.substr(2);
            std::wstring why;
            if (RevParse(c, L"HEAD") != sha) why = L"Other commits were made or checked out after it.";
            else {
                GitOpResult pushed = c.Run(L"branch -r --contains " + sha);
                if (pushed.ok() && !TrimA(pushed.out).empty()) why = L"It has already been pushed.";
            }
            if (!why.empty()) {
                o.title = L"Can't undo the " + e.label;
                o.message = why;
                o.then = dropEntry;
                return o;
            }
            const bool revert = e.kind == UndoEntry::Kind::Revert;
            r = c.Run((revert ? L"reset --keep " : L"reset --soft ") + prev);
            if (r.ok() && !revert) note = L"Its changes are staged again.";
            break;
        }
        }
        if (!r.ok()) {
            o.title = L"Undo failed";
            o.message = GitErrorText(r);
            return o;
        }
        o.ok = true;
        o.title = L"Undid " + e.label;
        o.message = note;
        o.then = [wk, e] {
            if (auto self = wk.lock()) {
                if (!self->undo.empty()) self->undo.pop_back();
                self->redo.push_back(e);
            }
        };
        return o;
    });
}

void RepoSession::Redo() {
    if (!data || opBusy || redo.empty()) return;
    const UndoEntry e = redo.back();
    std::weak_ptr<RepoSession> wk = shared_from_this();
    RunOp(ActRedo, L"Redoing " + e.label, [e, wk](const OpCtx& c) {
        OpOutcome o;
        UndoEntry again = e;
        GitOpResult r;
        if (e.kind == UndoEntry::Kind::Commit || e.kind == UndoEntry::Kind::Revert) {  // the commit still exists: point HEAD at it again
            if (RevParse(c, L"HEAD") != e.from.substr(2)) {
                o.title = L"Can't redo the " + e.label;
                o.message = L"HEAD has moved since the undo.";
                o.then = [wk] {
                    if (auto self = wk.lock(); self && !self->redo.empty()) self->redo.pop_back();
                };
                return o;
            }
            r = c.Run((e.kind == UndoEntry::Kind::Revert ? L"reset --keep " : L"reset --soft ") + e.to.substr(2));
        } else {
            again.from = HeadSpec(c);
            r = c.Run(e.kind == UndoEntry::Kind::Checkout && !e.to.empty() ? SwitchTo(e.to) : e.redoArgs);
        }
        if (!r.ok()) {
            o.title = L"Redo failed";
            o.message = GitErrorText(r);
            return o;
        }
        if (e.kind == UndoEntry::Kind::Stash) again.stash = RevParse(c, L"refs/stash");
        if (e.kind == UndoEntry::Kind::CreateBranch) again.sha = RevParse(c, L"HEAD");
        o.ok = true;
        o.title = L"Redid " + e.label;
        o.then = [wk, again] {
            if (auto self = wk.lock()) {
                if (!self->redo.empty()) self->redo.pop_back();
                self->undo.push_back(again);
            }
        };
        return o;
    });
}

// ---- working tree: stage, discard, commit

// Moves entries between the unstaged and staged lists right away; the status reload that follows
// the git command replaces them with the real state.
void RepoSession::ApplyStageLocally(const std::vector<std::wstring>& paths, bool stage) {
    if (!status) return;
    auto inScope = [&](const std::wstring& p) {
        if (paths.empty()) return true;
        for (const std::wstring& q : paths)
            if (p == q || (p.size() > q.size() && p.compare(0, q.size(), q) == 0 && p[q.size()] == L'/')) return true;
        return false;
    };
    auto st = std::make_shared<WorkStatus>(*status);
    std::vector<FileChange>& from = stage ? st->unstaged : st->staged;
    std::vector<FileChange>& to = stage ? st->staged : st->unstaged;
    std::vector<FileChange> moved, keep;
    for (const FileChange& f : from) (inScope(f.path) ? moved : keep).push_back(f);
    if (moved.empty()) return;
    from = std::move(keep);
    auto add = [&](FileChange f) {
        for (const FileChange& g : to)
            if (g.path == f.path) return;
        to.push_back(std::move(f));
    };
    for (FileChange f : moved) {
        if (stage) {
            if (f.status == L'?') f.status = L'A';
            else if (f.status == L'U') f.status = L'M';
            add(f);
        } else if (f.status == L'R' || f.status == L'C') {
            add({ L'?', f.path, L"" });
            if (f.status == L'R' && !f.oldPath.empty()) add({ L'D', f.oldPath, L"" });
        } else {
            if (f.status == L'A') f.status = L'?';
            add(f);
        }
    }
    std::sort(to.begin(), to.end(), [](const FileChange& a, const FileChange& b) { return a.path < b.path; });
    status = st;
    wipRowDelta = 0;
    Emit(SessionEvent::Status);
}

void RepoSession::Stage(const std::vector<std::wstring>& paths, bool stage) {
    if (!data || !status) return;
    std::wstring spec, tmp;
    if (!paths.empty()) {
        size_t total = 0;
        for (const std::wstring& p : paths) total += p.size() + 3;
        if (total > 6000) {  // long lists go through a NUL-separated pathspec file (command lines are limited)
            std::string list;
            for (const std::wstring& p : paths) {
                list += Narrow(p);
                list.push_back('\0');
            }
            tmp = WriteTempFile(list);
            if (tmp.empty()) return;
            spec = L" --pathspec-from-file=" + QuoteArg(tmp) + L" --pathspec-file-nul";
        } else {
            spec = L" --";
            for (const std::wstring& p : paths) spec += L" " + QuoteArg(p);
        }
    }
    std::wstring args;
    if (stage) args = L"--literal-pathspecs add -A" + spec;
    else if (data->unborn) args = L"--literal-pathspecs rm -r -q --cached" + (paths.empty() ? std::wstring(L" -- .") : spec);
    else args = paths.empty() ? std::wstring(L"reset -q") : L"--literal-pathspecs restore --staged" + spec;
    ApplyStageLocally(paths, stage);
    RunOp(-1, stage ? L"Staging" : L"Unstaging",
          [args, tmp, stage](const OpCtx& c) {
              OpOutcome o;
              GitOpResult r = c.Run(args);
              if (!tmp.empty()) RemoveFile(tmp);
              o.ok = r.ok();
              o.quiet = o.ok;
              o.statusOnly = true;
              if (!o.ok) {
                  o.title = stage ? L"Couldn't stage the changes" : L"Couldn't unstage the changes";
                  o.message = GitErrorText(r);
              }
              return o;
          },
          true);
}

void RepoSession::Discard(const FileChange& f) {
    if (!data || f.status == L'U') return;
    const bool untracked = f.status == L'?';
    const std::wstring args = untracked ? L"--literal-pathspecs clean -f -q -- " + QuoteArg(f.path)
                                        : L"--literal-pathspecs restore -- " + QuoteArg(f.path);
    RunOp(-1, L"Discarding changes",
          [args](const OpCtx& c) {
              OpOutcome o;
              GitOpResult r = c.Run(args);
              o.ok = r.ok();
              o.quiet = o.ok;
              o.statusOnly = true;
              if (!o.ok) {
                  o.title = L"Couldn't discard the changes";
                  o.message = GitErrorText(r);
              }
              return o;
          },
          true);
}

RepoSession::CommitResult RepoSession::Commit(const std::wstring& summaryIn, const std::wstring& bodyIn, bool amendIn) {
    if (!data || !status || opBusy) return CommitFailed;
    const std::wstring summary = TrimW(summaryIn), body = TrimW(bodyIn);
    const bool amend = amendIn && !data->unborn;
    const size_t staged = status->staged.size();
    if (summary.empty()) {
        Note(L"Write a commit summary", L"The first line of the commit message can't be empty.", true);
        return CommitNoSummary;
    }
    if (!amend && staged == 0) {
        Note(L"Nothing to commit", L"Stage the changes you want to commit first.", true);
        return CommitNothingStaged;
    }
    const std::wstring message = body.empty() ? summary : summary + L"\n\n" + body;
    const std::wstring tmp = WriteTempFile(Narrow(message) + "\n");
    if (tmp.empty()) {
        Note(L"Commit failed", L"The commit message couldn't be written to a temporary file.", true);
        return CommitFailed;
    }
    const std::wstring args = L"commit --cleanup=whitespace" + std::wstring(amend ? L" --amend" : L"") + L" -F " + QuoteArg(tmp);
    std::weak_ptr<RepoSession> wk = shared_from_this();
    RunOp(kCommitOp, amend ? L"Amending the last commit" : L"Committing", [args, tmp, amend, summary, wk](const OpCtx& c) {
        OpOutcome o;
        const std::wstring before = RevParse(c, L"HEAD");
        GitOpResult r = c.Run(args);
        RemoveFile(tmp);
        if (!r.ok()) {
            o.title = amend ? L"Amend failed" : L"Commit failed";
            o.message = GitErrorText(r);
            return o;
        }
        const std::wstring after = RevParse(c, L"HEAD");
        o.ok = true;
        o.title = (amend ? L"Amended " : L"Committed ") + after.substr(0, 7);
        o.message = summary;
        if (!before.empty() && !after.empty()) {
            o.hasUndo = true;
            o.undo.kind = UndoEntry::Kind::Commit;
            o.undo.label = amend ? L"amend" : L"commit";
            o.undo.from = L"d:" + before;
            o.undo.to = L"d:" + after;
        }
        o.then = [wk] {
            if (auto self = wk.lock()) self->Emit(SessionEvent::Committed);
        };
        return o;
    });
    return CommitStarted;
}

std::wstring RepoSession::CanRevert(int32_t commit) const {
    if (!data || commit < 0 || commit >= (int32_t)data->commits.size()) return L"Select a commit.";
    if (opBusy) return opLabel + L"…";
    if (data->unborn || (data->commits[(size_t)commit].flags & CF_STASH)) return L"Only commits can be reverted.";
    if (status && !status->staged.empty()) return L"Commit or unstage the staged changes first: reverting makes a new commit.";
    return L"";
}

void RepoSession::Revert(int32_t commit) {
    const std::wstring why = CanRevert(commit);
    if (!why.empty()) {
        Note(L"Can't revert", why, true);
        return;
    }
    const ::Commit& cm = data->commits[(size_t)commit];  // (Commit alone names the member function)
    const bool merge = cm.parentCount > 1;
    const std::wstring sha = cm.id.Hex();
    const std::wstring args = L"revert --no-edit " + std::wstring(merge ? L"-m 1 " : L"") + sha;
    RunOp(-1, L"Reverting " + sha.substr(0, 7), [args, sha](const OpCtx& c) {
        OpOutcome o;
        const std::wstring before = RevParse(c, L"HEAD");
        GitOpResult r = c.Run(args);
        if (!r.ok()) {
            const bool conflict = Contains(r.out, "CONFLICT") || Contains(r.err, "CONFLICT");
            o.title = conflict ? L"Revert stopped on conflicts" : L"Revert failed";
            o.message = GitErrorText(r);
            if (conflict) o.message = L"Resolve the conflicts and commit, or run git revert --abort.\n" + o.message;
            return o;
        }
        const std::wstring after = RevParse(c, L"HEAD");
        o.ok = true;
        o.title = L"Reverted " + sha.substr(0, 7);
        o.message = L"The new commit " + after.substr(0, 7) + L" undoes its changes.";
        if (!before.empty() && !after.empty()) {
            o.hasUndo = true;
            o.undo.kind = UndoEntry::Kind::Revert;
            o.undo.label = L"revert";
            o.undo.from = L"d:" + before;
            o.undo.to = L"d:" + after;
        }
        return o;
    });
}

void RepoSession::RequestHeadMessage() {
    if (!data || data->unborn) return;
    std::weak_ptr<RepoSession> wk = shared_from_this();
    const std::wstring root = data->root, safe = data->safeDir;
    std::thread([wk, root, safe] {
        const std::wstring msg = ReadHeadMessage(root, safe);
        RunOnMain([wk, msg] {
            auto self = wk.lock();
            if (!self) return;
            SplitMessage(msg, self->suggestion.summary, self->suggestion.body);
            self->suggestion.amend = true;
            self->Emit(SessionEvent::Message);
        });
    }).detach();
}

void RepoSession::PrefillMergeMessage() {
    if (!data) return;
    const std::wstring msg = ReadMergeMessage(data->gitDir);
    if (msg.empty()) {
        mergeMsgShown = false;
        return;
    }
    if (mergeMsgShown) return;
    mergeMsgShown = true;
    SplitMessage(msg, suggestion.summary, suggestion.body);
    suggestion.amend = false;
    Emit(SessionEvent::Message);
}

// ============================================================================ diff

bool RepoSession::OpenDiff(int section, const std::wstring& file) {
    if (!data) return false;
    DiffRequest req;
    req.section = section;
    req.path = file;
    const std::vector<FileChange>* files = nullptr;
    if (section == 0) {
        if (!details || !details->ok) return false;
        req.commit = details->id;
        files = &details->files;
    } else if (status) {
        files = section == 1 ? &status->unstaged : &status->staged;
    }
    if (!files) return false;
    bool found = false;
    for (const FileChange& f : *files)
        if (f.path == file) {
            req.status = f.status;
            req.oldPath = f.oldPath;
            found = true;
            break;
        }
    if (!found) return false;
    DiffState& v = diff;
    const bool same = v.open && v.req.section == req.section && v.req.path == req.path && v.req.commit == req.commit;
    v.open = true;
    v.req = req;
    if (!same) {
        v.diff.reset();
        v.hunkOpSeq = 0;
        ClearSelection(v);
    }
    ReloadDiff();
    Emit(SessionEvent::Diff);
    return !same;
}

void RepoSession::CloseDiff() {
    DiffState& v = diff;
    if (!v.open) return;
    v.open = false;
    v.diff.reset();
    v.hunkOpSeq = 0;
    ClearSelection(v);
    ++v.seq;  // drops a load still running
    Emit(SessionEvent::Diff);
}

void RepoSession::ReloadDiff() {
    DiffState& v = diff;
    if (!v.open || !data) return;
    const uint64_t s = ++v.seq;
    v.loadStart = NowMs();
    std::weak_ptr<RepoSession> wk = shared_from_this();
    const std::wstring root = data->root, safe = data->safeDir;
    const DiffRequest req = v.req;
    std::thread([wk, s, root, safe, req] {
        std::shared_ptr<FileDiff> d = LoadFileDiff(root, safe, req);
        RunOnMain([wk, s, d] {
            auto self = wk.lock();
            if (!self || !self->diff.open || self->diff.seq != s) return;
            DiffState& v = self->diff;
            // Nothing left on this side (every hunk was staged or unstaged): follow the file to the other list.
            if (d->ok && d->lines.empty() && !d->binary && d->modeChange.empty() && v.req.section > 0 && self->status) {
                const int other = v.req.section == 1 ? 2 : 1;
                for (const FileChange& f : other == 1 ? self->status->unstaged : self->status->staged) {
                    if (f.path != v.req.path) continue;
                    v.req.section = other;
                    v.req.status = f.status;
                    v.req.oldPath = f.oldPath;
                    ClearSelection(v);
                    self->ReloadDiff();
                    return;
                }
            }
            RemapSelection(v, *d);
            v.diff = d;
            v.diffSeq = s;
            self->Emit(SessionEvent::Diff);
        });
    }).detach();
}

void RepoSession::SyncDiff() {
    DiffState& v = diff;
    if (!v.open) return;
    if (!data) {
        CloseDiff();
        return;
    }
    if (v.req.section == 0) {
        // A commit never changes: keep its diff while the commit is still the selected one.
        const bool selected = !selWip && selCommit >= 0 && selCommit < (int32_t)data->commits.size() &&
                              data->commits[(size_t)selCommit].id == v.req.commit;
        if (!selected) CloseDiff();
        return;
    }
    if (!status) return;
    for (const int s : { v.req.section, v.req.section == 1 ? 2 : 1 }) {
        for (const FileChange& f : s == 1 ? status->unstaged : status->staged) {
            if (f.path != v.req.path) continue;
            if (s != v.req.section) ClearSelection(v);
            v.req.section = s;
            v.req.status = f.status;
            v.req.oldPath = f.oldPath;
            ReloadDiff();
            return;
        }
    }
    CloseDiff();  // committed, discarded or reverted: nothing left to show
}

void RepoSession::SelectDiffLine(int32_t line, bool extend, bool toggle) {
    DiffState& v = diff;
    const std::shared_ptr<FileDiff> d = v.diff;
    if (!d || line < 0 || line >= (int32_t)d->lines.size()) return;
    if (v.sel.size() != d->lines.size()) v.sel.assign(d->lines.size(), 0);
    if (toggle) {  // Ctrl/Cmd+click: adds or removes one line
        if (IsChange(d->lines[(size_t)line])) v.sel[(size_t)line] ^= 1;
        v.selAnchor = line;
    } else {  // click: one line; Shift+click or drag: every changed line from the anchor
        std::fill(v.sel.begin(), v.sel.end(), uint8_t(0));
        const int32_t from = extend && v.selAnchor >= 0 && v.selAnchor < (int32_t)d->lines.size() ? v.selAnchor : line;
        for (int32_t i = std::min(from, line); i <= std::max(from, line); ++i)
            if (IsChange(d->lines[(size_t)i])) v.sel[(size_t)i] = 1;
        v.selAnchor = from;
    }
    v.selCount = 0;
    for (const uint8_t f : v.sel) v.selCount += f;
    if (!v.selCount) v.sel.clear();
    Emit(SessionEvent::Diff);
}

void RepoSession::SetDiffLines(const std::vector<int32_t>& lines) {
    DiffState& v = diff;
    const std::shared_ptr<FileDiff> d = v.diff;
    if (!d) return;
    v.sel.assign(d->lines.size(), 0);
    v.selCount = 0;
    v.selAnchor = -1;
    for (int32_t i : lines) {
        if (i < 0 || i >= (int32_t)d->lines.size() || !IsChange(d->lines[(size_t)i]) || v.sel[(size_t)i]) continue;
        v.sel[(size_t)i] = 1;
        if (!v.selCount++) v.selAnchor = i;
    }
    if (!v.selCount) v.sel.clear();
    Emit(SessionEvent::Diff);
}

void RepoSession::ClearDiffSelection() {
    if (!diff.selCount && diff.sel.empty()) return;
    ClearSelection(diff);
    Emit(SessionEvent::Diff);
}

PatchJob RepoSession::HunkJob(int hunk, bool discard) const {
    PatchJob job;
    const DiffState& v = diff;
    const std::shared_ptr<FileDiff> d = v.diff;
    if (!d || v.req.section == 0 || (discard && v.req.section != 1) || hunk < 0 || hunk >= (int)d->hunks.size() || v.HunkBusy())
        return job;
    job.discard = discard;
    job.unstage = !discard && v.req.section == 2;
    job.what = L"hunk";
    job.patch = BuildPatch(*d, discard || v.req.section == 2, [&](int32_t i) { return d->lines[(size_t)i].hunk == hunk; });
    return job;
}

PatchJob RepoSession::LinesJob(bool discard) const {
    PatchJob job;
    const DiffState& v = diff;
    const std::shared_ptr<FileDiff> d = v.diff;
    if (!d || v.req.section == 0 || (discard && v.req.section != 1) || !v.selCount || v.HunkBusy()) return job;
    job.discard = discard;
    job.unstage = !discard && v.req.section == 2;
    job.what = v.selCount == 1 ? L"line" : L"lines";
    job.patch = BuildPatch(*d, discard || v.req.section == 2, [&](int32_t i) { return v.Selected(i); });
    return job;
}

// Runs git apply with a patch made from the open diff: on the index (staging, or unstaging with --reverse) or,
// to discard, backwards on the working tree.
void RepoSession::ApplyPatch(const PatchJob& job) {
    if (job.patch.empty() || !data) return;
    const std::wstring verb = job.discard ? L"discard" : job.unstage ? L"unstage" : L"stage";
    const std::wstring label = (job.discard ? L"Discarding " : job.unstage ? L"Unstaging " : L"Staging ") + job.what;
    const std::wstring fail = L"Couldn't " + verb + L" the " + job.what;
    const std::wstring tmp = WriteTempFile(job.patch);
    if (tmp.empty()) {
        Note(fail, L"A temporary file could not be written.", true);
        return;
    }
    diff.hunkOpSeq = diff.seq;  // until a load started from now on arrives, the lines shown are out of date
    ClearSelection(diff);
    // --recount: git counts the lines of each hunk itself
    const std::wstring args = std::wstring(L"apply ") + (job.discard ? L"--reverse" : job.unstage ? L"--cached --reverse" : L"--cached") +
                              L" --recount --whitespace=nowarn " + QuoteArg(tmp);
    RunOp(-1, label,
          [args, tmp, fail](const OpCtx& c) {
              OpOutcome o;
              const GitOpResult r = c.Run(args);
              RemoveFile(tmp);
              o.ok = r.ok();
              o.quiet = o.ok;
              o.statusOnly = true;
              if (!o.ok) {
                  o.title = fail;
                  o.message = GitErrorText(r);
              }
              return o;
          },
          true);
    Emit(SessionEvent::Diff);
}
