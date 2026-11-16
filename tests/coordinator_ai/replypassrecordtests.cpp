#include <cstdio>
#include <vector>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "ai/coordinator/replypassrecord.h"
#include "tests/coordinator_ai/masterassignmenttestfixture.h"
#include "tests/coordinator_ai/propertystockdiagnosticfixture.h"

namespace
{
    using namespace Coordinator;
    using MasterAssignmentTest::expect;

    constexpr std::int32_t DAY = 4;
    constexpr std::int32_t PLAYER = 1;
    constexpr std::uint64_t PLAN_SEQUENCE = 12;
    const DecisionTraceIdentity IDENTITY{.day = DAY, .playerId = PLAYER, .planSequence = PLAN_SEQUENCE};
    const ReplyBoard BOARD{.width = PropertyStockDiagnosticFixture::WIDTH, .height = 2};

    bool sameWiring(const ReplyPassWiring & wiring, bool replyFacts, bool handoff, bool record)
    {
        return wiring.replyFacts == replyFacts && wiring.handoff == handoff && wiring.record == record;
    }

    void testWiring()
    {
        for (const bool local : {false, true})
        {
            expect(sameWiring(replyPassWiring(ReplyPrediction::Off, ReplyPassMode::Off, local), false, false, false),
                   "with both switches off nothing is built, requested or written");
            expect(sameWiring(replyPassWiring(ReplyPrediction::Recorded, ReplyPassMode::Off, local), local, false,
                              false),
                   "the prediction alone builds reply facts in a local session but asks for no handoff or record");
        }
        expect(sameWiring(replyPassWiring(ReplyPrediction::Off, ReplyPassMode::Recorded, true), true, true, true),
               "the pass alone builds reply facts, asks for the handoff and writes a record");
        expect(sameWiring(replyPassWiring(ReplyPrediction::Off, ReplyPassMode::Recorded, false), false, false, true),
               "outside a local session the pass builds nothing but still records why it did not run");
    }

    QJsonObject skipped(bool local, bool legacy, MasterPlanHandoffStatus status, const ReplyBoard* pBoard)
    {
        const auto field = PropertyStockDiagnosticFixture::make();
        const ReplyPassRun run = recordedReplyPass(local, legacy, MasterPlanHandoffResult{.status = status}, pBoard,
                                                   field);
        expect(!run.result && !run.verification && run.planChoices.empty() && run.passNanos == 0,
               "a pass that cannot run computes nothing");
        return replyPassRecord(IDENTITY, run);
    }

    bool namesSkip(const QJsonObject & record, const char* reason, const char* handoff)
    {
        return record.value("schema").toInt() == 1 && record.value("day").toInt() == DAY &&
               record.value("player").toInt() == PLAYER &&
               record.value("planSequence").toInteger() == static_cast<qint64>(PLAN_SEQUENCE) &&
               record.value("mode").toString() == "recorded" && !record.value("ran").toBool(true) &&
               record.value("reason").toString() == reason && record.value("handoff").toString() == handoff &&
               record.value("planChoices").toArray().isEmpty() && !record.contains("end") &&
               record.value("verification").toObject() ==
                   QJsonObject{{"ran", false}, {"passed", false}, {"mismatch", QJsonValue()}, {"index", QJsonValue()}};
    }

