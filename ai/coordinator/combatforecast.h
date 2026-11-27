#pragma once

#include <queue>
#include <map>
#include <set>
#include <unordered_map>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>

#include "ai/coordinator/mobilityfield.h"
#include "ai/coordinator/townworkerfence.h"
#include "ai/coordinator/screenfireprofile.h"
#include "ai/coordinator/combatencirclement.h"
#include "ai/coordinator/combatrecruitreply.h"
#include "ai/coordinator/orderedopeningreplay.h"
#include <array>
#include "ai/coordinator/replyprediction.h"

namespace Coordinator
{
    enum class TacticalReply
    {
        Economic,
        InterruptCapture,
        ArmorAndInfantry,
        ClearGuard,
        PreserveCapture,
        InfantryCapture,
        TargetUnit,
        BypassUnit,
        ContainUnit,
        WaitOnProperty
    };

    struct CombatPawn
    {
        TilePoint tile{INVALID_TILE};
        bool present{false};
        bool enemy{false};
        bool ignoresCollision{false};
        std::int32_t grid{-1};
        std::int32_t movement{0};
        std::int32_t engineUnitId{NO_UNIT};
        bool capper{false};
        bool carrying{false};
        int availableShots{0};
        MilliFunds replacementCost{0};
        double realHp{-1};
        std::array<int,2> ammoByWeapon{-1,-1};
        // A parked indirect can travel on a prior turn without moving and firing on this one.
        int travelGrid{-1};
        // The base-weapon plus terrain model cannot authenticate a different
        // firing order or unit/CO-dependent combat bonuses.
        bool needsNativeTownExchange{};
        // Same native movement rules as this opening, before any fuel trim.
        int movementBeforeFuel{-1};
        bool transportCapacity{},supportsUnits{};
        bool hasNativeWeapon{};
    };

    struct ProductionForecastBoard;
    struct CombatMovementCache
    {
        using Key = std::tuple<int,int,int,bool,std::vector<int>>;
        using UnblockedKey = std::tuple<int,int,int>;
        using Costs = std::shared_ptr<const std::vector<std::int32_t>>;
        struct KeyHash
        {
            static constexpr std::size_t HASH_MIX = 0x9e3779b97f4a7c15ULL;
            static constexpr std::size_t HASH_LEFT_SHIFT = 6;
            std::size_t operator()(const Key & k) const noexcept
            {
                std::size_t h = static_cast<std::size_t>(std::get<0>(k)) * HASH_MIX;
                h ^= static_cast<std::size_t>(std::get<1>(k)) + HASH_MIX + (h << HASH_LEFT_SHIFT) + (h >> 2);
                h ^= static_cast<std::size_t>(std::get<2>(k)) + HASH_MIX + (h << HASH_LEFT_SHIFT) + (h >> 2);
                h ^= static_cast<std::size_t>(std::get<3>(k)) + HASH_MIX + (h << HASH_LEFT_SHIFT) + (h >> 2);
                for (const int b : std::get<4>(k))
                    h ^= static_cast<std::size_t>(b) + HASH_MIX + (h << HASH_LEFT_SHIFT) + (h >> 2);
                return h;
            }
        };
        struct UnblockedKeyHash
        {
            static constexpr std::size_t HASH_MIX = 0x9e3779b97f4a7c15ULL;
            static constexpr std::size_t HASH_LEFT_SHIFT = 6;
            std::size_t operator()(const UnblockedKey & k) const noexcept
            {
                std::size_t h = static_cast<std::size_t>(std::get<0>(k)) * HASH_MIX;
                h ^= static_cast<std::size_t>(std::get<1>(k)) + HASH_MIX + (h << HASH_LEFT_SHIFT) + (h >> 2);
                h ^= static_cast<std::size_t>(std::get<2>(k)) + HASH_MIX + (h << HASH_LEFT_SHIFT) + (h >> 2);
                return h;
            }
        };
        struct UnblockedField
        {
            Costs costs;
            std::vector<int> reachableCells;
        };
        static constexpr std::size_t maxEntries = 8192;
        std::unordered_map<Key,Costs,KeyHash> fields;
        std::unordered_map<UnblockedKey,UnblockedField,UnblockedKeyHash> unblockedFields;
        std::map<const MobilityCostGrid*,int> minimumEntryCost;
        int hits{}, misses{};
    };
    struct BreakableTerrainTile
    {
        TilePoint tile{INVALID_TILE};
        std::int32_t hpSteps{UNIT_HP_STEPS};
        bool opensPlasma{false};
        std::vector<TilePoint> openedTiles;
    };

    struct CombatTerrain
    {
        std::int32_t width{0};
        std::int32_t height{0};
        std::int64_t *workLeft{};
        TilePoint guardedHQ{INVALID_TILE};
        std::set<std::pair<int,int>> contestableSites;
        // A proposed F0 purchase is present before the opponent chooses its E0 movement.
        std::set<std::pair<int,int>> friendlyBirthSites;
        std::function<double(int, int, int)> weapon;
        OwnFireAt ownFire;
        std::function<double(int, TilePoint)> envWeapon;
        std::function<std::array<double,2>(int,int,int)> weaponSlots;
        std::function<TerrainDefenseRule(int, TilePoint)> defense;
        TacticalReply policy{TacticalReply::Economic};
        int replyTarget{NO_UNIT};
        const OpeningRecruitTarget *openingRecruitTarget{};
        const OrderedOpeningPolicy *orderedOpening{};
        int containShooter{NO_UNIT};
        TilePoint containFrom{INVALID_TILE};
        bool pay(std::int64_t work) const
        {
            pumpUiEventsIfDue();
            requireTownOwnerState("OUTER_COMBAT_WORK");
            if (!workLeft || work < 0 || *workLeft < work)
                return false;
            *workLeft -= work;
            return true;
        }
        std::vector<CombatPawn> units;
        std::vector<BreakableTerrainTile> breakableTerrains;
        std::vector<MobilityCostGrid> grids;
        std::shared_ptr<const ProductionForecastBoard> production;
        std::shared_ptr<CombatMovementCache> movementCache;
        std::int64_t comparisonWork{-1};
        bool completeProductionMenu{};
        bool ordinaryCaptureInstallments{};
        bool terrainAwareFiringTile{false};
        std::function<bool(int,int,std::int64_t&)> certifyRetainedWalkProfile;
        std::shared_ptr<const ScreenFireProfile> screenFire;
        std::shared_ptr<ScreenRuleDemand> screenDemand;
        std::function<bool(int,TilePoint)> canRepairOnTerrain;
        std::vector<ObservedReplyDefender> observedDefenders;
        std::int64_t quoteWorkOffer(std::int64_t remaining,int divisor,std::int64_t cap) const
        {
            const auto initial=comparisonWork<0 ? remaining : comparisonWork;
            return std::min(cap,initial/divisor);
        }
        std::optional<std::int64_t> quoteWorkQuota(std::int64_t remaining,int divisor,std::int64_t cap) const
        {
            const auto quota=quoteWorkOffer(remaining,divisor,cap);
            if (remaining<quota) return {};
            return quota;
        }
    };

