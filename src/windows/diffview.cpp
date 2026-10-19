// Diff view: the changes of one file, shown in place of the history.
//
// The diff itself, its loading and its line selection belong to the session (RepoSession::diff, shared with
// the macOS app); this file draws it. Rows have a fixed height and only the visible ones are drawn. Each
// line's text layout is created the first time the line is on screen and kept while the diff is open, so
// scrolling through very large diffs stays cheap.

#include "app.h"
#include "theme.h"

using namespace th;

namespace {
constexpr float kRowH = 20.f;
constexpr float kHeaderH = 44.f;

int Digits(int n) {
    int d = 1;
    while (n >= 10) { n /= 10; ++d; }
    return d;
}
}  // namespace

// ============================================================================ state

void App::OpenDiff(RepoTab& t, int section, const std::wstring& path) {
    if (t.OpenDiff(section, path)) t.diffUi = DiffViewUi{};  // another file: from the top
    Invalidate();
}

void App::CloseDiff(RepoTab& t) {
    t.CloseDiff();
    Invalidate();
}

void App::SetDiffMode(bool split) {
    if (diffSplit == split) return;
    // Keep the same lines at the top of every open diff.
    const float rowH = DR(kRowH);
    for (const auto& tp : tabs) {
        DiffViewUi& v = tp->diffUi;
        const std::shared_ptr<FileDiff> d = tp->diff.diff;
        if (!tp->diff.open || !d || d->split.empty() || d->lines.empty() || !ShowSplit(true, *d)) continue;
        const int top = (int)(v.vs.target / rowH);
        int row = 0;
        if (split) {
            row = SplitRowOfLine(*d, std::min(top, (int)d->lines.size() - 1));
        } else {
            const SplitRow& r = d->split[(size_t)std::min(top, (int)d->split.size() - 1)];
            row = std::max(0, r.left >= 0 ? r.left : r.right);
        }
        v.vs.content = (double)(split ? d->split.size() : d->lines.size()) * rowH + rowH * 2;
        v.vs.To((double)row * rowH, true);
    }
    diffSplit = split;
    SaveSettings();
    Invalidate();
}

void App::DiffJump(RepoTab& t, int dir) {
    DiffViewUi& v = t.diffUi;
    const std::shared_ptr<FileDiff> d = t.diff.diff;
    if (!d || d->hunks.empty()) return;
    const float rowH = DR(kRowH);
    const double top = v.vs.target / rowH + 1.0;  // a jump leaves one row above the hunk header
    const bool split = ShowSplit(diffSplit, *d);
    int target = -1;
    for (const DiffHunk& h : d->hunks) {
        const int row = split ? h.splitRow : h.line;
        if (row < 0) continue;
        if (dir > 0 && row > top + 0.5) { target = row; break; }
        if (dir < 0 && row < top - 0.5) target = row;
    }
    if (target >= 0) v.vs.To((target - 1) * (double)rowH, false);
    Invalidate();
}

bool App::DiffKey(RepoTab& t, UINT vk) {
    DiffViewUi& v = t.diffUi;
    if (!t.diff.open) return false;
    const float rowH = DR(kRowH);
    switch (vk) {
    case VK_ESCAPE:  // first drops the line selection
        if (t.diff.selCount) ClearDiffSelection(t);
        else CloseDiff(t);
        return true;
    case VK_UP: v.vs.By(-rowH * 2); break;
    case VK_DOWN: v.vs.By(rowH * 2); break;
    case VK_PRIOR: v.vs.By(-v.vs.view * 0.9); break;
    case VK_NEXT: v.vs.By(v.vs.view * 0.9); break;
    case VK_HOME: v.vs.To(0, false); break;
    case VK_END: v.vs.To(v.vs.content, false); break;
    case VK_LEFT: v.hscroll = std::max(0.f, v.hscroll - gfx.codeCharW * 8); break;
    case VK_RIGHT: v.hscroll += gfx.codeCharW * 8; break;
    default: return false;
    }
    Invalidate();
    return true;
}

