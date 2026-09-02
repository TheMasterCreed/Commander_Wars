#pragma once

#include <functional>
#include <map>
#include <set>

#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/propertystockfield.h"
#include "ai/coordinator/refinementledger.h"

namespace RelativeStockTests
{
    using namespace Coordinator;

    struct SearchPrefix
    {
        std::int64_t states{0};
        StockInterval bounds;
    };

    struct Term
    {
        StockInterval bounds{0, 200};
        MilliFunds exactValue{100};
        std::int64_t requiredStates{1};
        bool completed{false};
        bool witness{true};
        std::optional<SearchPrefix> prefix;
    };

    struct Problem
    {
        Term ours;
        std::vector<std::int32_t> captured;
        MilliFunds owned{0};
    };

    class Session final : public RelativeStockSession
    {
    public:
        Session(const PropertyStockField & field, const PropertyStockInstance & enemy,
                std::int64_t budget = LIVE_PAIR_REFINEMENT_BUDGET)
            : RelativeStockSession(field, enemy)
        {
            ledger.open(budget);
            enemies.emplace(std::vector<std::int32_t>{}, Term{{0, 1000}, 100, 1});
        }

        RelativeStockQuote quote(const TurnPlan & plan, MilliFunds economic) override
        {
            for (std::int32_t index = 0; index < plan.edgeCount(); ++index)
            {
                quotedVacateEdges += plan.edge(index).kind == PlanEdgeKind::Vacate ? 1 : 0;
            }
            ContinuationKey key;
            for (std::int32_t index = 0; index < plan.actionCount(); ++index)
            {
                const auto & action = plan.action(index);
                if (isLiveState(action.state) && stockActors.contains(action.unitId))
                {
                    key.push_back(CanonicalPlanActionKey::fromAction(
                        action.unitId, action.destination.x, action.destination.y,
                        action.kind == PlanBundleKind::Capture || action.kind == PlanBundleKind::MoveAndCapture));
                }
            }
            std::sort(key.begin(), key.end());
            return quote(key, economic);
        }

        RelativeStockQuote quote(const ContinuationKey & key, MilliFunds economic) override
        {
            auto found = plans.find(key);
            if (found == plans.end())
            {
                Problem problem;
                problem.ours.exactValue = evaluate ? evaluate(key) : 100;
                problem.ours.bounds.upper = problem.ours.exactValue + 100;
                found = plans.emplace(key, std::move(problem)).first;
            }
            const auto & problem = found->second;
            const auto & enemy = enemies.at(problem.captured);
            RelativeStockQuote result;
            result.identity = identity();
            result.key = key;
            result.economic = economic;
            result.owned = problem.owned;
            result.capturedColumns = problem.captured;
            result.ours = problem.ours.bounds;
            result.enemy = enemy.bounds;
            result.ourLowerWitnessReplays = problem.ours.witness;
            result.enemyLowerWitnessReplays = enemy.witness;
            result.ourSearchCompleted = problem.ours.completed;
            result.enemySearchCompleted = enemy.completed;
            const auto certificate =
                dualCertificates.find(key);
            if (certificate !=
                dualCertificates.end())
            {
                result.ourUpperCertificateId =
                    certificate->second.id;
            }
            result.valid = !invalidateOnRefinement || (ourCalls == 0 && enemyCalls == 0);
            return result;
        }

        RelativeStockRefinedTerm refineOur(const ContinuationKey & key, std::int64_t cap) override
        {
            ++ourCalls;
            return refine(plans.at(key).ours, cap, ourTariff);
        }

        RelativeStockRefinedTerm refineEnemy(const std::vector<std::int32_t> & captured, std::int64_t cap) override
        {
            ++enemyCalls;
            return refine(enemies.at(captured), cap, enemyTariff);
        }

        bool replayOurWitness(const RelativeStockQuote & expected) override
        {
            ++ourReplayCalls;
            return !failReplay && owns(expected) && expected.ourLowerWitnessReplays &&
                (!failReplayKey.has_value() || expected.key != *failReplayKey) &&
                sameRelativeStockQuote(expected, quote(expected.key, expected.economic));
        }

