#pragma once

#include "ai/coordinator/townrivalguard.h"

namespace Coordinator
{
    inline QJsonObject townActorIdentity(const CombatTerrain &terrain,std::span<const FundedCaptureGun> paid,int i)
    {
        const int stock=static_cast<int>(terrain.units.size());
        if(i>=0 && i<stock)return {{"unit",terrain.units[i].engineUnitId}};
        if(i<stock || i-stock>=static_cast<int>(paid.size()))return {};
        const auto &gun=paid[i-stock];if(!gun.unit)return {};
        return {{"birth",QJsonArray{gun.factory.x,gun.factory.y}},{"builtTick",gun.buildTick},{"type",gun.unit->type}};
    }
    // The guard has committed before the late side chooses whether, when
    // and how to challenge it. Keep the best affordable date *within* each
    // observed policy. The two dates need not be the same.
    struct TownObservedResponses
    {
        struct Choice
        {
            int covered{},bestDate{-1},capper{NO_UNIT},finish{-1};
            MilliFunds score{};
            QString action;
            bool partial{};
        };
        Choice open,guard,earlyOwnerStays;
        std::set<int> dates;
        bool possibleEarlyTitle{};
        int attackingSide{};

        static std::optional<MilliFunds> score(const TownAssaultAlternatives &q)
        {
            if(q.workLimited || q.unresolved)return {};
            return q.selectsAssault?q.responseValue:q.declineValue;
        }
        static void inspect(Choice &choice,const TownAssaultAlternatives &q,int side,int date,const QString &action)
        {
            const auto amount=score(q);
            if(!amount){choice.partial=true;return;}
            const auto *line=q.selectsResponse && q.response?&*q.response:q.assault?&*q.assault:nullptr;
            if(choice.bestDate<0 || TownAssaultDetail::better(*amount,choice.score,side))
            {
                choice.bestDate=date;choice.score=*amount;choice.action=q.selectsAssault?action:"DECLINE_DOWNSTREAM_ATTACK";
                choice.capper=q.selectsAssault && line?line->capper:NO_UNIT;
                choice.finish=q.selectsAssault && line?line->finishTick:-1;
            }
        }
        void observe(int side,int date,const TownRivalGuardComparison &q)
        {
            if(!dates.insert(date).second)return;
            attackingSide=side;possibleEarlyTitle|=q.earlyTitlePossible;
            if(q.earlyTitlePossible)
            {
                if(q.afterEarlyTitle && q.afterEarlyTitle->outcome && q.afterEarlyTitle->late)
                {
                    inspect(earlyOwnerStays,*q.afterEarlyTitle->late,side,date,"AFTER_EARLY_TITLE_WITH_RETAINED_HOLDER");
                    ++earlyOwnerStays.covered;
                }
                else earlyOwnerStays.partial=true;
            }
            if(!q.openEvaluated)open.partial=true;
            else if(q.openRefutes) {++open.covered;inspect(open,q.open,side,date,"OPEN_TOWN");}
            else
            {
                if(!q.open.workLimited && !q.open.unresolved)++open.covered;
                inspect(open,q.open,side,date,"OPEN_TOWN");
            }
            if(!q.guarded){guard.partial=true;return;}
            bool full=score(*q.guarded).has_value();
            inspect(guard,*q.guarded,side,date,"AFTER_GUARD_STAYS");
            if(!q.approachShots.isEmpty())
            {
                if(!q.afterGuardExchange || !q.competingGuardGunDuties.isEmpty())
                {guard.partial=true;full=false;}
                else
                {
                    full&=score(*q.afterGuardExchange).has_value();
                    inspect(guard,*q.afterGuardExchange,side,date,"AFTER_GUARD_EXCHANGE");
                }
            }
            if(full)++guard.covered;
        }
        QJsonObject record(const CombatTerrain &terrain,int expected,std::span<const FundedCaptureGun> paid={}) const
        {
            const auto describe=[&](const Choice &choice) {
                const auto actor=townActorIdentity(terrain,paid,choice.capper);
                const bool identified=choice.capper==NO_UNIT || !actor.isEmpty();
                const bool complete=identified && !choice.partial && choice.covered==expected && choice.bestDate>=0;
                QJsonObject result{{"coveredDates",choice.covered},{"complete",complete},
                    {"bestDate",choice.bestDate},{"capper",actor.value("unit").toInt(NO_UNIT)},
                    {"finish",choice.finish},{"action",choice.action},
                    {"score",complete?QJsonValue(static_cast<qint64>(choice.score)):QJsonValue(QJsonValue::Null)}};
                if(actor.contains("birth"))result["paidCapper"]=actor;
                if(!identified)result["actorStatus"]="UNRESOLVED_PARTICIPANT_IDENTITY";
                if(choice.bestDate>=0)result["examinedScore"]=static_cast<qint64>(choice.score);
                return result;
            };
            const auto avoided=describe(open),retained=describe(guard);
            const bool priced=avoided["complete"].toBool() && retained["complete"].toBool();
            const auto worse=TownAssaultDetail::better(open.score,guard.score,attackingSide)?guard.score:open.score;
            return {{"declinesEarlyMove",avoided},{"guardsTown",retained},
                {"worseOfGuardOrDecline",priced?QJsonValue(static_cast<qint64>(worse)):QJsonValue(QJsonValue::Null)},
                {"afterEarlyTitleHolderStays",describe(earlyOwnerStays)},
                {"lateReplyAfterEarlyTitle",earlyOwnerStays.covered?"PRICED_FOR_TESTED_RETAINED_POSTS":
                    possibleEarlyTitle?"REQUIRES_SEPARATE_RECAPTURE_LEDGER":"NO_EARLY_TITLE_QUOTED"}};
        }
    };
}
