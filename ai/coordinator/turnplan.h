#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <QString>

#include "ai/coordinator/coordinatorcommon.h"
#include "ai/coordinator/economicledger.h"
#include "ai/coordinator/terminalvalue.h"

namespace Coordinator
{
    enum class PlanActionState : std::int8_t
    {
        Pending,
        Blocked,
        Committed,
        Abandoned,
    };

    enum class PlanEdgeKind : std::int8_t
    {
        // the destination occupant must act first
        Vacate,
        // the wall unit must die before the route opens
        Wallbreak,
        // the blocking position must exist first
        Screen,
        // the shot resolves before nearby movement
        Coverage,
        // the service tile is claimed before approach
        Service,
    };

    enum class ReservationResult : std::int8_t
    {
        Granted,
        Conflict,
        Overkill,
        // a later claim disagrees about remaining hp
        StaleTarget,
        // malformed request, not a battlefield answer
        Invalid,
    };

    constexpr std::int32_t NO_ACTION = -1;

    struct PlannedAction
    {
        std::int32_t actionIndex{NO_ACTION};
        // engine unique unit id, stable across the turn
        std::int32_t unitId{NO_UNIT};
        PlanBundleKind kind{PlanBundleKind::Wait};
        // engine action id, an open set mods extend
        QString actionId;
        std::vector<TilePoint> path;
        // where the unit ends the action
        TilePoint destination{INVALID_TILE};
        // attack target tile or capture building tile
        TilePoint target{INVALID_TILE};
        std::int32_t targetUnitId{NO_UNIT};
        // section 13.1 lexicographic action value
        TerminalValue marginalValue{};
        std::int32_t plannedDamageSteps{0};
        PlanActionState state{PlanActionState::Pending};
    };

    struct PlanEdge
    {
        std::int32_t from{NO_ACTION};
        std::int32_t to{NO_ACTION};
        PlanEdgeKind kind{PlanEdgeKind::Vacate};
    };

    struct OccupancyEntry
    {
        std::int32_t unitId{NO_UNIT};
        TilePoint tile{INVALID_TILE};
    };

    // the occupant stays, so the planner compares values
    struct VacateConflict
    {
        std::int32_t moverAction{NO_ACTION};
        std::int32_t occupantUnit{NO_UNIT};
        std::int32_t occupantAction{NO_ACTION};
    };

    struct OrderingResult
    {
        std::vector<std::int32_t> order;
        // members of a strongly connected component
        std::vector<std::int32_t> cycleMembers;
        // unordered only because a cycle sits upstream
        std::vector<std::int32_t> blockedByCycle;
    };

    // every mutator reports what its cascade blocked, ascending
    struct AttackClaim
    {
        ReservationResult result{ReservationResult::Invalid};
        std::vector<std::int32_t> blocked;
    };

    struct PlanChange
    {
        bool accepted{false};
        std::vector<std::int32_t> blocked;
    };

    struct ConflictOutcome
    {
        std::int32_t loser{NO_ACTION};
        std::vector<std::int32_t> blocked;
    };

    constexpr bool isMovingBundle(PlanBundleKind kind)
    {
        return kind == PlanBundleKind::Move
               || kind == PlanBundleKind::MoveAndFire
               || kind == PlanBundleKind::MoveAndCapture;
    }

    constexpr bool isLiveState(PlanActionState state)
    {
        return state == PlanActionState::Pending || state == PlanActionState::Committed;
    }

    // higher value first, unit id is the final tie break
    inline bool outranksInPlan(const PlannedAction & candidate, const PlannedAction & incumbent)
    {
        if (candidate.marginalValue != incumbent.marginalValue)
        {
            return candidate.marginalValue > incumbent.marginalValue;
        }
        return candidate.unitId < incumbent.unitId;
    }

    class TurnPlan
    {
    public:
        TurnPlan() = default;