        bool replayEnemyWitness(const RelativeStockQuote & expected) override
        {
            ++enemyReplayCalls;
            return !failReplay && owns(expected) && expected.enemyLowerWitnessReplays &&
                sameRelativeStockQuote(expected, quote(expected.key, expected.economic));
        }

        std::int64_t remainingWork() const override { return ledger.balance(); }
        std::int64_t ourRerunWork() const override { return ourTariff; }
        std::int64_t enemyRerunWork(const std::vector<std::int32_t> &) const override { return enemyTariff; }
        bool supportsVerifiedDual() const override
        {
            return dualEnabled;
        }

        RelativeStockDualTightening
        tightenOurUpperWithVerifiedDual(
            const RelativeStockQuote & expected,
            std::int64_t maximumWork) override
        {
            ++dualCalls;
            RelativeStockDualTightening result;
            result.intervalBefore = expected.ours;
            result.intervalAfter = expected.ours;
            const auto problem = plans.find(expected.key);
            if (!dualEnabled || !owns(expected) ||
                problem == plans.end() ||
                problem->second.ours.bounds.lower !=
                    expected.ours.lower ||
                problem->second.ours.bounds.upper !=
                    expected.ours.upper)
            {
                result.globalFailure = true;
                return result;
            }
            const auto known =
                dualCertificates.find(expected.key);
            if (known != dualCertificates.end())
            {
                ++dualReuses;
                result.certifiedUpper =
                    known->second.upper;
                result.certificate =
                    known->second.certificate;
                result.certificateId =
                    known->second.id;
                result.intervalAfter =
                    problem->second.ours.bounds;
                result.catalogComplete = true;
                result.certificateValid = true;
                result.optimizerCompleted = true;
                result.verifierCompleted = true;
                result.reused = true;
                return result;
            }
            const auto configured =
                dualUppers.find(expected.key);
            if (configured == dualUppers.end())
            {
                result.globalFailure = true;
                return result;
            }
            std::int64_t required = ourTariff;
            if (!RelativeStockDetail::checkedAdd(
                    required,
                    dualCatalogWork) ||
                !RelativeStockDetail::checkedAdd(
                    required,
                    dualOptimizerWork) ||
                !RelativeStockDetail::checkedAdd(
                    required,
                    dualVerifierWork))
            {
                result.globalFailure = true;
                return result;
            }
            if (maximumWork < required ||
                ledger.balance() < required)
            {
                result.locallyBudgetExhausted = true;
                return result;
            }
            if (ledger.draw(
                    required,
                    RefinementWork::
                        PackingDualCertificate) ==
                NO_GRANT)
            {
                result.locallyBudgetExhausted = true;
                return result;
            }
            result.tariffWork = ourTariff;
            result.catalogWork = dualCatalogWork;
            result.optimizerWork = dualOptimizerWork;
            result.verifierWork = dualVerifierWork;
            result.totalWork = required;
            result.catalogComplete = true;
            result.optimizerCompleted = true;
            result.verifierCompleted = true;
            result.certificateValid = true;
            result.certifiedUpper = configured->second;
            if (result.certifiedUpper <
                problem->second.ours.bounds.lower)
            {
                result.certificateValid = false;
                result.globalFailure = true;
                return result;
            }
            auto certificate =
                std::make_shared<PackingDualCertificate>();
            certificate->identity.session =
                sessionIdentity();
            certificate->identity.key = expected.key;
            certificate->identity.incumbentFloor =
                problem->second.ours.bounds.lower;
            certificate->dualObjective =
                result.certifiedUpper;
            result.certificate = certificate;
            result.certificateId =
                nextDualCertificateId++;
            problem->second.ours.bounds.upper =
                std::min(
                    problem->second.ours.bounds.upper,
                    result.certifiedUpper);
            result.intervalAfter =
                problem->second.ours.bounds;
            dualCertificates.emplace(
                expected.key,
                DualCertificate{
                    .id = result.certificateId,
                    .upper = result.certifiedUpper,
                    .certificate =
                        std::move(certificate),
                });
            ++dualBuilds;
            if (corruptDualReceipt)
            {
                ++result.certificateId;
            }
            return result;
        }

