#pragma once

#include "ai/coordinator/townassaultledger.h"

namespace Coordinator
{
    // Conditional on the current initial orders and fixed purchases. One
    // owner decision keeps every already scheduled post and pays for no
    // additional defense. This is an upper on one ordinary-town initiative,
    // signed from our side; it never supplies a selected capture or rank.
    struct RetainedOwnerUpper
    {
        std::optional<MilliFunds> titleOnly,scopedTotal;
        QJsonObject details;
    };

    // Every choice below faces the same retained owner policy. The attacker
    // may choose any one of the generated dates, including declining. A
    // partial date cannot be replaced by a completed result on another date.
    inline QJsonValue retainedRosterDateUpper(int side,const QJsonArray &expected,const QJsonArray &observed)
    {
        if(expected.isEmpty())return QJsonValue(QJsonValue::Null);
        std::map<int,MilliFunds> values;
        for(const auto &v:observed)
        {
            const auto r=v.toObject();
            if(!r["completeOnRetainedRoster"].toBool() || !r["stockReplyScore"].isDouble())continue;
            const int tick=r["entry"].toInt(-1);const auto score=r["stockReplyScore"].toInteger();
            if(auto old=values.find(tick);old!=values.end() && old->second!=score)return QJsonValue(QJsonValue::Null);
            values[tick]=score;
        }
        MilliFunds best=0;
        for(const auto &v:expected)
        {
            const auto row=values.find(v.toInt(-1));if(row==values.end())return QJsonValue(QJsonValue::Null);
            if(TownAssaultDetail::better(row->second,best,side))best=row->second;
        }
        return QJsonValue(static_cast<qint64>(best));
    }

