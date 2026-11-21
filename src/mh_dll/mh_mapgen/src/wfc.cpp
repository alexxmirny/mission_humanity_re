// mh_mapgen -- tile-level wave-function collapse. Each cell's domain starts as the tiles the plan's macro case allows in that
// quadrant; legality is "this adjacent pair was seen in a shipped map" on a torus (so the wrap seam is legal by construction).
// Bitset domains, byte-table support lookups, min-entropy collapse with seeded tie priority, windowed reset on contradiction.
#include "gen_internal.h"
#include <algorithm>
#include <bit>
#include <queue>
#include <tuple>

namespace mh_mapgen::detail {

namespace {

constexpr int           DR[4]      = {0, 0, 1, -1}; // east, west, south, north
constexpr int           DC[4]      = {1, -1, 0, 0};
constexpr std::uint32_t MAX_RESETS = 2000;

} // namespace

struct SolverBase {
    const Model                     &m;
    const Plan                      &pl;
    const int                        N, M;
    const std::uint32_t              W;
    const std::vector<std::int32_t> *fixed;
    std::vector<std::uint64_t>       D;
    std::vector<std::uint8_t>        done;
    std::vector<std::uint32_t>       stack;

    std::uint32_t fail_src = 0, fail_dst = 0; // the pair whose propagation emptied a domain

    bool                       logging = false; // DecalProbe: record every narrowed domain so a failed pin can be undone
    std::vector<std::uint32_t> ucell;
    std::vector<std::uint64_t> uword;

    SolverBase(const Model &mdl, const Plan &plan, int n, const std::vector<std::int32_t> *fx) : m(mdl), pl(plan), N(n), M(n / 2), W(mdl.W), fixed(fx), D(std::size_t(n) * n * mdl.W), done(std::size_t(n) * n, 0) {}

    std::uint32_t cell_info(int r, int c) const { return std::uint32_t(pl.info[std::size_t((r / 2) * M + c / 2)]); }

    void load_init(int r, int c, std::uint64_t *out) const {
        const std::size_t cell = std::size_t(r) * N + c;
        if (fixed && (*fixed)[cell] >= 0) {
            for (std::uint32_t w = 0; w < W; ++w) out[w] = 0;
            const std::uint32_t t = std::uint32_t((*fixed)[cell]);
            out[t / 64]           = std::uint64_t(1) << (t % 64);
            return;
        }
        const int            q   = (r & 1) * 2 + (c & 1);
        const std::uint64_t *src = m.infos[cell_info(r, c)].dom[q].data();
        for (std::uint32_t w = 0; w < W; ++w) out[w] = src[w];
    }

    std::uint32_t count(const std::uint64_t *dom) const {
        std::uint32_t n = 0;
        for (std::uint32_t w = 0; w < W; ++w) n += std::uint32_t(std::popcount(dom[w]));
        return n;
    }

    int single_bit(const std::uint64_t *dom) const {
        int found = -1;
        for (std::uint32_t w = 0; w < W; ++w) {
            if (!dom[w]) continue;
            if (found >= 0 || (dom[w] & (dom[w] - 1))) return -1;
            found = int(w * 64 + std::uint32_t(std::countr_zero(dom[w])));
        }
        return found;
    }

