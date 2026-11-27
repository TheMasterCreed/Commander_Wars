#pragma once

#include "ai/coordinator/stockprogress.h"

namespace Coordinator
{
    // Bound immediate differences for a capturer leaving an attackable
    // stationary post. The caller separately compares complete team stock,
    // native purchases and defended HQ duties before publishing an open-rank
    // economic preference.
    inline QJsonObject stockEscapeBound(const CombatTerrain &terrain,const BattlefieldKnowledge &knowledge,
        std::span<const PropertyFacts> properties,int escaped,TilePoint oldPost,TilePoint newPost,
        std::span<const std::pair<int,TilePoint>> newTeam,const QJsonArray &purchases,
        const QJsonArray &stockTrips,int horizon,std::int64_t &work)
    {
        const auto gap=[](const char *reason){return QJsonObject{{"status","OPEN"},{"reason",reason}};};
        if(terrain.units.size()!=knowledge.units().size() || newTeam.empty())return gap("INCOMPLETE_F0_ARMY");
        const auto unit=std::ranges::find(terrain.units,escaped,&CombatPawn::engineUnitId);
        if(unit==terrain.units.end() || unit->tile!=oldPost || unit->enemy || !unit->capper)return gap("ESCAPING_ACTOR_UNBOUND");
        const auto qi=std::distance(terrain.units.begin(),unit);
        const auto &source=knowledge.units()[qi];
        const int ownMove=unit->movementBeforeFuel,ownRange=std::max(1,source.maxRange);
        if(ownMove<0 || source.hpRounded!=UNIT_HP_STEPS)return gap("ESCAPING_ACTOR_CAPABILITY_UNKNOWN");
        const int ownGrid=unit->travelGrid>=0?unit->travelGrid:unit->grid;
        if(ownGrid<0 || ownGrid>=int(terrain.grids.size()))return gap("ESCAPING_MOVEMENT_UNKNOWN");
        const auto &own=terrain.grids[ownGrid];
        const auto scan=4LL*own.width()*own.height();if(scan<=0 || scan>work)return gap("ESCAPE_WORK_LIMIT");
        work-=scan;
        for(int y=0;y<own.height();++y)for(int x=0;x<own.width();++x)for(auto dir:ENTRY_DIRECTIONS)
            if(own.cost(x,y,dir)==0)return gap("ZERO_COST_F1_MOVEMENT_UNBOUND");
        for(const auto &p:properties)if(TilePoint{p.x,p.y}==oldPost &&
            (p.capturable || p.canProduce || p.baseIncome>0 || p.objective==ObjectiveKind::Headquarters))
            return gap("VACATED_SITE_HAS_AN_ECONOMIC_OR_HQ_DUTY");
        const auto ownNear=[&](TilePoint t){return manhattanDistance(oldPost,t)<=ownMove+ownRange;};
        for(const auto &p:properties)if(p.canProduce && p.ownerId!=knowledge.observerId() && ownNear({p.x,p.y}))
            return gap("UNPRICED_F1_SHOT_ON_A_NEW_RECRUIT");
        std::vector<TilePoint> protectedPosts;
        std::set<int> friendly;
        for(const auto &[i,at]:newTeam){
            if(i<0 || i>=int(terrain.units.size()) || terrain.units[i].enemy || !friendly.insert(i).second)
                return gap("INVALID_F0_SURVIVOR");
            protectedPosts.push_back(at);
            if(i!=qi && knowledge.units()[i].maxRange>1)
                return gap("INDIRECT_ALLY_MAY_NEED_THE_NEW_FIRING_POST");
        }
        if(!friendly.contains(int(qi)))return gap("ESCAPING_ACTOR_MISSING");
        for(const auto &p:purchases){
            const auto at=p.toObject()["factory"].toArray();
            if(at.size()!=2)return gap("UNBOUND_OPENING_BIRTH");
            protectedPosts.push_back({at[0].toInt(),at[1].toInt()});
        }
        MilliFunds maximumLostShot=0;
        QJsonArray possibleOldTargets,finishing,outstandingStock;
        for(int i=0;i<int(terrain.units.size());++i){
            const auto &pawn=terrain.units[i];const auto &enemy=knowledge.units()[i];
            if(!pawn.present)return gap("UNBOUND_ROSTER");
            if(!pawn.enemy){
                if(!friendly.contains(i))return gap("UNBOUND_ALLIED_BODY");
                continue;
            }
            const int grid=pawn.travelGrid>=0?pawn.travelGrid:pawn.grid;
            const int move=std::max(pawn.movement,pawn.movementBeforeFuel),range=std::max(1,enemy.maxRange);
            if(grid<0 || grid>=int(terrain.grids.size()) || move<0 || pawn.supportsUnits || pawn.transportCapacity ||
               enemy.hpRounded!=UNIT_HP_STEPS || pawn.replacementCost<0)return gap("UNSUPPORTED_HOSTILE_CAPABILITY");
            if(work<1+std::int64_t(properties.size())+std::int64_t(protectedPosts.size()))return gap("ESCAPE_WORK_LIMIT");
            work-=1+std::int64_t(properties.size())+std::int64_t(protectedPosts.size());
            const auto &g=terrain.grids[grid];
            std::optional<DistanceField> field;
            const auto prepare=[&](){
                if(field)return true;
                const auto charge=6LL*g.width()*g.height();
                if(charge<=0 || charge>work)return false;
                work-=charge;field.emplace(DistanceField::build(g,{pawn.tile},FieldExpansion::FromSources));
                return true;
            };
            for(const auto post:protectedPosts){
                if(manhattanDistance(pawn.tile,post)>move+range)continue;
                if(!prepare())return gap("ESCAPE_WORK_LIMIT");
                for(int y=std::max(0,post.y-range);y<=std::min(g.height()-1,post.y+range);++y)
                    for(int x=std::max(0,post.x-range);x<=std::min(g.width()-1,post.x+range);++x){
                        if(work<=0)return gap("ESCAPE_WORK_LIMIT");
                        --work;
                        const int distance=field->distance(x,y);
                        if(manhattanDistance(post,{x,y})<=range && distance>=0 && distance<=move)
                            return gap("E0_HOSTILE_REACHES_AN_ALLIED_BODY_OR_BIRTH");
                    }
            }
            for(const auto &p:properties){
                const TilePoint at{p.x,p.y};
                // A newly reachable route can differ only if it uses the
                // formerly occupied cell. Allow all other blockers to vacate.
                // Positive entry costs were certified during offer collection.
                if((p.capturable || p.canProduce || p.objective==ObjectiveKind::Headquarters) &&
                    manhattanDistance(pawn.tile,oldPost)+manhattanDistance(oldPost,at)<=move){
                    if(!prepare())return gap("ESCAPE_WORK_LIMIT");
                    const int before=field->distance(oldPost.x,oldPost.y);
                    if(before>=0 && before+manhattanDistance(oldPost,at)<=move)return gap("VACANCY_MAY_CHANGE_AN_ENEMY_PROPERTY_OR_BUY");
                }
                if(p.capturerIndex!=i || p.capturePoints<=0)continue;
                if(p.captureTurnsRemaining!=1 && ownNear(at))return gap("F1_FIRE_MAY_INTERRUPT_AN_ACTIVE_CLAIM");
                if(p.captureTurnsRemaining==1){
                    if(p.objective==ObjectiveKind::Headquarters)return gap("ACTIVE_ENEMY_HQ_CLAIM");
                    finishing.append(QJsonArray{at.x,at.y});
                }
            }
            if(ownNear(pawn.tile)){
                possibleOldTargets.append(QJsonObject{{"unit",pawn.engineUnitId},{"wholeAsset",qint64(pawn.replacementCost)}});
                maximumLostShot=std::max(maximumLostShot,pawn.replacementCost);
                if(pawn.capper && horizon>1){
                    // A wounded or lost capper can change future title dates.
                    // Its replacement price does not cap those lost paydays.
                    // Prefer the actual dated job when the stock price exposed
                    // one, then keep a physically possible unscheduled job open.
                    QJsonObject job;
                    for(const auto &v:stockTrips){
                        if(work<=0)return gap("ESCAPE_WORK_LIMIT");--work;
                        const auto row=v.toObject();const auto tile=row["tile"].toArray();
                        if(!row["enemy"].toBool() || row["unit"].toInt(-1)!=pawn.engineUnitId ||
                           tile.size()!=2 || TilePoint{tile[0].toInt(),tile[1].toInt()}==pawn.tile)continue;
                        if(row["departureTick"].toInt(-1)<2)continue;
                        job={{"unit",pawn.engineUnitId},{"site",tile},{"departureTick",row["departureTick"]},
                             {"priceStatus",row["status"]}};break;
                    }
                    if(job.isEmpty())for(const auto &site:properties){
                        const TilePoint t{site.x,site.y};
                        if(!site.capturable || site.ownerId==enemy.ownerId || t==pawn.tile ||
                           (site.baseIncome<=0 && !site.canProduce && site.objective!=ObjectiveKind::Headquarters) ||
                           std::int64_t(manhattanDistance(t,pawn.tile))>std::int64_t(move)*(horizon-1))continue;
                        job={{"unit",pawn.engineUnitId},{"site",QJsonArray{t.x,t.y}},{"priceStatus","FUTURE_TITLE_OR_PAYDAY_UNBOUNDED"}};break;
                    }
                    if(!job.isEmpty())outstandingStock.append(job);
                }
            }
        }
        if(!outstandingStock.isEmpty())return {{"status","OPEN"},{"reason","F1_FIRE_MAY_DELAY_ENEMY_STOCK"},
            {"escaped",escaped},{"oldF1Targets",possibleOldTargets},{"inPlaceFinishes",finishing},
            {"lostF1MaterialUpper",qint64(maximumLostShot)},{"unpricedEconomicConsequences",outstandingStock},
            {"fullHorizonPriced",false}};
        // In the old upper the opponent can complete in-place claims, hold
        // its other pieces and decline every optional shot. This grants the
        // old standing actor one entire accessible target on F1 and charges
        // the challenger for losing that opportunity. No favorable counter
        // or abandoned enemy claim is guaranteed by the incumbent.
        return {{"status","BOUNDED_E0_ESCAPE"},{"oldReply","COMPLETE_IN_PLACE_CLAIMS_AND_DECLINE_FIRE"},
            {"escaped",escaped},{"from",QJsonArray{oldPost.x,oldPost.y}},{"to",QJsonArray{newPost.x,newPost.y}},
            {"preservedSubjects",int(protectedPosts.size())},{"inPlaceFinishes",finishing},{"oldF1Targets",possibleOldTargets},
            {"lostF1Upper",qint64(maximumLostShot)},{"fullHorizonPriced",false}};
    }