        std::map<ContinuationKey, Problem> plans;
        std::map<std::vector<std::int32_t>, Term> enemies;
        struct DualCertificate
        {
            std::uint64_t id{0};
            MilliFunds upper{0};
            std::shared_ptr<const PackingDualCertificate>
                certificate;
        };
        std::map<ContinuationKey, MilliFunds> dualUppers;
        std::map<ContinuationKey, DualCertificate>
            dualCertificates;
        std::set<std::int32_t> stockActors;
        std::function<MilliFunds(const ContinuationKey &)> evaluate;
        RefinementLedger ledger;
        std::int64_t ourTariff{3};
        std::int64_t enemyTariff{5};
        std::int32_t ourCalls{0};
        std::int32_t enemyCalls{0};
        std::int32_t ourReplayCalls{0};
        std::int32_t enemyReplayCalls{0};
        std::int32_t dualCalls{0};
        std::int32_t dualBuilds{0};
        std::int32_t dualReuses{0};
        std::int32_t quotedVacateEdges{0};
        std::int64_t dualCatalogWork{2};
        std::int64_t dualOptimizerWork{4};
        std::int64_t dualVerifierWork{3};
        std::uint64_t nextDualCertificateId{1};
        std::optional<ContinuationKey> failReplayKey;
        bool failWitness{false};
        bool widen{false};
        bool failReplay{false};
        bool exceedCap{false};
        bool invalidateOnRefinement{false};
        bool dualEnabled{false};
        bool corruptDualReceipt{false};

    private:
        bool verifiedUpperMatches(
            const RelativeStockQuote & expected) const override
        {
            if (expected.ourUpperCertificateId == 0)
            {
                return true;
            }
            const auto known =
                dualCertificates.find(expected.key);
            const auto problem =
                plans.find(expected.key);
            return known != dualCertificates.end() &&
                problem != plans.end() &&
                known->second.id ==
                    expected.ourUpperCertificateId &&
                known->second.certificate != nullptr &&
                known->second.certificate->identity.session ==
                    sessionIdentity() &&
                known->second.certificate->identity.key ==
                    expected.key &&
                problem->second.ours.bounds.lower ==
                    expected.ours.lower &&
                problem->second.ours.bounds.upper ==
                    expected.ours.upper &&
                expected.ours.lower <=
                    known->second.upper &&
                expected.ours.upper <=
                    known->second.upper;
        }

        RelativeStockRefinedTerm refine(Term & term, std::int64_t cap, std::int64_t tariff)
        {
            RelativeStockRefinedTerm result;
            const auto grant = ledger.draw(tariff + cap, RefinementWork::SearchSlice);
            if (grant == NO_GRANT)
            {
                result.budgetExhausted = true;
                return result;
            }
            const auto used = std::min(cap, term.requiredStates);
            ledger.refund(grant, cap - used);
            result.statesUsed = exceedCap ? cap + 1 : used;
            result.workUsed = tariff + used;
            result.searchCompleted = cap >= term.requiredStates;
            result.interval = result.searchCompleted ? StockInterval{term.exactValue, term.exactValue} : term.bounds;
            if (!result.searchCompleted && term.prefix.has_value() && cap >= term.prefix->states)
            {
                result.interval.lower = std::max(result.interval.lower, term.prefix->bounds.lower);
                result.interval.upper = std::min(result.interval.upper, term.prefix->bounds.upper);
            }
            if (widen)
            {
                result.interval = {term.bounds.lower - 1, term.bounds.upper + 1};
                result.searchCompleted = false;
            }
            result.lowerWitnessReplays = !failWitness;
            result.valid = true;
            if (!failWitness)
            {
                term.bounds = result.interval;
                term.completed = result.searchCompleted;
            }
            return result;
        }
    };

    class Valuer final : public PlanStockValuer
    {
    public:
        explicit Valuer(Session & session) : session(session)
        {
            const auto admission = session.ourTariff + session.enemyTariff + 2 * BOUNDARY_BASE_CAP;
            registered.open(static_cast<std::int32_t>(session.remainingWork() / admission));
        }

