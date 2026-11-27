#pragma once

#include "ai/coordinator/masterassignmentproblem.h"
#include "ai/coordinator/masterassignmentstock.h"
#include "ai/coordinator/combatforecast.h"
#include "ai/coordinator/replypassevaluation.h"
#include "ai/coordinator/propertyeconomics.h"
#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/terminalvalue.h"
#include <QJsonArray>
#include <QJsonObject>
#include <map>

class GameMap;
namespace Coordinator
{
    struct AssignmentInput;
    class PropertyStockField;
    struct StockProgressArmBounds
    {
        MilliFunds lower{},upper{};
        std::int64_t previousAllowance{};
        std::vector<MilliFunds> sitePrices;
    };
    // Constructed for one immutable hearing. No board, rule or future-catalog
    // changes are admitted while these complete physical teams share bounds.
    using StockProgressArms=std::map<std::vector<int>,StockProgressArmBounds>;
    struct StockProgressPricing
    {
        RefinementLedger ledger;
        MasterAssignmentStockContext stock;
        bool prepare(const PropertyStockField &field,const AssignmentInput &input,std::int64_t offer,std::int64_t &work);
    };
    QJsonObject priceStockProgress(const MasterAssignmentProblem &problem,StockProgressPricing &pricing,
        const std::vector<int> &before,const std::vector<int> &after,StockProgressArms &arms,
        std::int64_t allowance,std::int64_t &work);

    inline QJsonArray witnessedStockStarts(const QJsonArray &proofs)
    {
        QJsonArray starts;
        for(const auto &v:proofs)
        {
            const auto p=v.toObject();
            if(p["enemy"].toBool(true) || p["status"]!="WITNESSED" || p["departureTick"].toInt(-1)!=2 ||
               p["actualCompletionTick"].toInt(-1)<2 || !p["shots"].isArray() || !p["shots"].toArray().isEmpty())continue;
            const auto stops=p["stops"].toArray();
            if(stops.isEmpty())continue;
            const auto stop=stops.first().toObject();
            if(stop["tick"].toInt(-1)!=2 || !stop["cells"].isArray())continue;
            starts.append(QJsonObject{{"unit",p["unit"]},{"from",p["from"]},{"to",stop["tile"]},
                {"cells",stop["cells"]},{"property",p["tile"]},{"completion",p["actualCompletionTick"]}});
        }
        return starts;
    }

    struct StockProgressPlan
    {
        std::vector<int> choices;
        QJsonArray measures;
    };

    QString stockProgressOpeningScope(GameMap &map,const BattlefieldKnowledge &knowledge,const StockProgressPlan &selection,
        const QJsonArray &purchases,std::int64_t &work);