        // NO_ACTION on a duplicate unit or an inconsistent path
        std::int32_t addAction(PlannedAction action)
        {
            if (!acceptableShape(action) || actionOfUnit(action.unitId) != NO_ACTION)
            {
                return NO_ACTION;
            }
            const std::int32_t index = actionCount();
            action.actionIndex = index;
            m_actions.push_back(std::move(action));
            return index;
        }

        // the index and unit id survive, the old claims do not
        PlanChange replan(std::int32_t actionIndex, PlannedAction replacement)
        {
            if (!hasAction(actionIndex) || action(actionIndex).state == PlanActionState::Committed)
            {
                return PlanChange{};
            }
            replacement.unitId = action(actionIndex).unitId;
            if (!acceptableShape(replacement))
            {
                return PlanChange{};
            }
            PlanChange change{true, invalidateWallbreaksOnTargets(releaseClaims(actionIndex))};
            replacement.actionIndex = actionIndex;
            replacement.state = PlanActionState::Pending;
            m_actions[static_cast<std::size_t>(actionIndex)] = std::move(replacement);
            return change;
        }

        std::int32_t actionCount() const
        {
            return static_cast<std::int32_t>(m_actions.size());
        }

        bool hasAction(std::int32_t index) const
        {
            return index >= 0 && index < actionCount();
        }

        // precondition: hasAction(index)
        const PlannedAction & action(std::int32_t index) const
        {
            return m_actions[static_cast<std::size_t>(index)];
        }

        // only a pending action can be committed
        bool commit(std::int32_t index)
        {
            if (!hasAction(index) || action(index).state != PlanActionState::Pending)
            {
                return false;
            }
            m_actions[static_cast<std::size_t>(index)].state = PlanActionState::Committed;
            return true;
        }

        ReservationResult claimDestination(std::int32_t actionIndex, TilePoint tile)
        {
            return claimTile(m_destinationClaims, actionIndex, tile);
        }

        std::int32_t destinationClaimant(TilePoint tile) const
        {
            return tileClaimant(m_destinationClaims, tile);
        }

        ReservationResult claimCapture(std::int32_t actionIndex, TilePoint buildingTile)
        {
            return claimTile(m_captureClaims, actionIndex, buildingTile);
        }

        std::int32_t captureClaimant(TilePoint buildingTile) const
        {
            return tileClaimant(m_captureClaims, buildingTile);
        }

        ReservationResult claimService(std::int32_t actionIndex, std::int32_t partnerUnitId)
        {
            if (!hasAction(actionIndex) || partnerUnitId == NO_UNIT)
            {
                return ReservationResult::Invalid;
            }
            const std::size_t slot = findService(partnerUnitId);
            if (slot < m_serviceClaims.size())
            {
                if (m_serviceClaims[slot].actionIndex == actionIndex)
                {
                    return ReservationResult::Granted;
                }
                return ReservationResult::Conflict;
            }
            m_serviceClaims.push_back(ServiceClaim{partnerUnitId, actionIndex});
            return ReservationResult::Granted;
        }

        std::int32_t serviceClaimant(std::int32_t partnerUnitId) const
        {
            const std::size_t slot = findService(partnerUnitId);
            if (slot < m_serviceClaims.size())
            {
                return m_serviceClaims[slot].actionIndex;
            }
            return NO_ACTION;
        }

