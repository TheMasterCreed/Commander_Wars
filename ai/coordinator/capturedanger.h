#pragma once

#include "ai/coordinator/realcombatcapital.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ai/coordinator/damageoracle.h"
#include "ai/coordinator/economicledger.h"
#include "ai/coordinator/propertystock.h"

#include "coreengine/precisiongrid.h"

namespace Coordinator
{
    enum class CaptureReplyRule : std::int8_t
    {
        LargestShot,
        FocusedFire,
    };

    // Computed, NoWeapon and EngineRefused are facts; every other status means the shot is unknown.
    enum class ShotStatus : std::int8_t
    {
        Computed,
        NoWeapon,
        EngineRefused,
        NoFiringTile,
        NotOnProperty,
        AmmoGated,
        NoLiveShooter,
        RepairPending,
        NotQueried,
    };

    // One status per record, derived from its shots.
    enum class ExactStatus : std::int8_t
    {
        Computed,
        NoShooter,
        NoWeapon,
        EngineRefused,
        NoFiringTile,
        NotOnProperty,
        AmmoGated,
        NoLiveShooter,
        RepairPending,
        NotQueried,
    };

    enum class CaptureDamageFormula : std::int8_t
    {
        AdvanceWars4,
        AdvanceWars1To3,
    };

    constexpr std::int32_t NO_DELIVERY_TURN = -1;
    constexpr double NEUTRAL_DEFENSE = 100;
    constexpr double NEUTRAL_OFFENSE = 100;
    constexpr double CLASSIC_DEFENSE_CEILING = 200;
    // weapon.js calculateDamage replaces a defence at or below zero with this and moves the deficit to offence.
    constexpr double MIN_DEFENSE = 1;
    // ACTION_FIRE.getDefaultLuck, a script value with no C++ accessor.
    constexpr double ENGINE_DEFAULT_LUCK = 10;
    constexpr double NO_MISFORTUNE = 0;
    // Unit.cpp's file local FLOAT_PRECISION, the grid Unit::setHp floors stored hp to.
    constexpr double ENGINE_HP_PRECISION = 100;

    struct CaptureWalkInput
    {
        std::int32_t carriedPoints{0};
        double hp{0};
        std::int32_t rateBonus{0};
        std::int32_t pointsToCapture{0};
        std::int32_t horizonTurns{0};
    };

    // damage is the untruncated figure, applied the truncated one the engine subtracts; both stop at the hp left.
    struct CaptureReply
    {
        double damage{0};
        std::int32_t hpSteps{0};
        double applied{0};

        friend bool operator==(const CaptureReply &, const CaptureReply &) = default;
    };

    struct CaptureWalkResult
    {
        std::int32_t deliveredTurn{NO_DELIVERY_TURN};
        std::int32_t unrepliedDeliveredTurn{NO_DELIVERY_TURN};
        bool destroyed{false};
        double hp{0};
        std::int32_t hpSteps{0};
        std::int32_t points{0};
        std::vector<CaptureReply> replies;
    };

    struct ShotReport
    {
        ShotStatus status{ShotStatus::NotOnProperty};
        double damage{0};
    };

    // TERRAIN keeps base damage, terrain stars and average default luck; CO, unit, weather, building, field, support and true damage are out.
    struct TerrainDefenseRule
    {
        std::int32_t stars{0};
        std::int32_t defensePerStar{0};
        bool hpScaled{false};
        CaptureDamageFormula formula{CaptureDamageFormula::AdvanceWars4};
        double luck{0};

        friend bool operator==(const TerrainDefenseRule &, const TerrainDefenseRule &) = default;
    };

    // ACTION_FIRE.calcDamage in LuckDamageMode_Average, which rolls nothing.
    constexpr double averageLuck(double luck, double misfortune)
    {
        if (luck > -misfortune)
        {
            return (luck - misfortune) / 2;
        }
        return 0;
    }

    // The engine adds the rate as is and clamps the stored points at zero.
    constexpr std::int32_t captureRateAt(std::int32_t hpSteps, std::int32_t rateBonus)
    {
        return hpSteps + rateBonus;
    }

