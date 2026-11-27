#pragma once

#include "ai/coordinator/openinglanerecord.h"
#include "ai/coordinator/friendlydaystart.h"

namespace Coordinator
{
    inline void bindFriendlyDayStart(ProductionForecastBoard &board, GameMap &map,
                                     const BattlefieldKnowledge &knowledge,
                                     std::shared_ptr<OpeningLaneRulesMemo> rules)
    {
        board.friendlyDayStart = [&map,&knowledge, rules = std::move(rules)](
            const ProductionForecastBoard &funding, const CombatTerrain &terrain,
            const std::set<std::pair<int,int>> &owned, std::span<const TilePoint> positions,
            std::span<const double> hp, std::span<const int> fuel,
            std::span<const std::array<int,2>> clips, const OpeningRecruitFight *fight,
            MilliFunds gross, std::int64_t &work, QString &why) mutable -> std::optional<FriendlyServiceResult> {
            const auto fail=[&](const char *s)->std::optional<FriendlyServiceResult>{why=s;return {};};
            const int own=knowledge.observerId();
            if(!funding.friendlyOpening || map.getPlayerCount()!=2 || own<0 || own>1 ||
               map.getCurrentPlayer()!=map.getPlayer(own) || !map.getGameRules() ||
               map.getGameRules()->getRankingSystem() || map.getGameRules()->getFogMode()!=GameEnums::Fog_Off ||
               hp.size()!=positions.size() || hp.size()!=terrain.units.size() || fuel.size()!=hp.size() ||
               clips.size()!=hp.size() || gross<0 || gross%MILLI_FUNDS_PER_FUND)
                return fail("FRIENDLY_F1_INCOMPLETE_BOARD_OR_MONEY");
            for(int seat=0;seat<2;++seat)
                for(int c=0;c<map.getPlayer(seat)->getMaxCoCount();++c)
                    if(map.getPlayer(seat)->getCO(c))return fail("FRIENDLY_F1_CO_STATE_UNPRICED");
            QJsonArray types;
            for(const auto &buy:*funding.friendlyOpening)types.append(QJsonObject{{"type",buy.type}});
            // An as-yet unselected enemy buyer still chooses from these loaded
            // unit scripts. Verify that ending E0 cannot change the friendly board.
            std::set<QString> possibleEnemy;
            for(const auto &site:funding.sites)
                for(const auto &u:site.menu[1])possibleEnemy.insert(u.type);
            for(const auto &type:possibleEnemy)types.append(QJsonObject{{"type",type},{"enemy",true}});
            if(!rules->verify(map,knowledge,types,work,why,true))return {};
            const int n=static_cast<int>(hp.size());
            const auto full=n+static_cast<int>(funding.friendlyOpening->size());
            if(work<4LL*full)return fail("FRIENDLY_F1_SERVICE_WORK_LIMIT");
            work-=4LL*full;
            std::vector<FriendlyServiceSubject> subjects;
            std::set<std::pair<int,int>> seen;
            const bool noMaterial=map.getGameRules()->getGameRule("GAMERULE_REFILL_MATERIAL")==nullptr;
            auto add=[&](int i,Unit &u,TilePoint p,double life,int remaining,
                         std::array<int,2> load,MilliFunds value)->bool{
                if(!map.onMap(p.x,p.y) || !seen.emplace(p.x,p.y).second || !std::isfinite(life) || life<=0 ||
                   life>10. || u.getLoadedUnitCount() || u.isStatusStealthed() || u.hasTemporaryBonuses()) return false;
                if(!owned.contains({p.x,p.y}))return true;
                auto *shop=map.getTerrain(p.x,p.y)->getBuilding();if(!shop)return false;
                const QPoint at(p.x,p.y);
                if(!shop->canRepair(&u,true) || !u.canBeRepaired(at))return true;
                const auto cost=map.getPlayer(own)->getCosts(u.getUnitID(),at);
                if(cost<0 || toMilliFunds(cost)!=value)return false;
                const double price=cost/10.*u.getRepairCostModifier();
                const auto tenths=std::lround(life*10);
                if(std::abs(life*10-tenths)>0.000001 || tenths<1 || tenths>100 ||
                   !std::isfinite(price) || price<0 || price>std::numeric_limits<int>::max() || price!=std::floor(price))
                    return false;
                const auto steps=2+u.getRepairBonus(at);if(steps<0)return false;
                subjects.push_back({{i,static_cast<int>(tenths),steps,static_cast<std::int64_t>(price)},value,
                    remaining,u.getMaxFuel(),load,{u.getMaxAmmo1(),u.getMaxAmmo2()},
                    {!noMaterial || !u.getWeapon1ID().isEmpty(),!noMaterial || !u.getWeapon2ID().isEmpty()}});
                return true;
            };
            for(int i=0;i<n;++i){
                const auto &p=terrain.units[i];if(p.enemy || !p.present || hp[i]<=0)continue;
                const auto &k=knowledge.units()[i];auto *u=map.getTerrain(k.x,k.y)->getUnit();
                if(!u || u->getUniqueID()!=p.engineUnitId || !add(i,*u,positions[i],hp[i],fuel[i],clips[i],p.replacementCost))
                    return fail("FRIENDLY_F1_ORIGINAL_OR_SERVICE_RATE_UNVERIFIED");
            }
            for(std::size_t j=0;j<funding.friendlyOpening->size();++j){
                const auto &buy=(*funding.friendlyOpening)[j];
                const bool attacked=fight && fight->factory==buy.factory;
                if(attacked && fight->recruitAfter<=0)return fail("FRIENDLY_F1_OPENING_CASUALTY_SLOT_UNPRICED");
                Unit u(buy.type,map.getPlayer(own),false,&map);
                const auto load=attacked?fight->recruitLoad:std::array<int,2>{
                    u.getMaxAmmo1()<0?-1:u.getAmmo1(),u.getMaxAmmo2()<0?-1:u.getAmmo2()};
                if(!add(n+static_cast<int>(j),u,buy.factory,attacked?fight->recruitAfter:u.getHp(),u.getFuel(),load,buy.price))
                    return fail("FRIENDLY_F1_RECRUIT_OR_SERVICE_RATE_UNVERIFIED");
            }
            auto result=settleFriendlyDayStart(gross/MILLI_FUNDS_PER_FUND,std::move(subjects));
            if(!result)why="FRIENDLY_F1_COMPETING_REPAIRS_OR_ARITHMETIC";
            return result;
        };
    }
}