    void testSkippedRecords()
    {
        expect(namesSkip(skipped(false, false, MasterPlanHandoffStatus::NotRequested, nullptr), "NOT_LOCAL_SESSION",
                         "NOT_REQUESTED"),
               "a network or replay session is named");
        expect(namesSkip(skipped(true, false, MasterPlanHandoffStatus::Ready, nullptr), "NO_REPLY_BOARD", "READY"),
               "a missing reply board is named");
        expect(namesSkip(skipped(true, true, MasterPlanHandoffStatus::Ready, &BOARD), "LEGACY_PLAN", "READY"),
               "a plan from the ordinary planner is named even when control had handed one off");
        const std::vector<std::pair<MasterPlanHandoffStatus, const char*>> refusals{
            {MasterPlanHandoffStatus::NotRequested, "NOT_REQUESTED"},
            {MasterPlanHandoffStatus::NoProblem, "NO_PROBLEM"},
            {MasterPlanHandoffStatus::SelectionMismatch, "SELECTION_MISMATCH"},
            {MasterPlanHandoffStatus::ReplayFailed, "REPLAY_FAILED"},
            {MasterPlanHandoffStatus::PlanMismatch, "PLAN_MISMATCH"},
        };
        for (const auto & [status, name] : refusals)
        {
            expect(namesSkip(skipped(true, false, status, &BOARD), "NO_HANDOFF", name),
                   "each handoff refusal is named");
        }
    }

    void testNoPlanSkipsVerification()
    {
        MasterAssignmentTest::Fixture fixture(2);
        fixture.seat();
        const auto problem = fixture.extract();
        const std::vector<std::int32_t> choices(problem.incumbentChoices().begin(), problem.incumbentChoices().end());
        const MasterPlanHandoffResult handoff{MasterPlanHandoffStatus::Ready, MasterPlanHandoff{problem, choices}};
        const auto field = PropertyStockDiagnosticFixture::make();
        const ReplyPassRun run = recordedReplyPass(true, false, handoff, &BOARD, field);
        expect(run.result && run.result->end == ReplyPassEnd::IncumbentNoValue && run.result->plans.empty() &&
                   !run.verification && !run.planInEffectShots && run.planChoices == choices,
               "a plan without reply facts has no value, so nothing is retained and nothing is verified");
        const QJsonObject record = replyPassRecord(IDENTITY, run);
        const QJsonObject verification = record.value("verification").toObject();
        expect(record.value("ran").toBool() && record.value("reason").toString() == "NONE" &&
                   record.value("end").toString() == "INCUMBENT_NO_VALUE" &&
                   !verification.value("ran").toBool(true) && !verification.value("passed").toBool(true) &&
                   verification.value("mismatch").isNull() && record.value("shotUnits").isNull() &&
                   !record.value("finalDiffers").toBool(true),
               "the record says the pass ran, retained nothing and did not verify");
    }

    constexpr std::int64_t NO_VALUE_BASE = 110;
    constexpr std::int64_t COST_BASE = 200;
    constexpr std::int64_t COST_STEP = 10;