        // a repeat claim replaces this action's own share
        AttackClaim claimAttack(std::int32_t actionIndex, std::int32_t targetUnitId,
                                std::int32_t remainingHpSteps, std::int32_t damageSteps)
        {
            if (!hasAction(actionIndex) || targetUnitId == NO_UNIT)
            {
                return AttackClaim{};
            }
            if (!isHpStepCount(remainingHpSteps) || !isHpStepCount(damageSteps))
            {
                return AttackClaim{};
            }
            const std::size_t known = findDamage(targetUnitId);
            std::int32_t otherShares = 0;
            if (known < m_damageClaims.size())
            {
                if (m_damageClaims[known].remainingHpSteps != remainingHpSteps)
                {
                    return AttackClaim{ReservationResult::StaleTarget, {}};
                }
                otherShares = m_damageClaims[known].plannedHpSteps - contributionOf(m_damageClaims[known], actionIndex);
            }
            // a refused claim must not leave a booking behind
            if (otherShares >= remainingHpSteps)
            {
                return AttackClaim{ReservationResult::Overkill, {}};
            }
            const std::int32_t granted = std::min(damageSteps, remainingHpSteps - otherShares);
            DamageClaim & claim = damageClaimFor(targetUnitId, remainingHpSteps);
            claim.plannedHpSteps = otherShares + granted;
            setContribution(claim, actionIndex, granted);
            m_actions[static_cast<std::size_t>(actionIndex)].plannedDamageSteps = granted;
            return AttackClaim{ReservationResult::Granted, invalidateWallbreaksOn(targetUnitId)};
        }

        // 0 when nothing is booked on the target
        std::int32_t plannedDamageOn(std::int32_t targetUnitId) const
        {
            const std::size_t slot = findDamage(targetUnitId);
            if (slot < m_damageClaims.size())
            {
                return m_damageClaims[slot].plannedHpSteps;
            }
            return 0;
        }

        // 0 when nothing is booked on the target
        std::int32_t remainingHpStepsOn(std::int32_t targetUnitId) const
        {
            const std::size_t slot = findDamage(targetUnitId);
            if (slot < m_damageClaims.size())
            {
                return m_damageClaims[slot].remainingHpSteps;
            }
            return 0;
        }

        bool targetIsLethal(std::int32_t targetUnitId) const
        {
            const std::size_t slot = findDamage(targetUnitId);
            if (slot >= m_damageClaims.size())
            {
                return false;
            }
            return m_damageClaims[slot].plannedHpSteps >= m_damageClaims[slot].remainingHpSteps;
        }

        // false on an unknown index, self loop, or repeated pair
        bool addEdge(std::int32_t fromActionIndex, std::int32_t toActionIndex, PlanEdgeKind kind)
        {
            if (!hasAction(fromActionIndex) || !hasAction(toActionIndex) || fromActionIndex == toActionIndex)
            {
                return false;
            }
            for (const PlanEdge & dependency : m_edges)
            {
                if (dependency.from == fromActionIndex && dependency.to == toActionIndex)
                {
                    return false;
                }
            }
            m_edges.push_back(PlanEdge{fromActionIndex, toActionIndex, kind});
            return true;
        }

        // the push is legal only once the kill reaches lethal
        bool addWallbreakEdge(std::int32_t attackIndex, std::int32_t pushIndex)
        {
            if (!hasAction(attackIndex) || !targetIsLethal(action(attackIndex).targetUnitId))
            {
                return false;
            }
            return addEdge(attackIndex, pushIndex, PlanEdgeKind::Wallbreak);
        }

        std::int32_t edgeCount() const
        {
            return static_cast<std::int32_t>(m_edges.size());
        }

        // precondition: 0 <= index < edgeCount()
        const PlanEdge & edge(std::int32_t index) const
        {
            return m_edges[static_cast<std::size_t>(index)];
        }

        // an occupant that plans to stay is a conflict, not an edge
        std::vector<VacateConflict> addVacateEdges(std::span<const OccupancyEntry> occupancy)
        {
            std::vector<VacateConflict> conflicts;
            for (std::int32_t index = 0; index < actionCount(); ++index)
            {
                const PlannedAction & mover = action(index);
                if (!isLiveState(mover.state) || !isMovingBundle(mover.kind))
                {
                    continue;
                }
                const std::int32_t occupantUnit = occupantAt(occupancy, mover.destination);
                if (occupantUnit == NO_UNIT || occupantUnit == mover.unitId)
                {
                    continue;
                }
                const std::int32_t occupantAction = liveActionOfUnit(occupantUnit);
                if (occupantAction == NO_ACTION || !vacatesTile(action(occupantAction), mover.destination))
                {
                    conflicts.push_back(VacateConflict{index, occupantUnit, occupantAction});
                    continue;
                }
                addEdge(occupantAction, index, PlanEdgeKind::Vacate);
            }
            return conflicts;
        }

