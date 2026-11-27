#pragma once

#include "ai/coordinator/stockcapturevalidation.h"
#include "ai/coordinator/townassaultledger.h"

namespace Coordinator
{
    // Price only the marginal of the named original's subsequent initiative,
    // conditional on the specified completed lethal exchange. Prefix costs and
    // unrelated continuations are shared by both choices and are never a team price.
    template<class Timing> QJsonArray priceNamedStockCasualty(const StockCaptureSettlement &settled,
        const TownAssaultBoard &board,const TownAssaultTarget &target,int cutoff,std::span<const MilliFunds> cash,
        StockRouteLowerBounds &bounds,Timing timing,std::int64_t &work)
    {
        QJsonArray answers;
        if(target.requiredCapper==NO_UNIT || target.occupiedFrom<0 || target.occupiedFrom!=target.occupiedThrough ||
           target.occupiedFrom>board.cutoff || cutoff<board.cutoff || cash.empty() || cutoff>2*std::int64_t(cash.size()) ||
           board.hp.size()!=board.units.size() || board.rights.size()!=board.units.size() || board.terrain.units.size()!=board.units.size())return answers;
        for(const auto &d:settled.attemptedCasualties)
        {
            if(work<=0)break;
            --work;
            if(d.actor<0 || d.actor>=static_cast<int>(board.units.size()) || d.proof<0 || d.proof>=settled.proofs.size() ||
               board.terrain.units[d.actor].engineUnitId!=target.requiredCapper || d.side!=1-target.defendingSide ||
               d.target!=target.property.tile || d.death<0 || d.death>=target.occupiedFrom || d.requested>=target.occupiedFrom)continue;
            const auto receipt=settled.proofs[d.proof].toObject();const auto shots=receipt["shots"].toArray();
            const auto post=QJsonArray{d.post.x,d.post.y};
            if(receipt["status"]!="DECLINED_EXPOSED_APPROACH" || receipt["unit"].toInt(-1)!=target.requiredCapper ||
               receipt["tile"]!=QJsonArray{d.target.x,d.target.y} || shots.isEmpty() ||
               receipt["proposedEntryTick"].toInt(-1)!=d.requested || receipt["enemy"].toBool()!=bool(d.side))continue;
            const auto hit=shots.last().toObject();
            if(hit["targetTile"]!=post || hit["tick"].toInt(-1)!=d.death || hit["hpBefore"].toDouble(0)<=0 ||
               hit["hpAfter"].toDouble(-1)!=0)continue;
            QJsonObject branch{{"stockCapper",target.requiredCapper},{"stockProposedEntry",d.requested},
                {"stockAttempt",receipt["routeAttempt"]},{"tile",receipt["tile"]},{"stockStops",receipt["stops"]},
                {"stockShots",shots},{"claimantDeath",d.death},{"claimantPost",post},{"namedEntry",target.occupiedFrom},
                {"scope","NAMED_ORIGINAL_INITIATIVE_AFTER_THIS_STOCK_EXCHANGE"},
                {"ifStockPasses","RETAINED_BOARD_REQUIRES_ITS_OWN_COMPARISON"}};
            const auto n=static_cast<std::int64_t>(board.hp.size());
            const auto cost=2*n+shots.size()+4*(1+static_cast<std::int64_t>(cash.size())+cutoff);
            if(work<cost){branch["ifStockAttempts"]="CASUALTY_CONTINUATION_WORK_LIMIT";answers.append(branch);break;}
            work-=cost;
            TownAssaultLine prefix;prefix.side=d.side;prefix.town=d.target;
            for(const auto &v:shots){const auto q=v.toObject();prefix.losses.emplace_back(q["tick"].toInt(),q["newSignedLoss"].toInteger());}
            const auto timeline=townAssaultLedger(prefix,target.property,cash,cutoff,timing);
            if(!timeline){branch["ifStockAttempts"]="PREFIX_LEDGER_UNPRICED";answers.append(branch);continue;}
            auto after=board;
            std::vector<double> hp(board.hp.begin(),board.hp.end());hp[d.actor]=0;after.hp=hp;
            std::vector<TownAssaultRight> rights(board.rights.begin(),board.rights.end());rights[d.actor].readyTick=-1;after.rights=rights;
            // Grant an empty objective at every remaining activation. No other
            // capturer can earn the named original's own action, even if garrisons,
            // reinforcement purchases or a different job change elsewhere.
            auto option=target;option.occupant=NO_UNIT;option.occupiedThrough=after.cutoff;
            const auto value=[&](const TownAssaultLine &line)->std::optional<MilliFunds>{
                const auto delta=townAssaultLedger(line,target.property,timeline->flow,cutoff,timing);
                return delta?std::optional<MilliFunds>{delta->score}:std::nullopt;
            };
            TownAssaultProgress progress;
            const auto result=priceTownAssault(after,option,bounds,value,work,&progress);
            if(progress.complete && !result.workLimited && !result.unresolved && !result.assault && !result.response &&
               result.checkedCappers.empty() && result.intentions.empty() && result.declineValue==0)
            {
                branch["ifStockAttempts"]="DEAD_ORIGINAL_HAS_ONLY_PASS";
                branch["marginalLower"]=static_cast<qint64>(result.declineValue);
                branch["marginalUpper"]=static_cast<qint64>(result.declineValue);
                branch["throughHalfTurn"]=cutoff;
            }
            else branch["ifStockAttempts"]="CASUALTY_CONTINUATION_OPEN";
            answers.append(branch);
        }
        return answers;
    }
}
