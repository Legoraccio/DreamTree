#include "present.h"
#include "platform.h"
#include "text.h"
#include <cstdlib>

wchar_t StatusLetter(wchar_t st) {
    switch (st) {
    case L'?': return L'U';  // untracked
    case L'U': return L'C';  // conflict
    case L'T': return L'M';  // type change
    default: return st;
    }
}

const Ref* HeadRef(const RepoData& d) {
    for (const Ref& r : d.refs)
        if (r.head && r.kind == RefKind::Local) return &r;
    return nullptr;
}

static const StashEntry* StashOf(const RepoData& d, const Commit& c) {
    if (!(c.flags & CF_STASH) || c.stash < 0 || c.stash >= (int32_t)d.stashes.size()) return nullptr;
    return &d.stashes[(size_t)c.stash];
}

std::wstring CommitSubject(const RepoData& d, const Commit& c) {
    if (const StashEntry* s = StashOf(d, c)) return s->message;
    return Widen(d.Subject(c));
}

std::wstring CommitMeta(const RepoData& d, const Commit& c) {
    const StashEntry* s = StashOf(d, c);
    return (s ? s->name : d.authors[c.author].name) + L" · " + FormatRelative(c.authorTime) + L" · ";
}

std::wstring LabelText(const RepoData& d, const Label& L) {
    // With a single remote, a remote branch alone shows its branch name: the cloud icon says where it is.
    if (!L.local && L.remote && d.remotes.size() <= 1 && L.ref >= 0) return d.refs[(size_t)L.ref].branch;
    return L.text;
}

std::wstring HistorySubtitle(const RepoData& d) {
    std::wstring s = !d.headBranch.empty() ? d.headBranch : d.detached ? L"HEAD detached at " + d.headOid.Hex(7) : L"no commits yet";
    s += L" · " + Plural(d.commits.size() - d.stashes.size(), L"commit", L"commits");
    if (!d.remotes.empty() && d.fetchTime > 0) s += L" · fetched " + FormatRelative(d.fetchTime);
    return s;
}

std::wstring WipSummary(const RepoData& d, const WorkStatus* st) {
    std::wstring s;
    if (st) {
        if (!st->unstaged.empty()) s = FormatCount(st->unstaged.size()) + L" unstaged";
        if (!st->staged.empty()) s += (s.empty() ? L"" : L" · ") + FormatCount(st->staged.size()) + L" staged";
    }
    if (!d.headBranch.empty()) s += (s.empty() ? L"on " : L" · on ") + d.headBranch;
    return s;
}

std::wstring LineDelta(int32_t added, int32_t deleted) {
    std::wstring s;
    if (added > 0) s = L"+" + std::to_wstring(added);
    if (deleted > 0) s += (s.empty() ? L"−" : L" −") + std::to_wstring(deleted);
    return s;
}

std::wstring FilesSummary(const CommitDetails& det) {
    std::wstring s = Plural(det.files.size(), L"file", L"files");
    if (det.added || det.deleted) s += L" · +" + std::to_wstring(det.added) + L" −" + std::to_wstring(det.deleted);
    return s;
}

std::wstring CommitterNote(const RepoData& d, const Commit& c, const CommitDetails& det) {
    if (!det.ok || StashOf(d, c)) return L"";
    if (det.committer == d.authors[c.author].name && std::llabs(det.commitTime - c.authorTime) <= 120) return L"";
    return L"Committed by " + det.committer + L" · " + FormatDateLong(det.commitTime);
}

std::wstring CommitButtonLabel(size_t staged, bool amend) {
    const std::wstring files = Plural(staged, L"file", L"files");
    if (amend) return staged ? L"Amend previous commit with " + files : std::wstring(L"Amend previous commit");
    return staged ? L"Commit " + files : std::wstring(L"Stage files to commit");
}

std::wstring SearchStatus(const RepoSession& s) {
    if (!s.Searching() || !s.graph) return L"";
    if (s.searchCount == 0) return L"No results";
    const int32_t k = s.MatchOrdinal();  // the selected match among all of them, in history order
    return k ? std::to_wstring(k) + L" of " + FormatCount((size_t)s.searchCount) : Plural((size_t)s.searchCount, L"match", L"matches");
}

std::wstring CommitMessage(const RepoSession& s, int32_t commit) {
    if (!s.data || commit < 0 || commit >= (int32_t)s.data->commits.size()) return L"";
    const Commit& c = s.data->commits[(size_t)commit];
    if (s.details && s.details->ok && s.details->id == c.id && !StashOf(*s.data, c)) {
        std::wstring m = s.details->summary;
        if (!s.details->body.empty()) m += L"\n\n" + s.details->body;
        return m;
    }
    return CommitSubject(*s.data, c);
}

