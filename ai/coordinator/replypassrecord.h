#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <QJsonObject>

#include "ai/coordinator/decisiontrace.h"
#include "ai/coordinator/masterplanhandoff.h"
#include "ai/coordinator/replypass.h"
#include "ai/coordinator/replypassmode.h"

namespace Coordinator
{
    constexpr std::int32_t REPLY_PASS_RECORD_SCHEMA = 1;
    inline constexpr auto REPLY_PASS_RECORD_FILE = "coordinated-ai-reply-pass.jsonl";

    enum class ReplyPassSkip : std::int8_t
    {
        None,
        NotLocalSession,
        NoReplyBoard,
        LegacyPlan,
        NoHandoff,
    };

    inline constexpr std::array REPLY_PASS_SKIP_NAMES{
        "NONE", "NOT_LOCAL_SESSION", "NO_REPLY_BOARD", "LEGACY_PLAN", "NO_HANDOFF",
    };

    struct ReplyPassWiring
    {
        bool replyFacts{false};
        bool handoff{false};
        bool record{false};
    };

    // Our units the reply shoots in one plan, split by whether the pass may change them.
    struct ReplyPassShotUnits
    {
        std::int64_t shot{0};
        std::int64_t changeable{0};
        std::int64_t fixedRole{0};
        std::int64_t committed{0};
    };

    // verification is empty when the pass retained no plan, so there was nothing to verify.
    struct ReplyPassRun
    {
        ReplyPassMode mode{ReplyPassMode::Recorded};
        ReplyPassSkip skip{ReplyPassSkip::None};
        MasterPlanHandoffStatus handoff{MasterPlanHandoffStatus::NotRequested};
        std::vector<std::int32_t> planChoices;
        std::optional<ReplyPassResult> result;
        std::optional<ReplyPassShotUnits> planInEffectShots;
        std::vector<std::optional<ReplyPassShotUnits>> acceptedPlanShots;
        std::optional<ReplyPassVerification> verification;
        std::int64_t passNanos{0};
        std::int64_t shotNanos{0};
        std::int64_t verificationNanos{0};
    };

    ReplyPassWiring replyPassWiring(ReplyPrediction prediction, ReplyPassMode mode, bool localSession);

    // Installs nothing: the handoff, board and field are only read.
    ReplyPassRun recordedReplyPass(bool localSession, bool legacyPlanning, const MasterPlanHandoffResult & handoff,
                                   const ReplyBoard* pBoard, const PropertyStockField & field);

    QJsonObject replyPassRecord(const DecisionTraceIdentity & identity, const ReplyPassRun & run);
}
