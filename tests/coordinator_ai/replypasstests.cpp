#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/replypass.h"
#include "tests/coordinator_ai/propertystockdiagnosticfixture.h"

namespace
{
    using namespace Coordinator;
    using Fixture = PropertyStockDiagnosticFixture;

    int checks = 0;
    int failures = 0;

    void expect(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::printf("FAIL: %s\n", message);
        }
    }

    constexpr TilePoint CAPTURE_TILE{0, 0};
    constexpr TilePoint FIRST_HOME{1, 0};
    constexpr TilePoint SECOND_HOME{Fixture::WIDTH - 1, 0};
    constexpr TilePoint SHOOTER_HOME{0, 1};
    constexpr std::int32_t FIRST_UNIT = 100;
    constexpr std::int32_t SECOND_UNIT = 101;
    constexpr std::int32_t ENEMY_UNIT = 999;
    // The shooter is the third unit link, so a candidate can name it as a fire target.
    constexpr std::int32_t SHOOTER = 2;
    constexpr std::int32_t SECOND_SHOOTER = 8;
    constexpr MilliFunds UNIT_COST = 7000;
    constexpr double SIX_STEP_BASE = 60;
    constexpr double THREE_STEP_BASE = 30;
    constexpr double ONE_STEP_BASE = 10;
    constexpr MilliFunds HOPELESS = -100'000;
    const ReplyBoard BOARD{.width = Fixture::WIDTH, .height = 2};

    ReplyShooterFacts shooterAt(const TilePoint & target, std::int32_t index = SHOOTER)
    {
        return ReplyShooterFacts{
            .shooterIndex = index, .tile = {target.x, 1}, .dayStartHpSteps = UNIT_HP_STEPS, .distance = 1,
            .live = true, .cost = UNIT_COST,
        };
    }

    CandidateBundle option(std::int32_t knowledge, TilePoint from, TilePoint to, MilliFunds value,
                           std::vector<ReplyShooterFacts> shooters = {}, double base = SIX_STEP_BASE)
    {
        CandidateBundle result;
        result.bundle.unitId = knowledge;
        result.bundle.origin = from;
        result.bundle.destination = to;
        result.bundle.path = {from};
        if (from != to)
        {
            result.bundle.path.push_back(to);
        }
        result.valuation.valid = true;
        result.valuation.ledger.enemyCapital = value;
        result.actor.hpSteps = UNIT_HP_STEPS;
        result.actor.replacementCost = UNIT_COST;
        auto tile = std::make_shared<ReplyTileFacts>();
        tile->target = to;
        tile->shooters = std::move(shooters);
        auto facts = std::make_shared<ReplyFacts>();
        facts->tile = tile;
        facts->actorRealHp = UNIT_HP_STEPS;
        facts->shots.assign(tile->shooters.size(), ReplyShotBase{.rawBase = base, .withAmmo = base});
        result.replyFacts = facts;
        return result;
    }

    CandidateBundle capturing(CandidateBundle bundle)
    {
        bundle.bundle.components.push_back(captureComponent({.turnsUntilOwned = 2}));
        return bundle;
    }

    struct Scenario
    {
        AssignmentInput input;
        std::vector<std::int32_t> stockRows{0, 1};
        std::vector<MasterActorRole> roles{MasterActorRole::Mutable, MasterActorRole::Mutable};
    };

    Scenario twoUnits(std::vector<CandidateBundle> first, std::vector<CandidateBundle> second)
    {
        Scenario scenario;
        scenario.input.actionIds = {QStringLiteral("ACTION_WAIT"), QStringLiteral("ACTION_FIRE"),
                                    QStringLiteral("ACTION_CAPTURE")};
        scenario.input.unitLinks = {{FIRST_UNIT, FIRST_HOME}, {SECOND_UNIT, SECOND_HOME}, {ENEMY_UNIT, SHOOTER_HOME}};
        scenario.input.actors = {{0, FIRST_UNIT, std::move(first)}, {1, SECOND_UNIT, std::move(second)}};
        return scenario;
    }

    std::optional<MasterAssignmentProblem> extracted(const Scenario & scenario)
    {
        TurnPlan plan;
        std::vector<MasterActorSelection> selections;
        for (std::size_t slot = 0; slot < scenario.input.actors.size(); ++slot)
        {
            const auto & actor = scenario.input.actors[slot];
            const auto & selected = actor.candidates.front();
            const auto index = plan.addAction(
                plannedActionFrom(scenario.input.actionIds, scenario.input.unitLinks, actor.engineUnitId, selected));
            if (index == NO_ACTION || claimFor(plan, index, selected, std::nullopt) != ReservationResult::Granted)
            {
                return std::nullopt;
            }
            selections.push_back({actor.engineUnitId, index, 0, scenario.roles[slot], scenario.stockRows[slot]});
        }
        if (!plan.rebuildVacateEdges(planOccupancy(plan)).empty())
        {
            return std::nullopt;
        }
        const auto order = plan.executionOrder().order;
        auto built = buildMasterAssignmentProblem(scenario.input, {
            .sessionIdentity = 31, .selections = selections, .pIncumbentPlan = &plan, .incumbentExecutionOrder = order,
        });
        return built.valid() ? std::move(built.problem) : std::nullopt;
    }

    struct Run
    {
        std::optional<MasterAssignmentProblem> problem;
        PropertyStockField field;
        ReplyPassResult result;
    };

    Run run(const Scenario & scenario, const ReplyPassLimits & limits = {})
    {
        Run outcome{extracted(scenario), Fixture::make(2), {}};
        if (outcome.problem)
        {
            outcome.result =
                runReplyPass(*outcome.problem, outcome.problem->incumbentChoices(), BOARD, outcome.field, limits);
        }
        return outcome;
    }

    bool verified(const Run & outcome, const ReplyPassResult & result)
    {
        return outcome.problem && verifyReplyPass(*outcome.problem, BOARD, outcome.field, result).verified();
    }

    // Staying home, the capturer is out of the reply's reach.
    Scenario exposedCapturer(MilliFunds stayValue)
    {
        return twoUnits({capturing(option(0, FIRST_HOME, CAPTURE_TILE, 0, {shooterAt(CAPTURE_TILE)})),
                         option(0, FIRST_HOME, FIRST_HOME, stayValue)},
                        {option(1, SECOND_HOME, SECOND_HOME, 0)});
    }

    const ReplyPassChange* changeFor(const ReplyPassResult & result, std::int32_t round, std::int32_t slot,
                                     std::int32_t candidate)
    {
        for (const ReplyPassChange & change : result.changes)
        {
            if (change.round == round && change.actorSlot == slot && change.candidateIndex == candidate)
            {
                return &change;
            }
        }
        return nullptr;
    }

    bool stockUntouched(const ReplyPassResult & result)
    {
        bool idle = !result.stockContextBuilt;
        for (const ReplyPassCost & cost : result.costs)
        {
            idle = idle && cost == ReplyPassCost{};
        }
        return idle;
    }

    void testNoContact()
    {
        const auto outcome = run(twoUnits({option(0, FIRST_HOME, FIRST_HOME, 0),
                                           option(0, FIRST_HOME, CAPTURE_TILE, 0)},
                                          {option(1, SECOND_HOME, SECOND_HOME, 0)}));
        const ReplyPassResult & result = outcome.result;
        expect(outcome.problem.has_value(), "the no-contact fixture extracts");
        expect(result.end == ReplyPassEnd::NoContact && result.incumbentNoValue == ReplyPassNoValue::None &&
                   result.plans.empty() && result.switches.empty() && result.changes.empty(),
               "a plan no reply shot reaches ends at once, reason NoContact");
        expect(stockUntouched(result), "no contact builds no stock context and does no stock work");
        expect(result.finalChoices == std::vector<std::int32_t>{0, 0}, "no contact keeps the caller's choices");
    }

    void testNotChangeable()
    {
        auto scenario = exposedCapturer(0);
        scenario.roles[0] = MasterActorRole::Fixed;
        scenario.input.actors[1].candidates.push_back(option(1, SECOND_HOME, SECOND_HOME, 0));
        const auto outcome = run(scenario);
        const ReplyPassResult & result = outcome.result;
        expect(result.end == ReplyPassEnd::NoEligibleChange && result.counters.unitsShot == 1 &&
                   result.counters.shotFixedRole == 1 && result.counters.shotCommitted == 0 &&
                   result.counters.shotChangeable == 0 && result.counters.listedShot == 0 &&
                   result.counters.listedHitsShooter == 0,
               "a shot unit with a fixed role and no shooter hitter leaves no eligible change, and is counted");
        expect(stockUntouched(result) && result.finalChoices == std::vector<std::int32_t>{0, 0},
               "no eligible change builds no stock context and keeps the choices");

        constexpr std::int32_t hitterCandidate = 2;
        scenario.input.actors[1].candidates.push_back(option(1, SECOND_HOME, SECOND_HOME, 0));
        scenario.input.actors[1].candidates.back().bundle.components.push_back(fireComponent({
            .targetUnitId = SHOOTER, .targetReplacementCost = UNIT_COST, .targetHpSteps = UNIT_HP_STEPS,
            .damageSteps = 1,
        }));
        const auto problem = extracted(scenario);
        expect(problem.has_value(), "the shooter-hitter fixture extracts");
        if (!problem)
        {
            return;
        }
        const auto evaluation = evaluateReplyPassPlan(*problem, problem->incumbentChoices(), BOARD);
        ReplyPassCounters counters;
        const auto changes =
            ReplyPassDetail::listChanges(*problem, problem->incumbentChoices(), evaluation, 0, counters);
        expect(changes.size() == 1 && changes.front().actorSlot == 1 &&
                   changes.front().candidateIndex == hitterCandidate &&
                   changes.front().reason == ChangeReason::HitsShooter && changes.front().replyLoss == 0 &&
                   counters.listedHitsShooter == 1 && counters.listedShot == 0,
               "only the other unit's candidate that fires on the reply's shooter is listed, as HitsShooter");
    }

    void testFloorProof()
    {
        constexpr MilliFunds incumbentUpper = -4188;
        constexpr MilliFunds changedLower = 12;
        constexpr MilliFunds changedUpper = 27;
        constexpr MilliFunds margin = 4200;
        const auto outcome = run(exposedCapturer(0));
        const ReplyPassResult & result = outcome.result;
        expect(result.switches.size() == 1 && result.plans.size() == 2 &&
                   result.finalChoices == std::vector<std::int32_t>{1, 0},
               "one switch moves the shot capturer home");
        if (result.switches.size() != 1)
        {
            return;
        }
        const ReplyPassSwitch & made = result.switches.front();
        expect(made.tier == ReplyPassTier::Floor && made.incumbentUpper == incumbentUpper &&
                   made.changedLower == changedLower && made.margin == margin &&
                   made.proof.interval == ReplyPassInterval{changedLower, changedUpper} &&
                   made.reason == ChangeReason::Shot && made.actorSlot == 0 && made.candidateIndex == 1,
               "the floor at P's prices proves the change with a positive margin");
        expect(result.counters.switchesAtFloor == 1 && result.counters.floorsBuilt == 1 &&
                   result.end == ReplyPassEnd::NoContact,
               "the switched plan has no contact, so the pass ends there");
        expect(verified(outcome, result), "verification accepts the retained certificates");
    }

    void testTieAndOverlap()
    {
        constexpr MilliFunds tie = -4200;
        constexpr MilliFunds overlap = -4201;
        constexpr MilliFunds closedTie = -4206;
        constexpr MilliFunds tieUpper = -4173;
        constexpr MilliFunds overlapLower = -4189;
        constexpr MilliFunds overlapUpper = -4174;
        constexpr MilliFunds incumbentUpper = -4188;
        const ReplyPassLimits noRetune{.retuneChanges = 0};
        const auto tied = run(exposedCapturer(tie), noRetune);
        const ReplyPassChange* tiedChange = changeFor(tied.result, 0, 0, 1);
        expect(tied.result.end == ReplyPassEnd::NoProvenChange && tied.result.switches.empty() &&
                   tiedChange != nullptr && tiedChange->outcome == ReplyPassOutcome::Undecided &&
                   tiedChange->lower == incumbentUpper && tiedChange->upper == tieUpper,
               "lower(X) equal to upper(P) keeps P");
        const auto overlapped = run(exposedCapturer(overlap), noRetune);
        const ReplyPassChange* overlapChange = changeFor(overlapped.result, 0, 0, 1);
        expect(overlapped.result.switches.empty() && overlapChange != nullptr &&
                   overlapChange->outcome == ReplyPassOutcome::Undecided && overlapChange->lower == overlapLower &&
                   overlapChange->upper == overlapUpper,
               "an interval overlapping upper(P) keeps P");
        const auto closed = run(exposedCapturer(closedTie));
        const ReplyPassChange* closedChange = changeFor(closed.result, 0, 0, 1);
        expect(closed.result.switches.empty() && closedChange != nullptr &&
                   closedChange->tier == ReplyPassTier::Retune &&
                   closedChange->outcome == ReplyPassOutcome::Undecided && closedChange->lower == incumbentUpper &&
                   closedChange->upper == incumbentUpper && closed.result.counters.retunes == 1,
               "a re-tune that closes X exactly on upper(P) still keeps P");
    }

    Scenario damagedAtHome(MilliFunds captureValue)
    {
        return twoUnits({option(0, FIRST_HOME, FIRST_HOME, 0, {shooterAt(FIRST_HOME)}, ONE_STEP_BASE),
                         capturing(option(0, FIRST_HOME, CAPTURE_TILE, captureValue))},
                        {option(1, SECOND_HOME, SECOND_HOME, 0)});
    }

    void testCeilingOutcomes()
    {
        constexpr MilliFunds atLower = -718;
        constexpr MilliFunds atUpper = -712;
        constexpr ReplyPassInterval incumbent{-686, -680};
        const ReplyPassLimits oneRound{.warmupRounds = 1, .baselineRounds = 1, .retuneChanges = 0};
        const auto proven = run(damagedAtHome(atLower), oneRound);
        const ReplyPassChange* provenChange = changeFor(proven.result, 0, 0, 1);
        expect(!proven.result.plans.empty() && proven.result.plans.front().baseline.interval == incumbent &&
                   provenChange != nullptr && provenChange->outcome == ReplyPassOutcome::ProvenNotBetter &&
                   provenChange->tier == ReplyPassTier::Ceiling && provenChange->upper == incumbent.lower &&
                   !provenChange->lower && proven.result.counters.provenNotBetter == 1 &&
                   proven.result.counters.floorsBuilt == 0,
               "upper(X) equal to lower(P) is proven not better without a floor");
        const auto cleared = run(damagedAtHome(atUpper), oneRound);
        const ReplyPassChange* clearChange = changeFor(cleared.result, 0, 0, 1);
        expect(clearChange != nullptr && clearChange->outcome == ReplyPassOutcome::CannotClear &&
                   clearChange->tier == ReplyPassTier::Ceiling && clearChange->upper == incumbent.upper &&
                   !clearChange->lower && cleared.result.counters.cannotClear == 1 &&
                   cleared.result.counters.floorsBuilt == 0 && cleared.result.end == ReplyPassEnd::NoProvenChange,
               "upper(X) inside P's interval cannot clear it, and no floor is built");
    }

    void testNoValueChange()
    {
        auto scenario = exposedCapturer(0);
        auto unfacted = option(0, FIRST_HOME, FIRST_HOME, -HOPELESS);
        unfacted.replyFacts.reset();
        auto & candidates = scenario.input.actors[0].candidates;
        candidates.insert(candidates.begin() + 1, unfacted);
        const auto outcome = run(scenario);
        const ReplyPassResult & result = outcome.result;
        const ReplyPassChange* refused = changeFor(result, 0, 0, 1);
        constexpr auto missingFacts = static_cast<std::size_t>(ReplyPassNoValue::MissingFacts);
        expect(refused != nullptr && refused->outcome == ReplyPassOutcome::NoValue &&
                   refused->noValue == ReplyPassNoValue::MissingFacts && refused->tier == ReplyPassTier::Value &&
                   !refused->upper && !refused->lower && result.counters.noValue[missingFacts] == 1,
               "a change without reply facts has no value, whatever its acting value, and is counted under its reason");
        expect(result.switches.size() == 1 && result.switches.front().candidateIndex == 2 &&
                   result.finalChoices == std::vector<std::int32_t>{2, 0},
               "the pass switches to the next valued change, never to the one without a value");
    }

    void testRetuneProof()
    {
        constexpr MilliFunds stayValue = -4203;
        constexpr MilliFunds floorLower = -4191;
        constexpr MilliFunds tunedLower = -4185;
        constexpr MilliFunds margin = 3;
        const auto outcome = run(exposedCapturer(stayValue));
        const ReplyPassResult & result = outcome.result;
        const ReplyPassChange* change = changeFor(result, 0, 0, 1);
        expect(result.switches.size() == 1 && change != nullptr && change->outcome == ReplyPassOutcome::Switched &&
                   change->tier == ReplyPassTier::Retune && result.counters.undecided == 1 &&
                   result.counters.retunes == 1 && result.counters.switchesAtRetune == 1,
               "an Undecided change is proven by its re-tune");
        if (!result.switches.empty())
        {
            const ReplyPassSwitch & made = result.switches.front();
            expect(made.tier == ReplyPassTier::Retune && made.changedLower == tunedLower && made.margin == margin &&
                       made.proof.interval == ReplyPassInterval{tunedLower, tunedLower},
                   "the re-tune closes X above upper(P) by the margin");
        }
        expect(verified(outcome, result), "a re-tune proof verifies from its own retained prices and witness");
        const auto kept = run(exposedCapturer(stayValue), ReplyPassLimits{.retuneChanges = 0});
        const ReplyPassChange* open = changeFor(kept.result, 0, 0, 1);
        expect(kept.result.switches.empty() && kept.result.end == ReplyPassEnd::NoProvenChange && open != nullptr &&
                   open->outcome == ReplyPassOutcome::Undecided && open->lower == floorLower &&
                   kept.result.counters.retunes == 0 && kept.result.finalChoices == std::vector<std::int32_t>{0, 0},
               "with no re-tunes allowed the same change keeps P");
    }

    // One shooter reaches both units and picks the lower unit id, so the second is shot only once the first leaves.
    Scenario sharedShooter()
    {
        return twoUnits({capturing(option(0, FIRST_HOME, CAPTURE_TILE, 0, {shooterAt(CAPTURE_TILE)})),
                         option(0, FIRST_HOME, FIRST_HOME, 0)},
                        {option(1, SECOND_HOME, SECOND_HOME, 0, {shooterAt(SECOND_HOME)}),
                         capturing(option(1, SECOND_HOME, SECOND_HOME, 0))});
    }

    void testTwoSwitches()
    {
        constexpr MilliFunds firstMargin = 3;
        constexpr MilliFunds secondMargin = 4200;
        const auto outcome = run(sharedShooter());
        const ReplyPassResult & result = outcome.result;
        expect(result.switches.size() == 2 && result.finalChoices == std::vector<std::int32_t>{1, 1} &&
                   result.end == ReplyPassEnd::NoContact,
               "two switches in one call");
        if (result.switches.size() == 2)
        {
            expect(result.switches[0].actorSlot == 0 && result.switches[0].margin == firstMargin &&
                       result.switches[1].actorSlot == 1 && result.switches[1].margin == secondMargin,
                   "the first switch moves the first unit, the second the unit the reply then shoots");
        }
        expect(!changeFor(result, 0, 1, 1) && changeFor(result, 1, 1, 1) != nullptr && result.counters.unitsShot == 2,
               "the second unit's change is listed only from the first switched plan");
        expect(verified(outcome, result), "both switches verify");
        const auto limited = run(sharedShooter(), ReplyPassLimits{.switchLimit = 1});
        expect(limited.result.end == ReplyPassEnd::SwitchLimit && limited.result.switches.size() == 1 &&
                   limited.result.counters.endedByLimit &&
                   limited.result.finalChoices == std::vector<std::int32_t>{1, 0} && verified(limited, limited.result),
               "a switch limit of one stops after the first switch");
    }

    void testChangeOrder()
    {
        constexpr MilliFunds sixSteps = 4200;
        constexpr MilliFunds threeSteps = 2100;
        // Slot 1 holds the higher engine id and the larger loss, so slot order and candidate order both disagree.
        const auto outcome = run(twoUnits(
            {option(0, FIRST_HOME, FIRST_HOME, 0, {shooterAt(FIRST_HOME, SECOND_SHOOTER)}, THREE_STEP_BASE),
             option(0, FIRST_HOME, FIRST_HOME, HOPELESS)},
            {option(1, SECOND_HOME, SECOND_HOME, 0, {shooterAt(SECOND_HOME)}),
             option(1, SECOND_HOME, SECOND_HOME, HOPELESS), option(1, SECOND_HOME, SECOND_HOME, HOPELESS)}));
        const auto & changes = outcome.result.changes;
        const bool ordered = changes.size() == 3 && changes[0].actorSlot == 1 && changes[0].candidateIndex == 1 &&
                             changes[0].replyLoss == sixSteps && changes[1].actorSlot == 1 &&
                             changes[1].candidateIndex == 2 && changes[2].actorSlot == 0 &&
                             changes[2].candidateIndex == 1 && changes[2].replyLoss == threeSteps;
        bool tried = ordered;
        for (const ReplyPassChange & change : changes)
        {
            tried = tried && change.outcome == ReplyPassOutcome::ProvenNotBetter;
        }
        expect(ordered && tried, "changes are tried by reply loss, then engine unit id, then candidate index");
        const ReplyPassChangeOrder order;
        const ReplyPassChange bigLoss{.engineUnitId = SECOND_UNIT, .candidateIndex = 2, .replyLoss = sixSteps};
        const ReplyPassChange lowId{.engineUnitId = FIRST_UNIT, .candidateIndex = 2, .replyLoss = threeSteps};
        const ReplyPassChange lowCandidate{.engineUnitId = SECOND_UNIT, .candidateIndex = 1, .replyLoss = threeSteps};
        const ReplyPassChange highCandidate{.engineUnitId = SECOND_UNIT, .candidateIndex = 2, .replyLoss = threeSteps};
        expect(order(bigLoss, lowId) && order(lowId, lowCandidate) && order(lowCandidate, highCandidate) &&
                   !order(highCandidate, lowCandidate) && !order(lowCandidate, lowCandidate),
               "the change order compares loss descending, then engine id, then candidate index");
    }

    ReplyPassResult withoutNanos(ReplyPassResult result)
    {
        for (ReplyPassCost & cost : result.costs)
        {
            cost.nanos = 0;
        }
        return result;
    }

    void testDeterminism()
    {
        auto scenario = sharedShooter();
        const auto tally = std::make_shared<ReplyPredictionTally>();
        const auto board = std::make_shared<const ReplyBoard>(BOARD);
        scenario.input.replyPrediction = {.mode = ReplyPrediction::Recorded, .board = board, .tally = tally};
        const auto problem = extracted(scenario);
        expect(problem.has_value(), "the recorded determinism fixture extracts");
        if (!problem)
        {
            return;
        }
        const auto field = Fixture::make(2);
        const auto fieldBefore = Fixture::snapshot(field);
        const MasterAssignmentProblem copy = *problem;
        const std::vector<std::int32_t> choices(problem->incumbentChoices().begin(),
                                                problem->incumbentChoices().end());
        const ReplyPredictionTally counted = *tally;
        const auto first = runReplyPass(*problem, choices, BOARD, field);
        const auto second = runReplyPass(*problem, choices, BOARD, field);
        expect(first.switches.size() == 2 && withoutNanos(first) == withoutNanos(second),
               "two calls return equal results apart from nanos");
        expect(choices == std::vector<std::int32_t>{0, 0} &&
                   std::equal(copy.incumbentChoices().begin(), copy.incumbentChoices().end(),
                              problem->incumbentChoices().begin(), problem->incumbentChoices().end()) &&
                   Fixture::unchanged(field, fieldBefore),
               "the caller's choices, problem and field are unchanged");
        expect(tally->replays == counted.replays && tally->incomplete == counted.incomplete &&
                   tally->pairs == counted.pairs && tally->nanos == counted.nanos,
               "a recorded problem's shared reply tally is unchanged");
    }

    ReplyPassMismatch tampered(const Run & outcome, void (*tamper)(ReplyPassResult &))
    {
        ReplyPassResult changed = outcome.result;
        tamper(changed);
        return outcome.problem ? verifyReplyPass(*outcome.problem, BOARD, outcome.field, changed).mismatch
                               : ReplyPassMismatch::NotVerified;
    }

    void testVerificationRejectsTampering()
    {
        const auto outcome = run(exposedCapturer(0));
        if (outcome.result.switches.size() != 1 || outcome.result.plans.size() != 2 ||
            !verified(outcome, outcome.result))
        {
            expect(false, "the tampering fixture switches once and verifies untouched");
            return;
        }
        expect(tampered(outcome, [](ReplyPassResult & result) { ++result.switches[0].proof.prices[1]; }) ==
                   ReplyPassMismatch::Ceiling,
               "a changed price is rejected");
        expect(tampered(outcome, [](ReplyPassResult & result)
                        { ++result.switches[0].proof.witness[0].steps[0].turn; }) == ReplyPassMismatch::Witness,
               "a changed witness step is rejected");
        expect(tampered(outcome, [](ReplyPassResult & result) { result.plans[1].choices[0] = 0; }) ==
                   ReplyPassMismatch::SwitchLinks,
               "a changed choice is rejected");
        expect(tampered(outcome, [](ReplyPassResult & result) { result.switches[0].from = 1; }) ==
                   ReplyPassMismatch::SwitchLinks,
               "a switch naming the wrong plans is rejected");
        expect(tampered(outcome, [](ReplyPassResult & result) { result.plans[1].choices[1] = 1; }) ==
                   ReplyPassMismatch::SwitchLinks,
               "a switched plan differing in a second slot is rejected");
        expect(tampered(outcome, [](ReplyPassResult & result) { result.switches[0].candidateIndex = 0; }) ==
                   ReplyPassMismatch::SwitchLinks,
               "a switch whose candidate is not the switched plan's choice is rejected");
        expect(tampered(outcome, [](ReplyPassResult & result) { result.plans[0].baseline.present = false; }) ==
                   ReplyPassMismatch::MissingEvidence,
               "a switch from a plan without its baseline is rejected");
        expect(tampered(outcome, [](ReplyPassResult & result)
                        { std::swap(result.plans[0].executionOrder[0], result.plans[0].executionOrder[1]); }) ==
                   ReplyPassMismatch::ExecutionOrder,
               "a changed execution order is rejected");
        expect(tampered(outcome, [](ReplyPassResult & result)
                        { result.plans[0].afterReply[1] = CanonicalPlanActionKey::fromAction(1, 0, 0, false); }) ==
                   ReplyPassMismatch::Keys,
               "a changed key row is rejected");
        expect(tampered(outcome, [](ReplyPassResult & result)
                        { ++result.plans[0].actors[0].terms.replyCapitalLoss; }) ==
                   ReplyPassMismatch::Terms,
               "a changed exact term is rejected");
        expect(tampered(outcome, [](ReplyPassResult & result) { result.switches[0].margin = 0; }) ==
                   ReplyPassMismatch::Margin,
               "a margin made nonpositive is rejected");
    }

    void testMissingBaseline()
    {
        constexpr MilliFunds notBetter = -4215;
        const auto outcome = run(exposedCapturer(notBetter));
        expect(outcome.result.end == ReplyPassEnd::NoProvenChange && outcome.result.plans.size() == 1 &&
                   outcome.result.switches.empty() && verified(outcome, outcome.result),
               "a round without a switch retains its one plan with a verified baseline");
        expect(tampered(outcome, [](ReplyPassResult & result) { result.plans[0].baseline.present = false; }) ==
                   ReplyPassMismatch::MissingEvidence,
               "the first plan without its baseline is rejected");
    }

    // The capture change ranks first on ceiling gain and fails its re-tune; the stay change is re-tuned from p.
    Scenario twoUndecided()
    {
        constexpr MilliFunds stayValue = -4205;
        constexpr MilliFunds captureValue = -4209;
        auto scenario = exposedCapturer(stayValue);
        scenario.input.actors[0].candidates.push_back(capturing(option(0, FIRST_HOME, CAPTURE_TILE, captureValue)));
        return scenario;
    }

    void testRetuneRankingAndReinstall()
    {
        constexpr MilliFunds stayFloorLower = -4193;
        constexpr MilliFunds stayFloorUpper = -4178;
        constexpr ReplyPassInterval captureTuned{-4191, -4189};
        constexpr MilliFunds stayTuned = -4187;
        const std::vector<MilliFunds> incumbentPrices{0, 0};
        const std::vector<MilliFunds> stayPrices{9, 4};
        const auto both = run(twoUndecided());
        const ReplyPassChange* stay = changeFor(both.result, 0, 0, 1);
        const ReplyPassChange* capture = changeFor(both.result, 0, 0, 2);
        expect(capture != nullptr && capture->tier == ReplyPassTier::Retune &&
                   capture->outcome == ReplyPassOutcome::Undecided && capture->lower == captureTuned.lower &&
                   capture->upper == captureTuned.upper && both.result.counters.retunes == 2,
               "the larger ceiling gain is re-tuned first and stays Undecided");
        expect(stay != nullptr && stay->outcome == ReplyPassOutcome::Switched && stay->tier == ReplyPassTier::Retune &&
                   stay->lower == stayTuned && stay->upper == stayTuned && both.result.switches.size() == 1 &&
                   both.result.plans.front().baseline.prices == incumbentPrices &&
                   both.result.switches.front().proof.prices == stayPrices && verified(both, both.result),
               "after the failed re-tune the next one starts again from p and reaches the prices it proves at");
        const auto one = run(twoUndecided(), ReplyPassLimits{.retuneChanges = 1});
        const ReplyPassChange* kept = changeFor(one.result, 0, 0, 1);
        const ReplyPassChange* tuned = changeFor(one.result, 0, 0, 2);
        expect(one.result.end == ReplyPassEnd::NoProvenChange && kept != nullptr &&
                   kept->tier == ReplyPassTier::Floor && kept->lower == stayFloorLower &&
                   kept->upper == stayFloorUpper && tuned != nullptr && tuned->tier == ReplyPassTier::Retune &&
                   one.result.counters.retunes == 1,
               "with one re-tune allowed only the change with the larger ceiling gain is re-tuned");
    }

    void testInvalidLimits()
    {
        constexpr MilliFunds refusedPpm = REPLY_STOCK_PARTS_PER_MILLION + 1;
        const std::array invalid{
            ReplyPassLimits{.warmupRounds = 0}, ReplyPassLimits{.baselineRounds = 0},
            ReplyPassLimits{.retuneRounds = 0}, ReplyPassLimits{.retuneChanges = -1},
            ReplyPassLimits{.switchLimit = 0}, ReplyPassLimits{.targetGapPpm = refusedPpm},
            ReplyPassLimits{.targetGapPpm = -1},
        };
        bool refused = true;
        for (const ReplyPassLimits & limits : invalid)
        {
            const auto outcome = run(exposedCapturer(0), limits);
            refused = refused && outcome.problem && outcome.result.end == ReplyPassEnd::InvalidLimits &&
                      outcome.result.plans.empty() && outcome.result.changes.empty() &&
                      stockUntouched(outcome.result) && outcome.result.finalChoices == std::vector<std::int32_t>{0, 0};
        }
        expect(refused, "every invalid limit ends the pass before any work, keeping the caller's choices");
    }

    void testIntervalSumRefused()
    {
        const auto outcome = run(exposedCapturer(std::numeric_limits<MilliFunds>::max()));
        const ReplyPassChange* change = changeFor(outcome.result, 0, 0, 1);
        expect(change != nullptr && change->noValue == ReplyPassNoValue::None &&
                   change->outcome == ReplyPassOutcome::NoInterval && change->tier == ReplyPassTier::Ceiling &&
                   change->stockStatus == ReplyStockStatus::ArithmeticFailure && !change->upper && !change->lower &&
                   outcome.result.counters.noInterval == 1 && outcome.result.switches.empty(),
               "a valued change whose interval sum overflows has no interval and is never switched to");
    }

    void testDefaultsClaimNothing()
    {
        const ReplyPassResult result;
        const ReplyPassChange change;
        const ReplyPassSwitch made;
        const ReplyPassVerification verification;
        const ReplyPassStockEvidence evidence;
        expect(result.end == ReplyPassEnd::NotRun && result.incumbentNoValue == ReplyPassNoValue::NotEvaluated &&
                   result.plans.empty() && result.switches.empty() && result.changes.empty() &&
                   !result.stockContextBuilt && result.counters == ReplyPassCounters{},
               "a default result claims no run, no plan and no switch");
        expect(change.outcome == ReplyPassOutcome::NotTried && change.tier == ReplyPassTier::None &&
                   change.noValue == ReplyPassNoValue::NotEvaluated && !change.upper && !change.lower,
               "a default change claims no outcome and no interval");
        expect(made.tier == ReplyPassTier::None && made.margin == 0 && !made.proof.present && !evidence.present &&
                   !verification.verified(),
               "a default switch, evidence and verification prove nothing");
        const auto field = Fixture::make(2);
        const auto problem = extracted(exposedCapturer(0));
        expect(problem && !verifyReplyPass(*problem, BOARD, field, result).verified(),
               "verification refuses a result that retains nothing");
    }
}

int main()
{
    testNoContact();
    testNotChangeable();
    testFloorProof();
    testTieAndOverlap();
    testCeilingOutcomes();
    testNoValueChange();
    testRetuneProof();
    testTwoSwitches();
    testChangeOrder();
    testDeterminism();
    testVerificationRejectsTampering();
    testMissingBaseline();
    testRetuneRankingAndReinstall();
    testInvalidLimits();
    testIntervalSumRefused();
    testDefaultsClaimNothing();
    std::printf("Reply pass: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
