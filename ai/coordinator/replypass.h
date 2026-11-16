#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ai/coordinator/capturedangerpricing.h"
#include "ai/coordinator/masterassignmentproblem.h"
#include "ai/coordinator/replypassevaluation.h"
#include "ai/coordinator/replystockcontext.h"

namespace Coordinator
{
    constexpr std::int32_t REPLY_PASS_WARMUP_ROUNDS = 10;
    constexpr std::int32_t REPLY_PASS_BASELINE_ROUNDS = 200;
    constexpr MilliFunds REPLY_PASS_TARGET_GAP_PPM = 5'000;
    constexpr std::int32_t REPLY_PASS_RETUNE_CHANGES = 4;
    constexpr std::int32_t REPLY_PASS_RETUNE_ROUNDS = 50;
    constexpr std::int32_t REPLY_PASS_SWITCH_LIMIT = 8;

    struct ReplyPassLimits
    {
        std::int32_t warmupRounds{REPLY_PASS_WARMUP_ROUNDS};
        std::int32_t baselineRounds{REPLY_PASS_BASELINE_ROUNDS};
        MilliFunds targetGapPpm{REPLY_PASS_TARGET_GAP_PPM};
        std::int32_t retuneChanges{REPLY_PASS_RETUNE_CHANGES};
        std::int32_t retuneRounds{REPLY_PASS_RETUNE_ROUNDS};
        std::int32_t switchLimit{REPLY_PASS_SWITCH_LIMIT};
    };

    enum class ReplyPassEnd : std::int8_t
    {
        NotRun,
        InvalidLimits,
        IncumbentNoValue,
        NoContact,
        NoEligibleChange,
        BaselineFailed,
        NoProvenChange,
        SwitchLimit,
    };

    inline constexpr std::array REPLY_PASS_END_NAMES{
        "NOT_RUN", "INVALID_LIMITS", "INCUMBENT_NO_VALUE", "NO_CONTACT", "NO_ELIGIBLE_CHANGE", "BASELINE_FAILED",
        "NO_PROVEN_CHANGE", "SWITCH_LIMIT",
    };

    inline constexpr const char* REPLY_PASS_VALUE_LABEL = "T1_VALUE";
    inline constexpr const char* REPLY_PASS_CEILING_LABEL = "T2_CEILING";
    inline constexpr const char* REPLY_PASS_FLOOR_LABEL = "T3_FLOOR";
    inline constexpr const char* REPLY_PASS_RETUNE_LABEL = "T4_RETUNE";

    enum class ReplyPassTier : std::int8_t
    {
        None,
        Value,
        Ceiling,
        Floor,
        Retune,
    };

    inline constexpr std::array REPLY_PASS_TIER_NAMES{
        "NONE", REPLY_PASS_VALUE_LABEL, REPLY_PASS_CEILING_LABEL, REPLY_PASS_FLOOR_LABEL, REPLY_PASS_RETUNE_LABEL,
    };

    enum class ReplyPassOutcome : std::int8_t
    {
        NotTried,
        NoValue,
        NoInterval,
        ProvenNotBetter,
        CannotClear,
        Undecided,
        Switched,
    };

    inline constexpr std::array REPLY_PASS_OUTCOME_NAMES{
        "NOT_TRIED", "NO_VALUE", "NO_INTERVAL", "PROVEN_NOT_BETTER", "CANNOT_CLEAR", "UNDECIDED", "SWITCHED",
    };

    enum class ReplyPassStage : std::int8_t
    {
        Baseline,
        Value,
        Ceiling,
        Floor,
        Retune,
    };

    inline constexpr std::array REPLY_PASS_STAGE_NAMES{
        "BASELINE", REPLY_PASS_VALUE_LABEL, REPLY_PASS_CEILING_LABEL, REPLY_PASS_FLOOR_LABEL, REPLY_PASS_RETUNE_LABEL,
    };

    enum class ReplyPassMismatch : std::int8_t
    {
        None,
        NotVerified,
        NoPlans,
        PlanNoValue,
        ExecutionOrder,
        Keys,
        Terms,
        ExactNonStock,
        Owned,
        MissingEvidence,
        Prices,
        Ceiling,
        Witness,
        Floor,
        Interval,
        SwitchLinks,
        Margin,
        FinalChoices,
    };

    inline constexpr std::array REPLY_PASS_MISMATCH_NAMES{
        "NONE", "NOT_VERIFIED", "NO_PLANS", "PLAN_NO_VALUE", "EXECUTION_ORDER", "KEYS", "TERMS", "EXACT_NON_STOCK",
        "OWNED", "MISSING_EVIDENCE", "PRICES", "CEILING", "WITNESS", "FLOOR", "INTERVAL", "SWITCH_LINKS", "MARGIN",
        "FINAL_CHOICES",
    };

    struct ReplyPassInterval
    {
        MilliFunds lower{0};
        MilliFunds upper{0};

        friend bool operator==(const ReplyPassInterval &, const ReplyPassInterval &) = default;
    };

    // present is set only once every value below was computed and checked.
    struct ReplyPassStockEvidence
    {
        bool present{false};
        std::vector<MilliFunds> prices;
        MilliFunds ceiling{0};
        MilliFunds floor{0};
        std::vector<SequentialRowWitness> witness;
        ReplyPassInterval interval;

        friend bool operator==(const ReplyPassStockEvidence &, const ReplyPassStockEvidence &) = default;
    };

