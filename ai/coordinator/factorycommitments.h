#pragma once

#include <QJsonDocument>
#include "ai/coordinator/masterassignmentproblem.h"
#include "ai/coordinator/productionforecast.h"
#include "ai/coordinator/replypassevaluation.h"
#include "coreengine/scriptvariables.h"

namespace Coordinator
{
    inline const QString FUNDED_FACTORY_RELEASES = QStringLiteral("COORDINATED_FUNDED_FACTORY_RELEASES_V1");

    inline QJsonArray readDatedOrders(ScriptVariables *variables, const QString &key, int day, int player)
    {
        auto *stored = variables ? variables->getVariable(key) : nullptr;
        if (!stored || stored->getStoredData().size() > 65536)
            return {};
        const auto record = QJsonDocument::fromJson(stored->readDataString().toUtf8()).object();
        if (record["player"].toInt(-1) != player) return {};
        const int previous = record["day"].toInt(-2);
        if (day == previous) return record["due"].toArray();
        return day == previous + 1 ? record["next"].toArray() : QJsonArray{};
    }

    inline void writeDatedOrders(ScriptVariables *variables, const QString &key, int day, int player,
                                const QJsonArray &due, const QJsonArray &next)
    {
        if (!variables) return;
        variables->deleteVariable(key);
        if (due.isEmpty() && next.isEmpty()) return;
        const QJsonObject state{{"day",day},{"player",player},{"due",due},{"next",next}};
        variables->createVariable(key)->writeDataString(QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Compact)));
    }

    inline TilePoint factoryPoint(const QJsonValue &value)
    {
        const auto pair = value.toArray();
        if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble()) return INVALID_TILE;
        return {pair[0].toInt(-1), pair[1].toInt(-1)};
    }

    // Validate yesterday's conditional response against today's observed ownership,
    // unit, cash and generated movement options before it influences any comparison.
    inline QJsonArray matureFactoryReleases(const QJsonArray &prior, ProductionForecastBoard &board,
        const MasterAssignmentProblem &problem, const BattlefieldKnowledge &knowledge)
    {
        QJsonArray active;
        auto funds = board.funds[0];
        int remainingSlots = board.availableUnits[0];
        for (const auto &v : prior)
        {
            if (remainingSlots<=0) break;
            auto order = v.toObject();
            const auto at = factoryPoint(order["factory"]), exit = factoryPoint(order["vacaterEnd"]);
            const int unit = order["vacater"].toInt(NO_UNIT);
            if (unit == NO_UNIT || at == INVALID_TILE || exit == INVALID_TILE || at == exit) continue;
            const int occupant=knowledge.unitIndexAt(at.x,at.y);
            auto actor = std::find_if(problem.actors().begin(), problem.actors().end(), [&](const auto &a) {
                return a.knowledgeUnitIndex == occupant;
            });
            if (actor == problem.actors().end() ||
                (!problem.canChangeActor(actor->canonicalSlot) && !problem.canChangeCombatHelper(*actor))) continue;
            auto site = std::find_if(board.sites.begin(), board.sites.end(), [&](const auto &s) { return s.tile == at; });
            if (site == board.sites.end() || board.matureFactoryCells.contains({at.x,at.y})) continue;
            auto offer = std::find_if(site->menu[0].begin(), site->menu[0].end(), [&](const auto &o) {
                return o.type == order["type"].toString() && o.cost <= funds;
            });
            if (offer == site->menu[0].end()) continue;
            auto move=std::find_if(actor->candidates.begin(), actor->candidates.end(), [&](const auto &c) {
                return c.plannable && c.source.vacatesActiveFriendlyProduction && c.destination == exit;
            });
            if (move==actor->candidates.end())
                move=std::find_if(actor->candidates.begin(),actor->candidates.end(),[](const auto &c) {
                    return c.plannable && c.source.vacatesActiveFriendlyProduction;
                });
            if (move==actor->candidates.end()) continue;
            // Observed traffic may have changed the holder or its exit. Admit a
            // real order for today's holder; only a freshly priced team receives value.
            if (actor->engineUnitId!=unit || move->destination!=exit)
            {
                order["previousVacater"]=unit;
                order["previousVacaterEnd"]=order.value("vacaterEnd");
                order["vacater"]=actor->engineUnitId;
                order["vacaterEnd"]=QJsonArray{move->destination.x,move->destination.y};
            }
            funds -= offer->cost;
            --remainingSlots;
            active.append(order);
            board.matureFactoryCells.emplace(at.x,at.y);
        }
        return active;
    }

    inline void storeFactoryReleases(ScriptVariables *variables, int day, int player,
        const QJsonArray &due, const QJsonObject &policy, const ReplyPassEvaluation &plan, QJsonObject &receipt)
    {
        if (!variables) return;
        QJsonArray next, resolution;
        std::map<std::pair<int,int>,int> scheduled;
        auto remember = [&](const QJsonObject &order, bool replace) {
            const auto at = factoryPoint(order["factory"]);
            auto [it,fresh]=scheduled.emplace(std::pair{at.x,at.y},static_cast<int>(next.size()));
            const QJsonObject item{{"type",order["type"]},{"factory",order["factory"]},
                {"vacater",order["vacater"]},{"vacaterEnd",order["vacaterEnd"]},{"price",order["price"]}};
            if (fresh) next.append(item);
            else if (replace) next[it->second]=item;
        };
        for (const auto &v : due)
        {
            auto order = v.toObject();
            const auto at = factoryPoint(order["factory"]);
            int occupant = NO_UNIT;
            for (const auto &a : plan.actors)
                if (a.stepsAfterOwnActions > 0 && plan.replay.plan.action(a.actionIndex).destination == at)
                    occupant = a.engineUnitId;
            order["friendlyOccupantAfterOrders"] = occupant;
            order["openedThisTurn"] = occupant == NO_UNIT;
            resolution.append(order);
            // Only retain an unresolved promise for its original holder. A different
            // occupant is a new decision and must qualify on its own observed state.
            if (occupant != NO_UNIT && occupant == order["vacater"].toInt(NO_UNIT)) remember(order,false);
        }
        for (const auto &v : policy["productionOrders"].toArray())
        {
            const auto order = v.toObject();
            if (!order["enemy"].toBool() && order["buildDay"].toInt(-1) == 1 &&
                order["vacater"].toInt(NO_UNIT) != NO_UNIT) remember(order,true);
        }
        receipt["factoryReleaseResults"] = resolution;
        receipt["nextFactoryReleases"] = next;
        writeDatedOrders(variables,FUNDED_FACTORY_RELEASES,day,player,due,next);
    }
}
