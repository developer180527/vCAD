#include "cad/app/SelectionRanking.h"

#include <algorithm>

namespace cad::app {

void rankCandidates(std::vector<PickCandidate>& candidates) {
    // stable_sort, not sort. Candidates the rule cannot separate — same kind, same distance, same
    // depth, which happens constantly on a flat face — must keep the picker's order, or a Select
    // Other list reorders itself between two identical taps and the user cannot point at a row.
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const PickCandidate& a, const PickCandidate& b) {
                         if (a.kind != b.kind) return a.kind < b.kind;
                         if (a.distanceSq != b.distanceSq) return a.distanceSq < b.distanceSq;
                         return a.depth < b.depth;
                     });
}

void restrictToKind(std::vector<PickCandidate>& candidates, PickKind kind) {
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [kind](const PickCandidate& c) { return c.kind != kind; }),
                     candidates.end());
}

}   // namespace cad::app