        MilliFunds planStock(const TurnPlan &) override { ++scalarCalls; return 0; }
        MilliFunds originStock() const override { return 0; }
        MilliFunds stockCeiling() const override { return 1000000; }
        bool affectsStock(std::int32_t unit) const override { return session.stockActors.contains(unit); }
        bool livePairSwapIntervals() const override { return true; }
        bool liveSettlingIntervals() const override { return true; }
        RelativeStockSession* relativeStockSession() override { return relativeEnabled ? &session : nullptr; }
        LivePlanStockQuote livePlanStock(const TurnPlan & plan, MilliFunds economic, bool) override
        {
            const auto result = session.quote(plan, economic);
            const auto absolute = composeStockInterval(result.owned, result.ours, result.enemy);
            if (result.valid)
            {
                registered.encounter(result.key, economic + absolute.lower, economic + absolute.upper);
                if (absolute.exact())
                {
                    registered.markResolved(result.key);
                }
            }
            return {result.key, absolute, result.valid, result.ourLowerWitnessReplays};
        }
        LiveRefinementResult refineLiveAtBoundary(AssignPhase, std::int64_t maximumWork) override
        {
            ++absoluteRefinements;
            boundaryAllowances.push_back(maximumWork);
            LiveRefinementResult result;
            if (!boundaryEnabled)
            {
                return result;
            }
            for (const auto & key : registered.refinementOrder())
            {
                const auto step = refineKey(key, maximumWork - result.workUsed);
                result.refined = result.refined || step.refined;
                result.keysRefined += step.keysRefined;
                result.statesUsed += step.statesUsed;
                result.enemyStatesUsed += step.enemyStatesUsed;
                result.workUsed += step.workUsed;
                result.budgetExhausted = result.budgetExhausted || step.budgetExhausted;
                result.valid = result.valid && step.valid;
                if (!result.valid || step.workUsed > 0)
                {
                    break;
                }
            }
            boundaryResults.push_back(result);
            return result;
        }

        Session & session;
        ContenderRegistry registered;
        std::map<ContinuationKey, std::int64_t> boundaryCaps;
        std::vector<std::int64_t> boundaryAllowances;
        std::vector<LiveRefinementResult> boundaryResults;
        std::int32_t scalarCalls{0};
        std::int32_t absoluteRefinements{0};
        bool relativeEnabled{true};
        bool boundaryEnabled{false};
        bool globalClosed{false};
        bool pricingFailure{false};

    private:
        static constexpr std::int64_t BOUNDARY_BASE_CAP = 1;

        LiveRefinementResult refineKey(const ContinuationKey & key, std::int64_t maximumWork)
        {
            LiveRefinementResult result;
            const auto before = session.quote(key, 0);
            const bool ours = !before.ours.exact();
            const bool enemy = !before.enemy.exact();
            const auto found = boundaryCaps.find(key);
            const auto cap = found == boundaryCaps.end() ? BOUNDARY_BASE_CAP : found->second;
            const auto request = (ours ? session.ourTariff + cap : 0) + (enemy ? session.enemyTariff + cap : 0);
            if (request > maximumWork)
            {
                result.budgetExhausted = true;
                return result;
            }
            const auto balance = session.remainingWork();
            if (ours)
            {
                const auto term = session.refineOur(key, cap);
                result.statesUsed += term.statesUsed;
                result.valid = result.valid && term.valid;
                result.budgetExhausted = result.budgetExhausted || term.budgetExhausted;
            }
            if (enemy)
            {
                const auto term = session.refineEnemy(before.capturedColumns, cap);
                result.statesUsed += term.statesUsed;
                result.enemyStatesUsed += term.statesUsed;
                result.valid = result.valid && term.valid;
                result.budgetExhausted = result.budgetExhausted || term.budgetExhausted;
            }
            result.workUsed = balance - session.remainingWork();
            if (!result.valid)
            {
                globalClosed = true;
                pricingFailure = !result.budgetExhausted;
                return result;
            }
            const auto after = session.quote(key, 0);
            result.refined = after.ours.lower > before.ours.lower || after.ours.upper < before.ours.upper ||
                after.enemy.lower > before.enemy.lower || after.enemy.upper < before.enemy.upper;
            result.keysRefined = result.refined ? 1 : 0;
            registered.markAdmitted(key);
            if (after.absoluteExact())
            {
                registered.markResolved(key);
            }
            return result;
        }
    };
}
