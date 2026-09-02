#include <iostream>
#include <string_view>

#include "tests/arena/relativestocksupportaudit.h"
#include "tests/arena/relativestockreceipts.h"
#include "tests/coordinator_ai/relativestocktestfixture.h"

namespace
{
using namespace Coordinator;
using Audit = ArenaRelativeStockSupportAudit;
int checks = 0;
int failures = 0;

void expect(bool condition, std::string_view name)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::cerr << "FAILED: " << name << '\n';
    }
}

AssignmentInput makeInput()
{
    AssignmentInput input;
    input.actionIds = {QStringLiteral("ACTION_WAIT"), QStringLiteral("ACTION_FIRE"), QStringLiteral("ACTION_CAPTURE")};
    input.relativeStockWorkLimit = 0;
    for (std::int32_t slot = 0; slot < 7; ++slot)
    {
        AssignmentActor actor;
        actor.knowledgeUnitIndex = slot;
        actor.engineUnitId = slot + 1;
        const TilePoint origin{0, slot * 2};
        input.unitLinks.push_back({actor.engineUnitId, origin});
        for (std::int32_t choice = 0; choice < 2; ++choice)
        {
            CandidateBundle candidate;
            candidate.bundle.unitId = slot;
            candidate.bundle.origin = origin;
            candidate.bundle.destination = {choice, origin.y};
            candidate.bundle.path.push_back(origin);
            if (choice != 0)
            {
                candidate.bundle.path.push_back(candidate.bundle.destination);
            }
            candidate.valuation.valid = true;
            candidate.valuation.ledger.enemyCapital = 10;
            actor.candidates.push_back(std::move(candidate));
        }
        input.actors.push_back(std::move(actor));
    }
    return input;
}

std::vector<std::int32_t> choices(const std::vector<ActorProgress> & actors)
{
    std::vector<std::int32_t> result;
    for (const auto & actor : actors)
    {
        result.push_back(actor.chosen);
    }
    return result;
}

void exactSupports(AssignmentInput & input, const AssignmentResult & incumbent)
{
    const auto actors = Audit::progress(input, incumbent);
    const auto before = Audit::planIdentity(incumbent);
    for (std::int32_t size = 1; size <= 4; ++size)
    {
        auto tuple = choices(actors);
        std::vector<std::int32_t> support;
        for (std::int32_t slot = 0; slot < size; ++slot)
        {
            support.push_back(slot);
            tuple[slot] = 1 - tuple[slot];
        }
        const auto trial = Audit::makeTrial(input, incumbent, actors, support, tuple);
        expect(trial.valid, "exact support is a legal complete plan");
        if (!trial.valid)
        {
            continue;
        }
        for (std::size_t slot = 0; slot < actors.size(); ++slot)
        {
            const auto actor = actors[slot].pActor->engineUnitId;
            const bool same = Audit::actorIdentity(trial.assignment, actor) == Audit::actorIdentity(incumbent, actor);
            expect(same == (slot >= static_cast<std::size_t>(size)), "only exact support members change");
        }
        MilliFunds economic = 0;
        expect(Audit::replayIncumbent(input, trial.assignment, economic) && economic == trial.economic,
               "complete challenger claims and economic total replay");
        const auto repeat = Audit::makeTrial(input, incumbent, actors, support, tuple);
        expect(repeat.valid && Audit::planIdentity(repeat.assignment) == Audit::planIdentity(trial.assignment),
               "identical support produces identical complete identity");
        expect(Audit::planIdentity(incumbent) == before, "trial never mutates the incumbent");
    }
}

void invalidSupports(AssignmentInput & input, const AssignmentResult & incumbent)
{
    const auto actors = Audit::progress(input, incumbent);
    const auto base = choices(actors);
    auto tuple = base;
    tuple[0] = 1 - tuple[0];
    expect(!Audit::makeTrial(input, incumbent, actors, {}, tuple).valid, "empty support cannot conceal a change");
    expect(!Audit::makeTrial(input, incumbent, actors, {0, 1}, tuple).valid, "support cannot retain an incumbent member");
    expect(!Audit::makeTrial(input, incumbent, actors, {0, 0}, tuple).valid, "duplicate support member rejected");
    expect(!Audit::makeTrial(input, incumbent, actors, {99}, tuple).valid, "unknown support member rejected");
    tuple[1] = 1 - tuple[1];
    expect(!Audit::makeTrial(input, incumbent, actors, {0}, tuple).valid, "unlisted changed actor rejected");
    tuple = base;
    tuple[0] = NO_CANDIDATE;
    expect(!Audit::makeTrial(input, incumbent, actors, {0}, tuple).valid, "assigned actor cannot become unassigned");
    tuple[0] = 10000;
    expect(!Audit::makeTrial(input, incumbent, actors, {0}, tuple).valid, "unknown candidate rejected");
    tuple = base;
    tuple.back() = 1 - tuple.back();
    expect(!Audit::makeTrial(input, incumbent, actors, {6}, tuple).valid, "non-stock support member rejected");
    tuple = base;
    for (std::size_t slot = 0; slot < 5; ++slot)
    {
        tuple[slot] = 1 - tuple[slot];
    }
    expect(!Audit::makeTrial(input, incumbent, actors, {0, 1, 2, 3, 4}, tuple).valid, "no expansion past four actors");
}

