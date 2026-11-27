#pragma once

#include "ai/coordinator/townassaultintegration.h"
#include <QCborValue>

namespace Coordinator
{
    // Test-only, value-based replay comparison. Qt's generic equality can
    // compare an integer and a rounded double equal; preserve the actual type.
    inline bool equalTownRecord(const QJsonValue &a,const QJsonValue &b)
    {
        return QCborValue::fromJsonValue(a).toCbor()==QCborValue::fromJsonValue(b).toCbor();
    }
    inline bool equalTownCeiling(const TownArrivalCeiling &a,const TownArrivalCeiling &b)
    {
        const auto fields=[](const auto &x){return std::tie(x.value,x.assumedArrival,x.ordinaryAfterConditional,
            x.unexaminedActionTick,x.alternative.reason,x.alternative.tile,x.alternative.actor,x.alternative.tick,
            x.hpAtUnexaminedAction,x.nativeExchangeRequired,x.withdrawalCoverage);};
        const auto menu=[](const auto &m){return std::tie(m.attempted,m.enumerated,m.allArrivalResources,
            m.tick,m.remainingFuel,m.hp,m.remainingAmmo,m.best,m.withdraw);};
        return fields(a)==fields(b) && menu(a.lateOptions)==menu(b.lateOptions) &&
            bool(a.witness)==bool(b.witness) && (!a.witness || *a.witness==*b.witness) &&
            equalTownRecord(QJsonArray{a.withdrawals,a.defenders,a.lateOptions.actions,a.lateOptions.open,a.lateOptions.jobs},
                            QJsonArray{b.withdrawals,b.defenders,b.lateOptions.actions,b.lateOptions.open,b.lateOptions.jobs});
    }
    inline QString differenceInTownResult(const PricedTownEncounter &a,const PricedTownEncounter &b)
    {
        if(std::tie(a.bids,a.work,a.ownerAuditAllowance,a.ownerAuditWork,a.retainedBoardBounds,a.boundsWithoutWitness)!=
           std::tie(b.bids,b.work,b.ownerAuditAllowance,b.ownerAuditWork,b.retainedBoardBounds,b.boundsWithoutWitness))return "counters-or-bounds";
        if(!equalTownRecord(QJsonArray{a.purchases,a.auditOriginalPurchases,a.evidence,a.searches,a.coverage,a.conflicts,a.requestedCapper},
                            QJsonArray{b.purchases,b.auditOriginalPurchases,b.evidence,b.searches,b.coverage,b.conflicts,b.requestedCapper}))
            return "published-records";
        if(a.selected.size()!=b.selected.size() || a.challenges.size()!=b.challenges.size() ||
           a.landingBounds.size()!=b.landingBounds.size() || bool(a.ledger)!=bool(b.ledger))return "menu-size";
        if(a.ledger){
            const auto amounts=[](const auto &x){return std::tie(x.ownership,x.capitalLoss,x.timing,x.score,x.flow);};
            if(amounts(*a.ledger)!=amounts(*b.ledger))return "ledger";
        }
        for(std::size_t i=0;i<a.selected.size();++i){
            const auto &x=a.selected[i],&y=b.selected[i];
            if(x.line!=y.line || x.priorOwner!=y.priorOwner || x.ownership!=y.ownership || !equalTownRecord(x.captureProof,y.captureProof))
                return "selected-line";
        }
        const auto challenge=[](const auto &c){return std::tie(c.tile,c.attackingSide,c.status,c.signedChange,c.capperUnit,c.entryTick,c.required);};
        for(std::size_t i=0;i<a.challenges.size();++i)if(challenge(a.challenges[i])!=challenge(b.challenges[i]))return "challenge";
        for(const auto &[k,caps]:a.landingBounds){
            const auto pos=b.landingBounds.find(k);
            if(pos==b.landingBounds.end() || caps.size()!=pos->second.size())return "landing-menu";
            for(std::size_t i=0;i<caps.size();++i)if(!equalTownCeiling(caps[i],pos->second[i]))return "landing-bound";
        }
        return {};
    }
}