        // only Pending and Committed actions are ordered
        OrderingResult executionOrder() const
        {
            const std::size_t count = static_cast<std::size_t>(actionCount());
            std::vector<bool> live(count, false);
            for (std::int32_t index = 0; index < actionCount(); ++index)
            {
                live[static_cast<std::size_t>(index)] = isLiveState(action(index).state);
            }
            Adjacency forward(count);
            Adjacency backward(count);
            std::vector<std::int32_t> indegree(count, 0);
            for (const PlanEdge & dependency : m_edges)
            {
                const std::size_t from = static_cast<std::size_t>(dependency.from);
                const std::size_t to = static_cast<std::size_t>(dependency.to);
                if (!live[from] || !live[to])
                {
                    continue;
                }
                forward[from].push_back(dependency.to);
                backward[to].push_back(dependency.from);
                ++indegree[to];
            }
            std::vector<bool> emitted(count, false);
            OrderingResult result;
            result.order.reserve(count);
            for (std::size_t step = 0; step < count; ++step)
            {
                const std::int32_t next = pickNextReady(indegree, emitted, live);
                if (next == NO_ACTION)
                {
                    break;
                }
                emitted[static_cast<std::size_t>(next)] = true;
                result.order.push_back(next);
                for (std::int32_t successor : forward[static_cast<std::size_t>(next)])
                {
                    --indegree[static_cast<std::size_t>(successor)];
                }
            }
            splitUnordered(forward, backward, live, emitted, result);
            return result;
        }

        // no loser when the lower ranked action is committed or already dead
        ConflictOutcome resolveDestinationConflict(std::int32_t leftIndex, std::int32_t rightIndex)
        {
            if (!hasAction(leftIndex) || !hasAction(rightIndex) || leftIndex == rightIndex)
            {
                return ConflictOutcome{};
            }
            std::int32_t loser = rightIndex;
            if (outranksInPlan(action(rightIndex), action(leftIndex)))
            {
                loser = leftIndex;
            }
            const PlanActionState loserState = action(loser).state;
            if (loserState != PlanActionState::Pending && loserState != PlanActionState::Blocked)
            {
                return ConflictOutcome{};
            }
            ConflictOutcome outcome;
            outcome.loser = loser;
            outcome.blocked.push_back(loser);
            mergeBlocked(outcome.blocked, blockAction(loser));
            mergeBlocked(outcome.blocked, blockDependents(loser));
            sortUnique(outcome.blocked);
            return outcome;
        }

        // dependents and wallbreak victims, a committed action cannot fail
        std::vector<std::int32_t> markFailed(std::int32_t actionIndex)
        {
            if (!hasAction(actionIndex) || action(actionIndex).state == PlanActionState::Committed)
            {
                return std::vector<std::int32_t>();
            }
            m_actions[static_cast<std::size_t>(actionIndex)].state = PlanActionState::Abandoned;
            std::vector<std::int32_t> affected = invalidateWallbreaksOnTargets(releaseClaims(actionIndex));
            mergeBlocked(affected, blockDependents(actionIndex));
            sortUnique(affected);
            return affected;
        }

    private:
        using Adjacency = std::vector<std::vector<std::int32_t>>;

        static constexpr std::int32_t NO_COMPONENT = -1;

        struct TileClaim
        {
            TilePoint tile{INVALID_TILE};
            std::int32_t actionIndex{NO_ACTION};
        };

        struct ServiceClaim
        {
            std::int32_t partnerUnitId{NO_UNIT};
            std::int32_t actionIndex{NO_ACTION};
        };

        struct DamageContribution
        {
            std::int32_t actionIndex{NO_ACTION};
            std::int32_t hpSteps{0};
        };

        struct DamageClaim
        {
            std::int32_t targetUnitId{NO_UNIT};
            std::int32_t remainingHpSteps{0};
            std::int32_t plannedHpSteps{0};
            std::vector<DamageContribution> contributions;
        };