void App::SelectDiffLine(RepoTab& t, int32_t line, bool extend, bool toggle) {
    t.SelectDiffLine(line, extend, toggle);
    Invalidate();
}

void App::DragDiffSelection(RepoTab& t, float x, float y) {
    DiffViewUi& v = t.diffUi;
    // Past the top or bottom edge the view scrolls; the line at the edge then joins the selection.
    if (y < v.body.y) v.vs.By(-DR(kRowH));
    else if (y > v.body.b()) v.vs.By(DR(kRowH));
    const Zone* z = ZoneAt(x, std::clamp(y, v.body.y + 1, v.body.b() - 1));
    if (z && z->id == Hit::DiffLine) SelectDiffLine(t, z->a, true, false);
    Invalidate();
}

void App::ClearDiffSelection(RepoTab& t) {
    t.ClearDiffSelection();
    Invalidate();
}

void App::DoHunkAction(RepoTab& t, int hunk, bool discard) { ApplyDiffPatch(t, t.HunkJob(hunk, discard)); }

void App::DoLinesAction(RepoTab& t, bool discard) { ApplyDiffPatch(t, t.LinesJob(discard)); }

void App::ApplyDiffPatch(RepoTab& t, const PatchJob& job) {
    if (job.patch.empty()) return;
    if (!job.discard) {
        t.ApplyPatch(job);
        Invalidate();
        return;
    }
    std::weak_ptr<RepoTab> wk = PtrOf(&t);
    ShowDialog(DiscardPatchPrompt(job, t.diff.req.path), [wk, job](int b) {
                   if (auto tab = wk.lock(); tab && b == 0) tab->ApplyPatch(job);
               });
}

// ============================================================================ painting

