// Commit graph lane layout.
//
// Rows are processed top to bottom. Every lane remembers the commit it is waiting for (a parent of a
// row above). A commit takes the leftmost lane waiting for it; other lanes waiting for it end here
// with a curve into the node (branch fork point). The first parent continues in the node lane, merge
// parents either join a lane already waiting for them or open a new lane to the right.

#include "git.h"
#include "text.h"

Graph BuildGraph(const RepoData& d, bool withWip) {
    const double t0 = NowMs();
    Graph g;
    const int32_t n = (int32_t)d.commits.size();
    g.hasWip = withWip;
    g.rows.reserve((size_t)n + 1);
    g.rowOfCommit.assign((size_t)n, -1);
    g.segs.reserve((size_t)n * 3 + 8);

    std::vector<int32_t> active;  // lane -> awaited commit (-1 = free)
    std::vector<uint8_t> dash;    // lane -> dashed edge (WIP / stash)
    std::vector<int32_t> next;    // lane -> next lane awaiting the same commit
    std::vector<int32_t> waitHead((size_t)n, -1);
    std::vector<int32_t> waiting;
    int maxLanes = 1;

    auto alloc = [&](int from) -> int {
        for (int i = from; i < (int)active.size(); ++i) if (active[(size_t)i] < 0) return i;
        while ((int)active.size() < from) { active.push_back(-1); dash.push_back(0); next.push_back(-1); }
        active.push_back(-1);
        dash.push_back(0);
        next.push_back(-1);
        return (int)active.size() - 1;
    };
    auto await = [&](int lane, int32_t c, bool dashed) {
        active[(size_t)lane] = c;
        dash[(size_t)lane] = dashed ? 1 : 0;
        next[(size_t)lane] = waitHead[(size_t)c];
        waitHead[(size_t)c] = lane;
    };
    auto push = [&](int lane, uint8_t type, uint8_t dashed) { g.segs.push_back({ (uint16_t)lane, type, dashed }); };

    auto process = [&](int32_t commit, const int32_t* par, int pc, bool dashedOut) {
        GraphRow row{};
        row.commit = commit;
        row.segOff = (uint32_t)g.segs.size();

        waiting.clear();
        if (commit >= 0) {
            for (int l = waitHead[(size_t)commit]; l >= 0; l = next[(size_t)l]) waiting.push_back(l);
            waitHead[(size_t)commit] = -1;
            std::sort(waiting.begin(), waiting.end());
        }
        const int nodeLane = waiting.empty() ? alloc(0) : waiting[0];

        // Lanes passing straight through this row.
        for (int l = 0; l < (int)active.size(); ++l) {
            int32_t a = active[(size_t)l];
            if (a >= 0 && a != commit) push(l, SEG_PASS, dash[(size_t)l]);
        }
        // Incoming edges from the children above.
        for (size_t k = 0; k < waiting.size(); ++k) {
            int l = waiting[k];
            push(l, SEG_IN, dash[(size_t)l]);
            if (k > 0) { active[(size_t)l] = -1; dash[(size_t)l] = 0; }
        }
        // First parent continues in the node lane.
        if (pc > 0 && par[0] >= 0) {
            await(nodeLane, par[0], dashedOut);
            push(nodeLane, SEG_OUT, dashedOut ? 1 : 0);
        } else {
            active[(size_t)nodeLane] = -1;
            dash[(size_t)nodeLane] = 0;
        }
        // Merge parents.
        for (int k = 1; k < pc; ++k) {
            int32_t p = par[k];
            if (p < 0) continue;
            bool dup = false;
            for (int j = 0; j < k; ++j) if (par[j] == p) dup = true;
            if (dup) continue;
            int target = -1;
            for (int l = waitHead[(size_t)p]; l >= 0; l = next[(size_t)l])
                if (target < 0 || std::abs(l - nodeLane) < std::abs(target - nodeLane)) target = l;
            if (target < 0) {
                target = alloc(nodeLane + 1);
                await(target, p, false);
            }
            push(target, SEG_OUT, dash[(size_t)target]);
        }
        while (!active.empty() && active.back() < 0) { active.pop_back(); dash.pop_back(); next.pop_back(); }

        row.lane = (uint16_t)nodeLane;
        row.segCount = (uint16_t)std::min<size_t>(g.segs.size() - row.segOff, 65535);
        maxLanes = std::max({ maxLanes, nodeLane + 1, (int)active.size() });
        if (commit >= 0) g.rowOfCommit[(size_t)commit] = (int32_t)g.rows.size();
        g.rows.push_back(row);
    };

    if (withWip) {
        int32_t head = d.headCommit;
        process(-1, &head, head >= 0 ? 1 : 0, true);
    }
    for (int32_t i = 0; i < n; ++i) {
        const Commit& c = d.commits[(size_t)i];
        const bool stash = (c.flags & CF_STASH) != 0;
        process(i, d.Parents(c), stash ? std::min<int>(c.parentCount, 1) : c.parentCount, stash);
    }
    g.maxLanes = maxLanes;
    g.ms = NowMs() - t0;
    return g;
}
