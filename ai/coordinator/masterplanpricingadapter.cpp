#include "ai/coordinator/masterplanpricingadapter.h"

#include <bit>

#include "ai/coordinator/relativestockcomparison.h"

namespace
{
    using namespace Coordinator;

    bool modelled(const MasterAssignmentProblem & problem, const MasterActor & actor)
    {
        return actor.stockRow != NO_STOCK_ROW &&
               problem.incumbentChoices()[static_cast<std::size_t>(actor.canonicalSlot)] != NO_CANDIDATE;
    }

    std::vector<std::int32_t> pricedCandidates(const MasterAssignmentProblem & problem, const MasterActor & actor)
    {
        if (!problem.canChangeActor(actor.canonicalSlot))
        {
            return {problem.incumbentChoices()[static_cast<std::size_t>(actor.canonicalSlot)]};
        }
        std::vector<std::int32_t> result;
        for (const auto index : actor.canonicalCandidates)
        {
            if (actor.candidates[static_cast<std::size_t>(index)].plannable)
            {
                result.push_back(index);
            }
        }
        return result;
    }

    bool addProduct(std::int64_t & total, std::size_t count, std::size_t units)
    {
        const auto product = masterWorkProduct(count, units);
        return product && RelativeStockDetail::checkedAdd(total, *product);
    }

    std::size_t actionSearchWork(std::size_t actions)
    {
        return static_cast<std::size_t>(std::bit_width(actions + 1)) + 1;
    }

    std::optional<MasterPlanPricingCandidate> pricedCandidate(
        const MasterActor & actor, std::int32_t index, const CheapPricingModel & stock,
        const MasterFutureCatalog & catalog)
    {
        const auto & source = actor.candidates[static_cast<std::size_t>(index)];
        MasterPlanPricingCandidate result;
        result.candidateIndex = index;
        result.key = CanonicalPlanActionKey::fromAction(actor.stockRow, source.stockEndpoint.x, source.stockEndpoint.y,
                                                        source.capture.has_value());
        result.resources = source.resources;
        const auto* action = findMasterFutureAction(catalog, result.key);
        const auto* terms = stock.actionTerms(result.key);
        if (action == nullptr || terms == nullptr || action->row.weight.size() != catalog.columns.size() ||
            action->row.ownedTurn.size() != catalog.columns.size())
        {
            return std::nullopt;
        }
        result.row = action->row;
        result.immediateValue = source.immediateValue.economicValue;
        if (terms->capturedColumn < 0)
        {
            return result;
        }
        const auto swings = stock.capturedColumnSwings();
        const auto column = static_cast<std::size_t>(terms->capturedColumn);
        const auto node = futureNodeOfColumn(catalog.columns, terms->capturedColumn);
        if (node == NO_SEQUENTIAL_NODE || column >= swings.size() ||
            !RelativeStockDetail::checkedAdd(result.immediateValue, swings[column]))
        {
            return std::nullopt;
        }
        result.capturedNode = node;
        removeCapturedNode(result.row, node);
        return result;
    }
}

namespace Coordinator
{
    std::optional<std::int64_t> masterPlanAdapterWork(const MasterAssignmentProblem & problem,
                                                      const MasterFutureCatalog & catalog)
    {
        const auto nodes = catalog.columns.size() + 1;
        const auto search = actionSearchWork(catalog.actions.size());
        std::int64_t total = 0;
        if (!addProduct(total, problem.actors().size() + 1, 1) ||
            !addProduct(total, catalog.dayRows.size() + catalog.classes.size() + 1, nodes))
        {
            return std::nullopt;
        }
        for (const auto & actor : problem.actors())
        {
            for (const auto & candidate : actor.candidates)
            {
                if (!addProduct(total, 1, candidate.resources.size() + nodes + search))
                {
                    return std::nullopt;
                }
            }
        }
        return total;
    }