    struct LoadedCombatWeapon
    {
        double base{-1};
        int slot{-1};
    };

    inline LoadedCombatWeapon supportWeapon(const CombatTerrain &terrain, int actor, int target, int distance,
                                            const std::array<int,2> &load)
    {
        const auto slotsAtRange = terrain.weaponSlots ? terrain.weaponSlots(actor,target,distance) :
                                                       std::array<double,2>{terrain.weapon(actor,target,distance),-1.};
        LoadedCombatWeapon selected;
        for (int n=0;n<2;++n)
            if (load[n] != 0 && slotsAtRange[n] >= 0 && (selected.slot < 0 || slotsAtRange[n] > selected.base))
                selected = {slotsAtRange[n],n};
        return selected;
    }

    inline void spendCombatRound(std::array<int,2> &load, int slot)
    {
        if (slot >= 0 && slot < static_cast<int>(load.size()) && load[slot] > 0)
            --load[slot];
    }

    inline int minimumTerrainEntryCost(const MobilityCostGrid &grid)
    {
        int minimum=std::numeric_limits<int>::max();
        for(int y=0;y<grid.height();++y) for(int x=0;x<grid.width();++x)
            for(auto dir:ENTRY_DIRECTIONS)
                if(const auto cost=grid.cost(x,y,dir);cost>=0) minimum=std::min(minimum,cost);
        return minimum;
    }

    // Every team and opponent reply sees the same native directional terrain.
    // Price its lower movement bound once in board setup, before per-job work
    // is divided. A dated path is still required for any actual moving shot.
    inline std::int64_t prepareCaptureMovementBounds(const CombatTerrain &terrain,std::int64_t &budget)
    {
        const auto before=budget;
        if(!terrain.movementCache) return 0;
        auto &bounds=terrain.movementCache->minimumEntryCost;
        for(const auto &grid:terrain.grids)
        {
            if(bounds.contains(&grid)) continue;
            const auto work=static_cast<std::int64_t>(grid.width())*grid.height()*ENTRY_DIRECTION_COUNT;
            if(work>static_cast<std::uint64_t>(budget)) break;
            budget-=work;
            bounds.emplace(&grid,minimumTerrainEntryCost(grid));
        }
        return before-budget;
    }

    class CombatReplyPosition
    {
      public:
        CombatReplyPosition(const CombatTerrain &terrain, std::span<const PlanReplyAction> actions,
                            const ResolvedPlanReply &resolved)
            : m_terrain(terrain), m_units(terrain.units),
              m_cells(static_cast<std::size_t>(terrain.width) * terrain.height, NO_UNIT)
        {
            if(terrain.orderedOpening && terrain.orderedOpening->afterService.size()==m_units.size())
                for(std::size_t i=0;i<m_units.size();++i)if(m_units[i].enemy){
                    const auto mp=unchangedOpeningMovement(false,m_units[i].movementBeforeFuel,terrain.orderedOpening->afterService[i].fuel);
                    if(mp)m_units[i].movement=*mp;
                }
            for (std::size_t i = 0; i < actions.size(); ++i)
            {
                const auto &bundle = actions[i].pCandidate->bundle;
                auto &pawn = m_units.at(bundle.unitId);
                pawn.tile = bundle.destination;
                pawn.present = resolved.targets[i].target.hp > 0;
            }
            for (const auto &[index, damage] : resolved.own.chain.chain)
            {
                if (damage.after <= 0 && !isTerrainTargetId(index) &&
                    index >= 0 && index < static_cast<int>(m_units.size()))
                {
                    m_units.at(index).present = false;
                }
            }
            for (std::size_t i = 0; i < m_units.size(); ++i)
            {
                if (m_units[i].present)
                {
                    m_cells[cell(m_units[i].tile)] = static_cast<std::int32_t>(i);
                }
            }
        }

        bool sameLivingRoster(const OpeningLaneState &state) const
        {
            if(state.size()!=m_units.size())return false;
            for(std::size_t i=0;i<state.size();++i){
                const auto &p=m_units[i];const auto &v=state[i];
                if((v.hp>0)!=p.present || v.side!=static_cast<int>(p.enemy) || (p.present && v.at!=p.tile))return false;
            }
            return true;
        }

        // Cost of one enemy order on the current board. A friend can be crossed, but cannot share an endpoint.
        int movementCost(int unit, TilePoint tile)
        {
            const auto &pawn = m_units[unit];
            if (!pawn.present || tile == INVALID_TILE)
                return UNREACHABLE;
            if (pawn.tile == tile)
                return 0;
            if (pawn.grid < 0 || !m_terrain.grids[pawn.grid].onGrid(tile.x, tile.y) ||
                m_cells[cell(tile)] != NO_UNIT || m_terrain.friendlyBirthSites.contains({tile.x,tile.y}))
                return UNREACHABLE;
            auto [it, fresh] = m_reach.try_emplace(unit);
            if (fresh)
                it->second = reachable(pawn);
            return (*it->second)[cell(tile)];
        }

        bool hasReachField(int id) const { return m_reach.contains(id); }

        TilePoint firingTile(const ReplyShooterFacts &shooter)
        {
            return firingTile(shooter, INVALID_TILE, [](TilePoint) { return std::pair{0., 0}; });
        }

        template<class Rank>
        TilePoint firingTile(const ReplyShooterFacts &shooter, TilePoint fixed, Rank &&rank)
        {
            const auto &pawn = m_units.at(shooter.shooterIndex);
            if (!pawn.present)
                return INVALID_TILE;
            if (!shooter.direct)
                return pawn.tile == shooter.tile && (fixed == INVALID_TILE || fixed == pawn.tile) ? pawn.tile : INVALID_TILE;
            TilePoint best = INVALID_TILE;
            std::optional<std::tuple<double, int, int, int, int>> bestRank;
            for (auto tile : shooter.firingTiles)
            {
                if (fixed != INVALID_TILE && fixed != tile)
                    continue;
                const int cost = movementCost(shooter.shooterIndex, tile);
                if (cost < 0)
                    continue;
                const auto [counter, defense] = rank(tile);
                const auto key = std::tuple{counter, -defense, cost, tile.y, tile.x};
                if (!bestRank || key < *bestRank)
                {
                    bestRank = key;
                    best = tile;
                }
            }
            return best;
        }