void orderingAndIdentity(AssignmentInput & input, AssignmentResult incumbent)
{
    const auto actors = Audit::progress(input, incumbent);
    expect(incumbent.plan.addEdge(actors[4].actionIndex, actors[6].actionIndex, PlanEdgeKind::Coverage),
           "non-Vacate fixture dependency installed");
    incumbent.executionOrder = incumbent.plan.executionOrder().order;
    auto tuple = choices(actors);
    tuple[0] = 1 - tuple[0];
    const auto trial = Audit::makeTrial(input, incumbent, actors, {0}, tuple);
    expect(trial.valid && trial.assignment.plan.edgeCount() == incumbent.plan.edgeCount(),
           "non-Vacate dependencies survive a support trial");
    const auto clean = Audit::planIdentity(incumbent);
    auto changedPhase = incumbent;
    changedPhase.selections.back().phase = AssignmentResult::SelectionPhase::None;
    expect(Audit::planIdentity(changedPhase) != clean, "full identity includes fixed non-stock selection phase");
    auto changedClaim = incumbent;
    expect(changedClaim.plan.claimService(actors.back().actionIndex, 99) == ReservationResult::Granted,
           "service claim fixture installed");
    expect(Audit::planIdentity(changedClaim) != clean, "full identity includes service claims");
    auto changedState = incumbent;
    expect(changedState.plan.commit(actors.back().actionIndex), "committed state fixture installed");
    expect(Audit::planIdentity(changedState) != clean, "full identity includes action state");

    auto stale = incumbent;
    expect(stale.plan.addEdge(actors[0].actionIndex, actors[1].actionIndex, PlanEdgeKind::Vacate),
           "stale Vacate fixture installed");
    const auto fresh = Audit::makeTrial(input, stale, actors, {0}, tuple);
    expect(fresh.valid && fresh.assignment.plan.edgeCount() == incumbent.plan.edgeCount(),
           "derived Vacate edges are rebuilt while non-Vacate edges survive");

    auto colliding = input;
    colliding.actors[0].candidates[tuple[0]].bundle.destination =
        incumbent.plan.action(actors.back().actionIndex).destination;
    colliding.actors[0].candidates[tuple[0]].bundle.path.back() =
        colliding.actors[0].candidates[tuple[0]].bundle.destination;
    const auto collisionActors = Audit::progress(colliding, incumbent);
    expect(!Audit::makeTrial(colliding, incumbent, collisionActors, {0}, tuple).valid,
           "fixed nonmember destination blocks conflicting challenger");
    expect(Audit::planIdentity(incumbent) == clean, "failed claim trial leaves the complete incumbent intact");
}

void requestTuples()
{
    std::vector<std::int32_t> tuple;
    expect(ArenaStockReceipts::readTuple(QJsonArray{0, 18, 20}, tuple) &&
        tuple == std::vector<std::int32_t>{0, 18, 20}, "complete request tuple preserves every candidate index");
    for (const auto & value : {QJsonValue{QJsonArray{-1}}, QJsonValue{QJsonArray{0.5}},
                              QJsonValue{QJsonArray{}}, QJsonValue{QStringLiteral("0,18,20")}})
    {
        tuple.clear();
        expect(!ArenaStockReceipts::readTuple(value, tuple), "malformed request tuple rejected");
    }
    QJsonArray oversized;
    for (std::int32_t index = 0; index < 65; ++index)
    {
        oversized.push_back(0);
    }
    tuple.clear();
    expect(!ArenaStockReceipts::readTuple(oversized, tuple), "oversized full-plan tuple rejected");
}
}

int main()
{
    PropertyStockField field;
    PropertyStockInstance enemy;
    RelativeStockTests::Session session(field, enemy);
    for (std::int32_t actor = 1; actor <= 6; ++actor)
    {
        session.stockActors.insert(actor);
    }
    RelativeStockTests::Valuer valuer(session);
    Audit::SelectionTrace trace;
    auto input = makeInput();
    input.pStockValuer = &valuer;
    input.pTrace = &trace;
    const auto incumbent = MaximumValueAssignment::assign(input);
    expect(incumbent.stats.assignedUnits == 7, "complete seven-actor fixture");
    expect(incumbent.selections.size() == input.actors.size(), "complete selection metadata fixture");
    if (incumbent.selections.size() != input.actors.size())
    {
        return 4;
    }
    MilliFunds economic = 0;
    expect(Audit::replayIncumbent(input, incumbent, economic), "incumbent claims and ordering replay");
    exactSupports(input, incumbent);
    invalidSupports(input, incumbent);
    orderingAndIdentity(input, incumbent);
    requestTuples();
    auto withoutSelections = incumbent;
    withoutSelections.selections.clear();
    expect(Audit::progress(input, withoutSelections).empty(), "missing selection metadata is rejected");
    expect(!Audit::replayIncumbent(input, withoutSelections, economic), "missing metadata cannot pass incumbent replay");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 4;
}