        struct SearchFrame
        {
            std::int32_t node{NO_ACTION};
            std::size_t next{0};
        };

        bool acceptableShape(const PlannedAction & candidate) const
        {
            if (candidate.unitId == NO_UNIT)
            {
                return false;
            }
            if (isMovingBundle(candidate.kind) && candidate.path.empty())
            {
                return false;
            }
            return candidate.path.empty() || candidate.path.back() == candidate.destination;
        }

        static bool vacatesTile(const PlannedAction & occupant, TilePoint occupiedTile)
        {
            return isMovingBundle(occupant.kind) && occupant.destination != occupiedTile;
        }

        static std::size_t findTile(const std::vector<TileClaim> & claims, TilePoint tile)
        {
            for (std::size_t slot = 0; slot < claims.size(); ++slot)
            {
                if (claims[slot].tile == tile)
                {
                    return slot;
                }
            }
            return claims.size();
        }

        static std::int32_t tileClaimant(const std::vector<TileClaim> & claims, TilePoint tile)
        {
            const std::size_t slot = findTile(claims, tile);
            if (slot < claims.size())
            {
                return claims[slot].actionIndex;
            }
            return NO_ACTION;
        }

        ReservationResult claimTile(std::vector<TileClaim> & claims, std::int32_t actionIndex, TilePoint tile)
        {
            if (!hasAction(actionIndex) || tile == INVALID_TILE)
            {
                return ReservationResult::Invalid;
            }
            const std::size_t slot = findTile(claims, tile);
            if (slot < claims.size())
            {
                if (claims[slot].actionIndex == actionIndex)
                {
                    return ReservationResult::Granted;
                }
                return ReservationResult::Conflict;
            }
            claims.push_back(TileClaim{tile, actionIndex});
            return ReservationResult::Granted;
        }

        static void releaseTileClaims(std::vector<TileClaim> & claims, std::int32_t actionIndex)
        {
            std::size_t kept = 0;
            for (std::size_t slot = 0; slot < claims.size(); ++slot)
            {
                if (claims[slot].actionIndex != actionIndex)
                {
                    claims[kept] = claims[slot];
                    ++kept;
                }
            }
            claims.resize(kept);
        }

        std::size_t findService(std::int32_t partnerUnitId) const
        {
            for (std::size_t slot = 0; slot < m_serviceClaims.size(); ++slot)
            {
                if (m_serviceClaims[slot].partnerUnitId == partnerUnitId)
                {
                    return slot;
                }
            }
            return m_serviceClaims.size();
        }

        std::size_t findDamage(std::int32_t targetUnitId) const
        {
            for (std::size_t slot = 0; slot < m_damageClaims.size(); ++slot)
            {
                if (m_damageClaims[slot].targetUnitId == targetUnitId)
                {
                    return slot;
                }
            }
            return m_damageClaims.size();
        }

        // the first claim fixes the target's remaining hp
        DamageClaim & damageClaimFor(std::int32_t targetUnitId, std::int32_t remainingHpSteps)
        {
            const std::size_t slot = findDamage(targetUnitId);
            if (slot < m_damageClaims.size())
            {
                return m_damageClaims[slot];
            }
            DamageClaim fresh;
            fresh.targetUnitId = targetUnitId;
            fresh.remainingHpSteps = remainingHpSteps;
            m_damageClaims.push_back(std::move(fresh));
            return m_damageClaims.back();
        }

        static bool holdsContribution(const DamageClaim & claim, std::int32_t actionIndex)
        {
            for (const DamageContribution & share : claim.contributions)
            {
                if (share.actionIndex == actionIndex)
                {
                    return true;
                }
            }
            return false;
        }

        static std::int32_t contributionOf(const DamageClaim & claim, std::int32_t actionIndex)
        {
            for (const DamageContribution & share : claim.contributions)
            {
                if (share.actionIndex == actionIndex)
                {
                    return share.hpSteps;
                }
            }
            return 0;
        }