        // The four neighboring cells are a bounded assignment problem; one pawn can occupy at most one exit.
        void reserveExits(int target, const std::vector<bool> &spent, std::vector<TilePoint> &reserved,
                          CombatEncirclement &trap)
        {
            const auto &victim = m_units[target];
            if (!victim.present || victim.grid < 0 || victim.ignoresCollision || victim.movement <= 0)
                return;
            refreshExits(trap);
            std::vector<TilePoint> exits;
            for (const auto &exit : trap.exits)
            {
                if (!exit.passable)
                    continue;
                if (exit.unitIndex != NO_UNIT && !m_units[exit.unitIndex].enemy)
                    return;
                if (exit.unitIndex == NO_UNIT || !spent[exit.unitIndex])
                    exits.push_back(exit.tile);
            }
            if (exits.empty())
                return;
            struct Cover
            {
                bool feasible{};
                int abandoned{}, vehicles{}, travel{};
                std::array<int, 4> units{NO_UNIT, NO_UNIT, NO_UNIT, NO_UNIT};
                std::array<int,4> costs{};
                auto rank() const { return std::tie(abandoned, vehicles, travel, units); }
            };
            const int limit = 1 << static_cast<int>(exits.size());
            std::array<Cover, 16> solutions{};
            solutions[0].feasible = true;
            for (int i = 0; i < static_cast<int>(m_units.size()); ++i)
            {
                const auto &pawn = m_units[i];
                if (!pawn.enemy || !pawn.present || spent[i])
                    continue;
                if (!m_terrain.pay(limit * static_cast<int>(exits.size())))
                {
                    m_exhausted = true;
                    return;
                }
                std::array<int, 4> costs{};
                for (int k = 0; k < static_cast<int>(exits.size()); ++k)
                    costs[k] = movementCost(i, exits[k]);
                for (int mask = limit - 1; mask >= 0; --mask)
                {
                    if (!solutions[mask].feasible)
                        continue;
                    for (int k = 0; k < static_cast<int>(exits.size()); ++k)
                    {
                        if ((mask & (1 << k)) || costs[k] < 0)
                            continue;
                        auto choice = solutions[mask];
                        choice.units[k] = i;
                        choice.costs[k] = costs[k];
                        choice.abandoned += pawn.carrying && pawn.tile != exits[k];
                        choice.vehicles += !pawn.capper;
                        choice.travel += costs[k];
                        auto &next = solutions[mask | (1 << k)];
                        if (!next.feasible || choice.rank() < next.rank())
                            next = choice;
                    }
                }
            }
            const auto &cover = solutions[limit - 1];
            if (!cover.feasible || m_exhausted)
                return;
            for (int k = 0; k < static_cast<int>(exits.size()); ++k)
            {
                int i = cover.units[k];
                auto &pawn = m_units[i];
                trap.blockOrders.push_back({i, pawn.engineUnitId, pawn.tile, exits[k], false, cover.costs[k]});
                m_cells[cell(pawn.tile)] = NO_UNIT;
                pawn.tile = reserved[i] = exits[k];
                m_cells[cell(pawn.tile)] = i;
            }
            m_reach.clear();
        }

        void refreshExits(CombatEncirclement &trap) const
        {
            trap.exits.clear();
            trap.sealed = false;
            const auto &pawn = m_units.at(trap.targetKnowledge);
            if (!pawn.present || pawn.grid < 0 || pawn.ignoresCollision)
                return;
            const auto &grid = m_terrain.grids[pawn.grid];
            bool open = false, movable = false;
            for (auto direction : ENTRY_DIRECTIONS)
            {
                const auto delta = entryStep(direction);
                TilePoint tile{pawn.tile.x + delta.dx, pawn.tile.y + delta.dy};
                if (!grid.onGrid(tile.x, tile.y))
                    continue;
                const int cost = grid.cost(tile.x, tile.y, direction), occupant = m_cells[cell(tile)];
                const bool passable = cost >= 0 && cost <= pawn.movement;
                trap.exits.push_back({tile, occupant, occupant == NO_UNIT ? NO_UNIT : m_units[occupant].engineUnitId, passable});
                movable |= passable;
                open |= passable && (occupant == NO_UNIT || !m_units[occupant].enemy);
            }
            trap.sealed = movable && !open;
        }

        void apply(std::int32_t shooter, TilePoint firing, bool shooterSurvives, std::int32_t target,
                   bool targetSurvives)
        {
            auto &pawn = m_units[shooter];
            m_cells[cell(pawn.tile)] = NO_UNIT;
            pawn.tile = firing;
            pawn.present = shooterSurvives;
            if (shooterSurvives)
                m_cells[cell(firing)] = shooter;
            if (!targetSurvives)
            {
                m_cells[cell(m_units[target].tile)] = NO_UNIT;
                m_units[target].present = false;
            }
            m_reach.clear();
        }

        void applyOpeningFight(const OpeningRecruitFight &shot)
        {
            auto &pawn=m_units[shot.shooter];
            m_cells[cell(pawn.tile)]=NO_UNIT;
            pawn.tile=shot.from;
            pawn.present=shot.stockAfter>0;
            if(pawn.present) m_cells[cell(pawn.tile)]=shot.shooter;
            m_reach.clear();
        }

        std::vector<TilePoint> locations() const
        {
            std::vector<TilePoint> positions;
            positions.reserve(m_units.size());
            for (const auto &unit : m_units)
                positions.push_back(unit.present ? unit.tile : INVALID_TILE);
            return positions;
        }

        bool exhausted() const
        {
            return m_exhausted;
        }

      private:
        std::size_t cell(TilePoint tile) const
        {
            return static_cast<std::size_t>(tile.y) * m_terrain.width + tile.x;
        }

