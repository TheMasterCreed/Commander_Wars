#pragma once

#include <cstdio>
#include <cstdlib>

#include "ai/coordinator/bundleassignment.h"
#include "tests/coordinator_ai/masterassignmentoracle.h"

namespace MasterAssignmentTest
{
    using namespace Coordinator;

    inline int checks = 0;
    inline int failures = 0;

    inline void expect(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::printf("FAIL: %s\n", message);
        }
    }

    inline void require(bool condition, const char* message)
    {
        expect(condition, message);
        if (!condition)
        {
            std::exit(4);
        }
    }

    inline CandidateBundle moveCandidate(std::int32_t actor, TilePoint origin, TilePoint destination,
                                          MilliFunds value, std::int32_t generation = 0)
    {
        CandidateBundle result;
        result.generationIndex = generation;
        result.bundle.unitId = actor;
        result.bundle.origin = origin;
        result.bundle.destination = destination;
        result.bundle.path = {origin};
        if (origin != destination)
        {
            result.bundle.path.push_back(destination);
        }
        result.valuation.valid = true;
        result.valuation.ledger.enemyCapital = value;
        return result;
    }

    struct Fixture
    {
        AssignmentInput input;
        TurnPlan plan;
        std::vector<MasterActorSelection> selections;

        explicit Fixture(std::int32_t count)
        {
            input.actionIds = {QStringLiteral("ACTION_WAIT"), QStringLiteral("ACTION_FIRE"),
                               QStringLiteral("ACTION_CAPTURE")};
            for (std::int32_t actor = 0; actor < count; ++actor)
            {
                const TilePoint origin{actor, 0};
                input.unitLinks.push_back({100 + actor, origin});
                input.actors.push_back({actor, 100 + actor,
                                        {moveCandidate(actor, origin, origin, actor + 1)}});
            }
        }

        void seat(std::span<const std::int32_t> choices = {})
        {
            plan = TurnPlan{};
            selections.clear();
            for (std::size_t slot = 0; slot < input.actors.size(); ++slot)
            {
                const auto & actor = input.actors[slot];
                const auto choice = choices.empty() ? 0 : choices[slot];
                const auto & candidate = actor.candidates[static_cast<std::size_t>(choice)];
                const auto index = plan.addAction(plannedActionFrom(
                    input.actionIds, input.unitLinks, actor.engineUnitId, candidate));
                require(index != NO_ACTION, "fixture action construction");
                require(claimFor(plan, index, candidate) == ReservationResult::Granted, "fixture native claims");
                selections.push_back({actor.engineUnitId, index, choice, MasterActorRole::Mutable});
            }
            std::vector<OccupancyEntry> occupancy;
            for (const auto & link : input.unitLinks)
            {
                occupancy.push_back({link.engineUnitId, link.tile});
            }
            require(plan.rebuildVacateEdges(occupancy).empty(), "fixture occupancy");
        }

        MasterAssignmentProblem extract() const
        {
            const auto order = plan.executionOrder().order;
            const auto result = buildMasterAssignmentProblem(input, {
                .sessionIdentity = 73,
                .selections = selections,
                .pIncumbentPlan = &plan,
                .incumbentExecutionOrder = order,
            });
            require(result.valid(), "fixture extracts");
            return *result.problem;
        }
    };

    inline MasterAssignmentOracle::Request request()
    {
        auto revision = QString::fromLocal8Bit(qgetenv("COW_ORACLE_SOURCE_REVISION"));
        if (revision.isEmpty())
        {
            revision = QStringLiteral("phase3-working");
        }
        return {.sourceRevision = revision, .fixture = QStringLiteral("master-search")};
    }
}