    // baseline holds the certificate taken while this plan was the plan in effect.
    struct ReplyPassPlan
    {
        std::vector<std::int32_t> choices;
        std::vector<std::int32_t> executionOrder;
        std::vector<ReplyPassActor> actors;
        ContinuationKey afterOwnActions;
        ContinuationKey afterReply;
        MilliFunds exactNonStock{0};
        MilliFunds owned{0};
        ReplyPassStockEvidence baseline;

        friend bool operator==(const ReplyPassPlan &, const ReplyPassPlan &) = default;
    };

    // proof is the changed plan's evidence at the prices that proved lower(to) > upper(from).
    struct ReplyPassSwitch
    {
        std::size_t from{0};
        std::size_t to{0};
        std::int32_t actorSlot{NO_REPLY_PASS_ACTOR};
        std::int32_t candidateIndex{NO_CANDIDATE};
        ChangeReason reason{ChangeReason::Shot};
        ReplyPassTier tier{ReplyPassTier::None};
        MilliFunds incumbentUpper{0};
        MilliFunds changedLower{0};
        MilliFunds margin{0};
        ReplyPassStockEvidence proof;

        friend bool operator==(const ReplyPassSwitch &, const ReplyPassSwitch &) = default;
    };

    struct ReplyPassChange
    {
        std::int32_t round{0};
        std::int32_t actorSlot{NO_REPLY_PASS_ACTOR};
        std::int32_t engineUnitId{NO_UNIT};
        std::int32_t candidateIndex{NO_CANDIDATE};
        ChangeReason reason{ChangeReason::Shot};
        MilliFunds replyLoss{0};
        ReplyPassTier tier{ReplyPassTier::None};
        ReplyPassOutcome outcome{ReplyPassOutcome::NotTried};
        ReplyPassNoValue noValue{ReplyPassNoValue::NotEvaluated};
        ReplyStockStatus stockStatus{ReplyStockStatus::Invalid};
        std::optional<MilliFunds> upper;
        std::optional<MilliFunds> lower;

        friend bool operator==(const ReplyPassChange &, const ReplyPassChange &) = default;
    };

    struct ReplyPassChangeOrder
    {
        bool operator()(const ReplyPassChange & left, const ReplyPassChange & right) const
        {
            if (left.replyLoss != right.replyLoss)
            {
                return left.replyLoss > right.replyLoss;
            }
            if (left.engineUnitId != right.engineUnitId)
            {
                return left.engineUnitId < right.engineUnitId;
            }
            return left.candidateIndex < right.candidateIndex;
        }
    };

    struct ReplyPassCounters
    {
        std::int64_t unitsShot{0};
        std::int64_t shotChangeable{0};
        std::int64_t shotFixedRole{0};
        std::int64_t shotCommitted{0};
        std::int64_t listedShot{0};
        std::int64_t listedHitsShooter{0};
        std::array<std::int64_t, REPLY_PASS_NO_VALUE_NAMES.size()> noValue{};
        std::int64_t noInterval{0};
        std::int64_t provenNotBetter{0};
        std::int64_t cannotClear{0};
        std::int64_t floorsBuilt{0};
        std::int64_t undecided{0};
        std::int64_t retunes{0};
        std::int64_t switchesAtFloor{0};
        std::int64_t switchesAtRetune{0};
        bool endedByLimit{false};

        friend bool operator==(const ReplyPassCounters &, const ReplyPassCounters &) = default;
    };

    // Observed only; nothing in the pass reads these to decide.
    struct ReplyPassCost
    {
        std::int64_t nanos{0};
        std::int64_t work{0};
        std::int64_t calls{0};

        friend bool operator==(const ReplyPassCost &, const ReplyPassCost &) = default;
    };

    struct ReplyPassResult
    {
        ReplyPassEnd end{ReplyPassEnd::NotRun};
        ReplyPassNoValue incumbentNoValue{ReplyPassNoValue::NotEvaluated};
        ReplyStockStatus baselineStatus{ReplyStockStatus::Invalid};
        std::vector<std::int32_t> finalChoices;
        std::vector<ReplyPassPlan> plans;
        std::vector<ReplyPassSwitch> switches;
        std::vector<ReplyPassChange> changes;
        ReplyPassCounters counters;
        std::array<ReplyPassCost, REPLY_PASS_STAGE_NAMES.size()> costs{};
        bool stockContextBuilt{false};

        friend bool operator==(const ReplyPassResult &, const ReplyPassResult &) = default;
    };

    struct ReplyPassVerification
    {
        ReplyPassMismatch mismatch{ReplyPassMismatch::NotVerified};
        std::size_t index{0};

        bool verified() const
        {
            return mismatch == ReplyPassMismatch::None;
        }
    };

    namespace ReplyPassDetail
    {
        std::vector<ReplyPassChange> listChanges(const MasterAssignmentProblem & problem,
                                                 std::span<const std::int32_t> working,
                                                 const ReplyPassEvaluation & evaluation, std::int32_t round,
                                                 ReplyPassCounters & counters);
    }

    // Works on private copies only: the caller's problem, choices and field are never changed.
    ReplyPassResult runReplyPass(const MasterAssignmentProblem & problem, std::span<const std::int32_t> choices,
                                 const ReplyBoard & board, const PropertyStockField & field,
                                 const ReplyPassLimits & limits = {});

    ReplyPassVerification verifyReplyPass(const MasterAssignmentProblem & problem, const ReplyBoard & board,
                                          const PropertyStockField & field, const ReplyPassResult & result);
}