    inline bool isolatedStockEscapeExchange(const ReplyPassEvaluation &old,const ReplyPassEvaluation &next,int id)
    {
        if(!old.valued() || !next.valued() || !next.shots.empty() || old.actors.size()!=next.actors.size() ||
           old.replay.ordering.order!=next.replay.ordering.order)return false;
        for(const auto &shot:old.shots)if(shot.targetEngineUnitId!=id)return false;
        for(std::size_t i=0;i<old.actors.size();++i){
            const auto &a=old.actors[i],&b=next.actors[i];
            if(a.engineUnitId!=b.engineUnitId || a.actorSlot!=b.actorSlot || a.dayStartSteps!=b.dayStartSteps ||
               a.stepsAfterOwnActions!=b.stepsAfterOwnActions || a.ownShot!=b.ownShot ||
               a.terms.ownCounterCorrection!=b.terms.ownCounterCorrection)return false;
            if(a.engineUnitId!=id && (a.stepsAfterReply!=b.stepsAfterReply || a.destroyedBy!=b.destroyedBy ||
               a.survivorAmmo!=b.survivorAmmo || a.terms!=b.terms))return false;
            if(a.engineUnitId==id && b.stepsAfterReply!=b.stepsAfterOwnActions)return false;
        }
        return true;
    }
}
