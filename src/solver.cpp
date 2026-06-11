#include "solver.hpp"
#ifdef WP_HAVE_METAL
#include "gpu_score.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace wp {

// ---------------------------------------------------------------------------
// PatternMatrix::build
// ---------------------------------------------------------------------------
PatternMatrix PatternMatrix::build(const WordList& words, unsigned nthreads) {
    const std::size_t n = words.size();

    if (nthreads == 0)
        nthreads = std::max(1u, std::thread::hardware_concurrency());

    PatternMatrix pm;
    pm.n_ = n;
    pm.data_.resize(n * n);

    // Split rows (guess indices) across threads
    std::vector<std::thread> threads;
    threads.reserve(nthreads);

    const auto rows_per_thread = (n + nthreads - 1) / nthreads;
    for (unsigned t = 0; t < nthreads; ++t) {
        const auto row_start = t * rows_per_thread;
        const auto row_end   = std::min(row_start + rows_per_thread, n);
        if (row_start >= n) break;

        threads.emplace_back([&pm, &words, n, row_start, row_end] {
            for (std::size_t gi = row_start; gi < row_end; ++gi) {
                auto gv = words[static_cast<WordIndex>(gi)].view();
                for (std::size_t ai = 0; ai < n; ++ai) {
                    pm.data_[gi * n + ai] =
                        compute_pattern(gv, words[static_cast<WordIndex>(ai)].view());
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    return pm;
}

// ---------------------------------------------------------------------------
// EntropySolver::partition
// ---------------------------------------------------------------------------
std::array<std::vector<WordIndex>, PATTERN_COUNT>
EntropySolver::partition(std::span<const WordIndex> candidates,
                         WordIndex                  guess_idx,
                         const PatternMatrix&       pm) {
    std::array<std::vector<WordIndex>, PATTERN_COUNT> buckets;
    for (WordIndex ai : candidates) {
        buckets[pm.get(guess_idx, ai)].push_back(ai);
    }
    return buckets;
}

// ---------------------------------------------------------------------------
// EntropySolver::entropy (private) — weighted Shannon entropy
// ---------------------------------------------------------------------------
double EntropySolver::entropy(std::span<const WordIndex> candidates,
                              WordIndex                  guess_idx,
                              const PatternMatrix&       pm) const noexcept {
    std::array<double, PATTERN_COUNT> bucket_weight{};
    double total_weight = 0.0;

    for (WordIndex ai : candidates) {
        double w = weight_of(ai);
        bucket_weight[pm.get(guess_idx, ai)] += w;
        total_weight += w;
    }

    if (total_weight == 0.0) return 0.0;

    double H = 0.0;
    for (double bw : bucket_weight) {
        if (bw > 0.0) {
            double p = bw / total_weight;
            H -= p * std::log2(p);
        }
    }
    return H;
}

// ---------------------------------------------------------------------------
// EntropySolver::best_guess
// ---------------------------------------------------------------------------
WordIndex EntropySolver::best_guess(std::span<const WordIndex> candidates,
                                    bool                       restrict_to_candidates) const {
    if (candidates.empty()) return WordList::NPOS;
    if (candidates.size() == 1) return candidates[0];

    // If only 2 candidates remain, guess one of them directly
    if (candidates.size() == 2) return candidates[0];

    // Determine which pool of words to search
    std::vector<WordIndex> all_idx;
    std::span<const WordIndex> guess_pool;

    if (restrict_to_candidates) {
        guess_pool = candidates;
    } else {
        all_idx = words_.all_indices();
        guess_pool = all_idx;
    }

    double    best_H            = -1.0;
    WordIndex best_word         = candidates[0];  // fallback
    // Track best_word's candidacy as state instead of re-running binary_search
    // on it every iteration; only recomputed when best_word changes.
    bool      best_is_candidate = std::ranges::binary_search(candidates, best_word);

    // Prefer guesses that are themselves still candidates (breaks ties toward
    // an answer rather than a pure information guess)
    for (WordIndex gi : guess_pool) {
        double H = entropy(candidates, gi, patterns_);

        // Tie-break: prefer a candidate over a non-candidate for equal entropy
        bool gi_is_candidate = std::ranges::binary_search(candidates, gi);

        if (H > best_H ||
            (H == best_H && gi_is_candidate && !best_is_candidate) ||
            (H == best_H && gi_is_candidate == best_is_candidate && gi < best_word)) {
            best_H            = H;
            best_word         = gi;
            best_is_candidate = gi_is_candidate;
        }
    }
    return best_word;
}

// ---------------------------------------------------------------------------
// EntropySolver::solve
// ---------------------------------------------------------------------------
SolveResult EntropySolver::solve(WordIndex answer_idx, int max_rounds) const {
    SolveResult result;
    std::vector<WordIndex> candidates = words_.all_indices();

    for (int round = 0; round < max_rounds; ++round) {
        WordIndex guess_idx = best_guess(candidates);
        if (guess_idx == WordList::NPOS) break;

        Pattern p = patterns_.get(guess_idx, answer_idx);
        result.steps.push_back({guess_idx, p});

        if (p == PATTERN_SOLVED) {
            result.solved = true;
            break;
        }

        // Filter candidates to those consistent with this guess+response
        auto buckets = partition(candidates, guess_idx, patterns_);
        candidates   = std::move(buckets[p]);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Worst-case-bounded tree construction: is_feasible / best_guess_feasible
// ---------------------------------------------------------------------------
namespace {
std::uint64_t feas_hash(std::span<const WordIndex> s, int depth) {
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&](std::uint64_t x){ h ^= x; h *= 1099511628211ULL; };
    for (WordIndex v : s) mix(v + 1u);
    mix(0x5A17u);
    mix(static_cast<std::uint64_t>(depth));
    return h;
}

// Largest bucket size when `gi` partitions `candidates`, using a reusable
// thread-local histogram cleared sparsely (only the patterns we touched). This
// is in the hottest loop of the sweep — ranking all ~15k guesses per
// feasibility node — so it avoids per-call allocation and avoids zeroing all
// 243 buckets every time (the candidate set usually touches far fewer).
[[gnu::hot]] int max_bucket_size(const PatternMatrix& pm,
                                 std::span<const WordIndex> candidates,
                                 WordIndex gi) noexcept {
    thread_local std::array<uint16_t, PATTERN_COUNT> hist{};   // bucket COUNTS; stays zeroed between calls
    Pattern seen[PATTERN_COUNT];   // distinct patterns touched (≤ 243)
    int nseen = 0;
    int mb = 0;
    for (WordIndex ai : candidates) {
        const Pattern p = pm.get(gi, ai);
        const int c = ++hist[p];
        if (c == 1) seen[nseen++] = p;   // first time we hit this bucket
        if (c > mb) mb = c;
    }
    for (int i = 0; i < nseen; ++i) hist[seen[i]] = 0;   // sparse reset
    return mb;
}

// Admissible lower bound on min_total for a single guess, computed in one
// histogram pass (no candidate-list materialisation). A non-solved bucket of
// size k costs >= 2k-1 below this guess; a singleton costs 1; everyone pays 1
// for the guess itself (the +n). The SOLVED bucket (the guess == answer) is a
// direct hit costing 1, already in the +n. Returns {lb, max_bucket}.
struct GuessLB { int lb; int max_bucket; };
[[gnu::hot]] GuessLB guess_lower_bound(const PatternMatrix& pm,
                                       std::span<const WordIndex> candidates,
                                       WordIndex gi) noexcept {
    thread_local std::array<uint16_t, PATTERN_COUNT> hist{};
    Pattern seen[PATTERN_COUNT];
    int nseen = 0, mb = 0;
    const int n = static_cast<int>(candidates.size());
    for (WordIndex ai : candidates) {
        const Pattern p = pm.get(gi, ai);
        const int c = ++hist[p];
        if (c == 1) seen[nseen++] = p;
        if (c > mb) mb = c;
    }
    int lb = n;  // everyone pays for this guess
    for (int i = 0; i < nseen; ++i) {
        const Pattern p = seen[i];
        const int c = hist[p];
        if (p != PATTERN_SOLVED && c >= 2) lb += 2 * c - 1;  // bucket floor 2k-1
        // singletons and the SOLVED hit cost their already-counted 1 (in n)
        hist[p] = 0;  // sparse reset
    }
    return {lb, mb};
}

// Single-pass partition score: returns max-bucket and (unweighted) Shannon
// entropy in one histogram pass, so best_guess_feasible doesn't compute the
// histogram twice (once for the no-progress filter, once for entropy).
struct GuessScore { int max_bucket; double entropy; };
[[gnu::hot]] GuessScore score_guess(const PatternMatrix& pm,
                                    std::span<const WordIndex> candidates,
                                    WordIndex gi) noexcept {
    thread_local std::array<uint16_t, PATTERN_COUNT> hist{};  // bucket COUNTS
    Pattern seen[PATTERN_COUNT];
    int nseen = 0, mb = 0;
    for (WordIndex ai : candidates) {
        const Pattern p = pm.get(gi, ai);
        const int c = ++hist[p];
        if (c == 1) seen[nseen++] = p;
        if (c > mb) mb = c;
    }
    const double total = static_cast<double>(candidates.size());
    double H = 0.0;
    for (int i = 0; i < nseen; ++i) {
        const int c = hist[seen[i]];
        const double pr = c / total;
        H -= pr * std::log2(pr);
        hist[seen[i]] = 0;   // sparse reset as we go
    }
    return {mb, H};
}
}  // namespace

// Build the max-bucket-ordered guess ranking for `candidates`. Uses a cached
// ranking if the GPU path pre-primed one for this set; otherwise CPU-scans all
// guesses. The result is the (max_bucket, gi) list of progress-making guesses
// (max_bucket < n), unsorted — the caller sorts/partial_sorts.
void EntropySolver::build_ranking(std::span<const WordIndex> candidates, int n,
                                  std::vector<std::pair<int, WordIndex>>& out) const {
    out.clear();
    if (gpu_) {
        const std::uint64_t rk = feas_hash(candidates, /*depth tag=*/0);
        if (auto it = rank_cache_.find(rk); it != rank_cache_.end()) {
            out = it->second;          // GPU-primed ranking — no CPU scan
            return;
        }
    }
    const std::size_t W = words_.size();
    out.reserve(W);
    for (std::size_t g = 0; g < W; ++g) {
        const auto gi = static_cast<WordIndex>(g);
        const int mb = max_bucket_size(patterns_, candidates, gi);
        if (mb == n) continue;
        out.emplace_back(mb, gi);
    }
}

#ifdef WP_HAVE_METAL
// GPU-score a batch of sets in one dispatch; stash each set's progress-making
// guess ranking in rank_cache_ keyed by set hash. This replaces N separate
// 15k-guess CPU scans (one per sibling bucket) with a single GPU pass.
void EntropySolver::prime_rankings_gpu(
        const std::vector<std::span<const WordIndex>>& sets) const {
    if (!gpu_ || sets.empty()) return;

    // Pack only sets not already cached.
    std::vector<WordIndex> packed;
    std::vector<std::uint32_t> offsets{0};
    std::vector<std::uint64_t> keys;
    std::vector<int> sizes;
    for (auto s : sets) {
        const std::uint64_t rk = feas_hash(s, 0);
        if (rank_cache_.contains(rk)) continue;
        for (WordIndex c : s) packed.push_back(c);
        offsets.push_back(static_cast<std::uint32_t>(packed.size()));
        keys.push_back(rk);
        sizes.push_back(static_cast<int>(s.size()));
    }
    if (keys.empty()) return;

    auto scored = gpu_->score_sets(packed, offsets);
    if (!scored) return;  // on any GPU error, callers fall back to CPU scan

    const std::size_t W = words_.size();
    for (std::size_t si = 0; si < keys.size(); ++si) {
        const auto* row = scored->data() + si * W;
        const int n = sizes[si];
        std::vector<std::pair<int, WordIndex>> order;
        order.reserve(256);
        for (std::size_t g = 0; g < W; ++g) {
            const int mb = static_cast<int>(row[g].max_bucket);
            if (mb == n) continue;
            order.emplace_back(mb, static_cast<WordIndex>(g));
        }
        rank_cache_.emplace(keys[si], std::move(order));
    }
}
#else
void EntropySolver::prime_rankings_gpu(
        const std::vector<std::span<const WordIndex>>&) const {}
#endif

bool EntropySolver::is_feasible(std::span<const WordIndex> candidates, int depth,
                                WordIndex* witness) const {
    ++stats_.feasible_calls;
    const int n = static_cast<int>(candidates.size());
    if (n <= 1) { if (witness && n == 1) *witness = candidates[0]; return true; }
    if (depth <= 1) return false;
    // Admissible bound: with one guess (243 patterns), depth==2 can distinguish
    // at most 243 words into singleton buckets.
    if (depth == 2 && n > PATTERN_COUNT) return false;

    const std::uint64_t key = feas_hash(candidates, depth);
    // Shared cross-worker cache (sweep) takes precedence over the per-instance
    // memo when configured; either way is_feasible is a pure function of (set,
    // depth) so cached facts are reusable across openers and threads.
    if (shared_feas_) {
        FeasibilityCache::Entry e;
        if (shared_feas_->get(key, e)) {
            ++stats_.feasible_hits;
            if (e.status == 1 && witness) *witness = e.witness;
            return e.status == 1;
        }
    } else if (auto it = feas_memo_.find(key); it != feas_memo_.end()) {
        ++stats_.feasible_hits;
        if (it->second == 1 && witness) {
            if (auto w = feas_witness_.find(key); w != feas_witness_.end())
                *witness = w->second;
        }
        return it->second == 1;
    }
    ++stats_.feasible_recur;

    // Rank guesses by max-bucket ascending (best splitters first → prune hard).
    // We try guesses in this order and break on the first feasible one, so a
    // FULL sort of all ~15k splitters is wasteful — the answer is almost always
    // among the strongest few. partial_sort brings the best `kPartial` to the
    // front (cheap); only if those are exhausted without success do we sort the
    // remainder (rare), keeping the search exhaustive and exact.
    //
    // When depth-1 == 1 (depth==2) only mb==1 guesses can work, so the search
    // stops at the first mb>1; the bounded prefix already covers that.
    std::vector<std::pair<int, WordIndex>> order;
    build_ranking(candidates, n, order);  // GPU-primed if available, else CPU scan
    constexpr auto by_rank = [](const std::pair<int,WordIndex>& a,
                                const std::pair<int,WordIndex>& b){
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second;
    };
    constexpr std::size_t kPartial = 64;
    std::size_t sorted_upto = std::min(kPartial, order.size());
    std::ranges::partial_sort(order, order.begin() + static_cast<std::ptrdiff_t>(sorted_upto),
                              by_rank);

    bool ok = false;
    WordIndex chosen = WordList::NPOS;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (i == sorted_upto) {  // exhausted the partially-sorted prefix
            std::ranges::sort(order.begin() + static_cast<std::ptrdiff_t>(sorted_upto),
                              order.end(), by_rank);
            sorted_upto = order.size();
        }
        auto [mb, gi] = order[i];
        if (depth - 1 == 1 && mb > 1) break;  // remaining can't solve in 1 guess
        ++stats_.partitions;
        auto buckets = partition(candidates, gi, patterns_);
        // Check buckets largest-first: the biggest bucket is the hardest to
        // solve in depth-1, so it's the most likely to be infeasible — failing
        // on it first rejects a bad guess after one recursion instead of many.
        std::array<Pattern, PATTERN_COUNT> nonempty;
        int nb = 0;
        for (Pattern p = 0; p < PATTERN_COUNT - 1; ++p)   // skip GGGGG (242)
            if (!buckets[p].empty()) nonempty[nb++] = p;
        std::sort(nonempty.begin(), nonempty.begin() + nb,
                  [&](Pattern a, Pattern b){ return buckets[a].size() > buckets[b].size(); });
        // GPU path: rank all the multi-element sibling buckets that will need a
        // ranking in one batched dispatch, so the recursion below consumes
        // cached rankings instead of re-scanning ~15k guesses per bucket.
        if (gpu_ && depth - 1 > 1) {
            std::vector<std::span<const WordIndex>> batch;
            for (int bi = 0; bi < nb; ++bi) {
                auto& b = buckets[nonempty[bi]];
                if (b.size() > 1) batch.emplace_back(b);
            }
            prime_rankings_gpu(batch);
        }
        bool all_ok = true;
        for (int bi = 0; bi < nb && all_ok; ++bi) {
            if (!is_feasible(buckets[nonempty[bi]], depth - 1, nullptr)) all_ok = false;
        }
        if (all_ok) { ok = true; chosen = gi; break; }
    }

    if (shared_feas_) {
        shared_feas_->put(key, {static_cast<char>(ok ? 1 : 2), chosen});
    } else {
        feas_memo_[key] = ok ? 1 : 2;
        if (ok) feas_witness_[key] = chosen;
    }
    if (ok && witness) *witness = chosen;
    return ok;
}

int EntropySolver::feasible_total(std::span<const WordIndex> candidates,
                                  int budget) const {
    const int n = static_cast<int>(candidates.size());
    if (n == 0) return 0;
    if (n == 1) return 1;
    if (budget <= 1) return std::numeric_limits<int>::max();

    // Pick the highest-entropy feasible guess (lookahead 1) and accumulate.
    WordIndex gi = best_guess_feasible(candidates, budget, /*lookahead=*/1);
    if (gi == WordList::NPOS) return std::numeric_limits<int>::max();

    auto buckets = partition(candidates, gi, patterns_);
    int total = n;
    for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
        if (p == PATTERN_SOLVED) continue;
        auto& b = buckets[p];
        if (b.empty()) continue;
        total += (b.size() == 1) ? 1 : feasible_total(b, budget - 1);
    }
    return total;
}

int EntropySolver::tree_total_for_opener(std::span<const WordIndex> candidates,
                                         WordIndex opener, int budget,
                                         std::size_t lookahead) const {
    const int n = static_cast<int>(candidates.size());
    if (n == 0) return 0;
    if (budget <= 0) return std::numeric_limits<int>::max();

    auto buckets = partition(candidates, opener, patterns_);
    // The opener must keep every non-trivial bucket feasible within budget-1.
    for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
        if (p == PATTERN_SOLVED) continue;
        auto& b = buckets[p];
        if (b.size() <= 1) continue;
        if (!is_feasible(b, budget - 1, nullptr))
            return std::numeric_limits<int>::max();
    }
    // Accumulate total via the feasibility-constrained policy (lookahead-aware
    // when > 1; lookahead 1 uses the cheap greedy continuation).
    int total = n;
    for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
        if (p == PATTERN_SOLVED) continue;
        auto& b = buckets[p];
        if (b.empty()) continue;
        if (b.size() == 1) { total += 1; continue; }
        if (lookahead > 1) {
            // descend choosing best_guess_feasible with lookahead at each node
            WordIndex g = best_guess_feasible(b, budget - 1, lookahead);
            if (g == WordList::NPOS) return std::numeric_limits<int>::max();
            total += tree_total_for_opener(b, g, budget - 1, lookahead);
        } else {
            total += feasible_total(b, budget - 1);
        }
    }
    return total;
}

// ── Exact mean optimisation: min_total / optimal_guess ──────────────────────
//
// min_total(S, depth): minimal Σ-depth (direct hit = 1) over trees with
// worst-case <= depth. Branch-and-bound with transposition (sorted-set memo).
int EntropySolver::min_total(std::span<const WordIndex> candidates, int depth,
                             int bound) const {
    const int n = static_cast<int>(candidates.size());
    if (n == 0) return 0;
    if (n == 1) return 1;                       // solved by guessing it
    if (depth <= 1) return MIN_TOTAL_INFEASIBLE; // 2+ words need >=2 guesses
    // Admissible: depth-2 can split into at most 243 singleton buckets.
    if (depth == 2 && n > PATTERN_COUNT) return MIN_TOTAL_INFEASIBLE;

    // (c) Transposition: exact value of S at `depth` is order-independent.
    const std::uint64_t key = feas_hash(candidates, depth) ^ 0x6D7E'A11Cull;
    // Structural floor: every word pays for this guess (n), and at least one
    // word needs a 2nd guess → 2n-1. The memo may hold a tighter proven floor.
    int set_lower = 2 * n - 1;
    if (auto it = tot_memo_.find(key); it != tot_memo_.end()) {
        const TotEntry& e = it->second;
        if (e.total < MIN_TOTAL_INFEASIBLE) return e.total;  // exact, reuse
        if (e.lower > set_lower) set_lower = e.lower;         // tighter floor
    }
    // If even the proven floor can't beat the caller's bound, give up — but
    // record the floor so callers/siblings reuse it.
    if (set_lower >= bound) {
        auto& e = tot_memo_[key];
        e.total = MIN_TOTAL_INFEASIBLE;
        e.guess = WordList::NPOS;
        if (set_lower > e.lower) e.lower = set_lower;
        return MIN_TOTAL_INFEASIBLE;
    }

    // Order guesses by their admissible LOWER BOUND ascending. The LB directly
    // bounds the objective (2|b|-1 per non-solved bucket), so the lowest-LB
    // guesses are the most promising — trying them first finds a tight incumbent
    // fast, which makes αβ prune the long tail. (max-bucket is only a proxy.)
    // We also stash each guess's LB so the main loop can skip it without
    // partitioning once the incumbent has dropped below its floor.
    std::vector<std::pair<int, WordIndex>> order;  // (lb, gi)
    {
        const std::size_t W = words_.size();
        order.reserve(W);
        for (std::size_t g = 0; g < W; ++g) {
            const auto gi = static_cast<WordIndex>(g);
            const auto [lb, mb] = guess_lower_bound(patterns_, candidates, gi);
            if (mb == n) continue;             // no progress
            order.emplace_back(lb, gi);
        }
        std::ranges::sort(order, [](auto& a, auto& b){
            if (a.first != b.first) return a.first < b.first;
            return a.second < b.second;
        });
    }

    // (b) Seed the incumbent with a quick greedy-feasible upper bound so alpha-
    // beta prunes from the very first guess. feasible_total follows the entropy
    // policy (no branching) and is cheap; its total is a valid upper bound on the
    // optimum, so starting `best` there is sound and dramatically tightens αβ.
    // best_gi tracks the guess achieving `best`; seed it with the greedy guess
    // so that, if no guess strictly improves on the UB, we still return the UB
    // (which is achievable) rather than INFEASIBLE.
    int best = bound;
    WordIndex best_gi = WordList::NPOS;
    {
        WordIndex gg = best_guess_feasible(candidates, depth, /*lookahead=*/1);
        if (gg != WordList::NPOS) {
            const int ub = feasible_total(candidates, depth);
            if (ub < best) { best = ub; best_gi = gg; }
        }
    }
    for (auto& [glb, gi] : order) {
        // `order` is sorted by admissible LB ascending. Once a guess's LB can't
        // beat the incumbent, NO later guess can either — terminate the whole
        // loop, not just this guess. This is the key large-bucket accelerator:
        // after the first good guess tightens `best`, the long tail is skipped
        // wholesale without a single partition.
        if (glb >= best) break;

        auto buckets = partition(candidates, gi, patterns_);

        // Build this guess's non-solved bucket list with a TIGHTENED per-bucket
        // floor: max(2k-1, cached_lower(bucket)). Using the memoised lower bound
        // of each child subset (instead of the crude 2k-1) is the key
        // large-bucket accelerator — once a child has been partly explored its
        // proven floor is far tighter, pruning sibling guesses without recursion.
        struct Sub { Pattern p; int sz; int floor; };
        std::vector<Sub> subs;
        subs.reserve(64);
        int lb = n;                                  // n: everyone pays this guess
        for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
            if (p == PATTERN_SOLVED) continue;
            const int sz = static_cast<int>(buckets[p].size());
            if (sz == 0) continue;
            if (sz == 1) { lb += 1; continue; }       // singleton: exact cost 1
            const int fl = cached_lower(buckets[p], depth - 1);
            lb += fl;
            subs.push_back({p, sz, fl});
        }
        if (lb >= best) continue;                     // LB prune (no recursion)

        // Process buckets cheapest-floor first so the running total climbs fast
        // and the αβ cutoff fires before the expensive large buckets. Keep a
        // running floor of the *unprocessed* buckets for the tightest cutoff.
        std::ranges::sort(subs, [](const Sub& a, const Sub& b){ return a.floor < b.floor; });
        int rem_lb = 0;
        for (auto& s : subs) rem_lb += s.floor;

        int total = n;
        for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
            if (p == PATTERN_SOLVED) continue;
            if (buckets[p].size() == 1) total += 1;   // singletons counted here
        }

        bool feasible = true;
        for (auto& s : subs) {
            rem_lb -= s.floor;                         // this bucket leaves the LB
            // αβ: if even this bucket's floor + the rest can't beat best, stop.
            if (total + s.floor + rem_lb >= best) { feasible = false; break; }
            // Pass a tight child bound: the most this child may cost and still
            // let the whole guess beat `best`. min_total uses it for its own αβ.
            const int child_bound = best - (total + rem_lb);
            const int sub = min_total(buckets[s.p], depth - 1, child_bound);
            if (sub >= MIN_TOTAL_INFEASIBLE) { feasible = false; break; }
            total += sub;
            if (total + rem_lb >= best) { feasible = false; break; }  // αβ cutoff
        }
        if (feasible && total < best) { best = total; best_gi = gi; }
    }

    // Record the outcome.
    //
    // `best` was initialised to the incoming `bound`. The guess loop visits
    // EVERY guess whose admissible lower bound is < best (it breaks only once a
    // guess — and hence all later ones in LB order — cannot beat best). So:
    //
    //  • If best < bound, we strictly improved on the incoming bound, which can
    //    only happen by actually constructing a tree of total `best`. Because we
    //    examined every guess capable of beating `best`, that value is the EXACT
    //    minimum — store and return it. (This is true whether or not `bound` was
    //    finite; the earlier version wrongly discarded improving results found
    //    under a finite bound, causing parents to reject the optimal guess and
    //    over-count the mean.)
    //
    //  • If best == bound (nothing beat it), the true optimum is >= bound. We
    //    learned a proven lower bound of `bound`; record it but report INFEASIBLE
    //    (meaning "> the bound you gave me"). Never poison `total`.
    auto& e = tot_memo_[key];
    if (best < bound) {                 // strictly improved ⇒ exact optimum
        e.total = best;
        e.guess = best_gi;
        if (best > e.lower) e.lower = best;
        return best;
    }
    // Nothing beat the incoming bound: `bound` is a proven floor for this subset.
    if (bound > e.lower) e.lower = bound;
    return MIN_TOTAL_INFEASIBLE;
}

