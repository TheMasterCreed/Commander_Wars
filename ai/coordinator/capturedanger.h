#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>
#include <vector>

#include "ai/coordinator/damageoracle.h"
#include "ai/coordinator/economicledger.h"

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
                                          std::int32_t shooterHpSteps, std::int32_t defenderHpSteps)
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
        const double hpFactor = static_cast<double>(shooterHpSteps) / UNIT_HP_STEPS;
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
}
