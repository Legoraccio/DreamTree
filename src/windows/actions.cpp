// What the Windows interface adds to the git operations the sessions run (src\core\session.cpp): text
// fields, confirmations before destructive actions, notifications and the modal dialog.

#include "app.h"
#include "theme.h"
#include <cwctype>

// ============================================================================ text editing

static bool IsWordChar(wchar_t c) { return std::iswalnum((std::wint_t)c) || c == L'_'; }

void TextEdit::Insert(std::wstring_view s) {
    caret = std::min(caret, text.size());
    anchor = std::min(anchor, text.size());
    const size_t a = SelStart(), b = SelEnd();
    text.erase(a, b - a);
    std::wstring add;
    for (wchar_t c : s) {
        if (c == L'\r') continue;
        if (c == L'\n' && !multiline) c = L' ';
        if (c == L'\t') c = L' ';
        add.push_back(c);
    }
    if (text.size() + add.size() > maxLen) add.resize(maxLen > text.size() ? maxLen - text.size() : 0);
    text.insert(a, add);
    caret = anchor = a + add.size();
}

size_t TextEdit::HitTest(float x, float y) const {
    if (!layout) return text.size();
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS m{};
    if (FAILED(layout->HitTestPoint(x - origin.x, y - origin.y, &trailing, &inside, &m))) return text.size();
    return std::min<size_t>(m.textPosition + (trailing ? m.length : 0), text.size());
}

void TextEdit::SelectWordAt(size_t i) {
    i = std::min(i, text.size());
    size_t a = i, b = i;
    while (a && IsWordChar(text[a - 1])) --a;
    while (b < text.size() && IsWordChar(text[b])) ++b;
    if (a == b && b < text.size()) ++b;  // not on a word: select the character
    anchor = a;
    caret = b;
}

bool TextEdit::OnKey(UINT vk, bool ctrl, bool shift, HWND owner) {
    caret = std::min(caret, text.size());
    anchor = std::min(anchor, text.size());
    auto prevChar = [&](size_t i) { return (i >= 2 && IS_LOW_SURROGATE(text[i - 1]) && IS_HIGH_SURROGATE(text[i - 2])) ? i - 2 : (i ? i - 1 : 0); };
    auto nextChar = [&](size_t i) {
        if (i + 1 < text.size() && IS_HIGH_SURROGATE(text[i]) && IS_LOW_SURROGATE(text[i + 1])) return i + 2;
        return std::min(text.size(), i + 1);
    };
    auto prevStop = [&](size_t i) {
        while (i && !IsWordChar(text[i - 1])) --i;
        while (i && IsWordChar(text[i - 1])) --i;
        return i;
    };
    auto nextStop = [&](size_t i) {
        while (i < text.size() && !IsWordChar(text[i])) ++i;
        while (i < text.size() && IsWordChar(text[i])) ++i;
        return i;
    };
    auto moveTo = [&](size_t i) {
        caret = i;
        if (!shift) anchor = caret;
        return true;
    };
    switch (vk) {
    case VK_LEFT:
        if (HasSel() && !shift) return moveTo(SelStart());
        return moveTo(ctrl ? prevStop(caret) : prevChar(caret));
    case VK_RIGHT:
        if (HasSel() && !shift) return moveTo(SelEnd());
        return moveTo(ctrl ? nextStop(caret) : nextChar(caret));
    case VK_HOME:
        if (ctrl || !multiline) return moveTo(0);
        {
            const size_t nl = caret ? text.rfind(L'\n', caret - 1) : std::wstring::npos;
            return moveTo(nl == std::wstring::npos ? 0 : nl + 1);
        }
    case VK_END:
        if (ctrl || !multiline) return moveTo(text.size());
        {
            const size_t nl = text.find(L'\n', caret);
            return moveTo(nl == std::wstring::npos ? text.size() : nl);
        }
    case VK_UP:
    case VK_DOWN: {
        if (!multiline || !layout) return false;
        FLOAT px = 0, py = 0;
        DWRITE_HIT_TEST_METRICS m{};
        layout->HitTestTextPosition((UINT32)caret, FALSE, &px, &py, &m);
        const float ty = vk == VK_UP ? py - m.height * 0.5f : py + m.height * 1.5f;
        if (ty < 0) return moveTo(0);
        BOOL trailing = FALSE, inside = FALSE;
        DWRITE_HIT_TEST_METRICS hm{};
        layout->HitTestPoint(px, ty, &trailing, &inside, &hm);
        return moveTo(std::min<size_t>(hm.textPosition + (trailing ? hm.length : 0), text.size()));
    }
    case VK_BACK:
        if (HasSel()) Insert(L"");
        else if (caret) {
            const size_t from = ctrl ? prevStop(caret) : prevChar(caret);
            text.erase(from, caret - from);
            caret = anchor = from;
        }
        return true;
    case VK_DELETE:
        if (HasSel()) Insert(L"");
        else if (caret < text.size()) {
            text.erase(caret, (ctrl ? nextStop(caret) : nextChar(caret)) - caret);
            anchor = caret;
        }
        return true;
    case 'A':
        if (!ctrl) return false;
        SelectAll();
        return true;
    case 'C':
    case 'X': {
        if (!ctrl) return false;
        if (HasSel()) {
            std::wstring s = text.substr(SelStart(), SelEnd() - SelStart()), out;
            for (wchar_t c : s) {  // the clipboard expects CRLF line ends
                if (c == L'\n') out += L'\r';
                out += c;
            }
            CopyToClipboard(owner, out);
            if (vk == 'X') Insert(L"");
        }
        return true;
    }
    case 'V':
        if (!ctrl) return false;
        if (OpenClipboard(owner)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                    std::wstring s(p);
                    GlobalUnlock(h);
                    Insert(s);
                }
            }
            CloseClipboard();
        }
        return true;
    default:
        return false;
    }
}

