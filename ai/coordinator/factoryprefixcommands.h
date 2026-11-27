#pragma once

#include "ai/coordinator/decisiontrace.h"
#include "ai/coordinator/turnplan.h"
#include "coreengine/memorymanagement.h"
#include "coreengine/settings.h"
#include "game/building.h"
#include "game/gameaction.h"
#include "game/gamemap.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"
#include "game/unitpathfindingsystem.h"
#include "gameinput/markedfielddata.h"

#include <QJsonDocument>
#include <algorithm>

namespace Coordinator
{
    // Private measurement at the actual executor boundary. The caller writes
    // each dispatch so the consumer can authenticate the prefix against the tape.
    inline void recordFactoryPrefixCommands(GameMap &map,Player &player,const PlannedAction &planned,
                                            int actionIndex,qint64 sequence,GameAction &dispatched)
    {
        const auto point=[](QPoint p){return QJsonArray{p.x(),p.y()};};
        const auto route=[&](const auto &steps){QJsonArray path;for(const auto &p:steps)path.append(point(p));return path;};
        const auto origin=dispatched.getTarget(),dest=dispatched.getActionTarget();
        QJsonObject rec{{"day",map.getCurrentDay()},{"player",player.getPlayerID()},{"planSequence",sequence},
                        {"actionIndex",actionIndex},{"actor",planned.unitId},{"action",dispatched.getActionID()},
                        {"origin",point(origin)},{"destination",point(dest)},{"path",route(dispatched.getMovePath())}};
        if(dispatched.getActionID()=="ACTION_FIRE")rec["target"]=QJsonArray{planned.target.x,planned.target.y};
        auto *actor=map.getUnit(planned.unitId);
        auto *building=map.onMap(dest.x(),dest.y())?map.getTerrain(dest.x(),dest.y())->getBuilding():nullptr;
        if(actor && actor->getOwner()==&player && !actor->getHasMoved() && dispatched.getActionID()=="ACTION_WAIT" &&
           building && building->getBuildingID()=="FACTORY" && building->getOwner()==&player)
        {
            UnitPathFindingSystem pfs(&map,actor,&player);pfs.explore();
            auto nodes=pfs.getAllNodePointsFast();
            if(std::find(nodes.begin(),nodes.end(),origin)==nodes.end())nodes.push_back(origin);
            const auto nativeActions=actor->getActionList();
            const QStringList simple{"ACTION_WAIT","ACTION_CAPTURE","ACTION_JOIN","ACTION_LOAD"};
            QJsonArray unhandled,rows;
            for(const auto &id:nativeActions)if(!simple.contains(id) && id!="ACTION_FIRE")unhandled.append(id);
            for(const auto &to:nodes)
            {
                auto path=pfs.getPathFast(to.x(),to.y());if(to==origin && path.empty())path.push_back(origin);
                QJsonObject node{{"to",point(to)},{"path",route(path)}};
                if(path.empty() || path.front()!=to || path.back()!=origin)
                {node["status"]="NO_CANONICAL_PATH";rows.append(node);continue;}
                const auto cost=pfs.getCosts(path);
                node["movementCost"]=cost;
                if(cost<0){node["status"]="NO_NATIVE_MOVEMENT_COST";rows.append(node);continue;}
                auto make=[&](const QString &id){
                    auto action=MemoryManagement::create<GameAction>(id,&map);
                    action->setTarget(origin);action->setMovepath(path,cost);return action;
                };
                QJsonArray legal,open;
                for(const auto &id:simple)
                {
                    if(!nativeActions.contains(id))continue;
                    auto action=make(id);
                    if(!action->canBePerformed(id,false,&player))continue;
                    if(action->isFinalStep())legal.append(QJsonObject{{"action",id}});
                    else open.append(id);
                }
                if(nativeActions.contains("ACTION_FIRE"))
                {
                    const QString fire="ACTION_FIRE";
                    auto initial=make(fire);
                    if(initial->canBePerformed(fire,false,&player))
                    {
                        // The native final step rolls battle luck. Read the actual
                        // target menu without finalizing any hypothetical shot.
                        if(initial->getStepInputType()=="FIELD")
                        {
                            const auto marks=initial->getMarkedFieldStepData();
                            if(marks)node["offeredFireTargets"]=route(*marks->getPoints());
                        }
                        open.append(fire);
                    }
                }
                node["status"]="NATIVE_CALLBACKS_CHECKED";node["legal"]=legal;node["moreInputRequired"]=open;rows.append(node);
            }
            rec["menu"]=QJsonObject{{"scope","CURRENT_ACTOR_ONE_NATIVE_PATH_PER_REACHABLE_ENDPOINT"},
                {"nativeActions",QJsonArray::fromStringList(nativeActions)},{"unhandledActions",unhandled},
                {"origin",point(origin)},{"factory",point(dest)},{"nodes",rows}};
        }
        const auto file=Settings::getInstance()->getUserPath()+QStringLiteral("factory-prefix-commands.jsonl");
        appendTraceRecords(file,{QString::fromUtf8(QJsonDocument(rec).toJson(QJsonDocument::Compact))});
    }
}