    // building.js repairs a wounded unit on its owner's building at turn start, before it fires; any own building counts.
    constexpr bool repairPending(std::int32_t shooterHpSteps, bool onOwnBuilding)
    {
        return onOwnBuilding && shooterHpSteps < UNIT_HP_STEPS;
    }

    // Precedence: no live unit, then no weapon at all, then ammo, then repair; of these only NO_WEAPON is a known zero.
    constexpr ShotStatus shooterWeaponStatus(bool live, double baseDamageNow, double baseDamageWithAmmo,
                                             bool pendingRepair)
    {
        if (!live)
        {
            return ShotStatus::NoLiveShooter;
        }
        if (baseDamageWithAmmo < 0)
        {
            return ShotStatus::NoWeapon;
        }
        if (baseDamageNow < 0)
        {
            return ShotStatus::AmmoGated;
        }
        if (pendingRepair)
        {
            return ShotStatus::RepairPending;
        }
        return ShotStatus::Computed;
    }

    constexpr ShotReport positionFreeShot(ShotStatus weapon, double damage)
    {
        if (weapon != ShotStatus::Computed)
        {
            return ShotReport{weapon, 0};
        }
        return ShotReport{ShotStatus::Computed, damage};
    }

    // Computed means the engine may be asked; the engine reads the defender's real tile, so only a capturer on it qualifies.
    constexpr ShotStatus exactQueryStatus(ShotStatus weapon, bool defenderOnProperty, bool hasFiringTile)
    {
        if (weapon != ShotStatus::Computed)
        {
            return weapon;
        }
        if (!defenderOnProperty)
        {
            return ShotStatus::NotOnProperty;
        }
        if (!hasFiringTile)
        {
            return ShotStatus::NoFiringTile;
        }
        return ShotStatus::Computed;
    }

    constexpr ShotStatus exactShotStatus(ShotStatus query, bool enginePossible)
    {
        if (query != ShotStatus::Computed)
        {
            return query;
        }
        return enginePossible ? ShotStatus::Computed : ShotStatus::EngineRefused;
    }

    constexpr std::optional<double> shotContribution(const ShotReport & shot)
    {
        switch (shot.status)
        {
            case ShotStatus::Computed:
                return std::max(shot.damage, 0.0);
            case ShotStatus::NoWeapon:
            case ShotStatus::EngineRefused:
                return 0.0;
            case ShotStatus::NoFiringTile:
            case ShotStatus::NotOnProperty:
            case ShotStatus::AmmoGated:
            case ShotStatus::NoLiveShooter:
            case ShotStatus::RepairPending:
            case ShotStatus::NotQueried:
                break;
        }
        return std::nullopt;
    }

    // Null when any shot is unknown, since the missing one could be the largest.
    inline std::optional<double> largestShot(std::span<const ShotReport> shots)
    {
        double largest = 0;
        for (const ShotReport & shot : shots)
        {
            const std::optional<double> contribution = shotContribution(shot);
            if (!contribution)
            {
                return std::nullopt;
            }
            largest = std::max(largest, *contribution);
        }
        return largest;
    }