    // An unblocked enemy that can even reach a changed crossing or cover its
    // tile keeps that move in the ordinary tactical comparison. Positive native
    // direction costs yield an optimistic movement radius without using traffic.
    inline StockProgressPlan matchedStockProgress(const MasterAssignmentProblem &problem,const CombatTerrain &terrain,
        const BattlefieldKnowledge &knowledge,std::span<const PropertyFacts> properties,const QJsonObject &record,
        const std::vector<int> &before,const std::vector<int> &proposed,std::int64_t &work)
    {
        StockProgressPlan out{before,{}};
        if(before.size()!=proposed.size() || terrain.units.size()!=knowledge.units().size())return out;
        int omitted=NO_UNIT;
        int namedFriendly=NO_UNIT;
        if(!record["completeOpeningReplyMenu"].toBool() || record.contains("unpricedReplyPolicy"))
        {
            const auto policy=record["unpricedReplyPolicy"].toString();
            const auto separator=policy.lastIndexOf('_');
            bool ok=false;
            const auto id=policy.mid(separator+1).toInt(&ok);
            if(!ok || id<0)return out;
            if(policy=="WAIT_ON_PROPERTY_"+QString::number(id))omitted=id;
            else if(policy=="TARGET_UNIT_"+QString::number(id) || policy=="BYPASS_UNIT_"+QString::number(id))namedFriendly=id;
            else return out;
            if(namedFriendly!=NO_UNIT && std::ranges::none_of(terrain.units,[&](const auto &u){
                return u.present && !u.enemy && u.engineUnitId==namedFriendly;
            }))return out;
        }
        const auto replies=record["replyAlternatives"].toArray();
        if(replies.isEmpty())return out;
        std::set<int> guardIds;
        for(const auto &v:replies)
        {
            const auto p=v.toObject();
            if(p["replyPolicy"].toString().isEmpty() || !p["witnessedStockStarts"].isArray() ||
               p["terminalClass"].toInt(-1)!=int(TerminalClass::Unresolved))return out;
            if(p["hqStatus"]=="NO_NEAR_APPROACH")continue;
            const auto action=p["guardAction"].toObject();const int id=p["hqGuard"].toInt(NO_UNIT);
            if(p["hqStatus"]!="F1_RETURN_COVER" || id<0 || id!=action["unit"].toInt(NO_UNIT) ||
               !action["reservedOnF1"].toBool() || !action["returned"].toBool() || !action["covered"].toBool() ||
               action["afterFriendlyAction"].toInt()<=0 || action["afterFollowup"].toInt()<=0 ||
               action["clearableByStationaryVolley"].toBool(true) || action["from"].toArray().size()!=2 ||
               action["to"].toArray().size()!=2)return out;
            guardIds.insert(id);
        }
        struct HostileReach
        {
            TilePoint origin;
            int movement,range,grid;
            std::optional<DistanceField> distances;
        };
        std::vector<HostileReach> nativeReach;
        std::vector<std::pair<TilePoint,int>> threats;
        std::map<int,int> guardRadii;
        std::optional<std::pair<TilePoint,int>> secondThreat;
        std::map<int,int> minimums;
        for(std::size_t i=0;i<terrain.units.size();++i)
        {
            const auto &pawn=terrain.units[i];const auto &unit=knowledge.units()[i];
            if(!pawn.enemy && !guardIds.contains(pawn.engineUnitId))continue;
            if(!pawn.present || unit.carrier!=KnownUnit::NO_CARRIER || pawn.supportsUnits || pawn.transportCapacity>0 ||
               pawn.tile==INVALID_TILE || unit.maxRange<0)return out;
            const int grid=pawn.travelGrid>=0?pawn.travelGrid:pawn.grid;
            const int movement=std::max(pawn.movement,pawn.movementBeforeFuel);
            if(grid<0 || grid>=int(terrain.grids.size()) || movement<0)return out;
            auto [it,fresh]=minimums.try_emplace(grid,std::numeric_limits<int>::max());
            if(fresh)
            {
                const auto &g=terrain.grids[grid];
                const auto cells=4LL*g.width()*g.height();
                if(cells<1 || work<cells)return out;
                work-=cells;
                for(int y=0;y<g.height();++y)for(int x=0;x<g.width();++x)for(auto dir:ENTRY_DIRECTIONS)
                {
                    const int c=g.cost(x,y,dir);if(c>=0)it->second=std::min(it->second,c);
                }
            }
            if(it->second<=0)return out;
            const int steps=movement; // Every bound native entry costs at least one, even on later activations.
            if(!pawn.enemy){guardRadii[pawn.engineUnitId]=steps;continue;}
            threats.emplace_back(pawn.tile,steps+std::max(1,unit.maxRange));
            nativeReach.push_back({pawn.tile,steps,std::max(1,unit.maxRange),grid,{}});
            if(pawn.engineUnitId==omitted)
            {
                if(!pawn.capper || !std::ranges::any_of(properties,[&](const PropertyFacts &p){
                    return TilePoint{p.x,p.y}==pawn.tile;}))return out;
                secondThreat.emplace(pawn.tile,2*steps+std::max(1,unit.maxRange));
            }
        }
        if((omitted>=0 && !secondThreat) || guardRadii.size()!=guardIds.size())return out;
        struct ReturnDuty{TilePoint from,to;int steps;};
        std::vector<ReturnDuty> duties;
        for(const auto &v:replies){
            const auto p=v.toObject();
            if(p["hqStatus"]!="F1_RETURN_COVER")continue;
            const auto start=p["guardAction"].toObject()["from"].toArray();
            const auto end=p["guardAction"].toObject()["to"].toArray();
            duties.push_back({{start[0].toInt(),start[1].toInt()},{end[0].toInt(),end[1].toInt()},guardRadii.at(p["hqGuard"].toInt())});
        }
        const auto point=[](TilePoint t){return QJsonArray{t.x,t.y};};
        // Units complete their ordinary F0 movement before the enemy can act.
        // Test all legal hostile movement endpoints in a relaxation that allows
        // every other unit to vacate. This also covers an earlier E0 casualty
        // opening a lane. Bound native hooks and recheck the two complete teams
        // (including shots, HQ duties and actual purchases) before publication.
        const auto clearAtReply=[&](TilePoint post){
            for(auto &enemy:nativeReach){
                if(manhattanDistance(enemy.origin,post)>enemy.movement+enemy.range)continue;
                const auto &grid=terrain.grids[enemy.grid];
                if(!enemy.distances){
                    const auto cost=6LL*grid.width()*grid.height();
                    if(cost<1 || work<cost)return false;
                    work-=cost;
                    enemy.distances.emplace(DistanceField::build(grid,{enemy.origin},FieldExpansion::FromSources));
                }
                const int minY=std::max(0,post.y-enemy.range),maxY=std::min(grid.height()-1,post.y+enemy.range);
                const int minX=std::max(0,post.x-enemy.range),maxX=std::min(grid.width()-1,post.x+enemy.range);
                for(int y=minY;y<=maxY;++y)for(int x=minX;x<=maxX;++x){
                    if(work<=0)return false;
                    --work;
                    if(manhattanDistance({x,y},post)>enemy.range)continue;
                    const int distance=enemy.distances->distance(x,y);
                    if(distance>=0 && distance<=enemy.movement)return false;
                }
            }
            return true;
        };
        QJsonArray otherCaptures,otherStops;
        for(const auto &actor:problem.actors())
        {
            const int slot=actor.canonicalSlot;
            if(!problem.canChangeActor(slot) || actor.stockRow<0 || guardIds.contains(actor.engineUnitId) ||
               actor.knowledgeUnitIndex<0 || actor.knowledgeUnitIndex>=int(knowledge.units().size()) ||
               !knowledge.units()[actor.knowledgeUnitIndex].canCapture)continue;
            const auto &a=actor.candidates[before[slot]];
            if(actor.engineUnitId==namedFriendly)continue;
            if(proposed[slot]<0 || proposed[slot]>=int(actor.candidates.size()))continue;
            const auto &stock=actor.candidates[proposed[slot]];
            const bool standing=a.actionKind==PlanBundleKind::Wait && a.origin==a.destination;
            const bool repositioning=a.actionKind==PlanBundleKind::Move && a.origin!=a.destination;
            if((!standing && !repositioning) || a.capture ||
               !a.source.replyFacts || !a.source.replyFacts->tile || a.source.replyFacts->actorRealHp<UNIT_HP_STEPS)continue;
            const bool tacticalOrigin=!a.source.replyFacts->tile->shooters.empty() ||
                std::ranges::any_of(actor.candidates,[](const MasterCandidate &p){
                    return std::ranges::any_of(p.source.bundle.components,[](const BundleComponent &c){return c.kind==ComponentKind::Fire;});});
            if(tacticalOrigin && !standing)continue;
            for(int option=0;option<int(actor.candidates.size());++option)
            {
                if(work<=0)return out;
                --work;
                const auto &b=actor.candidates[option];
                const bool finishesHere=standing && b.capture && b.plannable && b.destination==a.origin &&
                    b.capture->turnsUntilOwned<=0 && std::ranges::any_of(properties,[&](const PropertyFacts &p){
                        return TilePoint{p.x,p.y}==a.origin && p.ownerId==NO_OWNER && p.visible && p.capturable &&
                            !p.canProduce && p.objective!=ObjectiveKind::Headquarters && p.capturerIndex==actor.knowledgeUnitIndex &&
                            p.capturePoints>0;
                    });
                if(b.origin!=a.origin || (b.destination==a.origin && !finishesHere) || (!b.capture && b.actionKind!=PlanBundleKind::Move) ||
                   !b.source.replyFacts || !b.source.replyFacts->tile || !b.source.replyFacts->tile->shooters.empty())continue;
                // The ordinary constructor can select a move on a zero-valued
                // tie. Hear the stock team's actual competing route and price
                // both complete teams, including the job at the original post.
                const bool alternativeRoute=repositioning && option==proposed[slot] &&
                    stock.plannable && !b.capture && b.destination!=a.destination &&
                    a.source.movementCost==b.source.movementCost;
                if(repositioning && !alternativeRoute && (!b.capture || b.capture->turnsUntilOwned<=0))continue;
                if(std::ranges::any_of(properties,[&](const PropertyFacts &p){
                    const TilePoint tile{p.x,p.y};
                    if(tile!=a.origin && tile!=a.destination && tile!=b.destination)return false;
                    // A common vacated origin has identical vacancy. The entire
                    // team's actual opening purchases are still compared later.
                    return p.objective==ObjectiveKind::Headquarters ||
                        (p.canProduce && (tile==a.destination || tile==b.destination)) ||
                        (tile==a.origin && p.capturerIndex==actor.knowledgeUnitIndex && p.capturePoints>0 && !finishesHere);}))continue;
                const auto &path=b.source.bundle.path;
                const auto outside=[&](TilePoint t){
                    const auto within=[&](const auto &zone){return manhattanDistance(t,zone.first)<=zone.second;};
                    return !std::ranges::any_of(threats,within) && (!secondThreat || !within(*secondThreat));
                };
                // F0 transit is replayed as part of the entire team. Only its
                // final occupants can block F1. This ellipse includes every
                // legal return, even if the stored report omits its exact path.
                const auto onDuty=[&](TilePoint t){return std::ranges::any_of(duties,[&](const ReturnDuty &d){
                    return manhattanDistance(t,d.from)+manhattanDistance(t,d.to)<=d.steps;});};
                if(path.empty() || path.front()!=a.origin || path.back()!=b.destination ||
                   onDuty(a.destination) || onDuty(b.destination))continue;
                // A target/bypass refusal can have further unpublished siblings.
                // Only disconnected changes are eligible for the full E0 recheck.
                if((namedFriendly!=NO_UNIT || finishesHere) && (tacticalOrigin || !outside(a.destination) ||
                   !std::ranges::all_of(path,outside)))continue;
                const bool witnessedPath=!tacticalOrigin && outside(a.destination) && std::ranges::all_of(path,outside);
                const bool stockStop=stock.plannable && stock.origin==a.origin && stock.destination!=a.origin &&
                    std::ranges::find(stock.source.bundle.path,b.destination)!=stock.source.bundle.path.end();
                const bool escape=tacticalOrigin && standing && b.plannable && b.capture.has_value() &&
                    a.destination==a.origin && clearAtReply(b.destination);
                const bool standingCandidate=!tacticalOrigin && standing && b.plannable && (b.capture.has_value() || stockStop) &&
                    (!secondThreat || (manhattanDistance(a.destination,secondThreat->first)>secondThreat->second &&
                                       manhattanDistance(b.destination,secondThreat->first)>secondThreat->second)) &&
                    clearAtReply(a.destination) && clearAtReply(b.destination);
                if(!witnessedPath && !standingCandidate && !escape)continue;
                QJsonArray cells;for(std::size_t j=1;j<path.size();++j)cells.append(point(path[j]));
                QJsonArray target;
                for(const auto &v:replies)
                {
                    bool found=false;
                    for(const auto &x:v.toObject()["witnessedStockStarts"].toArray())
                    {
                        if(work<=0)return out;
                        --work;
                        const auto p=x.toObject();const auto tile=p["property"].toArray();
                        if(tile.size()!=2 || (!target.isEmpty() && tile!=target))continue;
                        const bool sameLeg=p["unit"].toInt(-1)==actor.engineUnitId && p["from"].toArray()==point(a.origin) &&
                            p["to"].toArray()==point(b.destination) && p["cells"].toArray()==cells;
                        const bool nextCapture=p["unit"].toInt(-1)==actor.engineUnitId && p["from"].toArray()==point(a.destination) &&
                            p["to"].toArray()==point(b.destination) && tile==point(b.destination);
                        const bool match=repositioning ? nextCapture :
                            b.capture ? tile==point(b.destination) && (option==proposed[slot] || sameLeg) : sameLeg;
                        if(match){target=tile;found=true;break;}
                    }
                    if(!found){target={};break;}
                }
                QJsonObject action{{"slot",slot},{"unit",actor.engineUnitId},{"candidate",option},{"to",point(b.destination)},
                    {"captureNow",b.capture.has_value()},{"stockProposed",option==proposed[slot]},{"previousDestination",point(a.destination)}};
                if(alternativeRoute){
                    const auto &oldPath=a.source.bundle.path;
                    if(!witnessedPath || oldPath.empty() || oldPath.front()!=a.origin ||
                       oldPath.back()!=a.destination || !std::ranges::all_of(oldPath,outside))continue;
                    action["source"]="COMPETING_STOCK_ROUTE";
                    otherStops.append(action);
                }else if(finishesHere){
                    action["property"]=point(b.destination);
                    action["source"]="COMPLETE_NEUTRAL_CAPTURE_IN_PLACE";
                    otherCaptures.append(action);
                }else if(witnessedPath && !target.isEmpty() && !onDuty({target[0].toInt(),target[1].toInt()}) &&
                   (!secondThreat || outside({target[0].toInt(),target[1].toInt()}))){
                    action["property"]=target;
                    out.measures.append(action);
                }else if(escape && target==point(b.destination)){
                    // The standing team already prices this claimant's next
                    // capture. Contact at the old post needs a separate
                    // measured firing-opportunity bound before acceptance.
                    action["property"]=target;
                    action["source"]="CAPTURE_CLEAR_OF_ENEMY_FIRE";
                    otherCaptures.append(action);
                }else if(standingCandidate){
                    if(b.capture){
                        action["property"]=point(b.destination);
                        action["source"]="LEGAL_CAPTURE_FROM_WAIT";
                        otherCaptures.append(action);
                    }else{
                        action["stockDestination"]=point(stock.destination);
                        action["source"]="LEGAL_STOP_ON_STOCK_ROUTE";
                        otherStops.append(action);
                    }
                }
            }
        }
        // Price the originally witnessed teams first. An added option must
        // improve the actual accepted team, including earlier coassignments.
        for(const auto &option:otherCaptures)out.measures.append(option);
        for(const auto &option:otherStops)out.measures.append(option);
        return out;
    }
}
