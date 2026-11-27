#pragma once

#include <QJsonObject>
#include <optional>
#include <span>

class GameMap;
class ScriptVariables;
class SimpleProductionSystem;
namespace Coordinator
{
    class BattlefieldKnowledge;
    class MobilityFieldCache;
    class PropertyStockField;
    struct AssignmentResult;
    struct DecisionTraceIdentity;
    struct MasterPlanHandoff;
    struct AssignmentInput;
    struct PropertyFacts;
    struct ReplyBoard;
    class ReplyFactsBuilder;
    enum class CombatMode
    {
        Off,
        Recorded,
        Acting
    };
    CombatMode combatMode();
    QJsonObject compareCombatPlans(CombatMode mode, const DecisionTraceIdentity &identity, GameMap &map,
                                   const BattlefieldKnowledge &knowledge, MobilityFieldCache &mobility,
                                   const MasterPlanHandoff *handoff, const ReplyBoard *initialBoard,
                                   const PropertyStockField &field, AssignmentResult &assignment,
                                   std::int64_t allowance, ScriptVariables *variables = nullptr,
                                   SimpleProductionSystem *actingBuyer = nullptr, std::int64_t pairProofAllowance = 0,
                                   ReplyFactsBuilder *replyFacts = nullptr, const AssignmentInput *progressInput = nullptr,
                                   std::span<const PropertyFacts> progressSites = {});
    QJsonObject planUnifiedTurn(const DecisionTraceIdentity &identity, GameMap &map,
                                const BattlefieldKnowledge &knowledge, MobilityFieldCache &mobility,
                                const MasterPlanHandoff &handoff, const ReplyBoard &initialBoard,
                                const PropertyStockField &field, AssignmentResult &assignment,
                                std::span<const PropertyFacts> properties,
                                std::int64_t allowance = 0,
                                ReplyFactsBuilder *replyFacts = nullptr,
                                std::optional<quint32> turnLuckNonce = std::nullopt);
} // namespace Coordinator
