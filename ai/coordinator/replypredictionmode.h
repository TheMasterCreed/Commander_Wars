#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "ai/coordinator/turnplan.h"

namespace Coordinator
{
    inline std::int64_t nanosSince(std::chrono::steady_clock::time_point started)
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
            .count();
    }

    struct ReplyBoard;

    enum class ReplyPrediction : std::int8_t
    {
        Off,
        Recorded,
    };

    struct ReplyFactsStats
    {
        std::int64_t nanos{0};
        std::int64_t tileFacts{0};
        std::int64_t actorFacts{0};
        std::int64_t candidates{0};
    };

    struct ReplyPredictionTally
    {
        std::int64_t replays{0};
        std::int64_t incomplete{0};
        std::int64_t nanos{0};
        std::int64_t pairs{0};
    };

    struct ReplyPredictionTelemetry
    {
        ReplyPrediction mode{ReplyPrediction::Off};
        ReplyFactsStats facts;
        ReplyPredictionTally replays;
    };

    struct ReplyTelemetryField
    {
        const char* name{nullptr};
        std::int64_t value{0};
    };

    constexpr std::size_t REPLY_TELEMETRY_FIELD_COUNT = 8;

    // One list for PHASE_TIMING and the danger audit, so both spell each field the same.
    constexpr std::array<ReplyTelemetryField, REPLY_TELEMETRY_FIELD_COUNT> replyTelemetryFields(
        const ReplyPredictionTelemetry & telemetry)
    {
        return {{
            {"replyFactsNanos", telemetry.facts.nanos},
            {"replyTileFacts", telemetry.facts.tileFacts},
            {"replyActorFacts", telemetry.facts.actorFacts},
            {"replyFactCandidates", telemetry.facts.candidates},
            {"replyPredictionNanos", telemetry.replays.nanos},
            {"replyPredictionPairs", telemetry.replays.pairs},
            {"replyPredictions", telemetry.replays.replays},
            {"replyPredictionsIncomplete", telemetry.replays.incomplete},
        }};
    }

    // The tally is shared so a problem copy may outlive the turn that opened it.
    struct ReplyPredictionSetup
    {
        ReplyPrediction mode{ReplyPrediction::Off};
        std::shared_ptr<const ReplyBoard> board;
        std::shared_ptr<ReplyPredictionTally> tally;
    };

    struct PlanReplyActorOutcome
    {
        std::int32_t engineUnitId{NO_UNIT};
        std::int32_t actionIndex{NO_ACTION};
        double hpBefore{0};
        std::int32_t hpStepsBefore{0};
        std::int32_t stepsLost{0};
        bool destroyed{false};

        friend bool operator==(const PlanReplyActorOutcome &, const PlanReplyActorOutcome &) = default;
    };

    struct PlanReplyPrediction
    {
        bool complete{false};
        std::vector<PlanReplyActorOutcome> actors;
        std::int64_t pairsExamined{0};

        friend bool operator==(const PlanReplyPrediction &, const PlanReplyPrediction &) = default;
    };
}
