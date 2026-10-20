#pragma once
#include "session.h"

// What both apps say: the texts of the history, the details and the commit box, and the questions asked
// before actions, so the Windows and the macOS interface read the same.

wchar_t StatusLetter(wchar_t status);  // the letter shown for a file: M A D R C, U untracked, C conflict
const Ref* HeadRef(const RepoData& d);  // the checked-out branch, or null

std::wstring CommitSubject(const RepoData& d, const Commit& c);  // a stash's message, else the commit's subject
std::wstring CommitMeta(const RepoData& d, const Commit& c);     // "Ada · 5 minutes ago · " (the short id follows)
std::wstring LabelText(const RepoData& d, const Label& L);       // a branch or tag chip
std::wstring HistorySubtitle(const RepoData& d);                  // "main · 1,234 commits · fetched 5 minutes ago"
std::wstring WipSummary(const RepoData& d, const WorkStatus* st); // "3 unstaged · 1 staged · on main"
std::wstring LineDelta(int32_t added, int32_t deleted);           // "+3 −1"; empty when nothing (or unknown)
std::wstring FilesSummary(const CommitDetails& det);              // "12 files · +34 −5"
std::wstring CommitterNote(const RepoData& d, const Commit& c, const CommitDetails& det);  // "Committed by …", or empty
std::wstring CommitButtonLabel(size_t staged, bool amend);        // "Commit 2 files", "Amend previous commit", ...
std::wstring SearchStatus(const RepoSession& s);                  // "2 of 17", "17 matches", "No results"; empty when not searching
std::wstring CommitMessage(const RepoSession& s, int32_t commit); // the whole message once loaded, else the subject
std::wstring DiffWhere(const DiffState& m);   // "unstaged", "staged" or the commit's short id
std::wstring DiffNotice(const DiffState& m);  // what the diff view says instead of lines (loading, errors, binary or empty files); empty otherwise

// A question asked before an action that loses data, or that needs a choice.
struct Prompt {
    std::wstring title, message;
    std::vector<std::wstring> buttons;  // in reading order; the last one cancels
    int defaultButton = 0;              // the Enter key
    int danger = -1;                    // the button that loses data (shown as destructive), or -1
};
Prompt PushRejectedPrompt();                       // Pull / Force push / Cancel
Prompt DropStashPrompt(const StashEntry& s);       // Delete / Cancel
Prompt DiscardPrompt(const FileChange& f);         // Discard / Cancel
Prompt RevertPrompt(const RepoData& d, int32_t commit);  // Revert / Cancel
Prompt DiscardPatchPrompt(const PatchJob& job, const std::wstring& path);  // Discard / Cancel
Prompt RemoveAccountPrompt(const Account& a);      // Remove / Cancel

// Settings
const std::vector<std::wstring>& PullModeLabels();  // by Settings::pullMode
std::wstring PullModeHelp();
std::wstring AccountsIntro();   // what an account is for, and where its token is kept
std::wstring TrustedHelp();     // what the trusted folders are
