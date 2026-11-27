#pragma once

#include "ai/coordinator/openinglanemarket.h"
#include "ai/coordinator/openingbuyerprojection.h"
#include "ai/coordinator/openingrepairbill.h"
#include "game/building.h"
#include "game/gamerules.h"
#include "game/player.h"
#include <cmath>

namespace Coordinator
{
    struct OpeningLaneService
    {
        OpeningLaneHandoff after;
        OpeningRepairBill bill;
        MilliFunds materialLessCash{};
    };
    // The binding provider verifies ordinary building, unit and weather startup/repair behavior of the *loaded* ruleset.
    // No next-turn board, historical order or unconsumed purchase/RNG draw is supplied to this reconstruction.
    template<class Bind>
    std::optional<OpeningLaneService> openingLaneAfterTeam(GameMap &map,const BattlefieldKnowledge &knowledge,
        const MasterAssignmentProblem &problem,const CombatTerrain &terrain,
        const ReplyPassEvaluation &team,const QJsonObject &postF0,const QJsonArray &opening,Bind validatedRules,
        std::int64_t &work,QString *why=nullptr)
    {
        const auto refuse=[&](const char *reason)->std::optional<OpeningLaneService>{if(why)*why=reason;return {};};
        const int n=static_cast<int>(terrain.units.size());
        if(n!=static_cast<int>(knowledge.units().size()) || map.getPlayerCount()!=2 || work<6LL*n+opening.size())return refuse("INCOMPLETE_BOARD_OR_WORK");
        work-=6LL*n+opening.size();const int observer=knowledge.observerId(),enemySide=1-observer;
        auto *enemy=map.getPlayer(enemySide);auto *rules=map.getGameRules();
        if(!enemy||!rules||map.getCurrentPlayer()!=map.getPlayer(observer)||rules->getRankingSystem()||rules->getFogMode()!=GameEnums::Fog_Off)
            return refuse("DATED_VISIBILITY_OR_RANK_UNPRICED");
        for(int side=0;side<2;++side)for(int co=0;co<map.getPlayer(side)->getMaxCoCount();++co)if(map.getPlayer(side)->getCO(co))return refuse("CO_STATE_UNPRICED");
        if(!validatedRules(map,knowledge,work))return refuse("NONSTANDARD_OR_UNVERIFIED_DAY_START");
        OpeningLaneService result;auto &out=result.after;out.afterService.resize(n);out.movement.resize(n);
        std::map<int,int> index;std::vector<Unit*> bodies(n);std::vector<bool> seen(n);
        for(int i=0;i<n;++i){const auto &p=terrain.units[i];auto &s=out.afterService[i];s.side=int(p.enemy);const auto &k=knowledge.units()[i];
            if(!p.present)continue;const auto v=map.getTerrain(k.x,k.y)->getUnit();if(!v||v->getUniqueID()!=p.engineUnitId||!index.emplace(p.engineUnitId,i).second)return refuse("LIVE_STOCK_IDENTITY_CHANGED");
            // The candidate stock list excludes cargo; it cannot supply an unpriced power/supply modifier in this fixture family.
            if(v->getLoadedUnitCount())return refuse("CARGO_DAY_START_UNPRICED");
            if(v->isStatusStealthed())return refuse("PROJECTED_CLOAK_VISIBILITY_UNPRICED");
            bodies[i]=v;s.at=p.tile;s.hp=static_cast<int>(std::lround(10*v->getHp()));s.fuel=v->getFuel();s.rounds=p.ammoByWeapon;
            if(p.enemy && v->getFuel()<0 && v->getMaxFuel()>0)return refuse("E0_FUEL_DEATH_OR_RESUPPLY_UNPROVEN");
            if(p.enemy && !unchangedOpeningMovement(v->hasTemporaryBonuses(),p.movementBeforeFuel,v->getFuel()))return refuse("EXPIRING_UNIT_STATUS_OR_MOVEMENT");
            out.movement[i]=p.movementBeforeFuel;
        }
        for(const auto &value:postF0.value("units").toArray()){
            const auto row=value.toObject();const auto found=index.find(row["id"].toInt(NO_UNIT));if(found==index.end()||seen[found->second])return refuse("ROSTER_IDENTITY_CHANGED");
            const int i=found->second;auto &u=out.afterService[i];seen[i]=true;u.at={row["x"].toInt(-1),row["y"].toInt(-1)};u.hp=static_cast<int>(std::lround(10*row["hp"].toDouble(-1)));
            if(u.hp<0 || u.hp>100 || !map.onMap(u.at.x,u.at.y) || row["owner"].toInt(-1)!=knowledge.units()[i].ownerId)return refuse("ROSTER_FACT_CHANGED");
        }
        for(const auto &[uid,i]:index)if(!seen[i])return refuse("MISSING_STOCK_BODY");
        for(const auto &actor:team.actors){const auto &member=problem.actors()[actor.actorSlot];const int i=member.knowledgeUnitIndex;auto &u=out.afterService[i];const auto &candidate=member.candidates[actor.candidateIndex];
            const auto kind=planBundleKindOf(candidate.source.bundle);
            if(kind==PlanBundleKind::Service || kind==PlanBundleKind::MoveAndService || kind==PlanBundleKind::Compound)return refuse("MULTIBODY_F0_SERVICE_UNPRICED");
            if(u.fuel>=0)u.fuel=std::max(0,u.fuel-std::max(0,candidate.source.movementCost));
            if(actor.ownShot.target!=NO_UNIT){const auto &f=actor.ownShot;if(!debitOpeningLaneRound(u.rounds,f.report.weapon))return refuse("F0_WEAPON_RIGHT_CHANGED");
                if(f.report.counter>=0&&!debitOpeningLaneRound(out.afterService[f.target].rounds,f.report.counterWeapon))return refuse("F0_COUNTER_RIGHT_CHANGED");}
        }
        std::set<std::pair<int,int>> buyers;
        for(const auto &o:opening){auto shop=o.toObject();if(shop["enemy"].toBool()||shop["buildDay"].toInt()!=0)continue;auto p=shop["factory"].toArray();if(p.size()!=2||!buyers.emplace(p[0].toInt(),p[1].toInt()).second)return refuse("PURCHASE_SITE_UNRESOLVED");}
        if(buyers!=terrain.friendlyBirthSites)return refuse("PURCHASE_OCCUPANCY_CHANGED");
        std::map<std::pair<int,int>,int> owner;for(const auto &e:postF0.value("properties").toArray()){const auto p=e.toObject();if(!owner.emplace(std::pair{p["x"].toInt(-1),p["y"].toInt(-1)},p["owner"].toInt()).second)return refuse("DUPLICATE_PROPERTY");}
        for(const auto &b:knowledge.buildings()){const auto found=owner.find({b.x,b.y});if(found==owner.end() || ((found->second==enemySide)!=(b.ownerId==enemySide)))return refuse("ENEMY_PAYDAY_TITLE_CHANGED");}
        const bool preserveUnarmedMaterial=rules->getGameRule("GAMERULE_REFILL_MATERIAL")==nullptr;
        std::vector<OrdinaryOpeningRepair> repairs;
        for(int i=0;i<n;++i){auto &v=out.afterService[i];if(v.hp<=0 || !terrain.units[i].enemy)continue;auto *unit=bodies[i];const auto at=QPoint(v.at.x,v.at.y);
            auto *site=map.getTerrain(v.at.x,v.at.y)->getBuilding();if(!site || site->getOwner()!=enemy || !site->canRepair(unit) || !unit->canBeRepaired(at))continue;
            const int hp=std::max(0,2+unit->getRepairBonus(at));const double step=unit->getUnitCosts()/10.*unit->getRepairCostModifier();
            if(!std::isfinite(step)||step<0||step>std::numeric_limits<int>::max()||step!=std::floor(step))return refuse("NONINTEGRAL_SERVICE_PRICE");
            repairs.push_back({i,v.hp,hp,static_cast<std::int64_t>(step)});
            if(unit->getMaxFuel()>=0)v.fuel=unit->getMaxFuel();if(unit->getMaxAmmo1()>0&&(!preserveUnarmedMaterial||!unit->getWeapon1ID().isEmpty()))v.rounds[0]=unit->getMaxAmmo1();
            if(unit->getMaxAmmo2()>0&&(!preserveUnarmedMaterial||!unit->getWeapon2ID().isEmpty()))v.rounds[1]=unit->getMaxAmmo2();
        }
        const auto bill=ordinaryOpeningRepairs(static_cast<std::int64_t>(enemy->getFunds())+enemy->calcIncome(),repairs);
        if(!bill)return refuse("COMPETING_REPAIR_ORDER_OR_ARITHMETIC");result.bill=*bill;
        for(const auto &row:bill->rows){auto &s=out.afterService[row.actor];const auto value=terrain.units[row.actor].replacementCost;
            const auto increase=combatCapitalLoss(value,row.after/10.,row.before/10.);result.materialLessCash+=increase-row.debit*MILLI_FUNDS_PER_FUND;s.hp=row.after;}
        out.paidOpeningKnown=true;out.stockServiceKnown=true;out.datedCombatModifiersKnown=true;return result;
    }
}