    // Every serialised field holds a distinct value, so a renamed, dropped or swapped field changes the text.
    ReplyPassRun handBuiltRun()
    {
        ReplyPassResult result;
        result.end = ReplyPassEnd::NoProvenChange;
        result.incumbentNoValue = ReplyPassNoValue::KeyRejected;
        result.baselineStatus = ReplyStockStatus::LimitReached;
        result.stockContextBuilt = true;
        result.finalChoices = {2, 1};
        ReplyPassCounters & counters = result.counters;
        counters = {.unitsShot = 101, .shotChangeable = 102, .shotFixedRole = 103, .shotCommitted = 104,
                    .listedShot = 105, .listedHitsShooter = 106, .noInterval = 131, .provenNotBetter = 132,
                    .cannotClear = 133, .floorsBuilt = 134, .undecided = 135, .retunes = 136, .switchesAtFloor = 137,
                    .switchesAtRetune = 138, .endedByLimit = true};
        for (std::size_t reason = 0; reason < counters.noValue.size(); ++reason)
        {
            counters.noValue[reason] = NO_VALUE_BASE + static_cast<std::int64_t>(reason);
        }
        for (std::size_t stage = 0; stage < result.costs.size(); ++stage)
        {
            const auto base = COST_BASE + COST_STEP * static_cast<std::int64_t>(stage);
            result.costs[stage] = {.nanos = base, .work = base + 1, .calls = base + 2};
        }
        const auto ownKey = CanonicalPlanActionKey::checkedFromAction(3, 4, 5, true, 2, false);
        const auto replyKey = CanonicalPlanActionKey::checkedFromAction(6, 7, 8, false, 9, true);
        MasterAssignmentTest::require(ownKey && replyKey, "the record fixture keys encode");
        ReplyPassPlan plan;
        plan.choices = {0, 2};
        plan.executionOrder = {1, 0};
        plan.afterOwnActions = {*ownKey};
        plan.afterReply = {*replyKey};
        plan.exactNonStock = -401;
        plan.owned = 7'000'000'402;
        plan.baseline = {
            .present = true, .prices = {501, 502, 503}, .ceiling = 504, .floor = 505,
            .interval = {.lower = 506, .upper = 507},
        };
        plan.actors.push_back(ReplyPassActor{
            .actorSlot = 1, .engineUnitId = 301, .actionIndex = 302, .candidateIndex = 303, .grantedDamage = 304,
            .dayStartSteps = 305, .stepsAfterOwnActions = 306, .stepsAfterReply = 307,
            .destroyedBy = StockDeathCause::Reply, .stockRow = 308,
            .terms = {.actingSeated = 311, .ownCounterCorrection = -312, .replyCapitalLoss = 313,
                      .replyCounterCapital = 314, .nextShotAfterReply = 315},
        });
        result.plans = {plan};
        result.switches.push_back(ReplyPassSwitch{
            .from = 701, .to = 702, .actorSlot = 703, .candidateIndex = 704, .reason = ChangeReason::HitsShooter,
            .tier = ReplyPassTier::Retune, .incumbentUpper = 705, .changedLower = 706, .margin = 707,
        });
        result.changes.push_back(ReplyPassChange{
            .round = 801, .actorSlot = 802, .engineUnitId = 803, .candidateIndex = 804, .reason = ChangeReason::Shot,
            .replyLoss = 805, .tier = ReplyPassTier::Ceiling, .outcome = ReplyPassOutcome::Undecided,
            .noValue = ReplyPassNoValue::MissingFacts, .stockStatus = ReplyStockStatus::Inverted, .upper = 807,
            .lower = 806,
        });
        ReplyPassRun run;
        run.handoff = MasterPlanHandoffStatus::Ready;
        run.planChoices = {0, 2};
        run.result = result;
        run.planInEffectShots = ReplyPassShotUnits{.shot = 601, .changeable = 602, .fixedRole = 603, .committed = 604};
        run.acceptedPlanShots = {
            ReplyPassShotUnits{.shot = 611, .changeable = 612, .fixedRole = 613, .committed = 614},
        };
        run.verification = ReplyPassVerification{ReplyPassMismatch::Margin, 901};
        run.passNanos = 5001;
        run.shotNanos = 5002;
        run.verificationNanos = 5003;
        return run;
    }

