#include "ai/coordinator/combatmilestone.h"
#include "ai/coordinator/townnativebindings.h"
#include "ai/coordinator/openpurchasepreference.h"
#include <QDataStream>
#include "ai/coordinator/followupschedule.h"
#include "ai/coordinator/followuppolarity.h"
#include "ai/coordinator/openingsurvival.h"
#include "ai/coordinator/singleactivationhq.h"
#include "ai/coordinator/openingrecruitadmission.h"
#include "ai/coordinator/openinglanerecord.h"
#include "ai/coordinator/friendlyserviceprovider.h"

#include <QJsonArray>
#include <QCryptographicHash>
#include <QScopeGuard>
#include <QJsonDocument>
#ifdef COW_COORDINATED_REFINEMENT_TESTING
#include <QFile>
#include <QSaveFile>
#include "ai/coordinator/pairdiscoverycheckpoint.h"
#include "coreengine/settings.h"
#endif
#include <set>
#include <numeric>
#include <string_view>

#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/combatreferencerepair.h"
#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/decisiontrace.h"
#include "ai/coordinator/openingbuyerprojection.h"
#include "ai/coreai.h"
#include "ai/productionSystem/simpleproductionsystem.h"
#include "ai/coordinator/headquartersdefense.h"
#include "ai/coordinator/headquarterstransit.h"
#include "ai/coordinator/masterplanhandoff.h"
#include "ai/coordinator/stockprogress.h"
#include "ai/coordinator/stockopeningreplies.h"
#include "ai/coordinator/stockescape.h"
#include "ai/coordinator/capturefinishtrade.h"
#include "ai/coordinator/opentacticalrefinement.h"
#include "ai/coordinator/unifiedteamsearch.h"
#include "ai/coordinator/factoryprogramhearing.h"
#include "ai/coordinator/factoryopeningsecurity.h"
#include "ai/coordinator/factoryprogrambuyer.h"
#include "ai/coordinator/mobilityfieldcache.h"
#include "ai/coordinator/replypass.h"
#include "ai/coordinator/replydominance.h"
#include "ai/coordinator/replyfactsbuilder.h"
#include "ai/coordinator/combatcapturerace.h"
#include "ai/coordinator/townmarketprefetch.h"
#include "ai/coordinator/towncomparisonevidence.h"
#include "ai/coordinator/rankedpairproof.h"
#include "ai/coordinator/fundeddeploymentcomparison.h"
#include "ai/coordinator/combatinterruption.h"
#include "ai/coordinator/productionforecastbuilder.h"
#include "ai/coordinator/capturecommitments.h"
#include "game/gamemap.h"
#include "game/building.h"
#include "game/gamerules.h"
#include "game/gamescript.h"
#include "game/victoryrule.h"
#include "coreengine/scriptvariables.h"
#include "coreengine/scriptvariable.h"
#include "coreengine/interpreter.h"
#include "ai/coordinator/hqopeningbindings.h"
#include "ai/coordinator/retainedwalkprofile.h"
#include "ai/coordinator/screenfirerules.h"
#include "game/terrain.h"
#include "game/unit.h"
#include "game/player.h"
#include "ai/coordinator/replyfactsbuilder.h"
#include "ai/coordinator/isolatedpayday.h"

namespace Coordinator
{
    CombatMode combatMode()
    {
        const auto value = qgetenv("COW_COORDINATED_COMBAT");
        if (value == "acting")
            return CombatMode::Acting;
        if (value == "recorded")
            return CombatMode::Recorded;
        return CombatMode::Off;
    }
} // namespace Coordinator

namespace
{
    using namespace Coordinator;
    constexpr int MAX_COMBAT_TRIALS = 48;
    QJsonArray tile(TilePoint point)
    {
        return {point.x, point.y};
    }

    std::shared_ptr<CombatTerrain> combatTerrain(GameMap &map, const BattlefieldKnowledge &knowledge,
                                                 MobilityFieldCache &mobility,std::int64_t &work)
    {
        auto terrain = std::make_shared<CombatTerrain>();
        terrain->movementCache = std::make_shared<CombatMovementCache>();
        terrain->canRepairOnTerrain=[&map,&knowledge](int i,TilePoint tile){
            if(i<0||i>=static_cast<int>(knowledge.units().size())||!map.onMap(tile.x,tile.y))return false;
            const auto &u=knowledge.units()[i];auto *source=map.getTerrain(u.x,u.y)->getUnit();auto *site=map.getTerrain(tile.x,tile.y)->getBuilding();
            return source && site && site->canRepair(source,true) && source->canBeRepaired(QPoint(tile.x,tile.y));
        };
        terrain->width = knowledge.width();
        terrain->height = knowledge.height();
        std::map<std::pair<int, QString>, int> profiles;
        std::set<QString> types,ordinaryFiringOrder;
        for(const auto &unit:knowledge.units())types.insert(unit.unitId);
        std::vector<QString> names(types.begin(),types.end());
        RefinementLedger ruleWork;ruleWork.open(work);
        auto &engine=*Interpreter::getInstance();
        GuardedScriptOwnIndexCache properties(engine,ruleWork);
        const auto globals=GuardedScriptGlobalIndex::prepare(engine,names,ruleWork);
        if(globals.status()==GuardedScriptPropertyStatus::Found)for(const auto &type:names)
        {
            const auto object=globals.find(engine,type,ruleWork);
            if(object.status!=GuardedScriptPropertyStatus::Found)continue;
            const auto hook=guardedScriptProperty(object.value,QStringLiteral("getFirstStrike"),ruleWork,&properties);
            if(hook.status==GuardedScriptPropertyStatus::Missing || (hook.status==GuardedScriptPropertyStatus::Found &&
               (hook.value.isNull() || hook.value.kind()==GuardedScriptValueKind::Undefined)))ordinaryFiringOrder.insert(type);
        }
        work-=ruleWork.stats().spent();
        for (const KnownUnit &unit : knowledge.units())
        {
            CombatPawn pawn{{unit.x, unit.y},
                            unit.carrier == KnownUnit::NO_CARRIER && unit.hpRounded > 0,
                            knowledge.relation(unit.ownerId) == Relation::Enemy,
                            unit.ignoresUnitCollision,
                            -1,
                            unit.movementPoints};
            pawn.capper = unit.canCapture;
            pawn.carrying = unit.canCapture && unit.capturePoints > 0;
            if (pawn.present)
            {
                auto *live = map.getTerrain(unit.x, unit.y)->getUnit();
                const auto untrimmed=static_cast<std::int64_t>(live->getBaseMovementPoints())+
                    live->getBonusMovementpoints(QPoint(unit.x,unit.y));
                pawn.movementBeforeFuel=static_cast<int>(std::clamp<std::int64_t>(untrimmed,0,std::numeric_limits<int>::max()));
                pawn.engineUnitId = live->getUniqueID();
                auto *owner=live->getOwner();
                pawn.needsNativeTownExchange=!ordinaryFiringOrder.contains(unit.unitId) || !owner || owner->getCO(0) ||
                    owner->getCO(1) || live->getUnitRank()!=GameEnums::UnitRank_None;
                pawn.realHp = live->getHp();
                pawn.hasNativeWeapon = !live->getWeapon1ID().isEmpty() || !live->getWeapon2ID().isEmpty();
                pawn.transportCapacity=live->getLoadingPlace()>0;
                const auto supportActions=live->getActionList();
                pawn.supportsUnits=std::any_of(supportActions.begin(),supportActions.end(),[](const QString &action){return action.startsWith(QStringLiteral("ACTION_SUPPORT"));});
                pawn.ammoByWeapon = {live->getMaxAmmo1() < 0 ? -1 : live->getAmmo1(),
                                     live->getMaxAmmo2() < 0 ? -1 : live->getAmmo2()};
                pawn.replacementCost = toMilliFunds(live->getUnitCosts());
                pawn.availableShots = std::numeric_limits<int>::max();
                if (!live->getWeapon1ID().isEmpty() && live->getMaxAmmo1() > 0 && live->hasAmmo1())
                    pawn.availableShots = live->getAmmo1();
                if (!live->getWeapon2ID().isEmpty() && live->getMaxAmmo2() > 0 && live->hasAmmo2())
                    pawn.availableShots = std::min(pawn.availableShots, live->getAmmo2());
            }
            const bool canRefreshWeapon = unit.ownerId == knowledge.observerId() && unit.hasMoved && pawn.hasNativeWeapon;
            if (pawn.present && (unit.canCapture || unit.canFire || canRefreshWeapon))
            {
                auto [entry, fresh] = profiles.try_emplace({unit.ownerId, unit.unitId}, terrain->grids.size());
                if (fresh)
                    terrain->grids.push_back(mobility.grid(map, *map.getPlayer(unit.ownerId), unit.unitId));
                pawn.travelGrid = entry->second;
                if (unit.canCapture || (unit.canMoveAndFire && (unit.canFire || canRefreshWeapon))) pawn.grid=entry->second;
            }
            terrain->units.push_back(pawn);
        }
        auto live = [&map, &knowledge](int i) {
            const auto &u = knowledge.units()[i];
            return map.getTerrain(u.x, u.y)->getUnit();
        };
        terrain->ownFire = [live, &knowledge, oracle = DamageOracle(map)](int a, TilePoint from, int b, double actorHp,
                                                                          double targetHp, std::array<int,2> targetAmmo) mutable {
            const auto &victim = knowledge.units()[b];
            return oracle.estimateAtHp(a, *live(a), from, b, *live(b), {victim.x, victim.y}, actorHp, targetHp, targetAmmo);
        };
        terrain->weapon = TownStockWeapon{{},[live, &knowledge, oracle = DamageOracle(map)](TownStockWeapon::Map &cache,int a, int b, int distance) mutable {
            auto [it, fresh] = cache.try_emplace({a, b, distance});
            if (fresh)
                it->second = oracle.baseDamageAgainst(*live(a), *live(b), distance, knowledge.units()[a].minRange,
                                                      knowledge.units()[a].maxRange);
            return it->second;
        }};
        terrain->weaponSlots = TownStockSlots{{},[live, &knowledge, oracle = DamageOracle(map)](TownStockSlots::Map &cache,int actor,int target,int distance) mutable {
            auto [it,fresh] = cache.try_emplace({actor,target,distance});
            if (fresh)
            {
                const auto &unit = knowledge.units()[actor];
                auto &a = *live(actor);
                auto &b = *live(target);
                it->second = {oracle.bestSlotDamage(a,b,distance,unit.minRange,unit.maxRange,true,false),
                              oracle.bestSlotDamage(a,b,distance,unit.minRange,unit.maxRange,false,true)};
            }
            return it->second;
        }};
        auto defenseGrid = std::make_shared<TownDefenseGrid>();
        defenseGrid->width = terrain->width;
        defenseGrid->height = terrain->height;
        defenseGrid->tileStars.resize(static_cast<std::size_t>(terrain->width * terrain->height), 0);
        for (int y = 0; y < terrain->height; ++y)
            for (int x = 0; x < terrain->width; ++x)
                if (Terrain *pTerrain = map.getTerrain(x, y))
                    defenseGrid->tileStars[static_cast<std::size_t>(y * terrain->width + x)] = pTerrain->getBaseDefense();
        defenseGrid->unitUsesDefense.resize(terrain->units.size(), 0);
        bool initializedBase = false;
        for (int i = 0; i < static_cast<int>(terrain->units.size()); ++i)
            if (terrain->units[i].present)
                if (Unit *u = live(i))
                {
                    if (!initializedBase)
                    {
                        defenseGrid->base = projectedTerrainRule(map, *u, {0, 0});
                        defenseGrid->base.stars = 0;
                        initializedBase = true;
                    }
                    defenseGrid->unitUsesDefense[static_cast<std::size_t>(i)] = u->useTerrainDefense() ? 1 : 0;
                }
        TownStockDefense stockDefense;
        stockDefense.grid = defenseGrid;
        stockDefense.invoke = [defenseGrid](TownStockDefense::Map &, int i, TilePoint at) {
            return defenseGrid->lookup(i, at);
        };
        terrain->defense = std::move(stockDefense);
        return terrain;
    }

    QJsonObject bookedCaptureThreat(const QJsonArray &claims,const ReplyPassEvaluation &economic,
                                   const PropertyStockField &field,const CombatTerrain &terrain)
    {
        QJsonObject witness;
        MilliFunds best=0;
        for (const auto &item:claims)
        {
            const auto c=item.toObject();
            const auto credit=c.value("signedIncome").toInteger();
            if (c.value("enemy").toBool(true) || credit<=0 || c["departureDayOffset"].toInt()!=1 ||
                c["entryDayOffset"].toInt()!=1 || c["carry"].toInt(-1)!=0) continue;
            const int victim=c["unit"].toInt(NO_UNIT), index=c["unitKnowledge"].toInt(NO_UNIT);
            const auto baseline=economic.realAfterReply.find(index);
            if (baseline==economic.realAfterReply.end()) continue;
            auto it=economic.captureChips.lower_bound({victim,0});
            if (it==economic.captureChips.end() || it->first.first!=victim) continue;
            const auto xy=c["tile"].toArray();
            if (xy.size()!=2) continue;
            const int column=field.columnSlotAt({xy[0].toInt(),xy[1].toInt()});
            if (column==NO_STOCK_COLUMN) continue;
            const auto prop=mirroredColumn(field.enemyColumn(column));
            const int finish=c["completionDayOffset"].toInt(),rate=c["rate"].toInt();
            for (;it!=economic.captureChips.end() && it->first.first==victim;++it)
            {
                if (!terrain.pay(1)) return witness;
                const auto &hit=it->second;
                const auto delayed=delayedBookedCapture(field.capturePointsToCapture(),0,rate,finish,
                    field.horizonTurns(),ceilHpSteps(baseline->second),ceilHpSteps(hit.targetAfter));
                if (!delayed) continue;
                const auto early=captureRaceSwing(prop,false,finish,field.horizonTurns());
                const auto later=captureRaceSwing(prop,false,delayed->finish,field.horizonTurns());
                const auto lost=std::min<MilliFunds>(credit,early-later);
                auto value=hit.exchange();
                if (lost<=0 || !RelativeStockDetail::checkedAdd(value,lost) || value<=best) continue;
                best=value;
                witness=QJsonObject{{"unit",victim},{"unitKnowledge",index},{"tile",xy},
                    {"shooter",terrain.units[hit.shooter].engineUnitId},{"from",tile(hit.from)},
                    {"openingTargetHp",hit.targetBefore},{"economicTargetHp",baseline->second},{"witnessTargetHp",hit.targetAfter},
                    {"damageValue",static_cast<qint64>(hit.damage)},{"counterValue",static_cast<qint64>(hit.counter)},
                    {"shooterForegoneCapture",static_cast<qint64>(hit.foregoneCapture)},
                    {"bookedRate",rate},{"witnessRate",delayed->rate},{"bookedFinish",finish},{"witnessFinish",delayed->finish},
                    {"incomeAtRisk",static_cast<qint64>(lost)}};
            }
        }
        return witness;
    }

    QJsonObject forecastRecord(const ReplyPassEvaluation &evaluation, const std::set<int> &watched)
    {
        QJsonObject record{{"status", REPLY_PASS_NO_VALUE_NAMES[static_cast<int>(evaluation.reason)]}};
        if (!evaluation.valued())
        {
            if (evaluation.actorSlot >= 0) record["failedActorSlot"] = evaluation.actorSlot;
            return record;
        }
        QJsonArray shots, units, ownFire;
        for (const auto &shot : evaluation.shots)
        {
            shots.append(QJsonObject{{"shooterKnowledge", shot.shooterIndex},
                                     {"from", tile(shot.firingTile)},
                                     {"target", shot.targetEngineUnitId},{"targetKnowledge",shot.targetKnowledge},{"priorActivation",shot.target>=evaluation.actors.size()},{"counterWeapon",shot.counterWeapon},
                                     {"hpBefore", shot.hpStepsBefore},
                                     {"hpAfter", shot.hpStepsAfter}, {"realBefore",shot.realBefore},
                                     {"realAfter",shot.realAfter}, {"targetCapital",static_cast<qint64>(shot.targetCapital)},
                                     {"counterCapital",static_cast<qint64>(shot.counterCapital)}});
        }
        for (const auto &actor : evaluation.actors)
        {
            const auto &own = actor.ownShot;
            if (own.target != NO_UNIT)
                ownFire.append(QJsonObject{{"unit", actor.engineUnitId}, {"targetKnowledge", own.target},
                    {"targetTile", tile(evaluation.replay.plan.action(actor.actionIndex).target)},
                    {"attackerBefore", own.attackerBefore}, {"attackerAfter", own.attackerAfter},
                    {"targetBefore", own.targetBefore}, {"targetAfter", own.targetAfter},
                    {"damage", own.report.damage}, {"counter", own.report.counter},
                    {"weapon",own.report.weapon}, {"counterWeapon",own.report.counterWeapon},
                    {"targetCost",static_cast<qint64>(own.targetCost)}, {"actorCost",static_cast<qint64>(own.actorCost)},
                    {"targetCapital",static_cast<qint64>(own.targetCapital)}, {"counterCapital",static_cast<qint64>(own.counterCapital)},
                    {"actingSeated",static_cast<qint64>(actor.terms.actingSeated)},
                    {"ownCounterCorrection",static_cast<qint64>(actor.terms.ownCounterCorrection)}});
            if (!watched.contains(actor.engineUnitId) && actor.stepsAfterReply == actor.dayStartSteps && !actor.repricedSurvivor)
                continue;
            const auto &action = evaluation.replay.plan.action(actor.actionIndex);
            units.append(QJsonObject{{"unit", actor.engineUnitId},
                                     {"end", tile(action.destination)},
                                     {"grantedDamage", actor.grantedDamage},
                                     {"repricedSurvivor", actor.repricedSurvivor},
                                     {"nextShot", static_cast<qint64>(actor.terms.nextShotAfterReply)},
                                     {"afterOwn", actor.stepsAfterOwnActions},
                                     {"afterReply", actor.stepsAfterReply}});
        }
        record.insert("encirclement", evaluation.encirclement.target == NO_UNIT ? QJsonObject{}
                                          : combatEncirclementRecord(evaluation.encirclement));
        QJsonArray threats;
        for (const auto &threat : evaluation.lethalThreats)
        {
            QJsonArray origins;
            for (auto from : threat.origins) origins.append(tile(from));
            threats.append(QJsonObject{{"victim",threat.victim},{"shooterKnowledge",threat.shooter},{"origins",origins}});
        }
        record.insert("lethalFiringThreats",threats);
        record.insert("ownFire", ownFire);
        record.insert("shots", shots);
        record.insert("units", units);
        record.insert("nonStock", static_cast<qint64>(evaluation.exactNonStock));
        return record;
    }

    std::optional<MilliFunds> total(MilliFunds economic, const ReplyStockOwned &owned, MilliFunds stock)
    {
        if (owned.status != ReplyStockStatus::Valid || !RelativeStockDetail::checkedAdd(economic, owned.owned) ||
            !RelativeStockDetail::checkedAdd(economic, stock))
            return {};
        return economic;
    }

    // The first combat pass keeps the opponent's capture forecast common. Refuse a comparison if a
    // change alters an enemy capturer's own-action HP, counterdamage or choice of firing position.
    using EnemyEffects = std::vector<std::tuple<int, int, int, int, MilliFunds>>;
    EnemyEffects enemyCaptureEffects(const ReplyPassEvaluation &evaluation, const MasterAssignmentProblem &problem,
                                     const BattlefieldKnowledge &knowledge)
    {
        EnemyEffects effects;
        for (const auto &actor : evaluation.actors)
        {
            const auto &candidate = problem.actors()[actor.actorSlot].candidates[actor.candidateIndex];
            const auto *component = singleComponentOf(candidate.source.bundle);
            if (component && component->kind == ComponentKind::Fire &&
                knowledge.units().at(component->fire.targetUnitId).canCapture)
                effects.emplace_back(component->fire.targetUnitId, -1, -1, actor.grantedDamage, 0);
        }
        for (const auto &shot : evaluation.shots)
            if (knowledge.units().at(shot.shooterIndex).canCapture)
                effects.emplace_back(shot.shooterIndex, shot.firingTile.x, shot.firingTile.y, 0, shot.counterCapital);
        for (const auto &[index, hp] : evaluation.enemyReplyHp)
            if (knowledge.units().at(index).canCapture)
                effects.emplace_back(index, -2, -2, hp, 0);
        std::sort(effects.begin(), effects.end());
        return effects;
    }

    struct HeadquartersWatch
    {
        TilePoint home{INVALID_TILE}, away{INVALID_TILE};
        std::set<int> approaching;
        HeadquartersTransitTiles transit;
        std::vector<TilePoint> sites;
        int standing{NO_UNIT};
        bool ordinaryStandingCapture{};
        HeadquartersTerrainTurn standingTerrain;
        HeadquartersTurnUnits turnUnits;

    };

    HeadquartersWatch headquartersWatch(GameMap &map, const BattlefieldKnowledge &knowledge,
                                        const CombatTerrain &terrain)
    {
        HeadquartersWatch watch;
        if (!map.getGameRules()->hasVictoryRule(QStringLiteral("VICTORYRULE_NOHQ")))
            return watch;
        std::vector<TilePoint> own, enemy;
        for (const auto &b : knowledge.buildings())
        {
            auto *site = map.getTerrain(b.x, b.y)->getBuilding();
            if (!site || !site->isHq())
                continue;
            watch.sites.push_back({b.x, b.y});
            if (knowledge.relation(b.ownerId) == Relation::Own)
                own.push_back({b.x, b.y});
            if (knowledge.relation(b.ownerId) == Relation::Enemy)
                enemy.push_back({b.x, b.y});
        }
        if (enemy.size() == 1)
            watch.away = enemy.front();
        if (own.size() != 1)
            return watch;
        watch.home = own.front();
        for (int i = 0; i < static_cast<int>(terrain.units.size()); ++i)
        {
            const auto &pawn = terrain.units[i];
            if (!pawn.present || !pawn.enemy || !knowledge.units()[i].canCapture || pawn.grid < 0)
                continue;
            if (pawn.tile == watch.home)
                watch.standing = i;
            if (std::abs(pawn.tile.x - watch.home.x) + std::abs(pawn.tile.y - watch.home.y) > 2 * pawn.movement)
                continue;
            watch.approaching.insert(i);
            if (!pawn.ignoresCollision)
                addHeadquartersTransitTiles(watch.transit,terrain.grids[pawn.grid],pawn.tile,watch.home,pawn.movement);
        }
        if(watch.standing!=NO_UNIT)
        {
            // These native rules cannot end the match in the other side's
            // favour during F0 while both armies survive and no HQ changes hands.
            const auto *script=map.getGameScript();
            auto *rules=map.getGameRules();
            bool ordinary=!script || (!script->hasLoadedScript() && script->getScriptFile().isEmpty());
            auto *noHQ=rules->getVictoryRule(QStringLiteral("VICTORYRULE_NOHQ"));
            const auto key=QStringLiteral("Active")+QString::number(knowledge.observerId());
            auto *active=noHQ?noHQ->getVariables()->getVariable(key):nullptr;
            ordinary=ordinary && active && active->readDataBool();
            for(int i=0;ordinary && i<rules->getVictoryRuleSize();++i)
            {
                const auto id=rules->getVictoryRuleAtIndex(i)->getRuleID();
                ordinary=id=="VICTORYRULE_NOHQ" || id=="VICTORYRULE_NOUNITS";
            }
            const auto &capper=knowledge.units()[watch.standing];
            // Native unit capture adds rounded HP to optional unit and CO
            // bonuses. A changing bonus or turn-boundary power needs a fresh
            // native evaluation. Never subtract HP from yesterday's bonus.
            for(int side=0;ordinary && side<map.getPlayerCount();++side)
            {
                auto *player=map.getPlayer(side);
                ordinary=player && !player->getCO(0) && !player->getCO(1);
            }
            // The two-seat solver follows the same wrap as GameMap::nextPlayer.
            const auto *siteTerrain=map.getTerrain(watch.home.x,watch.home.y);
            watch.standingTerrain={capper.ownerId<knowledge.observerId(),siteTerrain->getHasStartOfTurn()};
            if(ordinary)for(const auto &unit:knowledge.units())
            {
                if(unit.ownerId==knowledge.observerId())watch.turnUnits.outgoing.insert(unit.unitId);
                else if(unit.ownerId==capper.ownerId)
                {
                    auto [at,inserted]=watch.turnUnits.incomingMinimumFuel.try_emplace(unit.unitId,unit.fuel);
                    if(!inserted)at->second=std::min(at->second,unit.fuel);
                }
            }
            watch.ordinaryStandingCapture=ordinary && capper.captureRate==capper.hpRounded;
        }
        return watch;
    }

    // Immediate wins/losses outrank the short-range exposure warning and ordinary income.
    using CombatRank = std::tuple<TerminalClass, int, int, MilliFunds>;
    using ReplyRanks = std::map<QString,CombatRank>;
    CombatRank recordedReplyRank(const QJsonObject &reply)
    {
        return {static_cast<TerminalClass>(reply.value("terminalClass").toInt()), reply.value("hqSecurity").toInt(),
                reply.value("conditionalThreatRank").toInt(),reply.value("policyTotal").toInteger()};
    }
    ReplyRanks replyRanks(const QJsonArray &replies)
    {
        ReplyRanks result;
        for (const auto &item : replies)
        {
            const auto reply = item.toObject();
            if (reply.contains("policyTotal")) result.emplace(reply.value("replyPolicy").toString(),recordedReplyRank(reply));
        }
        return result;
    }
    bool improvesMatchedReplies(const QJsonArray &priced,const ReplyRanks &incumbent,
                               bool complete,QJsonArray *improved=nullptr,bool sameMenuPossible=true)
    {
        const auto seen = replyRanks(priced);
        if (!canImproveReplyOutcomes(seen,incumbent,complete,sameMenuPossible)) return false;
        if (improved)
            for (const auto &[name,value] : seen)
            {
                const auto old=incumbent.find(name);
                if (old!=incumbent.end() && value>old->second) improved->append(name);
            }
        return true;
    }

    // Only explicit work refusals justify buying another pass on this pair.
    // The fingerprint describes proof obligations, not the elapsed work.
    struct PairProofProgress
    {
        bool workLimited{},townLimited{},stockLimited{},paidLimited{};
        std::int64_t retryArmOffer{};
        QJsonArray arms;
        void observe(const QJsonObject &team)
        {
            const auto reason=team.value("unpricedReplyReason").toString();
            workLimited|=team["economicSourceInterrupted"].toBool();
            retryArmOffer=std::max<std::int64_t>(retryArmOffer,team.value("retryArmOffer").toInteger());
            townLimited|=team.value("townWorkLimitedReply").toBool();
            workLimited|=retryArmOffer>0 || reason=="MOVEMENT_LIMIT" || reason.endsWith("WORK_LIMIT") ||
                team.value("townWorkLimitedReply").toBool() || team.value("budgetRefused").toBool();
            QJsonArray policies;
            for(const auto &v:team.value("replyAlternatives").toArray())
            {
                const auto r=v.toObject();
                const auto finance=r.value("unpricedFinancialInputs").toObject();
                stockLimited|=finance.value("stockWorkLimited").toBool();
                paidLimited|=finance.contains("paidCaptureWorkGap");
                workLimited|=stockLimited || paidLimited || r.value("productionWorkLimited").toBool() || finance.value("productionWorkLimited").toBool() ||
                    finance.value("deferredStockLeg").toObject().value("status")=="DEFERRED_WORK_LIMIT";
                QJsonArray windows;
                const auto facts=r.value("townDecisionFacts").toObject();
                for(const auto &kind:{"named","broad"})for(const auto &item:facts.value(kind).toArray())
                {
                    const auto row=item.toObject();
                    windows.append(QJsonArray{row.value("tile"),row.value("attackingSide"),row.value("capper"),
                        row.value("entryTick"),row.value("status"),row.value("reason"),row.value("answered"),row.value("result")});
                }
                policies.append(QJsonArray{r.value("replyPolicy"),r.value("priceStatus"),r.value("policyTotal"),
                    r.value("optionalAttackDecisionRange"),r.value("townComparisonEvidence").toObject().value("unfinishedSlots"),windows});
            }
            arms.append(QJsonArray{team.value("unpricedReplyPolicy"),reason,team.value("priceStatus"),policies});
        }
    };

    // One instance is owned by a single live pair, after its legal orders,
    // buyer input and incumbent are fixed. Existing discovery state stays live.
    struct PairProofMeter
    {
        static constexpr int DEFAULT_MAX_PASSES=2;
        std::int64_t first;
        std::int64_t &budget,&race;
        QJsonObject &trace;
        int trial;
        std::int64_t turnCap{std::numeric_limits<std::int64_t>::max()};
        int maxPasses{DEFAULT_MAX_PASSES};
        std::int64_t offer{},savedBudget{},savedRace{};
        bool active{},roundOpen{},growTown{},growStock{},growPaid{};
        int passes{};
        std::int64_t roundStart{};
        std::set<QByteArray> frontiers;

        void endRound(const QString &outcome)
        {
            if(!roundOpen)return;
            auto ledger=trace.value("dedicatedPairProofPasses").toArray();
            auto tranche=ledger.last().toObject();auto rounds=tranche["rounds"].toArray();
            auto row=rounds.last().toObject();row["spent"]=static_cast<qint64>(roundStart-budget);
            row["remaining"]=static_cast<qint64>(budget);row["outcome"]=outcome;
            rounds[rounds.size()-1]=row;tranche["rounds"]=rounds;ledger[ledger.size()-1]=tranche;
            trace["dedicatedPairProofPasses"]=ledger;roundOpen=false;
        }
        void finish(const QString &outcome)
        {
            if(!active)return;
            endRound(outcome);
            auto ledger=trace.value("dedicatedPairProofPasses").toArray();
            auto row=ledger.last().toObject();const auto spent=offer-budget;
            row["spent"]=static_cast<qint64>(spent);row["raceSpent"]=static_cast<qint64>(race-savedRace);
            row["remaining"]=static_cast<qint64>(budget);row["outcome"]=outcome;
            ledger[ledger.size()-1]=row;trace["dedicatedPairProofPasses"]=ledger;
            trace["dedicatedPairProofWork"]=trace.value("dedicatedPairProofWork").toInteger()+static_cast<qint64>(spent);
            budget=savedBudget;race=savedRace;active=false;
        }
        bool resume(const QJsonObject &left,const QJsonObject &right,const QJsonObject &scope,std::int64_t admissionFloor=2,bool newScope=false)
        {
            if(first<=0)return false;
            PairProofProgress progress;progress.observe(left);progress.observe(right);
            // Both teams receive equal shares. A refused indivisible quote can
            // need a larger share even when this tranche still has work left.
            if(progress.retryArmOffer>std::numeric_limits<std::int64_t>::max()/2){finish("PAIR_OFFER_OVERFLOW");return false;}
            admissionFloor=std::max(admissionFloor,2*progress.retryArmOffer);
            if(!progress.workLimited && !(active && newScope)){finish("NO_CONFIRMED_WORK_LIMIT");return false;}
            const auto key=QCryptographicHash::hash(QJsonDocument(QJsonArray{progress.arms,scope}).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex();
            const bool exhausted=active && budget<admissionFloor;
            const bool changed=frontiers.insert(key).second;
            if(active && !exhausted && (!changed || roundStart<=budget))
            {finish(changed?"ZERO_CHARGED_PROGRESS":"REPEATED_OPEN_FRONTIER");return false;}
            if(!active || exhausted)
            {
                if(exhausted)finish(passes>=maxPasses?QStringLiteral("PAIR_PROOF_ALLOWANCE_EXHAUSTED"):QStringLiteral("INSUFFICIENT_REMAINING_PAIR_ALLOWANCE"));
                const auto alreadySpent=trace.value("dedicatedPairProofWork").toInteger();
                const auto turnRemaining=turnCap>alreadySpent?turnCap-alreadySpent:std::int64_t{0};
                if(turnRemaining<admissionFloor)return false;
                auto tier=passes;
                while(tier<maxPasses && first*(1LL<<tier)<admissionFloor)++tier;
                if(tier>=maxPasses)return false;
                passes=tier;
                savedBudget=budget;savedRace=race;
                offer=std::min(first*(1LL<<passes++),turnRemaining);budget=offer;active=true;
                auto ledger=trace.value("dedicatedPairProofPasses").toArray();
                ledger.append(QJsonObject{{"trial",trial},{"pass",passes},{"offer",static_cast<qint64>(offer)},
                    {"frontierSHA256",QString::fromLatin1(key)}});
                trace["dedicatedPairProofPasses"]=ledger;
            }
            else endRound("ADVANCED_OPEN_FRONTIER");
            growTown=progress.townLimited;growStock=progress.stockLimited;growPaid=progress.paidLimited;
            auto ledger=trace.value("dedicatedPairProofPasses").toArray();auto tranche=ledger.last().toObject();auto rounds=tranche["rounds"].toArray();
            rounds.append(QJsonObject{{"startingBalance",static_cast<qint64>(budget)},{"frontierSHA256",QString::fromLatin1(key)},
                {"townWorkLimited",growTown},{"stockWorkLimited",growStock},{"paidRouteWorkLimited",growPaid}});
            tranche["rounds"]=rounds;ledger[ledger.size()-1]=tranche;trace["dedicatedPairProofPasses"]=ledger;
            roundStart=budget;roundOpen=true;return true;
        }
    };

    struct OpeningHeadquartersCapture
    {
        bool gained{}, won{};
    };

    OpeningHeadquartersCapture openingHeadquartersCapture(const HeadquartersWatch &watch,
        const MasterAssignmentProblem &problem,const PropertyStockField &field,const ReplyPassEvaluation &plan)
    {
        OpeningHeadquartersCapture result;
        for (const auto &actor : plan.actors)
        {
            const auto &c = problem.actors()[actor.actorSlot].candidates[actor.candidateIndex];
            const auto *part = singleComponentOf(c.source.bundle);
            if (part && part->kind == ComponentKind::Capture &&
                std::find(watch.sites.begin(), watch.sites.end(), c.destination) != watch.sites.end() &&
                field.actionCompletesCapture({actor.stockRow, c.destination, true}))
            {
                result.gained = true;
                if (c.destination == watch.away)
                    result.won = true;
            }
        }
        return result;
    }

    void recordUnverifiedHqFinish(QJsonObject &out,const char *reason,int unit,TilePoint tileAt,
                                  int hp,int rate,int carry,int threshold)
    {
        out["priceStatus"]="UNRESOLVED";
        out["unpricedReplyReason"]=QString::fromLatin1(reason);
        out["unpricedReplyDetails"]=QJsonObject{{"phase","OPENING_HQ_CAPTURE"},{"reason",QString::fromLatin1(reason)},
            {"unit",unit},{"tile",tile(tileAt)},{"hpAfterOwn",hp},{"captureRate",rate},
            {"carry",carry},{"threshold",threshold}};
    }

    std::optional<CombatRank> headquartersRank(const HeadquartersWatch &watch, const CombatTerrain &terrain,
                                               const BattlefieldKnowledge &knowledge,
                                               const MasterAssignmentProblem &problem, const PropertyStockField &field,
                                               const ReplyPassEvaluation &plan, MilliFunds funds, QJsonObject &receipt,
                                               std::vector<int> &guardChoices, bool &needsCover)
    {
        TerminalClass terminal = TerminalClass::Unresolved;
        int guard = NO_UNIT;
        const auto ownHQ=openingHeadquartersCapture(watch,problem,field,plan);
        const bool gainedHQ=ownHQ.gained;
        if(ownHQ.won)terminal=TerminalClass::ForcedWin;
        std::vector<TilePoint> blockers;
        for (int i = 0; i < static_cast<int>(plan.combatLocations.size()); ++i)
        {
            const auto tile = plan.combatLocations[i];
            if (tile == INVALID_TILE || terrain.units[i].enemy)
                continue;
            if (tile == watch.home)
                guard = i;
            else
                blockers.push_back(tile);
        }
        bool approaching = false;
        bool canEnterBeforeReturn = false;
        std::set<int> spentEnemyActions;
        if(plan.openingRecruitFight)spentEnemyActions.insert(plan.openingRecruitFight->shooter);
        for (const auto &shot : plan.shots)
            spentEnemyActions.insert(shot.shooterIndex);
        for (const auto &order : plan.encirclement.blockOrders)
            spentEnemyActions.insert(order.unitIndex);
        if (plan.enemyWaiting != NO_UNIT)
            spentEnemyActions.insert(plan.enemyWaiting);
        QJsonArray threats;
        ActivationField reach;
        for (int i : watch.approaching)
        {
            const auto &pawn = terrain.units[i];
            const auto &unit = knowledge.units()[i];
            auto it = plan.enemyAfterOwn.find(i);
            const int hp = it == plan.enemyAfterOwn.end() ? unit.hpRounded : it->second;
            if (hp <= 0)
                continue;
            const bool standing = pawn.tile == watch.home;
            const int rate = replyCaptureRate(unit.captureRate, unit.hpRounded - hp, false);
            if(standing && !ownHQ.won && openingHqFinish(hp,rate,unit.capturePoints,field.capturePointsToCapture(),
                gainedHQ,watch.standingTerrain,false)==OpeningHqFinish::Unverified)
            {
                recordUnverifiedHqFinish(receipt,"STANDING_HQ_CAPTURE_UNVERIFIED",pawn.engineUnitId,watch.home,hp,rate,
                    unit.capturePoints,field.capturePointsToCapture());
                return {};
            }
            if (!terrain.pay(2LL * terrain.width * terrain.height))
                return {};
            const auto &grid = terrain.grids[pawn.grid];
            // Shooting or moving to seal an exit both spend E0. The alternate capture
            // terminal below is tested separately against the pre-fire position.
            const auto position = plan.combatLocations[i];
            const bool used = spentEnemyActions.contains(i);
            reach.build(pawn.ignoresCollision ? grid : grid.withBlockedTiles(blockers),
                        position == INVALID_TILE ? pawn.tile : position, pawn.movement, used ? 1 : 2,
                        {watch.home});
            int arrival = reach.activations(watch.home.x, watch.home.y);
            if (arrival > 0 && position != INVALID_TILE)
            {
                approaching = true;
                canEnterBeforeReturn |= !used && arrival == 1;
                threats.append(QJsonObject{{"unit", pawn.engineUnitId}, {"from", tile(position)},
                                           {"earliestEnemyAction", arrival - 1 + int(used)}});
            }
            if (!standing && !ownHQ.won && guard==NO_UNIT &&
                openingHqFinish(hp,rate,0,field.capturePointsToCapture(),gainedHQ,{},false)==OpeningHqFinish::Unverified)
            {
                // A mobile finisher must have a legal path BEFORE enemy gunfire clears any blockers.
                std::vector<TilePoint> before;
                for (const auto &a : plan.actors)
                    if (a.stepsAfterOwnActions > 0)
                        before.push_back(plan.replay.plan.action(a.actionIndex).destination);
                for (int k = 0; k < static_cast<int>(terrain.units.size()); ++k)
                    if (terrain.units[k].present && !terrain.units[k].enemy &&
                        std::none_of(plan.actors.begin(), plan.actors.end(), [&](const auto &a) {
                            return problem.actors()[a.actorSlot].knowledgeUnitIndex == k;
                        }))
                        before.push_back(terrain.units[k].tile);
                if (std::find(before.begin(), before.end(), watch.home) == before.end())
                {
                    if (!terrain.pay(2LL * terrain.width * terrain.height))
                        return {};
                    reach.build(pawn.ignoresCollision ? grid : grid.withBlockedTiles(before), pawn.tile, pawn.movement,
                                1, {watch.home});
                    if(reach.activations(watch.home.x,watch.home.y)==1)
                    {
                        // A mobile capper starts with zero stored points. A
                        // one-step terrain path alone cannot establish its
                        // actual capture rate, action or fresh purchase traffic.
                        recordUnverifiedHqFinish(receipt,"MOBILE_HQ_CAPTURE_UNVERIFIED",pawn.engineUnitId,watch.home,
                            hp,rate,0,field.capturePointsToCapture());
                        return {};
                    }
                }
            }
        }
        needsCover = approaching && !gainedHQ;
        receipt["hqApproaches"] = threats;
        if (terminal == TerminalClass::Unresolved && needsCover)
        {
            if (guard != NO_UNIT)
                guardChoices.push_back(guard);
            else if (!canEnterBeforeReturn)
            {
                std::vector<int> available;
                for (const auto &actor : plan.actors)
                {
                    const int i = problem.actors()[actor.actorSlot].knowledgeUnitIndex;
                    if (actor.stepsAfterReply > 0 && plan.combatLocations[i] != INVALID_TILE &&
                        knowledge.relation(knowledge.units()[i].ownerId) == Relation::Own)
                        available.push_back(i);
                }
                auto returns = headquartersReturnCandidates(terrain, plan.combatLocations, watch.home, available);
                if (!returns)
                    return {};
                guardChoices = std::move(*returns);
            }
        }
        // A legal return is conditional until the actual forecast reserves the unit
        // after the enemy has spent its immediate capture or attack action.
        // A surviving occupant prevents E0 entry even when a later volley can clear it.
        const int security = !needsCover || guard != NO_UNIT ? 0 : canEnterBeforeReturn ? -2 : -1;
        receipt["hqStatus"] = terminal == TerminalClass::ForcedWin    ? "CAPTURE_WINS_NOW"
                              : terminal == TerminalClass::ForcedLoss ? "ENEMY_CAN_WIN_NEXT_ACTION"
                              : security == -2                        ? "IMMEDIATE_ENTRY_AVAILABLE"
                              : security < 0                          ? "OPEN_APPROACH"
                              : needsCover                            ? "GUARDED_APPROACH"
                                                                      : "NO_NEAR_APPROACH";
        receipt["hqSecurity"] = security;
        receipt["terminalClass"] = static_cast<int>(terminal);
        return CombatRank{terminal, security, 0, terminal == TerminalClass::Unresolved ? funds : 0};
    }

    struct Trial
    {
        int slot, option, priority;
        int partner{-1}, partnerOption{-1};
        bool vacatesForCapture{};
        int interruptionRank{-1};
        bool competingAssignments{};
        int companion{-1}, companionOption{-1};
    };
    using Cell = std::pair<int,int>;

    // A capacity is a count of legal purchases sharing one treasury. It is a
    // search/diagnostic fact, never an amount to add to the economic score.
    struct PurchaseWindow
    {
        std::vector<std::pair<MilliFunds,Cell>> sitesByCost;
        MilliFunds treasury{};
        int unitSlots{};

        template<class Occupied>
        int affordable(Occupied occupied, int available) const
        {
            MilliFunds funds=treasury;
            int builds=0;
            for (const auto &[cost,site]:sitesByCost)
            {
                if (builds>=available || cost>funds) break;
                if (occupied(site)) continue;
                funds-=cost;
                ++builds;
            }
            return builds;
        }
    };

    struct PurchaseBoundary
    {
        std::set<Cell> occupied;
        int available{};
        std::vector<std::pair<int,TilePoint>> defenders;
    };

    PurchaseBoundary purchaseBoundary(const MasterAssignmentProblem &problem, const CombatTerrain &terrain,
                                      const ReplyPassEvaluation &team, int available)
    {
        PurchaseBoundary result{{},available};
        std::vector<bool> acted(terrain.units.size());
        for (const auto &actor:team.actors)
        {
            acted[problem.actors()[actor.actorSlot].knowledgeUnitIndex]=true;
            if (actor.stepsAfterOwnActions>0)
            {
                const auto at=team.replay.plan.action(actor.actionIndex).destination;
                result.occupied.emplace(at.x,at.y);
                result.defenders.emplace_back(problem.actors()[actor.actorSlot].knowledgeUnitIndex,at);
            }
            else if (result.available<std::numeric_limits<int>::max()) ++result.available;
        }
        for (int i=0;i<static_cast<int>(terrain.units.size());++i)
        {
            const auto &unit=terrain.units[i];
            if (!unit.present || acted[i]) continue;
            const auto hp=team.enemyAfterOwn.find(i);
            if (hp!=team.enemyAfterOwn.end() && hp->second<=0) continue;
            result.occupied.emplace(unit.tile.x,unit.tile.y);
            if (!unit.enemy) result.defenders.emplace_back(i,unit.tile);
        }
        return result;
    }

    // A gun cannot remove a screen and move through it on the same activation.
    // Keep blockers that no OTHER living, loaded hostile could even hit this
    // turn. A merely possible supporting hit is treated as a possible clearance.
    struct OpeningFireLanes
    {
        std::optional<DistanceField> movement;
        std::set<Cell> blockers;
    };

    std::optional<OpeningFireLanes> openingFireLanes(const CombatTerrain &terrain,const BattlefieldKnowledge &knowledge,
        const ReplyPassEvaluation &team,const PurchaseBoundary &boundary,int shooter)
    {
        OpeningFireLanes lanes;
        const auto &gun=terrain.units[shooter];
        if (gun.grid<0 || !knowledge.units()[shooter].canMoveAndFire || gun.movement<=0) return lanes;
        std::vector<TilePoint> blockers;
        for (const auto &[target,at]:boundary.defenders)
        {
            bool removable=false;
            for (int i=0;i<static_cast<int>(terrain.units.size()) && !removable;++i)
            {
                const auto &other=terrain.units[i];
                if (i==shooter || !other.present || !other.enemy || other.availableShots<=0) continue;
                const auto hp=team.enemyAfterOwn.find(i);
                if (hp!=team.enemyAfterOwn.end() && hp->second<=0) continue;
                if (!terrain.pay(1)) return {};
                const auto &unit=knowledge.units()[i];
                if (!reinforcementExposure(terrain,unit,other,other.tile)[at.y*terrain.width+at.x]) continue;
                for (int range=std::max(1,unit.minRange);range<=std::min(unit.maxRange,terrain.width+terrain.height);++range)
                {
                    if (!terrain.pay(1)) return {};
                    if (supportWeapon(terrain,i,target,range,other.ammoByWeapon).base>0) { removable=true; break; }
                }
            }
            if (!removable) { lanes.blockers.emplace(at.x,at.y); blockers.push_back(at); }
        }
        if (!terrain.pay(3LL*terrain.width*terrain.height)) return {};
        const auto &grid=terrain.grids[gun.grid];
        lanes.movement=DistanceField::build(gun.ignoresCollision ? grid : grid.withBlockedTiles(blockers),
                                            {gun.tile},FieldExpansion::FromSources);
        return lanes;
    }

    struct ClosingProbe
    {
        Trial trial;
        int tier;
        const char *opportunity;
        int job{}, actorVisit{}, jobVisit{}, capacityChange{};
        bool settled{}, retainedSibling{}, teammateRevision{}, coordinatedRelease{};
        std::optional<MilliFunds> localValue,openingReplyGain;
        std::optional<CombatRank> previousRank;
    };

    bool improvesNextShot(const MasterCandidate &c)
    {
        return c.actionKind==PlanBundleKind::Move &&
            bestShot(c.source.actorNextShotsAtDestination)>bestShot(c.source.actorNextShotsAtOrigin);
    }

    bool concreteIdleAction(const MasterCandidate &c)
    {
        // The native builder calculated these shot spans with weapon, ammo and range.
        return c.targetUnitId!=NO_UNIT || c.capture.has_value() || improvesNextShot(c);
    }

    // Compare a hold with a concrete use of the unit or the occupied production site.
    // The trusted master plan still authenticates endpoint dependencies and firing order.
    std::vector<ClosingProbe> closingProbes(const MasterAssignmentProblem &problem, const BattlefieldKnowledge &knowledge,
        const CombatTerrain &terrain, const std::vector<int> &anchor, const std::vector<int> &opening,
        const QJsonArray &claims,
        const std::set<Cell> &factories, const std::map<Cell,TilePoint> &dueExits, std::int64_t &work,
        QJsonArray &routes, bool &complete)
    {
        std::map<Cell,int> ends;
        const auto cell=[](TilePoint p) { return Cell{p.x,p.y}; };
        for (const auto &a:problem.actors())
            if (anchor[a.canonicalSlot]>=0)
                ends[cell(a.candidates[anchor[a.canonicalSlot]].destination)]=a.canonicalSlot;
        // A later itinerary entry begins at the preceding property. Only the first
        // booked trip is evidence for starting movement from this turn's origin.
        std::map<int,std::pair<int,TilePoint>> firstClaim;
        for (const auto &v:claims)
        {
            const auto claim=v.toObject();
            if (claim["enemy"].toBool() || claim["signedIncome"].toInteger()<=0) continue;
            const int unit=claim["unit"].toInt(NO_UNIT), departure=claim["departureDayOffset"].toInt(-1);
            if (unit==NO_UNIT || departure<0) continue;
            const auto found=firstClaim.find(unit);
            if (found==firstClaim.end() || departure<found->second.first)
                firstClaim[unit]={departure,factoryPoint(claim["tile"])};
        }
        std::vector<ClosingProbe> probes;
        for (const auto &a:problem.actors())
        {
            const int slot=a.canonicalSlot;
            if (anchor[slot]<0 || (!problem.canChangeActor(slot) && !problem.canChangeCombatHelper(a))) continue;
            const auto &current=a.candidates[anchor[slot]];
            const auto &initial=a.candidates[opening[slot]>=0 ? opening[slot] : anchor[slot]];
            const auto currentSite=cell(current.destination), initialSite=cell(initial.destination);
            const bool factory=factories.contains(currentSite);
            const bool earlierFactory=factories.contains(initialSite) && opening[slot]!=anchor[slot];
            // A first improvement off a WAIT is not a proof about its other firing lanes
            // or routes on newly selected teammates. Keep those sibling orders alive.
            const bool fromWait=initial.actionKind==PlanBundleKind::Wait && initial.destination==initial.origin;
            const bool retained=opening[slot]!=anchor[slot] && (fromWait || earlierFactory);
            const bool waiting=current.actionKind==PlanBundleKind::Wait || (retained && fromWait);
            if (!factory && !earlierFactory && !waiting) continue;
            const auto promise=dueExits.find(earlierFactory ? initialSite : currentSite);
            std::optional<DistanceField> route;
            int remaining=UNREACHABLE;
            const auto claim=firstClaim.find(a.engineUnitId);
            const auto &pawn=terrain.units[a.knowledgeUnitIndex];
            const auto &unit=knowledge.units()[a.knowledgeUnitIndex];
            if (waiting && pawn.capper && pawn.grid>=0 && claim!=firstClaim.end() && claim->second.first>0 &&
                claim->second.second!=current.origin && claim->second.second!=INVALID_TILE)
            {
                const auto charge=6LL*terrain.width*terrain.height+static_cast<std::int64_t>(terrain.units.size());
                if (work<charge) {complete=false; return probes;}
                work-=charge;
                auto grid=terrain.grids[pawn.grid];
                for (int y=0;y<grid.height();++y)
                    for (int x=0;x<grid.width();++x)
                        for (const auto dir:ENTRY_DIRECTIONS)
                            if (grid.cost(x,y,dir)>pawn.movement) grid.setCost(x,y,dir,IMPASSABLE_COST);
                if (!pawn.ignoresCollision)
                    for (const auto &other:terrain.units)
                        if (other.present && other.enemy) grid.setTileCost(other.tile.x,other.tile.y,IMPASSABLE_COST);
                const auto target=claim->second.second;
                route=DistanceField::build(grid,{target},FieldExpansion::ToTargets);
                remaining=route->distance(current.origin.x,current.origin.y);
                if (remaining==UNREACHABLE) complete=false;
                routes.append(QJsonObject{{"unit",a.engineUnitId},{"target",tile(target)},{"movementCostFromHold",remaining},
                    {"routeStatus",remaining==UNREACHABLE ? "CURRENT_BLOCKERS_ROUTE_UNRESOLVED" : "CURRENT_TERRAIN_ROUTE"}});
            }
            for (int i=0; i<static_cast<int>(a.candidates.size()); ++i)
            {
                if (work<=0) { complete=false; return probes; }
                --work;
                const auto &c=a.candidates[i];
                if (i==anchor[slot] || !c.plannable || c.immediateValue.terminal<current.immediateValue.terminal) continue;
                const bool opens=(factory && c.destination!=current.destination) || (earlierFactory && c.destination!=initial.destination);
                const int next=route ? route->distance(c.destination.x,c.destination.y) : UNREACHABLE;
                const bool approaches=c.actionKind==PlanBundleKind::Move && next>=0 && next<remaining &&
                    (unit.fuel<0 || (c.source.movementCost<=unit.fuel && next<=unit.fuel-c.source.movementCost));
                const bool restore=retained && i==opening[slot];
                if (!opens && !restore && !(waiting && (concreteIdleAction(c) || approaches))) continue;
                const char *reason=restore ? "PREVIOUS_ORDER" : opens ? "FUNDED_FACTORY_EXIT" : c.targetUnitId!=NO_UNIT ? "FIRE" : c.capture ? "CAPTURE" :
                    approaches ? "BOOKED_CAPTURE_APPROACH" : "IMPROVED_NEXT_SHOT";
                const int tier=opens ? (promise!=dueExits.end() ? (c.destination==promise->second ? 0 : 1) : 2) : 3;
                const auto blocked=ends.find(cell(c.destination));
                if (blocked==ends.end() || blocked->second==slot)
                {
                    probes.push_back({{slot,i,0},tier,reason});
                    probes.back().retainedSibling=retained;
                    continue;
                }
                // This includes a new arrival on the factory: redirect that participant
                // rather than accepting a nominal exit which immediately refills the site.
                const auto &helper=problem.actors()[blocked->second];
                if (!problem.canChangeActor(helper.canonicalSlot) && !problem.canChangeCombatHelper(helper)) continue;
                for (int j=0; j<static_cast<int>(helper.candidates.size()); ++j)
                {
                    if (work<=0) { complete=false; return probes; }
                    --work;
                    const auto &move=helper.candidates[j];
                    if (!move.plannable || move.destination==c.destination || (factory && opens && move.destination==current.destination) ||
                        (earlierFactory && opens && move.destination==initial.destination) ||
                        move.immediateValue.terminal<helper.candidates[anchor[helper.canonicalSlot]].immediateValue.terminal) continue;
                    const auto occupied=ends.find(cell(move.destination));
                    if (occupied!=ends.end() && occupied->second!=slot && occupied->second!=helper.canonicalSlot) continue;
                    probes.push_back({{slot,i,0,helper.canonicalSlot,j},tier,reason});
                    probes.back().retainedSibling=retained;
                }
            }
        }
        return probes;
    }

    // Price actual support when a committed lethal reply had very few physical firing origins.
    // The screen is an admission witness; the subsequent full replay checks whether vacating its
    // old square opens another origin or lets a different enemy take the shot.
    void addLethalRescues(std::vector<Trial> &trials,const MasterAssignmentProblem &problem,
        const std::vector<int> &anchor,const ReplyPassEvaluation &reference,const QJsonArray &responses,const CombatTerrain &terrain)
    {
        auto threats = reference.lethalThreats;
        for (const auto &entry : responses)
            for (const auto &v : entry.toObject().value("lethalFiringThreats").toArray())
            {
                const auto r = v.toObject();
                LethalFiringThreat t{r["victim"].toInt(NO_UNIT),r["shooterKnowledge"].toInt(NO_UNIT),{}};
                for (const auto &point : r["origins"].toArray())
                {
                    const auto xy=point.toArray();
                    if (xy.size()==2) t.origins.push_back({xy[0].toInt(),xy[1].toInt()});
                }
                if (!t.origins.empty() && t.origins.size()<=2 &&
                    std::none_of(threats.begin(),threats.end(),[&](const auto &x) {
                        return x.victim==t.victim && x.shooter==t.shooter && x.origins==t.origins;
                    })) threats.push_back(std::move(t));
            }
        if (threats.empty()) return;
        auto admit = [&](Trial t) {
            auto old=std::find_if(trials.begin(),trials.end(),[&](const Trial &o) {
                return o.slot==t.slot && o.option==t.option && o.partner==t.partner && o.partnerOption==t.partnerOption;
            });
            if (old==trials.end()) trials.push_back(t);
            else if (t.priority < old->priority) old->priority=t.priority;
        };
        struct Screen {
            int slot, option;
            bool keepsShot;
            MilliFunds change;
        };
        // One early rescue is sufficient to seed another neighborhood. Bound the
        // optional scans and leave the current measured plan alone if work runs out.
        const auto candidates=std::accumulate(problem.actors().begin(),problem.actors().end(),std::int64_t{},
            [](auto sum,const auto &a){return sum+static_cast<std::int64_t>(a.candidates.size());});
        if (threats.size()>6) threats.resize(6);
        bool firstRescue=true;
        for (const auto &t : threats)
        {
            if (!terrain.pay(4*candidates+64)) break;
            const auto victim=std::find_if(problem.actors().begin(),problem.actors().end(),[&](const auto &a) {
                return a.engineUnitId==t.victim;
            });
            if (victim==problem.actors().end() || anchor[victim->canonicalSlot]<0) continue;
            std::vector<std::vector<Screen>> byOrigin(t.origins.size());
            for (const auto &actor : problem.actors())
            {
                const int slot=actor.canonicalSlot,current=anchor[slot];
                if (current<0 || (!problem.canChangeActor(slot) && !problem.canChangeCombatHelper(actor))) continue;
                const auto &before=actor.candidates[current];
                for (int i=0;i<static_cast<int>(actor.candidates.size());++i)
                {
                    const auto &c=actor.candidates[i];
                    if (i==current || !c.plannable || c.immediateValue.terminal < before.immediateValue.terminal) continue;
                    if (slot==victim->canonicalSlot)
                    {
                        if (before.destination!=before.origin && c.actionKind==PlanBundleKind::Wait && c.destination==before.origin)
                            admit({slot,i,-4});
                        continue;
                    }
                    auto origin=std::find(t.origins.begin(),t.origins.end(),c.destination);
                    if (origin==t.origins.end() ||
                        hpAfterAction(c.source.bundle,c.source.actor.hpSteps).hpSteps<=0 ||
                        problem.incompatible({slot,i},{victim->canonicalSlot,anchor[victim->canonicalSlot]})) continue;
                    const bool keepsShot=before.targetUnitId!=NO_UNIT && before.targetUnitId==c.targetUnitId;
                    auto change=c.immediateValue.economicValue;
                    if (!RelativeStockDetail::checkedSubtract(change,before.immediateValue.economicValue)) continue;
                    byOrigin[origin-t.origins.begin()].push_back({slot,i,keepsShot,change});
                }
            }
            const auto rank=[](const Screen &a,const Screen &b) {
                if (a.keepsShot!=b.keepsShot) return a.keepsShot;
                if (a.change!=b.change) return a.change>b.change;
                return std::pair{a.slot,a.option}<std::pair{b.slot,b.option};
            };
            for (auto &options:byOrigin)
            {
                const auto count=std::min<std::size_t>(8,options.size());
                std::partial_sort(options.begin(),options.begin()+count,options.end(),rank);
                options.resize(count);
            }
            if (byOrigin.front().empty()) continue;
            if (byOrigin.size()==1)
            {
                // Keep a second helper/alternative when available, without letting one
                // threatened unit monopolize the whole team's first search neighborhood.
                const auto &options=byOrigin.front();
                for (int k=0;k<std::min<int>(2,options.size());++k)
                {
                    const auto &s=options[k];
                    const int priority=firstRescue && k==0 ? -7 : -4;
                    admit({s.slot,s.option,priority});
                    // At the original anchor this is a duplicate concrete choice and costs
                    // no replay. It remains distinct if a later anchor withdrew the victim.
                    admit({s.slot,s.option,priority,victim->canonicalSlot,anchor[victim->canonicalSlot]});
                }
                firstRescue=false;
            }
            else
            {
                bool paired=false;
                for (const auto &a:byOrigin[0])
                {
                    for (const auto &b:byOrigin[1])
                        if (a.slot!=b.slot && !problem.incompatible({a.slot,a.option},{b.slot,b.option}))
                        {
                            admit({a.slot,a.option,firstRescue ? -7 : -4,b.slot,b.option});
                            firstRescue=false; paired=true; break;
                        }
                    if (paired) break;
                }
            }
        }
    }
    std::vector<Trial> combatTrials(const MasterAssignmentProblem &problem, const std::vector<int> &anchor,
                                    const ReplyPassEvaluation &reference, const BattlefieldKnowledge &knowledge,
                                    bool priceOpponent, const HeadquartersWatch &hq, const std::set<int> &potentialCappers,
                                    std::set<int> &watched,
                                    const QJsonArray &responses = {}, const CombatTerrain *rescueTerrain = nullptr)
    {
        std::set<int> shooters, threatened, threatenedHelpers, boxed, lostInOwnAttack, lostInReply;
        std::map<std::pair<int,int>,int> destinations;
        for (const auto &a : reference.actors)
        {
            const auto end = reference.replay.plan.action(a.actionIndex).destination;
            destinations[{end.x,end.y}] = a.actorSlot;
            if (a.stepsAfterOwnActions == 0)
                lostInOwnAttack.insert(a.actorSlot);
            else if (a.stepsAfterReply == 0)
                lostInReply.insert(a.actorSlot);
        }
        std::vector<TilePoint> firingTiles;
        for (const auto &response : responses)
        {
            const auto trap = response.toObject().value("encirclement").toObject();
            if (trap.value("sealed").toBool())
                boxed.insert(trap.value("target").toInt());
        }
        std::vector<std::pair<int,int>> garrisons, immediateCaptures;
        const bool urgentHQ = hq.standing != NO_UNIT || std::any_of(responses.begin(),responses.end(),[](const auto &reply) {
            const auto r = reply.toObject();
            return r.value("terminalClass").toInt() < 0 || r.value("hqSecurity").toInt() < 0;
        });
        for (const auto &shot : reference.shots)
        {
            if(shot.target>=reference.actors.size())continue;
            const auto &victim = reference.actors[shot.target];
            if (victim.stockRow == NO_STOCK_ROW)
            {
                if (!priceOpponent || !problem.canChangeCombatHelper(problem.actors()[victim.actorSlot]))
                    continue;
                threatenedHelpers.insert(victim.actorSlot);
                watched.insert(victim.engineUnitId);
                shooters.insert(shot.shooterIndex);
                firingTiles.push_back(shot.firingTile);
                continue;
            }
            threatened.insert(victim.actorSlot);
            watched.insert(victim.engineUnitId);
            shooters.insert(shot.shooterIndex);
            firingTiles.push_back(shot.firingTile);
        }
        // The reply that threatens a different capturer can be a focused attack rather
        // than the economic reply. Admit rescues and counterattacks against either one.
        for (auto response : responses)
            for (auto entry : response.toObject()["shots"].toArray())
            {
                const auto shot = entry.toObject();
                const int id = shot["target"].toInt();
                auto victim = std::find_if(reference.actors.begin(), reference.actors.end(),
                                           [&](const auto &a) { return a.engineUnitId == id; });
                if (victim == reference.actors.end())
                    continue;
                if (victim->stockRow == NO_STOCK_ROW)
                {
                    if (!priceOpponent || !problem.canChangeCombatHelper(problem.actors()[victim->actorSlot]))
                        continue;
                    threatenedHelpers.insert(victim->actorSlot);
                    watched.insert(id);
                    shooters.insert(shot["shooterKnowledge"].toInt());
                    const auto from = shot["from"].toArray();
                    if (from.size() == 2)
                        firingTiles.push_back({from[0].toInt(), from[1].toInt()});
                    continue;
                }
                threatened.insert(victim->actorSlot);
                watched.insert(id);
                shooters.insert(shot["shooterKnowledge"].toInt());
                const auto from = shot["from"].toArray();
                if (from.size() == 2)
                    firingTiles.push_back({from[0].toInt(), from[1].toInt()});
            }
        std::vector<Trial> trials;
        for (const auto &actor : problem.actors())
        {
            const bool capper = threatened.contains(actor.canonicalSlot) && problem.canChangeActor(actor.canonicalSlot);
            if (!capper && !problem.canChangeCombatHelper(actor) &&
                !(priceOpponent && problem.canChangeActor(actor.canonicalSlot)))
                continue;
            for (int i = 0; i < static_cast<int>(actor.candidates.size()); ++i)
            {
                const auto &c = actor.candidates[i];
                const int current = anchor[actor.canonicalSlot];
                if (!c.plannable || i == current || current < 0 ||
                    c.immediateValue.terminal < actor.candidates[current].immediateValue.terminal)
                    continue;
                const auto *component = singleComponentOf(c.source.bundle);
                const bool attacks = component && component->kind == ComponentKind::Fire &&
                                     shooters.contains(component->fire.targetUnitId);
                const bool screens =
                    std::find(firingTiles.begin(), firingTiles.end(), c.destination) != firingTiles.end();
                const bool interrupts = priceOpponent && component && component->kind == ComponentKind::Fire &&
                                        knowledge.units().at(component->fire.targetUnitId).capturePoints > 0 &&
                                        knowledge.units().at(component->fire.targetUnitId).canCapture;
                const auto selected = actor.candidates[current].destination;
                const bool changesHqTransit = selected != c.destination &&
                    (hq.transit.contains({c.destination.x,c.destination.y}) || hq.transit.contains({selected.x,selected.y}));
                const bool objective = changesHqTransit ||
                    (hq.away != INVALID_TILE && c.destination == hq.away && component &&
                     component->kind == ComponentKind::Capture) ||
                    (!hq.approaching.empty() &&
                     (c.destination == hq.home || (component && component->kind == ComponentKind::Fire &&
                                                   hq.approaching.contains(component->fire.targetUnitId))));
                const bool preemptsCapture = priceOpponent && component && component->kind == ComponentKind::Fire &&
                                             potentialCappers.contains(component->fire.targetUnitId);
                const bool startsCapture = priceOpponent && component && component->kind == ComponentKind::Capture;
                const bool finishes = startsCapture && c.destination == c.origin && component->capture.turnsUntilOwned == 0;
                const bool holds = priceOpponent && !knowledge.units()[actor.knowledgeUnitIndex].canCapture &&
                                   c.destination == c.origin && actor.candidates[current].destination != c.origin &&
                                   !shooters.empty();
                if (priceOpponent && !hq.approaching.empty())
                {
                    if (finishes)
                        immediateCaptures.emplace_back(actor.canonicalSlot, i);
                    if (c.destination == hq.home && !component)
                        garrisons.emplace_back(actor.canonicalSlot, i);
                }
                if (startsCapture)
                {
                    // A single change to this capture cannot work when another chosen move claims its tile.
                    // Price the orders together, including when the blocker intended to enter a still-empty city.
                    const auto taken = destinations.find({c.destination.x,c.destination.y});
                    if (taken != destinations.end() && taken->second != actor.canonicalSlot)
                    {
                        const auto &blocker = problem.actors()[taken->second];
                        const int held = anchor[blocker.canonicalSlot];
                        if (problem.canChangeActor(blocker.canonicalSlot) || problem.canChangeCombatHelper(blocker))
                            for (int j = 0; j < static_cast<int>(blocker.candidates.size()); ++j)
                            {
                                const auto &vacate = blocker.candidates[j];
                                if (held < 0 || j == held || !vacate.plannable || vacate.destination == c.destination ||
                                    vacate.immediateValue.terminal < blocker.candidates[held].immediateValue.terminal)
                                    continue;
                                const auto collision = destinations.find({vacate.destination.x, vacate.destination.y});
                                if (collision != destinations.end() && collision->second != actor.canonicalSlot &&
                                    collision->second != blocker.canonicalSlot)
                                    continue;
                                // Price a real, immediately payable capture before spending the search
                                // on unrelated diversions. The alternative is decline-and-restart, so
                                // include a physically compatible order for the arriving teammate.
                                const bool canStay = vacate.destination == vacate.origin;
                                const int priority = finishes && canStay && !urgentHQ ? -6 : finishes ? -3 : 0;
                                trials.push_back({actor.canonicalSlot, i, priority, blocker.canonicalSlot, j, true});
                            }
                    }
                }
                const bool avoidsBox = boxed.contains(actor.engineUnitId) &&
                                       c.destination != actor.candidates[current].destination;
                const auto &unit = knowledge.units()[actor.knowledgeUnitIndex];
                const bool savesAttacker = lostInOwnAttack.contains(actor.canonicalSlot) ||
                                           lostInReply.contains(actor.canonicalSlot);
                const bool threatenedHelper = threatenedHelpers.contains(actor.canonicalSlot) &&
                                              problem.canChangeCombatHelper(actor);
                const bool opensFactory = c.source.vacatesActiveFriendlyProduction;
                const bool stages = !component && (unit.canCapture || (unit.hasWeapons && !unit.canMoveAndFire));
                if (!startsCapture && !holds && !objective && !capper && !threatenedHelper && !screens && !attacks && !interrupts && !avoidsBox &&
                    !(priceOpponent && (preemptsCapture || savesAttacker || opensFactory || stages)))
                    continue;
                const auto committed = destinations.find({c.destination.x,c.destination.y});
                const bool openFinish = finishes && knowledge.units()[actor.knowledgeUnitIndex].capturePoints > 0 &&
                    actor.candidates[current].destination != c.origin &&
                    (committed == destinations.end() || committed->second == actor.canonicalSlot);
                const int priority = openFinish && !urgentHQ     ? -6
                                     : objective                 ? -4
                                     : finishes                  ? -3
                                     : holds                     ? -2
                                     : interrupts || savesAttacker ? -1
                                     : startsCapture || preemptsCapture || opensFactory || screens || avoidsBox ? 0
                                     : attacks                   ? 1
                                     : c.destination == c.origin ? 2
                                                                 : 3;
                trials.push_back({actor.canonicalSlot, i, priority});
            }
        }
        // Recompare the two simultaneous orders even if the intervening one-unit plan would
        // temporarily empty the HQ. The engine replay still checks vacating and execution order.
        for (auto [guard, hold] : garrisons)
            for (auto [capper, finish] : immediateCaptures)
                if (capper != guard)
                {
                    const auto &a = problem.actors()[capper].candidates[finish];
                    const auto &b = problem.actors()[guard].candidates[hold];
                    auto thirdClaim = [&](TilePoint end) {
                        const auto i = destinations.find({end.x,end.y});
                        return i != destinations.end() && i->second != capper && i->second != guard;
                    };
                    // Preserve the proposed orders for replay, but do not put a known
                    // three-unit collision ahead of an executable two-unit finish.
                    const bool conflict = a.destination == b.destination || thirdClaim(a.destination) || thirdClaim(b.destination);
                    trials.push_back({capper, finish, conflict ? 3 : -5, guard, hold});
                }
        std::stable_sort(trials.begin(), trials.end(),
                         [](const Trial &a, const Trial &b) { return a.priority < b.priority; });
        if (priceOpponent && rescueTerrain) addLethalRescues(trials,problem,anchor,reference,responses,*rescueTerrain);
        return trials;
    }

    void addJointCombatOrders(std::vector<Trial> &shortlist, const MasterAssignmentProblem &problem,
                              const ReplyPassEvaluation &reference, const BattlefieldKnowledge &knowledge,
                              const HeadquartersWatch &hq, const std::set<int> &potentialCappers,
                              const CombatTerrain *terrain, std::int64_t geometry,
                              const std::vector<int> &choices, const PropertyStockField &field, QJsonObject &result)
    {
        const auto hasIncumbent = [&](const MasterActor &actor) {
            const int choice = choices[actor.canonicalSlot];
            return choice >= 0 && choice < static_cast<int>(actor.candidates.size());
        };
        struct PairChoice
        {
            Trial trial;
            int priority;
        };
        std::vector<PairChoice> pairs;
        std::int64_t pairWork = std::min<std::int64_t>(geometry / 12, 100000);
        std::set<std::pair<int, int>> loneHelpers;
        for (const auto &capper : problem.actors())
        {
            if (pairWork <= 0)
                break;
            if (!hasIncumbent(capper) || !problem.canChangeActor(capper.canonicalSlot) ||
                capper.stockRow == NO_STOCK_ROW)
                continue;
            for (int i = 0; i < static_cast<int>(capper.candidates.size()); ++i)
            {
                if (pairWork <= 0)
                    break;
                const auto &cap = capper.candidates[i];
                const auto *part = singleComponentOf(cap.source.bundle);
                if (!cap.plannable || !part || part->kind != ComponentKind::Capture || !cap.source.replyFacts ||
                    cap.source.replyFacts->tile->shooters.empty() ||
                    cap.immediateValue.terminal <
                        capper.candidates[choices[capper.canonicalSlot]].immediateValue.terminal)
                    continue;
                for (const auto &helper : problem.actors())
                {
                    if (pairWork <= 0)
                        break;
                    if (!hasIncumbent(helper) || !problem.canChangeCombatHelper(helper) ||
                        knowledge.units()[helper.knowledgeUnitIndex].canCapture)
                        continue;
                    for (int j = 0; j < static_cast<int>(helper.candidates.size()); ++j)
                    {
                        std::int64_t cost = 1;
                        for (const auto &th : cap.source.replyFacts->tile->shooters)
                            cost += 1 + th.firingTiles.size();
                        if (cost > pairWork || !terrain->pay(cost))
                        {
                            pairWork = 0;
                            break;
                        }
                        pairWork -= cost;
                        const auto &support = helper.candidates[j];
                        if (!support.plannable ||
                            support.immediateValue.terminal <
                                helper.candidates[choices[helper.canonicalSlot]].immediateValue.terminal ||
                            j == choices[helper.canonicalSlot] || support.destination == cap.destination)
                            continue;
                        const auto *shot = singleComponentOf(support.source.bundle);
                        bool attack = false, screen = false;
                        for (const auto &threat : cap.source.replyFacts->tile->shooters)
                        {
                            attack |= shot && shot->kind == ComponentKind::Fire &&
                                      shot->fire.targetUnitId == threat.shooterIndex;
                            screen |= std::find(threat.firingTiles.begin(), threat.firingTiles.end(),
                                                support.destination) != threat.firingTiles.end();
                        }
                        if ((attack || screen) && i == choices[capper.canonicalSlot])
                        {
                            if (loneHelpers.emplace(helper.canonicalSlot, j).second &&
                                std::none_of(shortlist.begin(), shortlist.end(), [&](const Trial &t) {
                                    return t.slot == helper.canonicalSlot && t.option == j;
                                }))
                                pairs.push_back({{helper.canonicalSlot, j, 0}, 3});
                        }
                        else if (attack || screen)
                            pairs.push_back({{capper.canonicalSlot, i, 0, helper.canonicalSlot, j},
                                             (part->capture.turnsUntilOwned == 0 ? 0 : 2) + (attack ? 0 : 1)});
                    }
                }
            }
        }
        std::stable_sort(pairs.begin(), pairs.end(),
                         [](const auto &a, const auto &b) { return a.priority < b.priority; });
        auto insertion = shortlist.begin() + std::min<std::size_t>(shortlist.size(), 12);
        for (std::size_t n = 0; n < std::min<std::size_t>(pairs.size(), 8); ++n)
            insertion = shortlist.insert(insertion, pairs[n].trial) + 1;
        result["supportPairsGenerated"] = static_cast<qint64>(pairs.size());
        // Admit coordinated fire against a capturer or a shooter threatening a capture.
        // The ordinary master replay owns target HP capacity, action ordering and vacated tiles.
        std::set<int> tacticalTargets = hq.approaching;
        tacticalTargets.insert(potentialCappers.begin(), potentialCappers.end());
        for (int i = 0; i < static_cast<int>(terrain->units.size()); ++i)
            if (terrain->units[i].enemy && terrain->units[i].carrying)
                tacticalTargets.insert(i);
        for (const auto &a : reference.actors)
        {
            const auto &c = problem.actors()[a.actorSlot].candidates[a.candidateIndex];
            const auto *part = singleComponentOf(c.source.bundle);
            if (!part || part->kind != ComponentKind::Capture || !c.source.replyFacts)
                continue;
            for (const auto &sh : c.source.replyFacts->tile->shooters)
                tacticalTargets.insert(sh.shooterIndex);
        }
        struct FireOption
        {
            int slot, option, damage, counter;
        };
        std::map<int, std::vector<FireOption>> attacksByTarget;
        std::int64_t fireWork = std::min<std::int64_t>(geometry / 16, 40000);
        for (const auto &actor : problem.actors())
        {
            if (!hasIncumbent(actor) ||
                (!problem.canChangeActor(actor.canonicalSlot) && !problem.canChangeCombatHelper(actor)))
                continue;
            std::map<int, std::vector<FireOption>> options;
            for (int i = 0; i < static_cast<int>(actor.candidates.size()); ++i)
            {
                if (fireWork <= 0 || !terrain->pay(1))
                    break;
                --fireWork;
                const auto &c = actor.candidates[i];
                const auto *part = singleComponentOf(c.source.bundle);
                if (!c.plannable || !part || part->kind != ComponentKind::Fire ||
                    !tacticalTargets.contains(part->fire.targetUnitId) ||
                    c.immediateValue.terminal < actor.candidates[choices[actor.canonicalSlot]].immediateValue.terminal)
                    continue;
                options[part->fire.targetUnitId].push_back(
                    {actor.canonicalSlot, i, part->fire.damageSteps, part->fire.counterSteps});
            }
            for (auto &[id, opts] : options)
            {
                std::stable_sort(opts.begin(), opts.end(), [](const auto &a, const auto &b) {
                    return std::pair{-a.damage, a.counter} < std::pair{-b.damage, b.counter};
                });
                // A direct weapon has four adjacent firing endpoints. A second
                // attacker can occupy one, making a tied third endpoint critical.
                const auto limit = terrain->units[id].carrying ? 4u : 2u;
                for (std::size_t j = 0; j < std::min<std::size_t>(opts.size(), limit); ++j)
                    attacksByTarget[id].push_back(opts[j]);
            }
        }
        struct InterruptPair
        {
            Trial trial;
            CaptureInterruptionEstimate forecast;
            int target, first;
            bool holdingCapper{}, opposite{};
        };
        std::vector<PairChoice> dualFire;
        std::map<std::tuple<int,int,int>,std::vector<InterruptPair>> activePairs, activeHolds;
        std::map<std::tuple<int,int,int>,int> waits;
        for (const auto &actor : problem.actors())
            for (int n=0;n<static_cast<int>(actor.candidates.size());++n)
            {
                if (fireWork <= 0 || !terrain->pay(1)) { fireWork=0; break; }
                --fireWork;
                const auto &c=actor.candidates[n];
                if (c.plannable && (c.actionKind==PlanBundleKind::Move || c.actionKind==PlanBundleKind::Wait))
                    waits.try_emplace({actor.canonicalSlot,c.destination.x,c.destination.y},n);
            }
        std::map<std::pair<int,int>,int> occupied;
        for (const auto &actor : reference.actors)
        {
            const auto end = reference.replay.plan.action(actor.actionIndex).destination;
            occupied[{end.x,end.y}] = actor.actorSlot;
        }
        const auto outranks = [](const InterruptPair &a, const InterruptPair &b) {
            const auto rank = [](const InterruptPair &p) {
                const auto &f = p.forecast;
                return std::tuple{f.turnsBefore == 1 && f.turnsAfter > 1,
                                  f.turnsAfter-f.turnsBefore,f.exchange,-f.targetAfter};
            };
            return rank(a)>rank(b);
        };
        for (const auto &[target, options] : attacksByTarget)
        {
            std::optional<PairChoice> bestPair;
            for (std::size_t i = 0; i < options.size(); ++i)
                for (std::size_t j = i + 1; j < options.size(); ++j)
                {
                    if (fireWork <= 0 || !terrain->pay(1))
                        break;
                    --fireWork;
                    const auto &a = options[i], &b = options[j];
                    if (a.slot == b.slot || (a.option == choices[a.slot] && b.option == choices[b.slot]))
                        continue;
                    const auto &one = problem.actors()[a.slot].candidates[a.option],
                               &two = problem.actors()[b.slot].candidates[b.option];
                    if (terrain->units[target].carrying)
                    {
                        const auto thirdPartyEnd = [&](TilePoint tile) {
                            const auto it = occupied.find({tile.x,tile.y});
                            return it != occupied.end() && it->second != a.slot && it->second != b.slot;
                        };
                        if (problem.incompatible({a.slot,a.option},{b.slot,b.option}) ||
                            thirdPartyEnd(one.destination) || thirdPartyEnd(two.destination)) continue;
                        if (fireWork < 8 || !terrain->pay(8)) { fireWork = 0; break; }
                        fireWork -= 8;
                        std::optional<InterruptPair> best;
                        for (bool reverse : {false,true})
                        {
                            const auto &lead = reverse ? two : one, &second = reverse ? one : two;
                            auto outcome = measureCaptureInterruption(*terrain,knowledge.units()[target],
                                lead.source,second.source,field.capturePointsToCapture(),field.horizonTurns());
                            const auto &gun = reverse ? b : a, &support = reverse ? a : b;
                            const auto &friendActor=problem.actors()[support.slot];
                            const auto waiter=waits.find({support.slot,second.destination.x,second.destination.y});
                            // The same move can support a shot by taking/holding its endpoint
                            // without paying the counter or losing the capper's future speed.
                            if (waiter!=waits.end() && waiter->second!=choices[support.slot] &&
                                gun.option!=choices[gun.slot] &&
                                !problem.incompatible({gun.slot,gun.option},{support.slot,waiter->second}))
                            {
                                auto alone=measureCaptureInterruption(*terrain,knowledge.units()[target],
                                    lead.source,second.source,field.capturePointsToCapture(),field.horizonTurns(),false);
                                if (alone)
                                {
                                    const auto home=terrain->units[target].tile;
                                    const bool opposite=lead.destination.x+second.destination.x==2*home.x &&
                                                        lead.destination.y+second.destination.y==2*home.y;
                                    activeHolds[{target,gun.slot,support.slot}].push_back({
                                        {gun.slot,gun.option,-2,support.slot,waiter->second},*alone,target,
                                        problem.actors()[gun.slot].engineUnitId,
                                        knowledge.units()[friendActor.knowledgeUnitIndex].canCapture,opposite});
                                }
                            }
                            if (!outcome) continue;
                            InterruptPair proposal{{a.slot,a.option,-2,b.slot,b.option},*outcome,target,
                                problem.actors()[reverse ? b.slot : a.slot].engineUnitId};
                            if (!best || outranks(proposal,*best)) best = proposal;
                        }
                        if (best)
                            activePairs[{target,a.slot,b.slot}].push_back(*best);
                        continue;
                    }
                    if (one.destination == two.destination ||
                        std::max(a.damage, b.damage) >= knowledge.units()[target].hpRounded)
                        continue;
                    int strength = std::min(knowledge.units()[target].hpRounded, a.damage + b.damage);
                    int score = 100 * (target == hq.standing) + 10 * strength - a.counter - b.counter;
                    PairChoice pair{{a.slot, a.option, 0, b.slot, b.option}, -score};
                    if (!bestPair || pair.priority < bestPair->priority)
                        bestPair = pair;
                }
            if (bestPair)
                dualFire.push_back(*bestPair);
        }
        std::stable_sort(dualFire.begin(), dualFire.end(),
                         [](const auto &a, const auto &b) { return a.priority < b.priority; });
        auto fireAt = shortlist.begin() + std::min<std::size_t>(6, shortlist.size());
        for (std::size_t i = 0; i < std::min<std::size_t>(4, dualFire.size()); ++i)
            fireAt = shortlist.insert(fireAt, dualFire[i].trial) + 1;
        // Compare distinct contributors before spending the remaining openings
        // on a second firing geometry for the same pair. The two actual firing
        // orders compete under the ordinary full-team score for every admission.
        for (auto &[pair, options] : activePairs)
            std::stable_sort(options.begin(),options.end(),outranks);
        QJsonArray admissions;
        std::set<int> admittedTargets;
        for (std::size_t wave = 0; wave < 2; ++wave)
        {
            std::vector<InterruptPair> waveOptions;
            for (const auto &[pair, options] : activePairs)
                if (wave < options.size()) waveOptions.push_back(options[wave]);
            std::stable_sort(waveOptions.begin(),waveOptions.end(),outranks);
            for (int n = 0; n < std::min<int>(6,waveOptions.size()); ++n)
            {
                auto &p = waveOptions[n];
                auto &t = p.trial;
                t.interruptionRank = 2 * static_cast<int>(admissions.size());
                t.priority = wave == 0 ? -2 : -1;
                shortlist.push_back(t);
                const auto &a = problem.actors()[t.slot], &b = problem.actors()[t.partner];
                const auto &c = a.candidates[t.option], &d = b.candidates[t.partnerOption];
                admissions.append(QJsonObject{{"target",terrain->units[p.target].engineUnitId},{"tile",tile(c.target)},
                    {"actors",QJsonArray{a.engineUnitId,b.engineUnitId}},{"candidates",QJsonArray{t.option,t.partnerOption}},
                    {"from",QJsonArray{tile(c.destination),tile(d.destination)}},{"first",p.first},
                    {"captureTurnsBefore",p.forecast.turnsBefore},{"captureTurnsAfter",p.forecast.turnsAfter},
                    {"targetAfter",p.forecast.targetAfter},{"exchange",static_cast<qint64>(p.forecast.exchange)}});
                admittedTargets.insert(p.target);
            }
        }
        // Distribute the early comparisons across different shooters. Keep an
        // opposite-side configuration when equally good direct fire allows
        // the same pair to occupy either adjacent or opposing sides.
        const auto holdFirst = [&](const InterruptPair &a,const InterruptPair &b) {
            if (a.holdingCapper != b.holdingCapper) return a.holdingCapper;
            if (outranks(a,b)) return true;
            if (outranks(b,a)) return false;
            return a.opposite && !b.opposite;
        };
        std::map<int,std::vector<InterruptPair>> byShooter;
        for (auto &[pair, options] : activeHolds)
        {
            std::stable_sort(options.begin(),options.end(),holdFirst);
            for (int i=0;i<std::min<int>(2,options.size());++i)
                byShooter[options[i].first].push_back(options[i]);
        }
        for (auto &[gun, options] : byShooter)
            std::stable_sort(options.begin(),options.end(),holdFirst);
        QJsonArray holds;
        for (std::size_t wave=0;wave<3 && holds.size()<12;++wave)
        {
            std::vector<InterruptPair> batch;
            for (const auto &[gun,options] : byShooter)
                if (wave<options.size()) batch.push_back(options[wave]);
            std::stable_sort(batch.begin(),batch.end(),holdFirst);
            for (auto &p : batch)
            {
                if (holds.size()>=12) break;
                auto &t=p.trial;
                t.interruptionRank=2*static_cast<int>(holds.size())+1;
                t.priority=wave==0 ? -2 : -1;
                shortlist.push_back(t);
                const auto &a=problem.actors()[t.slot], &b=problem.actors()[t.partner];
                holds.append(QJsonObject{{"target",terrain->units[p.target].engineUnitId},{"fire",a.engineUnitId},
                    {"support",b.engineUnitId},{"from",tile(a.candidates[t.option].destination)},
                    {"hold",tile(b.candidates[t.partnerOption].destination)},
                    {"candidates",QJsonArray{t.option,t.partnerOption}},
                    {"captureTurnsBefore",p.forecast.turnsBefore},{"captureTurnsAfter",p.forecast.turnsAfter},
                    {"targetAfter",p.forecast.targetAfter},{"exchange",static_cast<qint64>(p.forecast.exchange)}});
            }
        }
        result["activeCaptureInterruptions"] = admissions;
        result["activeCaptureSupport"] = holds;
        result["combinedAttackTargets"] = static_cast<qint64>(dualFire.size()+admittedTargets.size());
    }

    struct ProjectedTravel
    {
        std::vector<MobilityCostGrid> grids;
        std::vector<int> slotsByUnit;
    };

    std::vector<TilePoint> occupants(const CombatTerrain &terrain, const ReplyPassEvaluation &plan, bool enemy)
    {
        std::vector<TilePoint> result;
        for (std::size_t i = 0; i < terrain.units.size(); ++i)
            if (terrain.units[i].enemy == enemy && plan.combatLocations[i] != INVALID_TILE)
                result.push_back(plan.combatLocations[i]);
        const auto order = [](TilePoint a, TilePoint b) { return std::tie(a.y, a.x) < std::tie(b.y, b.x); };
        std::sort(result.begin(), result.end(), order);
        return result;
    }

    ProjectedTravel friendlyTravel(const CombatTerrain &terrain, const ReplyPassEvaluation &plan)
    {
        const auto blockers = occupants(terrain, plan, true);
        ProjectedTravel travel;
        std::map<std::pair<int, bool>, int> classes;
        for (const auto &pawn : terrain.units)
        {
            int slot = -1;
            if (!pawn.enemy && pawn.grid >= 0)
            {
                auto [at, fresh] = classes.try_emplace({pawn.grid, pawn.ignoresCollision}, travel.grids.size());
                slot = at->second;
                if (fresh)
                    travel.grids.push_back(pawn.ignoresCollision ? terrain.grids[pawn.grid]
                                                                 : terrain.grids[pawn.grid].withBlockedTiles(blockers));
            }
            travel.slotsByUnit.push_back(slot);
        }
        return travel;
    }

    // Holding all projected occupants in place for continuation, prove that no enemy capture route
    // becomes faster. This includes the first leg and every possible subsequent property-to-property leg.
    bool enemyRoutesNoFaster(const CombatTerrain &terrain, const BattlefieldKnowledge &knowledge,
                             const PropertyStockField &field, const ReplyPassEvaluation &before,
                             const ReplyPassEvaluation &after, std::int64_t &budget)
    {
        const auto oldTiles = occupants(terrain, before, false);
        const auto newTiles = occupants(terrain, after, false);
        std::vector<TilePoint> targets;
        for (const auto &building : knowledge.buildings())
            if (field.columnSlotAt({building.x, building.y}) != NO_STOCK_COLUMN)
                targets.push_back({building.x, building.y});
        std::map<std::pair<int, int>, std::set<std::pair<int, int>>> origins;
        for (std::size_t i = 0; i < terrain.units.size(); ++i)
        {
            const auto &pawn = terrain.units[i];
            if (!pawn.enemy || !knowledge.units()[i].canCapture)
                continue;
            if (before.combatLocations[i] != after.combatLocations[i])
                return false;
            if (pawn.grid < 0 || pawn.ignoresCollision || before.combatLocations[i] == INVALID_TILE ||
                oldTiles == newTiles)
                continue;
            origins[{pawn.grid, pawn.movement}].insert({before.combatLocations[i].x, before.combatLocations[i].y});
        }
        ActivationField beforeReach, afterReach;
        for (auto &[key, starts] : origins)
        {
            for (auto tile : targets)
                starts.insert({tile.x, tile.y});
            const auto copyWork = 2LL * terrain.width * terrain.height;
            if (budget < copyWork)
                return false;
            budget -= copyWork;
            const auto oldGrid = terrain.grids[key.first].withBlockedTiles(oldTiles);
            const auto newGrid = terrain.grids[key.first].withBlockedTiles(newTiles);
            for (auto [x, y] : starts)
            {
                const auto tariff = 2LL * terrain.width * terrain.height;
                if (budget < tariff)
                    return false;
                budget -= tariff;
                beforeReach.build(oldGrid, {x, y}, key.second, field.horizonTurns(), targets);
                afterReach.build(newGrid, {x, y}, key.second, field.horizonTurns(), targets);
                for (auto tile : targets)
                {
                    const auto old = beforeReach.activations(tile.x, tile.y);
                    const auto next = afterReach.activations(tile.x, tile.y);
                    if (next >= 0 && (old < 0 || next < old))
                        return false;
                }
            }
        }
        return true;
    }

    bool changesCompletedCaptures(const PropertyStockField &field, const ContinuationKey &a, const ContinuationKey &b)
    {
        const auto completed = [&](const ContinuationKey &key) {
            std::set<std::pair<int, int>> tiles;
            for (const auto &action : key)
                if (action.captures() && field.actionCompletesCapture({action.row(), {action.x(), action.y()}, true}))
                    tiles.insert({action.x(), action.y()});
            return tiles;
        };
        return completed(a) != completed(b);
    }

    bool breakableScreen(const MasterAssignmentProblem &problem, const ReplyPassEvaluation &trial, int slot,
                         TilePoint previous)
    {
        const auto at =
            std::find_if(trial.actors.begin(), trial.actors.end(), [&](const auto &a) { return a.actorSlot == slot; });
        if (at == trial.actors.end())
            return true;
        const auto &candidate = problem.actors()[slot].candidates[at->candidateIndex];
        const auto end = candidate.destination;
        // Protect transit squares too. An alternative may interpose remotely without ending on the old firing tile.
        if (end == previous)
            return false;
        const auto &facts = *candidate.source.replyFacts;
        double upperDamage = 0;
        for (std::size_t i = 0; i < facts.shots.size(); ++i)
        {
            // Favor the attacker: even shots with poor immediate net, full HP, ammo and free firing squares.
            const auto power = std::max(facts.shots[i].rawBase, facts.shots[i].withAmmo);
            double strongest = 0;
            for (int hp = 1; hp <= at->stepsAfterOwnActions; ++hp)
                strongest = std::max(strongest, terrainReducedDamage(facts.terrain, power, UNIT_HP_STEPS, hp));
            upperDamage += std::max(0.0, std::ceil(strongest));
        }
        return at->stepsAfterOwnActions <= 0 || (upperDamage > 0 && upperDamage >= (at->stepsAfterOwnActions - 1) * 10);
    }

    QJsonArray witnessRecord(const ReplyStockFloor &floor)
    {
        QJsonArray rows;
        for (const auto &row : floor.witness)
        {
            QJsonArray visits;
            for (const auto &step : row.steps)
                visits.append(QJsonArray{step.node, step.turn});
            rows.append(QJsonObject{{"row", row.row}, {"value", static_cast<qint64>(row.value)}, {"visits", visits}});
        }
        return rows;
    }

    QJsonArray keyRecord(const ContinuationKey &key)
    {
        QJsonArray rows;
        for (const auto &k : key)
            rows.append(QJsonArray{k.row(), k.x(), k.y(), k.captures(), k.stepsLost(), k.destroyed()});
        return rows;
    }

    bool publish(const MasterPlanHandoff &handoff, const ReplyPassEvaluation &replay, int changed,
                 const ReplyPassEvaluation &reference, AssignmentResult &assignment)
    {
        if (!masterPlanEquivalent(handoff.problem, reference.replay.plan, reference.replay.ordering.order,
                                  assignment.plan, assignment.executionOrder))
            return false;
        AssignmentResult next = assignment;
        for (const auto &actor : replay.actors)
        {
            auto it = std::find_if(next.selections.begin(), next.selections.end(),
                                   [&](const auto &s) { return s.engineUnitId == actor.engineUnitId; });
            if (it == next.selections.end() || it->candidateIndex != handoff.choices[actor.actorSlot])
                return false;
            it->candidateIndex = actor.candidateIndex;
            it->actionIndex = actor.actionIndex;
            it->seatedValue = replay.replay.seatedValues[actor.actorSlot];
            if (actor.actorSlot == changed || actor.candidateIndex != handoff.choices[actor.actorSlot])
                it->phase = AssignmentResult::SelectionPhase::Master;
            it->completeValues.clear();
        }
        next.plan = replay.replay.plan;
        next.executionOrder = replay.replay.ordering.order;
        assignment = std::move(next);
        return true;
    }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
    // All four values originate from the same live combat meter. Stock assignment
    // has its own accounting. Race work is a subset of the charged combat work.
    QJsonObject privateCombatWork(std::int64_t before, std::int64_t after,
                                  std::int64_t raceBefore, std::int64_t raceAfter)
    {
        const auto spent = before - after;
        const auto race = raceAfter - raceBefore;
        return {{"start", static_cast<qint64>(before)}, {"remaining", static_cast<qint64>(after)},
                {"spent", static_cast<qint64>(spent)}, {"race", static_cast<qint64>(race)},
                {"other", static_cast<qint64>(spent - race)}};
    }

    std::optional<QJsonObject> runArenaBranch(CombatMode mode, const DecisionTraceIdentity &identity,
                                              const MasterPlanHandoff &handoff, const ReplyBoard &board,
                                              const ReplyPassEvaluation &reference, const std::vector<Trial> &trials,
                                              AssignmentResult &assignment, QJsonObject record)
    {
        if (mode != CombatMode::Recorded || qgetenv("COW_AI_ARENA_TEST") != "1" ||
            qgetenv("COW_COMBAT_COUNTERFACTUAL") != "1")
            return {};
        QFile file(Settings::getInstance()->getUserPath() + QStringLiteral("combat-counterfactual.json"));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        const auto config = QJsonDocument::fromJson(file.readAll()).object();
        if (config.value("day").toInt(-1) != identity.day || config.value("player").toInt(-1) != identity.playerId ||
            config.value("sequence").toInt(-1) != static_cast<qint64>(identity.planSequence))
            return {};
        record["status"] = "COUNTERFACTUAL_REFUSED";
        for (const auto &trial : trials)
        {
            const auto &actor = handoff.problem.actors()[trial.slot];
            if (actor.engineUnitId != config.value("unit").toInt(-1) ||
                trial.option != config.value("candidate").toInt(-1))
                continue;
            const auto &candidate = actor.candidates[trial.option];
            const auto &incumbent = actor.candidates[handoff.choices[trial.slot]];
            if (tile(candidate.destination) != config.value("destination").toArray() ||
                tile(candidate.target) != config.value("target").toArray() ||
                tile(incumbent.destination) != config.value("referenceDestination").toArray() ||
                tile(incumbent.target) != config.value("referenceTarget").toArray())
                return record;
            auto choices = handoff.choices;
            choices[trial.slot] = trial.option;
            const auto evaluated = evaluateReplyPassPlan(handoff.problem, choices, board);
            if (evaluated.valued() && publish(handoff, evaluated, trial.slot, reference, assignment))
            {
                record["status"] = "FORCED_ARENA_BRANCH";
                record["forcedArenaBranch"] = true;
                record["changedUnit"] = actor.engineUnitId;
                record["changedCandidate"] = trial.option;
                std::set<int> watched;
                for (const auto &a : reference.actors)
                    watched.insert(a.engineUnitId);
                record["experimentalForecast"] = forecastRecord(evaluated, watched);
            }
            return record;
        }
        return record;
    }
#endif

} // namespace

namespace Coordinator
{
    QJsonObject compareCombatPlans(CombatMode mode, const DecisionTraceIdentity &identity, GameMap &map,
                                   const BattlefieldKnowledge &knowledge, MobilityFieldCache &mobility,
                                   const MasterPlanHandoff *handoff, const ReplyBoard *initialBoard,
                                   const PropertyStockField &field, AssignmentResult &assignment,
                                   std::int64_t allowance, ScriptVariables *variables, SimpleProductionSystem *actingBuyer, std::int64_t pairProofAllowance, ReplyFactsBuilder *replyFacts,
                                   const AssignmentInput *progressInput,std::span<const PropertyFacts> progressSites)
    {
        QJsonObject result{{"schema", 1},
                           {"day", identity.day},
                           {"player", identity.playerId},
                           {"planSequence", static_cast<qint64>(identity.planSequence)},
                           {"horizon", identity.horizonTurns},
                           {"acting", mode == CombatMode::Acting},
                           {"status", "NO_HANDOFF"},
                           {"installed", false}};
        if (!handoff || !initialBoard)
            return result;
        if (allowance <= 0)
        {
            result["status"] = "NO_BUDGET";
            return result;
        }
        if (map.getPlayerCount() != 2)
        {
            result["status"] = "MULTIPLAYER_SIDES";
            return result;
        }
        if (map.getGameRules()->getFogMode() != GameEnums::Fog_Off)
        {
            result["status"] = "FOG_UNRESOLVED";
            return result;
        }
        for (const auto &known : knowledge.units())
        {
            if (known.carrier != KnownUnit::NO_CARRIER)
                continue;
            auto *live = map.getTerrain(known.x, known.y)->getUnit();
            if (live && live->isStatusStealthed())
            {
                result["status"] = "STEALTH_UNRESOLVED";
                return result;
            }
        }
        // Half for geometry, a third for pricing trials and a sixth for independent verification.
        std::int64_t geometry = allowance / 2;
        std::set<std::pair<int, QString>> profiles;
        for (const auto &unit : knowledge.units())
            if (unit.canCapture || unit.canFire)
                profiles.emplace(unit.ownerId, unit.unitId);
        const auto setup = static_cast<std::int64_t>(knowledge.width()) * knowledge.height() * (profiles.size() + 1);
        if (setup > static_cast<std::uint64_t>(geometry))
        {
            result["status"] = "SETUP_BUDGET";
            return result;
        }
        geometry -= setup;
        const auto beforeRules=geometry;
        auto terrain = combatTerrain(map,knowledge,mobility,geometry);
        result["townCombatRulesWork"]=static_cast<qint64>(beforeRules-geometry);
        terrain->workLeft = &geometry;
        QJsonArray observedDefenders;
        for(int i=0;i<int(knowledge.units().size());++i){
            const auto &known=knowledge.units()[i];const auto relation=knowledge.relation(known.ownerId);
            if((relation!=Relation::Own && relation!=Relation::Allied) || (relation==Relation::Own && !known.hasMoved) ||
               known.carrier!=KnownUnit::NO_CARRIER || known.hpRounded<=0)continue;
            const auto &pawn=terrain->units[i];
            auto *live=pawn.present?map.getTerrain(known.x,known.y)->getUnit():nullptr;
            const auto open=[&](const QString &reason){
                result["status"]="OBSERVED_E0_DEFENDER_OPEN";
                result["unresolvedConsequence"]=QJsonObject{{"reason",reason},{"unit",pawn.engineUnitId},
                    {"knowledge",i},{"tile",tile(pawn.tile)},{"tick",1},{"effect","E0_TARGET_OR_COUNTER_MAY_CHANGE"}};
                return result;
            };
            if(!live || live->getUniqueID()!=pawn.engineUnitId || !replyFacts)return open("OBSERVED_LIVE_DEFENDER_FACTS_UNAVAILABLE");
            if(!terrain->pay(1+std::int64_t(knowledge.units().size())*(1+std::int64_t(terrain->width)*terrain->height)))
                return open("OBSERVED_E0_TARGET_WORK_LIMIT");
            const auto facts=replyFacts->factsFor(i,*live,pawn.tile);
            if(!facts || !facts->tile || facts->tile->shooters.size()!=facts->shots.size())return open("OBSERVED_TARGET_WEAPONS_UNAVAILABLE");
            const bool exposed=std::any_of(facts->shots.begin(),facts->shots.end(),[](const auto &v){return v.rawBase>=0 || v.withAmmo>=0;});
            if(exposed && (relation!=Relation::Own || live->getLoadedUnitCount()>0 || live->isStatusStealthed()))
                return open("ALLIED_CARGO_OR_STEALTH_TARGET_CONSEQUENCE_UNPRICED");
            if(relation==Relation::Own){
                terrain->observedDefenders.push_back({i,pawn.engineUnitId,pawn.tile,pawn.replacementCost,facts,field.ourRowOf(i)});
                observedDefenders.append(QJsonObject{{"unit",pawn.engineUnitId},{"tile",tile(pawn.tile)},{"hp",facts->actorRealHp},
                    {"fuel",known.fuel},{"ammo",QJsonArray{known.ammo1,known.ammo2}},{"possibleShooters",int(facts->tile->shooters.size())}});
            }
        }
        if(!observedDefenders.isEmpty())result["observedSpentE0Defenders"]=observedDefenders;
        const auto beforeWalkProfile=geometry;
        const auto walkProfiles=prepareRetainedWalkProfiles(map,knowledge,*terrain,field.horizonTurns(),geometry);
        terrain->ordinaryCaptureInstallments=walkProfiles.ordinaryCaptureInstallments;
        result["retainedWalkProfile"]=QJsonObject{{"ordinaryCaptureInstallments",terrain->ordinaryCaptureInstallments},{"reason",walkProfiles.reason},{"units",walkProfiles.admitted},
            {"cells",walkProfiles.checkedCells},{"work",static_cast<qint64>(beforeWalkProfile-geometry)}};
        terrain->certifyRetainedWalkProfile=TownWalkBinding{walkProfiles.through};
        result["movementBoundsSetupWork"] = static_cast<qint64>(prepareCaptureMovementBounds(*terrain,geometry));
        ReplyBoard board = *initialBoard;
        board.combat = terrain;
        result["allowance"] = static_cast<qint64>(allowance);
        const auto &problem = handoff->problem;
        const auto &sourceChoices = handoff->choices;
        if (handoff->stockProposal)
        {
            QJsonArray submitted, proposed, order;
            for (int i : sourceChoices) submitted.append(i);
            for (int i : handoff->stockProposal->choices) proposed.append(i);
            for (int i : handoff->stockProposal->executionOrder) order.append(i);
            result["stockRefinement"] = QJsonObject{{"admission","REQUIRES_FULL_FORMULA"},
                {"incomingChoices",submitted},{"proposedChoices",proposed},{"proposedExecutionOrder",order}};
        }
        const auto sourceReference = evaluateReplyPassPlan(problem,sourceChoices,board);
        auto repair = reconcileOverbookedCombatReference(problem,board,sourceReference,sourceChoices,geometry);
        const auto &effectiveChoices = repair.evaluation ? repair.choices : sourceChoices;
        const auto &reference = repair.evaluation ? *repair.evaluation : sourceReference;
        const auto changedFromSource = [&](const std::vector<int> &selected) {
            QJsonArray changes;
            for (const auto &actor : problem.actors())
            {
                const int slot = actor.canonicalSlot, previous = sourceChoices[slot], chosen = selected[slot];
                if (previous == chosen) continue;
                const auto &before = actor.candidates[previous], &after = actor.candidates[chosen];
                changes.append(QJsonObject{{"slot",slot},{"unit",actor.engineUnitId},
                    {"sourceCandidate",previous},{"sourceKind",static_cast<int>(before.actionKind)},
                    {"sourceDestination",tile(before.destination)},{"sourceTarget",tile(before.target)},
                    {"sourceTargetUnit",before.targetUnitId},{"candidate",chosen},{"kind",static_cast<int>(after.actionKind)},
                    {"destination",tile(after.destination)},{"target",tile(after.target)}});
            }
            return changes;
        };
        if (sourceReference.reason == ReplyPassNoValue::TargetAlreadyGone)
        {
            QJsonArray source, effective;
            for (int i : sourceChoices) source.append(i);
            for (int i : effectiveChoices) effective.append(i);
            result["sourceChoices"] = source;
            result["referenceChoices"] = effective;
            result["referenceRepair"] = QJsonObject{{"source",forecastRecord(sourceReference,{})},
                {"status",repair.evaluation ? "RECOVERED" : repair.limited ? "WORK_LIMIT" : "NO_EXECUTABLE_REPAIR"},
                {"attempts",repair.attempts},{"changes",changedFromSource(effectiveChoices)}};
        }
        auto retainEffectiveReference = [&] {
            if (!repair.evaluation) return;
            QJsonArray choices, order;
            for (int i : effectiveChoices) choices.append(i);
            for (int i : reference.replay.ordering.order) order.append(i);
            result["executionOrder"] = order;
            result["choices"] = choices;
            result["selectedChanges"] = changedFromSource(effectiveChoices);
            result["selectedTrial"] = -1;
            result["status"] = "REPAIRED_REFERENCE_PREFERRED";
            if (mode == CombatMode::Acting)
            {
                const bool accepted = publish(*handoff,reference,NO_REPLY_PASS_ACTOR,sourceReference,assignment);
                result["installed"] = accepted;
                result["status"] = accepted ? "INSTALLED" : "PUBLICATION_REFUSED";
            }
        };
        std::set<int> watched;
        auto serviceBindings=std::make_shared<OpeningLaneRulesMemo>();
        const bool priceOpponent = qgetenv("COW_COORDINATED_OPPONENT_CAPTURE") != "0";
        QJsonArray maturedReleases, dueStarts, abandonedStarts;
        if (priceOpponent)
            dueStarts=matureCaptureStarts(readDatedOrders(variables,NEXT_CAPTURE_STARTS,identity.day,identity.playerId),
                                          problem,effectiveChoices,abandonedStarts,[&](TilePoint from, TilePoint to) {
                auto *unit=map.getTerrain(from.x,from.y)->getUnit();
                return unit ? unit->getCaptureRate(QPoint(to.x,to.y)) : 0;
            });
        result["dueCaptureStarts"]=dueStarts;
        result["abandonedCaptureStarts"]=abandonedStarts;
        if (priceOpponent)
        {
            bool productionSetupLimited=false;
            auto production = buildProductionForecastBoard(map, knowledge, mobility, *terrain, field.horizonTurns(),&productionSetupLimited);
            if (productionSetupLimited)
            {
                result["status"]="PRODUCTION_SETUP_BUDGET";
                return result;
            }
            if (production)
            {
                maturedReleases = matureFactoryReleases(readDatedOrders(variables, FUNDED_FACTORY_RELEASES, identity.day, identity.playerId),
                    *production, problem, knowledge);
                bindFriendlyDayStart(*production,map,knowledge,serviceBindings);
                terrain->production = std::move(production);
                if(walkProfiles.admitted>0)terrain->screenDemand=std::make_shared<ScreenRuleDemand>();
                result["retainedScreenFireProfile"]=QJsonObject{{"reason","NO_ELIGIBLE_DATED_CUT"},{"work",0}};
            }
        }
        result["maturedFactoryReleases"] = maturedReleases;
        PurchaseWindow purchaseWindow;
        std::set<Cell> affordableFactories;
        if (terrain->production)
        {
            std::set<Cell> owned;
            for (const auto &b:knowledge.buildings())
                if (b.ownerId==knowledge.observerId()) owned.emplace(b.x,b.y);
            purchaseWindow.treasury=terrain->production->funds[0];
            purchaseWindow.unitSlots=terrain->production->availableUnits[0];
            for (const auto &site:terrain->production->sites)
            {
                const Cell at{site.tile.x,site.tile.y};
                const auto cost=site.minimumBuildCost[0];
                if (!owned.contains(at) || cost<=0 || cost>purchaseWindow.treasury) continue;
                purchaseWindow.sitesByCost.emplace_back(cost,at);
                if (purchaseWindow.unitSlots>0) affordableFactories.insert(at);
            }
            std::sort(purchaseWindow.sitesByCost.begin(),purchaseWindow.sitesByCost.end());
        }
        std::map<Cell,TilePoint> dueFactoryExits;
        for (const auto &v:maturedReleases)
        {
            const auto obj=v.toObject();
            const auto at=factoryPoint(obj["factory"]);
            dueFactoryExits[{at.x,at.y}]=factoryPoint(obj["vacaterEnd"]);
        }
        auto storeEconomicOrders=[&](const QJsonObject &policy,const ReplyPassEvaluation &plan) {
            storeFactoryReleases(variables,identity.day,identity.playerId,maturedReleases,policy,plan,result);
            storeCaptureStarts(variables,identity.day,identity.playerId,dueStarts,policy,plan,result);
        };
        const auto hq = priceOpponent ? headquartersWatch(map, knowledge, *terrain) : HeadquartersWatch{};
        terrain->guardedHQ = hq.home;
        std::set<int> potentialCappers;
        for (const auto &b : knowledge.buildings())
        {
            const int slot = field.columnSlotAt({b.x, b.y});
            if (slot == NO_STOCK_COLUMN)
                continue;
            const auto owner = mirroredColumn(field.enemyColumn(slot)).ownerBefore;
            if (owner != OwnerSign::Ours)
                terrain->contestableSites.emplace(b.x, b.y);
            const int i = knowledge.unitIndexAt(b.x,b.y);
            if (owner != OwnerSign::Enemy && i != NO_UNIT && terrain->units[i].enemy && terrain->units[i].capper)
                potentialCappers.insert(i);
        }
        const auto candidates =
            combatTrials(problem, effectiveChoices, reference, knowledge, priceOpponent, hq, potentialCappers, watched,{},terrain.get());
        result["reference"] = forecastRecord(reference, watched);
        if (!reference.valued())
        {
            result["status"] = "REFERENCE_UNAVAILABLE";
            return result;
        }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        const auto forcedStart = allowance - allowance / 2 + geometry;
        if (auto branch = runArenaBranch(mode, identity, *handoff, board, sourceReference, candidates, assignment, result))
        {
            // Only the geometry tranche is active in this branch. Preserve the
            // other tranche in both readings to report remaining combat credits.
            const auto remaining = allowance - allowance / 2 + geometry;
            (*branch)["remainingCombatWork"] = static_cast<qint64>(remaining);
            (*branch)["raceWork"] = 0;
            (*branch)["geometryWork"] = static_cast<qint64>(allowance - remaining);
            (*branch)["forcedBranchWork"] = privateCombatWork(forcedStart, remaining, 0, 0);
            return *branch;
        }
#endif
        std::set<int> attackedTargets;
        const bool sharedTarget = priceOpponent && std::any_of(reference.actors.begin(), reference.actors.end(),
            [&](const auto &actor) { return actor.ownShot.target != NO_UNIT && !attackedTargets.insert(actor.ownShot.target).second; });
        const int closingActors=priceOpponent ? std::count_if(problem.actors().begin(),problem.actors().end(),[&](const auto &a) {
            const int selected=effectiveChoices[a.canonicalSlot];
            if (selected<0 || (!problem.canChangeActor(a.canonicalSlot) && !problem.canChangeCombatHelper(a))) return false;
            const auto &c=a.candidates[selected];
            const bool factory=affordableFactories.contains({c.destination.x,c.destination.y}) ||
                               affordableFactories.contains({c.origin.x,c.origin.y});
            return std::any_of(a.candidates.begin(),a.candidates.end(),[&](const auto &other) {
                return other.plannable && ((factory && other.destination!=c.destination) ||
                    (c.actionKind==PlanBundleKind::Wait && (concreteIdleAction(other) ||
                         (knowledge.units()[a.knowledgeUnitIndex].canCapture && other.actionKind==PlanBundleKind::Move))));
            });
        }) : 0;
        const auto factorySurfaceOffer=std::max(allowance,pairProofAllowance);
        auto factorySurfaceLeft=factorySurfaceOffer;
        const bool factoryProgrammeSurface=mode==CombatMode::Acting && actingBuyer && progressInput && priceOpponent &&
            FactoryPrograms::hasFactoryProgrammeProposal(problem,*terrain,knowledge,progressSites,factorySurfaceLeft);
        result["factoryProgramSurfaceWork"]=qint64(factorySurfaceOffer-factorySurfaceLeft);
        if (!factoryProgrammeSurface && !handoff->stockProposal && !repair.evaluation && candidates.empty() && !sharedTarget && !closingActors && (!priceOpponent || hq.standing == NO_UNIT))
        {
            result["status"] = "NO_CONTACT_TRIAL";
            return result;
        }
        if (priceOpponent)
        {
            result["schema"] = 4;
            result["valuation"] = "two-sided first-entry capture forecast";
            // Geometry and economic projections draw on the same turn allowance. Previously
            // an exhausted race budget stranded unused movement work on the very same turn.
            std::int64_t budget = allowance - allowance / 2 + geometry;
            terrain->workLeft = &budget;
            std::int64_t raceWork = 0;
            if(mode==CombatMode::Acting)
            {
                // Acting turns optimize directly in UnifiedTeamSearch; skip legacy 120M pair-proof spikes.
                pairProofAllowance=0;
            }
            else if(qEnvironmentVariableIsSet("COW_COORDINATED_PAIR_PROOF_WORK"))
            {
                bool valid=false;
                const auto configured=qgetenv("COW_COORDINATED_PAIR_PROOF_WORK").toLongLong(&valid);
                pairProofAllowance=valid?configured:0;
            }
            // Reserve headroom for all grants and integer ledger arithmetic.
            pairProofAllowance=std::clamp<std::int64_t>(pairProofAllowance,0,std::numeric_limits<std::int64_t>::max()/1024);
            std::int64_t pairProofTurnCap=2*pairProofAllowance;
            if(qEnvironmentVariableIsSet("COW_COORDINATED_PAIR_PROOF_TURN_CAP"))
            {
                bool valid=false;
                const auto configured=qgetenv("COW_COORDINATED_PAIR_PROOF_TURN_CAP").toLongLong(&valid);
                if(valid && configured>=0)pairProofTurnCap=configured;
            }
            int pairProofMaxPasses=PairProofMeter::DEFAULT_MAX_PASSES;
            if(qEnvironmentVariableIsSet("COW_COORDINATED_PAIR_PROOF_MAX_PASSES"))
            {
                bool valid=false;
                const auto configured=qgetenv("COW_COORDINATED_PAIR_PROOF_MAX_PASSES").toInt(&valid);
                if(valid)pairProofMaxPasses=std::clamp(configured,1,3);
            }
            result["dedicatedPairProofAllowance"]=static_cast<qint64>(pairProofAllowance);
            result["dedicatedPairProofWork"]=0;
            CaptureRaceRouteCache routeCache;
            routeCache.townMarkets.enabled=true;
            TownMarketPrefetch prefetch;
            if(prefetch.enabled())routeCache.townMarkets.ownerHooks=&prefetch;
            routeCache.f1OpportunityShelf=std::make_shared<F1OpportunityShelf>();
            terrain->comparisonWork=allowance;
            std::vector<TilePoint> propertyTiles;
            for (const auto &b:knowledge.buildings())
                if (field.columnSlotAt({b.x,b.y})!=NO_STOCK_COLUMN) propertyTiles.push_back({b.x,b.y});
            result["propertyBoundsSetupWork"]=static_cast<qint64>(
                prepareStockPropertyBounds(*terrain,propertyTiles,routeCache.preparedPropertyBounds,budget));
            using DeploymentPrefixes=std::map<std::vector<int>,std::map<int,QJsonObject>>;
            auto scoreWithCache = [&](const ReplyPassEvaluation &plan,QJsonObject &record,CaptureRaceRouteCache &routeCache,
                                      DeploymentPrefixes &prefixes) -> std::optional<CombatRank> {
                bool observedCommittedPost=false,missingCommittedPost=false,verifiedStockPrefix=false;
                QJsonObject prefixGap;
                TownComparisonEvidence evidence;
                int incompleteStockLeaves=0;bool recordedPostGap=false;
                QJsonObject financialGaps;
                record.remove("optionalAttackDecisionRange");
                record.remove("continuationRangeInputsComplete");
                const auto rankEvidence=[](const CombatRank &r) {
                    return QJsonArray{int(std::get<0>(r)),std::get<1>(r),std::get<2>(r),static_cast<qint64>(std::get<3>(r))};
                };
                std::set<TownIntention> required;
                for (const auto &intent:routeCache.requiredTownChallenges)
                    required.insert({intent.tile.x,intent.tile.y,intent.attackingSide,intent.capperUnit,intent.entryTick});
                if(const auto q=routeCache.priorityFor(routeCache.stockComparisonPolicy);q && (*q)[3]>=0)required.insert(*q);
                const auto finishEvidence=qScopeGuard([&] {
                    if(routeCache.fundedPostCommitment)
                    {
                        record["allScoredLeavesAtCommittedPost"]=observedCommittedPost && !missingCommittedPost;
                        record["sameStockPrefixCertified"]=verifiedStockPrefix && prefixGap.isEmpty();
                        if(!prefixGap.isEmpty())record["deploymentPrefixGap"]=prefixGap;
                        if(!observedCommittedPost || missingCommittedPost || !verifiedStockPrefix || !prefixGap.isEmpty())evidence.noteUnknown();
                    }
                    record["townComparisonEvidence"]=evidence.record();
                    if(!financialGaps.isEmpty())record["unpricedFinancialInputs"]=financialGaps;
                    if (!evidence.complete) recordEconomicEvidence(record,false);
                });
                // Every reply carries its own optional continuation fields. A lethal reply
                // must not inherit the hold forecast from an earlier, milder response.
                record["nextCapturePolicy"] = "NOT_APPLICABLE";
                record["withdrawTotal"] = QJsonValue::Null;
                record["holdTotal"] = QJsonValue::Null;
                record["hqGuard"] = NO_UNIT;
                record["hqGuardAlternatives"] = QJsonArray{};
                std::vector<int> guardChoices;
                bool needsCover = false;
                const auto rank = headquartersRank(hq, *terrain, knowledge, problem, field, plan, 0, record,
                                                   guardChoices, needsCover);
                if (!rank)
                    return {};
                if (std::get<0>(*rank) != TerminalClass::Unresolved)
                {
                    record["policyTotal"] = 0;
                    return rank;
                }
                // Admission of a support post does not price the unit's future
                // orders or the opponent's responses to them.
                QJsonArray unpricedSupport;
                for (const auto &actor : plan.actors)
                {
                    const auto &entry = problem.actors()[actor.actorSlot];
                    const auto &unit = knowledge.units()[entry.knowledgeUnitIndex];
                    if (unit.canFire || unit.canCapture) continue;
                    const auto post = plan.replay.plan.action(actor.actionIndex).destination;
                    auto *live=map.getTerrain(unit.x,unit.y)->getUnit();
                    unpricedSupport.append(QJsonObject{
                        {"unit",actor.engineUnitId},{"tile",tile(post)},{"firstEnemyAction",1},{"firstFriendlyAction",2},
                        {"nativeActionIds",live?QJsonArray::fromStringList(live->getActionList()):QJsonArray{}},
                        {"reason","NATIVE_SUPPORT_ORDERS_AND_OPPOSING_REPLIES_NOT_FULLY_PRICED"}});
                }
                if(!unpricedSupport.isEmpty()){
                    record["unpricedHeadquartersSupport"]=unpricedSupport;
                    evidence.deferChoice("HEADQUARTERS_TRANSIT_SUPPORT","NEXT_ACTIVATION_AND_OTHER_DUTIES_UNRESOLVED");
                }
                const RequestedFollowup *selectedFollowup=nullptr;
                bool conditionalFollowupLeaf=false;
                QJsonArray scheduledFollowups;
                const auto followupEnvironmentKnown=[&] {
                    return unpricedSupport.isEmpty() && required.empty() && !missingCommittedPost &&
                        prefixGap.isEmpty() && !recordedPostGap && !routeCache.fundedPostCommitment &&
                        routeCache.fundedPostQuestions.isEmpty();
                };
                const auto proofHistoryKnown=[&] {
                    return followupHistoryClosed(record,evidence.complete && !evidence.unresolvedEvents,
                        incompleteStockLeaves,followupEnvironmentKnown(),financialGaps.isEmpty());
                };
                const auto debtStamp=[&]{return FollowupDebtStamp{evidence.unresolvedEvents,incompleteStockLeaves,financialGaps};};
                bool followupParentKnown=false;
                FollowupDebtStamp followupStart;
                DecisionRange scopedFollowupRange;
                FollowupPolarityWindow scopedWindow;
                const auto publishFollowups=qScopeGuard([&] {
                    if(!scheduledFollowups.isEmpty())record["rankedFollowupSchedules"]=scheduledFollowups;
                });
                auto valueRace = [&](const CaptureRaceResult *race, QJsonObject &record, int guard) -> std::optional<CombatRank> {
                    // A hypothetical release which was declined by the retained
                    // schedule cannot earn a payday. Likewise a leg after the
                    // cutoff or a repeat title is settled on this calendar.
                    // An omitted priced alternative can still change the rank.
                    if(const auto &commit=routeCache.fundedPostCommitment;commit)
                    {
                        const FundedDeploymentId id{commit->side,commit->factory.x,commit->factory.y,commit->buildTick,commit->type};
                        const QJsonArray post{commit->post.x,commit->post.y};
                        const bool bound=std::any_of(race->productionOrders.begin(),race->productionOrders.end(),
                            [&](const auto &o){return paidAtPost(o.toObject(),id,post);});
                        observedCommittedPost|=bound;
                        missingCommittedPost|=!bound;
                    }
                    const auto registeredPosts=routeCache.fundedPostQuestions.value(routeCache.stockComparisonPolicy).toArray();
                    if(!registeredPosts.isEmpty())
                    {
                        // A conditional firing line cannot price every consequence of
                        // the already-discovered deployment. Keep even missing hearings open.
                        if(!recordedPostGap)
                        {
                            evidence.deferChoice("FUNDED_POST_BEFORE_RAID","POST_CONTINUATION_AND_OTHER_JOBS_UNRESOLVED");
                            recordedPostGap=true;
                        }
                        else evidence.noteUnknown();
                        ++incompleteStockLeaves;
                        record["unpricedFundedPostQuestions"]=registeredPosts;
                    }
                    else record.remove("unpricedFundedPostQuestions");
                    bool financialGap=race->productionLimited;
                    if(financialGap)financialGaps["productionWorkLimited"]=true;
                    if(!race->followupDecision.isEmpty()) {
                        record["followupDecision"]=race->followupDecision;
                        if(!conditionalFollowupLeaf && (race->followupDecision["workExhausted"].toBool() ||
                           !race->followupDecision["shots"].toArray().isEmpty())) {
                            financialGap=true;
                            financialGaps["followupPolicyTail"]="F1_SHOT_ROLE_AND_E1_CONTINUATION_UNBOUND";
                        }
                    } else record.remove("followupDecision");
                    if(race->deferredRouteAssignments>0)
                    {
                        int settled=0;
                        for(const auto &entry:race->routeProofs)
                        {
                            const auto row=entry.toObject();
                            if(row["entryTick"].toInt()!=-1)continue;
                            const auto reason=row["status"].toString();
                            if(reason=="DEFERRED_WORK_LIMIT")financialGaps["stockWorkLimited"]=true;
                            if(reason=="PREVIOUSLY_CAPTURED" || reason=="OUTSIDE_HORIZON")++settled;
                            else if(!financialGaps.contains("deferredStockLeg"))financialGaps["deferredStockLeg"]=row;
                        }
                        if(settled<race->deferredRouteAssignments)
                        {financialGap=true;financialGaps["deferredRouteAssignments"]=race->deferredRouteAssignments;}
                    }
                    if(!race->paidCaptureWorkGap.isEmpty()){financialGap=true;financialGaps["paidCaptureWorkGap"]=race->paidCaptureWorkGap;}
                    if(!race->paidCaptureUnsupported.isEmpty()){financialGap=true;financialGaps["paidCaptureUnsupported"]=race->paidCaptureUnsupported;}
                    record.remove("ownershipComponentBounds");
                    if(race->openingIncome)
                    {
                        const auto &b=*race->openingIncome;
                        MilliFunds actual=race->ownedIncome;
                        const bool consistent=RelativeStockDetail::checkedAdd(actual,race->stock) && actual>=b.nominalLow && actual<=b.nominalHigh;
                        record["ownershipComponentBounds"]=QJsonObject{{"scope","FIXED_STOCK_COLUMN_INCOME_AFTER_F0_AND_MODELED_E0"},
                            {"nominalLower",qint64(b.nominalLow)},{"nominalUpper",qint64(b.nominalHigh)},
                            {"weightedLowerNumerator",qint64(b.weightedLow)},{"weightedUpperNumerator",qint64(b.weightedHigh)},
                            {"weightDenominator",b.horizon},{"E0EnemyIncome",qint64(b.firstEnemy)},{"F1FriendlyIncome",qint64(b.firstFriendly)},
                            {"nominalOnQuotedBoard",qint64(actual)},{"consistent",consistent}};
                        if(!consistent){financialGap=true;financialGaps["ownershipOutsidePhysicalInterval"]=true;}
                    }
                    if(!race->alliedCaptureVacancy.isEmpty()){
                        financialGap=true;financialGaps["alliedCaptureVacancy"]=race->alliedCaptureVacancy;
                    }
                    if(!race->observedF1Stock.isEmpty())record["observedF1Stock"]=race->observedF1Stock;
                    if(!race->friendlyService.isEmpty())record["friendlyF1Service"]=race->friendlyService;
                    if(!race->friendlyServiceOpen.isEmpty()){
                        financialGap=true;financialGaps["friendlyF1Service"]=race->friendlyServiceOpen;
                    }
                    if(!race->unconfirmedFactoryReleases.isEmpty()){
                        financialGap=true;
                        financialGaps["unconfirmedFactoryReleases"]=race->unconfirmedFactoryReleases;
                    }
                    if(financialGap)evidence.noteUnknown();
                    if(financialGap || !race->observedStockComplete)++incompleteStockLeaves;
                    const auto nominal = nominalContinuationValue(plan.exactNonStock, *race);
                    if (!nominal)
                        return {};
                    auto sum = *nominal;
                    record["replacedCapperNextShot"] = static_cast<qint64>(race->replacedCapperNextShot);
                    record["combinedFollowup"] = race->combinedFollowup;
                    record["homeCaptureResponse"] = race->homeCaptureResponse;
                    record["replacedHomeCapperNextShot"] = static_cast<qint64>(race->replacedHomeCapperNextShot);
                    record["productionValue"] = static_cast<qint64>(race->productionValue);
                    record["productionOrders"] = race->productionOrders;
                    record["productionAvailableSites"] = race->availableProductionSites;
                    record["productionPositiveOffers"] = race->affordableProductionOrders;
                    record["productionWorkLimited"] = race->productionLimited;
                    record["unconfirmedFactoryReleases"] = race->unconfirmedFactoryReleases;
                    if (!race->approachRecord.isEmpty()) record["captureApproach"]=race->approachRecord;
                    else record.remove("captureApproach");
                    record["replacedVehicleNextShot"] = static_cast<qint64>(race->replacedNextShot);
                    record["survivorNextShotAdjustment"] = static_cast<qint64>(race->survivorNextShotAdjustment);
                    record["survivorNextShots"] = race->survivorNextShots;
                    record["returnFireValue"] = static_cast<qint64>(race->returnValue);
                    record["returnFire"] = race->returnFire;
                    record["boxContinuation"] = race->boxContinuation;
                    record["guardAction"] = race->guardAction;
                    record["enemyCaptureActions"] = race->enemyCaptureActions;
                    record["ownedIncome"] = static_cast<qint64>(race->ownedIncome);
                    record["holdCapitalLoss"] = static_cast<qint64>(race->holdingLoss);
                    record["holdForecast"] = race->holds;
                    record["withdrawals"] = race->withdrawals;
                    record["projectedWinner"] = race->terminalSide;
                    record["projectedTerminalHalfTurn"] = race->terminalTick;
                    record["foregoneNextShot"] = static_cast<qint64>(race->foregoneShot);
                    record["captureClaims"] = race->claims;
                    record["townEncounters"]=race->townEncounters;
                    record["townDecisionFacts"]=townDecisionFacts(race->townEncounters,race->townCoverage);
                    record["townEncounterBids"]=race->townEncounterBids;
                    record["townSearch"]=race->townSearch;
                    auto requiredOnBoard=required;
                    for(const auto &v:race->stockDutyReview)
                    {
                        const auto d=v.toObject();
                        if(d[d["matchedStockIntention"].toBool()?"exactRetainedStockTransfer":"presentInStockItinerary"].toBool())continue;
                        const auto loc=d["tile"].toArray();const int entry=d["entryTick"].toInt();
                        requiredOnBoard.insert({loc[0].toInt(),loc[1].toInt(),entry%2,d["unit"].toInt(),entry});
                    }
                    record["settledStockIntentions"]=race->settledStockIntentions;
                    const auto stockAnswers=matchStockAnswers(race->stockDutyReview,race->townCoverage);
                    if(!stockAnswers.isEmpty())record["matchedStockChecks"]=stockAnswers;
                    else record.remove("matchedStockChecks");
                    evidence.observe(race->townCoverage,race->townConflicts,requiredOnBoard,race->terminalTick);
                    for(const auto &a:stockAnswers)if(!a.toObject()["complete"].toBool())evidence.noteUnknown();
                    if (!race->observedStockComplete) evidence.noteUnknown();
                    if(!race->observedStockResponses.isEmpty())record["observedStockResponses"]=race->observedStockResponses;
                    record["observedStockComplete"]=race->observedStockComplete;
                    record["townCoverage"]=race->townCoverage;
                    record["townConflicts"]=race->townConflicts;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
                    if (!race->townFocusAudit.isEmpty()) record["townFocusAudit"]=race->townFocusAudit;
#endif
                    record["townEncounterWork"]=static_cast<qint64>(race->townEncounterWork);
                    record["paidCaptureChecks"] = race->paidCaptureChecks;
                    record["routePricingPasses"] = race->routePricingPasses;
                    record["routeTraversalWork"] = static_cast<qint64>(race->routeTraversalWork);
                    record["fundedPostShots"]=fundedPostShots(race->routeProofs);
                    if(!race->fundedPostResponses.isEmpty())record["fundedPostResponses"]=race->fundedPostResponses;
                    else record.remove("fundedPostResponses");
                    if(!race->stockDutyReview.isEmpty())record["stockDutyReview"]=race->stockDutyReview;
                    record["routeDefenseWork"] = static_cast<qint64>(race->routeDefenseWork);
                    record["fundedProjectionWork"] = static_cast<qint64>(race->fundedProjectionWork);
                    record["localRouteBids"] = race->localRouteBids;
                    record["deferredRouteAssignments"] = race->deferredRouteAssignments;
                    if (!race->routeProofs.isEmpty()) record["captureRouteProofs"] = race->routeProofs;
                    else record.remove("captureRouteProofs");
                    record["contestedHeadquarters"] = race->contestedHeadquarters;
                    record["contestedPropertyCaptures"] = race->contestedPropertyCaptures;
                    record["captureIncomeChange"] = static_cast<qint64>(race->stock);
                    record["nominalTotal"] = static_cast<qint64>(sum);
                    if (!RelativeStockDetail::checkedAdd(sum, race->timingAdjustment))
                        return {};
                    record["roundCashFlow"] = race->roundCashFlow;
                    record["timingAdjustment"] = static_cast<qint64>(race->timingAdjustment);
                    record["policyTotal"] = static_cast<qint64>(sum);
                    // A near projected terminal is conditional; immediate terminal and actual HQ coverage rank first.
                    const int urgency = projectedHqThreatRank(race->terminalSide, race->terminalTick);
                    record["conditionalThreatRank"] = urgency;
                    const bool covered = needsCover && guard != NO_UNIT && race->guardCovered;
                    record["hqGuard"] = covered ? terrain->units[guard].engineUnitId : NO_UNIT;
                    const bool heldThroughEnemyAction = guard != NO_UNIT && plan.combatLocations[guard] == hq.home;
                    const bool interrupted = race->homeCaptureResponse.value("urgent").toBool() &&
                        race->homeCaptureResponse.value("secured").toBool() && (race->terminalSide >= 0 || race->terminalTick > 3);
                    const int security = needsCover && !covered && !interrupted ? std::min(-1, std::get<1>(*rank)) : 0;
                    record["hqSecurity"] = security;
                    record["hqStatus"] = interrupted ? "F1_INTERRUPTS_HQ_CAPTURE" : !needsCover ? "NO_NEAR_APPROACH" : security == -2 ? "IMMEDIATE_ENTRY_AVAILABLE"
                                         : !covered && heldThroughEnemyAction ? "GUARD_DENIES_IMMEDIATE_ENTRY"
                                         : !covered ? "OPEN_APPROACH"
                                         : plan.combatLocations[guard] == hq.home ? "GUARDED_APPROACH"
                                                                                  : "F1_RETURN_COVER";
                    return CombatRank{std::get<0>(*rank), security, urgency, sum};
                };
                auto project = [&](bool hold, int guard, bool followup, bool punish, bool defendHome,
                                   ApproachBranch branch=ApproachBranch::Observe,const CaptureApproach *approach=nullptr,
                                   const CaptureApproach *priorDecline=nullptr) {
                    const auto before = budget;
                    auto r =
                        projectCaptureRace(*terrain, knowledge, problem, field, plan, budget, hq.home, hq.away, hold, guard, followup, punish,
                                           &routeCache, defendHome,branch,approach,priorDecline,selectedFollowup);
                    raceWork += before - budget;
                    if(r && (routeCache.fundedPostCommitment || !routeCache.fundedPostQuestions.isEmpty()))
                    {
                        std::vector<int> branchKey{hold,guard,followup,punish,defendHome,int(branch)};
                        branchKey.push_back(selectedFollowup!=nullptr);
                        if(selectedFollowup){
                            const auto &q=*selectedFollowup;
                            branchKey.insert(branchKey.end(),{q.actor,q.vehicle,q.target,q.end.x,q.end.y,q.weapon,int(q.answer.has_value())});
                            if(q.answer)branchKey.insert(branchKey.end(),{q.answer->shooter,q.answer->end.x,q.answer->end.y,q.answer->weapon});
                        }
                        for(const auto *a:{approach,priorDecline})
                        {
                            branchKey.push_back(a!=nullptr);
                            if(a)branchKey.insert(branchKey.end(),{a->actor,a->shooter,a->origin.x,a->origin.y,
                                a->stage.x,a->stage.y,a->target.x,a->target.y,a->gun.x,a->gun.y,a->firstCost,a->rate,a->finish});
                        }
                        if(const auto &commit=routeCache.fundedPostCommitment;commit)
                        {
                            const int at=commit->buildTick+2;
                            const auto branch=prefixes.find(branchKey);
                            const auto actual=r->fundedPostPrefixes.find(at);
                            QJsonObject old,current;
                            if(branch!=prefixes.end()){
                                const auto prefix=branch->second.find(at);
                                if(prefix!=branch->second.end())old=prefix->second;
                            }
                            if(actual!=r->fundedPostPrefixes.end())current=actual->second.record();
                            if(old["complete"].toBool() && current["complete"].toBool() && !old["digest"].toString().isEmpty() &&
                               old["digest"]==current["digest"])verifiedStockPrefix=true;
                            else if(prefixGap.isEmpty())prefixGap={{"firstPostTick",at},{"original",old},{"conditioned",current}};
                        }
                        else
                        {
                            auto &calendar=prefixes[branchKey];
                            for(const auto &[at,prefix]:r->fundedPostPrefixes)
                            {
                                const auto stamp=prefix.record();
                                auto [entry,added]=calendar.try_emplace(at,stamp);
                                if(!added && entry->second!=stamp)entry->second={{"complete",false},{"open","AMBIGUOUS_PRIOR_CALENDAR"}};
                            }
                        }
                    }
                    return r;
                };
                auto priceFollowupLeaf = [&](const CaptureRaceResult &now,QJsonObject &out,int guard,bool hold,
                    bool followup,bool punish,bool defendHome,ApproachBranch branch=ApproachBranch::Observe,
                    const CaptureApproach *approach=nullptr,const CaptureApproach *prior=nullptr)->std::optional<CombatRank> {
                    out.remove("fixedE1Upper");out.remove("fixedE1Lower");out.remove("orderedReplies");out.remove("responseQuantifier");
                    if(!selectedFollowup || !followup || punish) return valueRace(&now,out,guard);
                    // Fix the entire friendly commitment BEFORE allowing E1
                    // to choose its targeted answer. A prior sibling's debt is
                    // distinct from debt newly incurred by this commitment.
                    const bool e1Parent=followupWitnessClean(followupParentKnown,followupEnvironmentKnown(),
                        followupStart,debtStamp());
                    const auto evaluate=[&](const CaptureRaceResult &r,QJsonObject base){
                        FollowupVisit v;
                        const bool history=e1Parent && followupEnvironmentKnown();
                        const auto before=debtStamp();
                        v.nominal=valueRace(&r,base,guard);
                        if(v.nominal && followupWitnessClean(history,followupEnvironmentKnown(),before,debtStamp()))
                            v.range.lower=v.range.upper=*v.nominal;
                        else {
                            base["open"]=history?"UNPRICED_FIXED_CAPTURE_GUARD_OR_MARKET_TAIL":"INHERITED_HQ_STOCK_OR_FUNDING_EVIDENCE_OPEN";
                            base["tail"]=base.value("unpricedFinancialInputs");
                        }
                        v.record=std::move(base);return v;
                    };
                    auto pass=evaluate(now,out);
                    auto answers=now.followupDecision.value("responseMenu").toObject();
                    if(answers.isEmpty())answers={{"physicalE1MenuComplete",false},{"enumerationComplete",false},
                        {"unpriced","E1_PREFIX_NOT_BOUND"}};
                    QJsonArray visits;
                    const auto selected=*selectedFollowup;
                    auto replayAnswer=[&](const RequestedFollowup::Adverse &answer) {
                        FollowupVisit v;
                        if(budget<=0){v.record["open"]="NO_REMAINING_FOLLOWUP_WORK";return v;}
                        const auto start=budget;
                        auto q=selected;q.answer=answer;
                        const auto previous=selectedFollowup;selectedFollowup=&q;
                        const auto reset=qScopeGuard([&]{selectedFollowup=previous;});
                        const auto rebuilt=project(hold,guard,true,true,defendHome,branch,approach,prior);
                        if(rebuilt && rebuilt->followupDecision.value("requestedSourceReplyRevalidated").toBool()){
                            v=evaluate(*rebuilt,record);
                            if(!rebuilt->followupDecision.value("physicalE1FundingAndServiceComplete").toBool()){
                                v.range={};
                                v.record["open"]="MODELED_E1_SHOT_WITH_UNVERIFIED_POST_PURCHASE_RESOURCES";
                            }
                        }
                        else {
                            v.record["open"]="REQUESTED_E1_REPLY_OR_ITS_CAPTURE_CALENDAR_UNPRICED";
                            v.record["tail"]=routeCache.lastFailure;
                        }
                        v.record["replayWork"]=qint64(start-budget);
                        return v;
                    };
                    auto result=runAdverseFollowups(std::move(pass),answers,replayAnswer,visits);
                    out=std::move(result.record);
                    out.remove("fixedE1Upper");out.remove("fixedE1Lower");
                    out["orderedReplies"]=visits;
                    out["responseQuantifier"]=answers.value("physicalE1MenuComplete").toBool()?"COMPLETE":"OPEN";
                    // Only a verified adversarial ceiling survives an omitted
                    // hostile reply. No friendly lower may be inferred from it.
                    if(!result.range.exact()){
                        evidence.deferChoice("FOLLOWUP_E1","ADVERSE_REPLY_OR_FULL_CAPTURE_CALENDAR_UNBOUND",
                            result.range.lower?rankEvidence(*result.range.lower):QJsonArray{},
                            result.range.upper?rankEvidence(*result.range.upper):QJsonArray{});
                    }
                    if(result.range.upper)out["fixedE1Upper"]=rankEvidence(*result.range.upper);
                    if(result.range.lower)out["fixedE1Lower"]=rankEvidence(*result.range.lower);
                    return result.nominal;
                };
                auto priceApproach=[&](const CaptureRaceResult &initial,QJsonObject &out,int guard,bool hold,
                    bool followup,bool punish,bool defendHome) -> std::optional<CombatRank> {
                    if (!initial.approach) return priceFollowupLeaf(initial,out,guard,hold,followup,punish,defendHome);
                    // Opponent owns the decision to spend E1 on this gun. Friendly owns
                    // the decision to make the F1 approach or wait safely and replan.
                    // Compare the same physical F1 move with and without the E1 shot.
                    // Observe only found the witness; its abstract route is not the bypass.
                    const auto staged=project(hold,guard,followup,punish,defendHome,ApproachBranch::Bypass,&*initial.approach);
                    if (!staged || staged->approachRecord.value("status").toString()!="BYPASS")
                    {
                        out["unpricedCaptureApproach"]=initial.approachRecord;
                        return {};
                    }
                    const auto hit=project(hold,guard,followup,punish,defendHome,ApproachBranch::Shoot,&*initial.approach);
                    const auto declined=project(hold,guard,followup,punish,defendHome,ApproachBranch::Decline,&*initial.approach);
                    if (!hit || !declined || hit->approachRecord.value("status").toString()!="SHOOT" ||
                        declined->approachRecord.value("status").toString()!="DECLINE")
                    {
                        out["unpricedCaptureApproach"]=initial.approachRecord;
                        return {};
                    }
                    auto hitRecord=record, declineRecord=record;
                    const auto bypass=priceFollowupLeaf(*staged,out,guard,hold,followup,punish,defendHome,ApproachBranch::Bypass,&*initial.approach);
                    const auto shot=priceFollowupLeaf(*hit,hitRecord,guard,hold,followup,punish,defendHome,ApproachBranch::Shoot,&*initial.approach);
                    const auto avoid=priceFollowupLeaf(*declined,declineRecord,guard,hold,followup,punish,defendHome,ApproachBranch::Decline,&*initial.approach);
                    if (!bypass || !shot || !avoid) return {};
                    auto rankArray=[](const CombatRank &r) {
                        return QJsonArray{static_cast<int>(std::get<0>(r)),std::get<1>(r),std::get<2>(r),
                            static_cast<qint64>(std::get<3>(r))};
                    };
                    const bool safeAvoid=declined->approachRecord.value("avoidCertified").toBool(true);
                    std::optional<CombatRank> bestDecline=safeAvoid ? avoid : std::nullopt;
                    QJsonObject replacement;
                    if (declined->replacementApproach)
                    {
                        // The original actor still waits in both branches. A teammate taking
                        // its place must survive the very same staged shot and spend its own F1.
                        const auto *other=&*declined->replacementApproach;
                        const auto bypassOther=project(hold,guard,followup,punish,defendHome,ApproachBranch::Bypass,other,&*initial.approach);
                        const auto hitOther=project(hold,guard,followup,punish,defendHome,ApproachBranch::Shoot,other,&*initial.approach);
                        if (!bypassOther || !hitOther || bypassOther->approachRecord.value("status").toString()!="BYPASS" ||
                            hitOther->approachRecord.value("status").toString()!="SHOOT")
                        {
                            out["unpricedCaptureApproach"]=declined->approachRecord;
                            return {};
                        }
                        auto unhurt=record, injured=record;
                        const auto safe=priceFollowupLeaf(*bypassOther,unhurt,guard,hold,followup,punish,defendHome,ApproachBranch::Bypass,other,&*initial.approach);
                        const auto fired=priceFollowupLeaf(*hitOther,injured,guard,hold,followup,punish,defendHome,ApproachBranch::Shoot,other,&*initial.approach);
                        if (!safe || !fired) return {};
                        const bool shoots=*fired<*safe;
                        const auto replacementRank=shoots ? *fired : *safe;
                        const bool useOther=!bestDecline || replacementRank>*bestDecline;
                        if (useOther) { bestDecline=replacementRank; declineRecord=shoots ? std::move(injured) : std::move(unhurt); }
                        replacement={{"bypass",bypassOther->approachRecord},{"shoot",hitOther->approachRecord},
                            {"bypassRank",rankArray(*safe)},{"shootRank",rankArray(*fired)},
                            {"selectedIfDeclining",useOther},{"enemyFiresIfReplaced",shoots}};
                    }
                    const bool enemyFires=*shot<*bypass;
                    const auto approachRank=enemyFires ? *shot : *bypass;
                    const bool waitWins=bestDecline && *bestDecline>approachRank;
                    if (waitWins) out=std::move(declineRecord);
                    else if (enemyFires) out=std::move(hitRecord);
                    QJsonObject comparison{{"offer",initial.approachRecord},
                        {"bypass",staged->approachRecord},{"shoot",hit->approachRecord},{"decline",declined->approachRecord},
                        {"bypassRank",rankArray(*bypass)},{"shootRank",rankArray(*shot)},
                        {"enemyFiresIfApproached",enemyFires},{"selected",waitWins ? "DECLINE" : enemyFires ? "SHOOT" : "BYPASS"}};
                    if (safeAvoid) comparison["avoidRank"]=rankArray(*avoid);
                    if (bestDecline) comparison["declineRank"]=rankArray(*bestDecline);
                    if (!replacement.isEmpty()) comparison["replacement"]=replacement;
                    out["captureApproachComparison"]=comparison;
                    return waitWins ? *bestDecline : approachRank;
                };
                if (guardChoices.empty())
                    guardChoices.push_back(NO_UNIT);
                QJsonArray guardAlternatives;
                DecisionRange unpricedAttack;
                int aimCount=0;
                std::optional<CombatRank> bestDefense;
                QJsonObject bestRecord;
                for (const int guard : guardChoices)
                {
                    bool optionalHold = false;
                    auto aim = [&](bool hold, bool defendHome, QJsonObject &out) -> std::optional<CombatRank> {
                        ++aimCount;
                        const auto passive = project(hold, guard, false, false, defendHome);
                        const auto failure=routeCache.lastFailure;
                        const auto menu=passive?passive->followupDecision:failure.value("followupDecision").toObject();
                        if(!menu.isEmpty() && (!menu.value("shots").toArray().isEmpty() || menu.value("workExhausted").toBool())){
                            if(passive)optionalHold=passive->hasOptionalHold;
                            const auto start=budget;QJsonArray visits;
                            const bool parentBound=proofHistoryKnown();
                            auto replayChoice=[&](const std::optional<RequestedFollowup> &order,bool take) {
                                FollowupVisit visit;
                                const auto begin=budget;
                                const auto prev=selectedFollowup;const bool prevConditional=conditionalFollowupLeaf;
                                const auto prevProof=followupParentKnown;const auto prevStart=followupStart;
                                selectedFollowup=order?&*order:nullptr;conditionalFollowupLeaf=true;
                                followupParentKnown=parentBound && followupEnvironmentKnown();followupStart=debtStamp();
                                const auto reset=qScopeGuard([&]{
                                    selectedFollowup=prev;conditionalFollowupLeaf=prevConditional;
                                    followupParentKnown=prevProof;followupStart=prevStart;
                                });
                                std::optional<CaptureRaceResult> rebuilt;
                                const CaptureRaceResult *r=nullptr;
                                // A completed passive recording already carries the exact
                                // selected prior debit. Retain the missing debit as unknown.
                                if(!take && passive)r=&*passive;
                                else if(budget>0) {
                                    const bool reusesVerifiedDecline=!take && !order && menu.value("declinePriorKnown").toBool();
                                    const bool validRequest=order || reusesVerifiedDecline;
                                    if(validRequest)rebuilt=project(hold,guard,take,false,defendHome);
                                    if(rebuilt)r=&*rebuilt;
                                }
                                if(!r){
                                    visit.record["open"]=begin<=0?"NO_REMAINING_FOLLOWUP_WORK":"FIXED_F1_CALENDAR_OR_CHOICE_UNPRICED";
                                    const auto stopped=begin<=0?failure:routeCache.lastFailure;
                                    visit.record["tail"]=stopped;
                                    if(visit.record["tail"].toObject().isEmpty())
                                        visit.record["tail"]=QJsonObject{{"phase","ABSENT_FIXED_FRIENDLY_SUCCESSOR"}};
                                    // Even if the passive E1 continuation could not finish
                                    // its own calendar, retain/replay the observed E1
                                    // families. If F1 approach ownership is unknown, these
                                    // replays are diagnostic: no rank escapes that scope.
                                    const auto e1=stopped.value("followupDecision").toObject().value("responseMenu").toObject();
                                    if(take && order && !e1.isEmpty()){
                                        QJsonArray resumed;
                                        FollowupVisit pass;
                                        pass.record["open"]="PASSIVE_E1_CAPTURE_CALENDAR_UNPRICED";
                                        auto replay=[&](const RequestedFollowup::Adverse &answer){
                                            FollowupVisit v;
                                            if(budget<=0){v.record["open"]="NO_REMAINING_FOLLOWUP_WORK";return v;}
                                            auto q=*order;q.answer=answer;
                                            const auto prev=selectedFollowup;selectedFollowup=&q;
                                            const auto restore=qScopeGuard([&]{selectedFollowup=prev;});
                                            const auto ecost=budget;
                                            auto named=project(hold,guard,true,true,defendHome);
                                            if(named && !named->approach){
                                                auto receipt=record;v.nominal=valueRace(&*named,receipt,guard);
                                                v.record["tail"]=receipt.value("unpricedFinancialInputs");
                                            }else v.record["tail"]=routeCache.lastFailure;
                                            v.record["open"]="E1_WITNESS_AND_F1_APPROACH_SCOPE_UNBOUND";
                                            v.record["replayWork"]=qint64(ecost-budget);
                                            return v;
                                        };
                                        runAdverseFollowups(std::move(pass),e1,replay,resumed);
                                        visit.record["orderedReplies"]=resumed;
                                        visit.record["responseQuantifier"]="OPEN";
                                    }
                                    visit.record["replayWork"]=qint64(begin-budget);
                                    return visit;
                                }
                                optionalHold|=r->hasOptionalHold;
                                const bool history=followupWitnessClean(followupParentKnown,followupEnvironmentKnown(),followupStart,debtStamp());
                                auto receipt=record;
                                visit.nominal=priceApproach(*r,receipt,guard,hold,take,false,defendHome);
                                if(visit.nominal && followupWitnessClean(history,followupEnvironmentKnown(),followupStart,debtStamp()))
                                    visit.range.lower=visit.range.upper=*visit.nominal;
                                else {
                                    receipt["open"]="FULL_F1_COMMITMENT_AND_CAPTURE_CALENDAR_UNPRICED";
                                    receipt["tail"]=routeCache.lastFailure.isEmpty()?receipt.value("unpricedFinancialInputs"):QJsonValue(routeCache.lastFailure);
                                    // A direct Observe leaf is the same F1 policy at every
                                    // E1 outcome. A bound from one approach is never lifted
                                    // over unseen alternative friendly approaches.
                                    if(parentBound && history && followupEnvironmentKnown() && !r->approach && take){
                                        visit.range.upper=economicContinuationEndpoint(receipt.value("fixedE1Upper"));
                                        visit.range.lower=economicContinuationEndpoint(receipt.value("fixedE1Lower"));
                                    }
                                }
                                receipt["replayWork"]=qint64(begin-budget);
                                visit.record=std::move(receipt);return visit;
                            };
                            auto priced=runFriendlyFollowups(menu,replayChoice,visits);
                            QJsonObject hearing{{"owner","F1_FRIENDLY_MAX"},{"guard",guard},{"hold",hold},{"defendHome",defendHome},
                                {"budgetAtStart",qint64(start)},{"work",qint64(start-budget)},{"choices",visits},
                                {"friendlyMenuComplete",menu.value("sourceFriendlyMenuComplete")},{"declinePriorKnown",menu.value("declinePriorKnown")},
                                {"declineOffer",menu.value("declineOffer")},{"physicalOffers",menu.value("shots")}};
                            describeFollowupVisit(hearing,priced);
                            scheduledFollowups.append(hearing);
                            if(priced.nominal)out=std::move(priced.record);
                            out["followupChoiceSchedule"]=hearing;
                            // Global evidence stays open. This separate range was
                            // assembled by max/min using individually clean witnesses.
                            scopedFollowupRange=priced.range;
                            if(!priced.range.exact())evidence.deferChoice("F1_FOLLOWUP_SCHEDULE","UNPRICED_COMPLETE_TEAM_OR_E1_REPLY",
                                priced.range.lower?rankEvidence(*priced.range.lower):QJsonArray{},
                                priced.range.upper?rankEvidence(*priced.range.upper):QJsonArray{});
                            scopedWindow.finished(parentBound && followupEnvironmentKnown(),debtStamp());
                            if(!priced.nominal && routeCache.lastFailure.isEmpty())routeCache.lastFailure=failure;
                            return priced.nominal;
                        }
                        if (!passive)
                            return {};
                        optionalHold = passive->hasOptionalHold;
                        const auto beforePassive=evidence.unresolvedEvents;
                        const auto stockBeforePassive=incompleteStockLeaves;
                        auto best = priceApproach(*passive,out,guard,hold,false,false,defendHome);
                        const bool passiveKnown=best && beforePassive==evidence.unresolvedEvents && stockBeforePassive==incompleteStockLeaves;
                        if (!best || passive->combinedFollowup.value("status").toString() != "AVAILABLE")
                            return best;
                        const auto firing = project(hold, guard, true, false, defendHome);
                        if (!firing || firing->combinedFollowup.value("status").toString() != "TAKEN")
                        {
                            if(passiveKnown)unpricedAttack.raiseLower(*best,"REPLAYED_PASSIVE_CONTINUATION");
                            out["unpricedCombinedFollowup"] = true;
                            evidence.deferChoice("F1_COMBINED_ATTACK","OFFERED_ATTACK_COULD_NOT_BE_REPLAYED",
                                passiveKnown?rankEvidence(*best):QJsonArray{});
                            return best;
                        }
                        auto attack = record;
                        const auto beforeAttack=evidence.unresolvedEvents;
                        const auto stockBeforeAttack=incompleteStockLeaves;
                        auto fired = priceApproach(*firing,attack,guard,hold,true,false,defendHome);
                        const bool unpunishedKnown=fired && beforeAttack==evidence.unresolvedEvents && stockBeforeAttack==incompleteStockLeaves;
                        if (!fired)
                            return {};
                        const auto offered = firing->combinedFollowup.value("adverseReply").toObject();
                        if (offered.value("status").toString() == "AVAILABLE")
                        {
                            // Declining punishment is a legal enemy reply. Once that reply caps the
                            // attack at the passive rank, further punishment cannot improve our choice.
                            if (passiveBoundsOptionalAttack(passiveKnown,*best,unpunishedKnown,*fired))
                            {
                                out["boundedFollowupReply"]=QJsonObject{{"reason","ATTACK_CANNOT_IMPROVE_IF_ENEMY_DECLINES_PUNISHMENT"},
                                    {"passive",rankEvidence(*best)},{"attackUpperBound",rankEvidence(*fired)}};
                                return best;
                            }
                            const auto adverse = project(hold, guard, true, true, defendHome);
                            if (!adverse || adverse->combinedFollowup.value("adverseReply").toObject().value("status").toString() != "TAKEN")
                            {
                                DecisionRange passiveRange,declineRange;
                                if(passiveKnown)
                                {
                                    passiveRange.raiseLower(*best,"REPLAYED_PASSIVE_CONTINUATION");
                                    passiveRange.reduceUpper(*best,"REPLAYED_PASSIVE_CONTINUATION");
                                }
                                if(unpunishedKnown)declineRange.reduceUpper(*fired,"REPLAYED_DECLINE_PUNISHMENT");
                                const std::array<DecisionChild,2> replies{{{"DECLINE_PUNISHMENT",declineRange},{"PUNISH",{}}}};
                                const std::array<DecisionChild,2> choices{{{"PASSIVE",passiveRange},
                                    {"ATTACK",backUpDecisionRange(DecisionNode::OpponentMin,replies,true)}}};
                                unpricedAttack=backUpDecisionRange(DecisionNode::FriendlyMax,choices,true);
                                out["unpricedFollowupReply"]=true;
                                evidence.deferChoice("F1_COMBINED_ATTACK","ADVERSE_REPLY_COULD_NOT_BE_REPLAYED",
                                    passiveKnown?rankEvidence(*best):QJsonArray{},unpunishedKnown?rankEvidence(*fired):QJsonArray{});
                                return best;
                            }
                            auto punished = record;
                            const auto hit = priceApproach(*adverse,punished,guard,hold,true,true,defendHome);
                            if (!hit)
                                return {};
                            const auto bypass = attack.value("policyTotal"), shoot = punished.value("policyTotal");
                            if (*hit < *fired)
                            {
                                fired = hit;
                                attack = std::move(punished);
                            }
                            attack["opponentBypassesSupportTotal"] = bypass;
                            attack["opponentHitsSupportTotal"] = shoot;
                        }
                        const auto without = out.value("policyTotal"), with = attack.value("policyTotal");
                        if (*fired > *best)
                        {
                            best = fired;
                            out = std::move(attack);
                        }
                        out["withoutFollowupTotal"] = without;
                        out["withFollowupTotal"] = with;
                        return best;
                    };
                    auto continuation = [&](bool hold, QJsonObject &out) -> std::optional<CombatRank> {
                        auto best = aim(hold,false,out);
                        const auto originalHold = optionalHold;
                        const auto facts = out.value("homeCaptureResponse").toObject();
                        if (!best || !facts.value("urgent").toBool() || facts.value("secured").toBool()) return best;
                        auto defense = record;
                        const auto ranked = aim(hold,true,defense);
                        optionalHold = originalHold || optionalHold;
                        if (!ranked)
                        {
                            out["unpricedHomeCaptureResponse"] = true;
                            evidence.deferChoice("HOME_CAPTURE_DEFENSE","OFFERED_INTERRUPTION_COULD_NOT_BE_REPLAYED");
                            return best;
                        }
                        const auto alternative=defense.value("homeCaptureResponse").toObject();
                        const auto material=out.value("policyTotal"), interruption=defense.value("policyTotal");
                        const bool chosen=alternative.value("secured").toBool() && *ranked > *best;
                        if (chosen) {best=ranked;out=std::move(defense);}
                        out["materialShotPolicyTotal"]=material;
                        out["homeInterruptionPolicyTotal"]=interruption;
                        out["homeInterruptionSelected"]=chosen;
                        return best;
                    };
                    auto leaving = record;
                    auto best = continuation(false, leaving);
                    if (!best)
                        return {};
                    if (optionalHold)
                    {
                        auto staying = record;
                        const auto held = continuation(true, staying);
                        if (!held)
                            return {};
                        const auto leftValue = leaving.value("policyTotal"), heldValue = staying.value("policyTotal");
                        const bool keepHold = *held > *best;
                        if (keepHold)
                        {
                            best = held;
                            leaving = std::move(staying);
                        }
                        leaving["nextCapturePolicy"] = keepHold ? "HOLD_AND_START" : "WITHDRAW_ON_F1";
                        leaving["withdrawTotal"] = leftValue;
                        leaving["holdTotal"] = heldValue;
                    }
                    if (guard != NO_UNIT)
                        guardAlternatives.append(QJsonObject{{"unit", terrain->units[guard].engineUnitId},
                            {"from", tile(plan.combatLocations[guard])}, {"status", leaving.value("hqStatus")},
                            {"policyTotal", leaving.value("policyTotal")}});
                    if (!bestDefense || *best > *bestDefense)
                    {
                        bestDefense = best;
                        bestRecord = std::move(leaving);
                    }
                }
                record = std::move(bestRecord);
                const bool settledFollowupHistory=scheduledFollowups.isEmpty() || proofHistoryKnown();
                record["continuationRangeInputsComplete"]=unpricedSupport.isEmpty() && incompleteStockLeaves==0 && settledFollowupHistory;
                // A local attack bound is a policy bound only when every outer
                // guard, hold and home-defense choice led to this single node.
                if(settledFollowupHistory && aimCount==1 && unpricedAttack.valid() && (unpricedAttack.lower || unpricedAttack.upper))
                {
                    QJsonObject endpoints;
                    if(unpricedAttack.lower)endpoints["passive"]=decisionRankRecord(*unpricedAttack.lower);
                    if(unpricedAttack.upper)endpoints["bestIfEnemyPasses"]=decisionRankRecord(*unpricedAttack.upper);
                    record["optionalAttackDecisionRange"]=endpoints;
                }
                record.remove("verifiedFollowupPolarity");
                if(scopedWindow.canExport(aimCount,followupEnvironmentKnown(),debtStamp()) && scopedFollowupRange.valid() &&
                   !record.value("followupTeamBinding").toString().isEmpty() && (scopedFollowupRange.lower || scopedFollowupRange.upper)){
                    QJsonArray positions,serviced;
                    for(int i=0;i<int(plan.combatLocations.size());++i){
                        const auto hp=plan.realAfterReply.find(i);const auto enemy=plan.enemyReplyHp.find(i);
                        positions.append(QJsonArray{i,tile(plan.combatLocations[i]),hp==plan.realAfterReply.end()?-1.:hp->second,
                            enemy==plan.enemyReplyHp.end()?-1:enemy->second});
                    }
                    for(const auto &a:plan.servicedStockShots)serviced.append(QJsonArray{a.shot.shooter,a.shot.target,tile(a.shot.from),
                        a.native.hit,a.native.counter,a.native.weapon,a.native.counterWeapon,a.shooterBefore,a.shooterAfter,
                        a.targetBefore,a.targetAfter,a.startingLoad[0],a.startingLoad[1],a.returnLoad[0],a.returnLoad[1]});
                    QJsonObject actions=forecastRecord(plan,{});
                    actions["postE0"]=positions;actions["servicedShots"]=serviced;
                    actions["wait"]=plan.enemyWaiting;actions["responseKey"]=keyRecord(plan.afterReply);
                    actions["rosterService"]=record.value("enemyE0Service");actions["policy"]=routeCache.stockComparisonPolicy;
                    if(plan.openingRecruitFight)actions["recruit"]=openingRecruitRecord(*plan.openingRecruitFight,
                        terrain->units.at(plan.openingRecruitFight->shooter).engineUnitId);
                    const auto responseSeal=QString::fromLatin1(QCryptographicHash::hash(
                        QJsonDocument(actions).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
                    record["followupResponseBinding"]=responseSeal;
                    QJsonObject bounds{{"scope","ONLY_OUTER_FRIENDLY_DECISION_AFTER_FIXED_REPLY"},
                        {"team",record.value("followupTeamBinding")},{"policy",routeCache.stockComparisonPolicy},
                        {"response",responseSeal},{"program",scheduledFollowups[0]},
                        {"ancestorBound",true},{"outerChoices",aimCount},{"siblingDebtCoveredByPolarity",true}};
                    if(scopedFollowupRange.lower && !scopedFollowupRange.lowerActions.isEmpty()){
                        bounds["lower"]=rankEvidence(*scopedFollowupRange.lower);
                        bounds["lowerActions"]=QJsonArray::fromStringList(scopedFollowupRange.lowerActions);
                    }
                    if(scopedFollowupRange.upper && !scopedFollowupRange.upperActions.isEmpty()){
                        bounds["upper"]=rankEvidence(*scopedFollowupRange.upper);
                        bounds["upperActions"]=QJsonArray::fromStringList(scopedFollowupRange.upperActions);
                    }
                    if(bounds.contains("lower") || bounds.contains("upper"))record["verifiedFollowupPolarity"]=bounds;
                }
                record["hqGuardAlternatives"] = guardAlternatives;
                return bestDefense;
            };
            const auto score=[&](const ReplyPassEvaluation &plan,QJsonObject &record)->std::optional<CombatRank>{
                record.remove("fundedDeploymentContinuations");
                record.remove("allScoredLeavesAtCommittedPost");
                const auto initialRecord=record;
                DeploymentPrefixes prefixes;
                const auto baseline=scoreWithCache(plan,record,routeCache,prefixes);
                const auto &policy=routeCache.stockComparisonPolicy;
                const auto questions=routeCache.fundedPostQuestions.value(policy).toArray();
                if(questions.isEmpty() || !terrain->completeProductionMenu || !baseline)return baseline;
                const auto menu=proposedDeploymentMenu(questions);
                qsizetype remaining=0;
                for(const auto &[id,posts]:menu)remaining+=posts.size();
                QJsonArray groups;
                for(const auto &[id,posts]:menu)
                {
                    const auto &[side,x,y,build,type]=id;
                    QJsonArray children;
                    for(const auto &[px,py]:posts)
                    {
                        CaptureRaceRouteCache child;
                        child.preparedPropertyBounds=StockRouteLowerBounds(&routeCache.preparedPropertyBounds);
                        child.townMarkets.enabled=true;
                        child.townMarkets.orders=routeCache.townMarkets.orders;
                        child.townMarkets.reply=routeCache.townMarkets.reply;
                        child.stockComparisonPolicy=policy;
                        child.requiredTownChallenges=routeCache.requiredTownChallenges;
                        child.matchedStockChallenges=routeCache.matchedStockChallenges;
                        child.requestedDutyTrips=routeCache.requestedDutyTrips;
                        child.decisionFrontier=routeCache.decisionFrontier;
                        child.townEffort=routeCache.townEffort;
                        child.stockRouteEffort=routeCache.stockRouteEffort;
                        child.paidRouteEffort=routeCache.paidRouteEffort;
                        child.fundedPostCommitment=FundedPostCommitment{side,{x,y},build,type,{px,py}};
                        QJsonArray otherQuestions;
                        for(const auto &q:questions)
                            if(postPurchase(q.toObject())!=id)otherQuestions.append(q);
                        child.fundedPostQuestions[policy]=otherQuestions;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
                        child.townAuditFocus=routeCache.townAuditFocus;
                        child.privateTownQuoteCap=routeCache.privateTownQuoteCap;
                        child.privateDutyAllowance=routeCache.privateDutyAllowance;
#endif
                        // Reserve work for the remaining postings and ordinary
                        // opponent replies. All unspent credits return intact.
                        const auto offered=budget/(remaining+1);
                        --remaining;
                        const auto parked=budget-offered;budget=offered;
                        const auto restore=qScopeGuard([&]{budget+=parked;});
                        auto receipt=initialRecord;
                        const auto value=scoreWithCache(plan,receipt,child,prefixes);
                        routeCache.seenDutyTrips.insert(child.seenDutyTrips.begin(),child.seenDutyTrips.end());
                        const bool whole=value && receipt.value("allScoredLeavesAtCommittedPost").toBool() &&
                            receipt.value("sameStockPrefixCertified").toBool() &&
                            receipt.value("continuationRangeInputsComplete").toBool() &&
                            receipt.value("townComparisonEvidence").toObject()["complete"].toBool();
                        recordEconomicEvidence(receipt,whole);
                        QJsonObject line{{"post",QJsonArray{px,py}},{"offeredWork",static_cast<qint64>(offered)},
                            {"spentWork",static_cast<qint64>(offered-budget)}};
                        for(auto key:{"allScoredLeavesAtCommittedPost","sameStockPrefixCertified","deploymentPrefixGap","continuationRangeInputsComplete","priceStatus","policyTotal","partialPolicyTotal",
                            "terminalClass","hqSecurity","conditionalThreatRank","townComparisonEvidence","unpricedFinancialInputs",
                            "productionOrders","captureClaims","roundCashFlow","fundedPostResponses","optionalAttackDecisionRange",
                            "settledStockIntentions","matchedStockChecks","fundedPostShots"})
                            if(receipt.contains(key))line[key]=receipt.value(key);
                        if(!value)line["refusal"]=child.lastFailure.isEmpty()?QJsonObject{{"reason","FULL_DEPLOYMENT_CONTINUATION_UNPRICED"}}:child.lastFailure;
                        line["receiptDigest"]=QString::fromLatin1(QCryptographicHash::hash(
                            QJsonDocument(receipt).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
                        children.append(line);
                    }
                    const auto range=backedFundedDeployment(side,children);
                    QJsonObject group{{"enemy",bool(side)},{"factory",QJsonArray{x,y}},{"build",build},{"type",type},
                        {"continuations",children},{"menuComplete",false}};
                    if(range.valid() && range.lower){group["achievableLower"]=decisionRankRecord(*range.lower);group["lowerPost"]=range.lowerActions.first();}
                    if(range.valid() && range.upper){group["adverseUpper"]=decisionRankRecord(*range.upper);group["upperPost"]=range.upperActions.first();}
                    groups.append(group);
                }
                record["fundedDeploymentContinuations"]=groups;
                return baseline;
            };
#ifdef COW_COORDINATED_REFINEMENT_TESTING
            const bool fixedOrderAudit = qgetenv("COW_AI_ARENA_TEST") == "1" && qgetenv("COW_COMBAT_REPRICE_ORDERS") == "1";
#else
            constexpr bool fixedOrderAudit = false;
#endif
            auto *buyingAI=qobject_cast<CoreAI*>(map.getPlayer(knowledge.observerId())->getBaseGameInput());
            auto *buyer=buyingAI ? buyingAI->getSimpleProductionSystem() : nullptr;
            const auto openActingBuyer = [&] {
                if (mode == CombatMode::Acting && actingBuyer && actingBuyer == buyer)
                    actingBuyer->openCounterpointTurnStream();
            };
            if (buyer) buyer->beginCounterpointOpeningPreview();
            const auto finishPreview=qScopeGuard([buyer] { if (buyer) buyer->endCounterpointOpeningPreview(); });
            std::map<QByteArray,QJsonObject> openingPreviewCache;
            DamageOracle openingOracle(map);
            const auto limitedMarket=[](const QJsonObject &record){
                const auto rows=record["townCoverage"].toArray();
                return std::any_of(rows.begin(),rows.end(),[](const auto &v){return v.toObject()["status"]=="WORK_LIMITED";});
            };
            // A deeper pass uses the same ceiling on both boards. Reserve enough
            // of the actual turn meter to pay both arms of the preceding pass.
            const auto deepenPair=[&](std::int64_t work,bool limited){
                if(!limited || work<=0 || budget/2<work || routeCache.townEffort>=allowance)return false;
                routeCache.townEffort=std::min<std::int64_t>(allowance,2*routeCache.townEffort);
                return true;
            };
            const auto deepenConfirmedRoutes=[&](const PairProofMeter &meter){
                const auto grow=[&](std::int64_t &factor){factor=std::min<std::int64_t>(allowance,2*factor);};
                if(meter.growTown)grow(routeCache.townEffort);
                if(meter.growStock)grow(routeCache.stockRouteEffort);
                if(meter.growPaid)grow(routeCache.paidRouteEffort);
            };
            const auto withinShare=[&](std::int64_t share,auto price){
                const auto remainder=budget-std::min(share,budget);
                budget-=remainder;
                const auto restore=qScopeGuard([&]{budget+=remainder;});
                return price();
            };
            auto &ordinaryLaneBindings=*serviceBindings;
            const auto hasServiceSite=[&]{return std::any_of(terrain->units.begin(),terrain->units.end(),[&](const auto &pawn){
                        if(!pawn.present || !pawn.enemy)return false;
                        auto *square=map.getTerrain(pawn.tile.x,pawn.tile.y);
                        auto *unit=square->getUnit();auto *building=square->getBuilding();
                        return unit && building && building->getOwner()==unit->getOwner() &&
                            building->canRepair(unit) && unit->canBeRepaired(QPoint(pawn.tile.x,pawn.tile.y));
                    });};
            std::shared_ptr<const ScreenFireProfile> openingFireBounds;
            const auto screenAllowance=geometry/3;
            std::int64_t screenWork=0;
            const auto activateScreen=[&](bool afterRanking,std::int64_t repeatCost,std::int64_t *parked=nullptr){
                if(!terrain->screenDemand || terrain->screenDemand->attempted)return false;
                auto &d=*terrain->screenDemand;
                const auto available=budget+(parked?*parked:0);
                bool invoked=false;
                const bool activated=d.atSharedBoundary(afterRanking,available,screenAllowance,repeatCost,[&](std::int64_t offer){
                    invoked=true;
                    auto remaining=offer;
                    auto screen=prepareScreenFireProfile(map,knowledge,*terrain,walkProfiles,remaining);
                    screenWork=offer-remaining;
                    if(parked)
                    {
                        const auto fromShared=std::min(*parked,screenWork);
                        *parked-=fromShared;budget-=screenWork-fromShared;
                        const auto forBoth=std::min(*parked,std::max<std::int64_t>(0,repeatCost-budget));
                        *parked-=forBoth;budget+=forBoth;
                    }
                    else budget-=screenWork;
                    result["retainedScreenFireProfile"]=QJsonObject{{"reason",screen->reason},{"through",screen->through},
                        {"work",static_cast<qint64>(screenWork)},{"offered",static_cast<qint64>(offer)},
                        {"normalDemand",d.regular},{"afterRanking",afterRanking},
                        {"capper",d.actor},{"holder",d.holder},{"crossing",d.tick}};
                    if(screen->through<0)return false;
                    terrain->screenFire=std::move(screen);return true;
                });
                if(d.attempted && !invoked)
                    result["retainedScreenFireProfile"]=QJsonObject{{"reason","SHARED_SCREEN_RESERVE_UNAVAILABLE"},
                        {"work",0},{"afterRanking",afterRanking},{"capper",d.actor},{"holder",d.holder},{"crossing",d.tick},
                        {"available",static_cast<qint64>(available)},{"ruleReserve",static_cast<qint64>(screenAllowance)},
                        {"wholeGroupReplayReserve",static_cast<qint64>(repeatCost)}};
                return activated;
            };
            const QJsonObject economicObservation{{"day",identity.day},{"player",identity.playerId},{"planSequence",qint64(identity.planSequence)},
                {"board",QString::fromLatin1(map.getMapHash().toHex())},{"horizon",field.horizonTurns()},{"threshold",field.capturePointsToCapture()}};
            std::map<QByteArray,QJsonObject> paydayCache;
            auto rawWorstReply = [&](const std::vector<int> &choices, const ReplyPassEvaluation &beforePurchases,
                                  QJsonObject &rec, const CombatRank *incumbent = nullptr,
                                  CombatFireOrder fireOrder = {}, const ReplyRanks *incumbentReplies = nullptr, const QJsonObject *incumbentEvidence = nullptr,
                                  bool completeProduction=false,bool openingOnly=false) -> std::optional<CombatRank> {
                // Propose a single F0 portfolio from one economic continuation. From here
                // onward every opposing reply faces exactly the same paid units and cash.
                // The independent buyer can choose a different portfolio in the real game.
                QJsonObject completedF1;
                auto *oldF1=routeCache.completedF1Queries;
                routeCache.completedF1Queries=completeProduction && !openingOnly?&completedF1:nullptr;
                const auto keepF1=qScopeGuard([&]{
                    routeCache.completedF1Queries=oldF1;
                    if(!completedF1.isEmpty())rec["checkedF1Opportunities"]=completedF1;
                });
                rec.remove("checkedF1Opportunities");
                auto &markets=routeCache.townMarkets;
                markets.orders={fireOrder.first,fireOrder.second};
                markets.orders.insert(markets.orders.end(),choices.begin(),choices.end());
                markets.reply={int(TacticalReply::Economic),NO_UNIT,NO_UNIT,-1,-1,-1,-1,-1,-1,-1,0};
                routeCache.stockComparisonPolicy="ECONOMIC";
                rec["economicObservation"]=economicObservation;rec.remove("economicOrderBinding");
                rec.remove("economicSourceAttempted");rec.remove("economicSourceGap");rec.remove("economicEndpoint");rec.remove("economicOrderIdentity");
                rec.remove("economicSourceInterrupted");rec.remove("economicSourceWork");
                rec.remove("settledStockIntentions");rec.remove("matchedStockChecks");rec.remove("replyAlternatives");
                rec.remove("verifiedFollowupPolarity");rec.remove("followupTeamBinding");rec.remove("followupResponseBinding");rec.remove("rankedFollowupSchedules");rec.remove("followupChoiceSchedule");rec.remove("observedF1Stock");
                rec.remove("completeOpeningReplyMenu");rec.remove("physicalDecisionBoundary");
                rec.remove("unpricedOpeningReplyFamily");rec.remove("orderedRecruitResponses");
                rec.remove("optionalAttackDecisionRange");rec.remove("continuationRangeInputsComplete");
                rec.remove("openingSurvivalProof");rec.remove("openingSurvivalGap");rec.remove("openingSurvivalProofWork");rec.remove("openingLossProof");rec.remove("openingOrderBinding");rec.remove("singleActivationHqGap");rec.remove("purchaseBoundary");rec.remove("unpricedFinancialInputs");rec.remove("friendlyF1Service");rec.remove("enemyE0Service");
                rec.remove("unpricedReplyPolicy");rec.remove("unpricedReplyReason");rec.remove("unpricedReplyDetails");
                rec.remove("boundedByReply");rec.remove("policyTotal");rec.remove("partialPolicyTotal");
                std::optional<OrderedOpeningPolicy> servicedEnemy;
                const auto previousEnemyOpening=terrain->orderedOpening;
                const auto cleanup=qScopeGuard([&] {
                    terrain->orderedOpening=previousEnemyOpening;
                    if (terrain->production)
                    {
                        terrain->production->friendlyOpening.reset();
                        terrain->production->resolvedOpeningFireSite=INVALID_TILE;
                        terrain->production->canProposeOpening={};
                    }
                    terrain->friendlyBirthSites.clear();
                });
                std::optional<ReplyPassEvaluation> withBirths;
                std::vector<OpeningRecruitTarget> newbornTargets;

                if(openingOnly && !terrain->production)return {};
                if (terrain->production)
                {
                    const auto effort=static_cast<std::int64_t>(terrain->units.size()+beforePurchases.actors.size()+2*purchaseWindow.sitesByCost.size());
                    if (budget<effort) return {};
                    budget-=effort;
                    const auto boundary=purchaseBoundary(problem,*terrain,beforePurchases,purchaseWindow.unitSlots);
                    QJsonArray open, opening, exposed;
                    QJsonObject buyerPreview;
                    QString openingSource="NO_FUNDED_SLOT";
                    QString openingUncertainty;
                    for (const auto &[cost,site]:purchaseWindow.sitesByCost)
                        if (!boundary.occupied.contains(site)) open.append(QJsonArray{site.first,site.second});
                    const auto capacity=purchaseWindow.affordable([&](Cell site){return boundary.occupied.contains(site);},boundary.available);
                    std::vector<FixedProductionPurchase> portfolio;
                    if (capacity>0)
                    {
                        std::map<std::tuple<int,int,QString>,bool> eligibility;
                        std::map<int,OpeningFireLanes> lanes;
                        bool screenComplete=true;
                        terrain->production->canProposeOpening=[&](const ProductionSite &source,const ProductionChoice &choice) {
                            const auto at=source.tile;
                            auto [it,fresh]=eligibility.try_emplace(std::tuple{at.x,at.y,choice.type},true);
                            if (!fresh) return it->second;
                            for (int i=0;i<static_cast<int>(terrain->units.size());++i)
                            {
                                const auto &pawn=terrain->units[i];
                                if (!pawn.enemy || !pawn.present || pawn.availableShots<=0 || choice.incoming[i]<=0) continue;
                                const auto hp=beforePurchases.enemyAfterOwn.find(i);
                                if (hp!=beforePurchases.enemyAfterOwn.end() && hp->second<=0) continue;
                                const auto &unit=knowledge.units()[i];
                                if (!terrain->pay(1)) {screenComplete=false; return false;}
                                if (!reinforcementExposure(*terrain,unit,pawn,pawn.tile)[at.y*terrain->width+at.x]) continue;
                                auto lane=lanes.find(i);
                                if (lane==lanes.end())
                                {
                                    auto built=openingFireLanes(*terrain,knowledge,beforePurchases,boundary,i);
                                    if (!built) {screenComplete=false; return false;}
                                    lane=lanes.emplace(i,std::move(*built)).first;
                                }
                                const auto directDistance=manhattanDistance(pawn.tile,at);
                                bool possible=directDistance>=unit.minRange && directDistance<=unit.maxRange;
                                if (!possible && lane->second.movement)
                                {
                                    const int radius=std::min(unit.maxRange,terrain->width+terrain->height);
                                    for (int y=std::max(0,at.y-radius);y<=std::min(terrain->height-1,at.y+radius) && !possible;++y)
                                        for (int x=std::max(0,at.x-radius);x<=std::min(terrain->width-1,at.x+radius);++x)
                                        {
                                            if (!terrain->pay(1)) {screenComplete=false; return false;}
                                            const int range=manhattanDistance(at,{x,y});
                                            if (range<unit.minRange || range>unit.maxRange || lane->second.blockers.contains({x,y})) continue;
                                            if (lane->second.movement->reachable(x,y,pawn.movement)) {possible=true;break;}
                                        }
                                }
                                if (!possible) continue;
                                exposed.append(QJsonObject{{"factory",tile(at)},{"type",choice.type},{"possibleShooter",pawn.engineUnitId}});
                                it->second=false;
                            }
                            return it->second;
                        };
                        std::optional<QJsonArray> proposal;
                        openingSource="MODELED_JOBS";
                        if (buyer)
                        {
                            const auto projection=openingBuyerProjection(map,knowledge,*terrain,problem,field,beforePurchases,
                                                                         purchaseWindow.treasury,boundary.available);
                            const auto key=QJsonDocument(projection).toJson(QJsonDocument::Compact);
                            auto [it,fresh]=openingPreviewCache.try_emplace(key);
                            if (fresh) it->second=buyer->previewCounterpointOpening(projection);
                            buyerPreview=it->second;
                            if (buyerPreview["supported"].toBool())
                            {
                                QJsonArray orders;
                                std::set<Cell> filled;
                                auto remaining=purchaseWindow.treasury;
                                QString reason;
                                for (const auto &value:buyerPreview["orders"].toArray())
                                {
                                    const auto row=value.toObject();
                                    const TilePoint at{row["x"].toInt(-1),row["y"].toInt(-1)};
                                    const auto &sites=terrain->production->sites;
                                    const auto site=std::find_if(sites.begin(),sites.end(),[&](const auto &s){return s.tile==at;});
                                    if (site==sites.end() || boundary.occupied.contains({at.x,at.y}) ||
                                        !filled.emplace(at.x,at.y).second || orders.size()>=boundary.available)
                                    {reason="PROJECTED_SITE_UNAVAILABLE";break;}
                                    const auto type=row["type"].toString();
                                    const auto price=row["price"].toInteger(-1);
                                    const auto choice=std::find_if(site->menu[0].begin(),site->menu[0].end(),
                                        [&](const auto &unit){return unit.type==type;});
                                    if (choice==site->menu[0].end() || price<=0 || price>remaining/MILLI_FUNDS_PER_FUND ||
                                        choice->cost!=toMilliFunds(price)) {reason="PROJECTED_MENU_MISMATCH";break;}
                                    // The buyer owns this queue. Exposure affects possible enemy
                                    // replies after the purchase and cannot change what was bought.
                                    if (!terrain->production->canProposeOpening(*site,*choice))
                                    {
                                        if(!screenComplete) openingUncertainty="OPENING_EXPOSURE_WORK_LIMIT";
                                        for(const auto &entry:exposed)
                                        {
                                            const auto e=entry.toObject();
                                            if(e["factory"].toArray()!=tile(at) || e["type"].toString()!=type) continue;
                                            const int id=e["possibleShooter"].toInt(NO_UNIT);
                                            const auto actor=std::find_if(terrain->units.begin(),terrain->units.end(),
                                                [&](const CombatPawn &pawn){return pawn.engineUnitId==id;});
                                            if(actor==terrain->units.end()){openingUncertainty="OPENING_RECRUIT_SHOOTER_MISSING";break;}
                                            const auto target=openingRecruitTarget(map,knowledge,*terrain,beforePurchases,*site,*choice,
                                                static_cast<int>(actor-terrain->units.begin()),openingOracle);
                                            if(!target){openingUncertainty="OPENING_RECRUIT_MATCHUP_UNRESOLVED";break;}
                                            if(!newbornTargets.empty()&&newbornTargets.front().shooter!=target->shooter)
                                                openingUncertainty="OPENING_COMBINED_RECRUIT_FIRE_UNRESOLVED";
                                            if(!newbornTargets.empty() && newbornTargets.front().factory!=target->factory)
                                                openingUncertainty="OPENING_MULTIPLE_THREATENED_RECRUITS_UNRESOLVED";
                                            newbornTargets.push_back(*target);
                                        }
                                    }
                                    remaining-=choice->cost;
                                    orders.append(QJsonObject{{"factory",tile(at)},{"type",type},{"price",static_cast<qint64>(choice->cost)}});
                                }
                                if (reason.isEmpty() && buyerPreview["remainingFunds"].toInteger(-1)!=remaining/MILLI_FUNDS_PER_FUND)
                                    reason="PROJECTED_WALLET_MISMATCH";
                                if (reason.isEmpty()) {proposal=orders;openingSource="COUNTERPOINT_PREVIEW";}
                                else
                                {
                                    openingSource="BUYER_VALIDATION_UNRESOLVED";
                                    openingUncertainty=reason;
                                    buyerPreview["validationFailure"]=reason;
                                }
                            }
                        }
                        if (!proposal && !buyerPreview.value("supported").toBool())
                        {
                            const auto before=budget;
                            proposal=proposeOpeningPurchases(*terrain,knowledge,problem,field,beforePurchases,budget,
                                hq.home,hq.away,&routeCache);
                            raceWork+=before-budget;
                        }
                        terrain->production->canProposeOpening={};
                        if (!screenComplete) openingUncertainty="OPENING_EXPOSURE_WORK_LIMIT";
                        if (!proposal)
                        {
                            rec["purchaseBoundary"]=QJsonObject{{"openAffordableSites",open},{"spendableFunds",static_cast<qint64>(purchaseWindow.treasury)},
                                {"affordableBuildSlots",capacity},{"openingSource",openingSource},{"buyerPreview",buyerPreview}};
                            rec["unpricedReplyDetails"]=openingUncertainty.isEmpty() ? routeCache.lastFailure :
                                QJsonObject{{"phase","OPENING_PURCHASES"},{"reason",openingUncertainty}};
                            return {};
                        }
                        for (const auto &item:*proposal)
                        {
                            const auto order=item.toObject();
                            if (order["enemy"].toBool() || order["buildDay"].toInt()!=0) continue;
                            const auto at=factoryPoint(order["factory"]);
                            const auto &sites=terrain->production->sites;
                            const auto site=std::find_if(sites.begin(),sites.end(),[&](const auto &s){return s.tile==at;});
                            if (site==sites.end() || boundary.occupied.contains({at.x,at.y})) return {};
                            const auto type=order["type"].toString();
                            const auto choice=std::find_if(site->menu[0].begin(),site->menu[0].end(),[&](const auto &o){return o.type==type;});
                            if (choice==site->menu[0].end() || choice->cost!=order["price"].toInteger()) return {};
                            portfolio.push_back({at,type,choice->cost});
                            terrain->friendlyBirthSites.emplace(at.x,at.y);
                            opening.append(QJsonObject{{"factory",tile(at)},{"type",type},{"price",static_cast<qint64>(choice->cost)}});
                        }
                    }
                    terrain->production->friendlyOpening=std::move(portfolio);
                    rec["purchaseBoundary"]=QJsonObject{{"openAffordableSites",open},{"spendableFunds",static_cast<qint64>(purchaseWindow.treasury)},
                        {"affordableBuildSlots",capacity},{"modeledOpening",opening},{"unpricedExposedOffers",exposed},
                        {"openingSource",openingSource},{"buyerPreview",buyerPreview}};
                    if(openingOnly)return {};
                    if(beforePurchases.actors.size()==1 && knowledge.units().size()==2){
                        const auto &only=beforePurchases.actors.front();
                        const auto &order=beforePurchases.replay.plan.action(only.actionIndex);
                        if(only.engineUnitId==order.unitId){
                            QJsonArray route;for(const auto &p:order.path)route.append(tile(p));
                            const QJsonArray physical{QJsonObject{{"actor",order.unitId},{"action",order.actionId},
                                {"candidate",only.candidateIndex},{"path",route},{"destination",tile(order.destination)},
                                {"target",tile(order.target)},{"targetUnit",order.targetUnitId}}};
                            const QJsonObject context{{"observation",economicObservation},{"physicalOrders",physical},
                                {"openingNonStock",qint64(beforePurchases.exactNonStock)},{"buyer",rec["purchaseBoundary"]}};
                            const auto key=QJsonDocument(context).toJson(QJsonDocument::Compact);
                            const auto it=paydayCache.find(key);
                            IsolatedPaydayResult bound;
                            const auto started=budget;
                            if(it!=paydayCache.end() && budget>0){--budget;bound.candidate=true;bound.proof=it->second;}
                            else bound=withinShare(budget/2,[&]{return isolatedPayday(map,knowledge,order,rec["purchaseBoundary"].toObject(),
                                field.horizonTurns(),field.capturePointsToCapture(),beforePurchases.exactNonStock,budget);});
                            if(bound.candidate){
                                rec["economicSourceAttempted"]=true;rec["economicOrderIdentity"]=context;rec["economicOrderBinding"]=physical;
                                rec["economicSourceWork"]=qint64(started-budget);
                                if(!bound.proof.isEmpty()){
                                    bound.proof["identity"]=context;bound.proof["physicalOrders"]=physical;
                                    const bool lower=bound.proof["direction"]=="LOWER";
                                    bound.proof["rank"]=QJsonArray{int(TerminalClass::Unresolved),0,0,bound.proof["money"]};
                                    bound.proof["basis"]=lower?"ACHIEVABLE_PRIVATE_CAPTURE_AND_ALL_ADVERSE_PAYDAYS":"ADVERSE_PRIVATE_CAPTURE_AND_ALL_FRIENDLY_PAYDAYS";
                                    if(!lower)bound.proof["adversePolicy"]="EARLIEST_OPPOSING_PRIVATE_CAPTURE";
                                    rec["economicEndpoint"]=bound.proof;
                                    if(it==paydayCache.end())paydayCache.emplace(key,bound.proof);
                                }else{
                                    rec["economicSourceGap"]=bound.open;
                                    rec["economicSourceInterrupted"]=bound.interrupted;
                                    if(bound.interrupted && started<=std::numeric_limits<std::int64_t>::max()/2)
                                        rec["retryArmOffer"]=qint64(std::max<std::int64_t>(1,started*2));
                                }
                            }
                        }
                    }
                    rec.remove("openingSurvivalProof");
                    rec.remove("openingSurvivalGap");
                    QString survivalGap;
                    // Native callback authentication shares the existing full-pair
                    // allowance. Its cost is independent of financial leaf pricing.
                    auto survivalBudget=pairProofAllowance;
                    auto survival=oneVolleyOpeningSurvival(map,knowledge,beforePurchases,rec["purchaseBoundary"].toObject(),survivalBudget,survivalGap);
                    const auto survivalSpent=pairProofAllowance-survivalBudget;
                    if(survivalSpent>0)
                    {
                        result["openingSurvivalProofWork"]=result["openingSurvivalProofWork"].toInteger()+static_cast<qint64>(survivalSpent);
                        result["openingSurvivalProofAttempts"]=result["openingSurvivalProofAttempts"].toInt()+1;
                        rec["openingSurvivalProofWork"]=static_cast<qint64>(survivalSpent);
                    }
                    if(!survival.isEmpty())rec["openingSurvivalProof"]=std::move(survival);
                    else if(!survivalGap.isEmpty())rec["openingSurvivalGap"]=survivalGap;
                    if(beforePurchases.actors.size()==1){
                        const auto &holder=beforePurchases.actors.front();
                        const auto &order=beforePurchases.replay.plan.action(holder.actionIndex);
                        if(holder.engineUnitId==order.unitId){
                            auto laneBudget=pairProofAllowance;
                            const auto lane=assessSingleActivationHeadquarters(map,knowledge,order,rec["purchaseBoundary"].toObject(),
                                field.capturePointsToCapture(),laneBudget);
                            if(laneBudget!=pairProofAllowance){
                                result["singleActivationHqWork"]=result["singleActivationHqWork"].toInteger()+
                                    static_cast<qint64>(pairProofAllowance-laneBudget);
                            }
                            if(!lane.open.isEmpty())rec["singleActivationHqGap"]=lane.open;
                            if(!lane.loss.isEmpty() || !lane.survives.isEmpty()){
                                QJsonArray path;for(auto p:order.path)path.append(tile(p));
                                const QJsonArray physical{QJsonObject{{"actor",order.unitId},{"action",order.actionId},
                                    {"candidate",holder.candidateIndex},{"path",path},{"destination",tile(order.destination)},
                                    {"target",tile(order.target)},{"targetUnit",order.targetUnitId}}};
                                rec["openingOrderBinding"]=physical;
                                const auto bind=[&](QJsonObject p){p["physicalOrders"]=physical;return p;};
                                if(!lane.loss.isEmpty())rec["openingLossProof"]=bind(lane.loss);
                                if(!lane.survives.isEmpty())rec["openingSurvivalProof"]=bind(lane.survives);
                            }
                        }
                    }
                    if(hq.standing!=NO_UNIT)
                    {
                        const auto &unit=knowledge.units()[hq.standing];
                        const auto &pawn=terrain->units[hq.standing];
                        const auto hit=beforePurchases.enemyAfterOwn.find(hq.standing);
                        const int hp=hit==beforePurchases.enemyAfterOwn.end()?unit.hpRounded:hit->second;
                        const int rate=replyCaptureRate(unit.captureRate,unit.hpRounded-hp,false);
                        const auto ownHQ=openingHeadquartersCapture(hq,problem,field,beforePurchases);
                        const bool bothArmiesSurvive=hp>0 && std::any_of(beforePurchases.actors.begin(),beforePurchases.actors.end(),
                            [](const auto &actor){return actor.stepsAfterOwnActions>0;});
                        const bool targeted=std::any_of(beforePurchases.actors.begin(),beforePurchases.actors.end(),
                            [&](const auto &actor){return actor.ownShot.target==hq.standing;});
                        // A purchase callback may affect a remote piece. Its
                        // mere distance from the HQ cannot certify the loss.
                        if(!ownHQ.won && openingHqFinish(hp,rate,unit.capturePoints,field.capturePointsToCapture(),
                            ownHQ.gained,hq.standingTerrain,false)==OpeningHqFinish::Unverified)
                        {
                            const bool terrainCanAct=hq.standingTerrain.newDay && hq.standingTerrain.hasStartOfTurn;
                            const bool fixedPurchases=openingSource=="COUNTERPOINT_PREVIEW" || capacity==0;
                            const bool certified=hq.ordinaryStandingCapture && !terrainCanAct && fixedPurchases &&
                                bothArmiesSurvive && !targeted && ordinaryOpeningCaptureBindings(
                                    *Interpreter::getInstance(),unit.unitId,opening,budget,hq.turnUnits);
                            if(openingHqFinish(hp,rate,unit.capturePoints,field.capturePointsToCapture(),ownHQ.gained,
                                hq.standingTerrain,certified)!=OpeningHqFinish::Proved)
                            {
                                recordUnverifiedHqFinish(rec,terrainCanAct?"STANDING_HQ_TERRAIN_TURN_UNVERIFIED":
                                    "STANDING_HQ_CAPTURE_UNVERIFIED",pawn.engineUnitId,hq.home,hp,rate,
                                    unit.capturePoints,field.capturePointsToCapture());
                                if(!openingUncertainty.isEmpty())rec["unpricedRecruitFight"]=openingUncertainty;
                                return {};
                            }
                            // All validated recruits occupy empty factories and have spent F0.
                            // The winning capture needs no movement, damage, or newborn response.
                            // A reply in which this capper shoots first is an optional alternative.
                            const QJsonObject cap{{"unit",pawn.engineUnitId},{"tile",tile(hq.home)},{"roundedHpAfterOwn",hp},
                                {"carryBefore",unit.capturePoints},{"captureRate",rate},{"threshold",field.capturePointsToCapture()},
                                {"halfTurn",1},{"action","ACTION_CAPTURE"}};
                            TownComparisonEvidence terminalEvidence;
                            std::set<TownIntention> required;
                            for(const auto &i:routeCache.requiredTownChallenges)
                                required.insert({i.tile.x,i.tile.y,i.attackingSide,i.capperUnit,i.entryTick});
                            terminalEvidence.observe({}, {},required,1);
                            if(!terminalEvidence.complete)return {};
                            QJsonObject reply{{"replyPolicy","CAPTURE_STANDING_HQ"},{"hqStatus","ENEMY_CAN_WIN_NEXT_ACTION"},
                                {"terminalClass",int(TerminalClass::ForcedLoss)},{"projectedWinner",-1},{"projectedTerminalHalfTurn",1},
                                {"hqSecurity",-2},{"policyTotal",0},{"standingHeadquartersCapture",cap},{"physicalDecisionBoundary","LOST_ON_FIRST_REPLY"},
                                {"townComparisonEvidence",terminalEvidence.record()},{"shots",QJsonArray{}}};
                            recordEconomicEvidence(reply,true);
                            for(auto it=reply.begin();it!=reply.end();++it)rec[it.key()]=it.value();
                            rec["replyAlternatives"]=QJsonArray{reply};
                            // Do not describe the earlier trial's optional gunfire as part of the capture.
                            rec.remove("nonStock");rec.remove("encirclement");rec.remove("lethalFiringThreats");
                            QJsonArray units;
                            for(const auto &v:rec["units"].toArray())
                            {
                                const auto row=v.toObject();
                                units.append(QJsonObject{{"unit",row["unit"]},{"end",row["end"]},{"afterOwn",row["afterOwn"]},
                                    {"afterReply",row["afterOwn"]}});
                            }
                            rec["units"]=units;
                            rec["physicalDecisionBoundary"]="LOST_ON_FIRST_REPLY";
                            if(!openingUncertainty.isEmpty())rec["optionalRecruitFightOutsideTerminal"]=openingUncertainty;
                            return CombatRank{TerminalClass::ForcedLoss,-2,0,0};
                        }
                    }
                    if (!openingUncertainty.isEmpty())
                    {
                        rec["unpricedReplyDetails"]=QJsonObject{{"phase","OPENING_PURCHASES"},{"reason",openingUncertainty},
                            {"exposure",exposed},{"purchases",opening}};
                        rec["priceStatus"]="UNRESOLVED";
                        return {};
                    }
                    // Every reply to this frozen team starts after the same enemy
                    // payday and building service, including enemies that never fire.
                    if(hasServiceSite()){
                        const auto f0=openingBuyerProjection(map,knowledge,*terrain,problem,field,beforePurchases,0,0);
                        QString binding,gap;
                        auto shared=openingLaneAfterTeam(map,knowledge,problem,*terrain,beforePurchases,f0,opening,
                            [&](GameMap &m,const BattlefieldKnowledge &k,std::int64_t &w){
                                return ordinaryLaneBindings.verify(m,k,opening,w,binding);},budget,&gap);
                        if(!shared){
                            rec["unpricedReplyDetails"]=QJsonObject{{"phase","E0_DAY_START"},{"reason",gap},{"binding",binding}};
                            return {};
                        }
                        // A movement restoration can introduce firing tiles absent
                        // from the pre-service reply menu. Keep that family open.
                        for(int i=0;i<int(terrain->units.size());++i){
                            const auto &pawn=terrain->units[i];const auto &card=shared->after.afterService[i];
                            if(!pawn.enemy || card.hp<=0)continue;
                            const auto ready=unchangedOpeningMovement(false,pawn.movementBeforeFuel,card.fuel);
                            if(!ready || *ready>pawn.movement){
                                rec["unpricedReplyDetails"]=QJsonObject{{"phase","E0_DAY_START"},{"reason","RESTORED_MOVEMENT_MENU"},
                                    {"unit",pawn.engineUnitId},{"tile",tile(card.at)},{"tick",1}};
                                return {};
                            }
                            for(int slot=0;slot<2;++slot)if(pawn.ammoByWeapon[slot]==0 && card.rounds[slot]>0){
                                rec["unpricedReplyDetails"]=QJsonObject{{"phase","E0_DAY_START"},{"reason","RESTORED_WEAPON_MENU"},
                                    {"unit",pawn.engineUnitId},{"tile",tile(card.at)},{"weapon",slot},{"tick",1}};
                                return {};
                            }
                        }
                        for(auto &target:newbornTargets){
                            const auto &card=shared->after.afterService[target.shooter];
                            target.stockHp=card.hp/10.;target.stockLoad=card.rounds;
                        }
                        QJsonArray paid;
                        for(const auto &bill:shared->bill.rows)if(bill.debit || bill.before!=bill.after)
                            paid.append(QJsonObject{{"unit",terrain->units[bill.actor].engineUnitId},{"hpBefore",bill.before/10.},
                                {"hpAfter",bill.after/10.},{"debit",static_cast<qint64>(bill.debit)}});
                        rec["enemyE0Service"]=QJsonObject{{"before",static_cast<qint64>(shared->bill.before)},
                            {"after",static_cast<qint64>(shared->bill.after)},{"subjects",paid},
                            {"materialLessCash",static_cast<qint64>(shared->materialLessCash)}};
                        servicedEnemy.emplace(OrderedOpeningPolicy{std::move(shared->after.afterService),NO_UNIT,
                            std::move(shared->bill),{},openingLaneNativeExchange(*terrain->production),shared->materialLessCash});
                        terrain->orderedOpening=&*servicedEnemy;
                    }
                    if (servicedEnemy || !terrain->friendlyBirthSites.empty())
                    {
                        const auto effort=1+static_cast<std::int64_t>(choices.size())*(1+choices.size());
                        if (!terrain->pay(effort)) return {};
                        withBirths=evaluateReplyPassPlan(problem,choices,board,fireOrder);
                        if (!withBirths->valued()) return {};
                        std::set<int> members=watched;
                        for (const auto &v:rec["units"].toArray()) members.insert(v.toObject()["unit"].toInt());
                        const auto refreshed=forecastRecord(*withBirths,members);
                        for (auto i=refreshed.begin();i!=refreshed.end();++i) rec[i.key()]=i.value();
                    }
                }
                for(auto &target:newbornTargets)
                {
                    terrain->openingRecruitTarget=&target;
                    auto alternate=evaluateReplyPassPlan(problem,choices,board,fireOrder);
                    terrain->openingRecruitTarget=nullptr;
                    if(!alternate.valued())
                    {
                        rec["unpricedReplyDetails"]=QJsonObject{{"phase","OPENING_PURCHASES"},
                            {"reason",alternate.valued()?"NO_PROVEN_RECRUIT_FIRING_LANE":"RECRUIT_FIRING_WORK_LIMIT"},
                            {"enemyShooter",terrain->units[target.shooter].engineUnitId},{"factory",tile(target.factory)}};
                        return {};
                    }
                    auto evidence=rec.value("openingRecruitFights").toArray();
                    if(alternate.openingRecruitFight) evidence.append(openingRecruitRecord(*alternate.openingRecruitFight,terrain->units[target.shooter].engineUnitId));
                    rec["openingRecruitFights"]=evidence;

                }
                if(!newbornTargets.empty())terrain->production->resolvedOpeningFireSite=newbornTargets.front().factory;
                const auto &economic=withBirths ? *withBirths : beforePurchases;
                // The native opening buyer has sealed its orders. Only future
                // production offer enumeration uses the paired proof allowance.
                const bool previousMode=terrain->completeProductionMenu;
                terrain->completeProductionMenu=completeProduction;
                const auto restoreProduction=qScopeGuard([&]{terrain->completeProductionMenu=previousMode;});
                rec["requestedCompleteProductionMenu"]=completeProduction;
                if(completeProduction && !rec.value("economicObservation").toObject().isEmpty() &&
                   !rec.value("purchaseBoundary").toObject().isEmpty()){
                    QJsonArray chronology;
                    for(auto index:economic.replay.ordering.order)chronology.append(index);
                    QJsonArray bound{rec.value("economicObservation"),rec.value("purchaseBoundary"),
                        chronology,QJsonArray{fireOrder.first,fireOrder.second}};
                    for(const auto &a:economic.actors){
                        const auto &act=economic.replay.plan.action(a.actionIndex);
                        QJsonArray path;for(auto pos:act.path)path.append(tile(pos));
                        bound.append(QJsonArray{a.engineUnitId,a.actionIndex,a.candidateIndex,act.actionId,
                            path,tile(act.destination),tile(act.target),act.targetUnitId,a.ownShot.attackerBefore,
                            a.ownShot.attackerAfter,a.ownShot.targetBefore,a.ownShot.targetAfter,a.ownShot.report.weapon,a.ownShot.report.counterWeapon});
                    }
                    rec["followupTeamBinding"]=QString::fromLatin1(QCryptographicHash::hash(
                        QJsonDocument(bound).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
                }
                const auto planFields = rec;
                rec["responseKey"] = keyRecord(economic.afterReply);
                auto worst = score(economic, rec);
                const auto original = rec;
                const auto originalRank = worst;
                if (!worst)
                {
                    if (!rec.contains("unpricedReplyDetails") && !routeCache.lastFailure.isEmpty())
                        rec["unpricedReplyDetails"]=routeCache.lastFailure;
                    return {};
                }
                rec["replyPolicy"] = "ECONOMIC";
                bool unfinishedTownWork=limitedMarket(rec);
                QJsonArray replies;
                const bool recordProgress=progressInput && handoff->stockProposal &&
                    std::ranges::all_of(economic.actors,[&](const auto &a){return a.actorSlot>=0 && a.actorSlot<int(effectiveChoices.size()) &&
                        a.candidateIndex==effectiveChoices[a.actorSlot];});
                auto summary = [&](const QJsonObject &r) {
                    QJsonObject brief;
                    if(recordProgress)brief["witnessedStockStarts"]=witnessedStockStarts(r["captureRouteProofs"].toArray());
                    for(auto key:{"verifiedFollowupPolarity","followupResponseBinding","optionalAttackDecisionRange","continuationRangeInputsComplete","physicalDecisionBoundary","unpricedFinancialInputs","ownershipComponentBounds"})
                        if(r.contains(key))brief[key]=r[key];
                    if(r.contains("townDecisionFacts"))brief["townDecisionFacts"]=r["townDecisionFacts"];
                    if(r.contains("settledStockIntentions"))brief["settledStockIntentions"]=r["settledStockIntentions"];
                    if(r.contains("matchedStockChecks"))brief["matchedStockChecks"]=r["matchedStockChecks"];
                    if(r.contains("physicalE0Class"))brief["physicalE0Class"]=r["physicalE0Class"];
                    if (fixedOrderAudit)
                    {
                        brief["captureClaims"] = r.value("captureClaims");
                        brief["townEncounters"] = r.value("townEncounters");
                        brief["townCoverage"] = r.value("townCoverage");
                        brief["townConflicts"] = r.value("townConflicts");
                        if (r.contains("townFocusAudit")) brief["townFocusAudit"]=r["townFocusAudit"];
                        brief["paidCaptureChecks"] = r.value("paidCaptureChecks");
                        brief["observedStockResponses"]=r.value("observedStockResponses");
                        brief["captureRouteProofs"] = r.value("captureRouteProofs");
                        brief["stockDutyReview"]=r.value("stockDutyReview");
                    }
                    for (auto key : {"fundedDeploymentContinuations", "unpricedFundedPostQuestions", "fundedPostShots", "fundedPostResponses", "replyPolicy", "shots", "units", "policyTotal", "partialPolicyTotal", "priceStatus", "townComparisonEvidence", "hqStatus", "captureIncomeChange", "captureClaims",
                                     "holdForecast", "returnFire", "enemyCaptureActions", "survivorNextShots", "friendlyF1Service",
                                     "encirclement", "lethalFiringThreats", "boxContinuation", "guardAction", "hqApproaches", "contestedHeadquarters", "contestedPropertyCaptures",
                                     "productionValue", "productionOrders", "productionAvailableSites",
                                     "productionPositiveOffers", "productionWorkLimited", "combinedFollowup",
                                     "withoutFollowupTotal", "withFollowupTotal", "boundedFollowupReply", "replacedCapperNextShot",
                                     "opponentBypassesSupportTotal", "opponentHitsSupportTotal", "homeCaptureResponse", "homeInterruptionSelected",
                                     "openingRecruitFight", "materialShotPolicyTotal", "homeInterruptionPolicyTotal", "replacedHomeCapperNextShot", "captureBookingResponse",
                                     "captureApproach", "captureApproachComparison", "hqGuard", "hqGuardAlternatives",
                                     "projectedWinner", "projectedTerminalHalfTurn"})
                        if (r.contains(key))
                            brief[key] = r[key];
                    for (auto key : {"terminalClass", "hqSecurity", "conditionalThreatRank", "nextCapturePolicy",
                                     "withdrawTotal", "holdTotal", "withdrawals"})
                        if (r.contains(key))
                            brief[key] = r[key];
                    return brief;
                };
                bool everyReplyComplete=rec["townComparisonEvidence"].toObject()["complete"].toBool();
                SharedReplyBound<CombatRank> rejection(incumbentEvidence);
                if(incumbent)rejection.observe(rec,*worst);
                // A later, unexplored policy could expose a new actor/date
                // which lowers the incumbent. A provisional upper bound must
                // not remove this challenger from subsequent search.
                const bool immutableTownScope=incumbent && incumbentEvidence && !couldExposeFundedPost(*incumbentEvidence) && rejection.coversUniverse([&](auto checked){
                    for(const auto &at:propertyTiles)
                    {
                        if(at==hq.home || at==hq.away)continue;
                        for(int i=0;i<static_cast<int>(terrain->units.size());++i)
                        {
                            const auto &pawn=terrain->units[i];
                            if(!pawn.present || !knowledge.units()[i].canCapture)continue;
                            const int side=int(pawn.enemy);
                            for(int t=2+side;t<2*field.horizonTurns();t+=2)
                                if(!checked(TownIntention{at.x,at.y,side,pawn.engineUnitId,t}))return false;
                        }
                    }
                    return true;
                });
                replies.append(summary(rec));
                QString unfinishedPolicy="OPENING_REPLY_GENERATION";
                QJsonObject unpricedOpeningFamily;
                QJsonArray orderedOpeningResponses;
                bool completedTraversal=false;
                const auto retainPrefix=qScopeGuard([&]{
                    if(!orderedOpeningResponses.isEmpty())rec["orderedRecruitResponses"]=orderedOpeningResponses;
                    if(!unpricedOpeningFamily.isEmpty())
                    {
                        rec["unpricedOpeningReplyFamily"]=unpricedOpeningFamily;
                        if(!rec.contains("unpricedReplyDetails"))rec["unpricedReplyDetails"]=unpricedOpeningFamily;
                    }
                    if(completedTraversal)return;
                    rec["replyAlternatives"]=replies;
                    rec["completeOpeningReplyMenu"]=false;
                    rec["townWorkLimitedReply"]=unfinishedTownWork;
                    if(!rec.contains("unpricedReplyPolicy"))rec["unpricedReplyPolicy"]=unfinishedPolicy;
                    if(!rec.contains("unpricedReplyReason"))rec["unpricedReplyReason"]=rec.value("unpricedReplyDetails").toObject()
                        .value("reason").toString(QStringLiteral("OPENING_REPLY_NOT_FULLY_REPLAYED"));
                    recordEconomicEvidence(rec,false);
                });
                if (newbornTargets.empty() && std::get<0>(*worst) != TerminalClass::Unresolved)
                {
                    completedTraversal=true;
                    rec["replyAlternatives"] = replies;
                    recordEconomicEvidence(rec,everyReplyComplete);
                    return worst;
                }
                struct Response
                {
                    TacticalReply policy;
                    QString name;
                    int unit{NO_UNIT}, opener{NO_UNIT};
                    TilePoint firingTile{INVALID_TILE};
                    int originalIndex{-1}, anchorIndex{-1};
                    int recruitTarget{-1},recruitAfter{-1};
                    TilePoint recruitFrom{INVALID_TILE};
                    bool beforeBlockOrders{};
                };
                const auto responseName=[](const Response &r) {
                    if(r.recruitTarget>=0)return r.name;
                    auto name=r.unit==NO_UNIT ? r.name : r.name+"_"+QString::number(r.unit);
                    if (r.firingTile!=INVALID_TILE)
                        name+=QString("_FROM_%1_%2").arg(r.firingTile.x).arg(r.firingTile.y);
                    return name;
                };
                std::vector<Response> policies{{TacticalReply::InterruptCapture, "INTERRUPT_CAPTURE"},
                    {TacticalReply::ArmorAndInfantry, "ARMOR_AND_INFANTRY"},
                    {TacticalReply::ClearGuard, "CLEAR_HQ_GUARD"}, {TacticalReply::PreserveCapture, "PRESERVE_CAPTURE"}};
                if (std::any_of(economic.shots.begin(), economic.shots.end(), [&](const auto &s) {
                        return terrain->units[s.shooterIndex].capper;
                    }))
                    policies.push_back({TacticalReply::InfantryCapture, "INFANTRY_PURSUIT_OF_PROPERTIES"});
                // Compare a protected shot and a legal surround before assuming that armor can retreat.
                std::vector<Response> containment;
                for (int i : potentialCappers)
                {
                    const auto &pawn = terrain->units[i];
                    if (economic.combatLocations[i] == INVALID_TILE)
                        continue;
                    const auto captures = original.value("enemyCaptureActions").toArray();
                    const bool acted = std::any_of(captures.begin(), captures.end(), [&](const auto &a) {
                        return a.toObject().value("unit").toInt(NO_UNIT) == pawn.engineUnitId;
                    }) || std::any_of(economic.shots.begin(), economic.shots.end(), [&](const auto &shot) {
                        return shot.shooterIndex == i;
                    });
                    if (acted)
                        policies.push_back({TacticalReply::WaitOnProperty, "WAIT_ON_PROPERTY", pawn.engineUnitId});
                }
                for (const auto &actor : economic.actors)
                {
                    const auto &member = problem.actors()[actor.actorSlot];
                    const auto &pawn = terrain->units[member.knowledgeUnitIndex];
                    const auto &candidate = member.candidates[actor.candidateIndex];
                    if (!pawn.capper && pawn.grid >= 0 && !pawn.ignoresCollision && actor.stepsAfterOwnActions > 0 &&
                        candidate.source.replyFacts && candidate.source.replyFacts->tile->shooters.size() >= 2)
                        containment.push_back({TacticalReply::ContainUnit, "CONTAIN_UNIT", actor.engineUnitId});
                    const auto &option = problem.actors()[actor.actorSlot].candidates[actor.candidateIndex];
                    if (option.destination != hq.home && breakableScreen(problem, economic, actor.actorSlot, option.origin))
                        policies.push_back({TacticalReply::TargetUnit, "TARGET_UNIT", actor.engineUnitId});
                    if (std::any_of(economic.shots.begin(), economic.shots.end(), [&](const auto &s) {
                            return s.targetEngineUnitId==actor.engineUnitId;
                        }))
                        policies.push_back({TacticalReply::BypassUnit, "BYPASS_UNIT", actor.engineUnitId});
                }
                for(const auto &defender:terrain->observedDefenders){
                    const auto &pawn=terrain->units[defender.knowledgeIndex];
                    if(defender.facts->tile->shooters.empty())continue;
                    policies.push_back({TacticalReply::TargetUnit,"TARGET_UNIT",defender.engineUnitId});
                    if(!pawn.capper && pawn.grid>=0 && !pawn.ignoresCollision && defender.facts->tile->shooters.size()>=2)
                        containment.push_back({TacticalReply::ContainUnit,"CONTAIN_UNIT",defender.engineUnitId});
                    if(std::any_of(economic.shots.begin(),economic.shots.end(),[&](const auto &hit){return hit.targetKnowledge==defender.knowledgeIndex;}))
                        policies.push_back({TacticalReply::BypassUnit,"BYPASS_UNIT",defender.engineUnitId});
                }
                policies.insert(policies.begin(), containment.begin(), containment.end());
                const auto booking=bookedCaptureThreat(original.value("captureClaims").toArray(),economic,field,*terrain);
                const int bookedActor=booking.value("unit").toInt(NO_UNIT);
                if (bookedActor!=NO_UNIT && std::none_of(policies.begin(),policies.end(),[&](const Response &r) {
                        return r.policy==TacticalReply::TargetUnit && r.unit==bookedActor;
                    }))
                    policies.insert(policies.begin()+containment.size(),{TacticalReply::TargetUnit,"TARGET_UNIT",bookedActor});

                const bool canPreserveCapture = std::any_of(economic.shots.begin(),economic.shots.end(),
                    [&](const auto &shot) { return terrain->units[shot.shooterIndex].carrying; });
                const bool canClearGuard = !hq.approaching.empty() && std::any_of(economic.actors.begin(),economic.actors.end(),
                    [&](const auto &actor) { return economic.replay.plan.action(actor.actionIndex).destination == hq.home; });
                std::erase_if(policies,[&](const Response &r) {
                    return (r.policy==TacticalReply::PreserveCapture && !canPreserveCapture) ||
                           (r.policy==TacticalReply::ClearGuard && !canClearGuard);
                });
                std::vector<bool> witnessed(newbornTargets.size());
                const auto addRecruitReplies=[&](const Response &stock)->bool {
                    for(int n=0;n<static_cast<int>(newbornTargets.size());++n)
                    {
                        auto &target=newbornTargets[n];
                        const auto &gun=terrain->units[target.shooter];
                        if((stock.policy==TacticalReply::WaitOnProperty && gun.engineUnitId==stock.unit) ||
                           (stock.policy==TacticalReply::PreserveCapture && gun.carrying) ||
                           (stock.policy==TacticalReply::InfantryCapture && gun.capper)) continue;
                        target.afterStockFires=OpeningRecruitTarget::COLLECT;target.from=INVALID_TILE;target.beforeBlockOrders=false;
                        terrain->policy=stock.policy;terrain->replyTarget=stock.unit;
                        terrain->containShooter=stock.opener;terrain->containFrom=stock.firingTile;
                        terrain->openingRecruitTarget=&target;
                        const auto report=evaluateReplyPassPlan(problem,choices,board,fireOrder);
                        terrain->openingRecruitTarget=nullptr;terrain->policy=TacticalReply::Economic;
                        terrain->replyTarget=NO_UNIT;terrain->containShooter=NO_UNIT;terrain->containFrom=INVALID_TILE;
                        if(!report.valued())return false;
                        for(const auto &window:report.openingRecruitWindows)
                        {
                            witnessed[n]=true;
                            auto choice=stock;
                            choice.name=QString("HIT_RECRUIT_%1_%2_BY_%3_WITH_%4_AFTER_%5_AT_%6_%7").arg(target.factory.x)
                                .arg(target.factory.y).arg(terrain->units[target.shooter].engineUnitId).arg(responseName(stock))
                                .arg(window.afterStockFires).arg(window.from.x).arg(window.from.y);
                            choice.recruitTarget=n;choice.recruitAfter=window.afterStockFires;choice.recruitFrom=window.from;
                            if(window.beforeBlockOrders)choice.name+="_BEFORE_BLOCK_ORDERS";
                            choice.beforeBlockOrders=window.beforeBlockOrders;
                            policies.push_back(std::move(choice));
                        }
                    }
                    return true;
                };
                if(!newbornTargets.empty())
                {
                    auto bases=policies;bases.insert(bases.begin(),{TacticalReply::Economic,"ECONOMIC"});
                    for(const auto &base:bases)if(!addRecruitReplies(base))
                    {
                        unfinishedPolicy=responseName(base);
                        rec["unpricedReplyDetails"]=QJsonObject{{"phase","OPENING_PURCHASES"},{"reason","RECRUIT_TIMING_WORK_LIMIT"}};
                        return {};
                    }
                    if(std::find(witnessed.begin(),witnessed.end(),false)!=witnessed.end())
                    {
                        // The usual greedy/policy prefixes did not expose a firing square. A paid clearance is still an opposing option.
                        // Price the same cleared stock order both ways before including it in any fully closed decision.
                        QJsonArray orderedReplies;bool verifiedContact=false,unclosedLane=false;
                        const auto previousMarket=markets.reply;const auto restoreMarket=qScopeGuard([&]{markets.reply=previousMarket;});
                        const auto opening=rec.value("purchaseBoundary").toObject().value("modeledOpening").toArray();
                        for(int candidate=0;candidate<static_cast<int>(newbornTargets.size());++candidate){
                            if(witnessed[candidate])continue;
                            auto patched=priceFoundOpeningLane(map,knowledge,problem,field,*terrain,beforePurchases,opening,newbornTargets[candidate],ordinaryLaneBindings,budget,
                                [&]{return evaluateReplyPassPlan(problem,choices,board,fireOrder);},
                                [&](int arm,const ReplyPassEvaluation &trial,const OrderedOpeningPolicy &witness,QJsonObject &row){
                                    markets.reply={int(TacticalReply::Economic),NO_UNIT,NO_UNIT,-1,-1,-1,-1,-1,-1,-1,0,-17001,arm,witness.reservedShooter};
                                    markets.reply.push_back(static_cast<int>(witness.repair.before));markets.reply.push_back(static_cast<int>(witness.repair.after));
                                    for(const auto &v:witness.afterService){markets.reply.push_back(v.at.x);markets.reply.push_back(v.at.y);markets.reply.push_back(v.hp);markets.reply.push_back(v.fuel);markets.reply.push_back(v.rounds[0]);markets.reply.push_back(v.rounds[1]);}
                                    for(const auto &v:witness.prefix){markets.reply.push_back(v.shot.shooter);markets.reply.push_back(v.shot.target);markets.reply.push_back(v.shot.from.x);markets.reply.push_back(v.shot.from.y);}
                                    QJsonObject economicReceipt;const auto rank=withinShare(budget/(3-arm),[&]{return score(trial,economicReceipt);});
                                    if(rank){row["conditionalRank"]=QJsonArray{int(std::get<0>(*rank)),std::get<1>(*rank),std::get<2>(*rank),static_cast<qint64>(std::get<3>(*rank))};
                                        row["economicMenuComplete"]=economicReceipt.value("townComparisonEvidence").toObject().value("complete").toBool();}
                                    else row["unpricedEconomics"]=routeCache.lastFailure;
                                    return rank;
                                },[&](std::int64_t &remaining)->std::shared_ptr<const ScreenFireProfile>{
                                    if(terrain->screenFire && terrain->screenFire->ordinaryOpeningPurchases && terrain->screenFire->through>=1)return terrain->screenFire;
                                    if(!openingFireBounds){
                                        auto prepared=prepareScreenFireProfile(map,knowledge,*terrain,walkProfiles,remaining,true);
                                        if(prepared && prepared->through>=1)openingFireBounds=prepared;
                                        return prepared;
                                    }
                                    return openingFireBounds;
                                });
                            unclosedLane|=!patched.value("namedFiringLaneImpossible").toBool();
                            verifiedContact|=patched.value("normalContactVerified").toBool();orderedReplies.append(patched);
                        }
                        // A missing hostile response prevents a universal floor.
                        // Other legal opposing choices can still cap this team.
                        // Retain the open family while hearing the known replies.
                        orderedOpeningResponses=std::move(orderedReplies);
                        if(unclosedLane)unpricedOpeningFamily=QJsonObject{{"phase","OPENING_PURCHASES"},{"reason",verifiedContact?
                            "ORDERED_RECRUIT_CONTACT_OTHER_REPLIES_UNRESOLVED":"NO_PROVEN_RECRUIT_FIRING_LANE"}};
                    }
                }
                bool sameMenuPossible = unpricedOpeningFamily.isEmpty();
                if (sameMenuPossible && incumbentReplies)
                {
                    // Containment anchors are discovered during replay. The other labels are already final.
                    const auto oldBase = std::count_if(incumbentReplies->begin(),incumbentReplies->end(),
                        [](const auto &entry) { return !(entry.first.startsWith("CONTAIN_UNIT") && entry.first.contains("_FROM_")); });
                    sameMenuPossible = oldBase==static_cast<int>(policies.size())+1 &&
                        std::all_of(policies.begin(),policies.end(),[&](const auto &r) {return incumbentReplies->contains(responseName(r));});
                }
                for (int i=0; i<static_cast<int>(policies.size()); ++i) policies[i].originalIndex=i;
                // Prior replies order the search only. A bound always uses the new team's actual reply.
                if (incumbentReplies)
                    std::stable_sort(policies.begin(),policies.end(),[&](const Response &a,const Response &b) {
                        const auto left=incumbentReplies->find(responseName(a));
                        const auto right=incumbentReplies->find(responseName(b));
                        return left!=incumbentReplies->end() && (right==incumbentReplies->end() || left->second<right->second);
                    });
                // This moves only already admitted policies. The original indices
                // keep equal-rank witness selection independent of search priority.
                if(routeCache.decisionFrontier)
                    std::stable_partition(policies.begin(),policies.end(),[&](const Response &r){
                        return routeCache.priorityFor(responseName(r)).has_value();
                    });
                // Preserve the original evidence witness on ties, even if a stronger reply is tested first.
                auto worstOrder=std::tuple{false,-1,-1}; // ECONOMIC always comes first.
                struct PricedReply
                {
                    std::vector<ReplyPassShot> shots;
                    std::vector<OpeningLaneAction> servicedStockShots;
                    std::map<int,int> enemyHp;
                    std::vector<TilePoint> positions;
                    std::map<int,double> friendlyHp;
                    MilliFunds nonStock;
                    CombatEncirclement encirclement;
                    int enemyWaiting;
                    QJsonObject record;
                    CombatRank rank;
                    std::optional<OpeningRecruitFight> opening;
                    std::set<TownIntention> stockScope;
                    std::optional<TownIntention> priority;
                    QString physicalE0;
                    QJsonArray fundedPosts;
                };
                std::vector<PricedReply> priced{{economic.shots, economic.servicedStockShots, economic.enemyReplyHp, economic.combatLocations,
                    economic.realAfterReply, economic.exactNonStock, economic.encirclement, economic.enemyWaiting, original, *originalRank,
                    economic.openingRecruitFight,routeCache.stockChallengesFor("ECONOMIC"),routeCache.priorityFor("ECONOMIC"),"ECONOMIC",routeCache.fundedPostQuestions.value("ECONOMIC").toArray()}};
                for (std::size_t policyIndex = 0; policyIndex < policies.size(); ++policyIndex)
                {
                    const Response response = policies[policyIndex];
                    unfinishedPolicy=responseName(response);
                    // A legal opponent reply supplies an upper bound on the worst reply to this exact plan.
                    // Only an incumbent with all its replies evaluated can prune a challenger.
                    const bool sealed=immutableTownScope && incumbentReplies && sameMenuPossible && containment.empty();
                    const bool strictBound=sealed && rejection.rejects(*incumbent);
                    const bool tieBound=sealed && everyReplyComplete && rejection.allMatched() && *worst==*incumbent &&
                        (!incumbentReplies || !improvesMatchedReplies(replies,*incumbentReplies,false,nullptr,sameMenuPossible));
                    if (strictBound || tieBound)
                    {
                        rec["boundedByReply"]=strictBound ? QJsonValue(rejection.witness()) : rec.value("replyPolicy");
                        const auto &upper=strictBound ? rejection.upper() : *worst;
                        rec["worstReplyUpperBound"]=QJsonArray{static_cast<int>(std::get<0>(upper)),std::get<1>(upper),
                            std::get<2>(upper),static_cast<qint64>(std::get<3>(upper))};
                        if(strictBound)worst=upper;
                        rec["incumbentRankAtComparison"] = QJsonArray{static_cast<int>(std::get<0>(*incumbent)),
                            std::get<1>(*incumbent), std::get<2>(*incumbent), static_cast<qint64>(std::get<3>(*incumbent))};
                        break;
                    }
                    const auto policy = response.policy;
                    const auto policyName=responseName(response);
                    terrain->containShooter = response.opener;
                    terrain->containFrom = response.firingTile;
                    terrain->replyTarget = response.unit;
                    terrain->policy = policy;
                    const auto n=response.recruitTarget;
                    if(n>=0)
                    {
                        newbornTargets[n].from=response.recruitFrom;
                        newbornTargets[n].afterStockFires=response.recruitAfter;
                        newbornTargets[n].beforeBlockOrders=response.beforeBlockOrders;
                        terrain->openingRecruitTarget=&newbornTargets[n];
                    }
                    auto alternate = evaluateReplyPassPlan(problem, choices, board, fireOrder);
                    terrain->openingRecruitTarget=nullptr;
                    terrain->policy = TacticalReply::Economic;
                    terrain->replyTarget = NO_UNIT;
                    terrain->containShooter = NO_UNIT;
                    terrain->containFrom = INVALID_TILE;
                    if (!alternate.valued() || (n>=0 && !alternate.openingRecruitFight))
                    {
                        rec["unpricedReplyPolicy"] = policyName;
                        rec["unpricedReplyReason"] = REPLY_PASS_NO_VALUE_NAMES[static_cast<int>(alternate.reason)];
                        return {};
                    }
                    // Different firing squares have different escape and follow-up geometry, even
                    // on identical defense stars. A direct four-sided surround has at most four anchors.
                    const auto &box = alternate.encirclement;
                    if (n<0 && policy == TacticalReply::ContainUnit && response.firingTile == INVALID_TILE &&
                        box.openerKnowledge != NO_UNIT)
                    {
                        const auto beforeAnchors=policies.size();
                        int anchorIndex=0;
                        for (const auto &option : box.firingOptions)
                            if (option.tile != box.firingTile && manhattanDistance(option.tile, box.center) == 1)
                                policies.push_back({policy, response.name, response.unit, box.openerKnowledge, option.tile,
                                                    response.originalIndex,anchorIndex++});
                        if(!newbornTargets.empty() && response.recruitTarget<0)
                        {
                            const auto endAnchors=policies.size();
                            for(auto k=beforeAnchors;k<endAnchors;++k)
                            {
                                const auto anchor=policies[k];
                                if(!addRecruitReplies(anchor)){
                                    unfinishedPolicy=responseName(anchor);
                                    rec["unpricedReplyReason"]="RECRUIT_TIMING_WORK_LIMIT";
                                    return {};
                                }
                            }
                        }
                    }
                    if(n>=0 && alternate.openingRecruitFight)
                    {
                        const auto &shot=*alternate.openingRecruitFight;
                        const auto &originalLoad=newbornTargets[n].recruitLoad;
                        if(shot.recruitAfter<=0 || (originalLoad[0]>0 && shot.recruitLoad[0]==0) ||
                           (originalLoad[1]>0 && shot.recruitLoad[1]==0))
                        {
                            rec["unpricedReplyDetails"]=QJsonObject{{"phase","OPENING_PURCHASES"},
                                {"reason",shot.recruitAfter<=0?"OPENING_CASUALTY_FUTURE_SLOT_UNPRICED":"OPENING_DEPLETED_WEAPON_FUTURE_MATCHUPS"},
                                {"shot",openingRecruitRecord(shot,terrain->units[shot.shooter].engineUnitId)}};
                            return {};
                        }
                    }
                    std::set<int> all;
                    for (const auto &a : alternate.actors)
                        all.insert(a.engineUnitId);
                    auto other = forecastRecord(alternate, all);
                    if(alternate.openingRecruitFight)other["openingRecruitFight"]=openingRecruitRecord(
                        *alternate.openingRecruitFight,terrain->units[alternate.openingRecruitFight->shooter].engineUnitId);
                    other["responseKey"] = keyRecord(alternate.afterReply);
                    routeCache.stockComparisonPolicy=policyName;
                    const auto &stockScope=routeCache.stockChallengesFor(policyName);
                    auto equal = std::find_if(priced.begin(), priced.end(), [&](const PricedReply &p) {
                        return p.priority==routeCache.priorityFor(policyName) && p.fundedPosts==routeCache.fundedPostQuestions.value(policyName).toArray() && p.stockScope==stockScope && p.opening==alternate.openingRecruitFight &&
                               p.shots == alternate.shots && p.servicedStockShots==alternate.servicedStockShots && p.positions == alternate.combatLocations &&
                               p.enemyHp == alternate.enemyReplyHp && p.friendlyHp == alternate.realAfterReply &&
                               p.nonStock == alternate.exactNonStock && p.encirclement == alternate.encirclement &&
                               p.enemyWaiting == alternate.enemyWaiting;
                    });
                    markets.reply={int(policy),response.unit,response.opener,response.firingTile.x,response.firingTile.y,
                        response.recruitTarget,response.anchorIndex,response.recruitAfter,response.recruitFrom.x,
                        response.recruitFrom.y,int(response.beforeBlockOrders)};
                    std::optional<CombatRank> rank;
                    if (equal != priced.end())
                    {
                        for (auto it = equal->record.begin(); it != equal->record.end(); ++it)
                            if (!other.contains(it.key()))
                                other[it.key()] = it.value();
                        rank = equal->rank;
                        other["physicalE0Class"]=equal->physicalE0;
                    }
                    else if ((rank = score(alternate, other)))
                        priced.push_back({alternate.shots, alternate.servicedStockShots, alternate.enemyReplyHp, alternate.combatLocations,
                                          alternate.realAfterReply, alternate.exactNonStock, alternate.encirclement, alternate.enemyWaiting, other, *rank,alternate.openingRecruitFight,stockScope,routeCache.priorityFor(policyName),policyName,routeCache.fundedPostQuestions.value(policyName).toArray()});
                    if (!rank)
                    {
                        rec["unpricedReplyPolicy"] = policyName;
                        rec["unpricedReplyReason"]=other.value("unpricedReplyReason").toString(QStringLiteral("RACE_OR_BUDGET"));
                        if(other.contains("unpricedReplyDetails"))rec["unpricedReplyDetails"]=other["unpricedReplyDetails"];
                        else if(!routeCache.lastFailure.isEmpty())rec["unpricedReplyDetails"]=routeCache.lastFailure;
                        return {};
                    }
                    if (policy==TacticalReply::TargetUnit && response.unit==bookedActor && bookedActor!=NO_UNIT)
                    {
                        auto effect=booking;
                        const auto actual=alternate.realAfterReply.find(booking["unitKnowledge"].toInt());
                        if (actual!=alternate.realAfterReply.end()) effect["actualTargetHp"]=actual->second;
                        QJsonArray actualShots;
                        for (const auto &shot:alternate.shots)
                            if (shot.targetEngineUnitId==bookedActor)
                                actualShots.append(QJsonObject{{"unit",terrain->units[shot.shooterIndex].engineUnitId},
                                    {"from",tile(shot.firingTile)},{"hpBefore",shot.realBefore},{"hpAfter",shot.realAfter},
                                    {"damageValue",static_cast<qint64>(shot.targetCapital)},{"counterValue",static_cast<qint64>(shot.counterCapital)}});
                        effect["shots"]=actualShots;
                        effect["actualClaim"]=QJsonValue::Null;
                        for (const auto &row:other.value("captureClaims").toArray())
                            if (row.toObject()["tile"].toArray()==booking["tile"].toArray())
                            { effect["actualClaim"]=row; break; }
                        other["captureBookingResponse"]=effect;
                    }
                    other["replyPolicy"] = policyName;
                    unfinishedTownWork|=limitedMarket(other);
                    if(incumbent)rejection.observe(other,*rank);
                    everyReplyComplete &= other["townComparisonEvidence"].toObject()["complete"].toBool();
                    replies.append(summary(other));
                    const auto order=std::tuple{response.anchorIndex>=0,response.originalIndex,response.anchorIndex};
                    if (*rank < *worst || (*rank==*worst && order<worstOrder))
                    {
                        worstOrder=order;
                        // A different opponent action can end the game or spend an entirely
                        // different set of friendly follow-ups. Drop evidence unique to the old reply.
                        rec = planFields;
                        for (auto it = other.begin(); it != other.end(); ++it)
                            rec[it.key()] = it.value();
                        worst = rank;
                    }
                }
                if(!unpricedOpeningFamily.isEmpty())
                {
                    unfinishedPolicy="OPENING_REPLY_GENERATION";
                    rec["unpricedReplyDetails"]=unpricedOpeningFamily;
                    return {};
                }
                completedTraversal=true;
                if (!booking.isEmpty()) rec["bookedCaptureThreat"]=booking;
                rec["replyAlternatives"] = replies;
                rec["townWorkLimitedReply"]=unfinishedTownWork;
                rec["completeOpeningReplyMenu"]=!rec.contains("boundedByReply") && !rec.contains("unpricedHeadquartersSupport");
                recordEconomicEvidence(rec,everyReplyComplete && !rec.contains("boundedByReply"));
                if(incumbent && rejection.rejects(*incumbent) && !immutableTownScope)
                {
                    const auto &bound=rejection.upper();
                    rec["provisionalReplyUpperBound"]=QJsonObject{{"reply",rejection.witness()},
                        {"rank",QJsonArray{int(std::get<0>(bound)),std::get<1>(bound),std::get<2>(bound),static_cast<qint64>(std::get<3>(bound))}},
                        {"status","SUBJECT_TO_SHARED_CHALLENGE_DISCOVERY"}};
                }
                return worst;
            };
            // Verification is a shared immutable turn input. A real geometric cut
            // can nominate it while a local price runs, but its charge occurs
            // only after the complete arm has released its temporary allowance.
            const auto worstReply=[&](const std::vector<int> &choices,const ReplyPassEvaluation &plan,QJsonObject &record,
                const CombatRank *incumbent=nullptr,CombatFireOrder fireOrder={},const ReplyRanks *prior=nullptr,
                const QJsonObject *evidence=nullptr,std::int64_t share=-1,bool completeProduction=false)->std::optional<CombatRank>{
                const auto original=record;
                const auto run=[&]{
                    const auto price=[&]{return rawWorstReply(choices,plan,record,incumbent,fireOrder,prior,evidence,completeProduction);};
                    return share<0?price():withinShare(share,price);
                };
                record.remove("retryArmOffer");
                const auto before=budget;auto quoted=run();
                if(share<0 && activateScreen(false,before-budget))
                {
                    record=original;quoted=run();
                    record["replayedWithSharedScreenRules"]=true;
                }
                if(!quoted)
                {
                    record["completeOpeningReplyMenu"]=false;
                    if(!record.contains("unpricedReplyPolicy"))record["unpricedReplyPolicy"]="ECONOMIC";
                    if(!record.contains("unpricedReplyReason"))record["unpricedReplyReason"]=record.value("unpricedReplyDetails").toObject()
                        .value("reason").toString(QStringLiteral("ECONOMIC_OPENING_NOT_FULLY_REPLAYED"));
                    const auto refusal=record.value("unpricedReplyDetails").toObject();
                    const auto required=refusal.value("requiredAtRefusal").toInteger(-1);
                    const auto available=refusal.value("availableAtRefusal").toInteger(-1);
                    if(share>=0 && available>=0 && required>available && share<=std::numeric_limits<qint64>::max()-(required-available))
                        record["retryArmOffer"]=static_cast<qint64>(share)+(required-available);
                    recordEconomicEvidence(record,false);
                }
                return quoted;
            };
            // An unfinished opposing continuation can discover a new matched stock or town challenge. Its known adverse reply can
            // decide a comparison only when every such challenge was already covered on the price owner's own retained board.
            const auto closedForDecision=[&](const QJsonObject &owner,const SharedReplyBound<DecisionRank> &bound,const std::set<QString> &,const VerifiedPost *fixed=nullptr){
                if(fixed?canDiscoverAnotherDeployment(owner,*fixed):couldExposeFundedPost(owner))return false;
                for(const auto &v:owner["replyAlternatives"].toArray()){
                    const auto checked=readSharedTownReply(v.toObject());
                    for(const auto &at:propertyTiles)for(int i=0;i<int(terrain->units.size());++i){const auto &u=terrain->units[i];
                        if(!u.present || !knowledge.units()[i].canCapture)continue;
                        for(int tick=int(u.enemy);tick<2*field.horizonTurns();tick+=2){
                            if(budget<=0)return false;--budget;
                            const TownIntention option{at.x,at.y,int(u.enemy),u.engineUnitId,tick};
                            if(!checked.earnedStock.contains(option) && !checked.stockOffers.contains(option))return false;
                            if(!routeCache.requestedDutyTrips.contains({u.engineUnitId,at.x,at.y,tick}))return false;
                        }
                    }
                }
                return bound.coversUniverse([&](auto has){for(const auto &at:propertyTiles){
                    if(at==hq.home||at==hq.away)continue;
                    for(int i=0;i<int(terrain->units.size());++i){const auto &u=terrain->units[i];if(!u.present||!knowledge.units()[i].canCapture)continue;
                        for(int t=2+int(u.enemy);t<2*field.horizonTurns();t+=2){if(budget<=0)return false;--budget;
                            if(!has(TownIntention{at.x,at.y,int(u.enemy),u.engineUnitId,t}))return false;}}}return true;});
            };
            // A later capture discovery can change both prices. These two
            // empty capture universes are independent of future menu expansion.
            const bool noFutureCaptureChallenges=knowledge.buildings().empty() ||
                (terrain->production && terrain->production->sites.empty() &&
                 std::none_of(knowledge.units().begin(),knowledge.units().end(),[](const auto &u){return u.canCapture;}));
            const auto comparePair=[&](const QJsonObject &a,const QJsonObject &b,bool scope){
                // Purchases are part of the action being compared. Independent modeled
                // shortlists and previews from different random streams cannot bind it.
                if(terrain->production)
                {
                    QJsonValue seed,seedMode;
                    for(const auto *team:{&a,&b})
                    {
                        const auto boundary=team->value("purchaseBoundary").toObject();
                        if(boundary["openingSource"]=="NO_FUNDED_SLOT" && boundary["affordableBuildSlots"].toInt(-1)==0)continue;
                        const auto buyer=boundary["buyerPreview"].toObject();
                        if(boundary["openingSource"]!="COUNTERPOINT_PREVIEW" || !buyer["supported"].toBool() ||
                           !buyer["seed"].isDouble() || !buyer["seedMode"].isString() ||
                           (!seed.isNull() && (seed!=buyer["seed"] || seedMode!=buyer["seedMode"])))
                        {
                            RankedPairProof unknown;
                            unknown.missing.fill("UNVERIFIED_OPENING_BUYER_STREAM");
                            return unknown;
                        }
                        seed=buyer["seed"];seedMode=buyer["seedMode"];
                    }
                }
                auto ranges=matchedContinuationBounds(a,b,scope,noFutureCaptureChallenges);
                const std::array<const QJsonObject*,2> teams{&a,&b};
                for(int i=0;i<2;++i)
                {
                    const auto post=fundedTeamBounds(*teams[i],closedForDecision);
                    if(!post.valid())return RankedPairProof{};
                    if(post.lower && (!ranges[i].lower || *post.lower>*ranges[i].lower)){
                        ranges[i].raiseLower(*post.lower,"ALL_OPPOSING_REPLIES_FUNDABLE_COMMITMENT");
                        ranges[i].lowerActions=post.lowerActions;
                    }
                    if(post.upper && (!ranges[i].upper || *post.upper<*ranges[i].upper)){
                        ranges[i].reduceUpper(*post.upper,"EXECUTABLE_ADVERSE_DEPLOYMENT",post.adversePolicy);
                        ranges[i].upperActions=post.upperActions;
                    }
                }
                return priceDecisionBounds(a,b,scope,closedForDecision,ranges);
            };
            const auto proofScope=[&](bool scope,const std::set<TownIntention> &towns,const StockChallengeMenu &stock){
                QJsonArray jobs,duties;
                for(const auto &i:towns)jobs.append(describeTownIntention(i));
                for(const auto &d:routeCache.seenDutyTrips)duties.append(QJsonArray{d[0],d[1],d[2],d[3]});
                QJsonObject frame{{"scopeClosed",scope},{"towns",jobs},{"stock",describeStockChallenges(stock)},{"duties",duties},
                    {"fundedPostQuestions",routeCache.fundedPostQuestions}};
                if(const auto &q=routeCache.decisionFrontier)
                    frame["focus"]=QJsonObject{{"intention",describeTownIntention(q->intention)},{"policy",q->policy},{"stage",q->stage}};
                return frame;
            };
            const auto verifiedBuyer=[&](const QJsonObject &b){
                return !terrain->production || b["openingSource"]=="NO_FUNDED_SLOT" ||
                    (b["openingSource"]=="COUNTERPOINT_PREVIEW" && b["buyerPreview"].toObject()["supported"].toBool());
            };
            const auto shareServiceValidation=[&](const QJsonObject &a,const QJsonObject &b){
                if(!terrain->production || !hasServiceSite() || !verifiedBuyer(a) || !verifiedBuyer(b) ||
                   !a["modeledOpening"].isArray() || !b["modeledOpening"].isArray())return;
                const auto opening=a["modeledOpening"].toArray(),other=b["modeledOpening"].toArray();
                const auto key=OpeningLaneRulesMemo::methodKey(opening);
                if(key!=OpeningLaneRulesMemo::methodKey(other))return;
                const bool warm=ordinaryLaneBindings.prior.contains(key);const auto start=budget;
                QString reason;
                const bool ok=withinShare(budget/2,[&]{return ordinaryLaneBindings.verify(map,knowledge,opening,budget,reason);});
                QJsonObject meter=result["sharedOpeningServiceWork"].toObject();
                const auto field=ok?(warm?"lookup":"cold"):"refused";
                meter[field]=meter[field].toInteger()+qint64(start-budget);
                meter["pairs"]=meter["pairs"].toInt()+1;
                if(!ok)meter["lastRefusal"]=reason;
                result["sharedOpeningServiceWork"]=meter;
            };
            const auto bindPhysicalTeam=[&](const ReplyPassEvaluation &team,CombatFireOrder fire,const QJsonObject &receipt){
                QJsonArray orders,execution;
                for(const auto &a:team.actors)
                {
                    const auto &candidate=problem.actors()[a.actorSlot].candidates[a.candidateIndex];
                    const auto &action=team.replay.plan.action(a.actionIndex);
                    QJsonArray path;
                    for(const auto &at:action.path)path.append(tile(at));
                    QJsonObject shot;
                    if(a.ownShot.target!=NO_UNIT)
                        shot={{"actorHp",a.ownShot.attackerAfter},{"targetHp",a.ownShot.targetAfter}};
                    orders.append(QJsonObject{{"combatAfter",shot},{"slot",a.actorSlot},{"unit",a.engineUnitId},{"candidate",a.candidateIndex},
                        {"kind",int(candidate.actionKind)},{"action",action.actionId},{"from",tile(candidate.origin)},
                        {"to",tile(action.destination)},{"target",tile(action.target)},{"targetUnit",action.targetUnitId},
                        {"path",path},{"actionIndex",a.actionIndex}});
                }
                for(int i:team.replay.ordering.order)execution.append(i);
                return QJsonObject{{"day",identity.day},{"player",identity.playerId},{"planSequence",static_cast<qint64>(identity.planSequence)},
                    {"horizon",field.horizonTurns()},{"orders",orders},{"executionOrder",execution},
                    {"fireOrder",QJsonArray{fire.first,fire.second}},{"purchaseBoundary",receipt.value("purchaseBoundary")}};
            };
#ifdef COW_COORDINATED_REFINEMENT_TESTING
            // A private saved-game comparison can execute one authenticated team order on its
            // designated turn. Later turns use the ordinary acting planner and native performer.
            const bool executeFixed = mode == CombatMode::Acting && qgetenv("COW_COMBAT_EXECUTE_FIXED_ORDERS") == "1" &&
                qgetenv("COW_COMBAT_EXECUTE_DAY") == QByteArray::number(identity.day) &&
                qgetenv("COW_COMBAT_EXECUTE_PLAYER") == QByteArray::number(identity.playerId);
            if ((mode == CombatMode::Recorded || executeFixed) && fixedOrderAudit)
            {
                const auto fixedStart = budget, fixedRaceStart = raceWork;
                auto roundStart = budget, roundRaceStart = raceWork;
                auto armStart = budget, armRaceStart = raceWork;
                bool roundOpen = false, armOpen = false;
                QJsonArray rounds, armWork;
                QString armName;
                auto finishArm = [&](const QString &status) {
                    if (!armOpen) return;
                    auto meter = privateCombatWork(armStart, budget, armRaceStart, raceWork);
                    meter["name"] = armName;
                    meter["status"] = status;
                    armWork.append(meter);
                    armOpen = false;
                };
                PairProofMeter pairMeter{pairProofAllowance,budget,raceWork,result,-1,pairProofTurnCap,pairProofMaxPasses};
                auto finishRound = [&] {
                    if (!roundOpen) return;
                    finishArm(QStringLiteral("REFUSED_BEFORE_PRICE"));
                    auto meter = privateCombatWork(roundStart, budget, roundRaceStart, raceWork);
                    meter["arms"] = armWork;
                    if(pairMeter.active)meter["dedicatedPairProofPass"]=pairMeter.passes;
                    rounds.append(meter);
                    roundOpen = false;
                };
                std::map<QString,std::function<QJsonArray()>> screenAudits;
                auto finishFixed = [&] {
                    finishRound();
                    pairMeter.finish(result.value("rankedPairDecision").toObject().value("status").toString(QStringLiteral("PAIR_RETURNED_OPEN")));
                    result["remainingCombatWork"] = static_cast<qint64>(budget);
                    result["raceWork"] = static_cast<qint64>(raceWork);
                    result["geometryWork"] = static_cast<qint64>(allowance - budget - raceWork);
                    result["fixedComparisonWork"] = privateCombatWork(fixedStart, budget, fixedRaceStart, raceWork);
                    result["fixedPricingRounds"] = rounds;
                    if(mode==CombatMode::Recorded && terrain->screenDemand && terrain->screenDemand->audit &&
                       !screenAudits.empty() && (terrain->screenFire || activateScreen(true,0)))
                    {
                        // A diagnostic-only nomination cannot alter either normal
                        // team's rank or use a hidden private rules allowance.
                        // Replay only the requested per-policy audit after ranking.
                        QJsonArray audits;
                        const auto before=budget;
                        const auto each=budget/static_cast<std::int64_t>(screenAudits.size());
                        for(auto &[name,price]:screenAudits)
                        {
                            const auto start=budget;
                            auto policies=withinShare(each,price);
                            audits.append(QJsonObject{{"name",name},{"policies",policies},{"offered",static_cast<qint64>(each)},
                                {"used",static_cast<qint64>(start-budget)},{"hasRecordedPolicies",!policies.isEmpty()}});
                        }
                        result["postRankingRetainedScreenAudits"]=audits;
                        result["postRankingScreenReplayWork"]=static_cast<qint64>(before-budget);
                        result["remainingCombatWork"]=static_cast<qint64>(budget);
                        result["totalTurnWorkIncludingPrivateAudit"]=static_cast<qint64>(allowance-budget);
                    }
                    return result;
                };
                QFile request(Settings::getInstance()->getUserPath() + QStringLiteral("combat-reprice-orders.json"));
                result["status"] = "FIXED_ORDER_AUDIT_REFUSED";
                if (!request.open(QIODevice::ReadOnly) || request.size() > 65536)
                    return finishFixed();
                const auto config = QJsonDocument::fromJson(request.readAll()).object();
                if(config.contains("townQuoteCap"))
                {
                    const auto chosen=config.value("townQuoteCap").toInteger(-1);
                    if(mode!=CombatMode::Recorded || chosen<=0 || chosen>allowance)return finishFixed();
                    routeCache.privateTownQuoteCap=chosen;
                    result["privateTownQuoteCap"]=static_cast<qint64>(chosen);
                }
                if(config.contains("stockDutyWork"))
                {
                    if(mode!=CombatMode::Recorded)return finishFixed();
                    const auto cap=config["stockDutyWork"].toInteger(-1);
                    if(cap<0 || cap>allowance)return finishFixed();
                    routeCache.privateDutyAllowance=cap;
                    for(const auto &v:config["stockDutyTrips"].toArray())
                    {
                        const auto leg=v.toArray();
                        if(leg.size()!=4 || leg[0].toInt(-1)<0 || leg[3].toInt(-1)<0 || leg[3].toInt()>=2*field.horizonTurns())return finishFixed();
                        const auto x=leg[1].toInt(-1),y=leg[2].toInt(-1);
                        if(x<0 || y<0 || x>=terrain->width || y>=terrain->height)return finishFixed();
                        routeCache.requestedDutyTrips.insert({leg[0].toInt(),x,y,leg[3].toInt()});
                    }
                    if(routeCache.requestedDutyTrips.empty())return finishFixed();
                    result["privateStockDutyWork"]=static_cast<qint64>(cap);
                }
                if (config.contains("townAudit"))
                {
                    if (mode!=CombatMode::Recorded) return finishFixed();
                    const auto focus=config.value("townAudit").toObject();
                    const auto xy=focus.value("tile").toArray();
                    if (xy.size()!=2 || !focus.value("enemy").isBool()) return finishFixed();
                    const TilePoint at{xy[0].toInt(-1),xy[1].toInt(-1)};
                    if (at.x<0 || at.x>=terrain->width || at.y<0 || at.y>=terrain->height) return finishFixed();
                    const int capper=focus.value("unit").toInt(NO_UNIT);
                    if (focus.contains("unit") && capper<0) return finishFixed();
                    const int entry=focus.value("entryTick").toInt(-1);
                    if (focus.contains("entryTick") && (entry<0 || entry>=2*field.horizonTurns() ||
                        entry%2!=int(focus.value("enemy").toBool()))) return finishFixed();
                    routeCache.townAuditFocus=TownQuoteFocus{at,int(focus.value("enemy").toBool()),capper,entry};
                    if(focus.contains("friendlyExit"))
                    {
                        const auto exit=focus["friendlyExit"].toArray();
                        if(exit.size()!=2 || capper<0 || entry<0)return finishFixed();
                        const TilePoint destination{exit[0].toInt(-1),exit[1].toInt(-1)};
                        if(destination.x<0 || destination.x>=terrain->width || destination.y<0 || destination.y>=terrain->height)
                            return finishFixed();
                        routeCache.townAuditFocus->friendlyExit=destination;
                    }
                }
                const auto cases = config.value("plans").toArray();
                if (config.value("day").toInt(-1) != identity.day || config.value("player").toInt(-1) != identity.playerId ||
                    cases.isEmpty() || cases.size() > 8)
                    return finishFixed();
                if (executeFixed && (!config.value("execute").toBool() || cases.size() != 1 ||
                    config.value("sequence").toInteger(-1) != static_cast<qint64>(identity.planSequence)))
                    return finishFixed();
                const bool checkpointed=mode==CombatMode::Recorded && cases.size()==2 && config["checkpointDiscovery"].toBool();
                const int stopAfter=config.value("stopAfterDiscoveryRounds").toInt(-1);
                if((stopAfter!=-1 && (!checkpointed || stopAfter<1)) || (config.contains("resumeDigest") && !checkpointed))
                    return finishFixed();
                const auto mapDigest=checkpointed?QString::fromLatin1(map.getMapHash().toHex()):QString{};
                QString checkpointBinding;
                bool openingPass=checkpointed;
                const auto physicalBinding=[&](const QJsonArray &priced){
                    if(priced.size()!=2)return QString{};
                    QJsonArray frames{mapDigest,cases,field.horizonTurns()};
                    for(const auto &item:priced){
                        const auto team=item.toObject().value("physicalTeam").toObject();
                        if(team["orders"].toArray().isEmpty() || !verifiedBuyer(team["purchaseBoundary"].toObject()))return QString{};
                        frames.append(team);
                    }
                    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(frames).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
                };
                int discoveryRound=0;
                QJsonArray reports;
                // A later pair can run out of work partway through its replies.
                // Preserve the last complete reply menu as historical evidence
                // under its own challenge set. Only the final round can certify.
                const auto describePricedMenu=[](const QJsonObject &plan){
                    QJsonObject out{{"name",plan["name"]},{"priceStatus",plan["priceStatus"]},
                        {"principal",plan["replyPolicy"]},{"score",plan.value("policyTotal")},
                        {"partialScore",plan.value("partialPolicyTotal")},{"buyer",plan["purchaseBoundary"]}};
                    QJsonArray quoted;
                    for(const auto &v:plan["replyAlternatives"].toArray())
                    {
                        const auto reply=v.toObject();QJsonArray unfinished;int completed=0;
                        for(const auto &item:reply["townCoverage"].toArray())
                        {
                            const auto row=item.toObject();
                            if(row["complete"].toBool()){++completed;continue;}
                            unfinished.append(QJsonArray{row["tile"],row["attackingSide"],row["capperUnit"],
                                row["requiredEntryTick"],row["status"],row["reason"]});
                        }
                        quoted.append(QJsonObject{{"policy",reply["replyPolicy"]},{"state",reply["priceStatus"]},
                            {"score",reply.value("policyTotal")},{"partialScore",reply.value("partialPolicyTotal")},
                            {"completedSlots",completed},{"unfinished",unfinished}});
                    }
                    out["quotedReplies"]=quoted;return out;
                };
                std::set<TownIntention> required;
                bool fixedComplete=false,lastPairScope=false;
                for (;;)
                {
                    reports=QJsonArray{};
                    roundOpen = true;
                    roundStart = budget;
                    roundRaceStart = raceWork;
                    armWork = QJsonArray{};
                    for (const auto &entry : cases)
                    {
                        const auto spec = entry.toObject();
                        armStart = budget;
                        const auto openingWork=ordinaryLaneBindings.openingWork;
                        const auto friendlyWork=ordinaryLaneBindings.friendlyWork;
                        armRaceStart = raceWork;
                        armName = spec.value("name").toString();
                        armOpen = true;
                        const auto orders = spec.value("orders").toArray();
                        if (orders.size() != static_cast<qsizetype>(problem.actors().size()))
                            return finishFixed();
                        std::vector<int> choices(problem.actors().size());
                        std::set<int> watchedActors;
                        for (const auto &actor : problem.actors())
                        {
                            const auto order = orders[actor.canonicalSlot].toObject();
                            const int index = order.value("candidate").toInt(-1);
                            if (index < 0 || index >= static_cast<int>(actor.candidates.size()) ||
                                order.value("unit").toInt(-1) != actor.engineUnitId)
                                return finishFixed();
                            const auto &move = actor.candidates[index];
                            if (order.value("from").toArray() != tile(move.origin) ||
                                order.value("to").toArray() != tile(move.destination) ||
                                order.value("target").toArray() != tile(move.target) ||
                                order.value("kind").toInt(-1) != static_cast<int>(move.actionKind))
                                return finishFixed();
                            choices[actor.canonicalSlot] = index;
                            watchedActors.insert(actor.engineUnitId);
                        }
                        CombatFireOrder fireOrder;
                        if (spec.contains("fireOrder"))
                        {
                            const auto sequence = spec.value("fireOrder").toArray();
                            if (sequence.size() != 2 || !watchedActors.contains(sequence[0].toInt(NO_UNIT)) ||
                                !watchedActors.contains(sequence[1].toInt(NO_UNIT)) || sequence[0] == sequence[1])
                                return finishFixed();
                            fireOrder = {sequence[0].toInt(), sequence[1].toInt()};
                        }
                        const auto plan = evaluateReplyPassPlan(problem, choices, board, fireOrder);
                        if (executeFixed)
                        {
                            if (!plan.valued() || !publish(*handoff, plan, NO_UNIT, sourceReference, assignment))
                                return finishFixed();
                            openActingBuyer();
                        }
                        auto report = forecastRecord(plan, watchedActors);
                        if (fireOrder.first != NO_UNIT)
                            report["fireOrder"] = spec.value("fireOrder");
                        report["name"] = spec.value("name");
                        if(mode==CombatMode::Recorded && routeCache.townAuditFocus && plan.valued())
                        {
                            const auto name=spec.value("name").toString();
                            screenAudits.insert_or_assign(name,[&,choices,plan,fireOrder,original=report]{
                                auto held=original;
                                rawWorstReply(choices,plan,held,nullptr,fireOrder);
                                QJsonArray policies;
                                const auto append=[&](const QJsonObject &v){
                                    if(v.contains("townFocusAudit"))policies.append(QJsonObject{
                                        {"policy",v.value("replyPolicy")},{"townFocusAudit",v.value("townFocusAudit")}});
                                };
                                append(held);for(const auto &v:held.value("replyAlternatives").toArray())append(v.toObject());
                                return policies;
                            });
                        }
                        if(openingPass && plan.valued()){
                            withinShare(roundStart/cases.size(),[&]{return rawWorstReply(choices,plan,report,nullptr,fireOrder,nullptr,nullptr,true,true);});
                        }
                        else if (plan.valued() && !worstReply(choices,plan,report,nullptr,fireOrder,nullptr,nullptr,executeFixed?-1:roundStart/cases.size(),cases.size()==2 && !executeFixed))
                        {
                            report["priceStatus"]="UNRESOLVED";
                            if (report.contains("policyTotal")) report["partialPolicyTotal"]=report.take("policyTotal");
                            result["remainingFixedPlanWork"] = static_cast<qint64>(budget);
                        }
                        report["openingServiceWork"]=ordinaryLaneBindings.openingWork.since(openingWork);
                        report["friendlyF1ServiceWork"]=ordinaryLaneBindings.friendlyWork.since(friendlyWork);
                        if(plan.valued())report["physicalTeam"]=bindPhysicalTeam(plan,fireOrder,report);
                        finishArm(report.value("priceStatus").toString(QStringLiteral("UNPRICED")));
                        reports.append(report);
                        if (executeFixed)
                        {
                            result["acting"] = false;
                            result["forcedArenaBranch"] = true;
                            result["status"] = "FORCED_FIXED_ORDER";
                            result["fixedPlans"] = reports;
                            storeEconomicOrders(report["priceStatus"]=="UNRESOLVED" ? QJsonObject{} : report,plan);
                            return finishFixed();
                        }
                    }
                    finishRound();
                    if(openingPass){
                        openingPass=false;
                        result["fixedOpeningValidationWork"]=rounds.takeAt(rounds.size()-1);
                        checkpointBinding=physicalBinding(reports);
                        if(checkpointBinding.isEmpty()){
                            result["checkpointRefusal"]="NATIVE_TEAM_OR_BUYER_UNBOUND";
                            result["status"]="FIXED_RESUME_REFUSED";return finishFixed();
                        }
                        if(config.contains("resumeDigest")){
                            QFile input(Settings::getInstance()->getUserPath()+QStringLiteral("pair-discovery-resume.json"));
                            const auto bytes=input.open(QIODevice::ReadOnly) && input.size()<=4*1024*1024?input.readAll():QByteArray{};
                            const auto digest=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
                            const auto validQuestion=[&](const TownIntention &i){
                                return i[0]>=0 && i[1]>=0 && i[0]<terrain->width && i[1]<terrain->height &&
                                    i[4]>=0 && i[4]<2*field.horizonTurns() && i[4]%2==i[2] &&
                                    std::any_of(terrain->units.begin(),terrain->units.end(),
                                        [&](const auto &u){return u.present && u.engineUnitId==i[3] && int(u.enemy)==i[2];});
                            };
                            const auto restoredState=bytes.isEmpty() || digest!=config["resumeDigest"].toString()
                                ?std::nullopt:readPairDiscovery(QJsonDocument::fromJson(bytes).object(),checkpointBinding,allowance,validQuestion);
                            if(!restoredState)
                            {
                                result["checkpointRefusal"]="DISCOVERY_BINDING_OR_OBLIGATION_CHANGED";
                                result["status"]="FIXED_RESUME_REFUSED";return finishFixed();
                            }
                            required.insert(restoredState->towns.begin(),restoredState->towns.end());
                            mergeStockChallenges(routeCache.matchedStockChallenges,restoredState->stock);
                            routeCache.seenDutyTrips.insert(restoredState->duties.begin(),restoredState->duties.end());
                            routeCache.townEffort=std::max<qint64>(routeCache.townEffort,restoredState->efforts[0]);
                            routeCache.stockRouteEffort=std::max<qint64>(routeCache.stockRouteEffort,restoredState->efforts[1]);
                            routeCache.paidRouteEffort=std::max<qint64>(routeCache.paidRouteEffort,restoredState->efforts[2]);
                            mergeFundedPostQuestions(routeCache.fundedPostQuestions,restoredState->fundedPosts);
                            routeCache.f1OpportunityShelf->offered=restoredState->movementCuts;
                            routeCache.requestedDutyTrips=routeCache.seenDutyTrips;
                            routeCache.requiredTownChallenges.clear();
                            for(const auto &i:required)routeCache.requiredTownChallenges.push_back({{i[0],i[1]},i[2],i[3],i[4]});
                            result["resumedDiscovery"]=QJsonObject{{"digest",digest},{"towns",int(restoredState->towns.size())},
                                {"stock",describeStockChallenges(restoredState->stock).size()},{"duties",int(restoredState->duties.size())},
                                {"fundedPosts",restoredState->fundedPosts},{"restoredPriceEndpoints",0},{"offeredF1MovementProofs",restoredState->movementCuts.size()},{"beforeFirstPrice",true}};
                        }
                        shareServiceValidation(reports[0].toObject()["purchaseBoundary"].toObject(),reports[1].toObject()["purchaseBoundary"].toObject());
                        continue;
                    }
                    std::int64_t largestArm=0;
                    for(const auto &v:armWork)largestArm=std::max<std::int64_t>(largestArm,v.toObject()["spent"].toInteger());
                    if(activateScreen(false,cases.size()*largestArm))continue;
                    const bool allRepliesVisited=std::all_of(reports.begin(),reports.end(),[](const auto &v){
                        const auto p=v.toObject();
                        return !p.contains("unpricedReplyPolicy") && !p.value("replyAlternatives").toArray().isEmpty();
                    });
                    if(allRepliesVisited)
                    {
                        QJsonArray plans,scope;
                        for(const auto &v:reports)plans.append(describePricedMenu(v.toObject()));
                        for(const auto &i:required)scope.append(describeTownIntention(i));
                        result["lastCompleteOpponentMenu"]=QJsonObject{{"round",rounds.size()},
                            {"requiredTownIntentions",scope},{"plans",plans},{"remainingCombatWork",static_cast<qint64>(budget)}};
                    }
                    bool newFrontier=false;
                    if(reports.size()==2)
                        newFrontier=mergeFundedPostQuestions(routeCache.fundedPostQuestions,pairedFundedPosts(reports[0].toObject(),reports[1].toObject()));
                    // Missing replies remain obligations in the economic comparison.
                    // They need not hide a dated payoff already capable of changing
                    // which of the physically replayed policies sets the minimum.
                    if(reports.size()==2 && !routeCache.decisionFrontier)
                    {
                        routeCache.decisionFrontier=nominateTownFrontier(reports[0].toObject(),reports[1].toObject(),&budget,TownPriorityScope::ObservedPolicies);
                        newFrontier|=routeCache.decisionFrontier.has_value();
                    }
                    if(routeCache.decisionFrontier && routeCache.decisionFrontier->property() && reports.size()==2)
                        if(auto due=bindTownAdmission(*routeCache.decisionFrontier,reports[0].toObject(),reports[1].toObject()))
                        {
                            result["pairedTownAdmission"]=townFrontierPair(*routeCache.decisionFrontier,reports[0].toObject(),reports[1].toObject());
                            routeCache.decisionFrontier=std::move(due);newFrontier=true;
                        }
                    if(routeCache.decisionFrontier && reports.size()==2)
                        result["pairedTownFrontier"]=townFrontierPair(*routeCache.decisionFrontier,reports[0].toObject(),reports[1].toObject());
                    std::set<TownIntention> next;
                    StockChallengeMenu stockNext;
                    QJsonArray pairedStockGaps;
                    fixedComplete=true;
                    for (int i=0;i<reports.size();++i)
                    {
                        const auto plan=reports[i].toObject();
                        fixedComplete &= plan.value("priceStatus")=="WITNESSED";
                        for (int j=0;j<i;++j)
                        {
                            const auto found=additionalTownChallenges(reports[j].toObject(),plan,required);
                            next.insert(found.begin(),found.end());
                            const auto other=reports[j].toObject();
                            mergeStockChallenges(stockNext,additionalStockChallenges(other,plan,routeCache.matchedStockChallenges));
                            fixedComplete&=stockComparisonCovered(other,plan,routeCache.matchedStockChallenges,&pairedStockGaps);
                        }
                    }
                    mergeStockChallenges(routeCache.matchedStockChallenges,stockNext);
                    if(!stockNext.empty())
                    {
                        pairedStockGaps={};
                        for(int a=0;a<reports.size();++a)for(int b=0;b<a;++b)
                            stockComparisonCovered(reports[b].toObject(),reports[a].toObject(),
                                routeCache.matchedStockChallenges,&pairedStockGaps);
                    }
                    const auto openPosts=reports.size()==2?fundedPostObligations(routeCache.fundedPostQuestions,reports[0].toObject(),reports[1].toObject()):QJsonArray{};
                    result["unansweredFundedPostChecks"]=openPosts;
                    fixedComplete &= openPosts.isEmpty();
                    lastPairScope=openPosts.isEmpty() && !newFrontier && next.empty() && stockNext.empty() &&
                        routeCache.requestedDutyTrips==routeCache.seenDutyTrips;
                    result["unansweredMatchedStockChecks"]=pairedStockGaps;
                    if(checkpointed)
                    {
                        ++discoveryRound;
                        if(physicalBinding(reports)!=checkpointBinding){
                            result["checkpointRefusal"]="VERIFIED_OPENING_CHANGED_DURING_PRICE";
                            lastPairScope=false;fixedComplete=false;break;
                        }
                        if(!checkpointBinding.isEmpty())
                        {
                            PairDiscoveryCheckpoint state{required,routeCache.matchedStockChallenges,routeCache.seenDutyTrips,
                                {routeCache.townEffort,routeCache.stockRouteEffort,routeCache.paidRouteEffort}};
                            state.fundedPosts=routeCache.fundedPostQuestions;
                            state.movementCuts=routeCache.f1OpportunityShelf->proved;
                            state.towns.insert(next.begin(),next.end());
                            const auto payload=QJsonDocument(state.record(checkpointBinding)).toJson(QJsonDocument::Compact);
                            QSaveFile output(Settings::getInstance()->getUserPath()+QStringLiteral("pair-discovery-state.json"));
                            if(!output.open(QIODevice::WriteOnly) || output.write(payload)!=payload.size() || !output.commit())
                                result["checkpointRefusal"]="DISCOVERY_CHECKPOINT_WRITE_FAILED";
                            else result["checkpointRound"]=discoveryRound;
                        }
                        if(stopAfter>0 && discoveryRound>=stopAfter)
                        {
                            // Reaching the requested slice length does not invalidate the
                            // evidence or coverage just established in this round.
                            result["discoveryRoundLimitReached"]=true;
                            break;
                        }
                    }
                    if(cases.size()==2 && !executeFixed && !routeCache.townAuditFocus && !routeCache.privateTownQuoteCap && !routeCache.privateDutyAllowance &&
                       !fixedComplete && pairProofAllowance>0)
                    {
                        const auto a=reports[0].toObject(),b=reports[1].toObject();
                        const bool scope=lastPairScope && pairedStockGaps.isEmpty() && stockComparisonCovered(a,b,routeCache.matchedStockChallenges);
                        const auto proof=comparePair(a,b,scope);
                        if(proof.result()==PairPreference::Open)
                        {
                            auto obligations=required;
                            obligations.insert(next.begin(),next.end());
                            const bool freshScope=newFrontier || !next.empty() || !stockNext.empty() || routeCache.requestedDutyTrips!=routeCache.seenDutyTrips;
                            if(pairMeter.resume(a,b,proofScope(scope,obligations,routeCache.matchedStockChallenges),2,freshScope))
                            {
                                routeCache.requestedDutyTrips=routeCache.seenDutyTrips;
                                required.insert(next.begin(),next.end());
                                routeCache.requiredTownChallenges.clear();
                                for(const auto &i:required)routeCache.requiredTownChallenges.push_back({{i[0],i[1]},i[2],i[3],i[4]});
                                deepenConfirmedRoutes(pairMeter);
                                continue;
                            }
                            if(pairMeter.passes)break;
                        }
                        else break;
                    }
                    if(!routeCache.privateDutyAllowance && !routeCache.privateTownQuoteCap && !routeCache.townAuditFocus &&
                        routeCache.requestedDutyTrips!=routeCache.seenDutyTrips && budget>=2*(roundStart-budget))
                    {
                        routeCache.requestedDutyTrips=routeCache.seenDutyTrips;
                        required.insert(next.begin(),next.end());
                        routeCache.requiredTownChallenges.clear();
                        for(const auto &i:required)routeCache.requiredTownChallenges.push_back({{i[0],i[1]},i[2],i[3],i[4]});
                        continue;
                    }
                    if(newFrontier && next.empty() && stockNext.empty() && budget>=roundStart-budget)continue;
                    if (next.empty() && stockNext.empty())
                    {
                        // User-specified private audit quotas continue to mean
                        // one measured ceiling. The ordinary paired comparison
                        // can devote its remaining meter to the same two squads.
                        const bool stillLimited=std::any_of(reports.begin(),reports.end(),[](const auto &v){
                            return v.toObject()["townWorkLimitedReply"].toBool();
                        });
                        if(fixedComplete || routeCache.privateTownQuoteCap || routeCache.townAuditFocus ||
                           cases.size()!=2 || !deepenPair(roundStart-budget,stillLimited))break;
                        continue;
                    }
                    if (budget<1024)
                    {
                        fixedComplete=false;
                        break;
                    }
                    required.insert(next.begin(),next.end());
                    routeCache.requiredTownChallenges.clear();
                    for (const auto &i:required)
                        routeCache.requiredTownChallenges.push_back({{i[0],i[1]},i[2],i[3],i[4]});
                }
                result["fixedComparisonStatus"]=fixedComplete && (reports.size()!=2 || lastPairScope) ? "WITNESSED" : "UNRESOLVED";
                QJsonArray shared;
                for (const auto &i:required) shared.append(describeTownIntention(i));
                result["requiredTownIntentions"]=shared;
                result["matchedOriginalStockIntentions"]=describeStockChallenges(routeCache.matchedStockChallenges);
                result["reusedCompletedTownSlots"]=routeCache.townMarkets.completedReads;
                result["reusedIdenticalReplySlots"]=routeCache.townMarkets.aliasCompletedReads;
                result["priorityTownMarketEvictions"]=routeCache.townMarkets.priorityEvictions;
                result["resumedExactTownSlots"]=routeCache.townMarkets.resumedReads;
                if(reports.size()==2){const auto a=reports[0].toObject(),b=reports[1].toObject();
                    const bool scope=lastPairScope && additionalTownChallenges(a,b,required).empty() && additionalStockChallenges(a,b,routeCache.matchedStockChallenges).empty() &&
                        stockComparisonCovered(a,b,routeCache.matchedStockChallenges) && routeCache.requestedDutyTrips==routeCache.seenDutyTrips && !routeCache.townAuditFocus;
                    const auto proof=comparePair(a,b,scope);
                    auto comparison=proof.record(a["name"].toString(),b["name"].toString(),
                            routeCache.decisionFrontier ? result.value("pairedTownFrontier").toObject() : QJsonObject{});
                    const auto pending=fundedPostObligations(routeCache.fundedPostQuestions,a,b);
                    comparison["unansweredFundedPostChecks"]=pending;
                    if(proof.result()==PairPreference::Open && !pending.isEmpty() && !comparison.contains("unresolvedConsequence"))
                        comparison["unresolvedConsequence"]=pending.first();
                    comparison["bindings"]=QJsonArray{a.value("physicalTeam"),b.value("physicalTeam")};
                    result["rankedPairDecision"]=comparison;
                    if(result.value("discoveryRoundLimitReached").toBool())
                        result["comparisonInterrupted"]=proof.result()==PairPreference::Open;
                }
                const auto &shelf=*routeCache.f1OpportunityShelf;
                result["f1QueryWork"]=QJsonObject{{"fresh",shelf.fresh},{"liveHits",shelf.liveHits},
                    {"reverifiedMovements",shelf.restored},{"newSparseQuotes",shelf.sparse},{"keyWork",shelf.keyWork},{"oldQuoteWorkAvoided",shelf.saved}};
                result["fixedPlans"] = reports;
                result["status"] = "FIXED_ORDER_AUDIT";
                return finishFixed();
            }
#endif
            openActingBuyer();
            auto base = result["reference"].toObject();
            const auto sourcePriceRecord=base;
            const auto beforeReference = budget;
            auto best = worstReply(effectiveChoices, reference, base);
            if (!best)
            {
                if (base.contains("policyTotal"))
                    base["partialTotal"] = base.take("policyTotal");
                base["priceStatus"] = "UNRESOLVED";
                result["openingReferenceUnpriced"] = true;
            }
            result["reference"] = base;
            const auto referencePriceWork=beforeReference-budget-screenWork;
            const auto certificationReserve=base["townWorkLimitedReply"].toBool()?
                budget/2:std::min(budget/4,2*referencePriceWork);
            budget-=certificationReserve;
            std::set<TownIntention> discovered;
            auto remember=[&](const QJsonObject &record) {
                for (const auto &[policy,items]:quotedTownIntentions(record))
                    discovered.insert(items.begin(),items.end());
            };
            remember(base);
            const auto economicSearchOpen=[&] {return !best || std::get<0>(*best)==TerminalClass::Unresolved;};
            if (best && std::get<0>(*best) == TerminalClass::ForcedLoss && hq.standing != NO_UNIT)
            {
                bool escape = false;
                for (const auto &a : problem.actors())
                    for (const auto &c : a.candidates)
                    {
                        if (!c.plannable)
                            continue;
                        const auto *part = singleComponentOf(c.source.bundle);
                        if (part && ((part->kind == ComponentKind::Fire && part->fire.targetUnitId == hq.standing) ||
                                     (part->kind == ComponentKind::Capture && c.destination == hq.away &&
                                      field.actionCompletesCapture({a.stockRow, c.destination, true}))))
                            escape = true;
                    }
                result["forcedLossInGeneratedActions"] = !escape;
            }
            QJsonArray trials;
            std::optional<ReplyPassEvaluation> winner;
            Trial winningTrial{};
            struct PendingPair
            {
                Trial trial;
                ReplyPassEvaluation plan;
                std::vector<int> orders;
                CombatFireOrder fire;
                MilliFunds opening{};
                int index{-1};
            };
            std::optional<PendingPair> pendingPair;
            std::map<int,std::vector<int>> unfinishedOpenings;
            auto choices = effectiveChoices;
            auto shortlist = combatTrials(problem, effectiveChoices, reference, knowledge, true, hq, potentialCappers, watched,
                                          base["replyAlternatives"].toArray(),terrain.get());
            for (const auto &changed : repair.changes)
            {
                const auto &actor = problem.actors()[changed.slot];
                for (int i = 0; i < static_cast<int>(actor.candidates.size()); ++i)
                    if (i != effectiveChoices[changed.slot] && actor.candidates[i].plannable &&
                        std::none_of(shortlist.begin(),shortlist.end(),[&](const Trial &t) {
                            return t.slot == changed.slot && t.option == i && t.partner < 0;
                        }))
                        shortlist.push_back({changed.slot,i,0});
            }
            addJointCombatOrders(shortlist, problem, reference, knowledge, hq, potentialCappers, terrain.get(), budget, choices, field, result);
            // Compare each funded release explicitly; then compare the corresponding pair
            // even if neither single improves the whole plan by itself.
            std::vector<Trial> dueOptions;
            for (const auto &v : maturedReleases)
            {
                const auto promise = v.toObject();
                const auto exit = factoryPoint(promise["vacaterEnd"]);
                for (const auto &actor : problem.actors())
                {
                    if (actor.engineUnitId != promise["vacater"].toInt(NO_UNIT)) continue;
                    for (int i=0; i<static_cast<int>(actor.candidates.size()); ++i)
                    {
                        const auto &option = actor.candidates[i];
                        if (option.plannable && option.destination == exit && option.source.vacatesActiveFriendlyProduction)
                        {
                            dueOptions.push_back({actor.canonicalSlot,i,-2});
                            break;
                        }
                    }
                }
            }
            for (std::size_t i=0;i<dueOptions.size();++i)
            {
                const auto &a=dueOptions[i];
                auto existing=std::find_if(shortlist.begin(),shortlist.end(),[&](const Trial &t) {
                    return t.slot==a.slot && t.option==a.option && t.partner<0;
                });
                if (existing==shortlist.end()) shortlist.push_back(a);
                else existing->priority=std::min(existing->priority,a.priority);
                for (std::size_t j=0;j<i;++j)
                    shortlist.push_back({a.slot,a.option,-2,dueOptions[j].slot,dueOptions[j].option});
            }
            std::vector<std::pair<int,int>> dueCaptureOptions;
            for (const auto &v:dueStarts)
            {
                const auto promised=v.toObject();
                if (promised["lockedStart"].toBool()) continue;
                const int slot=promised["slot"].toInt(), option=promised["candidate"].toInt();
                dueCaptureOptions.emplace_back(slot,option);
                if (effectiveChoices[slot]==option) continue;
                auto probe=std::find_if(shortlist.begin(),shortlist.end(),[&](const Trial &t) {
                    return t.slot==slot && t.option==option && t.partner<0;
                });
                // An immediate HQ or lethal rescue still takes precedence. Reserve the
                // additional whole-team comparison below even if the first pass is short.
                const int priority=hq.standing==NO_UNIT && hq.approaching.empty() ? -5 : -2;
                if (probe==shortlist.end()) shortlist.push_back({slot,option,priority});
                else probe->priority=std::min(priority,probe->priority);
            }
            std::vector<int> winningChoices;
            QJsonArray winningReplyKey;
            using PlanKey = std::tuple<std::vector<int>, int, int>;
            // Keep the exact complete choice vector and fire order with its outcome. A
            // bound on a different team does not settle a hold on the current incumbent.
            struct DeferredClosingOrder {Trial trial;CombatFireOrder fire;PlanKey key;};
            std::vector<DeferredClosingOrder> movementLimited, unpricedPhysicalOrders;
            std::vector<int> openFactoryWaitSlots;
            if(mode==CombatMode::Acting && !fixedOrderAudit && actingBuyer && buyer==actingBuyer && terrain->production){
                for(const auto &member:reference.actors){
                    const auto &a=reference.replay.plan.action(member.actionIndex);
                    if(a.actionId!="ACTION_WAIT" || member.stepsAfterOwnActions<=0)continue;
                    auto *building=map.getTerrain(a.destination.x,a.destination.y)->getBuilding();
                    if(building && building->getBuildingID()=="FACTORY" && building->getOwner()==map.getPlayer(knowledge.observerId()))
                        openFactoryWaitSlots.push_back(member.actorSlot);
                }
            }
            std::map<PlanKey,QJsonObject> openPurchaseComparisons;
            std::set<PlanKey> settledPurchasePairs;
            std::map<PlanKey,int> attempted;
            attempted.emplace(PlanKey{effectiveChoices,NO_UNIT,NO_UNIT},-1);
            CombatFireOrder winningOrder, anchorOrder;
            // Equal execution sequences for identical team choices have the same actions and opposing replies.
            // Remember only a complete price or a bound established against a fully priced incumbent.
            std::map<std::pair<std::vector<int>, std::vector<int>>, int> pricedSequences;
            pricedSequences.emplace(std::pair{effectiveChoices, reference.replay.ordering.order}, -1);
            QJsonArray equivalentOrders;
            std::map<std::pair<int, int>, CombatRank> estimates;
            using TrialChoices = std::tuple<int,int,int,int>;
            auto trialChoices = [](const Trial &t) { return TrialChoices{t.slot,t.option,t.partner,t.partnerOption}; };
            std::map<TrialChoices,CombatRank> measuredImprovements;
            int winningRecord = -1;
            int round = 0, unpriced = 0, unreplayable = 0, bounded = 0;
            std::vector<int> anchor = effectiveChoices;

            // An illegal composition still needs a master replay and a receipt. Start only
            // if its full admission charge fits; the tactical stages meter their own work.
            const auto replayTariff =
                1 + static_cast<std::int64_t>(choices.size()) * (1 + static_cast<std::int64_t>(choices.size()));
            const auto minimumReplayBudget = std::max<std::int64_t>(1024, replayTariff);
            const auto sourceRivalChoices = handoff->stockProposal
                ? std::span<const std::int32_t>(handoff->stockProposal->choices) : problem.incumbentChoices();
            const auto sourceRivalOrder = handoff->stockProposal
                ? std::span<const std::int32_t>(handoff->stockProposal->executionOrder) : problem.incumbentExecutionOrder();
            const auto sourceRivalLabel = handoff->stockProposal ? QStringLiteral("STOCK_REFINEMENT") : QStringLiteral("MASTER_INCOMING");
            auto evaluateTrial = [&](const Trial &trial, CombatFireOrder order, ReplyPassEvaluation *prepared=nullptr,bool forPairedPrice=false,
                                     const std::vector<int> *completeOrders=nullptr,bool nativeIncoming=false) {
                choices = anchor;
                const auto &actor = problem.actors()[trial.slot];
                const auto &option = actor.candidates[trial.option];
                choices[trial.slot] = trial.option;
                if (trial.partner >= 0) choices[trial.partner] = trial.partnerOption;
                if (trial.companion >= 0) choices[trial.companion] = trial.companionOption;
                if(completeOrders)choices=*completeOrders;
                nativeIncoming |= order.first==NO_UNIT && order.second==NO_UNIT &&
                    std::ranges::equal(choices,sourceRivalChoices);
                const auto [entry,inserted]=attempted.try_emplace(PlanKey{choices,order.first,order.second},trials.size());
                if (!inserted) return;
                if (!prepared) budget -= replayTariff;
                auto evaluated = prepared ? std::move(*prepared) : evaluateReplyPassPlan(problem,choices,board,order);
                const bool originalOrder=!nativeIncoming ||
                    std::ranges::equal(evaluated.replay.ordering.order,sourceRivalOrder);
                if(nativeIncoming && !originalOrder)evaluated.reason=ReplyPassNoValue::ReplayIncomplete;
                if (!evaluated.valued())
                {
                    ++unreplayable;
                    if(evaluated.reason==ReplyPassNoValue::MovementLimit && evaluated.replay.valid() &&
                       evaluated.replay.complete() && anchor==effectiveChoices)
                    {
                        movementLimited.push_back({trial,order,entry->first});
                        unfinishedOpenings.emplace(static_cast<int>(trials.size()),evaluated.replay.ordering.order);
                    }
                }
                else if (const auto same = pricedSequences.find({choices, evaluated.replay.ordering.order});
                         same != pricedSequences.end())
                {
                    entry->second=same->second;
                    equivalentOrders.append(QJsonObject{{"fireOrder", QJsonArray{order.first, order.second}},
                        {"trial", same->second}, {"round", round}});
                    return;
                }
                auto selected = watched;
                selected.insert(actor.engineUnitId);
                auto rec = forecastRecord(evaluated, selected);
                rec["searchRound"] = round;
                if(nativeIncoming)
                {
                    rec["sourceTeam"]=sourceRivalLabel;
                    rec["sourceOrderVerified"]=originalOrder;
                }
                if (order.first != NO_UNIT)
                    rec["fireOrder"] = QJsonArray{order.first,order.second};
                QJsonArray active;
                for (const auto &member : problem.actors())
                    if (choices[member.canonicalSlot] != effectiveChoices[member.canonicalSlot])
                    {
                        const int optionIndex = choices[member.canonicalSlot];
                        const auto &c = member.candidates[optionIndex];
                        active.append(QJsonObject{{"slot", member.canonicalSlot},
                                                  {"unit", member.engineUnitId},
                                                  {"candidate", optionIndex},
                                                  {"destination", tile(c.destination)},
                                                  {"target", tile(c.target)}});
                    }
                rec["changes"] = active;
                rec["unit"] = actor.engineUnitId;
                rec["candidate"] = trial.option;
                rec["baseCandidate"] = effectiveChoices[trial.slot];
                rec["baseDestination"] = tile(actor.candidates[effectiveChoices[trial.slot]].destination);
                if (trial.partner >= 0)
                {
                    rec["partnerUnit"] = problem.actors()[trial.partner].engineUnitId;
                    rec["partnerCandidate"] = trial.partnerOption;
                    const auto &partner = problem.actors()[trial.partner].candidates[trial.partnerOption];
                    rec["partnerDestination"] = tile(partner.destination);
                    rec["partnerTarget"] = tile(partner.target);
                    rec["jointOrder"] = trial.competingAssignments ? "REASSIGN_COMPETING_DESTINATION" :
                        trial.vacatesForCapture ? "VACATE_AND_CAPTURE" :
                        trial.priority == -5 ? "FINISH_AND_GUARD_HQ" :
                        trial.interruptionRank>=0 && partner.target==INVALID_TILE ? "FIRE_AND_HOLD" :
                        option.target != INVALID_TILE && option.target == partner.target ? "COMBINED_FIRE" : "CAPTURE_AND_COVER";
                }

                if (trial.companion>=0)
                {
                    rec["companionUnit"]=problem.actors()[trial.companion].engineUnitId;
                    rec["companionCandidate"]=trial.companionOption;
                }
                rec["destination"] = tile(option.destination);
                rec["target"] = tile(option.target);
                if(forPairedPrice && evaluated.valued())
                {
                    // The next step prices both actual squads. The opening
                    // estimate is neither a verdict nor another paid trial.
                    rec["status"]="RACE_UNPRICED";
                    rec["priceStatus"]="UNRESOLVED";
                    rec.remove("policyTotal");++unpriced;
                    unfinishedOpenings.emplace(static_cast<int>(trials.size()),evaluated.replay.ordering.order);
                    pendingPair=PendingPair{trial,std::move(evaluated),choices,order,0,static_cast<int>(trials.size())};
                    trials.append(rec);
                    return;
                }
                if (evaluated.valued())
                {
                    choices[trial.slot] = trial.option;
                    const auto priorRecord = winningRecord < 0 ? base : trials[winningRecord].toObject();
                    const auto priorReplies = replyRanks(priorRecord.value("replyAlternatives").toArray());
                    const auto candidateRecord=rec;
                    const auto beforePrice=budget;
                    const bool certifiedIncumbent=best && priorRecord.value("priceStatus")=="WITNESSED";
                    auto value=worstReply(choices,evaluated,rec,certifiedIncumbent ? &*best : nullptr,order,&priorReplies,&priorRecord);
                    remember(rec);
                    auto comparisonRank=best;
                    auto comparableReplies=priorReplies;
                    bool comparable=value && certifiedIncumbent && rec.value("priceStatus")=="WITNESSED";
                    auto missing=additionalTownChallenges(priorRecord,rec);
                    auto missingStock=additionalStockChallenges(priorRecord,rec);
                    auto postQuestions=pairedFundedPosts(priorRecord,rec);
                    bool newPostQuestions=!postQuestions.isEmpty();
                    comparable &= !newPostQuestions;
                    auto pairedFrontier=nominateTownFrontier(priorRecord,rec,&budget,TownPriorityScope::ObservedPolicies);
                    // A bounded rejection needs only one complete adverse reply. An unbounded
                    // challenger must face the incumbent's discoveries on its own board.
                    if (value && !rec.contains("boundedByReply") && (!missing.empty() || !missingStock.empty() || pairedFrontier || newPostQuestions))
                    {
                        const auto elapsed=beforePrice-budget;
                        const auto available=std::min(budget,2*(referencePriceWork+elapsed));
                        auto saved=budget-available;
                        budget=available;
                        const auto restore=qScopeGuard([&] {budget+=saved;routeCache.requiredTownChallenges.clear();routeCache.matchedStockChallenges.clear();routeCache.decisionFrontier.reset();routeCache.fundedPostQuestions={};});
                        routeCache.fundedPostQuestions=std::move(postQuestions);
                        routeCache.decisionFrontier=pairedFrontier;
                        std::set<TownIntention> required;
                        QJsonArray revisions;
                        comparable=false;
                        while ((!missing.empty() || !missingStock.empty() || pairedFrontier || newPostQuestions) && budget>=minimumReplayBudget)
                        {
                            mergeStockChallenges(routeCache.matchedStockChallenges,missingStock);
                            required.insert(missing.begin(),missing.end());
                            routeCache.requiredTownChallenges.clear();
                            for (const auto &i:required)
                                routeCache.requiredTownChallenges.push_back({{i[0],i[1]},i[2],i[3],i[4]});
                            auto a=winner ? forecastRecord(*winner,watched) : sourcePriceRecord;
                            auto b=candidateRecord;
                            const auto beforeScreenPair=budget;
                            const auto perArm=beforeScreenPair/2;
                            const auto anchorPrice=worstReply(winner?winningChoices:effectiveChoices,
                                winner?*winner:reference,a,nullptr,winningOrder,nullptr,nullptr,perArm);
                            const auto afterFirst=budget;
                            const auto challengerPrice=worstReply(choices,evaluated,b,nullptr,order,nullptr,nullptr,perArm);
                            if(activateScreen(false,2*std::max(beforeScreenPair-afterFirst,afterFirst-budget),&saved))continue;
                            pairedFrontier.reset();
                            if(routeCache.decisionFrontier && routeCache.decisionFrontier->property())
                                if(auto due=bindTownAdmission(*routeCache.decisionFrontier,a,b))
                                {
                                    revisions.append(townFrontierPair(*routeCache.decisionFrontier,a,b));pairedFrontier=due;
                                    routeCache.decisionFrontier=std::move(due);
                                }
                            // One focused offer per paired scope. Subsequent existing rounds may resume it.
                            if(routeCache.decisionFrontier)rec["pairedTownFrontier"]=townFrontierPair(*routeCache.decisionFrontier,a,b);
                            QJsonArray requests;
                            for (const auto &i:required) requests.append(describeTownIntention(i));
                            revisions.append(QJsonObject{{"required",requests},{"matchedStock",describeStockChallenges(routeCache.matchedStockChallenges)},
                                {"incumbent",a.value("priceStatus")},
                                {"challenger",b.value("priceStatus")},{"incumbentReply",a.value("replyPolicy")},
                                {"challengerReply",b.value("replyPolicy")}});
                            newPostQuestions=mergeFundedPostQuestions(routeCache.fundedPostQuestions,pairedFundedPosts(a,b));
                            const auto openPosts=fundedPostObligations(routeCache.fundedPostQuestions,a,b);
                            b["unansweredFundedPostChecks"]=openPosts;
                            rec["unansweredFundedPostChecks"]=openPosts;
                            if (!anchorPrice || !challengerPrice) break;
                            remember(a);remember(b);
                            comparisonRank=anchorPrice;
                            value=challengerPrice;
                            comparableReplies=replyRanks(a.value("replyAlternatives").toArray());
                            missing=additionalTownChallenges(a,b,required);
                            missingStock=additionalStockChallenges(a,b,routeCache.matchedStockChallenges);
                            comparable=openPosts.isEmpty() && missing.empty() && missingStock.empty() && stockComparisonCovered(a,b,routeCache.matchedStockChallenges) &&
                                a.value("priceStatus")=="WITNESSED" && b.value("priceStatus")=="WITNESSED";
                            if(routeCache.decisionFrontier)b["pairedTownFrontier"]=townFrontierPair(*routeCache.decisionFrontier,a,b);
                            rec=std::move(b);
                        }
                        rec["sharedTownComparisons"]=std::move(revisions);
                    }
                    if (value && !rec.contains("boundedByReply") && !comparable)
                    {
                        rec["comparisonStatus"]="UNRESOLVED";
                        recordEconomicEvidence(rec,false);
                        value.reset();
                    }
                    QJsonArray improved;
                    const bool dominates = value && comparisonRank && *value == *comparisonRank && !rec.contains("boundedByReply") &&
                        improvesMatchedReplies(rec.value("replyAlternatives").toArray(),comparableReplies,true,&improved);
                    if (!value)
                    {
                        // The legal opening can settle a directional comparison
                        // even if its missing financial input has no town price.
                        if (choices!=effectiveChoices || order.first!=NO_UNIT || order.second!=NO_UNIT)
                            unpricedPhysicalOrders.push_back({trial,order,entry->first});
                        unfinishedOpenings.emplace(static_cast<int>(trials.size()),evaluated.replay.ordering.order);
                        rec["status"] = "RACE_UNPRICED";
                        rec.remove("policyTotal");
                    // Order unfinished squads by the same per-order bundle
                    // estimates that seeded the search. Only the later certified
                    // comparison can publish the squad.
                    if(!winner && (!best || (rec["priceStatus"]=="UNRESOLVED" && rec["townWorkLimitedReply"].toBool()) ||
                        (base["physicalDecisionBoundary"]=="LOST_ON_FIRST_REPLY" &&
                         rec["physicalDecisionBoundary"]!="LOST_ON_FIRST_REPLY")) &&
                       choices!=effectiveChoices)
                    {
                        MilliFunds opening=0;bool valid=true;
                        for(int k=0;k<static_cast<int>(choices.size());++k)
                        {
                            if(choices[k]==effectiveChoices[k])continue;
                            const auto &menu=problem.actors()[k].candidates;
                            valid=RelativeStockDetail::checkedAdd(opening,menu[choices[k]].immediateValue.economicValue) &&
                                RelativeStockDetail::checkedSubtract(opening,menu[effectiveChoices[k]].immediateValue.economicValue);
                            if(!valid)break;
                        }
                        if(valid && (!pendingPair || opening>pendingPair->opening))
                            pendingPair=PendingPair{trial,evaluated,choices,order,opening,static_cast<int>(trials.size())};
                    }
                    }
                    else if (!rec.contains("boundedByReply") && comparisonRank && (*value > *comparisonRank || dominates))
                    {
                        rec["selectionBasis"] = dominates ? "SHARED_REPLY_DOMINANCE" : "HIGHER_WORST_REPLY";
                        rec["comparedToTrial"] = winningRecord;
                        if (dominates) rec["improvedReplyPolicies"] = improved;
                        if (trial.companion<0) measuredImprovements.insert_or_assign(trialChoices(trial),*value);
                        best = value;
                        winner = std::move(evaluated);
                        winningTrial = trial;
                        winningOrder = order;
                        winningChoices = choices;
                        winningRecord = trials.size();
                        winningReplyKey = rec["responseKey"].toArray();
                    }
                    if (value && !rec.contains("boundedByReply"))
                        pricedSequences.emplace(std::pair{choices, (winningRecord == trials.size() ? winner->replay.ordering.order :
                                                                   evaluated.replay.ordering.order)}, trials.size());
                    if (rec.contains("boundedByReply"))
                    {
                        ++bounded;
                        rec["status"] = "DOMINATED_BY_REPLY";
                        rec["policyUpperBound"]=rec.value("worstReplyUpperBound").toArray().at(3);
                        rec.remove("policyTotal");
                    }
                    if (value && !rec.contains("boundedByReply") && trial.partner < 0)
                        estimates[{trial.slot, trial.option}] = *value;
                    if (!value)
                        ++unpriced;
                }
                choices = effectiveChoices;
                trials.append(rec);
            };
            // First measure standalone moves. Then carry the best entire team plan into
            // the next neighborhood; re-evaluate other orders, even those that lost alone.
            // Previously only finish-in-place orders could be added to a separate move.
            // Use local economic deltas only to schedule full replays. The winner is
            // selected exclusively by the full team score against all applicable replies.
            auto localEstimate = [&](const Trial &t, const std::vector<int> &from) {
                MilliFunds delta = 0;
                auto add = [&](int slot, int choice) {
                    const auto &a = problem.actors()[slot];
                    return RelativeStockDetail::checkedAdd(delta, a.candidates[choice].immediateValue.economicValue) &&
                           RelativeStockDetail::checkedSubtract(delta,
                                                                a.candidates[from[slot]].immediateValue.economicValue);
                };
                return add(t.slot, t.option) && (t.partner < 0 || add(t.partner, t.partnerOption)) &&
                       (t.companion<0 || add(t.companion,t.companionOption))
                           ? std::optional<MilliFunds>{delta}
                           : std::nullopt;
            };
            // Within each urgency tier, give each actor a first look ahead of a second
            // look at another actor. Keep the existing joint tactical probes in the mix.
            std::map<std::pair<int, int>, int> seen;
            std::map<std::tuple<int, int, int, int>, int> actorOrder;
            auto id = [](const Trial &t) { return std::tuple{t.slot, t.option, t.partner, t.partnerOption}; };
            std::stable_sort(shortlist.begin(), shortlist.end(), [&](const Trial &a, const Trial &b) {
                if (a.priority != b.priority)
                    return a.priority < b.priority;
                return localEstimate(a, anchor) > localEstimate(b, anchor);
            });
            for (const auto &t : shortlist)
                actorOrder[id(t)] = seen[{t.priority, t.slot}]++;
            std::stable_sort(shortlist.begin(), shortlist.end(), [&](const Trial &a, const Trial &b) {
                const auto turn = [&](const Trial &t) {
                    return std::tuple{t.priority,t.interruptionRank < 0,
                        t.interruptionRank >= 0 ? t.interruptionRank : actorOrder.at(id(t))};
                };
                return turn(a) < turn(b);
            });
            // Even when the incoming shots stay exactly the same, their order can change the
            // counters, casualties and whether the second shot remains legal.
            for (const auto &actor : reference.actors)
                if (actor.ownShot.target != NO_UNIT && std::any_of(reference.actors.begin(), reference.actors.end(),
                    [&](const auto &other) { return other.actorSlot != actor.actorSlot && other.ownShot.target == actor.ownShot.target; }))
                    shortlist.push_back({actor.actorSlot,actor.candidateIndex,-2});
            const auto originalCandidates = shortlist;
            std::set<int> actors;
            for (const auto &t : shortlist)
            {
                actors.insert(t.slot);
                if (t.partner >= 0)
                    actors.insert(t.partner);
            }
            std::vector<std::pair<int,int>> readyFinishes;
            for (const auto &actor : problem.actors())
            {
                if (!problem.canChangeActor(actor.canonicalSlot) || knowledge.units()[actor.knowledgeUnitIndex].capturePoints <= 0)
                    continue;
                for (int j = 0; j < static_cast<int>(actor.candidates.size()); ++j)
                {
                    const auto &option = actor.candidates[j];
                    const auto *part = singleComponentOf(option.source.bundle);
                    if (option.plannable && option.destination == option.origin && part && part->kind == ComponentKind::Capture &&
                        part->capture.turnsUntilOwned == 0)
                        readyFinishes.emplace_back(actor.canonicalSlot,j);
                }
            }
            // The tactical neighborhood still runs first, including witnessed lethal rescues.
            // Hold one fraction of the remaining allowance to compare a lost payday on the
            // published team rather than only on a displaced, earlier assignment.
            const auto finishReserve = (readyFinishes.empty() && dueCaptureOptions.empty()) || !economicSearchOpen()
                ? std::int64_t{0} : std::min(budget/2,(beforeReference-budget)+(beforeReference-budget)/4);
            // One isolated reference price only buys one complete challenging team.
            // A changed team invalidates earlier bounds on its individual exits. Allow
            // for a complete comparison per idle actor and keep most work for team search.
            const auto closingReserve=closingActors && economicSearchOpen()
                ? std::min(budget/3,(beforeReference-budget)*(std::min(closingActors,6)+1)) : std::int64_t{0};
            // Share the reserve with immediate finishes; both phases compare an entire
            // team and use the same turn budget and the same opponent-reply evaluator.
            const auto finalReserve=std::max(finishReserve,closingReserve);
            budget -= finalReserve;
            std::set<PlanKey> deferredPlans;
            std::vector<DeferredClosingOrder> deferredOnConstructor;
            QJsonArray rounds;
            for (;; ++round)
            {
                const int start = trials.size();
                const auto levelStart = budget;
                // Preserve part of the real work budget for coordination, without making
                // a partially evaluated reply look like a loss at the phase boundary.
                const auto nextPass = actors.size() > 1 && round == 0 ? budget / 3 : 0;
                if (round > 0)
                {
                    shortlist = originalCandidates;
                    auto fresh = combatTrials(problem, anchor, *winner, knowledge, true, hq, potentialCappers, watched,
                                              trials[winningRecord].toObject()["replyAlternatives"].toArray(),terrain.get());
                    std::set<decltype(id(Trial{}))> currentRescues;
                    for (const auto &t : fresh)
                        if (t.priority <= -7) currentRescues.insert(id(t));
                    for (auto &t : shortlist)
                        if (t.priority <= -7 && !currentRescues.contains(id(t))) t.priority = 0;
                    std::set<decltype(id(Trial{}))> available;
                    for (const auto &t : shortlist)
                        available.insert(id(t));
                    for (const auto &t : fresh)
                        if (available.insert(id(t)).second)
                            shortlist.push_back(t);
                    for (const auto &a : problem.actors())
                        if (anchor[a.canonicalSlot] != effectiveChoices[a.canonicalSlot])
                            shortlist.push_back({a.canonicalSlot, effectiveChoices[a.canonicalSlot], 0});
                    std::stable_sort(shortlist.begin(), shortlist.end(), [&](const Trial &a, const Trial &b) {
                        // Carry an immediate finish or HQ response onto the new team plan even when its
                        // standalone score was bounded by the previous guard position.
                        const auto previousA = measuredImprovements.find(trialChoices(a));
                        const auto previousB = measuredImprovements.find(trialChoices(b));
                        if ((previousA != measuredImprovements.end()) != (previousB != measuredImprovements.end()))
                            return previousA != measuredImprovements.end();
                        if (previousA != measuredImprovements.end() && previousA->second != previousB->second)
                            return previousA->second > previousB->second;
                        if ((a.priority <= -7) != (b.priority <= -7)) return a.priority <= -7;
                        if ((a.priority == -6) != (b.priority == -6)) return a.priority == -6;
                        if ((a.priority <= -3) != (b.priority <= -3))
                            return a.priority <= -3;
                        const auto ia = estimates.find({a.slot, a.option}), ib = estimates.find({b.slot, b.option});
                        const bool ap = a.partner < 0 && ia != estimates.end(),
                                   bp = b.partner < 0 && ib != estimates.end();
                        if (ap != bp)
                            return ap;
                        if (ap && ia->second != ib->second)
                            return ia->second > ib->second;
                        return localEstimate(a, anchor) > localEstimate(b, anchor);
                    });
                    // Two individually improving moves may compete for one square. Retrying
                    // the displaced contender alone can only return a collision. Reassign
                    // its blocker to the best compatible, already measured alternative and
                    // reprice both with this exact team before trying the known collision.
                    std::map<int,std::vector<std::pair<CombatRank,int>>> proven;
                    for (const auto &[move,rank]:measuredImprovements)
                        if (std::get<2>(move)<0) proven[std::get<0>(move)].emplace_back(rank,std::get<1>(move));
                    for (auto &[slot,prior]:proven)
                        std::stable_sort(prior.begin(),prior.end(),[](const auto &a,const auto &b){return a.first>b.first;});
                    std::map<Cell,int> endpoints;
                    for (const auto &a:problem.actors())
                        if (anchor[a.canonicalSlot]>=0)
                        {
                            const auto at=a.candidates[anchor[a.canonicalSlot]].destination;
                            endpoints[{at.x,at.y}]=a.canonicalSlot;
                        }
                    std::vector<Trial> coordinated;
                    std::set<TrialChoices> inserted;
                    for (const auto &t:shortlist)
                    {
                        const auto &actor=problem.actors()[t.slot];
                        const auto at=actor.candidates[t.option].destination;
                        const auto occupied=endpoints.find({at.x,at.y});
                        if (budget>=minimumReplayBudget && t.partner<0 && anchor[t.slot]!=t.option &&
                            measuredImprovements.contains(trialChoices(t)) && occupied!=endpoints.end() && occupied->second!=t.slot)
                        {
                            const auto &helper=problem.actors()[occupied->second];
                            const int other=helper.canonicalSlot;
                            if (measuredImprovements.contains(TrialChoices{other,anchor[other],-1,-1}) &&
                                (problem.canChangeActor(other) || problem.canChangeCombatHelper(helper)))
                            {
                                auto partnerOption=[&](int i) {
                                    if (i<0 || i==anchor[other] || !helper.candidates[i].plannable) return false;
                                    const auto end=helper.candidates[i].destination;
                                    const auto block=endpoints.find({end.x,end.y});
                                    return end!=at && (block==endpoints.end() || block->second==other || block->second==t.slot);
                                };
                                int choice=effectiveChoices[other];
                                for (const auto &[ranked,i]:proven[other])
                                    if (partnerOption(i)) {choice=i; break;}
                                if (partnerOption(choice))
                                {
                                    auto joint=t;
                                    joint.partner=other;
                                    joint.partnerOption=choice;
                                    joint.competingAssignments=true;
                                    if (inserted.insert(trialChoices(joint)).second) coordinated.push_back(joint);
                                }
                                budget-=std::min<std::int64_t>(budget,1+proven[other].size());
                            }
                        }
                        coordinated.push_back(t);
                    }
                    shortlist=std::move(coordinated);
                }
                QJsonArray deferred;
                const auto anchorChanges = winningRecord;
                bool carryFinish = false;
                int carriedReassignment = -1;
                for (const auto &t : shortlist)
                {
                    auto proposed = anchor;
                    proposed[t.slot] = t.option;
                    if (t.partner >= 0)
                        proposed[t.partner] = t.partnerOption;
                    if (t.companion >= 0)
                        proposed[t.companion] = t.companionOption;
                    std::vector<CombatFireOrder> orders{{}};
                    auto admitOrder = [&](CombatFireOrder order) {
                        if (std::find(orders.begin(), orders.end(), order) == orders.end())
                            orders.push_back(order);
                    };
                    for (int changed : {t.slot, t.partner, t.companion})
                    {
                        if (changed < 0)
                            continue;
                        const auto &actor = problem.actors()[changed];
                        const auto &shot = actor.candidates[proposed[changed]];
                        if (shot.targetUnitId == NO_UNIT)
                            continue;
                        for (const auto &other : problem.actors())
                        {
                            const auto index = proposed[other.canonicalSlot];
                            if (other.canonicalSlot == changed || index < 0 ||
                                other.candidates[index].targetUnitId != shot.targetUnitId)
                                continue;
                            admitOrder({actor.engineUnitId,other.engineUnitId});
                            admitOrder({other.engineUnitId,actor.engineUnitId});
                        }
                    }
                    if (anchorOrder.first != NO_UNIT)
                    {
                        int target = NO_UNIT, matching = 0;
                        for (const auto &actor : problem.actors())
                            if ((actor.engineUnitId == anchorOrder.first || actor.engineUnitId == anchorOrder.second) &&
                                proposed[actor.canonicalSlot] >= 0)
                            {
                                const int enemy = actor.candidates[proposed[actor.canonicalSlot]].targetUnitId;
                                if (enemy != NO_UNIT && (matching == 0 || target == enemy))
                                { target = enemy; ++matching; }
                            }
                        if (matching == 2)
                            admitOrder(anchorOrder);
                    }
                    for (auto order : orders)
                    {
                        const PlanKey key{proposed,order.first,order.second};
                        if (attempted.contains(key))
                            continue;
                        // A successful reassignment changes which destinations are free.
                        // Defer stale ordinary probes, preserving urgent rescues and finishes;
                        // the next round reconstructs candidates on the actual winning team.
                        if (carryFinish || (carriedReassignment>=0 && t.priority>-6) ||
                            budget < minimumReplayBudget || (budget < nextPass && winner))
                        {
                            if (deferredPlans.insert(key).second && anchor==effectiveChoices)
                                deferredOnConstructor.push_back({t,order,key});
                            deferred.append(QJsonArray{problem.actors()[t.slot].engineUnitId, t.option,
                                t.partner >= 0 ? problem.actors()[t.partner].engineUnitId : NO_UNIT, t.partnerOption,
                                order.first, order.second});
                            continue;
                        }
                        const auto rankBefore=best;
                        evaluateTrial(t,order);
                        if (t.competingAssignments && best && rankBefore && *best>*rankBefore &&
                            std::get<0>(*best)==TerminalClass::Unresolved && std::get<1>(*best)>=0 &&
                            std::get<2>(*best)>=0 && hq.standing==NO_UNIT && hq.approaching.empty())
                            carriedReassignment=winningRecord;
                        // Carry a fully priced finish and the teammate which makes it legal
                        // into the remaining comparisons. Otherwise a later single-unit win
                        // can displace both, with too little budget left to compare all three.
                        if (round == 0 && winner && (winningTrial.priority == -6 ||
                            (winningTrial.priority <= -7 && !readyFinishes.empty())))
                            carryFinish = true;
                    }
                }
                rounds.append(QJsonObject{{"round", round},
                                          {"attempts", trials.size() - start}, {"carriedImmediateFinish",carryFinish},
                                          {"anchorTrial", anchorChanges},{"carriedReassignmentTrial",carriedReassignment},
                                          {"deferredOrders", deferred},
                                          {"work", static_cast<qint64>(levelStart - budget)},
                                          {"incumbentTrial", winningRecord}});

                if (!winner || (winningChoices == anchor && winningOrder == anchorOrder) || budget < minimumReplayBudget)
                    break;
                anchor = winningChoices;
                anchorOrder = winningOrder;
            }
            budget += finalReserve;
            QJsonArray finishChecks;
            if (finishReserve > 0)
            {
                ++round;
                const int start = trials.size();
                const auto prior = budget;
                auto endChecks=readyFinishes;
                endChecks.insert(endChecks.end(),dueCaptureOptions.begin(),dueCaptureOptions.end());
                for (const auto &[slot, option] : endChecks)
                {
                    anchor = winner ? winningChoices : effectiveChoices;
                    if (anchor[slot] == option || budget < minimumReplayBudget) continue;
                    const bool due=std::find(dueCaptureOptions.begin(),dueCaptureOptions.end(),std::pair{slot,option})!=dueCaptureOptions.end();
                    const auto end = problem.actors()[slot].candidates[option].destination;
                    int occupant = -1;
                    for (const auto &actor : problem.actors())
                        if (actor.canonicalSlot != slot && anchor[actor.canonicalSlot] >= 0 &&
                            actor.candidates[anchor[actor.canonicalSlot]].destination == end) occupant = actor.canonicalSlot;
                    const int first = trials.size();
                    auto order = winningOrder;
                    const auto completingUnit = problem.actors()[slot].engineUnitId;
                    if (completingUnit == order.first || completingUnit == order.second) order = {};
                    if (occupant < 0)
                        evaluateTrial({slot,option,-6},order);
                    else if (problem.canChangeActor(occupant) || problem.canChangeCombatHelper(problem.actors()[occupant]))
                    {
                        const auto &other = problem.actors()[occupant];
                        if (other.engineUnitId == order.first || other.engineUnitId == order.second) order = {};
                        for (int move=0; move<static_cast<int>(other.candidates.size()) && budget>=minimumReplayBudget; ++move)
                        {
                            const auto &c = other.candidates[move];
                            if (!c.plannable || c.destination==end || (!due && c.destination!=c.origin)) continue;
                            if (due && std::any_of(problem.actors().begin(),problem.actors().end(),[&](const auto &a) {
                                return a.canonicalSlot!=occupant && a.canonicalSlot!=slot && anchor[a.canonicalSlot]>=0 &&
                                       a.candidates[anchor[a.canonicalSlot]].destination==c.destination;
                            })) continue;
                            evaluateTrial({slot,option,-6,occupant,move,true},order);
                            if (due) break;
                        }
                    }
                    finishChecks.append(QJsonObject{{"unit",completingUnit},{"dueStart",due},{"firstTrial",first},{"newTrials",trials.size()-first}});
                }
                rounds.append(QJsonObject{{"round",round},{"finalCompletions",finishChecks},{"attempts",trials.size()-start},
                    {"work",static_cast<qint64>(prior-budget)},{"incumbentTrial",winningRecord}});
            }
            result["completionReserve"] = static_cast<qint64>(finishReserve);
            result["finalTeamReserve"] = static_cast<qint64>(finalReserve);
            QJsonArray closingRounds;
            std::set<PlanKey> retried;
            bool closingComplete=false;
            const auto closingOpening=winner ? winningChoices : effectiveChoices;
            std::map<std::pair<int,int>,int> closingAttempts;
            std::vector<DeferredClosingOrder> unvisitedClosing,unfinishedClosing;
            std::set<PlanKey> savedClosingOrders;
            while (priceOpponent && budget>=minimumReplayBudget && economicSearchOpen())
            {
                ++round;
                const int anchorTrial=winningRecord;
                anchor=winner ? winningChoices : effectiveChoices;
                anchorOrder=winningOrder;
                const auto prior=budget;
                bool complete=true;
                const auto anchorReport=anchorTrial<0 ? base : trials[anchorTrial].toObject();
                QJsonArray routes;
                auto probes=closingProbes(problem,knowledge,*terrain,anchor,closingOpening,anchorReport["captureClaims"].toArray(),
                    affordableFactories,dueFactoryExits,budget,routes,complete);
                // Losing a comparison on a former squad proves nothing about the same
                // assignment after its teammates move. Revisit previously priced orders,
                // including an earlier attack or a useful staging square of a unit already
                // moving. If the destination was taken by a teammate, price both changes.
                std::set<TrialChoices> queued;
                for (const auto &p : probes) queued.insert(trialChoices(p.trial));
                std::vector<std::set<int>> earlierOptions(anchor.size());
                for (const auto &[order,rank] : estimates) earlierOptions[order.first].insert(order.second);
                std::map<Cell,int> heldEnds;
                for (const auto &a : problem.actors())
                {
                    const int slot=a.canonicalSlot;
                    if (effectiveChoices[slot]>=0) earlierOptions[slot].insert(effectiveChoices[slot]);
                    if (anchor[slot]<0) continue;
                    const auto at=a.candidates[anchor[slot]].destination;
                    heldEnds[{at.x,at.y}]=slot;
                }
                const auto appendRevision = [&](Trial t) {
                    if (!queued.insert(trialChoices(t)).second) return;
                    probes.push_back({t,3,"REPRICE_TEAMMATE_ORDER"});
                    probes.back().teammateRevision=true;
                };
                for (const auto &a : problem.actors())
                {
                    const int slot=a.canonicalSlot;
                    if (anchor[slot]<0 || (!problem.canChangeActor(slot) && !problem.canChangeCombatHelper(a))) continue;
                    for (const int index : earlierOptions[slot])
                    {
                        if (budget<=0) {complete=false;break;}
                        --budget;
                        const auto &c=a.candidates[index];
                        if (index==anchor[slot] || !c.plannable || c.immediateValue.terminal<a.candidates[anchor[slot]].immediateValue.terminal) continue;
                        const auto lock=heldEnds.find({c.destination.x,c.destination.y});
                        if (lock==heldEnds.end() || lock->second==slot) {appendRevision({slot,index,0});continue;}
                        const int other=lock->second;
                        const auto &peer=problem.actors()[other];
                        if (!problem.canChangeActor(other) && !problem.canChangeCombatHelper(peer)) continue;
                        for (const int replacement:earlierOptions[other])
                        {
                            if (budget<=0) {complete=false;break;}
                            --budget;
                            if (replacement==anchor[other]) continue;
                            const auto &alternate=peer.candidates[replacement];
                            if (!alternate.plannable || alternate.destination==c.destination ||
                                alternate.immediateValue.terminal<peer.candidates[anchor[other]].immediateValue.terminal) continue;
                            const auto end=heldEnds.find({alternate.destination.x,alternate.destination.y});
                            if (end!=heldEnds.end() && end->second!=other && end->second!=slot) continue;
                            Trial exchange{slot,index,0,other,replacement};
                            exchange.competingAssignments=true;
                            appendRevision(exchange);
                        }
                    }
                }
                // A displacement can restore a teammate's shot; another unit can
                // leave a funded site on that same turn. Losing either isolated
                // comparison is no bound on the joint plan. Require disjoint final
                // squares and send every nominated plan through the ordinary evaluator.
                const auto originalProbeCount=probes.size();
                std::vector<Trial> releases;
                for (const auto &probe:probes)
                {
                    const auto &t=probe.trial;
                    if (t.partner>=0 || probe.tier>=3) continue;
                    const auto &a=problem.actors()[t.slot];
                    const auto at=a.candidates[anchor[t.slot]].destination, to=a.candidates[t.option].destination;
                    if (affordableFactories.contains({at.x,at.y}) && !affordableFactories.contains({to.x,to.y})) releases.push_back(t);
                }
                for (std::size_t i=0;i<originalProbeCount;++i)
                {
                    const auto support=probes[i].trial;
                    if (!probes[i].teammateRevision) continue;
                    const auto &move=problem.actors()[support.slot].candidates[support.option];
                    if (support.partner<0 && move.targetUnitId==NO_UNIT && !move.capture) continue;
                    for (const auto &exit:releases)
                    {
                        if (budget<=0) {complete=false;break;}
                        --budget;
                        if (exit.slot==support.slot || exit.slot==support.partner) continue;
                        const auto end=problem.actors()[exit.slot].candidates[exit.option].destination;
                        if (end==move.destination || (support.partner>=0 &&
                            end==problem.actors()[support.partner].candidates[support.partnerOption].destination)) continue;
                        auto team=support;
                        if (team.partner<0) {team.partner=exit.slot;team.partnerOption=exit.option;}
                        else {team.companion=exit.slot;team.companionOption=exit.option;}
                        probes.push_back({team,3,"COORDINATED_REASSIGNMENT"});
                        probes.back().coordinatedRelease=true;
                    }
                }
                if (probes.empty() && complete) { closingComplete=true; break; }
                std::int64_t levels=1;
                for (auto width=probes.size(); width>1; width/=2) ++levels;
                const auto scheduleWork=static_cast<std::int64_t>(probes.size())*(2*anchor.size()+3*levels+1+purchaseWindow.sitesByCost.size());
                if (scheduleWork>static_cast<std::uint64_t>(budget))
                {
                    closingComplete=false;
                    closingRounds.append(QJsonObject{{"round",round},{"anchorTrial",anchorTrial},{"selectedTrial",winningRecord},
                        {"catalogComplete",false},{"newTrials",0},{"units",QJsonArray{}},{"bookedRoutes",routes},
                        {"unresolvedWorklist",static_cast<qint64>(probes.size())},{"work",static_cast<qint64>(prior-budget)}});
                    break;
                }
                budget-=static_cast<std::int64_t>(scheduleWork);
                auto exactKey=[&](const Trial &t) {
                    auto proposed=anchor;
                    proposed[t.slot]=t.option;
                    if (t.partner>=0) proposed[t.partner]=t.partnerOption;
                    if (t.companion>=0) proposed[t.companion]=t.companionOption;
                    // Preserve the actual team fire order unless an altered shooter no
                    // longer fires at that common target. The replay validates either order.
                    auto order=anchorOrder;
                    if (order.first!=NO_UNIT)
                    {
                        int target=NO_UNIT;
                        for (const auto &a:problem.actors())
                            if (a.engineUnitId==order.first || a.engineUnitId==order.second)
                            {
                                if (proposed[a.canonicalSlot]<0) { order={}; break; }
                                const int shot= a.candidates[proposed[a.canonicalSlot]].targetUnitId;
                                if (shot==NO_UNIT || (target!=NO_UNIT && target!=shot)) { order={}; break; }
                                target=shot;
                            }
                    }
                    return PlanKey{std::move(proposed),order.first,order.second};
                };
                // Independent opportunities share the allowance. A dominated combination
                // on an old team may guide scheduling, but only an exact choice/order key
                // can remove a comparison. Known keys consume no position in the queue.
                const auto &priorTeam=winner ? *winner : reference;
                const auto boundary=purchaseBoundary(problem,*terrain,priorTeam,purchaseWindow.unitSlots);
                const auto priorCapacity=purchaseWindow.affordable([&](Cell site){return boundary.occupied.contains(site);},boundary.available);
                std::map<PlanKey,ReplyPassEvaluation> openingPrices;
                const auto lookaheadWork=budget/4;
                const bool scoutJoint=std::any_of(probes.begin(),probes.end(),[](const auto &p) {return p.coordinatedRelease;});
                auto coordinatedLookahead=scoutJoint ? lookaheadWork/3 : 0;
                auto individualLookahead=lookaheadWork-coordinatedLookahead;
                for (auto &probe:probes)
                {
                    const auto &t=probe.trial;
                    const auto key=exactKey(t);
                    const auto entry=attempted.find(key);
                    probe.settled=entry!=attempted.end() && (entry->second<0 ||
                        trials[entry->second].toObject()["status"]!="RACE_UNPRICED");
                    const auto &c=problem.actors()[t.slot].candidates[t.option];
                    probe.job=probe.coordinatedRelease ? 4 : probe.teammateRevision ? 3 : probe.tier<3 ? 0 : (c.targetUnitId!=NO_UNIT || improvesNextShot(c) ? 1 : 2);
                    if (probe.job==0)
                    {
                        // Keep all other endpoints, including a teammate that refills a site.
                        // This is an ordering estimate only; the replay above measures deaths.
                        auto occupied=[&](Cell site) {
                            const auto isAt=[&](int slot,int index) {
                                const auto at=problem.actors()[slot].candidates[index].destination;
                                return Cell{at.x,at.y}==site;
                            };
                            if (isAt(t.slot,t.option) || (t.partner>=0 && isAt(t.partner,t.partnerOption))) return true;
                            if (isAt(t.slot,anchor[t.slot]) || (t.partner>=0 && isAt(t.partner,anchor[t.partner]))) return false;
                            return boundary.occupied.contains(site);
                        };
                        probe.capacityChange=purchaseWindow.affordable(occupied,boundary.available)-priorCapacity;
                        if (probe.capacityChange==0 && std::string_view(probe.opportunity)=="FUNDED_FACTORY_EXIT")
                            probe.opportunity="CHANGE_PRODUCTION_SITE_WITHOUT_EXTRA_SLOT";
                    }
                    probe.localValue=localEstimate(t,anchor);
                    // On a new team a future shot can exist only after the enemy moves.
                    // Replay the opening on this exact squad before scheduling full prices.
                    // These values order work only. Selection still requires every opponent reply.
                    auto &lookaheadLeft=probe.coordinatedRelease ? coordinatedLookahead : individualLookahead;
                    if (!probe.settled && (t.partner<0 || probe.coordinatedRelease) && lookaheadLeft>=replayTariff)
                    {
                        auto prepared=openingPrices.find(key);
                        if (prepared==openingPrices.end())
                        {
                            const auto before=lookaheadLeft;
                            lookaheadLeft-=replayTariff;
                            const auto oldWork=terrain->workLeft;
                            terrain->workLeft=&lookaheadLeft;
                            const auto restore=qScopeGuard([&] {terrain->workLeft=oldWork;});
                            auto replay=evaluateReplyPassPlan(problem,std::get<0>(key),board,
                                CombatFireOrder{std::get<1>(key),std::get<2>(key)});
                            budget-=before-lookaheadLeft;
                            if (replay.valued()) prepared=openingPrices.emplace(key,std::move(replay)).first;
                        }
                        if (prepared!=openingPrices.end())
                        {
                            auto gain=prepared->second.exactNonStock;
                            if (RelativeStockDetail::checkedSubtract(gain,priorTeam.exactNonStock)) probe.openingReplyGain=gain;
                        }
                    }
                    const auto old=estimates.find({t.slot,t.option});
                    if (t.partner<0 && old!=estimates.end()) probe.previousRank=old->second;
                }
                auto quality=[](const ClosingProbe &a,const ClosingProbe &b) {
                    // Compare a genuine extra purchase opportunity before a nominal
                    // release that merely exchanges two sites. Only the full rank can win.
                    if (a.job==0 && b.job==0 && a.capacityChange!=b.capacityChange)
                        return a.capacityChange>b.capacityChange;
                    if ((a.trial.partner<0)!=(b.trial.partner<0)) return a.trial.partner<0;
                    // The local shot/capture spans measure the actual action. Movement
                    // off a factory can have zero local value at every square, so use
                    // its previously tested whole-team ranking to order equally valued exits.
                    if (a.openingReplyGain.value_or(0)!=b.openingReplyGain.value_or(0))
                        return a.openingReplyGain.value_or(0)>b.openingReplyGain.value_or(0);
                    if (a.localValue!=b.localValue) return a.localValue>b.localValue;
                    if (a.previousRank!=b.previousRank) return a.previousRank>b.previousRank;
                    return a.tier<b.tier;
                };
                std::stable_sort(probes.begin(),probes.end(),[&](const auto &a,const auto &b) {
                    if (a.job!=b.job) return a.job<b.job;
                    if (a.trial.slot!=b.trial.slot) return a.trial.slot<b.trial.slot;
                    return quality(a,b);
                });
                std::map<std::pair<int,int>,int> actorVisits;
                for (auto &p:probes)
                    if (!p.settled)
                    {
                        const auto actor=std::pair{p.job,p.trial.slot};
                        p.actorVisit=closingAttempts[actor]+actorVisits[actor]++;
                    }
                std::stable_sort(probes.begin(),probes.end(),[&](const auto &a,const auto &b) {
                    if (a.job!=b.job) return a.job<b.job;
                    if (a.actorVisit!=b.actorVisit) return a.actorVisit<b.actorVisit;
                    if ((a.tier<2)!=(b.tier<2)) return a.tier<2;
                    if (a.retainedSibling!=b.retainedSibling) return a.retainedSibling;
                    return quality(a,b);
                });
                std::array<int,5> jobVisits{};
                for (auto &p:probes)
                    if (!p.settled) p.jobVisit=jobVisits[p.job]++;
                std::stable_sort(probes.begin(),probes.end(),[](const auto &a,const auto &b) {
                    const auto firstGain=[](const ClosingProbe &p) {
                        return p.actorVisit==0 && p.openingReplyGain.value_or(0)>0;
                    };
                    // Positive immediate fire is an ordering hint within a visit.
                    // A different family still gets to test its first full ten-turn alternative.
                    return std::tuple{!a.settled,a.jobVisit,!firstGain(a),a.job}<
                           std::tuple{!b.settled,b.jobVisit,!firstGain(b),b.job};
                });
                std::map<int,std::array<int,4>> outcomes;
                std::map<int,QJsonObject> opportunities;
                std::set<int> retainedUnits;
                int newPrices=0;
                const auto preparedOpeningCount=openingPrices.size();
                for (const auto &probe:probes)
                {
                    const auto &t=probe.trial;
                    const auto key=exactKey(t);
                    const CombatFireOrder order{std::get<1>(key),std::get<2>(key)};
                    auto priorTrial=attempted.find(key);
                    const int unit=problem.actors()[t.slot].engineUnitId;
                    if (probe.retainedSibling) retainedUnits.insert(unit);
                    auto &counts=outcomes[unit];
                    auto &reasons=opportunities[unit];
                    reasons[probe.opportunity]=reasons.value(probe.opportunity).toInt()+1;
                    const bool rebase=winningRecord!=anchorTrial;
                    if (!rebase && priorTrial!=attempted.end() && priorTrial->second>=0 &&
                        trials[priorTrial->second].toObject()["status"]=="RACE_UNPRICED" && budget>=minimumReplayBudget &&
                        retried.insert(key).second)
                    {
                        attempted.erase(priorTrial);
                        priorTrial=attempted.end();
                    }
                    // Carry independent gains forward while there is still room to
                    // check them together. A rebase never marks a skipped order priced.
                    if (priorTrial==attempted.end() && budget>=minimumReplayBudget && !rebase)
                    {
                        const int first=trials.size();
                        // A retry must rebuild the opening if the first price consumed it.
                        auto prepared=openingPrices.extract(key);
                        evaluateTrial(t,order,prepared.empty()?nullptr:&prepared.mapped());
                        newPrices+=trials.size()-first;
                        if (trials.size()>first) ++closingAttempts[{probe.job,t.slot}];
                        priorTrial=attempted.find(key);
                    }
                    if (priorTrial==attempted.end())
                    {
                        // Save only the still-current team's actual generated
                        // probe, including every teammate and firing order.
                        if(!rebase && anchorTrial<0 && anchor==effectiveChoices && savedClosingOrders.insert(key).second)
                            unvisitedClosing.push_back({t,order,key});
                        ++counts[3];continue;
                    }
                    const auto status=priorTrial->second>=0 ? trials[priorTrial->second].toObject()["status"].toString() : QString();
                    if (status=="RACE_UNPRICED")
                    {
                        ++counts[3];
                        if(!rebase && anchorTrial<0 && anchor==effectiveChoices && savedClosingOrders.insert(key).second)
                            unfinishedClosing.push_back({t,order,key});
                    }
                    else if (status=="DOMINATED_BY_REPLY") ++counts[1];
                    else if (priorTrial->second<0 || trials[priorTrial->second].toObject().contains("policyTotal")) ++counts[0];
                    else ++counts[2];
                }
                QJsonArray units;
                for (const auto &[unit,counts]:outcomes)
                    units.append(QJsonObject{{"unit",unit},{"retainedSiblings",retainedUnits.contains(unit)},
                        {"opportunities",opportunities[unit]},{"priced",counts[0]},{"bounded",counts[1]},
                        {"invalid",counts[2]},{"unresolved",counts[3]}});
                closingComplete=complete && winningRecord==anchorTrial && std::none_of(outcomes.begin(),outcomes.end(),
                    [](const auto &entry) {return entry.second[3]>0;});
                closingRounds.append(QJsonObject{{"round",round},{"anchorTrial",anchorTrial},{"selectedTrial",winningRecord},
                    {"catalogComplete",complete},{"openingReplies",static_cast<qint64>(preparedOpeningCount)},
                    {"openingReplyWork",static_cast<qint64>(lookaheadWork-individualLookahead-coordinatedLookahead)},
                    {"newTrials",newPrices},{"units",units},{"bookedRoutes",routes},{"work",static_cast<qint64>(prior-budget)}});
                if (winningRecord==anchorTrial) break;
            }
            budget+=certificationReserve;
            bool finalComparisonUnresolved=false,selectedWithRankBound=false;
            int certificationPass=0;
            bool extendedFundingHearing=false;
            std::set<PlanKey> exhaustedPairProofs;
            QJsonArray rejectedAgainstReference;
            std::int64_t unusedAdmissionWork=0;
            const auto certifyTeam=[&](bool firstFullQuote,bool firstHearing=false){
                ++certificationPass;
                result.remove("selectedPairDecision");result.remove("selectedPairArm");
                selectedWithRankBound=false;
                const bool advancingPair=!winner && pendingPair.has_value();
                if(advancingPair)
                {
                    winningTrial=pendingPair->trial;winningOrder=pendingPair->fire;winningRecord=pendingPair->index;
                    winningChoices=std::move(pendingPair->orders);winner=std::move(pendingPair->plan);
                    if(!result.contains("continuedCandidate"))result["continuedCandidate"]=winningRecord;
                    pendingPair.reset();
                }
                StockChallengeMenu finalStock;
                const auto clearFrontier=qScopeGuard([&]{routeCache.decisionFrontier.reset();routeCache.fundedPostQuestions={};});
                if(winner && winningRecord>=0)routeCache.fundedPostQuestions=pairedFundedPosts(base,trials[winningRecord].toObject());
                if(!firstFullQuote && winner && winningRecord>=0)
                    routeCache.decisionFrontier=nominateTownFrontier(base,trials[winningRecord].toObject(),&budget,TownPriorityScope::ObservedPolicies);
                if(!firstFullQuote && winner && winningRecord>=0)finalStock=additionalStockChallenges(base,trials[winningRecord].toObject());
                if (winner)
                {
                    const PlanKey currentPairKey{winningChoices,winningOrder.first,winningOrder.second};
                    std::set<TownIntention> requested;
                    bool decided=false;
                    // Sharing is a positive certificate of the common methods. The
                    // upcoming comparison replays each physical team and buyer.
                    shareServiceValidation(base["purchaseBoundary"].toObject(),trials[winningRecord].toObject()["purchaseBoundary"].toObject());
                    PairProofMeter pairMeter{pairProofAllowance,budget,raceWork,result,winningRecord,pairProofTurnCap,pairProofMaxPasses};
                    const auto restoreLocalQuotas=qScopeGuard([&]{routeCache.stockRouteEffort=1;routeCache.paidRouteEffort=1;});
                    const auto finishMeter=qScopeGuard([&]{pairMeter.finish(result.value("finalChallengeComparison").toObject()
                        .value("rankedPairDecision").toObject().value("status").toString(QStringLiteral("PAIR_RETURNED_OPEN")));});
                    routeCache.seenDutyTrips.clear();
                    const auto restore=qScopeGuard([&] {routeCache.requiredTownChallenges.clear();routeCache.matchedStockChallenges.clear();});
                    while (budget>=minimumReplayBudget)
                    {
                        routeCache.matchedStockChallenges=finalStock;
                        requested=discovered;
                        routeCache.requestedDutyTrips=routeCache.seenDutyTrips;
                        routeCache.requiredTownChallenges.clear();
                        for (const auto &i:requested)
                            routeCache.requiredTownChallenges.push_back({{i[0],i[1]},i[2],i[3],i[4]});
                        auto referenceReceipt=sourcePriceRecord;
                        auto winnerReceipt=forecastRecord(*winner,watched);
                        const auto beforePair=budget;
                        TownMarketPrefetchHint prefetchHint{routeCache.townEffort,beforePair/2,std::nullopt,
                            routeCache.requiredTownChallenges.size(),true};
#ifdef COW_COORDINATED_REFINEMENT_TESTING
                        prefetchHint.privateTownQuoteCap=routeCache.privateTownQuoteCap;
#endif
                        auto pairRegion=prefetch.enterPair(winningRecord,routeCache.townMarkets,*terrain,prefetchHint);
                        const auto left=worstReply(effectiveChoices,reference,referenceReceipt,nullptr,{},nullptr,nullptr,beforePair/2,true);
                        const auto afterLeft=budget;
                        const auto right=worstReply(winningChoices,*winner,winnerReceipt,nullptr,winningOrder,nullptr,nullptr,beforePair/2,true);
                        if(activateScreen(false,2*std::max(beforePair-afterLeft,afterLeft-budget)))continue;
                        remember(referenceReceipt);remember(winnerReceipt);
                        const auto newStock=additionalStockChallenges(referenceReceipt,winnerReceipt,finalStock);
                        mergeStockChallenges(finalStock,newStock);
                        QJsonArray openStock;
                        const auto policyOf=[](const QJsonObject &team){
                            const auto missing=team["unpricedReplyPolicy"].toString();
                            return missing.isEmpty()?team["replyPolicy"].toString():missing;
                        };
                        const bool sameStock=stockComparisonCovered(referenceReceipt,winnerReceipt,routeCache.matchedStockChallenges,&openStock);
                        result["finalChallengeComparison"]=QJsonObject{{"trial",winningRecord},{"pass",certificationPass},{"requested",static_cast<int>(requested.size())},
                            {"requiredStock",describeStockChallenges(routeCache.matchedStockChallenges)},
                            {"newStock",describeStockChallenges(newStock)},{"unansweredStock",openStock},
                            {"referencePolicy",policyOf(referenceReceipt)},{"candidatePolicy",policyOf(winnerReceipt)},
                            {"referenceDetails",referenceReceipt.value("unpricedReplyDetails")},{"candidateDetails",winnerReceipt.value("unpricedReplyDetails")},
                            {"remainingBefore",static_cast<qint64>(beforePair)},{"equalPriceAllowance",static_cast<qint64>(beforePair/2)},
                            {"referenceSpent",static_cast<qint64>(beforePair-afterLeft)},{"candidateSpent",static_cast<qint64>(afterLeft-budget)},
                            {"remainingAfter",static_cast<qint64>(budget)},{"referenceRefusal",referenceReceipt.value("unpricedReplyReason")},
                            {"candidateRefusal",winnerReceipt.value("unpricedReplyReason")},
                            {"reference",referenceReceipt.value("priceStatus")},{"candidate",winnerReceipt.value("priceStatus")},
                            {"referenceEvidence",referenceReceipt.value("townComparisonEvidence")},
                            {"candidateEvidence",winnerReceipt.value("townComparisonEvidence")}};
                        bool newFrontier=mergeFundedPostQuestions(routeCache.fundedPostQuestions,pairedFundedPosts(referenceReceipt,winnerReceipt));
                        if(!routeCache.decisionFrontier)
                        {
                            routeCache.decisionFrontier=nominateTownFrontier(referenceReceipt,winnerReceipt,&budget,TownPriorityScope::ObservedPolicies);
                            newFrontier|=routeCache.decisionFrontier.has_value();
                        }
                        if(routeCache.decisionFrontier && routeCache.decisionFrontier->property())
                            if(auto due=bindTownAdmission(*routeCache.decisionFrontier,referenceReceipt,winnerReceipt))
                            {
                                result["finalTownAdmission"]=townFrontierPair(*routeCache.decisionFrontier,referenceReceipt,winnerReceipt);
                                routeCache.decisionFrontier=std::move(due);newFrontier=true;
                            }
                        if(routeCache.decisionFrontier)result["finalTownFrontier"]=townFrontierPair(*routeCache.decisionFrontier,referenceReceipt,winnerReceipt);
                        const auto openPosts=fundedPostObligations(routeCache.fundedPostQuestions,referenceReceipt,winnerReceipt);
                        const bool scope=openPosts.isEmpty() && !newFrontier && requested==discovered && sameStock && newStock.empty() && routeCache.requestedDutyTrips==routeCache.seenDutyTrips;
                        const auto proof=comparePair(referenceReceipt,winnerReceipt,scope);
                        const auto preferred=proof.result();auto pair=result["finalChallengeComparison"].toObject();
                        pair["unansweredFundedPostChecks"]=openPosts;
                        auto comparison=proof.record("incumbent","challenger",
                            routeCache.decisionFrontier ? result.value("finalTownFrontier").toObject() : QJsonObject{});
                        comparison["bindings"]=QJsonArray{bindPhysicalTeam(reference,{},referenceReceipt),bindPhysicalTeam(*winner,winningOrder,winnerReceipt)};
                        if(!openFactoryWaitSlots.empty()){
                            const PlanKey key{winningChoices,winningOrder.first,winningOrder.second};
                            if(preferred==PairPreference::Open && !settledPurchasePairs.contains(key))openPurchaseComparisons[key]=comparison;
                            else if(preferred!=PairPreference::Open){settledPurchasePairs.insert(key);openPurchaseComparisons.erase(key);}
                        }
                        comparison["unansweredFundedPostChecks"]=openPosts;
                        if(preferred==PairPreference::Open && !openPosts.isEmpty() && !comparison.contains("unresolvedConsequence"))
                            comparison["unresolvedConsequence"]=openPosts.first();
                        pair["rankedPairDecision"]=comparison;
                        result["finalChallengeComparison"]=pair;
                        const bool strict=preferred==PairPreference::First || preferred==PairPreference::Second;
                        const bool publishBound=strict && (referenceReceipt["priceStatus"]!="WITNESSED" || winnerReceipt["priceStatus"]!="WITNESSED");
                        // The endpoints belong to these exact physical orders. Rebuild both
                        // openings before acting on a proof without two exact policy totals.
                        bool verifiedRetainedPair=false;
                        if(publishBound || (preferred==PairPreference::First && budget>=2*replayTariff))
                        {
                            bool bothBuyersReplayed=true;
                            if(budget<2*replayTariff){pair["publicationRefusal"]="OPENING_REPLAY_BUDGET";result["finalChallengeComparison"]=pair;break;}
                            budget-=2*replayTariff;
                            QString replayGap;
                            const auto refuse=[&](QString why){replayGap=std::move(why);return false;};
                            const auto matches=[&](const auto &orders,CombatFireOrder fire,const ReplyPassEvaluation &quoted,const QJsonObject &receipt){
                                const auto rebuilt=evaluateReplyPassPlan(problem,orders,board,fire);
                                if(!rebuilt.valued())return refuse("REPLAY_UNVALUED_"+QString::number(int(rebuilt.reason)));
                                if(receipt.contains("economicEndpoint")){
                                    const auto identity=receipt["economicOrderIdentity"].toObject();
                                    if(identity["observation"].toObject()["board"]!=QString::fromLatin1(map.getMapHash().toHex()))return refuse("OBSERVED_BOARD_CHANGED");
                                    if(identity["buyer"]!=receipt["purchaseBoundary"])return refuse("ECONOMIC_PROOF_BUYER_CHANGED");
                                    if(identity["physicalOrders"]!=receipt["economicOrderBinding"])return refuse("ECONOMIC_PROOF_ORDER_CHANGED");
                                }
                                const auto pinned=receipt["purchaseBoundary"].toObject();
                                if(pinned.isEmpty())bothBuyersReplayed=false;
                                else if(receipt.contains("economicEndpoint") || pinned["openingSource"]=="NO_FUNDED_SLOT" ||
                                        pinned["openingSource"]=="COUNTERPOINT_PREVIEW"){
                                    const auto actual=purchaseBoundary(problem,*terrain,rebuilt,purchaseWindow.unitSlots);
                                    const auto capacity=purchaseWindow.affordable([&](Cell site){return actual.occupied.contains(site);},actual.available);
                                    if(pinned["affordableBuildSlots"].toInt(-1)!=capacity)return refuse("REPLAY_CAPACITY_CHANGED");
                                    if(pinned["spendableFunds"].toInteger(-1)!=purchaseWindow.treasury)return refuse("REPLAY_TREASURY_CHANGED");
                                    if(capacity>0){
                                        if(!buyer || pinned["openingSource"]!="COUNTERPOINT_PREVIEW")return refuse("BUYER_UNAVAILABLE");
                                        const auto projection=openingBuyerProjection(map,knowledge,*terrain,problem,field,rebuilt,
                                            purchaseWindow.treasury,actual.available);
                                        auto repeated=buyer->previewCounterpointOpening(projection);
                                        auto quotedBuyer=pinned["buyerPreview"].toObject();
                                        // Counterpoint appends this provenance flag to a copy of
                                        // its cached portfolio. All decision fields stay bound.
                                        repeated.remove("reusedEquivalent");quotedBuyer.remove("reusedEquivalent");
                                        if(repeated!=quotedBuyer){
                                            pair["replayedBuyer"]=repeated;pair["quotedBuyer"]=pinned["buyerPreview"];
                                            return refuse("REPLAY_BUYER_CHANGED");
                                        }
                                    }
                                }
                                else bothBuyersReplayed=false;
                                if(rebuilt.afterOwnActions!=quoted.afterOwnActions)return refuse("OWN_ACTION_LEDGER_CHANGED");
                                if(rebuilt.afterReply!=quoted.afterReply)return refuse("REPLY_LEDGER_CHANGED");
                                return
                                    masterPlanEquivalent(problem,rebuilt.replay.plan,rebuilt.replay.ordering.order,quoted.replay.plan,quoted.replay.ordering.order);
                            };
                            if(!matches(effectiveChoices,{},reference,referenceReceipt) || !matches(winningChoices,winningOrder,*winner,winnerReceipt)){
                                pair["publicationRefusal"]="OPENING_REPLAY_CHANGED";
                                pair["publicationDetail"]=replayGap.isEmpty()?QStringLiteral("PHYSICAL_PLAN_OR_EXECUTION_ORDER_CHANGED"):replayGap;
                                result["finalChallengeComparison"]=pair;
                                if(publishBound)break;
                            }else verifiedRetainedPair=bothBuyersReplayed;
                        }
                        if(publishBound && preferred==PairPreference::First){
                            settledPurchasePairs.emplace(winningChoices,winningOrder.first,winningOrder.second);
                            result["selectedPairDecision"]=comparison;result["selectedPairArm"]=0;
                            auto rejected=trials[winningRecord].toObject();
                            if(rejected["status"]=="RACE_UNPRICED"){--unpriced;++bounded;}
                            rejected["status"]="DOMINATED_BY_REPLY";
                            rejected["rankedPairDecision"]=comparison;
                            if(verifiedRetainedPair)rejectedAgainstReference.append(QJsonObject{{"trial",winningRecord},{"acceptedArm",0},{"observation",economicObservation},{"rankedPairDecision",comparison}});
                            rejected.remove("policyTotal");trials[winningRecord]=rejected;
                            base=referenceReceipt;result["reference"]=base;best=base["priceStatus"]=="WITNESSED"?left:std::nullopt;
                            winner.reset();winningRecord=-1;decided=true;finalComparisonUnresolved=false;selectedWithRankBound=true;break;
                        }
                        // A changed purchase queue and an interrupted directional search
                        // identify a concrete outstanding comparison. Offer at most one
                        // such first hearing a regular, explicitly metered continuation.
                        const auto referenceQueue=referenceReceipt["purchaseBoundary"].toObject().value("modeledOpening");
                        const auto candidateQueue=winnerReceipt["purchaseBoundary"].toObject().value("modeledOpening");
                        PairProofProgress progress;progress.observe(referenceReceipt);progress.observe(winnerReceipt);
                        const bool resumeFunding=firstHearing && progress.workLimited && referenceQueue.isArray() && candidateQueue.isArray() &&
                            referenceQueue!=candidateQueue && (!extendedFundingHearing || pairMeter.active);
                        if(preferred==PairPreference::Open && resumeFunding)extendedFundingHearing=true;
                        if(preferred==PairPreference::Open && firstHearing && !resumeFunding)
                        {
                            // This is a real price attempt on this opening, not
                            // a bound. Retain its buyer and missing consequence
                            // so the deep pass can return to the same team.
                            auto item=trials[winningRecord].toObject();
                            item["firstPairDecision"]=comparison;
                            item["firstReferenceBuyer"]=referenceReceipt.value("purchaseBoundary");
                            item["firstCandidateBuyer"]=winnerReceipt.value("purchaseBoundary");
                            item["firstReferenceRefusal"]=referenceReceipt.value("unpricedReplyReason");
                            item["firstCandidateRefusal"]=winnerReceipt.value("unpricedReplyReason");
                            trials[winningRecord]=item;
                            break;
                        }
                        if(preferred==PairPreference::Open && pairProofAllowance>0)
                        {
                            const bool freshScope=newFrontier || requested!=discovered || !newStock.empty() || routeCache.requestedDutyTrips!=routeCache.seenDutyTrips;
                            if(pairMeter.resume(referenceReceipt,winnerReceipt,proofScope(scope,discovered,finalStock),minimumReplayBudget,freshScope))
                            {
                                deepenConfirmedRoutes(pairMeter);
                                continue;
                            }
                            if(pairMeter.passes)break;
                        }
                        if ((!left || !right) && !publishBound && !newFrontier && newStock.empty())break;
                        if((newFrontier || !newStock.empty()) && !publishBound)continue;
                        if((!sameStock || !openPosts.isEmpty()) && !publishBound)break;
                        if(!publishBound && (referenceReceipt.value("priceStatus")!="WITNESSED" || winnerReceipt.value("priceStatus")!="WITNESSED"))
                        {
                            const bool limited=referenceReceipt["townWorkLimitedReply"].toBool() || winnerReceipt["townWorkLimitedReply"].toBool();
                            if(requested!=discovered || (routeCache.requestedDutyTrips!=routeCache.seenDutyTrips && budget/2>=beforePair-budget) ||
                               deepenPair(beforePair-budget,limited))continue;
                            break;
                        }
                        if (!publishBound && (requested!=discovered || routeCache.requestedDutyTrips!=routeCache.seenDutyTrips)) continue;
                        if(preferred==PairPreference::Open)break;
                        base=std::move(referenceReceipt);
                        result["reference"]=base;
                        decided=true;
                        if(advancingPair)
                        {
                            auto original=trials[winningRecord].toObject();
                            for(auto key:{"changes","unit","candidate","baseCandidate","baseDestination","partnerUnit","partnerCandidate",
                                  "partnerDestination","companionUnit","companionCandidate","destination","target","searchRound",
                                  "fireOrder","coordination","sharedTownComparisons"})
                                if(original.contains(key))winnerReceipt[key]=original[key];
                            winnerReceipt["status"]="CERTIFIED_COMPARISON";
                            trials[winningRecord]=winnerReceipt;
                            if(original["status"]=="RACE_UNPRICED")--unpriced;
                        }
                        if (preferred==PairPreference::Second)
                        {
                            const auto original=trials[winningRecord].toObject();
                            for (auto key:{"changes","unit","candidate","baseCandidate","baseDestination","partnerUnit","partnerCandidate",
                                          "partnerDestination","companionUnit","companionCandidate","destination","target","searchRound",
                                          "fireOrder","comparedToTrial","selectionBasis","coordination","sharedTownComparisons"})
                                if (original.contains(key)) winnerReceipt[key]=original.value(key);
                            if(strict && preferred==PairPreference::Second)
                            {result["selectedPairDecision"]=comparison;result["selectedPairArm"]=1;}
                            if(publishBound)
                            {
                                winnerReceipt["selectionBasis"]="STRICT_MATCHED_BOUND";
                                if(winnerReceipt["priceStatus"]!="WITNESSED")
                                {
                                    recordEconomicEvidence(winnerReceipt,false);
                                    winnerReceipt["status"]="STRICT_PARTIAL_PREFERENCE";
                                }
                            }
                            else if(advancingPair)winnerReceipt["selectionBasis"]="CERTIFIED_TEN_TURN_COMPARISON";
                            trials[winningRecord]=winnerReceipt;
                            winningReplyKey=winnerReceipt["responseKey"].toArray();
                            selectedWithRankBound=publishBound;
                            best=winnerReceipt["priceStatus"]=="WITNESSED"?right:std::nullopt;finalComparisonUnresolved=false;
                        }
                        else
                        {
                            // An exact loss or tie already settles these orders.
                            // Later economic preferences apply only to open pairs.
                            settledPurchasePairs.emplace(winningChoices,winningOrder.first,winningOrder.second);
                            if(strict && preferred==PairPreference::First){
                                result["selectedPairDecision"]=comparison;result["selectedPairArm"]=0;
                                if(verifiedRetainedPair)rejectedAgainstReference.append(QJsonObject{{"trial",winningRecord},{"acceptedArm",0},{"observation",economicObservation},{"rankedPairDecision",comparison}});
                            }
                            best=left;
                            winner.reset();winningRecord=-1;
                        }
                        break;
                    }
                    if (!decided)
                    {
                        if(pairMeter.passes>0)exhaustedPairProofs.insert(currentPairKey);
                        finalComparisonUnresolved=true;
                        winner.reset();winningRecord=-1;
                    }
                }
            };
            const bool originalAnchor=!winner && anchor==effectiveChoices && anchorOrder.first==NO_UNIT;
            const auto initialEffort=routeCache.townEffort;
            const std::optional<PlanKey> firstFinalKey=winner ? std::optional<PlanKey>{{winningChoices,winningOrder.first,winningOrder.second}} :
                pendingPair ? std::optional<PlanKey>{{pendingPair->orders,pendingPair->fire.first,pendingPair->fire.second}} : std::nullopt;
            bool certifiedDuringFirstHearings=false;
            if(originalAnchor && pairProofAllowance>0)
            {
                // The shared turn meter may have covered legality but stopped
                // inside movement prediction. An open order must reach both
                // buyers before another open order takes all the deep passes.
                std::optional<PendingPair> priorTeam;
                auto primary=std::move(pendingPair);
                pendingPair.reset();
                std::vector<DeferredClosingOrder> queue;
                // When one actor determines the entire team, every plannable
                // revision is already a complete candidate for this source.
                if(effectiveChoices.size()==1){
                    const auto &single=problem.actors().front();
                    for(int option=0;option<int(single.candidates.size());++option)
                        if(option!=effectiveChoices.front() && single.candidates[option].plannable)
                            queue.push_back({Trial{0,option,0},CombatFireOrder{},PlanKey{{option},NO_UNIT,NO_UNIT}});
                }
                if(primary)queue.push_back({primary->trial,primary->fire,PlanKey{primary->orders,primary->fire.first,primary->fire.second}});
                queue.insert(queue.end(),movementLimited.begin(),movementLimited.end());
                queue.insert(queue.end(),unvisitedClosing.begin(),unvisitedClosing.end());
                queue.insert(queue.end(),unfinishedClosing.begin(),unfinishedClosing.end());
                queue.insert(queue.end(),unpricedPhysicalOrders.begin(),unpricedPhysicalOrders.end());
                // The upstream stock comparison can exchange an entire squad.
                // Keep its independently legal incoming orders available to
                // the full formula even when more than three teammates changed.
                const std::vector<int> incoming(sourceRivalChoices.begin(),sourceRivalChoices.end());
                const PlanKey incomingKey{incoming,NO_UNIT,NO_UNIT};
                if(incoming.size()==effectiveChoices.size() && incoming!=effectiveChoices)
                {
                    const int slot=int(std::mismatch(incoming.begin(),incoming.end(),effectiveChoices.begin()).first-incoming.begin());
                    queue.insert(queue.begin(),{Trial{slot,incoming[slot],0},CombatFireOrder{},incomingKey});
                }
                std::set<PlanKey> unique;
                std::erase_if(queue,[&](const auto &order){return !unique.insert(order.key).second;});
                // The ordinary first pass may run out of work before a complete
                // proposed team gets its own buyer. Keep its actual teammates
                // and firing order. Only a strict full comparison may install it.
                const auto previousQueueSize=queue.size();
                for(const auto &deferred:deferredOnConstructor)
                    if(!attempted.contains(deferred.key) && unique.insert(deferred.key).second)
                        queue.push_back(deferred);
                auto pool=pairProofAllowance;
                auto deferredPool=queue.size()>previousQueueSize?std::min(pairProofAllowance,allowance):0;
                const auto deferredAllowance=deferredPool;
                QJsonArray admissions;
                const auto savedBudget=budget,savedRace=raceWork;
                constexpr int MAX_CONSECUTIVE_ADMISSION_OPEN=10;
                constexpr int MAX_CONSECUTIVE_DEFERRED_OPEN=8;
                int consecutiveMainOpen=0;
                int consecutiveDeferredOpen=0;
                const std::optional<PlanKey> primaryKey=primary
                    ? std::optional<PlanKey>{PlanKey{primary->orders,primary->fire.first,primary->fire.second}}
                    : std::nullopt;
                for(std::size_t n=0;n<queue.size() && !winner;++n)
                {
                    const auto &next=queue[n];const auto &orders=std::get<0>(next.key);
                    const bool fromConstructor=n>=previousQueueSize;
                    const bool nativeIncoming=next.key==incomingKey;
                    const bool isPrimary=primaryKey && next.key==*primaryKey;
                    if(!fromConstructor && !nativeIncoming && !isPrimary &&
                       consecutiveMainOpen>=MAX_CONSECUTIVE_ADMISSION_OPEN)continue;
                    if(fromConstructor && consecutiveDeferredOpen>=MAX_CONSECUTIVE_DEFERRED_OPEN)break;
                    auto &remaining=fromConstructor?deferredPool:pool;
                    const auto end=fromConstructor?queue.size():previousQueueSize;
                    auto assigned=remaining/static_cast<std::int64_t>(end-n);
                    if(fromConstructor)
                        assigned=std::max(assigned,std::min(remaining,replayTariff+2*minimumReplayBudget));
                    if(assigned<replayTariff+2*minimumReplayBudget)continue;
                    const auto prior=attempted.find(next.key);
                    const bool fresh=prior==attempted.end();
                    int index=fresh?int(trials.size()):prior->second;
                    if(!fresh && (index<0 || !unfinishedOpenings.contains(index)))continue;
                    budget=assigned;raceWork=0;
                    routeCache.townEffort=initialEffort;
                    routeCache.requiredTownChallenges.clear();routeCache.matchedStockChallenges.clear();
                    routeCache.seenDutyTrips.clear();routeCache.requestedDutyTrips.clear();
                    pendingPair.reset();
                    if(fresh)evaluateTrial(next.trial,next.fire,nullptr,true,&orders,nativeIncoming);
                    else
                    {
                        budget-=replayTariff;
                        auto replay=evaluateReplyPassPlan(problem,orders,board,next.fire);
                        if(replay.valued() && replay.replay.ordering.order==unfinishedOpenings.at(index) &&
                           (!nativeIncoming || std::ranges::equal(replay.replay.ordering.order,sourceRivalOrder)))
                        {
                            auto item=trials[index].toObject();
                            if(item["status"]=="MOVEMENT_LIMIT")
                            {
                                --unreplayable;++unpriced;
                                item["status"]="RACE_UNPRICED";item["priceStatus"]="UNRESOLVED";
                                item["recoveredMovementAdmission"]=true;trials[index]=item;
                            }
                            pendingPair=PendingPair{next.trial,std::move(replay),orders,next.fire,0,index};
                        }
                    }
                    const bool entered=bool(pendingPair);
                    if(entered)
                    {
                        if(nativeIncoming)priorTeam=*pendingPair;
                        certifyTeam(true,true);
                        certifiedDuringFirstHearings=bool(winner);
                    }
                    const auto used=assigned-budget;remaining-=used;
                    QJsonObject row{{"trial",index},{"entered",entered},{"offered",static_cast<qint64>(assigned)},
                        {"spent",static_cast<qint64>(used)},{"race",static_cast<qint64>(raceWork)}};
                    if(nativeIncoming)row["sourceTeam"]=sourceRivalLabel;
                    else if(fromConstructor)row["sourceTeam"]="DEFERRED_CONSTRUCTOR_TEAM";
                    if(entered){
                        const auto checked=result.value("finalChallengeComparison").toObject();
                        const auto pairStatus=checked["rankedPairDecision"].toObject()["status"].toString();
                        row["status"]=pairStatus;
                        const bool openPair=pairStatus.isEmpty() || pairStatus=="OPEN" || pairStatus=="UNRESOLVED" || pairStatus=="INSUFFICIENT_BOUNDS";
                        if(fromConstructor)consecutiveDeferredOpen=openPair?consecutiveDeferredOpen+1:0;
                        else if(!nativeIncoming && !isPrimary)consecutiveMainOpen=openPair?consecutiveMainOpen+1:0;
                        if(checked.contains("publicationRefusal")){
                            row["publicationRefusal"]=checked["publicationRefusal"];row["publicationDetail"]=checked["publicationDetail"];
                            if(checked.contains("replayedBuyer")){row["replayedBuyer"]=checked["replayedBuyer"];row["quotedBuyer"]=checked["quotedBuyer"];}
                        }
                    }
                    else if(index>=0 && index<trials.size()){
                        row["status"]=trials[index].toObject()["status"];
                        if(fromConstructor)++consecutiveDeferredOpen;
                    }
                    admissions.append(row);
                    budget=savedBudget;raceWork=savedRace;
                }
                routeCache.townEffort=initialEffort;
                pendingPair.reset();
                if(!winner && priorTeam && (!primary || priorTeam->index!=primary->index) &&
                   trials[priorTeam->index].toObject()["status"]=="RACE_UNPRICED" &&
                   !exhaustedPairProofs.contains(PlanKey{priorTeam->orders,priorTeam->fire.first,priorTeam->fire.second}))
                {
                    // Both native source portfolios merit the same substantive
                    // comparison as a local candidate. Leave the local
                    // primary its part of the original certification reserve.
                    pendingPair=std::move(priorTeam);
                    const int originalIndex=pendingPair->index;
                    withinShare(primary?budget/2:budget,[&]{certifyTeam(false);return true;});
                    const auto checked=result.value("finalChallengeComparison").toObject();
                    if(checked["trial"].toInt(-1)==originalIndex)
                        result[handoff->stockProposal ? "stockProposalComparison" : "upstreamTeamComparison"]=checked;
                    certifiedDuringFirstHearings|=bool(winner);
                }
                if(!winner && primary && trials[primary->index].toObject()["status"]=="RACE_UNPRICED" &&
                   !exhaustedPairProofs.contains(PlanKey{primary->orders,primary->fire.first,primary->fire.second}))
                    pendingPair=std::move(primary);
                result["openingAdmissionWork"]=static_cast<qint64>(pairProofAllowance-pool);
                // The candidate queue is closed. Its completed obligations and
                // traces remain in place. Only unused credits can move on.
                unusedAdmissionWork=pool;
                result["openingAdmissionAllowance"]=static_cast<qint64>(pairProofAllowance);
                result["openingAdmissions"]=admissions;
                result["deferredConstructorOffered"]=static_cast<qint64>(queue.size()-previousQueueSize);
                result["deferredConstructorAllowance"]=static_cast<qint64>(deferredAllowance);
                result["deferredConstructorWork"]=static_cast<qint64>(deferredAllowance-deferredPool);
            }
            if(!certifiedDuringFirstHearings)certifyTeam(false);
            QJsonArray remainingTrials;
            const auto initialPair=result.value("finalChallengeComparison");
            // A previous partial quote is a useful starting point for a full comparison.
            // It cannot settle an order. Give each remaining order access to the reserve
            // and return unused work to later pairs on this same opening squad.
            unvisitedClosing.insert(unvisitedClosing.end(),unfinishedClosing.begin(),unfinishedClosing.end());
            std::erase_if(unvisitedClosing,[&](const auto &order){
                if((firstFinalKey && order.key==*firstFinalKey) || exhaustedPairProofs.contains(order.key))return true;
                const auto found=attempted.find(order.key);
                return found!=attempted.end() && (found->second<0 || !unfinishedOpenings.contains(found->second) ||
                    trials[found->second].toObject()["status"]!="RACE_UNPRICED");
            });
            for(std::size_t n=0;n<unvisitedClosing.size();++n)
            {
                const auto &next=unvisitedClosing[n];
                if(!originalAnchor || winner || budget<replayTariff+2*minimumReplayBudget)break;
                const auto old=attempted.find(next.key);
                const bool resume=old!=attempted.end();
                const auto before=budget;
                const int index=resume ? old->second : int(trials.size());
                // The previous pair may have run out inside a different
                // guard search. Its exact market proofs can be reused only
                // by a matching board. Start this new pair at the usual rate.
                routeCache.townEffort=initialEffort;
                routeCache.requiredTownChallenges.clear();routeCache.matchedStockChallenges.clear();
                routeCache.requestedDutyTrips.clear();routeCache.seenDutyTrips.clear();
                pendingPair.reset();winningOrder={};winningChoices.clear();
                bool entered=false;
                const auto share=budget/static_cast<std::int64_t>(unvisitedClosing.size()-n);
                if(share<replayTariff+2*minimumReplayBudget)
                {
                    remainingTrials.append(QJsonObject{{"unit",problem.actors()[next.trial.slot].engineUnitId},
                        {"candidate",next.trial.option},{"trial",resume?index:-1},{"wholePairAttempted",false},
                        {"reason","PAIR_ALLOWANCE_BELOW_REPLAY_FLOOR"},{"pairAllowance",static_cast<qint64>(share)}});
                    continue;
                }
                withinShare(share,[&]{
                    if(resume)
                    {
                        const auto &orders=std::get<0>(next.key);
                        budget-=replayTariff;
                        auto replay=evaluateReplyPassPlan(problem,orders,board,next.fire);
                        if(replay.valued() && replay.replay.ordering.order==unfinishedOpenings.at(index))
                            pendingPair=PendingPair{next.trial,std::move(replay),orders,next.fire,0,index};
                    }
                    else evaluateTrial(next.trial,next.fire,nullptr,true);
                    entered=pendingPair.has_value();
                    if(entered)certifyTeam(!resume);
                    return entered;
                });
                const auto item=index<trials.size()?trials[index].toObject():QJsonObject{};
                const auto attempt=result.value("finalChallengeComparison").toObject();
                remainingTrials.append(QJsonObject{{"unit",problem.actors()[next.trial.slot].engineUnitId},
                    {"candidate",next.trial.option},{"fireOrder",QJsonArray{next.fire.first,next.fire.second}},
                    {"trial",index<trials.size()?index:-1},{"wholePairAttempted",entered},
                    {"resumedUnpricedTrial",resume},{"pairAllowance",static_cast<qint64>(share)},
                    {"priceStatus",item["priceStatus"]},{"trialStatus",item["status"]},
                    {"pairedComparison",entered && attempt["pass"].toInt(-1)==certificationPass && attempt["trial"].toInt(-1)==index?QJsonValue(attempt):QJsonValue(QJsonValue::Null)},
                    {"remainingBefore",static_cast<qint64>(before)},{"remainingAfter",static_cast<qint64>(budget)}});
            }
            if(!remainingTrials.isEmpty())
            {
                result["initialFinalChallengeComparison"]=initialPair;
                result["closingCoverageMeasuredBeforeReserveContinuation"]=true;
                result["continuedClosingTrials"]=remainingTrials;
            }
            result["certificationReserve"]=static_cast<qint64>(certificationReserve);
            result["reusedCompletedTownSlots"]=routeCache.townMarkets.completedReads;
            result["reusedIdenticalReplySlots"]=routeCache.townMarkets.aliasCompletedReads;
            result["priorityTownMarketEvictions"]=routeCache.townMarkets.priorityEvictions;
            result["resumedExactTownSlots"]=routeCache.townMarkets.resumedReads;
            result["finalTeamComparisons"]=closingRounds;
            result["finalTeamScopedComparisonsComplete"]=closingComplete;
            result["finalTeamComparisonApplicable"]=economicSearchOpen();
            result["finalTeamComparisonScope"]="FUNDED_OCCUPIED_SITES_IDLE_ACTIONS_AND_PREVIOUS_TEAMMATE_ASSIGNMENTS";
            result["equivalentFireOrders"] = equivalentOrders;
            result["searchRounds"] = rounds;
            if (handoff->stockProposal && winner)
            {
                const auto proof=result.value("selectedPairDecision").toObject();
                if (result.value("selectedPairArm").toInt(-1)!=1 || proof["status"]!="SECOND_PROVED")
                {
                    winner.reset();
                    winningRecord=-1;
                    best.reset();
                    finalComparisonUnresolved=true;
                    result.remove("selectedPairArm");
                    result.remove("selectedPairDecision");
                }
            }
            result["selectedTrial"] = winningRecord;
            QJsonArray referenceChoices;
            for (int choice : effectiveChoices)
                referenceChoices.append(choice);
            result["referenceChoices"] = referenceChoices;
            result["initialEligible"] = static_cast<qint64>(originalCandidates.size());
            const auto pending = std::count_if(deferredPlans.begin(), deferredPlans.end(),
                                               [&](const auto &plan) { return !attempted.contains(plan); });
            result["deferredPlans"] = static_cast<qint64>(pending);
            result["eligible"] = static_cast<qint64>(trials.size() + pending);
            result["trials"] = trials;
            result["truncated"] =
                (!closingComplete && economicSearchOpen()) || pending > 0 || unpriced > 0 ||
                (winner && winningChoices != anchor && budget < minimumReplayBudget);
            result["unpriced"] = unpriced;
            result["unreplayable"] = unreplayable;
            result["boundedByReply"] = bounded;
            const auto directionalOnly=std::count_if(trials.begin(),trials.end(),[](const auto &v){
                return v.toObject().value("status")=="STRICT_PARTIAL_PREFERENCE";
            });
            result["provedWithoutExactPrice"]=static_cast<qint64>(directionalOnly);
            result["fullyScored"] = trials.size() - unpriced - unreplayable - bounded - directionalOnly;
            result["selectedChanges"] =
                winningRecord < 0 ? QJsonArray{} : trials[winningRecord].toObject()["changes"].toArray();
            result["raceWork"] = static_cast<qint64>(raceWork);
            result["replyMovementCacheHits"] = terrain->movementCache->hits;
            result["replyMovementCacheMisses"] = terrain->movementCache->misses;
            result["captureRouteCacheHits"] = routeCache.hits;
            result["captureRouteCacheMisses"] = routeCache.misses;
            result["geometryWork"] = static_cast<qint64>(allowance - budget - raceWork);
            result["status"] =
                trials.size() == unpriced + unreplayable && !trials.empty() ? "NO_SCORABLE_ALTERNATIVE" : "RETAINED";
            if (winner)
            {
                if (repair.evaluation) result["selectedChanges"] = changedFromSource(winningChoices);
                result["changedUnit"] = problem.actors()[winningTrial.slot].engineUnitId;
                result["changedCandidate"] = winningTrial.option;
                QJsonArray order, selected;
                for (int index : winner->replay.ordering.order)
                    order.append(index);
                choices = winningChoices;
                if (winningTrial.partner >= 0)
                {
                    result["changedPartner"] = problem.actors()[winningTrial.partner].engineUnitId;
                    result["changedPartnerCandidate"] = winningTrial.partnerOption;
                }
                for (int index : choices)
                    selected.append(index);
                result["executionOrder"] = order;
                result["choices"] = selected;
                result["economicReplyKey"] = keyRecord(winner->afterReply);
                result["selectedKey"] = winningReplyKey;
                if (winningOrder.first != NO_UNIT)
                    result["selectedFireOrder"] = QJsonArray{winningOrder.first,winningOrder.second};
                if(best && trials[winningRecord].toObject()["priceStatus"]=="WITNESSED")
                    result["selectedPolicyTotal"] = static_cast<qint64>(std::get<3>(*best));
                result["status"] = "POLICY_PREFERRED";
                if (mode == CombatMode::Acting)
                {
                    const bool accepted = publish(*handoff, *winner, winningTrial.slot, sourceReference, assignment);
                    result["installed"] = accepted;
                    result["status"] = accepted ? "INSTALLED" : "PUBLICATION_REFUSED";
                }
            }
            bool openingAlternativeInstalled=false;
            StockProgressPlan progress{effectiveChoices,{}};
            const auto reservedOpening=std::max(pairProofAllowance,allowance);
            auto availableOpening=reservedOpening-(factorySurfaceOffer-factorySurfaceLeft);
            if(!winner && openFactoryWaitSlots.empty() && buyer && mode==CombatMode::Acting && !fixedOrderAudit &&
               progressInput && handoff->stockProposal)
                progress=matchedStockProgress(problem,*terrain,knowledge,progressSites,base,effectiveChoices,
                    handoff->stockProposal->choices,availableOpening);
            const bool openTacticalEligible = !winner && buyer && mode == CombatMode::Acting && !fixedOrderAudit &&
                progressInput && !problem.actors().empty();
            if(!winner && (!openFactoryWaitSlots.empty() || !progress.measures.isEmpty() || factoryProgrammeSurface || openTacticalEligible) && buyer && mode==CombatMode::Acting && !fixedOrderAudit){
                // These are complete teams that the acting planner admitted or
                // deferred. Keep each original firing order in the identity.
                struct PhysicalOption {PlanKey key;int trial;};
                std::vector<PhysicalOption> options;
                std::set<PlanKey> collected;
                const auto rememberOption=[&](const PlanKey &key,int index){
                    const auto &v=std::get<0>(key);
                    if(v.size()!=effectiveChoices.size() || v==effectiveChoices || settledPurchasePairs.contains(key))return;
                    if(std::none_of(openFactoryWaitSlots.begin(),openFactoryWaitSlots.end(),[&](int i){return v[i]!=effectiveChoices[i];}))return;
                    if(collected.insert(key).second)options.push_back({key,index});
                };
                for(const auto &[key,i]:attempted)rememberOption(key,i);
                for(const auto &key:deferredPlans)rememberOption(key,-1);
                for(const auto &x:deferredOnConstructor)rememberOption(x.key,-1);
                for(const auto &x:unvisitedClosing)rememberOption(x.key,-1);
                for(const auto &x:unfinishedClosing)rememberOption(x.key,-1);
                const auto changeCount=[&](const PlanKey &k){
                    int n=0;const auto &v=std::get<0>(k);
                    for(std::size_t i=0;i<v.size();++i)n+=v[i]!=effectiveChoices[i];
                    return n;
                };
                std::stable_sort(options.begin(),options.end(),[&](const auto &a,const auto &b){
                    const auto priority=[&](const auto &v){return std::pair{changeCount(v.key),v.trial<0?std::numeric_limits<int>::max():v.trial};};
                    return priority(a)<priority(b);
                });
                if(!options.empty() || !progress.measures.isEmpty() || factoryProgrammeSurface || openTacticalEligible){
                    openActingBuyer();
                    const auto buyerState=[&]{
                        QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);
                        buyer->serializeObject(stream);
                        return stream.status()==QDataStream::Ok?QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex():QByteArray{};
                    };
                    auto local=std::make_shared<CombatTerrain>(*terrain);
                    local->policy=TacticalReply::Economic;local->replyTarget=NO_UNIT;
                    local->openingRecruitTarget=nullptr;local->orderedOpening=nullptr;local->friendlyBirthSites.clear();
                    local->containFrom=INVALID_TILE;local->containShooter=NO_UNIT;
                    auto actingBoard=board;actingBoard.combat=local;
                    const std::int64_t granted=std::max(pairProofAllowance,allowance);
                    std::int64_t work=availableOpening;local->workLeft=&work;
                    const auto mapAtStart=map.getMapHash();
                    const auto startingBuyer=buyerState();
                    const auto openInitialAssignment=assignment;
                    struct Opening {ReplyPassEvaluation team;QJsonObject receipt;OpenPurchaseBoard board;QByteArray buyerStart;};
                    const auto replayOpening=[&](const std::vector<int> &orders,CombatFireOrder fire,
                                                 const QJsonObject &earlier)->std::optional<Opening>{
                        if(work<replayTariff || map.getMapHash()!=mapAtStart || startingBuyer.isEmpty() || buyerState()!=startingBuyer)return {};
                        work-=replayTariff;
                        Opening arm;
                        arm.team=evaluateReplyPassPlan(problem,orders,actingBoard,fire);
                        if(!arm.team.valued() || !arm.team.replay.valid() || !arm.team.replay.complete())return {};
                        const auto at=purchaseBoundary(problem,*local,arm.team,purchaseWindow.unitSlots);
                        const auto count=purchaseWindow.affordable([&](Cell c){return at.occupied.contains(c);},at.available);
                        QJsonArray available;
                        for(const auto &[cost,site]:purchaseWindow.sitesByCost)
                            if(cost<=purchaseWindow.treasury && !at.occupied.contains(site))available.append(QJsonArray{site.first,site.second});
                        if(count==0)available={};
                        const auto projection=openingBuyerProjection(map,knowledge,*local,problem,field,arm.team,purchaseWindow.treasury,at.available);
                        // A fresh buyer preview for each resulting board, including the
                        // zero-order portfolio when every production site is occupied.
                        buyer->endCounterpointOpeningPreview();buyer->beginCounterpointOpeningPreview();
                        arm.buyerStart=buyerState();
                        auto paid=buyer->previewCounterpointOpening(projection);paid.remove("reusedEquivalent");
                        if(!paid["supported"].toBool() || arm.buyerStart!=startingBuyer || buyerState()!=startingBuyer)return {};
                        QJsonArray modeled;
                        for(const auto &v:paid["orders"].toArray()){
                            const auto o=v.toObject();modeled.append(QJsonObject{{"factory",QJsonArray{o["x"],o["y"]}},
                                {"type",o["type"]},{"price",qint64(o["price"].toInteger())*1000}});
                        }
                        arm.receipt=earlier;
                        arm.receipt["economicObservation"]=economicObservation;
                        arm.receipt["purchaseBoundary"]=QJsonObject{{"openAffordableSites",available},{"spendableFunds",qint64(purchaseWindow.treasury)},
                            {"affordableBuildSlots",count},{"modeledOpening",modeled},{"openingSource",count?"COUNTERPOINT_PREVIEW":"NO_FUNDED_SLOT"},{"buyerPreview",paid}};
                        arm.receipt["physicalTeam"]=bindPhysicalTeam(arm.team,fire,arm.receipt);
                        // Avoid inferring vacancies from changed actors alone. Include
                        // stock that has already acted, enemies, and nonacting teammates.
                        std::map<Cell,int> occupants;
                        for(const auto &[i,t]:at.defenders)occupants[{t.x,t.y}]=local->units[i].engineUnitId;
                        for(const auto &u:local->units){
                            const Cell site{u.tile.x,u.tile.y};
                            if(u.present && u.enemy && at.occupied.contains(site) && !occupants.contains(site))occupants[site]=u.engineUnitId;
                        }
                        for(const auto &v:projection["properties"].toArray()){
                            const auto property=v.toObject();
                            const Cell site{property["x"].toInt(),property["y"].toInt()};
                            auto *b=map.getTerrain(site.first,site.second)->getBuilding();
                            if(!b || !b->isProductionBuilding())continue;
                            if(at.occupied.contains(site) && !occupants.contains(site))return {};
                            const auto body=occupants.find(site);
                            const int id=body==occupants.end()?NO_UNIT:body->second;
                            arm.board.sites.emplace(site,OpenPurchaseSite{b->getBuildingID()=="FACTORY" &&
                                property["owner"].toInt(-1)==knowledge.observerId(),id,id});
                        }
                        arm.board.complete=true;
                        return arm;
                    };
                    auto left=replayOpening(effectiveChoices,{},base);
                    if(left && (!masterPlanEquivalent(problem,reference.replay.plan,reference.replay.ordering.order,
                        left->team.replay.plan,left->team.replay.ordering.order) || reference.afterOwnActions!=left->team.afterOwnActions))left.reset();
                    int examined=0;
                    for(const auto &opt:options){
                        if(!left || work<3*replayTariff)break;
                        const auto &[v,first,second]=opt.key;const CombatFireOrder order{first,second};
                        const auto earlier=opt.trial>=0 && opt.trial<trials.size()?trials[opt.trial].toObject():QJsonObject{};
                        auto right=replayOpening(v,order,earlier);++examined;
                        if(!right)continue;
                        if(std::ranges::equal(v,sourceRivalChoices) && first==NO_UNIT && second==NO_UNIT &&
                           !std::ranges::equal(right->team.replay.ordering.order,sourceRivalOrder))continue;
                        OpenPurchaseBoard sites=left->board;
                        if(sites.sites.size()!=right->board.sites.size())continue;
                        for(auto &[cell,s]:sites.sites){
                            const auto i=right->board.sites.find(cell);
                            if(i==right->board.sites.end()){sites.complete=false;break;}
                            s.factory=s.factory && i->second.factory;
                            s.challengerOccupant=i->second.incumbentOccupant;
                        }
                        sites.buyerStart={left->buyerStart,right->buyerStart};
                        QJsonObject comparison;
                        if(const auto it=openPurchaseComparisons.find(opt.key);it!=openPurchaseComparisons.end()){
                            const auto &previous=it->second;const auto bound=previous.value("bindings").toArray();
                            if(bound.size()!=2)continue;
                            const auto sameOrders=[](const QJsonObject &old,const QJsonObject &fresh){
                                for(const auto key:{"orders","executionOrder","fireOrder","day","player","planSequence","horizon"})
                                    if(old.value(key)!=fresh.value(key))return false;
                                const auto prior=old.value("purchaseBoundary").toObject();
                                const auto buyer=prior.value("buyerPreview").toObject();
                                const auto now=fresh.value("purchaseBoundary").toObject()["buyerPreview"].toObject();
                                if(prior["openingSource"]=="COUNTERPOINT_PREVIEW" && buyer["supported"].toBool())
                                    for(const auto key:{"orders","seed","drawCounter","remainingFunds","preparedDrawCounter"})
                                        if(buyer.value(key)!=now.value(key))return false;
                                return true;
                            };
                            if(!sameOrders(bound[0].toObject(),left->receipt["physicalTeam"].toObject()) ||
                               !sameOrders(bound[1].toObject(),right->receipt["physicalTeam"].toObject()))continue;
                            comparison=previous;
                        }else{
                            // Submit the two actual teams to the existing comparator
                            // with all retained formula evidence. An absent policy
                            // score leaves the comparison open.
                            auto decision=comparePair(left->receipt,right->receipt,false);
                            if(decision.result()!=PairPreference::Open)continue;
                            comparison=decision.record("incumbent","challenger");
                        }
                        comparison["bindings"]=QJsonArray{left->receipt["physicalTeam"],right->receipt["physicalTeam"]};
                        OpenPurchaseVetoes guard;
                        QJsonObject incumbentHq,challengerHq;std::vector<int> incumbentGuards,challengerGuards;bool home0=false,home1=false;
                        const auto l=headquartersRank(hq,*local,knowledge,problem,field,left->team,0,incumbentHq,incumbentGuards,home0);
                        const auto r=headquartersRank(hq,*local,knowledge,problem,field,right->team,0,challengerHq,challengerGuards,home1);
                        guard.checked=true;
                        guard.incumbentDefence=settledPurchasePairs.contains(opt.key);
                        guard.incumbentHigherPriority=l && r && std::get<0>(*l)==TerminalClass::ForcedWin &&
                            std::get<0>(*r)!=TerminalClass::ForcedWin;
                        guard.challengerHeadquarters=!right->receipt.value("openingLossProof").toObject().isEmpty() ||
                            (r && std::get<0>(*r)==TerminalClass::ForcedLoss);
                        const auto preferred=evaluateOpenPurchasePreference(left->receipt,right->receipt,comparison,sites,guard);
                        if(!preferred.eligible)continue;
                        // Reconstruct both physical orders and buyers immediately
                        // before handing the unchanged ordered plan to the actor.
                        auto referenceAgain=replayOpening(effectiveChoices,{},base);
                        auto selectedAgain=replayOpening(v,order,earlier);
                        if(!referenceAgain || !selectedAgain ||
                           referenceAgain->receipt["physicalTeam"]!=left->receipt["physicalTeam"] ||
                           selectedAgain->receipt["physicalTeam"]!=right->receipt["physicalTeam"] ||
                           selectedAgain->team.afterOwnActions!=right->team.afterOwnActions ||
                           selectedAgain->team.replay.ordering.order!=right->team.replay.ordering.order)continue;
                        const auto mismatch=std::mismatch(v.begin(),v.end(),effectiveChoices.begin());
                        const int changed=int(mismatch.first-v.begin());
                        if(changed>=int(v.size()) || !publish(*handoff,selectedAgain->team,changed,sourceReference,assignment))continue;
                        QJsonArray released,purchased;
                        for(const auto &v:preferred.releasedPurchases){
                            const auto b=v.toObject();released.append(QJsonArray{b["x"],b["y"]});
                        }
                        for(const auto &v:preferred.challengerPurchases){
                            const auto b=v.toObject();purchased.append(QJsonArray{b["x"],b["y"]});
                        }
                        result["fundedClearPreference"]=QJsonObject{{"status","FUNDED_CLEAR_ON_OPEN"},
                            {"reason",preferred.reason},{"comparison",comparison},{"trial",opt.trial},{"nativeExecutionPending",true},
                            {"referencePhysicalTeam",left->receipt["physicalTeam"]},{"selectedPhysicalTeam",right->receipt["physicalTeam"]},
                            {"referenceBuyerStartSHA256",QString::fromLatin1(left->buyerStart)},{"selectedBuyerStartSHA256",QString::fromLatin1(right->buyerStart)},
                            {"releasedSites",released},{"purchasedSites",purchased},{"releasedPurchases",preferred.releasedPurchases},
                            {"incumbentPurchases",preferred.incumbentPurchases},{"challengerPurchases",preferred.challengerPurchases}};
                        result["installed"]=true;result["status"]="FUNDED_CLEAR_ON_OPEN";result["economicComparisonStatus"]="UNRESOLVED";
                        result.remove("selectedPolicyTotal");result.remove("selectedPairArm");result.remove("selectedPairDecision");
                        result["selectedTrial"]=opt.trial;result["selectedChanges"]=changedFromSource(v);
                        QJsonArray orders;for(int c:v)orders.append(c);result["choices"]=orders;
                        result["executionOrder"]=right->receipt["physicalTeam"].toObject()["executionOrder"];
                        if(first!=NO_UNIT)result["selectedFireOrder"]=QJsonArray{first,second};
                        storeEconomicOrders(QJsonObject{},selectedAgain->team);
                        openingAlternativeInstalled=true;break;
                    }
                    StockProgressArms sharedArmBounds;
                    StockProgressPricing sharedPricing;
                    bool sharedPricingPrepared=false;
                    if(!openingAlternativeInstalled && left && !progress.measures.isEmpty())
                    {
                        QJsonArray marginals;
                        auto &armBounds=sharedArmBounds;
                        bool unresolvedStock=false;
                        // Share refinement across all live physical alternatives.
                        // Retain the remaining budget for native bindings and
                        // two complete physical replays before installation.
                        std::int64_t stockBalance=work/3;
                        auto &pricing=sharedPricing;
                        const auto beforeStockSetup=work;
                        const bool prepared=pricing.prepare(field,*progressInput,stockBalance,work);
                        sharedPricingPrepared=prepared;
                        const auto setupWork=beforeStockSetup-work;
                        stockBalance=prepared?stockBalance-setupWork:0;
                        result["stockProgressSetup"]=QJsonObject{{"prepared",prepared},{"work",qint64(setupWork)}};
                        const auto individualSteps=[&] {
                            StockProgressPlan selected{effectiveChoices,{}};
                            std::vector<StockProgressPlan> improvements,individual;
                            QJsonArray pending,escapes,unresolved;
                            for(const auto &option:progress.measures){
                                if(option.toObject()["source"]=="CAPTURE_CLEAR_OF_ENEMY_FIRE")escapes.append(option);
                                else pending.append(option);
                            }
                            for(int pass=0;pass<3;++pass){
                            if(pass==1){
                                if(selected.measures.isEmpty())continue;
                                pending=unresolved;
                            }else if(pass==2)pending=escapes;
                            if(stockBalance<=0)break;
                            int witnessedLeft=0;
                            for(const auto &option:pending)witnessedLeft+=!option.toObject().contains("source");
                            int additionalLeft=int(pending.size())-witnessedLeft;
                            for(const auto &entry:pending)
                            {
                                const auto measure=entry.toObject();
                                const int slot=measure["slot"].toInt(-1);
                                auto next=selected.choices;
                                next[slot]=measure["candidate"].toInt(-1);
                                const auto common=armBounds.find(selected.choices);
                                const int pendingCommon=common==armBounds.end() || common->second.lower<common->second.upper;
                                // Preserve the allowance for each original witnessed
                                // hearing. Extra native options use its unspent work.
                                auto &remaining=measure.contains("source")?additionalLeft:witnessedLeft;
                                const auto offer=(stockBalance/(remaining+pendingCommon))*(1+pendingCommon);
                                --remaining;
                                if(next==selected.choices)continue;
                                const auto start=work;
                                const auto proof=priceStockProgress(problem,pricing,selected.choices,next,armBounds,offer,work);
                                stockBalance-=start-work;
                                marginals.append(QJsonObject{{"unit",measure["unit"]},{"candidate",next[slot]},{"revisitedAfterTeamChange",pass==1},
                                    {"offeredWork",qint64(offer)},{"usedWork",qint64(start-work)},{"comparison",proof}});
                                if(proof["status"]!="STRICT_STOCK_IMPROVEMENT" && proof["status"]!="NO_STRICT_STOCK_MARGIN"){
                                    unresolvedStock=true;
                                    if(pass==0)unresolved.append(measure);
                                }
                                if(selected.choices!=effectiveChoices || proof["status"]!="NO_STRICT_STOCK_MARGIN"){
                                    auto alone=effectiveChoices;alone[slot]=measure["candidate"].toInt(-1);
                                    individual.push_back({std::move(alone),QJsonArray{measure}});
                                }
                                if(proof["status"]=="STRICT_STOCK_IMPROVEMENT"){
                                    for(int i=0;i<selected.measures.size();++i)if(selected.measures[i].toObject()["slot"]==slot){
                                        selected.measures.removeAt(i);break;
                                    }
                                    selected.choices=std::move(next);selected.measures.append(measure);
                                    improvements.push_back(selected);
                                }
                            }
                            }
                            // A downstream actor can still have a positive feasible
                            // single change if a larger tentative team loses a
                            // purchase or tactical obligation. Every fallback must
                            // receive its own direct comparator and native replays.
                            std::set<std::vector<int>> seen;
                            for(const auto &step:improvements)seen.insert(step.choices);
                            std::erase_if(individual,[&](const auto &step){return !seen.insert(step.choices).second;});
                            improvements.insert(improvements.begin(),individual.begin(),individual.end());
                            if(improvements.empty())improvements.push_back(selected);
                            return improvements;
                        };
                        QJsonArray declined;
                        std::set<int> boundActors;
                        const auto initialAssignment=assignment;
                        auto committedChoices=effectiveChoices;
                        std::optional<Opening> committedOpening;
                        for(bool compareWholeProposal:{false,true})
                        {
                            if(compareWholeProposal && (!prepared || unusedAdmissionWork<=0))continue;
                            const auto &anchorChoices=compareWholeProposal?committedChoices:effectiveChoices;
                            const Opening *anchor=compareWholeProposal && committedOpening?&*committedOpening:&*left;
                            const auto savedWork=work;
                            const auto transfer=compareWholeProposal?std::exchange(unusedAdmissionWork,0):0;
                            if(compareWholeProposal)work=transfer;
                            const auto restoreWork=qScopeGuard([&]{
                                if(compareWholeProposal){
                                    result["stockAdmissionTransfer"]=QJsonObject{{"available",qint64(transfer)},
                                        {"used",qint64(transfer-work)},{"unused",qint64(work)}};
                                    work=savedWork;
                                }
                            });
                            bool acceptedHere=false;
                            std::vector<StockProgressPlan> improvements;
                            if(compareWholeProposal){
                                // Keep every already accepted improvement, then compare the
                                // remaining compatible stock orders against that actual team.
                                StockProgressPlan team{anchorChoices,{}};
                                for(const auto &entry:progress.measures){
                                    const auto measure=entry.toObject();
                                    if(!measure["stockProposed"].toBool() || measure["source"]=="CAPTURE_CLEAR_OF_ENEMY_FIRE")continue;
                                    const int slot=measure["slot"].toInt();
                                    const int choice=measure["candidate"].toInt();
                                    if(anchorChoices[slot]!=effectiveChoices[slot] || team.choices[slot]==choice)continue;
                                    team.choices[slot]=choice;
                                    team.measures.append(measure);
                                }
                                if(team.measures.isEmpty())continue;
                                const PlanKey key{team.choices,NO_UNIT,NO_UNIT};
                                if(settledPurchasePairs.contains(key))continue;
                                // Use the ordinary split between economic pricing and
                                // physical verification, funded entirely by the closed queue.
                                const auto offer=work/3;
                                const auto start=work;
                                const auto proof=priceStockProgress(problem,pricing,anchorChoices,team.choices,armBounds,offer,work);
                                result["jointStockProposal"]=QJsonObject{{"measures",team.measures},{"offeredWork",qint64(offer)},
                                    {"usedWork",qint64(start-work)},{"comparison",proof}};
                                if(proof["status"]!="STRICT_STOCK_IMPROVEMENT")continue;
                                improvements.push_back(std::move(team));
                            }else{
                                stockBalance=std::min(stockBalance,work);
                                improvements=individualSteps();
                            }
                            for(auto step=improvements.rbegin();step!=improvements.rend();++step)
                            {
                                const auto &selected=*step;
                                const auto &v=selected.choices;
                                const PlanKey key{v,NO_UNIT,NO_UNIT};
                                const auto prior=attempted.find(key);
                                const int index=prior!=attempted.end()?prior->second:-1;
                                const auto previous=index>=0?trials[index].toObject():QJsonObject{};
                                QJsonObject hearing{{"requested",progress.measures},{"admitted",selected.measures},
                                    {"marginals",marginals},{"trial",index},{"status","OPEN"}};
                                const auto consider=[&](){
                                    if(selected.measures.isEmpty())return unresolvedStock?QStringLiteral("STOCK_INTERVALS_OPEN"):
                                        QStringLiteral("NO_STRICT_STOCK_MARGIN");
                                    if(settledPurchasePairs.contains(key) || previous["status"]=="DOMINATED_BY_REPLY" ||
                                       previous["status"]=="UNREPLAYABLE" || !previous["openingLossProof"].toObject().isEmpty())
                                        return QStringLiteral("PRIOR_TACTICAL_DECISION");
                                    const auto right=replayOpening(v,{},previous);
                                    int escapeSlot=-1;
                                    for(const auto &entry:selected.measures)if(entry.toObject()["source"]=="CAPTURE_CLEAR_OF_ENEMY_FIRE"){
                                        if(escapeSlot>=0)return QStringLiteral("MULTIPLE_ESCAPES_NEED_A_JOINT_BOUND");
                                        escapeSlot=entry.toObject()["slot"].toInt(-1);
                                    }
                                    const int escapeId=escapeSlot<0?NO_UNIT:problem.actors()[escapeSlot].engineUnitId;
                                    if(!right || anchor->team.replay.ordering.order!=right->team.replay.ordering.order)
                                        return QStringLiteral("OPENING_ORDER_CHANGED");
                                    if(escapeSlot<0){
                                        if(anchor->team.shots!=right->team.shots || anchor->team.exactNonStock>right->team.exactNonStock)
                                            return QStringLiteral("OPENING_MATERIAL_OR_ORDER_CHANGED");
                                    }else if(!isolatedStockEscapeExchange(anchor->team,right->team,escapeId))
                                        return QStringLiteral("ESCAPE_CHANGES_OTHER_COMBAT");
                                    const auto lb=anchor->receipt["purchaseBoundary"].toObject(),rb=right->receipt["purchaseBoundary"].toObject();
                                    if(lb["openAffordableSites"]!=rb["openAffordableSites"] || lb["spendableFunds"]!=rb["spendableFunds"] ||
                                       lb["modeledOpening"]!=rb["modeledOpening"] || lb["buyerPreview"].toObject()["orders"]!=rb["buyerPreview"].toObject()["orders"] ||
                                       anchor->buyerStart!=right->buyerStart)return QStringLiteral("BUYER_OR_FUNDED_SITES_CHANGED");
                                    if(!std::all_of(selected.measures.constBegin(),selected.measures.constEnd(),[&](const auto &entry){
                                        return boundActors.contains(entry.toObject()["unit"].toInt(-1));})){
                                        const auto scope=stockProgressOpeningScope(map,knowledge,selected,lb["modeledOpening"].toArray(),work);
                                        if(!scope.isEmpty())return scope;
                                        // The rule, buyer and initial board are identical.
                                        // Service and native unit hooks are actor-local.
                                        for(const auto &entry:selected.measures)boundActors.insert(entry.toObject()["unit"].toInt(-1));
                                    }
                                    MilliFunds lostOpportunity=0;
                                    if(escapeSlot>=0){
                                        const auto &actor=problem.actors()[escapeSlot];
                                        const auto from=actor.candidates[anchorChoices[escapeSlot]].destination;
                                        const auto to=actor.candidates[v[escapeSlot]].destination;
                                        const auto boundary=purchaseBoundary(problem,*local,right->team,purchaseWindow.unitSlots);
                                        const auto capture=stockEscapeBound(*local,knowledge,progressSites,escapeId,from,to,boundary.defenders,
                                            rb["modeledOpening"].toArray(),base["captureRouteProofs"].toArray(),field.horizonTurns(),work);
                                        hearing["escapeBound"]=capture;
                                        if(capture["status"]!="BOUNDED_E0_ESCAPE")return capture["reason"].toString();
                                        lostOpportunity=capture["lostF1Upper"].toInteger(-1);
                                        if(lostOpportunity<0)return QStringLiteral("INVALID_ESCAPE_UPPER");
                                        MilliFunds known=anchor->team.exactNonStock;
                                        if(!RelativeStockDetail::checkedSubtract(known,right->team.exactNonStock))
                                            return QStringLiteral("ESCAPE_MATERIAL_ARITHMETIC");
                                        lostOpportunity=std::max(lostOpportunity,known);
                                    }
                                    QJsonObject h0,h1;std::vector<int> g0,g1;bool home0=false,home1=false;
                                    const auto a=headquartersRank(hq,*local,knowledge,problem,field,anchor->team,0,h0,g0,home0);
                                    const auto b=headquartersRank(hq,*local,knowledge,problem,field,right->team,0,h1,g1,home1);
                                    if(!a || !b || *a!=*b || home0!=home1 || g0!=g1 || h0["hqStatus"]!=h1["hqStatus"])
                                        return QStringLiteral("HEADQUARTERS_DUTY_CHANGED");
                                    const auto gap=base["unpricedReplyPolicy"].toString();
                                    const bool needsFullReplyCheck=std::ranges::any_of(selected.measures,[](const auto &entry){
                                        const auto source=entry.toObject().value(QStringLiteral("source"));
                                        return source=="COMPLETE_NEUTRAL_CAPTURE_IN_PLACE" || source=="COMPETING_STOCK_ROUTE";
                                    });
                                    if(compareWholeProposal || needsFullReplyCheck || gap.startsWith("TARGET_UNIT_") || gap.startsWith("BYPASS_UNIT_"))
                                    {
                                        if(escapeSlot>=0)return QStringLiteral("OPENING_ESCAPE_FAMILY_UNBOUND");
                                        const auto quote=[&](std::span<const int> team){
                                            return evaluateReplyPassPlan(problem,team,actingBoard);
                                        };
                                        const auto equalHq=[&](const ReplyPassEvaluation &a,const ReplyPassEvaluation &b,bool &needsCover){
                                            QJsonObject oldHq,newHq;
                                            std::vector<int> oldGuards,newGuards;
                                            bool oldCover=false,newCover=false;
                                            const auto x=headquartersRank(hq,*local,knowledge,problem,field,a,0,oldHq,oldGuards,oldCover);
                                            const auto y=headquartersRank(hq,*local,knowledge,problem,field,b,0,newHq,newGuards,newCover);
                                            needsCover=oldCover || newCover;
                                            return x && y && *x==*y && oldCover==newCover && oldGuards==newGuards &&
                                                   oldHq["hqStatus"]==newHq["hqStatus"] && oldHq["hqApproaches"]==newHq["hqApproaches"];
                                        };
                                        QJsonObject detail;
                                        const auto why=checkStockOpeningReplies(problem,knowledge,*local,anchorChoices,v,
                                            rb["modeledOpening"].toArray(),hq.home,replayTariff,work,quote,equalHq,detail);
                                        hearing["allOpeningReplies"]=detail;
                                        if(!why.isEmpty())return why;
                                    }
                                    else if(!base["completeOpeningReplyMenu"].toBool())
                                    {
                                        const auto policy=base["unpricedReplyPolicy"].toString();bool ok=false;
                                        const int hold=policy.mid(QStringLiteral("WAIT_ON_PROPERTY_").size()).toInt(&ok);
                                        if(!ok || hold<0 || policy!="WAIT_ON_PROPERTY_"+QString::number(hold) ||
                                           work<2*replayTariff)return QStringLiteral("OPENING_REPLY_UNAVAILABLE");
                                        local->policy=TacticalReply::WaitOnProperty;local->replyTarget=hold;
                                        const auto restore=qScopeGuard([&]{local->policy=TacticalReply::Economic;local->replyTarget=NO_UNIT;});
                                        work-=replayTariff;
                                        const auto oldWait=evaluateReplyPassPlan(problem,anchorChoices,actingBoard);
                                        if(work<replayTariff)return QStringLiteral("OPENING_REPLY_WORK_LIMIT");
                                        work-=replayTariff;
                                        const auto newWait=evaluateReplyPassPlan(problem,v,actingBoard);
                                        if(escapeSlot>=0){
                                            if(oldWait.enemyWaiting<0 || oldWait.enemyWaiting!=newWait.enemyWaiting ||
                                               local->units[oldWait.enemyWaiting].engineUnitId!=hold ||
                                               !isolatedStockEscapeExchange(oldWait,newWait,escapeId))
                                                return QStringLiteral("PROPERTY_WAIT_CHANGES_OTHER_COMBAT");
                                            MilliFunds known=oldWait.exactNonStock;
                                            if(!RelativeStockDetail::checkedSubtract(known,newWait.exactNonStock))
                                                return QStringLiteral("ESCAPE_MATERIAL_ARITHMETIC");
                                            lostOpportunity=std::max(lostOpportunity,known);
                                        }else{
                                            if(!oldWait.valued() || !newWait.valued() || oldWait.enemyWaiting<0 ||
                                               oldWait.enemyWaiting!=newWait.enemyWaiting || local->units[oldWait.enemyWaiting].engineUnitId!=hold ||
                                               oldWait.shots!=newWait.shots || oldWait.enemyReplyHp!=newWait.enemyReplyHp ||
                                               oldWait.realAfterReply!=newWait.realAfterReply || oldWait.exactNonStock!=newWait.exactNonStock ||
                                               oldWait.combatLocations.size()!=local->units.size() || newWait.combatLocations.size()!=local->units.size() ||
                                               oldWait.replay.ordering.order!=newWait.replay.ordering.order ||
                                               oldWait.actors.size()!=newWait.actors.size())return QStringLiteral("PROPERTY_WAIT_CHANGES_EXCHANGE");
                                            auto projected=oldWait.combatLocations;
                                            for(const auto &event:selected.measures){
                                                const int slot=event.toObject()["slot"].toInt();
                                                const auto &member=problem.actors()[slot];
                                                if(projected[member.knowledgeUnitIndex]!=member.candidates[anchorChoices[slot]].destination)
                                                    return QStringLiteral("PROPERTY_WAIT_DISPLACES_PROGRESS_ACTOR");
                                                projected[member.knowledgeUnitIndex]=member.candidates[v[slot]].destination;
                                            }
                                            if(projected!=newWait.combatLocations)return QStringLiteral("PROPERTY_WAIT_DISPLACES_ANOTHER_ACTOR");
                                            for(std::size_t i=0;i<oldWait.actors.size();++i){
                                                const auto &old=oldWait.actors[i],&next=newWait.actors[i];
                                                if(old.actorSlot!=next.actorSlot || old.stepsAfterOwnActions!=next.stepsAfterOwnActions ||
                                                   old.stepsAfterReply!=next.stepsAfterReply || old.destroyedBy!=next.destroyedBy ||
                                                   old.survivorAmmo!=next.survivorAmmo)return QStringLiteral("PROPERTY_WAIT_CHANGES_RESOURCES");
                                            }
                                        }
                                        QJsonObject oldHq,newHq;std::vector<int> oldGuards,newGuards;bool oldCover=false,newCover=false;
                                        const auto oldRank=headquartersRank(hq,*local,knowledge,problem,field,oldWait,0,oldHq,oldGuards,oldCover);
                                        const auto newRank=headquartersRank(hq,*local,knowledge,problem,field,newWait,0,newHq,newGuards,newCover);
                                        if(!oldRank || !newRank || *oldRank!=*newRank || oldCover!=newCover ||
                                           oldGuards!=newGuards || oldHq["hqStatus"]!=newHq["hqStatus"])
                                            return QStringLiteral("PROPERTY_WAIT_CHANGES_HEADQUARTERS");
                                        hearing["commonPropertyWait"]=QJsonObject{{"policy",policy},{"unit",hold},
                                            {"shots",int(oldWait.shots.size())},{"headquarters",oldHq["hqStatus"]},
                                            {"standingGuard",base["hqGuard"]},{"fullContinuationPriced",false}};
                                    }
                                    auto proof=priceStockProgress(problem,pricing,anchorChoices,v,armBounds,work,work);
                                    hearing["stockComparison"]=proof;
                                    if(proof["status"]!="STRICT_STOCK_IMPROVEMENT")return QStringLiteral("STOCK_COMPARISON_OPEN");
                                    if(escapeSlot>=0){
                                        auto otherOrders=v;otherOrders[escapeSlot]=anchorChoices[escapeSlot];
                                        const auto marginal=priceStockProgress(problem,pricing,otherOrders,v,armBounds,work,work);
                                        hearing["escapeStockMarginal"]=marginal;
                                        hearing["escapeOpportunityUpper"]=qint64(lostOpportunity);
                                        if(marginal["status"]!="STRICT_STOCK_IMPROVEMENT" ||
                                           marginal["margin"].toInteger()<=lostOpportunity || proof["margin"].toInteger()<=lostOpportunity)
                                            return QStringLiteral("ESCAPE_GAIN_DOES_NOT_COVER_FOREGONE_SHOT");
                                    }
                                    const auto oldAgain=replayOpening(anchorChoices,{},anchor->receipt);
                                    auto newAgain=replayOpening(v,{},previous);
                                    if(!oldAgain || !newAgain || oldAgain->receipt["physicalTeam"]!=anchor->receipt["physicalTeam"] ||
                                       newAgain->receipt["physicalTeam"]!=right->receipt["physicalTeam"] || oldAgain->team.shots!=anchor->team.shots ||
                                       newAgain->team.shots!=right->team.shots || newAgain->team.afterOwnActions!=right->team.afterOwnActions ||
                                       newAgain->team.replay.ordering.order!=right->team.replay.ordering.order)
                                        return QStringLiteral("NATIVE_OPENING_RECHECK_CHANGED");
                                    const auto mismatch=std::mismatch(v.begin(),v.end(),effectiveChoices.begin());
                                    const int changed=static_cast<int>(mismatch.first-v.begin());
                                    auto nextAssignment=initialAssignment;
                                    if(changed>=int(v.size()) || !publish(*handoff,newAgain->team,changed,sourceReference,nextAssignment))
                                        return QStringLiteral("PUBLICATION_REFUSED");
                                    assignment=std::move(nextAssignment);
                                    hearing["referencePhysicalTeam"]=oldAgain->receipt["physicalTeam"];
                                    hearing["selectedPhysicalTeam"]=newAgain->receipt["physicalTeam"];
                                    hearing["referenceOpeningPurchases"]=lb["modeledOpening"];
                                    hearing["candidateOpeningPurchases"]=rb["modeledOpening"];
                                    result["installed"]=true;result["status"]="STOCK_PROGRESS_ON_OPEN";
                                    result["economicComparisonStatus"]="UNRESOLVED";
                                    result.remove("selectedPolicyTotal");result.remove("selectedPairArm");result.remove("selectedPairDecision");
                                    result["selectedTrial"]=index;result["selectedChanges"]=changedFromSource(v);
                                    QJsonArray chosen;for(int option:v)chosen.append(option);result["choices"]=chosen;
                                    result["executionOrder"]=newAgain->receipt["physicalTeam"].toObject()["executionOrder"];
                                    storeEconomicOrders(QJsonObject{},newAgain->team);
                                    openingAlternativeInstalled=true;
                                    acceptedHere=true;
                                    if(!compareWholeProposal){
                                        committedChoices=v;
                                        committedOpening=std::move(newAgain);
                                    }
                                    return QStringLiteral("STRICT_STOCK_IMPROVEMENT_ON_OPEN");
                                };
                                hearing["status"]=consider();
                                if(!declined.isEmpty())hearing["largerTeamsDeclined"]=declined;
                                if(compareWholeProposal)result["jointStockHearing"]=hearing;
                                if(compareWholeProposal && acceptedHere && committedOpening)
                                    result["priorStockProgressHearing"]=result.value("stockProgressHearing");
                                if(acceptedHere || !openingAlternativeInstalled)result["stockProgressHearing"]=hearing;
                                if(acceptedHere)break;
                                QJsonObject refusal{{"measures",selected.measures},{"status",hearing["status"]}};
                                if(hearing.contains("escapeBound"))refusal["escapeBound"]=hearing["escapeBound"];
                                if(hearing.contains("escapeStockMarginal"))refusal["escapeStockMarginal"]=hearing["escapeStockMarginal"];
                                declined.append(refusal);
                            }
                        }
                    }
                    if(!openFactoryWaitSlots.empty()){
                        result["fundedPreferenceCandidatesExamined"]=examined;
                        result["fundedPreferenceReplayWork"]=qint64(granted-work);
                    }else if(!progress.measures.isEmpty()) result["stockProgressReplayWork"]=qint64(granted-work);
                    const auto priorFactoryDecision=result["selectedPairDecision"].toObject()["status"].toString();
                    if(factoryProgrammeSurface && !winner && (priorFactoryDecision.isEmpty() || priorFactoryDecision=="OPEN" || priorFactoryDecision=="UNRESOLVED") &&
                       (finalComparisonUnresolved || unpriced>0 || base["priceStatus"]!="WITNESSED"))
                    {
                        const auto startWork=work;
                        auto anchor=effectiveChoices;
                        if(openingAlternativeInstalled){
                            const auto options=result["choices"].toArray();
                            if(options.size()==int(anchor.size()))for(int i=0;i<options.size();++i)anchor[i]=options[i].toInt();
                        }
                        const auto priorRecord=[&](const std::vector<int> &v){
                            if(v==effectiveChoices)return base;
                            const auto it=attempted.find(PlanKey{v,NO_UNIT,NO_UNIT});
                            return it!=attempted.end()?trials[it->second].toObject():QJsonObject{};
                        };
                        const auto priced=priorRecord(anchor);
                        const auto dueProgram=readDatedOrders(variables,QStringLiteral("COORDINATED_FACTORY_PROGRAM_V1"),identity.day,identity.playerId);
                        const auto make=[&](){
                            auto live=FactoryPrograms::nativeProgramBoard(map,knowledge,*local,progressSites,field,
                                priced["productionOrders"].isArray()?priced:base,work);
                            std::vector<std::vector<int>> seeds{anchor};
                            if(handoff->stockProposal && handoff->stockProposal->choices!=anchor)seeds.push_back(handoff->stockProposal->choices);
                            std::vector<std::vector<int>> recordedTeams;
                            for(const auto &[key,index]:attempted){
                                const auto &[orders,first,second]=key;
                                if(first==NO_UNIT && second==NO_UNIT && index>=0 && trials[index].toObject()["status"]!="UNREPLAYABLE")
                                    recordedTeams.push_back(orders);
                            }
                            const auto tariff=[&](const QJsonObject &projection){
                                return 256LL+48LL*projection["units"].toArray().size()+32LL*projection["properties"].toArray().size();
                            };
                            std::map<QByteArray,QJsonObject> buyerQuotes;
                            const auto buy=[&](const QJsonObject &projection,const QString &policy,std::int64_t &allowance){
                                if(!FactoryPrograms::debit(allowance,tariff(projection)))return QJsonObject{{"reason","FACTORY_BUYER_WORK_LIMIT"}};
                                const auto key=QJsonDocument(QJsonArray{policy,projection}).toJson(QJsonDocument::Compact);
                                if(const auto it=buyerQuotes.find(key);it!=buyerQuotes.end())return it->second;
                                if(map.getMapHash()!=mapAtStart || buyerState()!=startingBuyer)return QJsonObject{{"reason","LIVE_BUYER_OR_BOARD_CHANGED"}};
                                auto result=quoteFactoryProgramBuyer(*buyer,projection,policy);
                                if(buyerState()!=startingBuyer)return QJsonObject{{"reason","LIVE_BUYER_CHANGED_AFTER_PROGRAM_PREVIEW"}};
                                if(buyerQuotes.size()<512)buyerQuotes.emplace(key,result);
                                return result;
                            };
                            const auto prepare=[&](const std::vector<int> &v,QString &reason)->std::optional<FactoryPrograms::Opening>{
                                const PlanKey key{v,NO_UNIT,NO_UNIT};const auto record=priorRecord(v);
                                if(v!=anchor && (settledPurchasePairs.contains(key) || record["status"]=="DOMINATED_BY_REPLY" ||
                                   record["status"]=="UNREPLAYABLE" || !record["openingLossProof"].toObject().isEmpty())){
                                    reason="CONTRARY_RANKED_OR_OPENING_PROOF";return {};
                                }
                                const auto fresh=replayOpening(v,{},record);
                                if(!fresh){reason="COMPLETE_NATIVE_OPENING_OR_BUYER_UNAVAILABLE";return {};}
                                auto physical=fresh->receipt["physicalTeam"].toObject();
                                if(v==anchor && openingAlternativeInstalled){
                                    auto previous=result["stockProgressHearing"].toObject()["selectedPhysicalTeam"].toObject();
                                    if(previous.isEmpty())previous=result["fundedClearPreference"].toObject()["selectedPhysicalTeam"].toObject();
                                    if(previous.isEmpty() || previous!=physical){reason="INSTALLED_INCUMBENT_BINDING_CHANGED";return {};}
                                }
                                const auto at=purchaseBoundary(problem,*local,fresh->team,purchaseWindow.unitSlots);
                                auto projection=openingBuyerProjection(map,knowledge,*local,problem,field,fresh->team,purchaseWindow.treasury,at.available);
                                projection["opponentFunds"]=qint64(live.initial.funds[1]/MILLI_FUNDS_PER_FUND);
                                projection["projectedDay"]=identity.day;projection["player"]=identity.playerId;
                                projection["includeContinuationState"]=true;
                                const auto quote=buy(projection,QStringLiteral("matched-ordinary-economic-programme-v1"),work);
                                return FactoryPrograms::nativeProgramOpening(live,problem,fresh->team,physical,
                                    fresh->receipt["purchaseBoundary"].toObject()["buyerPreview"].toObject(),projection,quote,reason,work);
                            };
                            const auto safety=[&](const std::vector<int> &v,const auto &offer,const FactoryPrograms::Opening &old,
                                const FactoryPrograms::Opening &next){
                                QJsonArray duties=factoryOpeningGuardDuties(priorRecord(v));
                                for(const auto &x:factoryOpeningGuardDuties(priorRecord(offer.choices)))duties.append(x);
                                duties.append(FactoryPrograms::point(live.board.sites[offer.site].at));
                                return factoryOpeningSecurity(map,knowledge,*local,old.physicalTeam,next.physicalTeam,duties,work);
                            };
                            return FactoryPrograms::hearFactoryPrograms(problem,live,anchor,seeds,recordedTeams,dueProgram,prepare,buy,safety,work);
                        };
                        auto hearing=make();
                        hearing.receipt["maturedIntention"]=dueProgram;
                        if(hearing.selected){
                            const auto incumbentAgain=replayOpening(hearing.referenceChoices,{},priorRecord(hearing.referenceChoices));
                            const auto selectedAgain=replayOpening(hearing.choices,{},priorRecord(hearing.choices));
                            const auto selectedTeam=hearing.receipt["candidateOpening"].toObject();
                            if(!incumbentAgain || !selectedAgain || incumbentAgain->receipt["physicalTeam"]!=hearing.receipt["referenceOpening"] ||
                               selectedAgain->receipt["physicalTeam"]!=selectedTeam || map.getMapHash()!=mapAtStart || buyerState()!=startingBuyer)
                                hearing.receipt["status"]="NATIVE_ORDER_OR_BUYER_RECHECK_CHANGED";
                            else{
                                const auto &priorOrders=hearing.retained?sourceChoices:anchor;
                                const auto pos=std::mismatch(priorOrders.begin(),priorOrders.end(),hearing.choices.begin());
                                const int slot=int(pos.first-priorOrders.begin());
                                const bool alreadyPhysical=hearing.retained && (openingAlternativeInstalled || slot==int(priorOrders.size()));
                                if(!alreadyPhysical && (slot>=int(priorOrders.size()) || !publish(*handoff,selectedAgain->team,slot,sourceReference,assignment)))
                                    hearing.receipt["status"]="PUBLICATION_REFUSED";
                                else{
                                    result["installed"]=openingAlternativeInstalled || !alreadyPhysical;
                                    result["status"]=hearing.retained?"FACTORY_PROGRAM_RETAINED_ON_OPEN":"FACTORY_PROGRAM_ON_OPEN";
                                    result["economicComparisonStatus"]="UNRESOLVED";
                                    result.remove("selectedPolicyTotal");result.remove("selectedPairArm");result.remove("selectedPairDecision");
                                    result["selectedChanges"]=changedFromSource(hearing.choices);
                                    QJsonArray choices;for(int v:hearing.choices)choices.append(v);
                                    result["choices"]=choices;result["executionOrder"]=selectedTeam["executionOrder"];
                                    const auto it=attempted.find(PlanKey{hearing.choices,NO_UNIT,NO_UNIT});
                                    result["selectedTrial"]=it==attempted.end()?-1:it->second;
                                    hearing.receipt["selectedPhysicalTeam"]=selectedTeam;
                                    storeEconomicOrders(QJsonObject{},selectedAgain->team);
                                    writeDatedOrders(variables,QStringLiteral("COORDINATED_FACTORY_PROGRAM_V1"),
                                        identity.day,identity.playerId,dueProgram,hearing.next);
                                    openingAlternativeInstalled=true;
                                }
                            }
                        }
                        hearing.receipt["work"]=qint64(startWork-work);
                        result["factoryProgramHearing"]=hearing.receipt;
                    }
                    if(!winner && progressInput && !problem.actors().empty()){
                        auto teamBeforeRefinement=effectiveChoices;
                        if(openingAlternativeInstalled){
                            const auto selected=result["choices"].toArray();
                            if(selected.size()==int(teamBeforeRefinement.size()))
                                for(int i=0;i<selected.size();++i)teamBeforeRefinement[i]=selected[i].toInt();
                        }
                        const auto hqRankFor=[&](const ReplyPassEvaluation &eval){
                            QJsonObject hqObj;std::vector<int> guards;bool cover=false;
                            return headquartersRank(hq,*local,knowledge,problem,field,eval,0,hqObj,guards,cover);
                        };
                        const auto buildSlotsFor=[&](const ReplyPassEvaluation &eval){
                            const auto at=purchaseBoundary(problem,*local,eval,purchaseWindow.unitSlots);
                            return purchaseWindow.affordable([&](Cell c){return at.occupied.contains(c);},at.available);
                        };
                        const auto allowedByPrior=[&](const std::vector<int> &v){
                            const PlanKey key{v,NO_UNIT,NO_UNIT};
                            if(settledPurchasePairs.contains(key))return false;
                            const auto it=attempted.find(key);
                            if(it==attempted.end() || it->second<0 || it->second>=trials.size())return true;
                            const auto prev=trials[it->second].toObject();
                            return prev["status"]!="DOMINATED_BY_REPLY" && prev["status"]!="UNREPLAYABLE" &&
                                prev["openingLossProof"].toObject().isEmpty();
                        };
                        const auto scoreJson=[](const UnifiedTeamScore &s){
                            return QJsonObject{
                                {"valid",s.valid},
                                {"terminalClass",int(std::get<0>(s.worstHqRank))},
                                {"hqCaptureStartSafe",s.hqCaptureStartSafe},
                                {"worstEconomicValue",qint64(s.worstEconomicValue)},
                                {"stagingScore",qint64(s.stagingScore)},
                                {"realizedMaterial",qint64(s.realizedMaterial)},
                                {"postReplyStock",qint64(s.postReplyStock)},
                                {"enemyDenial",qint64(s.enemyDenial)},
                                {"ownCaptureRisk",qint64(s.ownCaptureRisk)},
                                {"factoryValue",qint64(s.factoryValue)},
                                {"affordableBuildSlots",s.affordableBuildSlots},
                                {"bindingPolicy",int(s.bindingPolicy)},
                                {"bindingTargetUnit",s.bindingTargetUnit}};
                        };
                        // Scale work budget with live actor count so 50v50 turns always complete Sweep 1 + pair passes.
                        const std::int64_t actorScaledWork=std::max<std::int64_t>(
                            UNIFIED_SEARCH_WORK_BUDGET,
                            static_cast<std::int64_t>(problem.actors().size())*UNIFIED_SEARCH_WORK_PER_ACTOR);
                        const std::int64_t unifiedWorkStart=std::max<std::int64_t>(
                            std::max<std::int64_t>(0,work-UNIFIED_FINAL_REPLAY_WORK_FLOOR),
                            std::max(actorScaledWork,replayTariff*UNIFIED_SEARCH_MAX_EVALS));
                        std::int64_t unifiedWork=unifiedWorkStart;
                        const auto stockProposed=handoff->stockProposal
                            ? std::span<const int>(handoff->stockProposal->choices)
                            : std::span<const int>{};
                        const auto search=optimizeUnifiedTeam(problem,knowledge,field,progressSites,*local,actingBoard,
                            teamBeforeRefinement,stockProposed,replayTariff,unifiedWork,
                            hqRankFor,buildSlotsFor,allowedByPrior);
                        QJsonObject unifiedReceipt{
                            {"mode","UNIFIED_VALUE_OVER_TIME"},
                            {"winningSeed",search.winningSeed},
                            {"sweeps",search.sweeps},
                            {"evals",search.evals},
                            {"pruned",search.pruned},
                            {"pairImprovements",search.pairImprovements},
                            {"workUsed",qint64(unifiedWorkStart-unifiedWork)},
                            {"baseline",scoreJson(search.baselineScore)},
                            {"selected",scoreJson(search.bestScore)}};
                        if(search.improved){
                            std::int64_t finalWork=std::max(work,replayTariff+UNIFIED_FINAL_REPLAY_WORK_FLOOR);
                            const auto priorPolicy=local->policy;
                            const int priorTarget=local->replyTarget;
                            const int priorShooter=local->containShooter;
                            const auto priorFrom=local->containFrom;
                            auto *const priorWorkLeft=local->workLeft;
                            const auto restoreLocal=qScopeGuard([&]{
                                local->policy=priorPolicy;
                                local->replyTarget=priorTarget;
                                local->containShooter=priorShooter;
                                local->containFrom=priorFrom;
                                local->workLeft=priorWorkLeft;
                            });
                            local->policy=TacticalReply::Economic;
                            local->replyTarget=NO_UNIT;
                            local->containShooter=NO_UNIT;
                            local->containFrom=INVALID_TILE;
                            local->workLeft=&finalWork;
                            // Replay directly without Counterpoint buyer preview, which rejects newly captured factories.
                            const auto finalTeam=evaluateReplyPassPlan(problem,search.bestTeam,actingBoard,{},search.bestFireOrders);
                            const bool teamOk=finalTeam.valued() && finalTeam.replay.valid() && finalTeam.replay.complete();
                            const auto mismatch=std::mismatch(search.bestTeam.begin(),search.bestTeam.end(),effectiveChoices.begin());
                            const int changedSlot=mismatch.first==search.bestTeam.end()?NO_REPLY_PASS_ACTOR:static_cast<int>(mismatch.first-search.bestTeam.begin());
                            auto nextAssignment=openInitialAssignment;
                            const bool mapOk=map.getMapHash()==mapAtStart;
                            const bool buyerOk=buyerState()==startingBuyer;
                            if(teamOk && mapOk && buyerOk &&
                               publish(*handoff,finalTeam,changedSlot,sourceReference,nextAssignment)){
                                const auto at=purchaseBoundary(problem,*local,finalTeam,purchaseWindow.unitSlots);
                                const auto count=purchaseWindow.affordable([&](Cell c){return at.occupied.contains(c);},at.available);
                                QJsonArray available;
                                for(const auto &[cost,site]:purchaseWindow.sitesByCost)
                                    if(cost<=purchaseWindow.treasury && !at.occupied.contains(site))available.append(QJsonArray{site.first,site.second});
                                if(count==0)available={};
                                const QJsonObject boundaryReceipt{{"purchaseBoundary",QJsonObject{
                                    {"openAffordableSites",available},{"spendableFunds",qint64(purchaseWindow.treasury)},
                                    {"affordableBuildSlots",count}}}};
                                const CombatFireOrder primaryFireOrder = search.bestFireOrders.empty()
                                    ? CombatFireOrder{} : search.bestFireOrders.front();
                                const auto physicalTeam=bindPhysicalTeam(finalTeam,primaryFireOrder,boundaryReceipt);
                                assignment=std::move(nextAssignment);
                                QJsonArray chosen;for(int c:search.bestTeam)chosen.append(c);
                                unifiedReceipt["status"]="OPEN_TACTICAL_REFINEMENT";
                                unifiedReceipt["selectedPhysicalTeam"]=physicalTeam;
                                result["openTacticalHearing"]=unifiedReceipt;
                                result["installed"]=true;
                                result["status"]="OPEN_TACTICAL_REFINEMENT";
                                result["economicComparisonStatus"]="UNRESOLVED";
                                result.remove("selectedPolicyTotal");result.remove("selectedPairArm");result.remove("selectedPairDecision");
                                result["selectedTrial"]=-1;
                                result["selectedChanges"]=changedFromSource(search.bestTeam);
                                result["choices"]=chosen;
                                result["executionOrder"]=physicalTeam["executionOrder"];
                                storeEconomicOrders(QJsonObject{},finalTeam);
                                openingAlternativeInstalled=true;
                            }else{
                                unifiedReceipt["status"]="PUBLICATION_REFUSED";
                                unifiedReceipt["refusalReason"]=!teamOk?"NO_FINAL_TEAM"
                                    :!mapOk?"MAP_CHANGED"
                                    :!buyerOk?"BUYER_CHANGED"
                                    :"PUBLISH_FAILED";
                                result["openTacticalHearing"]=unifiedReceipt;
                            }
                        }else{
                            unifiedReceipt["status"]="UNIFIED_RETAINED";
                            result["openTacticalHearing"]=unifiedReceipt;
                        }
                    }

                }
            }
            if (!openingAlternativeInstalled && !winner && repair.evaluation)
            {
                retainEffectiveReference();
                result["selectedKey"] = base["responseKey"];
                result["economicReplyKey"] = keyRecord(reference.afterReply);
                if (best && !finalComparisonUnresolved && base.value("priceStatus")=="WITNESSED")
                    result["selectedPolicyTotal"] = static_cast<qint64>(std::get<3>(*best));
            }
            if (!openingAlternativeInstalled && !winner && (finalComparisonUnresolved || unpriced>0 || base.value("priceStatus")!="WITNESSED"))
            {
                result["economicComparisonStatus"]="UNRESOLVED";
                if (!repair.evaluation) result["status"]="COMPARISON_UNRESOLVED";
            }
            if (mode == CombatMode::Acting)
            {
                if (winner && result["installed"].toBool())
                    storeEconomicOrders(selectedWithRankBound?QJsonObject{}:trials[winningRecord].toObject(),*winner);
                else if (!openingAlternativeInstalled && ((!winner && !repair.evaluation) || (repair.evaluation && result["installed"].toBool())))
                {
                    storeEconomicOrders(!selectedWithRankBound && !finalComparisonUnresolved && base.value("priceStatus")=="WITNESSED" ? base : QJsonObject{},reference);
                    if(!rejectedAgainstReference.isEmpty())result["replayValidatedRejectedChallengers"]=rejectedAgainstReference;
                }
            }
            return result;
        }
        ReplyStockContext prices(field, allowance / 3);
        if (!prices.valid())
        {
            result["status"] = "PRICING_SETUP_BUDGET";
            return result;
        }
        const auto opening = prices.tune(reference.afterReply, REPLY_PASS_BASELINE_ROUNDS);
        const auto upper = total(reference.exactNonStock, prices.owned(reference.afterReply), opening.ceiling);
        if (opening.status != ReplyStockStatus::Valid || !upper)
        {
            result["status"] = "BASELINE_UNPRICED";
            return result;
        }
        result["incumbentUpper"] = static_cast<qint64>(*upper);
        result["referenceKey"] = keyRecord(reference.afterReply);
        result["referenceWitness"] = witnessRecord(opening.floor);
        result["intervalModel"] = "day-start travel shortlist; installation rechecks post-reply travel";
        QJsonArray records;
        const auto enemy = enemyCaptureEffects(reference, problem, knowledge);
        auto choices = effectiveChoices;
        std::vector<std::pair<MilliFunds, Trial>> proposals;
        for (const auto &trial : candidates)
        {
            if (records.size() >= MAX_COMBAT_TRIALS)
                break;
            const auto &actor = problem.actors()[trial.slot];
            const auto &option = actor.candidates[trial.option];
            choices[trial.slot] = trial.option;
            const auto evaluated = evaluateReplyPassPlan(problem, choices, board);
            choices[trial.slot] = effectiveChoices[trial.slot];
            auto selected = watched;
            selected.insert(actor.engineUnitId);
            auto rec = forecastRecord(evaluated, selected);
            const int incumbent = effectiveChoices[trial.slot];
            rec["baseCandidate"] = incumbent;
            rec["baseDestination"] = tile(actor.candidates[incumbent].destination);
            rec["baseTarget"] = tile(actor.candidates[incumbent].target);
            rec["unit"] = actor.engineUnitId;
            rec["candidate"] = trial.option;
            rec["destination"] = tile(option.destination);
            rec["target"] = tile(option.target);
            if (evaluated.valued())
            {
                rec["afterReplyKey"] = keyRecord(evaluated.afterReply);
                const auto owned = prices.owned(evaluated.afterReply);
                const auto ceiling = prices.ceiling(evaluated.afterReply);
                const auto floor = prices.floor(evaluated.afterReply, &opening.floor);
                const auto lower = total(evaluated.exactNonStock, owned, floor.floor);
                const auto hi = total(evaluated.exactNonStock, owned, ceiling.ceiling);
                if (floor.status == ReplyStockStatus::Valid && ceiling.status == ReplyStockStatus::Valid && lower &&
                    hi && *lower <= *hi)
                {
                    rec["witness"] = witnessRecord(floor);
                    rec["lower"] = static_cast<qint64>(*lower);
                    rec["upper"] = static_cast<qint64>(*hi);
                    const bool comparable = enemyCaptureEffects(evaluated, problem, knowledge) == enemy;
                    rec["sameEnemyCaptureActions"] = comparable;
                    if (comparable && *lower > *upper &&
                        !changesCompletedCaptures(field, reference.afterOwnActions, evaluated.afterOwnActions))
                        proposals.emplace_back(*lower, trial);
                }
                else
                    rec["status"] = "STOCK_UNPRICED";
            }
            records.append(rec);
        }
        result["trials"] = records;
        result["eligible"] = static_cast<qint64>(candidates.size());
        result["status"] = "RETAINED";
        result["trialStockWork"] = static_cast<qint64>(prices.work());
        result["geometryWork"] = static_cast<qint64>(allowance / 2 - geometry);
        result["truncated"] = candidates.size() > static_cast<std::size_t>(records.size());
        if (proposals.empty())
        {
            retainEffectiveReference();
            return result;
        }
        std::stable_sort(proposals.begin(), proposals.end(),
                         [](const auto &a, const auto &b) { return a.first > b.first; });
        const auto travelCopyWork =
            static_cast<std::int64_t>(terrain->width) * terrain->height * (terrain->grids.size() + 1);
        if (!terrain->pay(travelCopyWork))
        {
            retainEffectiveReference();
            return result;
        }
        const auto baseTravel = friendlyTravel(*terrain, reference);
        std::int64_t verifyBudget = allowance / 6;
        ReplyStockContext original(field, verifyBudget, baseTravel.grids, baseTravel.slotsByUnit);
        if (!original.valid() || original.installPrices(prices.installedPrices()).status != ReplyStockStatus::Valid)
        {
            retainEffectiveReference();
            return result;
        }
        const auto bound = original.ceiling(reference.afterReply);
        const auto verifiedUpper = total(reference.exactNonStock, original.owned(reference.afterReply), bound.ceiling);
        if (bound.status != ReplyStockStatus::Valid || !verifiedUpper)
        {
            retainEffectiveReference();
            return result;
        }
        verifyBudget -= original.work();
        result["projectedIncumbentUpper"] = static_cast<qint64>(*verifiedUpper);
        QJsonArray verification;
        for (const auto &[estimate, trial] : proposals)
        {
            choices = effectiveChoices;
            choices[trial.slot] = trial.option;
            const auto verified = evaluateReplyPassPlan(problem, choices, board);
            QJsonObject check{{"unit", problem.actors()[trial.slot].engineUnitId}, {"candidate", trial.option}};
            if (!verified.valued() || verifyBudget <= 0)
            {
                check["status"] = "REPLAY_OR_BUDGET";
                verification.append(check);
                break;
            }
            if (breakableScreen(problem, verified, trial.slot, problem.actors()[trial.slot].candidates[effectiveChoices[trial.slot]].destination))
            {
                check["status"] = "SCREEN_CLEAR_UNRESOLVED";
                verification.append(check);
                continue;
            }
            if (!enemyRoutesNoFaster(*terrain, knowledge, field, reference, verified, geometry))
            {
                check["status"] = "ENEMY_ROUTE_OR_BUDGET";
                verification.append(check);
                continue;
            }
            if (!terrain->pay(travelCopyWork))
            {
                check["status"] = "TRAVEL_BUDGET";
                verification.append(check);
                break;
            }
            const auto travel = friendlyTravel(*terrain, verified);
            ReplyStockContext revised(field, verifyBudget, travel.grids, travel.slotsByUnit);
            if (!revised.valid())
            {
                check["status"] = "REBUILD_BUDGET";
                verification.append(check);
                break;
            }
            const auto installed = revised.installPrices(prices.installedPrices());
            const auto rebuilt = revised.floor(verified.afterReply);
            const auto floor = revised.replayWitness(verified.afterReply, rebuilt.witness);
            const auto lower = total(verified.exactNonStock, revised.owned(verified.afterReply), floor.floor);
            verifyBudget -= revised.work();
            if (installed.status != ReplyStockStatus::Valid || rebuilt.status != ReplyStockStatus::Valid ||
                floor.status != ReplyStockStatus::Valid || !lower || *lower <= *verifiedUpper)
            {
                check["status"] = "NO_PROJECTED_MARGIN";
                verification.append(check);
                continue;
            }
            result["verifiedLower"] = static_cast<qint64>(*lower);
            result["verifiedKey"] = keyRecord(verified.afterReply);
            result["verifiedWitness"] = witnessRecord(floor);
            QJsonArray order, selected;
            for (int i : verified.replay.ordering.order)
                order.append(i);
            for (int i : choices)
                selected.append(i);
            result["executionOrder"] = order;
            result["choices"] = selected;
            result["status"] = "VERIFIED";
            result["changedUnit"] = problem.actors()[trial.slot].engineUnitId;
            result["changedCandidate"] = trial.option;
            if (mode == CombatMode::Acting)
            {
                const bool accepted = publish(*handoff, verified, trial.slot, sourceReference, assignment);
                result["installed"] = accepted;
                result["status"] = accepted ? "INSTALLED" : "PUBLICATION_REFUSED";
            }
            check["status"] = result["status"];
            verification.append(check);
            break;
        }
        if (repair.evaluation && !result.contains("verifiedLower"))
            retainEffectiveReference();
        result["verification"] = verification;
        result["geometryWork"] = static_cast<qint64>(allowance / 2 - geometry);
        result["verificationStockWork"] = static_cast<qint64>(allowance / 6 - verifyBudget);
        return result;
    }
} // namespace Coordinator
