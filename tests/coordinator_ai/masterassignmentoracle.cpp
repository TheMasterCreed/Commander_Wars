#include "tests/coordinator_ai/masterassignmentoracle.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <set>

namespace
{
    using namespace MasterAssignmentOracle;

    bool normalizeDomain(const MasterAssignmentProblem & problem, const Request & request, Result & result)
    {
        const auto full = fullDomain(problem);
        result.fullTupleProduct = tupleProduct(full);
        result.domain = request.domain.empty() ? full : request.domain;
        if (result.domain.size() != full.size())
        {
            result.reason = QStringLiteral("domain actor count");
            return false;
        }
        for (std::size_t slot = 0; slot < full.size(); ++slot)
        {
            auto & choices = result.domain[slot];
            std::set<std::int32_t> seen;
            for (const auto choice : choices)
            {
                if (std::find(full[slot].begin(), full[slot].end(), choice) == full[slot].end() ||
                    !seen.insert(choice).second)
                {
                    result.reason = QStringLiteral("unknown, duplicated or changed fixed candidate");
                    return false;
                }
            }
            std::sort(choices.begin(), choices.end(), [&](auto left, auto right)
            {
                return std::find(full[slot].begin(), full[slot].end(), left) <
                       std::find(full[slot].begin(), full[slot].end(), right);
            });
            result.reducedDomain = result.reducedDomain || choices != full[slot];
        }
        if (result.reducedDomain && request.restriction.isEmpty())
        {
            result.reason = QStringLiteral("reduced domain requires an explicit restriction");
            return false;
        }
        result.tupleProduct = tupleProduct(result.domain);
        if (!result.tupleProduct)
        {
            result.status = ResultStatus::ArithmeticFailure;
            result.reason = QStringLiteral("tuple product overflow");
            return false;
        }
        return true;
    }

    void compareReconstruction(const MasterAssignmentProblem & problem, TupleReceipt & receipt, Result & result)
    {
        const auto production = replayMasterAssignment(problem, receipt.choices);
        receipt.productionValid = production.valid();
        receipt.productionComplete = production.complete();
        receipt.productionError = production.error;
        const auto & reference = receipt.reference;
        receipt.comparisonApplicable = reference.status == TupleStatus::Feasible ||
                                       reference.status == TupleStatus::Infeasible;
        if (!receipt.comparisonApplicable)
        {
            return;
        }
        result.falsePositives += !reference.complete && production.complete() ? 1 : 0;
        result.falseNegatives += reference.complete && !production.complete() ? 1 : 0;
        bool equal = reference.complete == production.complete() &&
                     reference.representationValid == production.valid();
        if (reference.representationValid && production.valid())
        {
            equal = equal && reference.plan.equivalentState(production.plan) &&
                    reference.ordering.order == production.ordering.order &&
                    reference.ordering.cycleMembers == production.ordering.cycleMembers &&
                    reference.ordering.blockedByCycle == production.ordering.blockedByCycle &&
                    reference.actionIndices == production.actionIndices &&
                    reference.seatedValues == production.seatedValues &&
                    reference.seatedTotal == production.seatedEconomicTotal &&
                    reference.key == production.continuationKey;
        }
        receipt.comparisonEqual = equal;
        result.reconstructionMismatches += equal ? 0 : 1;
    }

    void selectRepresentative(const MasterAssignmentProblem & problem, Result & result)
    {
        const auto index = result.tuples.size() - 1;
        const auto & candidate = result.tuples[index];
        if (candidate.objective.status != ObjectiveStatus::Exact)
        {
            return;
        }
        const bool incumbent = std::equal(
            candidate.choices.begin(), candidate.choices.end(),
            problem.incumbentChoices().begin(), problem.incumbentChoices().end());
        if (!result.representative ||
            candidate.objective.total > result.tuples[*result.representative].objective.total ||
            (candidate.objective.total == result.tuples[*result.representative].objective.total && incumbent))
        {
            result.representative = index;
            result.incumbentRetained = incumbent;
        }
    }

    void finish(Result & result)
    {
        if (result.status == ResultStatus::ArithmeticFailure || result.status == ResultStatus::InvalidInput)
        {
            return;
        }
        if (result.reconstructionMismatches != 0)
        {
            result.status = ResultStatus::ReconstructionMismatch;
        }
        else if (!result.enumerationComplete)
        {
            result.status = ResultStatus::Incomplete;
        }
        else if (result.unsupported != 0)
        {
            result.status = ResultStatus::Unsupported;
        }
        else if (result.unresolved != 0)
        {
            result.status = ResultStatus::NonExactObjective;
        }
        else if (result.feasible == 0)
        {
            result.status = ResultStatus::NoFeasibleAssignment;
        }
        else
        {
            result.status = ResultStatus::ExactOptimum;
            result.exactOptimum = result.tuples[*result.representative].objective.total;
        }
    }

    bool nextTuple(const std::vector<std::vector<std::int32_t>> & domain, std::vector<std::size_t> & digits)
    {
        for (std::size_t index = digits.size(); index > 0; --index)
        {
            const auto slot = index - 1;
            if (++digits[slot] < domain[slot].size())
            {
                return true;
            }
            digits[slot] = 0;
        }
        return false;
    }
}