        static void setContribution(DamageClaim & claim, std::int32_t actionIndex, std::int32_t hpSteps)
        {
            for (DamageContribution & share : claim.contributions)
            {
                if (share.actionIndex == actionIndex)
                {
                    share.hpSteps = hpSteps;
                    return;
                }
            }
            claim.contributions.push_back(DamageContribution{actionIndex, hpSteps});
        }

        static void dropContribution(DamageClaim & claim, std::int32_t actionIndex)
        {
            std::size_t kept = 0;
            for (std::size_t slot = 0; slot < claim.contributions.size(); ++slot)
            {
                if (claim.contributions[slot].actionIndex != actionIndex)
                {
                    claim.contributions[kept] = claim.contributions[slot];
                    ++kept;
                }
            }
            claim.contributions.resize(kept);
        }

        // a dead action must not keep booking the battlefield
        std::vector<std::int32_t> releaseClaims(std::int32_t actionIndex)
        {
            std::vector<std::int32_t> touchedTargets;
            releaseTileClaims(m_destinationClaims, actionIndex);
            releaseTileClaims(m_captureClaims, actionIndex);
            std::size_t keptServices = 0;
            for (std::size_t slot = 0; slot < m_serviceClaims.size(); ++slot)
            {
                if (m_serviceClaims[slot].actionIndex != actionIndex)
                {
                    m_serviceClaims[keptServices] = m_serviceClaims[slot];
                    ++keptServices;
                }
            }
            m_serviceClaims.resize(keptServices);
            std::size_t keptClaims = 0;
            for (std::size_t slot = 0; slot < m_damageClaims.size(); ++slot)
            {
                DamageClaim & claim = m_damageClaims[slot];
                if (holdsContribution(claim, actionIndex))
                {
                    touchedTargets.push_back(claim.targetUnitId);
                    claim.plannedHpSteps -= contributionOf(claim, actionIndex);
                    dropContribution(claim, actionIndex);
                }
                if (claim.contributions.empty())
                {
                    continue;
                }
                // self move assignment would empty the contributions
                if (keptClaims != slot)
                {
                    m_damageClaims[keptClaims] = std::move(claim);
                }
                ++keptClaims;
            }
            m_damageClaims.resize(keptClaims);
            m_actions[static_cast<std::size_t>(actionIndex)].plannedDamageSteps = 0;
            return touchedTargets;
        }

        static void mergeBlocked(std::vector<std::int32_t> & affected, const std::vector<std::int32_t> & blocked)
        {
            affected.insert(affected.end(), blocked.begin(), blocked.end());
        }

        static void sortUnique(std::vector<std::int32_t> & affected)
        {
            std::sort(affected.begin(), affected.end());
            affected.erase(std::unique(affected.begin(), affected.end()), affected.end());
        }

        std::vector<std::int32_t> blockAction(std::int32_t actionIndex)
        {
            m_actions[static_cast<std::size_t>(actionIndex)].state = PlanActionState::Blocked;
            return invalidateWallbreaksOnTargets(releaseClaims(actionIndex));
        }

        std::vector<std::int32_t> dependentsOf(std::int32_t actionIndex) const
        {
            std::vector<std::int32_t> dependents;
            for (const PlanEdge & dependency : m_edges)
            {
                if (dependency.from == actionIndex)
                {
                    dependents.push_back(dependency.to);
                }
            }
            return dependents;
        }

        std::vector<std::int32_t> blockDependents(std::int32_t actionIndex)
        {
            std::vector<std::int32_t> affected;
            std::vector<std::int32_t> frontier;
            frontier.push_back(actionIndex);
            while (!frontier.empty())
            {
                const std::int32_t current = frontier.back();
                frontier.pop_back();
                // copied first, blocking a dependent can remove edges
                for (std::int32_t dependent : dependentsOf(current))
                {
                    if (action(dependent).state != PlanActionState::Pending)
                    {
                        continue;
                    }
                    const std::vector<std::int32_t> collateral = blockAction(dependent);
                    affected.push_back(dependent);
                    mergeBlocked(affected, collateral);
                    frontier.push_back(dependent);
                }
            }
            sortUnique(affected);
            return affected;
        }