        CombatMovementCache::Costs reachable(const CombatPawn &pawn)
        {
            const auto &grid = m_terrain.grids[pawn.grid];
            auto *cache = m_terrain.movementCache.get();
            const int startCell = static_cast<int>(cell(pawn.tile));
            CombatMovementCache::Key key;
            if (cache)
            {
                if (!m_terrain.pay(1))
                {
                    m_exhausted = true;
                    return std::make_shared<const std::vector<std::int32_t>>(m_cells.size(), UNREACHABLE);
                }
                const CombatMovementCache::UnblockedKey uKey{pawn.grid, startCell, pawn.movement};
                auto uIt = cache->unblockedFields.find(uKey);
                if (uIt == cache->unblockedFields.end())
                {
                    auto computed = std::make_shared<std::vector<std::int32_t>>(m_cells.size(), UNREACHABLE);
                    auto &uCosts = *computed;
                    std::vector<int> reachCells;
                    using Entry = std::tuple<std::int32_t, std::int32_t, std::int32_t>;
                    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
                    if (!m_terrain.pay(static_cast<std::int64_t>(uCosts.size())))
                    {
                        m_exhausted = true;
                        return computed;
                    }
                    uCosts[startCell] = 0;
                    queue.emplace(0, pawn.tile.y, pawn.tile.x);
                    while (!queue.empty() && !m_exhausted)
                    {
                        auto [cost, y, x] = queue.top();
                        queue.pop();
                        const int curCell = static_cast<int>(y * m_terrain.width + x);
                        if (cost != uCosts[curCell])
                            continue;
                        reachCells.push_back(curCell);
                        if (++m_settled > MAX_SETTLED_PER_REPLAY || !m_terrain.pay(ENTRY_DIRECTION_COUNT))
                        {
                            m_exhausted = true;
                            break;
                        }
                        for (const auto direction : ENTRY_DIRECTIONS)
                        {
                            const auto step = entryStep(direction);
                            TilePoint next{x + step.dx, y + step.dy};
                            const auto price = grid.cost(next.x, next.y, direction);
                            if (price < 0 || price > pawn.movement - cost)
                                continue;
                            const auto slot = cell(next);
                            if (uCosts[slot] >= 0 && uCosts[slot] <= cost + price)
                                continue;
                            uCosts[slot] = cost + price;
                            queue.emplace(uCosts[slot], next.y, next.x);
                        }
                    }
                    if (m_exhausted)
                        return computed;
                    uIt = cache->unblockedFields.emplace(
                        uKey, CombatMovementCache::UnblockedField{std::move(computed), std::move(reachCells)}).first;
                }
                if (pawn.ignoresCollision)
                {
                    ++cache->hits;
                    return uIt->second.costs;
                }
                std::vector<int> blocked;
                for (const int slot : uIt->second.reachableCells)
                {
                    if (slot == startCell)
                        continue;
                    const auto occupant = m_cells[slot];
                    const TilePoint pt{slot % m_terrain.width, slot / m_terrain.width};
                    if ((occupant != NO_UNIT && !m_units[occupant].enemy) ||
                        m_terrain.friendlyBirthSites.contains({pt.x, pt.y}))
                    {
                        blocked.push_back(slot);
                    }
                }
                if (blocked.empty())
                {
                    ++cache->hits;
                    return uIt->second.costs;
                }
                key = {pawn.grid, startCell, pawn.movement, false, std::move(blocked)};
                if (auto prior = cache->fields.find(key); prior != cache->fields.end())
                {
                    ++cache->hits;
                    return prior->second;
                }
                ++cache->misses;
            }
            auto computed = std::make_shared<std::vector<std::int32_t>>(m_cells.size(),UNREACHABLE);
            auto &costs = *computed;
            using Entry = std::tuple<std::int32_t, std::int32_t, std::int32_t>;
            std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
            if (!m_terrain.pay(static_cast<std::int64_t>(costs.size())))
            {
                m_exhausted = true;
                return computed;
            }
            costs[cell(pawn.tile)] = 0;
            queue.emplace(0, pawn.tile.y, pawn.tile.x);
            while (!queue.empty() && !m_exhausted)
            {
                auto [cost, y, x] = queue.top();
                queue.pop();
                if (cost != costs[cell({x, y})])
                    continue;
                if (++m_settled > MAX_SETTLED_PER_REPLAY || !m_terrain.pay(ENTRY_DIRECTION_COUNT))
                {
                    m_exhausted = true;
                    break;
                }
                for (const auto direction : ENTRY_DIRECTIONS)
                {
                    const auto step = entryStep(direction);
                    TilePoint next{x + step.dx, y + step.dy};
                    const auto price = grid.cost(next.x, next.y, direction);
                    if (price < 0 || price > pawn.movement - cost)
                        continue;
                    const auto slot = cell(next);
                    const auto occupant = m_cells[slot];
                    if (!pawn.ignoresCollision && ((occupant != NO_UNIT && !m_units[occupant].enemy) ||
                        m_terrain.friendlyBirthSites.contains({next.x,next.y})))
                        continue;
                    if (costs[slot] >= 0 && costs[slot] <= cost + price)
                        continue;
                    costs[slot] = cost + price;
                    queue.emplace(costs[slot], next.y, next.x);
                }
            }
            if (cache && !m_exhausted)
            {
                if (cache->fields.size() >= CombatMovementCache::maxEntries)
                    cache->fields.clear();
                cache->fields.emplace(std::move(key), computed);
            }
            return computed;
        }

        static constexpr std::int32_t MAX_SETTLED_PER_REPLAY = 100000;
        const CombatTerrain &m_terrain;
        std::vector<CombatPawn> m_units;
        std::vector<std::int32_t> m_cells;
        std::map<std::int32_t,CombatMovementCache::Costs> m_reach;
        std::int32_t m_settled{0};
        bool m_exhausted{false};
    };

    struct CaptureChip
    {
        int shooter{NO_UNIT};
        TilePoint from{INVALID_TILE};
        double targetBefore{}, targetAfter{};
        MilliFunds damage{}, counter{}, foregoneCapture{};
        MilliFunds exchange() const { return damage-counter-foregoneCapture; }
    };

    // One economically best physical witness per victim and remaining displayed HP.
    // Different hits in the same HP bucket have the same capture rate.
    using CaptureChips = std::map<std::pair<int,int>,CaptureChip>;

    struct LethalFiringThreat
    {
        int victim{NO_UNIT}, shooter{NO_UNIT};
        std::vector<TilePoint> origins;
    };