// Best known admissible lower bound on min_total(b, depth).
int EntropySolver::cached_lower(std::span<const WordIndex> b, int depth) const {
    const int k = static_cast<int>(b.size());
    if (k <= 1) return k;                 // 0→0, 1→1 (direct hit)
    int lo = 2 * k - 1;                   // structural floor
    const std::uint64_t key = feas_hash(b, depth) ^ 0x6D7E'A11Cull;
    if (auto it = tot_memo_.find(key); it != tot_memo_.end()) {
        const TotEntry& e = it->second;
        const int cand = (e.total < MIN_TOTAL_INFEASIBLE) ? e.total : e.lower;
        if (cand > lo) lo = cand;
    }
    return lo;
}

WordIndex EntropySolver::optimal_guess(std::span<const WordIndex> candidates,
                                       int depth) const {
    const int n = static_cast<int>(candidates.size());
    if (n == 1) return candidates[0];
    const std::uint64_t key = feas_hash(candidates, depth) ^ 0x6D7E'A11Cull;
    if (auto it = tot_memo_.find(key); it != tot_memo_.end()) return it->second.guess;
    return WordList::NPOS;
}

WordIndex EntropySolver::best_guess_feasible(std::span<const WordIndex> candidates,
                                             int budget,
                                             std::size_t lookahead) const {
    ++stats_.choice_calls;
    const int n = static_cast<int>(candidates.size());
    if (n == 0) return WordList::NPOS;
    if (n == 1) return candidates[0];
    if (n == 2) return candidates[0];
    if (budget <= 1) return WordList::NPOS;

    // Memoized choice: the production build calls this once per node, and many
    // candidate sets recur across the tree. Key on (set, budget, lookahead).
    const std::uint64_t ckey =
        feas_hash(candidates, budget) ^ (0xC0FFEEull * lookahead + 1);
    if (auto it = feas_choice_.find(ckey); it != feas_choice_.end()) {
        ++stats_.choice_hits;
        return it->second;
    }

    // Guarantee a feasible fallback exists (and warm the feasibility memo for
    // this set). The witness is a guess that keeps every bucket solvable within
    // budget; if the set isn't feasible at all, bail.
    WordIndex witness = WordList::NPOS;
    if (!is_feasible(candidates, budget, &witness)) {
        feas_choice_[ckey] = WordList::NPOS;
        return WordList::NPOS;
    }

    // Rank guesses by entropy desc (low-mean heuristic), then check feasibility
    // on only the top EXPLORE_POOL of them. This bounds per-node cost: feasibility
    // checks (the expensive part) run on a small, promising pool, not all ~15k
    // words. The witness guarantees we always have a feasible choice.
    constexpr std::size_t EXPLORE_POOL = 80;
    const std::size_t W = words_.size();
    std::vector<std::pair<double, WordIndex>> order;
    order.reserve(W);
    for (std::size_t g = 0; g < W; ++g) {
        const auto gi = static_cast<WordIndex>(g);
        const auto sc = score_guess(patterns_, candidates, gi);
        if (sc.max_bucket == n) continue;  // no progress
        order.emplace_back(sc.entropy, gi);
    }
    const std::size_t pool = std::min<std::size_t>(EXPLORE_POOL, order.size());
    std::ranges::partial_sort(order, order.begin() + static_cast<std::ptrdiff_t>(pool),
        [](auto& a, auto& b){
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        });
    order.resize(pool);

    // Expand up to `lookahead` feasible guesses; keep the one with lowest total.
    WordIndex best_gi = WordList::NPOS;
    int       best_total = std::numeric_limits<int>::max();
    std::size_t expanded = 0;

    for (auto& [H, gi] : order) {
        if (expanded >= lookahead && best_gi != WordList::NPOS) break;
        auto buckets = partition(candidates, gi, patterns_);
        bool feasible_here = true;
        for (Pattern p = 0; p < PATTERN_COUNT && feasible_here; ++p) {
            if (p == PATTERN_SOLVED) continue;
            auto& b = buckets[p];
            if (b.size() <= 1) continue;
            if (!is_feasible(b, budget - 1, nullptr)) feasible_here = false;
        }
        if (!feasible_here) continue;
        ++expanded;

        if (lookahead <= 1) { feas_choice_[ckey] = gi; return gi; }

        // Estimate total depth under a greedy continuation for tie-breaking.
        int total = n;
        for (Pattern p = 0; p < PATTERN_COUNT; ++p) {
            if (p == PATTERN_SOLVED) continue;
            auto& b = buckets[p];
            if (b.empty()) continue;
            total += (b.size() == 1) ? 1
                   : feasible_total(b, budget - 1);
        }
        if (total < best_total) { best_total = total; best_gi = gi; }
    }
    // If no high-entropy pool member was feasible, fall back to the guaranteed
    // feasible witness (worst-case correctness over mean).
    if (best_gi == WordList::NPOS) best_gi = witness;
    feas_choice_[ckey] = best_gi;
    return best_gi;
}

// ---------------------------------------------------------------------------
// any_consistent_word — solver-mode consistency check
// ---------------------------------------------------------------------------
bool any_consistent_word(const WordList& candidates,
                         std::span<const GuessResponse> history) noexcept {
    for (const auto& w : candidates.span()) {
        std::string_view av = w.view();
        bool ok = true;
        for (const auto& [g, r] : history) {
            if (compute_pattern(g, av) != r) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

} // namespace wp