    template<class Timing>
    RetainedOwnerUpper retainedOwnerTownUpper(const TownAssaultBoard &b,const ProductionProperty &property,
        int side,std::span<const TilePoint> initial,std::span<const double> initialHp,const StockRouteLowerBounds &bounds,
        std::span<const MilliFunds> cash,int cashCutoff,Timing timing,std::int64_t &work)
    {
        using namespace TownAssaultDetail;
        RetainedOwnerUpper result;
        if(property.column.ownerBefore!=(side?OwnerSign::Ours:OwnerSign::Enemy) || property.existingCompletion>=0 ||
           !property.ownershipChanges.empty() || initial.size()!=b.units.size() || initialHp.size()!=b.units.size())return result;
        const auto town=property.tile;const int sq=cell(b.terrain,town),n=static_cast<int>(b.units.size()),first=2+side;
        int earliest=b.cutoff+1,unrestricted=b.cutoff+1;
        QJsonArray sources,affected,posts;
        bool unknownEntrant=false;
        const auto entry=[&](TilePoint origin,const MobilityCostGrid *grid,int movement,int ready)->int {
            if(ready>b.cutoff || origin==town)return ready;
            if(!grid || !grid->onGrid(origin.x,origin.y))return ready; // grant an immediate arrival on missing geometry
            if(movement<=0)return b.cutoff+1;
            const auto *dist=bounds.preparedCost(*grid,sq);
            int floor=0;
            if(const auto cache=b.terrain.movementCache;cache && cache->minimumEntryCost.contains(grid))
                floor=cache->minimumEntryCost.at(grid);
            const auto cost=dist?std::int64_t((*dist)[cell(b.terrain,origin)]):std::int64_t(floor)*manhattanDistance(origin,town);
            if(cost==std::numeric_limits<int>::max() || cost>std::int64_t(b.cutoff+1)*movement)return b.cutoff+1;
            return ready+2*static_cast<int>(std::max<std::int64_t>(0,(cost+movement-1)/movement-1));
        };
        const auto admit=[&](int ready,TilePoint pos,const MobilityCostGrid *grid,int movement,
                int rate,int carry,bool displaced,QJsonObject id){
            const auto begin=entry(pos,grid,movement,ready);
            if(rate<=0 || begin>b.cutoff){if(displaced)affected.append(id);return;}
            const auto finish=begin+2*(widenedCaptureTurns(carry,rate,b.threshold)-1);
            if(finish>b.cutoff){if(displaced)affected.append(id);return;}
            earliest=std::min<int>(earliest,static_cast<int>(finish));
            id["earliestGrantedEntry"]=static_cast<int>(begin);id["earliestGrantedFinish"]=static_cast<int>(finish);
            id["maxCaptureRate"]=rate;
            if(displaced)affected.append(id);
            sources.append(id);
        };
        for(int i=0;i<n;++i)
        {
            if(work<=0)return result;--work;
            const auto &p=b.terrain.units[i];const auto &u=b.units[i];
            if(int(p.enemy)!=side || !u.canCapture)continue;
            if(u.carrier!=KnownUnit::NO_CARRIER && u.hpRounded>0){unknownEntrant=true;unrestricted=first;continue;}
            if(!p.present || initialHp[i]<=0)continue;
            unrestricted=std::min(unrestricted,first);
            const int idx=p.travelGrid>=0?p.travelGrid:p.grid;
            const auto *grid=idx<0?nullptr:&b.terrain.grids[idx];
            const auto &r=b.rights[i];
            const bool spent=r.readyTick<0 || r.readyTick>first || r.lastFreeTick<b.cutoff ||
                r.mobileFrom>first || r.displacedValue!=0 || b.positions[i]!=initial[i];
            // Even a damaged soldier may repair before arrival. A soldier
            // already on the town gets the most favorable possible carry.
            admit(first,initial[i],grid,p.movement,u.captureRate+std::max(0,10-u.hpRounded),
                  initial[i]==town?b.threshold-1:0,spent,QJsonObject{{"unit",p.engineUnitId}});
        }
        for(int k=0;k<static_cast<int>(b.recruits.size());++k)
        {
            if(work<=0)return result;--work;
            const auto &g=b.recruits[k];if(g.side!=side || g.buildTick+2>b.cutoff)continue;
            if(!g.unit){unknownEntrant=true;unrestricted=std::min(unrestricted,g.buildTick+2);continue;}
            const auto &u=*g.unit;const auto rate=u.captureRate.find(sq);if(rate==u.captureRate.end())continue;
            const int ready=g.buildTick+2;
            unrestricted=std::min(unrestricted,ready);
            admit(ready,g.factory,u.movementGrid.get(),u.movement,rate->second,0,
                !b.recruitFree || !b.recruitFree(k,ready,b.cutoff),
                QJsonObject{{"factory",QJsonArray{g.factory.x,g.factory.y}},{"buildTick",g.buildTick},{"type",u.type}});
        }
        if(unknownEntrant)earliest=std::min(earliest,unrestricted);
        TownAssaultLine optimistic;optimistic.side=side;optimistic.town=town;optimistic.finishTick=earliest<=b.cutoff?earliest:-1;
        if(work<cashCutoff+1)return result;work-=cashCutoff+1;
        const auto price=townAssaultLedger(optimistic,property.column,cash,cashCutoff,timing);
        if(!price)return result;
        // Stock movement can recover with refuelling. Native capture bonuses
        // depend on the property and may differ from the source-square rate.
        // The tighter frozen-rule date is stated separately. For an absolute
        // material/income limit grant even an instant full capture at the next
        // possible activation. A booked unit's opportunity is never declared free.
        auto freeEntry=optimistic;freeEntry.finishTick=unrestricted<=b.cutoff?unrestricted:-1;
        if(work<cashCutoff+1)return result;work-=cashCutoff+1;
        const auto absolute=townAssaultLedger(freeEntry,property.column,cash,cashCutoff,timing);
        if(!absolute)return result;
        result.titleOnly=absolute->score;
        // A defender may be cleared on its retained post, even when no
        // capper can earn another payday. A town garrison can admit an
        // additional kill to open the firing lane; never cap that at one unit.
        bool garrison=false,dependent=false,unknownCapital=false;
        std::map<int,MilliFunds> largestAt;
        std::set<std::tuple<int,int,int>> unique;
        for(const auto v:std::array<TilePoint,5>{{{0,0},{-1,0},{0,-1},{1,0},{0,1}}})
        {
            const TilePoint at{town.x+v.x,town.y+v.y};
            if(at.x<0 || at.y<0 || at.x>=b.terrain.width || at.y>=b.terrain.height)continue;
            for(const auto &use:b.traffic.uses(cell(b.terrain,at)))
            {
                if(work<=0)return {};
                --work;
                if(!use.stopped || use.side==side)continue;
                int when=std::max(first,use.first);when+=int(when%2!=side);
                if(when>std::min(b.cutoff,use.last) || !unique.insert({use.actor,when,cell(b.terrain,at)}).second)continue;
                QJsonObject p{{"at",QJsonArray{at.x,at.y}},{"first",when},{"through",use.last}};
                if(use.actor>=0 && use.actor<n)
                {
                    p["unit"]=b.terrain.units[use.actor].engineUnitId;
                    p["fullCapital"]=static_cast<qint64>(b.terrain.units[use.actor].replacementCost);
                }
                else if(use.actor>=n && use.actor-n<static_cast<int>(b.recruits.size()))
                {
                    const auto &g=b.recruits[use.actor-n];p["factory"]=QJsonArray{g.factory.x,g.factory.y};p["buildTick"]=g.buildTick;
                    if(g.unit)p["fullCapital"]=static_cast<qint64>(g.unit->cost);
                }
                const auto cost=p["fullCapital"].toInteger(-1);
                if(cost<0)unknownCapital=true;else largestAt[when]=std::max<MilliFunds>(cost,largestAt[when]);
                garrison|=at==town;dependent|=use.actor<0 || use.actor>=n || !b.canRemove || !b.canRemove(use.actor,when);
                posts.append(p);
            }
        }
        std::optional<MilliFunds> singleScreen=std::nullopt;
        if(!unknownCapital && !garrison)
        {
            singleScreen=0;
            for(const auto &[tick,amount]:largestAt)
            {
                if(work<cashCutoff+2){singleScreen.reset();break;}work-=cashCutoff+2;
                TownAssaultLine loss;loss.side=side;loss.losses={{tick,side?amount:-amount}};
                const auto gross=townAssaultLedger(loss,property.column,cash,cashCutoff,timing);
                if(!gross){singleScreen.reset();break;}
                if(better(gross->score,*singleScreen,side))singleScreen=gross->score;
            }
        }
        if(posts.isEmpty() && affected.isEmpty() && !unknownEntrant)result.scopedTotal=absolute->score;
        result.details={{"policy","OWNER_RETAINS_EXISTING_POSTS_AND_WITHHOLDS_OPTIONAL_SHOTS"},
            {"attackingSide",side},{"earliestOptimisticFinish",optimistic.finishTick},
            {"frozenMovementAndCaptureRateTitleUpper",static_cast<qint64>(price->score)},
            {"firstActivationGrant",freeEntry.finishTick},
            {"titleOnlySignedCeiling",static_cast<qint64>(*result.titleOnly)},{"entrants",sources},
            {"displacedOrProtectedEntrants",affected},{"possibleClearingTargets",posts},
            {"unknownOrTransportedEntrant",unknownEntrant},{"occupiedTownMayRequireAdditionalClear",garrison},
            {"oneAdjacentScreenMaterialUpper",singleScreen?QJsonValue(static_cast<qint64>(*singleScreen)):QJsonValue(QJsonValue::Null)},
            {"clearingMayDisplaceExistingReceipt",dependent},
            {"scope","ONE_ORDINARY_TOWN_ON_FIXED_PURCHASE_PORTFOLIO"},
            {"scopedSignedCeiling",result.scopedTotal?QJsonValue(static_cast<qint64>(*result.scopedTotal)):QJsonValue(QJsonValue::Null)}};
        return result;
    }
}