    constexpr auto EXPECTED_RECORD = R"({
        "schema": 1, "day": 4, "player": 1, "planSequence": 12, "mode": "recorded", "ran": true, "reason": "NONE",
        "handoff": "READY", "passNanos": 5001, "shotNanos": 5002, "verificationNanos": 5003, "planChoices": [0, 2],
        "end": "NO_PROVEN_CHANGE", "incumbentNoValue": "KEY_REJECTED", "baselineStatus": "LIMIT_REACHED",
        "stockContextBuilt": true,
        "shotUnits": {"shot": 601, "changeable": 602, "notChangeable": 1207, "fixedRole": 603, "committed": 604},
        "counters": {
            "unitsShot": 101, "shotChangeable": 102, "shotNotChangeable": 207, "shotFixedRole": 103,
            "shotCommitted": 104, "listedShot": 105, "listedHitsShooter": 106,
            "noValue": {"NONE": 110, "REPLAY_INVALID": 111, "REPLAY_INCOMPLETE": 112, "UNOWNED_ACTION": 113,
                        "MISSING_FACTS": 114, "UNSUPPORTED_ACTION": 115, "SELF_SERVICE": 116,
                        "TARGET_ALREADY_GONE": 117, "UNUSABLE_PLAN_CHAIN": 118, "MISSING_NEXT_SHOT_BASIS": 119,
                        "KEY_REJECTED": 120, "ARITHMETIC": 121, "NOT_EVALUATED": 122},
            "noInterval": 131, "provenNotBetter": 132, "cannotClear": 133, "floorsBuilt": 134, "undecided": 135,
            "retunes": 136, "switchesAtFloor": 137, "switchesAtRetune": 138, "endedByLimit": true
        },
        "costs": {
            "BASELINE": {"nanos": 200, "work": 201, "calls": 202},
            "T1_VALUE": {"nanos": 210, "work": 211, "calls": 212},
            "T2_CEILING": {"nanos": 220, "work": 221, "calls": 222},
            "T3_FLOOR": {"nanos": 230, "work": 231, "calls": 232},
            "T4_RETUNE": {"nanos": 240, "work": 241, "calls": 242}
        },
        "plans": [{
            "choices": [0, 2], "executionOrder": [1, 0],
            "afterOwnActions": [[3, 4, 5, true, 2, false]], "afterReply": [[6, 7, 8, false, 9, true]],
            "actors": [{
                "actor": 1, "unit": 301, "action": 302, "candidate": 303, "grantedDamage": 304,
                "dayStartSteps": 305, "stepsAfterOwnActions": 306, "stepsAfterReply": 307, "destroyedBy": "REPLY",
                "stockRow": 308, "actingSeated": 311, "ownCounterCorrection": -312, "replyCapitalLoss": 313,
                "replyCounterCapital": 314, "nextShotAfterReply": 315
            }],
            "exactNonStock": -401, "owned": 7000000402,
            "shotUnits": {"shot": 611, "changeable": 612, "notChangeable": 1227, "fixedRole": 613, "committed": 614},
            "baseline": {"present": true, "floor": 505, "ceiling": 504, "lower": 506, "upper": 507, "priceCount": 3,
                         "prices": [501, 502, 503]}
        }],
        "switches": [{
            "from": 701, "to": 702, "actor": 703, "candidate": 704, "reason": "HITS_SHOOTER", "tier": "T4_RETUNE",
            "incumbentUpper": 705, "changedLower": 706, "margin": 707
        }],
        "changes": [{
            "round": 801, "actor": 802, "unit": 803, "candidate": 804, "reason": "SHOT", "replyLoss": 805,
            "tier": "T2_CEILING", "outcome": "UNDECIDED", "noValue": "MISSING_FACTS", "stockStatus": "INVERTED",
            "lower": 806, "upper": 807
        }],
        "verification": {"ran": true, "passed": false, "mismatch": "MARGIN", "index": 901},
        "finalChoices": [2, 1], "finalDiffers": true
    })";

    void testHandBuiltRecord()
    {
        const QByteArray actual =
            QJsonDocument(replyPassRecord(IDENTITY, handBuiltRun())).toJson(QJsonDocument::Compact);
        const QByteArray expected = QJsonDocument::fromJson(EXPECTED_RECORD).toJson(QJsonDocument::Compact);
        expect(!expected.isEmpty() && expected != "{}", "the expected record literal parses");
        expect(actual == expected, "the record matches the complete literal object, field by field");
        if (actual != expected)
        {
            std::printf("actual:   %s\nexpected: %s\n", actual.constData(), expected.constData());
        }
    }}

int main()
{
    testWiring();
    testSkippedRecords();
    testNoPlanSkipsVerification();
    testHandBuiltRecord();
    std::printf("Reply pass record: %d checks, %d failures\n", MasterAssignmentTest::checks,
                MasterAssignmentTest::failures);
    return MasterAssignmentTest::failures == 0 ? 0 : 1;
}