    // Returns the cell whose domain emptied, or -1 when the stack drained.
    int propagate() {
        while (!stack.empty()) {
            const std::uint32_t cell = stack.back();
            stack.pop_back();
            const int            r = int(cell) / N, c = int(cell) % N;
            const std::uint64_t *dom    = &D[std::size_t(cell) * W];
            const int            single = single_bit(dom);
            for (int d = 0; d < 4; ++d) {
                const int            nr = r + DR[d] < 0 ? N - 1 : (r + DR[d] >= N ? 0 : r + DR[d]);
                const int            nc = c + DC[d] < 0 ? N - 1 : (c + DC[d] >= N ? 0 : c + DC[d]);
                const std::size_t    nb = std::size_t(nr) * N + nc;
                const std::uint64_t *sup;
                if (single >= 0) sup = m.sup_row(d, std::uint32_t(single));
                else sup = m.support_cached(dom, d);
                std::uint64_t *nd      = &D[nb * W];
                bool           changed = false, nonzero = false;
                for (std::uint32_t w = 0; w < W; ++w) {
                    const std::uint64_t v = nd[w] & sup[w];
                    changed               = changed || v != nd[w];
                    nonzero               = nonzero || v != 0;
                }
                if (!changed) continue;
                if (!nonzero) {
                    fail_src = cell;
                    fail_dst = std::uint32_t(nb);
                    return int(nb);
                }
                if (logging) {
                    ucell.push_back(std::uint32_t(nb));
                    uword.insert(uword.end(), nd, nd + W);
                }
                for (std::uint32_t w = 0; w < W; ++w) nd[w] &= sup[w];
                stack.push_back(std::uint32_t(nb));
            }
        }
        return -1;
    }

    int init_all() {
        for (int r = 0; r < N; ++r)
            for (int c = 0; c < N; ++c) load_init(r, c, &D[(std::size_t(r) * N + c) * W]);
        stack.clear();
        for (std::uint32_t i = 0; i < std::uint32_t(N) * std::uint32_t(N); ++i) stack.push_back(i);
        return propagate();
    }