    inline std::optional<PlanReplyRun> combatPlanReply(const ResolvedPlanReply &resolved,
                                                       std::span<const PlanReplyAction> actions,
                                                       const CombatTerrain &terrain, std::vector<TilePoint> &locations,
                                                       std::map<int, int> &enemyReplyHp,
                                                       CombatEncirclement *containment = nullptr,
                                                       std::map<int,double> *realAfter = nullptr,
                                                       std::vector<LethalFiringThreat> *lethalThreats = nullptr,
                                                       CaptureChips *captureChips = nullptr,
                                                       std::optional<OpeningRecruitFight> *recruitFight=nullptr,
                                                       std::vector<TilePoint> *alternativeFiringSquares=nullptr,
                                                       std::vector<OpeningRecruitWindow> *openingWindows=nullptr,
                                                       std::vector<OpeningLaneAction> *servicedShots=nullptr,
                                                       std::vector<int> *counterWeapons=nullptr)
    {
        CombatReplyPosition position(terrain, actions, resolved);
        if (realAfter)
        {
            for (int i = 0; i < static_cast<int>(terrain.units.size()); ++i)
                if (terrain.units[i].enemy && terrain.units[i].present && terrain.units[i].realHp >= 0)
                    (*realAfter)[i] = terrain.units[i].realHp;
            for (const auto &[i, hp] : resolved.own.targetRealAfter)
                if (!isTerrainTargetId(i))
                    (*realAfter)[i] = hp;
        }
        PlanReplyRun run;
        for (const auto &[index, damage] : resolved.own.chain.chain)
            if (!isTerrainTargetId(index))
                enemyReplyHp[index] = damage.after;
        for (const auto &[index, startHp] : resolved.startHp)
        {
            run.pool.push_back(index);
            const auto changed = resolved.own.chain.chain.find(index);
            run.poolHp.push_back(changed == resolved.own.chain.chain.end() ? startHp : changed->second.after);
        }
        std::vector<int> targetUnits;
        std::vector<bool> completedCaptures;
        for(const auto &action:actions){
            const auto &bundle=action.pCandidate->bundle;
            targetUnits.push_back(bundle.unitId);
            completedCaptures.push_back(std::any_of(bundle.components.begin(),bundle.components.end(),[](const auto &c){
                return c.kind==ComponentKind::Capture && c.capture.turnsUntilOwned==0;
            }));
        }
        for(const auto &defender:terrain.observedDefenders){
            targetUnits.push_back(defender.knowledgeIndex);
            completedCaptures.push_back(false);
        }
        if(targetUnits.size()!=resolved.targets.size())return std::nullopt;
        // All defenders share the same E0 resource timeline, including those
        // whose F0 shot is part of this plan rather than the observed prefix.
        std::vector<std::array<int,2>> counterLoad;
        for(std::size_t i=0;i<targetUnits.size();++i){
            auto rounds=terrain.units.at(targetUnits[i]).ammoByWeapon;
            if(i<resolved.own.ownShots.size()){
                const auto &own=resolved.own.ownShots[i];
                if(own.target!=NO_UNIT)spendCombatRound(rounds,own.report.weapon);
            }
            counterLoad.push_back(rounds);
        }
        // Refuse oversized pair sets before allocating or repeatedly scanning them.
        constexpr std::size_t MAX_PAIR_CELLS = 24000;
        if (!resolved.targets.empty() && run.pool.size() > MAX_PAIR_CELLS / resolved.targets.size())
            return std::nullopt;
        if (!terrain.pay(static_cast<std::int64_t>(resolved.targets.size()) * (1 + run.pool.size()) * (1 + run.pool.size()) +
                         static_cast<std::int64_t>(terrain.width) * terrain.height))
            return std::nullopt;
        struct Pair
        {
            const ReplyShooterFacts *facts{};
            const ReplyShotBase *base{};
            TilePoint firing{INVALID_TILE};
            ResolvedReplyShot shot;
            double firstHitRemaining{-1},rawPrimary{};
            std::optional<OpeningLaneExchange> native;
            std::vector<CombatFiringOption> firingOptions;
        };
        const std::size_t targetStride = resolved.targets.size();
        std::vector<Pair> pairsFlat(run.pool.size() * targetStride);
        const auto pairAt = [&](std::size_t s, std::size_t t) -> Pair & {
            return pairsFlat[s * targetStride + t];
        };
        std::vector<OneActionTarget> targets;
        for (std::size_t t = 0; t < resolved.targets.size(); ++t)
        {
            targets.push_back(resolved.targets[t].target);
            const auto &entry = resolved.entries[t];
            for (const auto slot : entry.shooterSlots)
            {
                const auto &facts = entry.pFacts->tile->shooters[slot];
                const auto s =
                    std::lower_bound(run.pool.begin(), run.pool.end(), facts.shooterIndex) - run.pool.begin();
                const auto counter = entry.pFacts->shots[slot].counterRawBase;
                if (!std::isfinite(counter) || counter > std::numeric_limits<std::int32_t>::max())
                    return std::nullopt;
                auto & cell = pairAt(static_cast<std::size_t>(s), t);
                cell.facts = &facts;
                cell.base = &entry.pFacts->shots[slot];
            }
        }
        CombatEncirclement trap;
        bool contain = false;
        std::vector<bool> spent(terrain.units.size());
        std::vector<TilePoint> reserved(terrain.units.size(), INVALID_TILE);
        std::vector<double> realHp;
        for (std::size_t t = 0; t < resolved.targets.size(); ++t)
        {
            realHp.push_back(targets[t].hp);
            if (terrain.policy == TacticalReply::ContainUnit && terrain.replyTarget != NO_UNIT &&
                targets[t].unitId == terrain.replyTarget)
            {
                contain = true;
                trap.target = terrain.replyTarget;
                trap.targetKnowledge = targetUnits[t];
                trap.center = resolved.entries[t].destination;
            }
        }
        const auto counterDamage = [&](const ResolvedReplyShot &shot, int shooter, TilePoint from, int after, double targetBefore, double rawPrimary) {
            if (shot.counterRawBase < 0 || after <= 0)
                return 0.;
            return terrain.defense ? engineShotDamage(sameActionCounterDamage(terrain.defense(shooter, from),
                                         shot.counterRawBase,targetBefore,rawPrimary,shot.pricedHpSteps))
                                   : hpScaledBase(shot.counterRawBase, after);
        };
        const auto shooterRealHp = [&](int shooter, int pricedSteps, bool onBuilding) {
            if(terrain.orderedOpening)return terrain.orderedOpening->afterService.at(shooter).hp/10.;
            const auto changed = resolved.own.targetRealAfter.find(shooter);
            double hp = terrain.units[shooter].realHp;
            if (changed != resolved.own.targetRealAfter.end())
                hp = changed->second;
            else if (const auto legacy = resolved.own.chain.chain.find(shooter); legacy != resolved.own.chain.chain.end())
                hp = legacy->second.after;
            if (hp < 0)
                return static_cast<double>(pricedSteps);
            if (onBuilding && ceilHpSteps(hp) < pricedSteps)
                hp = std::min<double>(UNIT_HP_STEPS, hp + BUILDING_REPAIR_STEPS);
            return hp;
        };
        const auto counterSteps = [&](const Pair &pair, int shooter, double damage) {
            const int hp = pair.shot.pricedHpSteps;
            const auto before = shooterRealHp(shooter, hp, pair.facts->onOwnBuilding);
            return hp - ceilHpSteps(engineHpAfterReply(before, damage));
        };
        std::vector<TilePoint> firedFrom;
        bool incompleteOpening=false;
        const auto *ordered=terrain.orderedOpening;
        const auto orders=ordered?std::span<const OpeningLaneAction>(ordered->prefix):std::span<const OpeningLaneAction>{};
        OrderedOpeningReplay prefix(orders);
        OpeningLaneState orderedState;
        std::optional<OpeningLaneAction> prepared;
        if(ordered){
            if(!position.sameLivingRoster(ordered->afterService) || !terrain.workLeft || !ordered->exchange)return std::nullopt;
            orderedState=ordered->afterService;
            for(std::size_t i=0;i<orderedState.size();++i)if(terrain.units[i].enemy){
                enemyReplyHp[i]=ceilHpSteps(orderedState[i].hp/10.);
                if(realAfter)(*realAfter)[i]=orderedState[i].hp/10.;
            }
        }
        const auto reserveContainmentPosts=[&] {
            const int loan=terrain.openingRecruitTarget ? terrain.openingRecruitTarget->shooter : NO_UNIT;
            const bool wasSpent=loan!=NO_UNIT && spent.at(loan);
            if(loan!=NO_UNIT) spent[loan]=true;
            position.reserveExits(trap.targetKnowledge,spent,reserved,trap);
            if(loan!=NO_UNIT) spent[loan]=wasSpent;
        };
        const auto tryNewborn=[&](bool afterAll,bool beforeBlocks=false) {
            const auto *target=terrain.openingRecruitTarget;
            if(!target || incompleteOpening || !prefix.complete()) return;
            if(!recruitFight || !terrain.workLeft || !terrain.defense){incompleteOpening=true;return;}
            if(*recruitFight) return;
            const int prefix=static_cast<int>(firedFrom.size());
            const bool collect=target->afterStockFires==OpeningRecruitTarget::COLLECT;
            if(collect)
            {
                if(afterAll && !(contain && trap.openerKnowledge==NO_UNIT && !trap.blockOrders.empty()))return;
            }
            else if(beforeBlocks!=target->beforeBlockOrders || afterAll!=(target->afterStockFires==OpeningRecruitTarget::AFTER_ALL) ||
                    (!afterAll && target->afterStockFires!=prefix))return;
            const int gun=target->shooter;
            if(gun<0 || gun>=static_cast<int>(terrain.units.size())){incompleteOpening=true;return;}
            if(spent[gun] || reserved[gun]!=INVALID_TILE)return;
            const auto &pawn=terrain.units[gun];bool complete=true;
            const auto options=openingRecruitShots(gun,pawn.tile,target->factory,terrain.width,terrain.height,
                target->minRange,target->maxRange,pawn.grid>=0,ordered?orderedState[gun].hp/10.:target->stockHp,10.,
                pawn.replacementCost,target->recruitCost,ordered?orderedState[gun].rounds:target->stockLoad,target->recruitLoad,target->defense,
                [&](TilePoint from){return position.movementCost(gun,from);},target->powers,
                [&](TilePoint from){return terrain.defense(gun,from);},*terrain.workLeft,complete);
            if(!complete || position.exhausted()){incompleteOpening=true;return;}
            if(collect)
            {
                if(!openingWindows){incompleteOpening=true;return;}
                const int time=afterAll ? OpeningRecruitTarget::AFTER_ALL : prefix;
                for(const auto &shot:options)openingWindows->push_back({time,shot.from,beforeBlocks});
                return;
            }
            if(alternativeFiringSquares)
                for(const auto &shot:options)alternativeFiringSquares->push_back(shot.from);
            const auto selected=std::find_if(options.begin(),options.end(),[&](const auto &shot){
                return target->from==INVALID_TILE || target->from==shot.from;
            });
            if(selected==options.end())return;
            *recruitFight=*selected;
            spent[gun]=true;
            position.applyOpeningFight(*selected);
            if(ordered){
                auto &a=orderedState[gun];
                a.at=selected->from;a.hp=static_cast<int>(std::lround(10*selected->stockAfter));a.spent=true;
                if(a.fuel>=0)a.fuel-=selected->movementCost;
                if(!debitOpeningLaneRound(a.rounds,selected->weapon)){incompleteOpening=true;return;}
            }
            const int hp=ceilHpSteps(selected->stockAfter);
            enemyReplyHp[gun]=hp;
            if(realAfter)(*realAfter)[gun]=selected->stockAfter;
            const auto found=std::lower_bound(run.pool.begin(),run.pool.end(),gun);
            if(found!=run.pool.end() && *found==gun)run.poolHp[found-run.pool.begin()]=hp;
        };
        tryNewborn(false);
        run.reply = oneActionReply(
            targets, run.pool,
            [&](std::size_t s, std::size_t t) {
                auto &pair = pairAt(s, t);
                const auto *forced=prefix.next();
                if(forced && (run.pool[s]!=forced->shot.shooter || targetUnits[t]!=forced->shot.target))return false;
                if (terrain.policy == TacticalReply::BypassUnit && terrain.replyTarget != NO_UNIT && targets[t].unitId == terrain.replyTarget)
                    return false;
                if (terrain.policy == TacticalReply::InfantryCapture && terrain.units[run.pool[s]].capper)
                    return false;
                if (terrain.policy == TacticalReply::PreserveCapture && terrain.units[run.pool[s]].carrying)
                    return false;
                if (!pair.facts || run.poolHp[s] <= 0 || position.exhausted())
                    return false;
                const auto id = run.pool[s];
                if (terrain.openingRecruitTarget && terrain.openingRecruitTarget->shooter==id) return false;
                if (terrain.policy == TacticalReply::WaitOnProperty && terrain.units[id].engineUnitId == terrain.replyTarget)
                    return false;
                const bool focus = contain && trap.targetKnowledge == targetUnits[t];
                if (focus && (trap.openerKnowledge != NO_UNIT || (terrain.containShooter != NO_UNIT && terrain.containShooter != id)))
                    return false;
                const auto &entry = resolved.entries[t];
                const auto shotAt = [&](TilePoint from) {
                    auto base = *pair.base;
                    const int distance = manhattanDistance(from, entry.destination);
                    if ((focus || terrain.terrainAwareFiringTile) && terrain.weapon && distance != pair.facts->distance)
                    {
                        // The cached weapon slots describe the sampled range. Do not carry an
                        // indirect weapon or a speculative reload over to a different range.
                        base.rawBase = terrain.weapon(id, targetUnits[t], distance);
                        base.withAmmo = base.rawBase;
                    }
                    if(!ordered && terrain.weaponSlots && drawsCounter(pair.facts->direct,from,entry.destination))
                        base.counterRawBase=supportWeapon(terrain,targetUnits[t],id,distance,counterLoad[t]).base;
                    auto shot=resolveReplyShot(*pair.facts, base, run.poolHp[s], from, entry.destination);
                    if(ordered)shot.pricedHpSteps=ceilHpSteps(orderedState[id].hp/10.);
                    return shot;
                };
                pair.firingOptions.clear();
                const auto fixed = forced ? forced->shot.from : focus && terrain.containFrom != INVALID_TILE ? terrain.containFrom : reserved[id];
                pair.firing = position.firingTile(*pair.facts, fixed, [&](TilePoint from) {
                    if (!focus && !terrain.terrainAwareFiringTile)
                        return std::pair{0., 0};
                    const auto defended = shotAt(from);
                    if (!pricedShotPossible(defended.resolution))
                        return std::pair{std::numeric_limits<double>::infinity(), 0};
                    const double damage = pricedTerrainDamage(entry.pFacts->terrain, defended,
                                                               defended.pricedHpSteps, ceilHpSteps(realHp[t]));
                    const int after = ceilHpSteps(engineHpAfterReply(realHp[t], engineShotDamage(damage)));
                    const double counter = counterDamage(defended,id,from,after,realHp[t],damage);
                    const auto rule = terrain.defense ? terrain.defense(id, from) : TerrainDefenseRule{};
                    const int hp = defended.pricedHpSteps;
                    if (focus)
                    {
                        const double before = shooterRealHp(id, hp, pair.facts->onOwnBuilding);
                        const double remaining = engineHpAfterReply(before, counter);
                        pair.firingOptions.push_back({from, position.movementCost(id, from), rule.stars,
                            hp - ceilHpSteps(remaining), before - remaining});
                    }
                    return std::pair{counter, terrainDefenseBonus(rule, hp)};
                });
                if (pair.firing == INVALID_TILE)
                    return false;
                if(forced){
                    if(!terrain.workLeft || std::lround(10*realHp[t])!=forced->targetBefore ||
                       !position.sameLivingRoster(orderedState)){incompleteOpening=true;return false;}
                    if(!prepared){
                        auto trial=orderedState;
                        auto move=[&](const OpeningLaneState&,int actor,TilePoint to,std::int64_t&){return position.movementCost(actor,to);};
                        const int protectedGun=ordered->reservedShooter;
                        if(terrain.openingRecruitTarget && protectedGun!=terrain.openingRecruitTarget->shooter){incompleteOpening=true;return false;}
                        prepared=applyOpeningLaneShot(trial,forced->shot,protectedGun,move,ordered->exchange,*terrain.workLeft);
                        if(!prepared){incompleteOpening=true;return false;}
                    }
                }
                pair.shot = shotAt(pair.firing);
                pair.native.reset();
                if(ordered && !forced){
                    const int victim=targetUnits[t];
                    if(!terrain.workLeft || orderedState[victim].hp!=std::lround(realHp[t]*10)){
                        incompleteOpening=true;return false;
                    }
                    pair.native=ordered->exchange(orderedState,OpeningLaneShot{id,victim,pair.firing},*terrain.workLeft);
                    if(!pair.native || !std::isfinite(pair.native->hit) || !std::isfinite(pair.native->counter) ||
                       pair.native->hit>10000 || pair.native->counter>10000){incompleteOpening=true;return false;}
                    if(pair.native->hit<0)return false;
                }
                if (!pair.native && !pricedShotPossible(pair.shot.resolution))
                    return false;
                if(reserved[id]!=INVALID_TILE && pair.native)
                {
                    if(static_cast<int>(std::max(0.,pair.native->counter))>=orderedState[id].hp)return false;
                }
                else if (reserved[id] != INVALID_TILE)
                {
                    const auto damage = pricedTerrainDamage(entry.pFacts->terrain, pair.shot, pair.shot.pricedHpSteps,
                                                             ceilHpSteps(realHp[t]));
                    const int after = ceilHpSteps(engineHpAfterReply(realHp[t], engineShotDamage(damage)));
                    if (counterSteps(pair,id,counterDamage(pair.shot,id,pair.firing,after,realHp[t],damage)) >= pair.shot.pricedHpSteps)
                        return false;
                }
                return true;
            },
            [&](std::size_t s, std::size_t t, std::int32_t hp) {
                auto &pair=pairAt(s, t);
                const auto &shot=pair.shot;
                if(prefix.next() && prepared)return prepared->native.hit;
                const auto damage=pair.native?pair.native->hit:pricedTerrainDamage(resolved.entries[t].pFacts->terrain,shot,shot.pricedHpSteps,hp);
                pair.rawPrimary=damage;
                if (captureChips && terrain.policy==TacticalReply::Economic && firedFrom.empty() &&
                    terrain.units[targetUnits[t]].capper)
                    pair.firstHitRemaining=engineHpAfterReply(realHp[t],engineShotDamage(damage));
                return damage;
            },
            [&](std::size_t s, std::size_t t, std::int32_t hp) {
                const auto &pair = pairAt(s, t);
                if(prefix.next() && prepared)return combatCapitalLoss(pair.facts->cost,prepared->shooterBefore/10.,prepared->shooterAfter/10.);
                if (terrain.ownFire)
                {
                    const int i = run.pool[s];
                    const double before = shooterRealHp(i, pair.shot.pricedHpSteps, pair.facts->onOwnBuilding);
                    const auto counter=combatCapitalLoss(pair.facts->cost,before,
                        engineHpAfterReply(before,pair.native?engineShotDamage(pair.native->counter):counterDamage(pair.shot,i,pair.firing,hp,realHp[t],pair.rawPrimary)));
                    const double left=pair.firstHitRemaining;
                    if (captureChips && firedFrom.empty() && left>0 && ceilHpSteps(left)<ceilHpSteps(realHp[t]) &&
                        terrain.pay(1))
                    {
                        // Geometry, weapon, ammo and counter are exactly those already resolved
                        // for this legal E0 opening. Capture before the greedy net filter can
                        // spend that opposing unit elsewhere. No hypothetical damage is booked.
                        const int victim=targets[t].unitId;
                        CaptureChip offered{i,pair.firing,realHp[t],left,
                            combatCapitalLoss(targets[t].cost,realHp[t],left),counter,pair.facts->captureCost};
                        auto [it,fresh]=captureChips->try_emplace({victim,hp},offered);
                        if (!fresh && offered.exchange()>it->second.exchange()) it->second=offered;
                    }
                    return counter;
                }
                return bookDamage(pair.facts->cost,counterSteps(pair,run.pool[s],
                    counterDamage(pair.shot,run.pool[s],pair.firing,hp,realHp[t],pair.rawPrimary)));
            },
            [&](std::size_t s, std::size_t t) { return pairAt(s, t).facts->captureCost; },
            [&](const OneActionShot &chosen) {
                const auto &pair = pairAt(chosen.shooter, chosen.target);
                const auto hp = pair.shot.pricedHpSteps;
                const auto id = run.pool[chosen.shooter];
                const auto *expected=prefix.next();
                auto actual=prepared;
                if(!expected && pair.native){
                    const int victim=targetUnits[chosen.target];
                    const auto &a=orderedState[id];const auto &v=orderedState[victim];
                    const auto block=std::find_if(trap.blockOrders.begin(),trap.blockOrders.end(),
                        [&](const auto &b){return b.unitIndex==id && b.destination==pair.firing;});
                    const int cost=block==trap.blockOrders.end()?position.movementCost(id,pair.firing):block->movementCost;
                    if(cost<0 || (a.fuel>=0 && a.fuel<cost)){incompleteOpening=true;return;}
                    actual=OpeningLaneAction{{id,victim,pair.firing},*pair.native,cost,a.hp,
                        std::max(0,a.hp-static_cast<int>(std::max(0.,pair.native->counter))),v.hp,
                        std::max(0,v.hp-static_cast<int>(pair.native->hit)),a.rounds,v.rounds,a.at,v.at};
                }
                if(expected && (!actual || !prefix.accept(*actual))){incompleteOpening=true;return;}
                // The primary and its counter share this one exchange. A later primary rounds
                // its shooter HP, but rounding the surviving target here can change the target selected.
                const auto returned=actual ? actual->native.counter :
                    counterDamage(pair.shot,id,pair.firing,chosen.hpStepsAfter,realHp[chosen.target],pair.rawPrimary);
                const auto counter = actual ? hp-ceilHpSteps(actual->shooterAfter/10.) : counterSteps(pair,id,returned);
                const auto damage = pricedTerrainDamage(resolved.entries[chosen.target].pFacts->terrain, pair.shot,
                                                         hp, chosen.hpStepsBefore);
                realHp[chosen.target] = actual?actual->targetAfter/10.:engineHpAfterReply(realHp[chosen.target], engineShotDamage(damage));
                if(actual){
                    if(servicedShots)servicedShots->push_back(*actual);
                    const int victim=actual->shot.target;
                    auto &a=orderedState[id],&v=orderedState[victim];
                    a.hp=actual->shooterAfter;v.hp=actual->targetAfter;a.at=actual->shot.from;a.spent=true;
                    if(a.fuel>=0)a.fuel-=actual->moveCost;
                    if(!debitOpeningLaneRound(a.rounds,actual->native.weapon) ||
                       (actual->native.counter>=0 && !debitOpeningLaneRound(v.rounds,actual->native.counterWeapon)) ||
                       chosen.hpStepsAfter!=ceilHpSteps(realHp[chosen.target])){incompleteOpening=true;return;}
                    prepared.reset();
                }
                int counterWeapon=-1;
                if(actual)counterWeapon=actual->native.counter>=0?actual->native.counterWeapon:-1;
                else if(chosen.hpStepsAfter>0 && pair.shot.counterRawBase>=0 && terrain.weaponSlots){
                    const int t=targetUnits[chosen.target];
                    auto &load=counterLoad[chosen.target];
                    const int range=manhattanDistance(pair.firing,resolved.entries[chosen.target].destination);
                    counterWeapon=supportWeapon(terrain,t,id,range,load).slot;
                    spendCombatRound(load,counterWeapon);
                }
                if(counterWeapons)counterWeapons->push_back(counterWeapon);
                spent[id] = true;
                if (realAfter)
                    (*realAfter)[id] = actual ? actual->shooterAfter/10. : engineHpAfterReply(shooterRealHp(id, hp, pair.facts->onOwnBuilding),
                        returned);
                enemyReplyHp[run.pool[chosen.shooter]] = std::max(0, hp - counter);
                if (lethalThreats && chosen.hpStepsAfter == 0 && pair.facts->direct &&
                    terrain.units[targetUnits[chosen.target]].capper &&
                    (pair.facts->firingTiles.size()==1 || position.hasReachField(id)) &&
                    pair.facts->firingTiles.size() <= 12 && terrain.pay(1 + pair.facts->firingTiles.size()))
                {
                    // Read the same occupied board and reach field the chosen shot used,
                    // before that shot moves the gun and removes the victim. A focus policy's
                    // requested firing tile is not a physical restriction on this shooter.
                    LethalFiringThreat threat{targets[chosen.target].unitId,id,{}};
                    const auto targetTile = resolved.entries[chosen.target].destination;
                    for (const auto &from : pair.facts->firingTiles)
                    {
                        if (position.movementCost(id,from) < 0) continue;
                        const int range = manhattanDistance(from,targetTile);
                        if (from != pair.firing && range != pair.facts->distance && terrain.weapon &&
                            terrain.weapon(id,targetUnits[chosen.target],range) <= 0)
                            continue;
                        if (std::find(threat.origins.begin(),threat.origins.end(),from) == threat.origins.end())
                            threat.origins.push_back(from);
                        if (threat.origins.size()>2) break;
                    }
                    if (!threat.origins.empty() && threat.origins.size()<=2)
                        lethalThreats->push_back(std::move(threat));
                }
                position.apply(run.pool[chosen.shooter], pair.firing, hp > counter,
                               targetUnits[chosen.target], chosen.hpStepsAfter > 0);
                firedFrom.push_back(pair.firing);
                if (contain && targets[chosen.target].unitId == trap.target && trap.openerKnowledge == NO_UNIT)
                {
                    trap.opener = terrain.units[id].engineUnitId;
                    trap.openerKnowledge = id;
                    trap.firingTile = pair.firing;
                    trap.counterSteps = counter;
                    trap.firingOptions = pair.firingOptions;
                    tryNewborn(false,true);
                    reserveContainmentPosts();
                }
                else if (reserved[id] != INVALID_TILE)
                    for (auto &order : trap.blockOrders)
                        if (order.unitIndex == id)
                            order.fired = true;
                tryNewborn(false);
            },
            [&](std::size_t shooter, std::size_t victim) {
                if(prefix.next())return 1;
                if (contain)
                    return targets[victim].unitId == trap.target && trap.openerKnowledge == NO_UNIT ? 1 : 0;
                if (terrain.policy == TacticalReply::TargetUnit)
                    return terrain.replyTarget != NO_UNIT && targets[victim].unitId == terrain.replyTarget ? 1 : 0;
                if (terrain.policy == TacticalReply::Economic || terrain.policy == TacticalReply::PreserveCapture ||
                    terrain.policy == TacticalReply::InfantryCapture || terrain.policy == TacticalReply::BypassUnit ||
                    terrain.policy == TacticalReply::WaitOnProperty)
                    return 0;
                const auto at=resolved.entries[victim].destination;
                const int index=targetUnits[victim];
                if (terrain.policy == TacticalReply::ClearGuard)
                    return at == terrain.guardedHQ ? 2 : 0;

                // An infantry on a capturable property remains a target if it issued WAIT
                // or FIRE. Only a completed transfer removes this particular capture threat.
                const bool capping=terrain.units[index].capper && terrain.contestableSites.contains({at.x,at.y}) && !completedCaptures[victim];
                const bool vehicle = !terrain.units[index].capper;
                if (terrain.policy == TacticalReply::InterruptCapture)
                    return capping ? 1 : 0;
                const bool infantry = terrain.units[run.pool[shooter]].capper;
                return ((infantry && capping) || (!infantry && vehicle)) ? 1 : 0;
            }, static_cast<bool>(terrain.ownFire));
        if (position.exhausted())
            return std::nullopt;
        if (contain && trap.targetKnowledge != NO_UNIT)
        {
            if (trap.openerKnowledge == NO_UNIT)
                reserveContainmentPosts();
            if (position.exhausted())
                return std::nullopt;
        }
        tryNewborn(true);
        if(incompleteOpening || !prefix.complete()) return std::nullopt;
        if (contain && trap.targetKnowledge!=NO_UNIT)
        {
            position.refreshExits(trap);
            if (containment) *containment=std::move(trap);
        }
        locations = position.locations();
        for (std::size_t i = 0; i < firedFrom.size(); ++i)
            run.reply.shots[i].firingTile = firedFrom[i];
        return run;
    }
} // namespace Coordinator