    // R2 fires the largest base damage first; ties keep the listed order.
    inline std::vector<std::size_t> focusedFireOrder(std::span<const double> baseDamage)
    {
        std::vector<std::size_t> order(baseDamage.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right)
        {
            return baseDamage[left] > baseDamage[right];
        });
        return order;
    }

    constexpr ExactStatus exactStatusOf(std::span<const ShotReport> shots)
    {
        bool computed = false;
        bool refused = false;
        for (const ShotReport & shot : shots)
        {
            switch (shot.status)
            {
                case ShotStatus::Computed:
                    computed = true;
                    break;
                case ShotStatus::EngineRefused:
                    refused = true;
                    break;
                case ShotStatus::NoWeapon:
                    break;
                case ShotStatus::NoFiringTile:
                    return ExactStatus::NoFiringTile;
                case ShotStatus::NotOnProperty:
                    return ExactStatus::NotOnProperty;
                case ShotStatus::AmmoGated:
                    return ExactStatus::AmmoGated;
                case ShotStatus::NoLiveShooter:
                    return ExactStatus::NoLiveShooter;
                case ShotStatus::RepairPending:
                    return ExactStatus::RepairPending;
                case ShotStatus::NotQueried:
                    return ExactStatus::NotQueried;
            }
        }
        if (shots.empty())
        {
            return ExactStatus::NoShooter;
        }
        if (computed)
        {
            return ExactStatus::Computed;
        }
        if (refused)
        {
            return ExactStatus::EngineRefused;
        }
        return ExactStatus::NoWeapon;
    }

    constexpr bool exactWalkKnown(ExactStatus status)
    {
        switch (status)
        {
            case ExactStatus::Computed:
            case ExactStatus::NoShooter:
            case ExactStatus::NoWeapon:
            case ExactStatus::EngineRefused:
                return true;
            case ExactStatus::NoFiringTile:
            case ExactStatus::NotOnProperty:
            case ExactStatus::AmmoGated:
            case ExactStatus::NoLiveShooter:
            case ExactStatus::RepairPending:
            case ExactStatus::NotQueried:
                break;
        }
        return false;
    }

    // Unit::getBonusDefensive truncates its integer bonus, and here the terrain term is the whole bonus.
    constexpr std::int32_t terrainDefenseBonus(const TerrainDefenseRule & rule, std::int32_t hpSteps)
    {
        double scale = 1;
        if (rule.hpScaled)
        {
            scale = static_cast<double>(hpSteps) / UNIT_HP_STEPS;
        }
        return static_cast<std::int32_t>(static_cast<double>(rule.stars * rule.defensePerStar) * scale);
    }

    // weapon.js calculateDamage term for term in its own operation order, so the truncated shot matches the engine's.
    constexpr double terrainReducedDamage(const TerrainDefenseRule & rule, double baseDamage,
                                          double offensiveHp, std::int32_t defenderHpSteps)
    {
        if (baseDamage <= 0)
        {
            return baseDamage;
        }
        double offense = NEUTRAL_OFFENSE;
        double defense = NEUTRAL_DEFENSE + terrainDefenseBonus(rule, defenderHpSteps);
        if (defense <= 0)
        {
            offense += defense;
            defense = MIN_DEFENSE;
        }
        const double hpFactor = offensiveHp / UNIT_HP_STEPS;
        switch (rule.formula)
        {
            case CaptureDamageFormula::AdvanceWars4:
                return std::max(hpFactor * baseDamage * (offense / defense) +
                                    rule.luck * hpFactor * NEUTRAL_DEFENSE / defense, 0.0);
            case CaptureDamageFormula::AdvanceWars1To3:
                break;
        }
        const double classic = (CLASSIC_DEFENSE_CEILING - defense) / NEUTRAL_DEFENSE;
        return std::max(hpFactor * (baseDamage * offense) / NEUTRAL_DEFENSE * classic + rule.luck * hpFactor * classic,
                        0.0);
    }

    // ACTION_FIRE.calcDefenderDamage uses virtual HP minus the untruncated
    // incoming percentage before either result is written to the game action.
    // Only a new primary action rounds its shooter HP up to a whole step.
    inline double sameActionCounterDamage(const TerrainDefenseRule &cover,double base,double hpBeforeHit,
                                          double rawPrimary,int primaryShooterHpSteps)
    {
        const auto remaining=hpBeforeHit-rawPrimary/FastBattleEstimate::HP_SCALE;
        return remaining>0 ? terrainReducedDamage(cover,base,remaining,primaryShooterHpSteps) : 0.;
    }

    // ACTION_FIRE passes each shot through writeDataInt32, which drops the fraction.
    inline double engineShotDamage(double damage)
    {
        return std::trunc(std::max(damage, 0.0));
    }

    // Unit::setHp floors to the precision grid, and getHpRounded rounds that stored value up.
    inline double engineHpAfterReply(double hpBefore, double damage)
    {
        return std::max(PrecisionGrid::floor(hpBefore - damage / FastBattleEstimate::HP_SCALE, ENGINE_HP_PRECISION), 0.0);
    }

    namespace CaptureDangerDetail
    {
        inline void applyShot(double damage, double & hp, CaptureReply & reply)
        {
            const double raw = std::max(damage, 0.0);
            const double shot = engineShotDamage(raw);
            const double remaining = hp * FastBattleEstimate::HP_SCALE;
            reply.damage += std::min(raw, remaining);
            reply.applied += std::min(shot, remaining);
            hp = engineHpAfterReply(hp, shot);
        }

        // Turn 1 is the first capture, and the enemy replies only between two of our turns inside the horizon.
        template <typename EnemyTurn>
        CaptureWalkResult walkPoints(const CaptureWalkInput & input, EnemyTurn && enemyTurn)
        {
            CaptureWalkResult result;
            result.hp = std::max(input.hp, 0.0);
            result.hpSteps = ceilHpSteps(result.hp);
            result.points = input.carriedPoints;
            if (result.hpSteps <= 0)
            {
                result.destroyed = true;
                return result;
            }
            for (std::int32_t turn = 1; turn <= input.horizonTurns; ++turn)
            {
                result.points = std::max(result.points + captureRateAt(result.hpSteps, input.rateBonus), 0);
                if (result.points >= input.pointsToCapture)
                {
                    result.deliveredTurn = turn;
                    return result;
                }
                if (turn == input.horizonTurns)
                {
                    break;
                }
                CaptureReply reply;
                enemyTurn(result.hp, reply);
                result.hpSteps = ceilHpSteps(result.hp);
                reply.hpSteps = result.hpSteps;
                result.replies.push_back(reply);
                if (result.hpSteps <= 0)
                {
                    result.destroyed = true;
                    return result;
                }
            }
            return result;
        }
    }

    inline std::int32_t unrepliedDeliveredTurn(const CaptureWalkInput & input)
    {
        return CaptureDangerDetail::walkPoints(input, [](double &, CaptureReply &) {}).deliveredTurn;
    }

    // Today's reply leaves hpAfterReply and no later reply lands.
    inline std::int32_t deliveredTurnAfterReply(const CaptureWalkInput & input, double hpAfterReply)
    {
        bool replied = false;
        return CaptureDangerDetail::walkPoints(input, [&](double & hp, CaptureReply &)
        {
            if (!replied)
            {
                hp = std::max(hpAfterReply, 0.0);
                replied = true;
            }
        }).deliveredTurn;
    }

    // reply(hpSteps) is one engine shot per enemy turn against the displayed hp.
    template <typename Reply>
    CaptureWalkResult walkCapture(const CaptureWalkInput & input, Reply && reply)
    {
        CaptureWalkResult result = CaptureDangerDetail::walkPoints(input, [&](double & hp, CaptureReply & out)
        {
            CaptureDangerDetail::applyShot(static_cast<double>(reply(ceilHpSteps(hp))), hp, out);
        });
        result.unrepliedDeliveredTurn = unrepliedDeliveredTurn(input);
        return result;
    }

    // shotAt(index, hpSteps) is shot index against the hp it meets; shots land one by one as the engine applies them.
    template <typename ShotAt>
    CaptureWalkResult walkShots(const CaptureWalkInput & input, std::size_t shotCount, ShotAt && shotAt)
    {
        CaptureWalkResult result = CaptureDangerDetail::walkPoints(input, [&](double & hp, CaptureReply & out)
        {
            for (std::size_t index = 0; index < shotCount && ceilHpSteps(hp) > 0; ++index)
            {
                CaptureDangerDetail::applyShot(static_cast<double>(shotAt(index, ceilHpSteps(hp))), hp, out);
            }
        });
        result.unrepliedDeliveredTurn = unrepliedDeliveredTurn(input);
        return result;
    }

    // shotsAt(hpSteps) lists every shooter at that hp in R2 firing order; statuses do not depend on hp, so one check covers the walk.
    template <typename ShotsAt>
    std::optional<CaptureWalkResult> walkSource(const CaptureWalkInput & input, CaptureReplyRule rule, ShotsAt && shotsAt)
    {
        const auto start = shotsAt(ceilHpSteps(input.hp));
        if (!largestShot(start))
        {
            return std::nullopt;
        }
        switch (rule)
        {
            case CaptureReplyRule::LargestShot:
                return walkCapture(input, [&](std::int32_t hpSteps)
                {
                    return largestShot(shotsAt(hpSteps)).value_or(0);
                });
            case CaptureReplyRule::FocusedFire:
                break;
        }
        return walkShots(input, start.size(), [&](std::size_t index, std::int32_t hpSteps)
        {
            return shotContribution(shotsAt(hpSteps)[index]).value_or(0);
        });
    }

    // fired is indexed like the shots and marks each one that removed hp.
    struct ReplyOutcome
    {
        CaptureReply reply;
        double hp{0};
        std::int32_t hpSteps{0};
        bool destroyed{false};
        std::vector<bool> fired;

        friend bool operator==(const ReplyOutcome &, const ReplyOutcome &) = default;
    };

    namespace CaptureDangerDetail
    {
        inline void fireShot(double damage, double & hp, CaptureReply & reply, std::vector<bool> & fired,
                             std::size_t index)
        {
            fired[index] = engineShotDamage(damage) > 0;
            applyShot(damage, hp, reply);
        }

        inline ReplyOutcome finishReply(ReplyOutcome outcome, double hp)
        {
            outcome.hp = hp;
            outcome.hpSteps = ceilHpSteps(hp);
            outcome.reply.hpSteps = outcome.hpSteps;
            outcome.destroyed = outcome.hpSteps <= 0;
            return outcome;
        }
    }

    // One enemy turn under R1 or R2; null when a shot is unknown, as for the walks.
    template <typename ShotsAt>
    std::optional<ReplyOutcome> singleReply(double hpBefore, CaptureReplyRule rule, ShotsAt && shotsAt)
    {
        double hp = std::max(hpBefore, 0.0);
        const auto start = shotsAt(ceilHpSteps(hp));
        const std::optional<double> largest = largestShot(start);
        if (!largest)
        {
            return std::nullopt;
        }
        ReplyOutcome outcome;
        outcome.fired.assign(start.size(), false);
        for (std::size_t index = 0; index < start.size() && ceilHpSteps(hp) > 0; ++index)
        {
            switch (rule)
            {
                case CaptureReplyRule::LargestShot:
                    if (shotContribution(start[index]).value_or(0) == *largest)
                    {
                        CaptureDangerDetail::fireShot(*largest, hp, outcome.reply, outcome.fired, index);
                        return CaptureDangerDetail::finishReply(std::move(outcome), hp);
                    }
                    break;
                case CaptureReplyRule::FocusedFire:
                    CaptureDangerDetail::fireShot(shotContribution(shotsAt(ceilHpSteps(hp))[index]).value_or(0), hp,
                                                  outcome.reply, outcome.fired, index);
                    break;
            }
        }
        return CaptureDangerDetail::finishReply(std::move(outcome), hp);
    }

    // P resolves each unknown instead of nulling the reply.
    enum class PricedResolution : std::int8_t
    {
        Known,
        AmmoResupplied,
        AmmoAway,
        Repaired,
        NoLiveShooter,
        NoFiringTile,
    };

    enum class CaptureInterruption : std::int8_t
    {
        None,
        Delayed,
        Restarted,
    };

    // building.js repairUnit's default health; CO repair bonuses and the owner's funds are left out.
    constexpr std::int32_t BUILDING_REPAIR_STEPS = 2;
    constexpr double NO_COUNTER = -1;
    // The stock walk credits a capturer standing on its tile with arrival on its first activation.
    constexpr std::int32_t CAPTURE_IN_PLACE_ACTIVATION = 1;
    constexpr std::int32_t CAPTURE_NEXT_ACTIVATION = 2;

    constexpr std::int32_t repairedHpSteps(std::int32_t hpSteps)
    {
        return std::min(hpSteps + BUILDING_REPAIR_STEPS, UNIT_HP_STEPS);
    }

    // Precedence follows shooterWeaponStatus; a shooter with no tile to fire from never fires.
    constexpr PricedResolution pricedResolution(ShotStatus weapon, bool onOwnBuilding, bool hasFiringTile)
    {
        if (weapon == ShotStatus::NoLiveShooter)
        {
            return PricedResolution::NoLiveShooter;
        }
        if (!hasFiringTile)
        {
            return PricedResolution::NoFiringTile;
        }
        if (weapon == ShotStatus::AmmoGated)
        {
            return onOwnBuilding ? PricedResolution::AmmoResupplied : PricedResolution::AmmoAway;
        }
        if (weapon == ShotStatus::RepairPending)
        {
            return PricedResolution::Repaired;
        }
        return PricedResolution::Known;
    }

    constexpr bool pricedShotPossible(PricedResolution resolution)
    {
        switch (resolution)
        {
            case PricedResolution::Known:
            case PricedResolution::AmmoResupplied:
            case PricedResolution::Repaired:
                return true;
            case PricedResolution::AmmoAway:
            case PricedResolution::NoLiveShooter:
            case PricedResolution::NoFiringTile:
                break;
        }
        return false;
    }

    // building.js repairs and refills together, so a resupplied shooter is also repaired.
    constexpr std::int32_t pricedShooterHpSteps(std::int32_t hpSteps, bool onOwnBuilding)
    {
        return repairPending(hpSteps, onOwnBuilding) ? repairedHpSteps(hpSteps) : hpSteps;
    }

    constexpr double pricedRawBase(PricedResolution resolution, double rawBase, double baseWithAmmo)
    {
        return resolution == PricedResolution::AmmoResupplied ? baseWithAmmo : rawBase;
    }

    // scaledBaseDamage's own arithmetic, so BASE is the planner's next-turn shot.
    constexpr double hpScaledBase(double rawBase, std::int32_t hpSteps)
    {
        return rawBase * hpSteps / UNIT_HP_STEPS;
    }

    // The counter is a BASE shot with roles swapped, from the hp our unit keeps after the enemy shot.
    constexpr MilliFunds counterCost(double counterRawBase, std::int32_t defenderHpStepsAfter,
                                     std::int32_t shooterHpSteps, MilliFunds shooterCost)
    {
        if (counterRawBase < 0 || defenderHpStepsAfter <= 0)
        {
            return 0;
        }
        return bookDamage(shooterCost,
                          damageStepsFrom(shooterHpSteps, hpScaledBase(counterRawBase, defenderHpStepsAfter)));
    }

    constexpr CaptureInterruption captureInterruption(bool capturingHere, bool mustMove)
    {
        if (!capturingHere)
        {
            return CaptureInterruption::None;
        }
        return mustMove ? CaptureInterruption::Restarted : CaptureInterruption::Delayed;
    }

    // What the enemy's own stock credits a capturer standing on its column: the row weight rowWeights builds.
    constexpr MilliFunds captureStreamInPlace(const PropertyStockColumn & theirColumn, std::int32_t carriedPoints,
                                              std::int32_t ratePerTurn, std::int32_t pointsToCapture,
                                              std::int32_t horizonTurns)
    {
        return columnWeight(theirColumn, ArrivalFacts{CAPTURE_IN_PLACE_ACTIVATION, carriedPoints, ratePerTurn},
                            pointsToCapture, horizonTurns);
    }

    // Firing in place costs one turn of the capture; moving away to fire restarts it from zero a turn later.
    constexpr MilliFunds forgoneCaptureStream(const PropertyStockColumn & theirColumn, std::int32_t carriedPoints,
                                              std::int32_t ratePerTurn, CaptureInterruption interruption,
                                              std::int32_t pointsToCapture, std::int32_t horizonTurns)
    {
        if (interruption == CaptureInterruption::None)
        {
            return 0;
        }
        const std::int32_t keptPoints = interruption == CaptureInterruption::Delayed ? carriedPoints : 0;
        return captureStreamInPlace(theirColumn, carriedPoints, ratePerTurn, pointsToCapture, horizonTurns) -
               columnWeight(theirColumn, ArrivalFacts{CAPTURE_NEXT_ACTIVATION, keptPoints, ratePerTurn},
                            pointsToCapture, horizonTurns);
    }

    constexpr bool pricedShotFires(MilliFunds worth, MilliFunds counter, MilliFunds capture)
    {
        return worth > counter + capture;
    }

    struct PricedShooter
    {
        PricedResolution resolution{PricedResolution::Known};
        MilliFunds captureCost{0};
    };

    struct PricedShot
    {
        bool fires{false};
        MilliFunds worth{0};
        MilliFunds counterCost{0};
        MilliFunds captureCost{0};
    };

    struct PricedReply
    {
        ReplyOutcome outcome;
        std::vector<PricedShot> shots;
    };

    namespace CaptureDangerDetail
    {
        // damageAt(index, hpSteps) is the shot at the hp it meets; counterCostAt(index, hpStepsAfter) prices the counter.
        template <typename DamageAt, typename CounterCostAt>
        PricedReply pricedTurn(double & hp, MilliFunds actorCost, std::span<const PricedShooter> shooters,
                               DamageAt && damageAt, CounterCostAt && counterCostAt)
        {
            PricedReply result;
            result.outcome.fired.assign(shooters.size(), false);
            result.shots.resize(shooters.size());
            for (std::size_t index = 0; index < shooters.size(); ++index)
            {
                PricedShot & shot = result.shots[index];
                shot.captureCost = shooters[index].captureCost;
                const std::int32_t hpSteps = ceilHpSteps(hp);
                if (hpSteps <= 0 || !pricedShotPossible(shooters[index].resolution))
                {
                    continue;
                }
                const double damage = static_cast<double>(damageAt(index, hpSteps));
                const std::int32_t hpStepsAfter = ceilHpSteps(engineHpAfterReply(hp, engineShotDamage(damage)));
                shot.worth = bookDamage(actorCost, hpSteps - hpStepsAfter);
                shot.counterCost = counterCostAt(index, hpStepsAfter);
                shot.fires = pricedShotFires(shot.worth, shot.counterCost, shot.captureCost);
                if (shot.fires)
                {
                    fireShot(damage, hp, result.outcome.reply, result.outcome.fired, index);
                }
            }
            return result;
        }
    }

    // P: one enemy turn where each shooter in order fires only when its book damage beats what firing costs it.
    template <typename DamageAt, typename CounterCostAt>
    PricedReply pricedReply(double hpBefore, MilliFunds actorCost, std::span<const PricedShooter> shooters,
                            DamageAt && damageAt, CounterCostAt && counterCostAt)
    {
        double hp = std::max(hpBefore, 0.0);
        PricedReply result = CaptureDangerDetail::pricedTurn(hp, actorCost, shooters, damageAt, counterCostAt);
        result.outcome = CaptureDangerDetail::finishReply(std::move(result.outcome), hp);
        return result;
    }

    template <typename DamageAt, typename CounterCostAt>
    CaptureWalkResult walkPriced(const CaptureWalkInput & input, MilliFunds actorCost,
                                 std::span<const PricedShooter> shooters, DamageAt && damageAt,
                                 CounterCostAt && counterCostAt)
    {
        CaptureWalkResult result = CaptureDangerDetail::walkPoints(input, [&](double & hp, CaptureReply & out)
        {
            out = CaptureDangerDetail::pricedTurn(hp, actorCost, shooters, damageAt, counterCostAt).outcome.reply;
        });
        result.unrepliedDeliveredTurn = unrepliedDeliveredTurn(input);
        return result;
    }

    constexpr std::size_t NO_ONE_ACTION_TARGET = static_cast<std::size_t>(-1);

    // unitId and knowledgeIndex only break ties; hp is the real hp after the unit's own action.
    struct OneActionTarget
    {
        std::int32_t unitId{NO_UNIT};
        double hp{0};
        MilliFunds cost{0};
    };

    // The hp and counter cost the selection priced the shot at, kept so callers need not price it again.
    struct OneActionShot
    {
        std::size_t shooter{0};
        std::size_t target{0};
        MilliFunds net{0};
        std::int32_t hpStepsBefore{0};
        std::int32_t hpStepsAfter{0};
        MilliFunds counterCost{0};
        TilePoint firingTile{INVALID_TILE};
        double realBefore{}, realAfter{};
        MilliFunds targetCapital{};
    };

    // outcomes[target].fired lists that target's shots in firing order; chosen[shooter] is its target or none.
    struct OneActionReply
    {
        std::vector<ReplyOutcome> outcomes;
        std::vector<OneActionShot> shots;
        std::vector<std::size_t> chosen;
        // Pairs whose shot was priced, the selection's work.
        std::int64_t pairsExamined{0};
    };

    // Larger net first, then the lower shooter knowledge index, then the lower target unit id.
    constexpr bool oneActionShotBeats(MilliFunds net, std::int32_t shooterIndex, std::int32_t unitId,
                                      MilliFunds bestNet, std::int32_t bestShooterIndex, std::int32_t bestUnitId)
    {
        if (net != bestNet)
        {
            return net > bestNet;
        }
        if (shooterIndex != bestShooterIndex)
        {
            return shooterIndex < bestShooterIndex;
        }
        return unitId < bestUnitId;
    }

    // P1: each shooter acts once over the whole plan, on the positive pair of largest net, priced with P's facts.
    struct IgnoreReplyShot
    {
        void operator()(const OneActionShot &) const {}
    };

    struct EqualReplyPriority
    {
        int operator()(std::size_t, std::size_t) const
        {
            return 0;
        }
    };
    template <typename CanFire, typename DamageAt, typename CounterCostAt, typename CaptureCostAt,
              typename OnShot = IgnoreReplyShot, typename PriorityAt = EqualReplyPriority>
    OneActionReply oneActionReply(std::span<const OneActionTarget> targets,
                                  std::span<const std::int32_t> shooterIndices, CanFire &&canFire, DamageAt &&damageAt,
                                  CounterCostAt &&counterCostAt, CaptureCostAt &&captureCostAt, OnShot onShot = {},
                                  PriorityAt priorityAt = {}, bool useRealCapital = false)
    {
        OneActionReply result;
        result.chosen.assign(shooterIndices.size(), NO_ONE_ACTION_TARGET);
        std::vector<double> hp;
        hp.reserve(targets.size());
        for (const OneActionTarget &target : targets)
        {
            hp.push_back(std::max(target.hp, 0.0));
        }
        result.outcomes.resize(targets.size());
        while (true)
        {
            std::optional<OneActionShot> best;
            double bestDamage = 0;
            int bestPriority = 0;
            for (std::size_t shooter = 0; shooter < shooterIndices.size(); ++shooter)
            {
                if (result.chosen[shooter] != NO_ONE_ACTION_TARGET)
                {
                    continue;
                }
                for (std::size_t target = 0; target < targets.size(); ++target)
                {
                    const std::int32_t hpSteps = ceilHpSteps(hp[target]);
                    if (hpSteps <= 0 || !canFire(shooter, target))
                    {
                        continue;
                    }
                    ++result.pairsExamined;
                    const double damage = static_cast<double>(damageAt(shooter, target, hpSteps));
                    const double remaining = engineHpAfterReply(hp[target], engineShotDamage(damage));
                    const std::int32_t hpStepsAfter = ceilHpSteps(remaining);
                    const MilliFunds counter = counterCostAt(shooter, target, hpStepsAfter);
                    const MilliFunds capital = useRealCapital ? combatCapitalLoss(targets[target].cost, hp[target], remaining)
                                                             : bookDamage(targets[target].cost, hpSteps - hpStepsAfter);
                    const MilliFunds net = capital - counter - captureCostAt(shooter, target);
                    const int priority = priorityAt(shooter, target);
                    if ((priority > 0 && engineShotDamage(damage) <= 0) || (net <= 0 && priority <= 0) ||
                        (best && (priority < bestPriority ||
                                  (priority == bestPriority &&
                                   !oneActionShotBeats(net, shooterIndices[shooter], targets[target].unitId, best->net,
                                                       shooterIndices[best->shooter], targets[best->target].unitId)))))
                    {
                        continue;
                    }
                    best = OneActionShot{shooter, target, net, hpSteps, hpStepsAfter, counter, INVALID_TILE};
                    if (useRealCapital)
                    {
                        best->realBefore = hp[target];
                        best->realAfter = remaining;
                        best->targetCapital = capital;
                    }
                    bestDamage = damage;
                    bestPriority = priority;
                }
            }
            if (!best)
            {
                break;
            }
            result.chosen[best->shooter] = best->target;
            result.shots.push_back(*best);
            onShot(*best);
            ReplyOutcome &outcome = result.outcomes[best->target];
            outcome.fired.push_back(false);
            CaptureDangerDetail::fireShot(bestDamage, hp[best->target], outcome.reply, outcome.fired,
                                          outcome.fired.size() - 1);
        }
        for (std::size_t target = 0; target < targets.size(); ++target)
        {
            result.outcomes[target] = CaptureDangerDetail::finishReply(std::move(result.outcomes[target]), hp[target]);
        }
        return result;
    }
}
