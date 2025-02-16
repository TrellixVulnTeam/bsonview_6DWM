/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>
#include <queue>
#include <vector>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

struct CandidatePlan;
struct PlanRankingDecision;

/**
 * Ranks 2 or more plans.
 */
class PlanRanker {
public:
    /**
     * Returns a PlanRankingDecision which has the ranking and the information about the ranking
     * process with status OK if everything worked. 'candidateOrder' within the PlanRankingDecision
     * holds indices into candidates ordered by score (winner in first element).
     *
     * Returns an error if there was an issue with plan ranking (e.g. there was no viable plan).
     */
    static StatusWith<std::unique_ptr<PlanRankingDecision>> pickBestPlan(
        const std::vector<CandidatePlan>& candidates);

    /**
     * Assign the stats tree a 'goodness' score. The higher the score, the better
     * the plan. The exact value isn't meaningful except for imposing a ranking.
     */
    static double scoreTree(const PlanStageStats* stats);
};

/**
 * A container holding one to-be-ranked plan and its associated/relevant data.
 * Does not own any of its pointers.
 */
struct CandidatePlan {
    CandidatePlan(std::unique_ptr<QuerySolution> solution, PlanStage* r, WorkingSet* w)
        : solution(std::move(solution)), root(r), ws(w), failed(false) {}

    std::unique_ptr<QuerySolution> solution;
    PlanStage* root;  // Not owned here.
    WorkingSet* ws;   // Not owned here.

    // Any results produced during the plan's execution prior to ranking are retained here.
    std::queue<WorkingSetID> results;

    bool failed;
};

/**
 * Information about why a plan was picked to be the best.  Data here is placed into the cache
 * and used to compare expected performance with actual.
 */
struct PlanRankingDecision {
    PlanRankingDecision() {}

    /**
     * Make a deep copy.
     */
    PlanRankingDecision* clone() const {
        PlanRankingDecision* decision = new PlanRankingDecision();
        for (size_t i = 0; i < stats.size(); ++i) {
            PlanStageStats* s = stats[i].get();
            invariant(s);
            decision->stats.push_back(std::unique_ptr<PlanStageStats>{s->clone()});
        }
        decision->scores = scores;
        decision->candidateOrder = candidateOrder;
        decision->failedCandidates = failedCandidates;
        return decision;
    }

    // Stats of all plans sorted in descending order by score.
    // Owned by us.
    std::vector<std::unique_ptr<PlanStageStats>> stats;

    // The "goodness" score corresponding to 'stats'.
    // Sorted in descending order.
    std::vector<double> scores;

    // Ordering of original plans in descending of score.
    // Filled in by PlanRanker::pickBestPlan(candidates, ...)
    // so that candidates[candidateOrder[0]] refers to the best plan
    // with corresponding cores[0] and stats[0]. Runner-up would be
    // candidates[candidateOrder[1]] followed by
    // candidates[candidateOrder[2]], ...
    //
    // Contains only non-failing plans.
    std::vector<size_t> candidateOrder;

    // Contains the list of original plans that failed.
    //
    // Like 'candidateOrder', the contents of this array are indicies into the 'candidates' array.
    std::vector<size_t> failedCandidates;

    // Whether two plans tied for the win.
    //
    // Reading this flag is the only reliable way for callers to determine if there was a tie,
    // because the scores kept inside the PlanRankingDecision do not incorporate the EOF bonus.
    bool tieForBest = false;
};

}  // namespace mongo