        std::vector<std::int32_t> removeEdgesFrom(std::int32_t actionIndex, PlanEdgeKind kind)
        {
            std::vector<std::int32_t> removed;
            std::size_t kept = 0;
            for (std::size_t slot = 0; slot < m_edges.size(); ++slot)
            {
                if (m_edges[slot].from == actionIndex && m_edges[slot].kind == kind)
                {
                    removed.push_back(m_edges[slot].to);
                    continue;
                }
                m_edges[kept] = m_edges[slot];
                ++kept;
            }
            m_edges.resize(kept);
            return removed;
        }

        // lethality belongs to the target, so every attacker on it is re-checked
        std::vector<std::int32_t> invalidateWallbreaksOn(std::int32_t targetUnitId)
        {
            std::vector<std::int32_t> affected;
            // a kill that still stands invalidates nothing
            if (targetUnitId == NO_UNIT || targetIsLethal(targetUnitId))
            {
                return affected;
            }
            for (std::int32_t index = 0; index < actionCount(); ++index)
            {
                if (action(index).targetUnitId != targetUnitId)
                {
                    continue;
                }
                mergeBlocked(affected, invalidateWallbreaks(index));
            }
            sortUnique(affected);
            return affected;
        }

        std::vector<std::int32_t> invalidateWallbreaksOnTargets(const std::vector<std::int32_t> & targetUnitIds)
        {
            std::vector<std::int32_t> affected;
            for (std::int32_t targetUnitId : targetUnitIds)
            {
                mergeBlocked(affected, invalidateWallbreaksOn(targetUnitId));
            }
            sortUnique(affected);
            return affected;
        }

        // a push whose kill no longer reaches lethal must be replanned
        std::vector<std::int32_t> invalidateWallbreaks(std::int32_t attackIndex)
        {
            std::vector<std::int32_t> affected;
            if (targetIsLethal(action(attackIndex).targetUnitId))
            {
                return affected;
            }
            for (std::int32_t push : removeEdgesFrom(attackIndex, PlanEdgeKind::Wallbreak))
            {
                if (action(push).state != PlanActionState::Pending)
                {
                    continue;
                }
                const std::vector<std::int32_t> collateral = blockAction(push);
                affected.push_back(push);
                mergeBlocked(affected, collateral);
                mergeBlocked(affected, blockDependents(push));
            }
            sortUnique(affected);
            return affected;
        }

        static std::int32_t occupantAt(std::span<const OccupancyEntry> occupancy, TilePoint tile)
        {
            for (const OccupancyEntry & entry : occupancy)
            {
                if (entry.tile == tile)
                {
                    return entry.unitId;
                }
            }
            return NO_UNIT;
        }

        std::int32_t actionOfUnit(std::int32_t unitId) const
        {
            for (std::int32_t index = 0; index < actionCount(); ++index)
            {
                if (action(index).unitId == unitId)
                {
                    return index;
                }
            }
            return NO_ACTION;
        }

        std::int32_t liveActionOfUnit(std::int32_t unitId) const
        {
            const std::int32_t index = actionOfUnit(unitId);
            if (index == NO_ACTION || !isLiveState(action(index).state))
            {
                return NO_ACTION;
            }
            return index;
        }

        std::int32_t pickNextReady(const std::vector<std::int32_t> & indegree, const std::vector<bool> & emitted,
                                   const std::vector<bool> & live) const
        {
            std::int32_t best = NO_ACTION;
            for (std::int32_t index = 0; index < actionCount(); ++index)
            {
                const std::size_t slot = static_cast<std::size_t>(index);
                if (!live[slot] || emitted[slot] || indegree[slot] > 0)
                {
                    continue;
                }
                if (best == NO_ACTION || outranksInPlan(action(index), action(best)))
                {
                    best = index;
                }
            }
            return best;
        }

