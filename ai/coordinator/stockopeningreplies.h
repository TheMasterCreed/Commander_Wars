#pragma once

#include "ai/coordinator/stockprogress.h"
#include <set>

namespace Coordinator
{
    // Test the entire immediate response family. The first unpriced response
    // in a ten-turn trace does not describe its unvisited siblings.
    template<class Quote, class SameHq>
    QString checkStockOpeningReplies(const MasterAssignmentProblem &problem,const BattlefieldKnowledge &knowledge,
            CombatTerrain &terrain,
            std::span<const int> original,std::span<const int> selected,const QJsonArray &buys,TilePoint home,
            std::int64_t tariff,std::int64_t &work,Quote quote,SameHq sameHq,QJsonObject &receipt)
    {
        using Policy=std::pair<TacticalReply,int>;
        std::set<Policy> policies;
        for(auto policy:{TacticalReply::Economic,TacticalReply::InterruptCapture,TacticalReply::ArmorAndInfantry,
                         TacticalReply::ClearGuard,TacticalReply::PreserveCapture,TacticalReply::InfantryCapture})
            policies.emplace(policy,NO_UNIT);
        std::set<int> changedRows;
        std::vector<int> changed;
        for(const auto &actor:problem.actors())
        {
            const int slot=actor.canonicalSlot;
            policies.emplace(TacticalReply::TargetUnit,actor.engineUnitId);
            policies.emplace(TacticalReply::BypassUnit,actor.engineUnitId);
            if(original[slot]!=selected[slot]){
                changed.push_back(slot);
                changedRows.insert(actor.stockRow);
            }
        }
        std::vector<int> spentContainment;
        for(const auto &defender:terrain.observedDefenders){
            const auto &pawn=terrain.units[defender.knowledgeIndex];
            if(!defender.facts || !defender.facts->tile)return QStringLiteral("SPENT_DEFENDER_UNBOUND");
            if(!pawn.capper && pawn.grid>=0 && !pawn.ignoresCollision && defender.facts->tile->shooters.size()>=2)
                spentContainment.push_back(defender.engineUnitId);
            policies.emplace(TacticalReply::TargetUnit,defender.engineUnitId);
            policies.emplace(TacticalReply::BypassUnit,defender.engineUnitId);
        }
        if(terrain.orderedOpening || terrain.openingRecruitTarget)return QStringLiteral("ORDERED_OPENING_PREFIX_UNBOUND");
        auto birthSites=terrain.friendlyBirthSites;
        const auto priorPolicy=terrain.policy;
        const int priorTarget=terrain.replyTarget,priorShooter=terrain.containShooter;
        const auto priorFrom=terrain.containFrom;
        const auto restore=qScopeGuard([&]{
            terrain.policy=priorPolicy;
            terrain.replyTarget=priorTarget;
            terrain.containShooter=priorShooter;
            terrain.containFrom=priorFrom;
            terrain.friendlyBirthSites=std::move(birthSites);
        });
        std::set<std::pair<int,int>> births;
        for(const auto &v:buys){
            const auto xy=v.toObject()["factory"].toArray();
            if(xy.size()!=2)return QStringLiteral("FUNDED_BODY_UNBOUND");
            births.emplace(xy[0].toInt(),xy[1].toInt());
        }
        terrain.friendlyBirthSites=births;
        for(int i=0;i<int(terrain.units.size());++i)
        {
            const auto &u=terrain.units[i];
            if(!u.present)continue;
            if(!u.enemy)continue;
            if(u.capper)policies.emplace(TacticalReply::WaitOnProperty,u.engineUnitId);
            // Exposed newborns have additional ordered attack and movement
            // branches. Decline this comparison until they are priced.
            const int reach=std::max(u.movement,u.movementBeforeFuel)+std::max(1,knowledge.units()[i].maxRange);
            for(const auto &[x,y]:births)
                if(manhattanDistance(u.tile,{x,y})<=reach)return QStringLiteral("FUNDED_BODY_EXPOSURE_OPEN");
        }
        // Other possible F1 guards can differ from the one named in a priced
        // reply. Protect every geometrically reachable return path to the HQ.
        bool possibleReturnConflict=false;
        for(const auto &actor:problem.actors()){
            const auto &guard=terrain.units[actor.knowledgeUnitIndex];
            const int range=std::max(guard.movement,guard.movementBeforeFuel);
            const auto from=actor.candidates[original[actor.canonicalSlot]].destination;
            for(int slot:changed){
                const auto &a=problem.actors()[slot];
                for(auto pos:{a.candidates[original[slot]].destination,a.candidates[selected[slot]].destination}){
                    if(manhattanDistance(from,pos)+manhattanDistance(pos,home)<=range)
                        possibleReturnConflict=true;
                }
            }
        }
        const auto unaffected=[&](ContinuationKey key){
            std::erase_if(key,[&](const auto &action){return changedRows.contains(action.row());});
            return key;
        };
        const auto chipEqual=[](const auto &a,const auto &b){
            const auto &x=a.second,&y=b.second;
            return a.first==b.first && x.shooter==y.shooter && x.from==y.from && x.targetBefore==y.targetBefore &&
                x.targetAfter==y.targetAfter && x.damage==y.damage && x.counter==y.counter && x.foregoneCapture==y.foregoneCapture;
        };
        const auto threatEqual=[](const auto &a,const auto &b){
            return a.victim==b.victim && a.shooter==b.shooter && a.origins==b.origins;
        };
        // Match the ordinary generator on each actual post-F0 team. Its moving
        // actors are in native execution order, followed by previously spent defenders.
        const auto containmentTargets=[&](const ReplyPassEvaluation &economic){
            std::vector<int> result;
            for(const auto &actor:economic.actors){
                const auto &member=problem.actors()[actor.actorSlot];
                const auto &pawn=terrain.units[member.knowledgeUnitIndex];
                const auto &option=member.candidates[actor.candidateIndex];
                if(!pawn.capper && pawn.grid>=0 && !pawn.ignoresCollision && actor.stepsAfterOwnActions>0 &&
                   option.source.replyFacts && option.source.replyFacts->tile && option.source.replyFacts->tile->shooters.size()>=2)
                    result.push_back(actor.engineUnitId);
            }
            result.insert(result.end(),spentContainment.begin(),spentContainment.end());
            return result;
        };
        struct Reply {TacticalReply policy;int id;int opener{NO_UNIT};TilePoint from{INVALID_TILE};};
        std::vector<Reply> pending{{TacticalReply::Economic,NO_UNIT}};
        int checked=0,containmentChecks=0,alternateAnchors=0;
        QJsonArray containment;
        for(std::size_t index=0;index<pending.size();++index)
        {
            const auto [policy,id,opener,from]=pending[index];
            receipt={{"checked",checked},{"policies",int(pending.size())},{"policy",int(policy)},{"unit",id},
                     {"openerKnowledge",opener},{"firingPost",QJsonArray{from.x,from.y}},{"containment",containment}};
            if(work<2*tariff)return QStringLiteral("OPENING_REPLY_WORK_LIMIT");
            terrain.policy=policy;terrain.replyTarget=id;terrain.containShooter=opener;terrain.containFrom=from;
            work-=tariff;
            auto left=quote(original);
            if(work<tariff)return QStringLiteral("OPENING_REPLY_WORK_LIMIT");
            work-=tariff;
            auto right=quote(selected);
            if(!left.valued() || !right.valued())return QStringLiteral("NATIVE_OPENING_REPLY_UNAVAILABLE");
            if(left.shots!=right.shots || left.enemyAfterOwn!=right.enemyAfterOwn ||
               left.enemyReplyHp!=right.enemyReplyHp || left.realAfterReply!=right.realAfterReply ||
               left.servicedStockShots!=right.servicedStockShots || left.openingRecruitFight || right.openingRecruitFight ||
               !left.openingRecruitWindows.empty() || !right.openingRecruitWindows.empty() ||
               left.encirclement!=right.encirclement ||
               left.enemyWaiting!=right.enemyWaiting || left.replay.ordering.order!=right.replay.ordering.order ||
               left.actors.size()!=right.actors.size() || left.combatLocations.size()!=terrain.units.size() ||
               right.combatLocations.size()!=terrain.units.size() || left.exactNonStock!=right.exactNonStock ||
               !std::ranges::equal(left.captureChips,right.captureChips,chipEqual) ||
               !std::ranges::equal(left.lethalThreats,right.lethalThreats,threatEqual))
                return QStringLiteral("OPENING_REPLY_CHANGES_COMBAT");
            auto posts=left.combatLocations;
            for(int slot:changed){
                const auto &actor=problem.actors()[slot];
                if(posts[actor.knowledgeUnitIndex]!=actor.candidates[original[slot]].destination)
                    return QStringLiteral("OPENING_REPLY_DISPLACES_PROGRESS_UNIT");
                posts[actor.knowledgeUnitIndex]=actor.candidates[selected[slot]].destination;
            }
            if(posts!=right.combatLocations || unaffected(left.afterOwnActions)!=unaffected(right.afterOwnActions) ||
               unaffected(left.afterReply)!=unaffected(right.afterReply))return QStringLiteral("OPENING_REPLY_CHANGES_OTHER_JOB");
            for(std::size_t j=0;j<left.actors.size();++j){
                const auto &a=left.actors[j],&b=right.actors[j];
                if(a.actorSlot!=b.actorSlot || a.actionIndex!=b.actionIndex || a.ownShot!=b.ownShot ||
                   a.stepsAfterOwnActions!=b.stepsAfterOwnActions || a.stepsAfterReply!=b.stepsAfterReply ||
                   a.destroyedBy!=b.destroyedBy || a.survivorAmmo!=b.survivorAmmo)
                    return QStringLiteral("OPENING_REPLY_CHANGES_UNIT_RESOURCES");
            }
            bool needsCover=false;
            if(!sameHq(left,right,needsCover))return QStringLiteral("OPENING_REPLY_CHANGES_HEADQUARTERS");
            // Keep the conservative return-path guard on each reply that
            // actually needs cover. A unit within walking distance of its own
            // HQ does not acquire a duty on a board with no approaching capper.
            if(needsCover && possibleReturnConflict)return QStringLiteral("POSSIBLE_HEADQUARTERS_RETURN_CONFLICT");
            if(policy==TacticalReply::Economic){
                const auto targets=containmentTargets(left);
                if(targets!=containmentTargets(right))return QStringLiteral("OPENING_CONTAINMENT_MENU_CHANGED");
                for(int target:targets)pending.push_back({TacticalReply::ContainUnit,target});
                for(const auto &[other,unit]:policies)if(other!=TacticalReply::Economic)pending.push_back({other,unit});
            }
            if(policy==TacticalReply::ContainUnit){
                const auto &box=left.encirclement;
                if(box.target!=id || (from!=INVALID_TILE && (box.openerKnowledge!=opener || box.firingTile!=from)))
                    return QStringLiteral("OPENING_CONTAINMENT_ANCHOR_UNBOUND");
                // A sealed center has its own F1 escape/counterfire and E1
                // volley valuation. Equal E0 exits alone do not price it.
                if(box.sealed)return QStringLiteral("SEALED_CONTAINMENT_FOLLOWUP_UNBOUND");
                // Movement receipts contain the endpoint and cost. If a changed
                // occupant could lie on a participant's march, the full transit
                // has not been bound. Native ordinary steps each cost at least one.
                const auto clearMarch=[&](int mover,TilePoint post){
                    if(mover==NO_UNIT)return true;
                    const auto &pawn=terrain.units.at(mover);
                    if(post==pawn.tile)return true;
                    const int reach=std::max(pawn.movement,pawn.movementBeforeFuel);
                    for(int slot:changed){
                        const auto &member=problem.actors()[slot];
                        const auto old=member.candidates[original[slot]].destination;
                        const auto next=member.candidates[selected[slot]].destination;
                        if(old==next)continue;
                        for(auto at:{old,next})if(manhattanDistance(pawn.tile,at)+manhattanDistance(at,post)<=reach)return false;
                    }
                    return true;
                };
                if(!clearMarch(box.openerKnowledge,box.firingTile))return QStringLiteral("CONTAINMENT_OPENING_TRANSIT_UNBOUND");
                for(const auto &order:box.blockOrders)if(!clearMarch(order.unitIndex,order.destination))
                    return QStringLiteral("CONTAINMENT_BLOCK_TRANSIT_UNBOUND");
                if(from==INVALID_TILE && box.openerKnowledge!=NO_UNIT){
                    // The full ordered list of options, their costs and counterfire
                    // already compared equal above. Replay every other adjacent
                    // firing post with the same opener, as the ordinary ranker does.
                    for(const auto &option:box.firingOptions)if(option.tile!=box.firingTile && manhattanDistance(option.tile,box.center)==1)
                        pending.push_back({policy,id,box.openerKnowledge,option.tile});
                }
                ++containmentChecks;
                if(from!=INVALID_TILE)++alternateAnchors;
                auto physical=combatEncirclementRecord(box);
                physical["requestedPost"]=QJsonArray{from.x,from.y};
                containment.append(physical);
            }else if(left.encirclement.target!=NO_UNIT)return QStringLiteral("UNEXPECTED_OPENING_CONTAINMENT");
            ++checked;
        }
        receipt={{"checked",checked},{"policies",int(pending.size())},{"sameBuyer",true},
                 {"containmentChecks",containmentChecks},{"alternateAnchors",alternateAnchors},
                 {"containment",containment},{"fullContinuationPriced",false}};
        return {};
    }
}