    std::optional<MasterPlanPricingInstance> buildMasterPlanPricingInstance(
        const MasterAssignmentProblem & problem, const CheapPricingModel & stock,
        const MasterFutureCatalog & catalog, RefinementLedger & ledger, std::int64_t reserve)
    {
        const auto work = masterPlanAdapterWork(problem, catalog);
        auto required = work.value_or(0);
        if (!catalog.complete || catalog.dayRows.size() != stock.dayRows().size() || !work || reserve < 0 ||
            !RelativeStockDetail::checkedAdd(required, reserve) || ledger.balance() < required ||
            !ledger.consume(*work, RefinementWork::MasterPreparation))
        {
            return std::nullopt;
        }
        MasterPlanPricingInstance result;
        result.work = *work;
        auto & model = result.model;
        model.catalog.horizon = catalog.horizon;
        model.catalog.columns = catalog.columns;
        model.catalog.classes = catalog.classes;
        model.catalog.dayRows = catalog.dayRows;
        model.catalog.complete = true;
        model.rowActors.assign(catalog.dayRows.size(), MASTER_PLAN_NO_ACTOR);
        for (const auto & actor : problem.actors())
        {
            const auto slot = static_cast<std::size_t>(actor.canonicalSlot);
            const auto incumbent = problem.incumbentChoices()[slot];
            if (!modelled(problem, actor))
            {
                if (incumbent != NO_CANDIDATE)
                {
                    const auto & resources = actor.candidates[static_cast<std::size_t>(incumbent)].resources;
                    model.occupiedResources.insert(model.occupiedResources.end(), resources.begin(), resources.end());
                }
                continue;
            }
            const auto row = static_cast<std::size_t>(actor.stockRow);
            if (row >= model.rowActors.size() || model.rowActors[row] != MASTER_PLAN_NO_ACTOR)
            {
                return std::nullopt;
            }
            MasterPlanPricingActor priced{actor.stockRow, {}};
            for (const auto index : pricedCandidates(problem, actor))
            {
                auto candidate = pricedCandidate(actor, index, stock, catalog);
                if (!candidate)
                {
                    return std::nullopt;
                }
                priced.candidates.push_back(std::move(*candidate));
            }
            if (findMasterPlanCandidate(priced, incumbent) == nullptr)
            {
                return std::nullopt;
            }
            model.rowActors[row] = static_cast<std::int32_t>(model.actors.size());
            model.actors.push_back(std::move(priced));
            result.actorSlots.push_back(actor.canonicalSlot);
        }
        if (!indexMasterPlanActions(model))
        {
            return std::nullopt;
        }
        return result;
    }

    std::optional<MasterAssignmentState> masterPlanWitnessState(
        const MasterAssignmentProblem & problem, const MasterPlanPricingInstance & instance,
        const MasterPlanWitness & witness)
    {
        if (!witness.valid || witness.candidateIndices.size() != instance.actorSlots.size())
        {
            return std::nullopt;
        }
        MasterAssignmentState result{problem.identity(), {problem.incumbentChoices().begin(),
                                                          problem.incumbentChoices().end()}};
        for (std::size_t slot = 0; slot < instance.actorSlots.size(); ++slot)
        {
            result.choices[static_cast<std::size_t>(instance.actorSlots[slot])] = witness.candidateIndices[slot];
        }
        return result;
    }

    std::optional<MasterPlanFriendlyWitness> masterPlanFriendlyWitness(const MasterPlanWitness & witness)
    {
        if (!witness.valid)
        {
            return std::nullopt;
        }
        MasterPlanFriendlyWitness result;
        for (const auto & row : witness.rows)
        {
            if (row.steps.empty())
            {
                continue;
            }
            if (!RelativeStockDetail::checkedAdd(result.value, row.value))
            {
                return std::nullopt;
            }
            result.rows.push_back(row);
        }
        return result;
    }

    std::optional<std::int64_t> masterPlanRestrictionWork(const MasterPlanPricingModel & model)
    {
        const auto search = actionSearchWork(model.catalog.actions.size());
        std::int64_t total = 0;
        if (!addProduct(total, model.actors.size() + 1, 1))
        {
            return std::nullopt;
        }
        for (const auto & actor : model.actors)
        {
            for (const auto & candidate : actor.candidates)
            {
                if (!addProduct(total, 1, candidate.resources.size() + candidate.row.weight.size() + search))
                {
                    return std::nullopt;
                }
            }
        }
        return total;
    }

    std::optional<std::vector<std::int32_t>> masterPlanStateChoices(
        const MasterAssignmentProblem & problem, const MasterPlanPricingInstance & instance,
        const MasterAssignmentState & state)
    {
        if (state.choices.size() != problem.actors().size() ||
            instance.actorSlots.size() != instance.model.actors.size())
        {
            return std::nullopt;
        }
        std::vector<bool> priced(problem.actors().size(), false);
        std::vector<std::int32_t> result;
        for (std::size_t actor = 0; actor < instance.actorSlots.size(); ++actor)
        {
            const auto slot = static_cast<std::size_t>(instance.actorSlots[actor]);
            const auto choice = state.choices[slot];
            if (findMasterPlanCandidate(instance.model.actors[actor], choice) == nullptr)
            {
                return std::nullopt;
            }
            priced[slot] = true;
            result.push_back(choice);
        }
        // An actor outside the model is held at its incumbent, whose resources the model books as occupied.
        for (std::size_t slot = 0; slot < priced.size(); ++slot)
        {
            if (!priced[slot] && state.choices[slot] != problem.incumbentChoices()[slot])
            {
                return std::nullopt;
            }
        }
        return result;
    }
}