    std::uint32_t weight_of(int r, int c, std::uint32_t tile, std::size_t &cursor) const {
        const std::size_t cell = std::size_t(r) * N + c;
        if (fixed && (*fixed)[cell] >= 0) return 1;
        const int   q = (r & 1) * 2 + (c & 1);
        const auto &w = m.infos[cell_info(r, c)].wt[q];
        while (cursor < w.size() && w[cursor].first < tile) ++cursor;
        return cursor < w.size() && w[cursor].first == tile ? w[cursor].second : 0;
    }
};

using Solver = SolverBase;

struct TileState::Impl {
    SolverBase                 s;
    std::vector<std::uint32_t> pending; // cells to re-propagate after an aborted pass
    std::vector<std::uint8_t>  mark;
    Impl(const Model &m, const Plan &pl, int N) : s(m, pl, N, nullptr), mark(std::size_t(N) * N, 0) {}
};

TileState::TileState(const Model &m, const Plan &plan, int N) : p_(std::make_unique<Impl>(m, plan, N)) {}
TileState::~TileState() = default;

bool TileState::init(int &bad_r, int &bad_c) {
    const int bad = p_->s.init_all();
    if (bad < 0) return true;
    p_->pending = {p_->s.fail_src, p_->s.fail_dst};
    bad_r       = bad / p_->s.N;
    bad_c       = bad % p_->s.N;
    return false;
}

bool TileState::refresh(const std::vector<int> &macros, int &bad_r, int &bad_c) {
    SolverBase                &s = p_->s;
    const int                  N = s.N, M = s.M;
    std::vector<std::uint32_t> win;
    auto                       add = [&](int r, int c) {
        const std::size_t i = std::size_t(pmod(r, N)) * N + std::size_t(pmod(c, N));
        if (!p_->mark[i]) {
            p_->mark[i] = 1;
            win.push_back(std::uint32_t(i));
        }
    };
    for (int mc : macros) {
        const int R = mc / M, C = mc % M;
        for (int r = 2 * R - 2; r <= 2 * R + 3; ++r)
            for (int c = 2 * C - 2; c <= 2 * C + 3; ++c) add(r, c);
    }
    for (std::uint32_t i : p_->pending) add(int(i) / N, int(i) % N);
    p_->pending.clear();
    const std::vector<std::uint32_t> inside = win;
    for (std::uint32_t i : inside) s.load_init(int(i) / N, int(i) % N, &s.D[std::size_t(i) * s.W]);
    // the ring just outside the window pushes its (older, possibly narrower) constraints in
    for (std::uint32_t i : inside)
        for (int d = 0; d < 4; ++d) add(int(i) / N + DR[d], int(i) % N + DC[d]);
    for (std::uint32_t i : win) p_->mark[i] = 0;
    for (std::uint32_t i : win) s.stack.push_back(i);
    const int bad = s.propagate();
    if (bad < 0) return true;
    p_->pending = {s.fail_src, s.fail_dst};
    bad_r       = bad / N;
    bad_c       = bad % N;
    return false;
}

struct DecalProbe::Impl {
    SolverBase s;
    bool       ok = false;
    Impl(const Model &m, const Plan &pl, int N, const std::vector<std::int32_t> *fx) : s(m, pl, N, fx) {}
};

DecalProbe::DecalProbe(const Model &m, const Plan &plan, int N, const std::vector<std::int32_t> &fixed) : p_(std::make_unique<Impl>(m, plan, N, &fixed)) { p_->ok = p_->s.init_all() < 0; }
DecalProbe::~DecalProbe() = default;
bool DecalProbe::valid() const { return p_->ok; }

bool DecalProbe::pin(const std::vector<std::pair<std::uint32_t, std::int32_t>> &cells) {
    SolverBase         &s = p_->s;
    const std::uint32_t W = s.W;
    s.logging             = true;
    s.ucell.clear();
    s.uword.clear();
    std::vector<std::uint64_t> old; // the pinned cells' own domains
    bool                       ok = true;
    s.stack.clear();
    for (const auto &[cell, tile] : cells) {
        std::uint64_t *d = &s.D[std::size_t(cell) * W];
        if (!((d[std::uint32_t(tile) / 64] >> (std::uint32_t(tile) % 64)) & 1)) {
            ok = false;
            break;
        }
        s.ucell.push_back(cell);
        s.uword.insert(s.uword.end(), d, d + W);
        for (std::uint32_t w = 0; w < W; ++w) d[w] = 0;
        d[std::uint32_t(tile) / 64] = std::uint64_t(1) << (std::uint32_t(tile) % 64);
        s.stack.push_back(cell);
    }
    if (ok) ok = s.propagate() < 0;
    s.logging = false;
    if (!ok) { // undo in reverse order
        for (std::size_t i = s.ucell.size(); i-- > 0;) {
            std::uint64_t *d = &s.D[std::size_t(s.ucell[i]) * W];
            for (std::uint32_t w = 0; w < W; ++w) d[w] = s.uword[i * W + w];
        }
        s.stack.clear();
    }
    return ok;
}

WfcResult wfc_initial(const Model &m, const Plan &plan, int N, const std::vector<std::int32_t> *fixed) {
    WfcResult res;
    Solver    s(m, plan, N, fixed);
    const int bad = s.init_all();
    if (bad >= 0) {
        res.why   = "initial propagation emptied a cell";
        res.bad_r = bad / N;
        res.bad_c = bad % N;
        return res;
    }
    res.ok = true;
    return res;
}

// The collapse loop, from a state whose initial propagation already succeeded.
static WfcResult solve_state(const Model &m, int N, std::uint64_t seed, Solver &s) {
    WfcResult res;
    using Entry = std::tuple<std::uint32_t, std::uint64_t, std::int32_t, std::int32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> heap;
    const std::uint32_t                                            W = m.W;
    for (int r = 0; r < N; ++r)
        for (int c = 0; c < N; ++c) heap.push({s.count(&s.D[(std::size_t(r) * N + c) * W]), h64({seed, std::uint64_t(r), std::uint64_t(c)}), r, c});
    std::uint32_t              resets = 0;
    std::vector<std::uint32_t> opts_t, opts_w;
    while (!heap.empty()) {
        const auto [n, pr, r, c] = heap.top();
        heap.pop();
        const std::size_t cell = std::size_t(r) * N + std::size_t(c);
        if (s.done[cell]) continue;
        const std::uint32_t cnt = s.count(&s.D[cell * W]);
        if (cnt != n) {
            heap.push({cnt, pr, r, c});
            continue;
        }
        // weighted pick over the domain
        opts_t.clear();
        opts_w.clear();
        std::uint64_t total  = 0;
        std::size_t   cursor = 0;
        for (std::uint32_t w = 0; w < W; ++w)
            for (std::uint64_t bits = s.D[cell * W + w]; bits; bits &= bits - 1) {
                const std::uint32_t t  = w * 64 + std::uint32_t(std::countr_zero(bits));
                const std::uint32_t wt = s.weight_of(r, c, t, cursor) + 1;
                opts_t.push_back(t);
                opts_w.push_back(wt);
                total += wt;
            }
        std::uint64_t k    = h64({seed, std::uint64_t(r), std::uint64_t(c), resets, 7}) % total;
        std::size_t   pick = 0;
        for (; pick < opts_t.size(); ++pick) {
            if (k < opts_w[pick]) break;
            k -= opts_w[pick];
        }
        if (pick >= opts_t.size()) pick = opts_t.size() - 1;
        for (std::uint32_t w = 0; w < W; ++w) s.D[cell * W + w] = 0;
        s.D[cell * W + opts_t[pick] / 64] = std::uint64_t(1) << (opts_t[pick] % 64);
        s.done[cell]                      = 1;
        s.stack.assign(1, std::uint32_t(cell));
        const int bad = s.propagate();
        if (bad < 0) continue;
        ++resets;
        if (resets > MAX_RESETS) {
            res.why    = "too many resets";
            res.resets = resets;
            return res;
        }
        const int                  br = bad / N, bc = bad % N;
        int                        rad = 2;
        std::vector<std::uint32_t> win;
        for (;;) {
            rad += 2;
            if (rad > N / 2) {
                res.why    = "unrecoverable contradiction";
                res.resets = resets;
                return res;
            }
            for (int dr = -rad; dr <= rad; ++dr)
                for (int dc = -rad; dc <= rad; ++dc) s.done[std::size_t(pmod(br + dr, N)) * N + std::size_t(pmod(bc + dc, N))] = 0;
            win.clear();
            for (int dr = -rad - 3; dr <= rad + 3; ++dr)
                for (int dc = -rad - 3; dc <= rad + 3; ++dc) win.push_back(std::uint32_t(pmod(br + dr, N) * N + pmod(bc + dc, N)));
            for (std::uint32_t w : win)
                if (!s.done[w]) s.load_init(int(w) / N, int(w) % N, &s.D[std::size_t(w) * W]);
            s.stack = win;
            if (s.propagate() < 0) break;
        }
        for (std::uint32_t w : win)
            if (!s.done[w]) heap.push({s.count(&s.D[std::size_t(w) * W]), h64({seed, w / std::uint64_t(N), w % std::uint64_t(N), resets}), std::int32_t(w / std::uint32_t(N)), std::int32_t(w % std::uint32_t(N))});
    }
    res.tile.assign(std::size_t(N) * N, 0);
    for (std::size_t i = 0; i < res.tile.size(); ++i) {
        std::int32_t t = -1;
        for (std::uint32_t w = 0; w < W && t < 0; ++w)
            if (s.D[i * W + w]) t = std::int32_t(w * 64 + std::uint32_t(std::countr_zero(s.D[i * W + w])));
        res.tile[i] = t;
    }
    res.ok     = true;
    res.resets = resets;
    return res;
}

WfcResult wfc_solve(const Model &m, const Plan &plan, int N, std::uint64_t seed, const std::vector<std::int32_t> *fixed) {
    WfcResult res;
    Solver    s(m, plan, N, fixed);
    const int bad0 = s.init_all();
    if (bad0 >= 0) {
        res.why   = "initial propagation emptied a cell";
        res.bad_r = bad0 / N;
        res.bad_c = bad0 % N;
        return res;
    }
    return solve_state(m, N, seed, s);
}

// The probe's domains are the (unique) arc-consistent fixpoint of the plan plus every pinned cell, i.e. exactly what wfc_solve's own
// initial propagation would compute over the same fixed cells, so the solve starts from a copy of them.
WfcResult DecalProbe::solve(std::uint64_t seed) const {
    Solver s(p_->s);
    return solve_state(s.m, s.N, seed, s);
}

} // namespace mh_mapgen::detail