TextEdit* App::EditById(int id) {
    if (id == 3) return &inlineInput.edit;
    RepoTab* t = Active();
    if (!t) return nullptr;
    if (id == 1) return &t->commitSummary;
    if (id == 2) return &t->commitBody;
    if (id == 4) return &t->search;
    return nullptr;
}

void App::FocusEdit(int id) {
    if (id) {
        CancelBranchInput();
        searchFocus = false;
    }
    editFocus = id;
    Invalidate();
}

void CopyToClipboard(HWND h, const std::wstring& text) {
    std::wstring s;  // Windows line breaks
    s.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) s.push_back(L'\r');
        s.push_back(text[i]);
    }
    if (!OpenClipboard(h)) return;
    EmptyClipboard();
    const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(g), s.c_str(), bytes);
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

// ============================================================================ what the sessions report

void App::OnSessionEvent(RepoTab& t, SessionEvent e) {
    switch (e) {
    case SessionEvent::Loaded:
        t.hoverRow = -1;
        t.sideDirty = true;
        t.fileRowsSrc[0] = t.fileRowsSrc[1] = t.fileRowsSrc[2] = nullptr;
        if (&t == Active()) refPopup = {};
        if (t.data) {
            if (!t.lastLoadRefresh) {
                t.gs.To(0, true);
                t.ss.To(0, true);
                t.ds.To(0, true);
                t.collapsed = { L"L", L"T", L"S", L"R", L"M" };  // the sections of the navigation pane start closed
            }
            SaveSettings();  // the repository moved to the top of the recent list
        }
        break;
    case SessionEvent::Status:
        t.fileRowsSrc[1] = t.fileRowsSrc[2] = nullptr;
        if (t.wipRowDelta && t.data) {
            // The row of the uncommitted changes appeared or went away at the top: what is on screen stays put.
            const float rowH = DR(kGraphRowH);
            const bool atTop = t.gs.target < rowH * 0.5f;
            if (t.wipRowDelta > 0) {
                if (!atTop) {
                    t.gs.pos += rowH;
                    t.gs.target += rowH;
                }
                if (t.selCommit == t.data->headCommit && atTop) {
                    t.selWip = true;
                    t.selCommit = -1;
                }
            } else {
                t.gs.pos = std::max(0.0, t.gs.pos - rowH);
                t.gs.target = std::max(0.0, t.gs.target - rowH);
            }
            t.wipRowDelta = 0;
        }
        break;
    case SessionEvent::Diff:
        if (!t.diff.open) {
            t.diffUi.layouts.clear();
            t.diffUi.layoutsFor = nullptr;
        }
        break;
    case SessionEvent::Notice:
        ShowToast(t.notice.title, t.notice.message, t.notice.error);
        break;
    case SessionEvent::Message:
        if (!t.commitSummary.text.empty() || !t.commitBody.text.empty()) break;  // never over what was typed
        if (t.suggestion.amend && !t.amend) break;
        t.commitSummary.Set(t.suggestion.summary);
        t.commitBody.Set(t.suggestion.body);
        if (t.suggestion.amend) t.amendPrefill = t.suggestion.summary + L"\n" + t.suggestion.body;
        break;
    case SessionEvent::Committed:
        t.commitSummary.Set(L"");
        t.commitBody.Set(L"");
        t.amend = false;
        t.amendPrefill.clear();
        break;
    case SessionEvent::PushRejected: {
        std::weak_ptr<RepoTab> wk = PtrOf(&t);
        ShowDialog(PushRejectedPrompt(), [wk](int b) {
                       auto tab = wk.lock();
                       if (!tab) return;
                       if (b == 0) tab->Pull();
                       else if (b == 1) tab->Push(true);
                   });
        break;
    }
    default:
        break;
    }
    Invalidate();
}

// ============================================================================ confirmations