std::wstring DiffWhere(const DiffState& m) {
    return m.req.section == 1 ? L"unstaged" : m.req.section == 2 ? L"staged" : m.req.commit.Hex(7);
}

std::wstring DiffNotice(const DiffState& m) {
    const FileDiff* d = m.diff.get();
    if (!d) return L"Loading diff…";
    if (!d->ok) return d->error.empty() ? std::wstring(L"The diff could not be loaded.") : d->error;
    if (d->binary) return d->newFile ? L"Binary file added." : d->deletedFile ? L"Binary file deleted." : L"Binary file changed.";
    if (!d->lines.empty()) return L"";
    if (!d->modeChange.empty()) return L"Only the file mode changed: " + d->modeChange;
    return d->newFile ? L"New empty file." : d->deletedFile ? L"Empty file deleted." : L"No changes.";
}

// ---------------------------------------------------------------- questions

Prompt PushRejectedPrompt() {
    Prompt p;
    p.title = L"Push rejected";
    p.message = L"The remote branch has commits that your local branch does not have.\n\n"
                L"Pull them first, or force push to replace the remote branch with yours. "
                L"Force push uses --force-with-lease, so it stops if someone else pushed in the meantime.";
    p.buttons = { L"Pull", L"Force push", L"Cancel" };
    p.danger = 1;
    return p;
}

Prompt DropStashPrompt(const StashEntry& s) {
    Prompt p;
    p.title = L"Delete stash?";
    p.message = L"“" + s.message + L"” (" + s.name + L") will be deleted. This can't be undone.";
    p.buttons = { L"Delete", L"Cancel" };
    p.defaultButton = 1;
    p.danger = 0;
    return p;
}

Prompt DiscardPrompt(const FileChange& f) {
    Prompt p;
    p.title = L"Discard changes?";
    p.message = f.status == L'?' ? L"“" + f.path + L"” is not tracked by Git, so it will be deleted from the disk. This can't be undone."
                                 : L"Your unstaged changes to “" + f.path + L"” will be lost. This can't be undone.";
    p.buttons = { L"Discard", L"Cancel" };
    p.defaultButton = 1;
    p.danger = 0;
    return p;
}

Prompt RevertPrompt(const RepoData& d, int32_t commit) {
    const Commit& c = d.commits[(size_t)commit];
    const std::wstring where = d.headBranch.empty() ? std::wstring(L"the detached HEAD") : L"'" + d.headBranch + L"'";
    Prompt p;
    p.title = L"Revert this commit?";
    p.message = L"A new commit on " + where + L" will undo the changes of “" + Widen(d.Subject(c)) + L"” (" + c.id.Hex(7) + L")." +
                (c.parentCount > 1 ? L"\n\nThis is a merge: the changes it brought in from the merged branch are undone." : L"");
    p.buttons = { L"Revert", L"Cancel" };
    return p;
}

Prompt RemoveAccountPrompt(const Account& a) {
    Prompt p;
    p.title = std::wstring(L"Remove the ") + InfoOf(a.forge).name + L" account " + a.login + L"?";
    p.message = L"Mangrove forgets the account and deletes its token from " + SecretStoreName() + L". The token stays valid on " + a.Host() +
                L": revoke it there if you no longer need it.";
    p.buttons = { L"Remove", L"Cancel" };
    p.defaultButton = 1;
    p.danger = 0;
    return p;
}

// ---------------------------------------------------------------- settings

const std::vector<std::wstring>& PullModeLabels() {
    static const std::vector<std::wstring> labels = { L"Merge (fast-forward if possible)", L"Fast-forward only", L"Rebase" };
    return labels;
}

std::wstring PullModeHelp() {
    return L"How Pull brings in the commits of the upstream branch. Uncommitted changes are set aside and put back afterwards.";
}

std::wstring AccountsIntro() {
    return L"Connect your accounts to fetch, pull and push over HTTPS without being asked for a password. Each token is kept in " +
           SecretStoreName() + L", never in Mangrove's files, and is sent only to its own server.";
}

std::wstring TrustedHelp() {
    return L"Repositories in these folders belong to another user account; Mangrove opens them anyway (git's safe.directory, for "
           L"Mangrove only). Folders are added with “Trust” when Mangrove can't open a repository.";
}

Prompt DiscardPatchPrompt(const PatchJob& job, const std::wstring& path) {
    const bool hunk = job.what == L"hunk";
    Prompt p;
    p.title = hunk ? L"Discard this hunk?" : job.what == L"line" ? L"Discard this line?" : L"Discard these lines?";
    p.message = (hunk ? std::wstring(L"The changes of this hunk in “") : L"Your changes on the selected " + job.what + L" of “") + path +
                L"” will be lost. This can't be undone.";
    p.buttons = { L"Discard", L"Cancel" };
    p.defaultButton = 1;
    p.danger = 0;
    return p;
}
