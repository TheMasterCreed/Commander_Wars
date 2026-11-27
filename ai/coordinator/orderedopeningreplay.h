#pragma once

#include "ai/coordinator/orderedreplylane.h"
#include <span>
#include <functional>
#include "ai/coordinator/openingrepairbill.h"
#include "ai/coordinator/economicledger.h"

namespace Coordinator
{
    inline std::optional<int> unchangedOpeningMovement(bool temporaryBonuses,int fullMovement,int fuel)
    {
        if(temporaryBonuses || fullMovement<0)return {};
        return fuel<0?fullMovement:std::min(fullMovement,fuel);
    }

    struct OrderedOpeningPolicy
    {
        // Shared by ordinary, clearing-without-the-final-shot and clearing-with-the-final-shot on this exact F0 team.
        OpeningLaneState afterService;
        int reservedShooter{NO_UNIT};
        OpeningRepairBill repair;
        std::vector<OpeningLaneAction> prefix;
        std::function<std::optional<OpeningLaneExchange>(const OpeningLaneState&,const OpeningLaneShot&,std::int64_t&)> exchange;
        MilliFunds materialLessCash{};
    };

    // Keep ordered prefix witness independent of the ordinary decline and residual economic jobs.
    // Every recorded event is compared against the new branch before its damage can be used.
    // A report can be priced only when all prefix shots and the funded terminal shot actually occurred.
    class OrderedOpeningReplay
    {
    public:
        explicit OrderedOpeningReplay(std::span<const OpeningLaneAction> charges):m_charges(charges){}

        const OpeningLaneAction *next() const
        {
            return !m_bad && m_applied<m_charges.size()?&m_charges[m_applied]:nullptr;
        }
        bool allows(int gun,int target,TilePoint from) const
        {
            const auto *a=next();
            return !m_bad && (!a || (a->shot.shooter==gun && a->shot.target==target && a->shot.from==from));
        }
        // The ordinary reply owns these facts after all preceding actions. Integer output follows native ACTION_FIRE.
        bool accept(const OpeningLaneAction &actual)
        {
            const auto *p=next();if(!p || !allows(actual.shot.shooter,actual.shot.target,actual.shot.from))return refuse();
            if(p->origin!=actual.origin || p->targetTile!=actual.targetTile || p->moveCost!=actual.moveCost ||
               p->shooterBefore!=actual.shooterBefore || p->targetBefore!=actual.targetBefore ||
               p->shooterAfter!=actual.shooterAfter || p->targetAfter!=actual.targetAfter ||
               p->startingLoad!=actual.startingLoad || p->returnLoad!=actual.returnLoad ||
               p->native.weapon!=actual.native.weapon || p->native.counterWeapon!=actual.native.counterWeapon ||
               p->native.hit!=actual.native.hit || p->native.counter!=actual.native.counter)return refuse();
            ++m_applied;return true;
        }
        bool complete() const{return !m_bad && m_applied==m_charges.size();}
        bool admitsPaidShot(bool physicallyFired) const{return complete() && physicallyFired;}
    private:
        bool refuse(){m_bad=true;return false;}
        std::span<const OpeningLaneAction> m_charges;
        std::size_t m_applied{};
        bool m_bad{};
    };
}