        // Kosaraju with explicit stacks, deep chains cannot overflow
        static std::vector<std::int32_t> stronglyConnectedComponents(const Adjacency & forward, const Adjacency & backward,
                                                                    const std::vector<bool> & live)
        {
            const std::size_t count = live.size();
            std::vector<bool> visited(count, false);
            std::vector<std::int32_t> finishOrder;
            finishOrder.reserve(count);
            std::vector<SearchFrame> stack;
            for (std::size_t root = 0; root < count; ++root)
            {
                if (!live[root] || visited[root])
                {
                    continue;
                }
                visited[root] = true;
                stack.push_back(SearchFrame{static_cast<std::int32_t>(root), 0});
                while (!stack.empty())
                {
                    const std::size_t depth = stack.size() - 1;
                    const std::size_t node = static_cast<std::size_t>(stack[depth].node);
                    if (stack[depth].next < forward[node].size())
                    {
                        const std::int32_t next = forward[node][stack[depth].next];
                        ++stack[depth].next;
                        if (!visited[static_cast<std::size_t>(next)])
                        {
                            visited[static_cast<std::size_t>(next)] = true;
                            stack.push_back(SearchFrame{next, 0});
                        }
                        continue;
                    }
                    finishOrder.push_back(stack[depth].node);
                    stack.pop_back();
                }
            }
            std::vector<std::int32_t> component(count, NO_COMPONENT);
            std::int32_t nextComponent = 0;
            std::vector<std::int32_t> pending;
            for (std::size_t step = finishOrder.size(); step > 0; --step)
            {
                const std::int32_t root = finishOrder[step - 1];
                if (component[static_cast<std::size_t>(root)] != NO_COMPONENT)
                {
                    continue;
                }
                component[static_cast<std::size_t>(root)] = nextComponent;
                pending.push_back(root);
                while (!pending.empty())
                {
                    const std::int32_t current = pending.back();
                    pending.pop_back();
                    for (std::int32_t previous : backward[static_cast<std::size_t>(current)])
                    {
                        if (component[static_cast<std::size_t>(previous)] == NO_COMPONENT)
                        {
                            component[static_cast<std::size_t>(previous)] = nextComponent;
                            pending.push_back(previous);
                        }
                    }
                }
                ++nextComponent;
            }
            return component;
        }

        static void splitUnordered(const Adjacency & forward, const Adjacency & backward, const std::vector<bool> & live,
                                   const std::vector<bool> & emitted, OrderingResult & result)
        {
            const std::size_t count = live.size();
            std::size_t liveCount = 0;
            for (std::size_t node = 0; node < count; ++node)
            {
                if (live[node])
                {
                    ++liveCount;
                }
            }
            if (result.order.size() == liveCount)
            {
                return;
            }
            const std::vector<std::int32_t> component = stronglyConnectedComponents(forward, backward, live);
            std::vector<std::int32_t> componentSize(count, 0);
            for (std::size_t node = 0; node < count; ++node)
            {
                if (component[node] != NO_COMPONENT)
                {
                    ++componentSize[static_cast<std::size_t>(component[node])];
                }
            }
            for (std::size_t node = 0; node < count; ++node)
            {
                if (!live[node] || emitted[node])
                {
                    continue;
                }
                const std::int32_t index = static_cast<std::int32_t>(node);
                // pass one visits every live node, so component[node] is set
                if (componentSize[static_cast<std::size_t>(component[node])] > 1)
                {
                    result.cycleMembers.push_back(index);
                    continue;
                }
                result.blockedByCycle.push_back(index);
            }
        }

        std::vector<PlannedAction> m_actions;
        std::vector<PlanEdge> m_edges;
        std::vector<TileClaim> m_destinationClaims;
        std::vector<TileClaim> m_captureClaims;
        std::vector<ServiceClaim> m_serviceClaims;
        std::vector<DamageClaim> m_damageClaims;
    };
}