namespace MasterAssignmentOracle
{
    std::optional<std::uint64_t> tupleProduct(const std::vector<std::vector<std::int32_t>> & domain)
    {
        if (std::any_of(domain.begin(), domain.end(), [](const auto & choices) { return choices.empty(); }))
        {
            return 0;
        }
        std::uint64_t product = 1;
        for (const auto & choices : domain)
        {
            if (product > std::numeric_limits<std::uint64_t>::max() / choices.size())
            {
                return std::nullopt;
            }
            product *= choices.size();
        }
        return product;
    }

    std::vector<std::vector<std::int32_t>> fullDomain(const MasterAssignmentProblem & problem)
    {
        std::vector<std::vector<std::int32_t>> result;
        for (const auto & actor : problem.actors())
        {
            const bool immutable = actor.role == MasterActorRole::Fixed ||
                (actor.actionIndex != NO_ACTION &&
                 problem.actionTemplates()[actor.actionIndex].state == PlanActionState::Committed);
            result.push_back(immutable ? std::vector<std::int32_t>{actor.incumbentCandidateIndex}
                                       : actor.canonicalCandidates);
        }
        return result;
    }

    Result enumerate(const MasterAssignmentProblem & problem, const Request & request)
    {
        const auto start = std::chrono::steady_clock::now();
        Result result;
        if (!normalizeDomain(problem, request, result))
        {
            return result;
        }
        result.status = ResultStatus::Incomplete;
        std::vector<std::size_t> digits(result.domain.size(), 0);
        while (result.evaluated < *result.tupleProduct && result.evaluated < request.tupleLimit &&
               (!request.interrupted || !request.interrupted()))
        {
            TupleReceipt receipt;
            for (std::size_t slot = 0; slot < digits.size(); ++slot)
            {
                receipt.choices.push_back(result.domain[slot][digits[slot]]);
            }
            receipt.reference = constructReference(problem, receipt.choices);
            compareReconstruction(problem, receipt, result);
            ++result.evaluated;
            switch (receipt.reference.status)
            {
                case TupleStatus::Feasible:
                    ++result.feasible;
                    receipt.objective = evaluateObjective(receipt.reference, request.continuation,
                                                          request.continuationLimit);
                    for (const auto work : {receipt.objective.friendlyProof.work, receipt.objective.enemyProof.work})
                    {
                        if (result.continuationWork > std::numeric_limits<std::uint64_t>::max() - work)
                        {
                            result.status = ResultStatus::ArithmeticFailure;
                            result.reason = QStringLiteral("continuation work counter overflow");
                        }
                        else
                        {
                            result.continuationWork += work;
                        }
                    }
                    if (receipt.objective.status != ObjectiveStatus::Exact)
                    {
                        ++result.unresolved;
                    }
                    if (receipt.objective.status == ObjectiveStatus::ArithmeticFailure)
                    {
                        result.status = ResultStatus::ArithmeticFailure;
                    }
                    else if (receipt.objective.status == ObjectiveStatus::InvalidInput ||
                             receipt.objective.status == ObjectiveStatus::WitnessFailure)
                    {
                        result.status = ResultStatus::InvalidInput;
                    }
                    break;
                case TupleStatus::Infeasible:
                    ++result.infeasible;
                    break;
                case TupleStatus::Unsupported:
                    ++result.unsupported;
                    ++result.unresolved;
                    break;
                case TupleStatus::InvalidInput:
                    ++result.unresolved;
                    result.status = ResultStatus::InvalidInput;
                    break;
                case TupleStatus::ArithmeticFailure:
                    ++result.unresolved;
                    result.status = ResultStatus::ArithmeticFailure;
                    break;
            }
            result.tuples.push_back(std::move(receipt));
            selectRepresentative(problem, result);
            if (!nextTuple(result.domain, digits))
            {
                break;
            }
        }
        result.enumerationComplete = result.evaluated == *result.tupleProduct;
        finish(result);
        result.elapsedNanos = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
        return result;
    }

    SearchClaimCheck checkSearchClaim(const Result & exact, MilliFunds lower, MilliFunds upper,
                                     std::optional<MilliFunds> completed)
    {
        if (exact.status != ResultStatus::ExactOptimum || !exact.exactOptimum)
        {
            return SearchClaimCheck::NoExactReference;
        }
        if (lower > upper)
        {
            return SearchClaimCheck::InvalidInterval;
        }
        if (lower > *exact.exactOptimum)
        {
            return SearchClaimCheck::LowerAboveOptimum;
        }
        if (upper < *exact.exactOptimum)
        {
            return SearchClaimCheck::UpperBelowOptimum;
        }
        if (completed && *completed != *exact.exactOptimum)
        {
            return SearchClaimCheck::CompletedValueMismatch;
        }
        return SearchClaimCheck::Consistent;
    }

    bool retainsOptimum(const Result & exact, std::span<const std::vector<std::int32_t>> remainingTuples)
    {
        if (exact.status != ResultStatus::ExactOptimum || !exact.exactOptimum)
        {
            return false;
        }
        for (const auto & receipt : exact.tuples)
        {
            if (receipt.reference.complete && receipt.objective.status == ObjectiveStatus::Exact &&
                receipt.objective.total == *exact.exactOptimum &&
                std::find(remainingTuples.begin(), remainingTuples.end(), receipt.choices) != remainingTuples.end())
            {
                return true;
            }
        }
        return false;
    }
}
