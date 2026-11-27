#pragma once

#include "ai/coordinator/stockcapturevalidation.h"
#include "ai/coordinator/relativestockcomparison.h"
#include "ai/coordinator/townassault.h"

namespace Coordinator
{
    inline TownWithdrawalJob unpricedTownWithdrawal(const CombatTerrain &terrain,std::span<const KnownUnit> units,
        std::span<const ProductionProperty> properties,const StockCaptureSettlement &settled,const ProductionProjection &production,
        std::span<const TilePoint> positions,std::span<const TownAssaultRight> rights,
        int actor,TilePoint town,int tick,double hp,int cutoff,int lastPayday,int threshold,const StockCaptureTraffic &traffic,
        std::int64_t &work)
    {
        if(actor<0 || actor>=static_cast<int>(units.size()) || actor>=static_cast<int>(rights.size()))
            return {"UNPRICED_RECRUIT_REASSIGNMENT",town,NO_UNIT,tick};
        if(tick>cutoff || hp<=0)return {};
        const auto &pawn=terrain.units[actor];
        const int id=pawn.engineUnitId,side=int(pawn.enemy);
        const auto origin=positions[actor];
        const auto limit=[&](TilePoint p){return TownWithdrawalJob{"WITHDRAWAL_JOB_WORK_LIMIT",p,id,tick};};
        if(rights[actor].displacedValue!=0)return {"UNPRICED_DISPLACED_CAPTURE_JOB",origin,id,tick};
        for(auto tile:{origin,town})
        {
            if(tile==INVALID_TILE)continue;
            const int cell=tile.y*terrain.width+tile.x;
            if(const auto it=settled.townPostDependencies.find(cell);it!=settled.townPostDependencies.end())
                for(const auto &d:it->second)
                {
                    if(work<=0)return limit(tile);
                    --work;
                    if(d.claimant==actor || (d.holder!=actor && d.holder!=NO_UNIT) ||
                       (d.occupiedTick>=0 && d.occupiedTick<tick-2))continue;
                    const int claimant=d.claimant>=0 && d.claimant<static_cast<int>(units.size())?
                        terrain.units[d.claimant].engineUnitId:NO_UNIT;
                    return {"UNPRICED_RELEASED_STOCK_ROUTE",d.target,claimant,d.occupiedTick};
                }
        }
        for(const auto &g:production.fundedGuns)
        {
            if(work<=0)return limit(origin);
            --work;
            if(g.targetUnit==actor && g.impactTick>=tick)
                return {"UNPRICED_DISPLACED_PAID_RESPONSE",g.targetTile,id,g.impactTick};
            if(g.side==side && g.buildTick>=tick-2 && (g.factory==origin || g.factory==town))
                return {"UNPRICED_RELEASED_FUNDED_SITE",g.factory,id,g.buildTick};
        }
        // Releasing a facility owned by this side may change its real buyer.
        // A retained bank sufficient for the menu is evidence of an option,
        // not a claim that a new unit is bought or has any useful job.
        if(terrain.production)for(const auto &site:terrain.production->sites)
        {
            if(work<=0)return limit(origin);
            --work;
            if(site.tile!=origin || site.minimumBuildCost[side]<=0)continue;
            if(work<static_cast<std::int64_t>(properties.size()))return limit(origin);
            work-=static_cast<std::int64_t>(properties.size());
            const auto property=std::find_if(properties.begin(),properties.end(),[&](const auto &p){return p.tile==origin;});
            if(property==properties.end() || productionOwnerAt(*property,tick)!=(side?OwnerSign::Enemy:OwnerSign::Ours))continue;
            auto bank=terrain.production->funds[side];int lastBuy=0;
            for(const auto &item:production.orders)
            {
                if(work<=0)return limit(origin);
                --work;
                const auto order=item.toObject();const int at=order["buildHalfTurn"].toInt(-1);
                if(int(order["enemy"].toBool())==side && at>=lastBuy && at<=tick &&
                   order.contains("treasuryAfter")){bank=order["treasuryAfter"].toInteger();lastBuy=at;}
            }
            for(int at=lastBuy+1;at<=tick;++at)if(at%2==side)for(const auto &p:properties)
            {
                if(work<1+static_cast<std::int64_t>(p.ownershipChanges.size()))return limit(origin);
                work-=1+static_cast<std::int64_t>(p.ownershipChanges.size());
                if(productionOwnerAt(p,at)!=(side?OwnerSign::Enemy:OwnerSign::Ours))continue;
                const auto income=side?p.column.income.enemyPerTurn:p.column.income.oursPerTurn;
                if(!RelativeStockDetail::checkedAdd(bank,income))return limit(origin);
            }
            if(terrain.production->availableUnits[side]>0 && bank>=site.minimumBuildCost[side])
                return {"UNPRICED_AFFORDABLE_FACTORY_RELEASE",origin,id,tick};
            // Later paydays, casualties and rival use of the same wallet
            // have not been replayed for this changed production lane.
            if(tick+2<=cutoff)
            {
                auto upper=std::max(bank,terrain.production->funds[side]);
                for(int at=tick+2;at<=cutoff;at+=2)for(const auto &p:properties)
                {
                    if(work<=0)return limit(origin);
                    --work;
                    if(!RelativeStockDetail::checkedAdd(upper,side?p.column.income.enemyPerTurn:p.column.income.oursPerTurn))
                        return limit(origin);
                }
                if(upper>=site.minimumBuildCost[side])return {"UNVERIFIED_LATER_FACTORY_RELEASE",origin,id,tick};
            }
        }
        // Earlier than a capper's own next installment, no later transfer
        // can affect a payday. Already-held paydays cannot be earned again.
        const auto myOwner=side?OwnerSign::Enemy:OwnerSign::Ours;
        const auto affectsIncome=[&](const ProductionProperty &p,int finish){
            for(int t=finish+1;t<=lastPayday;++t)
            {
                const auto owner=productionOwnerAt(p,t);
                if(owner==myOwner)continue;
                const auto income=t%2?p.column.income.enemyPerTurn:p.column.income.oursPerTurn;
                if(income>0 && (t%2==side || owner!=OwnerSign::Neutral))return true;
            }
            return false;
        };
        const ProductionProperty *nearest=nullptr;
        for(const auto &p:properties)
        {
            const auto effort=std::int64_t(1+lastPayday-tick)*(1+static_cast<std::int64_t>(p.ownershipChanges.size()));
            if(work<effort)return limit(p.tile);
            work-=effort;
            if(p.tile==town || !affectsIncome(p,tick))continue;
            if(!nearest || manhattanDistance(town,p.tile)<manhattanDistance(town,nearest->tile))nearest=&p;
        }
        if(!nearest)return {};
        const auto at=nearest->tile;
        const auto &u=units[actor];
        const int rate=replyCaptureRate(u.captureRate,u.hpRounded-ceilHpSteps(hp),false);
        if(pawn.grid>=0 && pawn.grid<static_cast<int>(terrain.grids.size()) && rate>0 && pawn.movement>0)
        {
            const int turns=widenedCaptureTurns(0,rate,threshold);
            const int latest=std::min(cutoff-2*(turns-1),rights[actor].lastFreeTick-2*(turns-1));
            if(latest>=tick)
            {
                const auto path=stockCaptureRoute(terrain.grids[pawn.grid],traffic,actor,side,town,at,pawn.movement,
                    rights[actor].fuel,tick,tick,latest,turns,work);
                if(!path)return limit(at);
                if(path->entryTick>=tick && affectsIncome(*nearest,path->entryTick+2*(turns-1)))
                    return {"ALTERNATE_CAPTURE_ROUTE_NEEDS_PRICE",at,id,path->entryTick};
            }
        }
        // Initial movement may be fuel-trimmed or a future assignment may
        // itself have to yield. Failure above does not establish a closed job.
        return {"ALTERNATE_TITLE_TIMING_UNVERIFIED",at,id,tick};
    }
}