void App::DoDropStash(int index) {
    auto t = ActivePtr();
    if (!t || !t->data || t->opBusy || index < 0 || index >= (int)t->data->stashes.size()) return;
    const StashEntry s = t->data->stashes[(size_t)index];
    std::weak_ptr<RepoTab> wk = t;
    ShowDialog(DropStashPrompt(s), [wk, index](int b) {
                   if (auto tab = wk.lock(); tab && b == 0) tab->DropStash(index);
               });
}

void App::DoDiscard(RepoTab& t, const FileChange& f) {
    if (!t.data || f.status == L'U') return;
    std::weak_ptr<RepoTab> wk = PtrOf(&t);
    ShowDialog(DiscardPrompt(f), [wk, f](int b) {
                   if (auto tab = wk.lock(); tab && b == 0) tab->Discard(f);
               });
}

void App::DoRevert(RepoTab& t, int32_t commit) {
    const std::wstring why = t.CanRevert(commit);
    if (!why.empty()) {
        ShowToast(L"Can't revert", why, true);
        return;
    }
    std::weak_ptr<RepoTab> wk = PtrOf(&t);
    ShowDialog(RevertPrompt(*t.data, commit), [wk, commit](int b) {
                   if (auto tab = wk.lock(); tab && b == 0) tab->Revert(commit);
               });
}

// ============================================================================ commit box

void App::DoCommit() {
    auto t = ActivePtr();
    if (!t) return;
    const RepoSession::CommitResult r = t->Commit(t->commitSummary.text, t->commitBody.text, t->amend);
    if (r == RepoSession::CommitNoSummary) FocusEdit(1);
    else if (r == RepoSession::CommitStarted) FocusEdit(0);
}

void App::ToggleAmend(RepoTab& t) {
    if (!t.data || t.data->unborn) return;
    t.amend = !t.amend;
    if (t.amend) {
        // start from the message being amended (it arrives as a suggestion)
        if (t.commitSummary.text.empty() && t.commitBody.text.empty()) t.RequestHeadMessage();
    } else {
        if (!t.amendPrefill.empty() && t.commitSummary.text + L"\n" + t.commitBody.text == t.amendPrefill) {
            t.commitSummary.Set(L"");
            t.commitBody.Set(L"");
        }
        t.amendPrefill.clear();
    }
    Invalidate();
}

// ============================================================================ new branch (inline input)

void App::BeginBranchInput(RepoTab& t, int32_t commit) {
    if (!t.data || !t.graph || t.opBusy || t.data->commits.empty()) return;
    inlineInput = InlineInput{};
    inlineInput.active = true;
    inlineInput.tab = PtrOf(&t);
    inlineInput.commit = commit;
    searchFocus = false;
    const Graph& g = *t.graph;
    int32_t row = -1;
    if (commit >= 0 && commit < (int32_t)g.rowOfCommit.size()) row = g.rowOfCommit[(size_t)commit];
    else if (g.hasWip) row = 0;
    else if (t.data->headCommit >= 0) row = g.rowOfCommit[(size_t)t.data->headCommit];
    if (row >= 0) EnsureRowVisible(t, row);
    Invalidate();
}

void App::CancelBranchInput() {
    if (!inlineInput.active) return;
    inlineInput.active = false;
    inlineInput.error.clear();
    Invalidate();
}

void App::CommitBranchInput() {
    auto t = inlineInput.tab.lock();
    if (!t || !t->data || t->opBusy) {
        CancelBranchInput();
        return;
    }
    const std::wstring err = t->CreateBranch(inlineInput.edit.text, inlineInput.commit);
    if (!err.empty()) {
        inlineInput.error = err;
        Invalidate();
        return;
    }
    inlineInput.active = false;
    Invalidate();
}

// ============================================================================ notifications and dialog

void App::ShowToast(const std::wstring& title, const std::wstring& message, bool error) {
    if (title.empty() && message.empty()) return;
    toasts.push_back({ title, message, error, NowMs(), error ? 0.0 : 4500.0 });
    if (toasts.size() > 4) toasts.erase(toasts.begin());
    SetTimer(hwnd, 1, 200, nullptr);
    Invalidate();
}

void App::ShowDialog(const std::wstring& title, const std::wstring& message, std::vector<std::wstring> buttons, int defButton,
                     int danger, std::function<void(int)> onResult) {
    CancelBranchInput();
    searchFocus = false;
    dialog.active = true;
    dialog.title = title;
    dialog.message = message;
    dialog.buttons = std::move(buttons);
    dialog.defButton = defButton;
    dialog.danger = danger;
    dialog.onResult = std::move(onResult);
    hover = {};
    Invalidate();
}

void App::CloseDialog(int result) {
    std::function<void(int)> cb = std::move(dialog.onResult);
    dialog = DialogState{};
    hover = {};
    Invalidate();
    if (cb) cb(result);
}