void App::PaintDiff(RepoTab& t) {
    const DiffState& m = t.diff;
    DiffViewUi& v = t.diffUi;
    const RectF a = rcGraph;  // on the content card, which is already painted
    const std::shared_ptr<FileDiff> d = m.diff;
    const bool wip = m.req.section > 0;

    // ---- header: controls laid out from the right, then the file; narrow views drop the extras first
    const RectF hr{ a.x, a.y, a.w, DR(kHeaderH) };
    gfx.HLine(a.x, a.r(), hr.b() - 1, DividerSoft);
    const bool compact = hr.w < DR(820);
    float rx = hr.r() - DR(12);
    {
        const float w = compact ? DR(30) : gfx.Measure(L"Close", F::UISemi) + DR(40);
        const RectF b{ rx - w, hr.cy() - DR(14), w, DR(28) };
        gfx.FillR(b, DR(5), Hov(Hit::DiffClose) ? SubtleHover : Subtle);
        gfx.StrokeR(b, DR(5), DividerSoft);
        if (compact) {
            gfx.Glyph(0xE711, b, F::Icon10, Text);
        } else {
            gfx.Glyph(0xE711, { b.x + DR(6), b.y, DR(22), b.h }, F::Icon10, Text);
            gfx.Text(L"Close", { b.x + DR(28), b.y, w - DR(32), b.h }, F::UISemi, Text);
        }
        AddZone(b, Hit::DiffClose);
        if (Hov(Hit::DiffClose)) { tipText = L"Close (Esc)"; tipAnchor = b; }
        rx = b.x - DR(10);
    }
    {
        const float sw = compact ? DR(124) : DR(160);
        const RectF seg{ rx - sw, hr.cy() - DR(14), sw, DR(28) };
        gfx.FillR(seg, DR(5), InputBg);
        for (int i = 0; i < 2; ++i) {
            const RectF h{ seg.x + (float)i * seg.w * 0.5f, seg.y, seg.w * 0.5f, seg.h };
            const bool on = (i == 1) == diffSplit;
            if (on) gfx.FillR(h.Inset(DR(2), DR(2)), DR(4), AccentDim);
            else if (Hov(Hit::DiffMode, i)) gfx.FillR(h.Inset(DR(2), DR(2)), DR(4), Hover);
            gfx.Text(i == 0 ? L"Unified" : L"Split", h, compact ? F::SmallSemi : F::UISemi, on ? AccentText : TextSec, DWRITE_TEXT_ALIGNMENT_CENTER);
            AddZone(h, Hit::DiffMode, i);
        }
        gfx.StrokeR(seg, DR(5), DividerSoft);
        rx = seg.x - DR(10);
    }
    {
        const bool any = d && d->hunks.size() > 1;
        for (const int dir : { 1, -1 }) {
            const RectF b{ rx - DR(28), hr.cy() - DR(14), DR(28), DR(28) };
            if (any && Hov(Hit::DiffNav, dir)) gfx.FillR(b, DR(5), Hover);
            gfx.Glyph(dir > 0 ? 0xE70D : 0xE70E, b, F::Icon12, any ? Text : TextDim);
            AddZone(b, Hit::DiffNav, dir);
            if (Hov(Hit::DiffNav, dir)) { tipText = dir > 0 ? L"Next change" : L"Previous change"; tipAnchor = b; }
            rx = b.x - DR(2);
        }
        rx -= DR(10);
    }
    if (wip) {
        const bool stage = m.req.section == 1;
        const wchar_t* label = stage ? (compact ? L"Stage" : L"Stage file") : (compact ? L"Unstage" : L"Unstage file");
        const Color c = stage ? Added : Deleted;
        const float w = gfx.Measure(label, F::SmallSemi) + DR(24);
        const RectF b{ rx - w, hr.cy() - DR(13), w, DR(26) };
        gfx.FillR(b, DR(4), A(c, Hov(Hit::DiffFileStage) ? 0.26f : 0.10f));
        gfx.StrokeR(b, DR(4), A(c, 0.8f));
        gfx.Text(label, b, F::SmallSemi, TintText(c), DWRITE_TEXT_ALIGNMENT_CENTER);
        AddZone(b, Hit::DiffFileStage);
        rx = b.x - DR(14);
    }
    if (float x = hr.x + DR(16); rx - x >= DR(60)) {  // else no room is left for the file
        FileIcon(m.req.status, x + DR(7), hr.cy());
        x += DR(24);
        float room = rx - x;
        // +added / -deleted, right-aligned against the controls
        if (d && d->ok && !d->binary && !d->lines.empty()) {
            const std::wstring plus = L"+" + std::to_wstring(d->added), minus = L"−" + std::to_wstring(d->deleted);
            const float pw = gfx.Measure(plus, F::SmallSemi), mw = gfx.Measure(minus, F::SmallSemi);
            const float statsW = pw + mw + DR(10);
            if (room - statsW >= DR(160)) {
                gfx.Text(plus, { rx - statsW, hr.y, pw + 2, hr.h }, F::SmallSemi, DiffAddMark);
                gfx.Text(minus, { rx - mw, hr.y, mw + 2, hr.h }, F::SmallSemi, DiffDelMark);
                room -= statsW + DR(12);
            }
        }
        const std::wstring where = DiffWhere(m);
        const float ww = gfx.Measure(where, F::MonoSmall) + DR(14);
        if (room - ww - DR(10) >= DR(110)) {
            const RectF tag{ x, hr.cy() - DR(10), ww, DR(20) };
            gfx.FillR(tag, DR(3), m.req.section == 0 ? AccentDim : Subtle);
            gfx.Text(where, tag, F::MonoSmall, m.req.section == 0 ? AccentText : TextSec, DWRITE_TEXT_ALIGNMENT_CENTER);
            x = tag.r() + DR(10);
            room -= ww + DR(10);
        }
        const std::wstring& path = m.req.path;
        const size_t slash = path.find_last_of(L'/');
        std::wstring dir = slash == std::wstring::npos ? L"" : path.substr(0, slash + 1);
        const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
        std::wstring old = m.req.oldPath;
        if (old.empty() && d) old = d->oldPath;
        if (!old.empty() && old != path) dir = old + L"  →  " + dir;
        const float nw = std::min(gfx.Measure(name, F::UISemi), room);
        const float dw = std::max(0.f, std::min(gfx.Measure(dir, F::UI), room - nw));
        if (dw > DR(20)) gfx.Text(dir, { x, hr.y, dw, hr.h }, F::UI, TextDim);
        gfx.Text(name, { x + (dw > DR(20) ? dw : 0.f), hr.y, nw + 2, hr.h }, F::UISemi, Text);
    }

    // ---- body
    const RectF body{ a.x, hr.b(), a.w, a.b() - hr.b() };
    AddZone(body, Hit::DiffBody);
    auto message = [&](const std::wstring& s, const Color& c) { gfx.Text(s, body, F::UI, c, DWRITE_TEXT_ALIGNMENT_CENTER); };
    if (!d) {
        animating = true;
        if (NowMs() - m.loadStart < 150) return;  // most diffs arrive before a spinner is worth showing
        Spinner(body.cx(), body.y + body.h * 0.4f, DR(12));
        gfx.Text(DiffNotice(m), { body.x, body.y + body.h * 0.4f + DR(22), body.w, DR(22) }, F::UI, TextSec,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    if (const std::wstring notice = DiffNotice(m); !notice.empty()) {  // errors, binary and empty files
        message(notice, d->ok ? TextSec : Deleted);
        return;
    }
    if (v.layoutsFor != d.get() || v.layoutsScale != g_S || v.layouts.size() > 4000) {
        v.layouts.clear();
        v.layoutsFor = d.get();
        v.layoutsScale = g_S;
    }
    IDWriteTextFormat* code = gfx.Fmt(F::Code);
    const float rowH = DR(kRowH);
    const bool split = ShowSplit(diffSplit, *d);
    const size_t rows = split ? d->split.size() : d->lines.size();
    v.vs.view = body.h;
    v.vs.content = (double)rows * rowH + rowH * 2;
    v.vs.Clamp();
    const float cw = gfx.codeCharW;
    const float numW = std::round((float)Digits(std::max(999, d->maxLineNo)) * cw + DR(16));
    const float markW = DR(18);
    const int first = std::max(0, (int)(v.vs.pos / rowH));
    const int last = std::min((int)rows, (int)((v.vs.pos + body.h) / rowH) + 1);
    auto rowY = [&](int r) { return (float)std::round((double)body.y + (double)r * rowH - v.vs.pos); };
    auto layoutOf = [&](int idx) -> IDWriteTextLayout* {
        if (auto it = v.layouts.find((uint32_t)idx); it != v.layouts.end()) return it->second.Get();
        const DiffLine& L = d->lines[(size_t)idx];
        ComPtr<IDWriteTextLayout> lay;
        if (FAILED(gfx.dwrite()->CreateTextLayout(d->text.data() + L.off, L.len, code, 1e6f, rowH, &lay))) return nullptr;
        IDWriteTextLayout* p = lay.Get();
        v.layouts.emplace((uint32_t)idx, std::move(lay));
        return p;
    };
    float noEolW = 0;  // measured on first use
    auto drawCode = [&](int idx, float x, float y) {
        const DiffLine& L = d->lines[(size_t)idx];
        const D2D1_POINT_2F org{ x - v.hscroll, y };
        float end = org.x;
        if (IDWriteTextLayout* lay = L.len ? layoutOf(idx) : nullptr) {
            if (L.hlLen && (L.kind == DiffLine::Added || L.kind == DiffLine::Deleted)) {
                DWRITE_HIT_TEST_METRICS m[8];
                UINT32 n = 0;
                if (SUCCEEDED(lay->HitTestTextRange(L.hlStart, L.hlLen, org.x, org.y, m, 8, &n)))
                    for (UINT32 i = 0; i < n; ++i)
                        gfx.FillR({ m[i].left, y + DR(2), std::max(m[i].width, DR(3)), rowH - DR(4) }, DR(2),
                                  L.kind == DiffLine::Added ? DiffAddHl : DiffDelHl);
            }
            gfx.dc()->DrawTextLayout(org, lay, gfx.B(L.kind == DiffLine::Context ? CodeText : Text), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            if (L.noEol) {
                DWRITE_TEXT_METRICS tm{};
                lay->GetMetrics(&tm);
                end += tm.widthIncludingTrailingWhitespace;
            }
        }
        if (L.noEol) {  // what git prints as "\ No newline at end of file"
            const wchar_t* s = L"no newline at end of file";
            if (noEolW == 0) noEolW = gfx.Measure(s, F::Tiny) + DR(14);
            const RectF pill{ end + DR(10), y + DR(3), noEolW, rowH - DR(6) };
            gfx.FillR(pill, DR(3), Subtle);
            gfx.Text(s, pill, F::Tiny, TextSec, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    };
    auto number = [&](int n, float x, float y) {
        if (n >= 0) gfx.Text(std::to_wstring(n), { x, y, numW - DR(8), rowH }, F::MonoSmall, TextDim, DWRITE_TEXT_ALIGNMENT_TRAILING);
    };
    auto marker = [&](const DiffLine& L, float x, float y) {
        if (L.kind == DiffLine::Added) gfx.Text(L"+", { x, y, markW, rowH }, F::Code, DiffAddMark, DWRITE_TEXT_ALIGNMENT_CENTER);
        else if (L.kind == DiffLine::Deleted) gfx.Text(L"−", { x, y, markW, rowH }, F::Code, DiffDelMark, DWRITE_TEXT_ALIGNMENT_CENTER);
    };
    // Changes of the working tree or of the index can be applied back hunk by hunk or line by line.
    const bool canApply = wip && CanApplyBack(*d);
    const bool hunkBusy = m.HunkBusy();
    // Selection and hover of a changed line, and its zone for clicks and drag selection.
    auto lineState = [&](int idx, const RectF& r, float barX) {
        if (!canApply || !IsChange(d->lines[(size_t)idx])) return;
        if ((size_t)idx < m.sel.size() && m.sel[(size_t)idx]) {
            gfx.Fill(r, LineSelected);
            gfx.Fill({ barX, r.y, DR(3), r.h }, Accent);
        } else if (Hov(Hit::DiffLine, idx) && drag != Drag::DiffSelect) {
            gfx.Fill(r, Hover);
        }
        AddZone(r, Hit::DiffLine, idx);
    };
    // Hunk headers span the whole width and carry the hunk's buttons.
    auto bannerRow = [&](int idx, float y) {
        const DiffLine& L = d->lines[(size_t)idx];
        const RectF row{ body.x, y, body.w - DR(12), rowH };
        const bool hunk = L.kind == DiffLine::Hunk;
        gfx.Fill(row, hunk ? DiffHunkBg : Faint);
        float right = row.r() - DR(10);
        if (hunk && canApply && L.hunk >= 0) {
            const bool stage = m.req.section == 1;
            const wchar_t* label = stage ? L"Stage hunk" : L"Unstage hunk";
            const Color c = stage ? Added : Deleted;
            const float bw = gfx.Measure(label, F::TinySemi) + DR(18);
            const RectF b{ right - bw, y + DR(2), bw, rowH - DR(4) };
            const bool hb = !hunkBusy && hover.id == Hit::DiffHunk && hover.a == L.hunk && hover.b == 0;
            gfx.FillR(b, DR(3), A(c, hunkBusy ? 0.05f : hb ? 0.30f : 0.12f));
            gfx.StrokeR(b, DR(3), A(c, hunkBusy ? 0.3f : 0.8f));
            gfx.Text(label, b, F::TinySemi, hunkBusy ? TextDim : TintText(c), DWRITE_TEXT_ALIGNMENT_CENTER);
            AddZone(b, Hit::DiffHunk, L.hunk, 0);
            right = b.x - DR(6);
            if (stage) {  // discard: back to the version in the index
                const RectF db{ right - DR(24), y + DR(2), DR(24), rowH - DR(4) };
                const bool hd = !hunkBusy && hover.id == Hit::DiffHunk && hover.a == L.hunk && hover.b == 1;
                if (hd) gfx.FillR(db, DR(3), A(Deleted, 0.22f));
                gfx.Glyph(0xE74D, db, F::Icon10, hunkBusy ? TextDim : hd ? Deleted : TextSec);
                AddZone(db, Hit::DiffHunk, L.hunk, 1);
                if (hd) { tipText = L"Discard hunk"; tipAnchor = db; }
                right = db.x;
            }
            right -= DR(10);
        }
        gfx.Text(std::wstring_view(d->text.data() + L.off, L.len), { row.x + DR(12), y, right - row.x - DR(12), rowH }, F::MonoSmall,
                 hunk ? DiffHunkText : TextDim);
    };
    auto isBanner = [&](int idx) {
        const DiffLine::Kind k = d->lines[(size_t)idx].kind;
        return k == DiffLine::Hunk || k == DiffLine::Info;
    };

    gfx.PushClip(body);
    if (!split) {
        // ---- unified: old line no | new line no | +/- | text
        const float textX = body.x + 2 * numW + markW;
        const float textW = body.r() - DR(12) - textX;
        v.hscroll = std::clamp(v.hscroll, 0.f, std::max(0.f, (float)d->maxLen * cw + DR(60) - textW));
        gfx.Fill({ body.x, body.y, 2 * numW, body.h }, DiffGutter);
        for (int r = first; r < last; ++r) {
            if (isBanner(r)) continue;
            const DiffLine& L = d->lines[(size_t)r];
            const float y = rowY(r);
            const RectF row{ body.x, y, body.w - DR(12), rowH };
            if (L.kind == DiffLine::Added) gfx.Fill(row, DiffAddBg);
            else if (L.kind == DiffLine::Deleted) gfx.Fill(row, DiffDelBg);
            lineState(r, row, body.x + 2 * numW);
            number(L.oldNo, body.x, y);
            number(L.newNo, body.x + numW, y);
            marker(L, body.x + 2 * numW, y);
        }
        code->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);  // the line layouts are created from this format
        gfx.PushClip({ textX, body.y, std::max(0.f, textW), body.h });
        for (int r = first; r < last; ++r)
            if (!isBanner(r)) drawCode(r, textX, rowY(r));
        gfx.PopClip();
        for (int r = first; r < last; ++r)
            if (isBanner(r)) bannerRow(r, rowY(r));
    } else {
        // ---- split: the old file on the left, the new one on the right
        const float half = std::floor((body.w - DR(12)) * 0.5f);
        const float lx = body.x, rxp = body.x + half + 1;
        const float textW = half - numW - markW - DR(6);
        v.hscroll = std::clamp(v.hscroll, 0.f, std::max(0.f, (float)d->maxLen * cw + DR(60) - textW));
        gfx.Fill({ lx, body.y, numW, body.h }, DiffGutter);
        gfx.Fill({ rxp, body.y, numW, body.h }, DiffGutter);
        auto full = [&](const SplitRow& sr) { return sr.left >= 0 && sr.left == sr.right; };
        for (int r = first; r < last; ++r) {
            const SplitRow& sr = d->split[(size_t)r];
            if (full(sr) && isBanner(sr.left)) continue;
            const float y = rowY(r);
            for (int side = 0; side < 2; ++side) {
                const int idx = side == 0 ? sr.left : sr.right;
                const float px = side == 0 ? lx : rxp;
                const RectF cell{ px, y, half - (side == 1 ? 1.f : 0.f), rowH };
                if (idx < 0) {
                    gfx.Fill(cell, DiffFiller);
                    continue;
                }
                const DiffLine& L = d->lines[(size_t)idx];
                if (L.kind == DiffLine::Added) gfx.Fill(cell, DiffAddBg);
                else if (L.kind == DiffLine::Deleted) gfx.Fill(cell, DiffDelBg);
                lineState(idx, cell, px + numW);
                number(side == 0 ? L.oldNo : L.newNo, px, y);
                marker(L, px + numW, y);
            }
        }
        code->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        for (int side = 0; side < 2; ++side) {
            const float tx = (side == 0 ? lx : rxp) + numW + markW;
            gfx.PushClip({ tx, body.y, std::max(0.f, textW), body.h });
            for (int r = first; r < last; ++r) {
                const SplitRow& sr = d->split[(size_t)r];
                const int idx = side == 0 ? sr.left : sr.right;
                if (idx < 0 || (full(sr) && isBanner(idx))) continue;
                drawCode(idx, tx, rowY(r));
            }
            gfx.PopClip();
        }
        gfx.VLine(body.x + half, body.y, body.b(), Divider);
        for (int r = first; r < last; ++r) {
            const SplitRow& sr = d->split[(size_t)r];
            if (full(sr) && isBanner(sr.left)) bannerRow(sr.left, rowY(r));
        }
    }
    if (d->truncated && last >= (int)rows) {
        const float y = rowY((int)rows);
        gfx.Text(L"The diff is too large and was cut here.", { body.x + DR(16), y, body.w - DR(32), rowH * 1.5f }, F::Small, Modified);
    }
    gfx.PopClip();
    PaintScrollbar(body, v.vs, 3);
    v.body = body;

    // ---- floating bar for the selected lines
    if (!canApply || !m.selCount) return;
    const bool stage = m.req.section == 1, one = m.selCount == 1;
    const std::wstring count = std::to_wstring(m.selCount) + (one ? L" line selected" : L" lines selected");
    const wchar_t* mainLabel = stage ? (one ? L"Stage line" : L"Stage lines") : (one ? L"Unstage line" : L"Unstage lines");
    const float countW = gfx.Measure(count, F::Small), mainW = gfx.Measure(mainLabel, F::SmallSemi) + DR(26);
    const float discW = stage ? gfx.Measure(L"Discard", F::SmallSemi) + DR(26) : 0.f;
    const float barW = DR(16) + countW + DR(14) + mainW + (stage ? DR(8) + discW : 0.f) + DR(8) + DR(28) + DR(8);
    const RectF bar{ std::round(body.cx() - barW * 0.5f), body.b() - DR(64), std::round(barW), DR(44) };
    gfx.FillR({ bar.x + DR(1), bar.y + DR(3), bar.w, bar.h }, DR(8), Shadow);
    gfx.FillR(bar, DR(8), PopupBg);
    gfx.StrokeR(bar, DR(8), A(Accent, 0.45f));
    AddZone(bar, Hit::DiffLineAction, -1);  // clicks on the bar do not reach the lines below
    float bx = bar.x + DR(16);
    gfx.Text(count, { bx, bar.y, countW + 2, bar.h }, F::Small, Text);
    bx += countW + DR(14);
    auto button = [&](const wchar_t* label, float w, const Color& c, int id) {
        const RectF b{ bx, bar.cy() - DR(14), w, DR(28) };
        const bool hb = !hunkBusy && Hov(Hit::DiffLineAction, id);
        gfx.FillR(b, DR(5), A(c, hunkBusy ? 0.06f : hb ? 0.32f : 0.16f));
        gfx.StrokeR(b, DR(5), A(c, hunkBusy ? 0.3f : 0.85f));
        gfx.Text(label, b, F::SmallSemi, hunkBusy ? TextDim : TintText(c), DWRITE_TEXT_ALIGNMENT_CENTER);
        AddZone(b, Hit::DiffLineAction, id);
        bx = b.r() + DR(8);
    };
    button(mainLabel, mainW, stage ? Added : Deleted, 0);
    if (stage) button(L"Discard", discW, Deleted, 1);
    const RectF x{ bx, bar.cy() - DR(14), DR(28), DR(28) };
    if (Hov(Hit::DiffLineAction, 2)) gfx.FillR(x, DR(5), Hover);
    gfx.Glyph(0xE711, x, F::Icon10, TextSec);
    AddZone(x, Hit::DiffLineAction, 2);
    if (Hov(Hit::DiffLineAction, 2)) { tipText = L"Clear the selection (Esc)"; tipAnchor = x; }
}
