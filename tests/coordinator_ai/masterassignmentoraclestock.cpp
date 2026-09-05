#include "tests/coordinator_ai/masterassignmentoracle.h"

#include <algorithm>
#include <limits>
#include <set>

#include "ai/coordinator/relativestockcomparison.h"

namespace
{
    using namespace MasterAssignmentOracle;

    bool addMagnitude(MilliFunds & total, MilliFunds value)
    {
        MilliFunds magnitude = value;
        if (value < 0)
        {
            magnitude = 0;
            if (!RelativeStockDetail::checkedSubtract(magnitude, value))
            {
                return false;
            }
        }
        return RelativeStockDetail::checkedAdd(total, magnitude);
    }

    ObjectiveStatus validate(const SequentialInput & input)
    {
        const auto nodes = input.columns.size();
        if (input.horizon < 0 || input.horizon > std::numeric_limits<std::int32_t>::max() / 4 ||
            input.capturedNodes.size() != nodes || nodes > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            (nodes != 0 && nodes > std::numeric_limits<std::size_t>::max() / nodes))
        {
            return ObjectiveStatus::InvalidInput;
        }
        MilliFunds envelope = 0;
        std::set<std::int32_t> columns;
        for (const auto & column : input.columns)
        {
            if (column.slot < 0 || !columns.insert(column.slot).second ||
                (column.ownerBefore != OwnerSign::Ours && column.ownerBefore != OwnerSign::Enemy &&
                 column.ownerBefore != OwnerSign::Neutral))
            {
                return ObjectiveStatus::InvalidInput;
            }
            MilliFunds income = 0;
            if (!addMagnitude(income, column.income.oursPerTurn) ||
                !addMagnitude(income, column.income.enemyPerTurn) ||
                (input.horizon > 0 && income > std::numeric_limits<MilliFunds>::max() / input.horizon) ||
                !RelativeStockDetail::checkedAdd(envelope, income * input.horizon))
            {
                return ObjectiveStatus::ArithmeticFailure;
            }
        }
        for (const auto & entry : input.classes)
        {
            if (entry.interArrivals.size() != nodes * nodes ||
                (entry.captureTurns != NO_CAPTURE_TURNS &&
                 (entry.captureTurns < 1 || entry.captureTurns > input.horizon + 1)))
            {
                return ObjectiveStatus::InvalidInput;
            }
            for (const auto arrival : entry.interArrivals)
            {
                if (arrival > std::numeric_limits<std::int32_t>::max() / 4)
                {
                    return ObjectiveStatus::ArithmeticFailure;
                }
            }
        }
        for (const auto & row : input.rows)
        {
            if (row.classIndex < 0 || static_cast<std::size_t>(row.classIndex) >= input.classes.size() ||
                row.weight.size() != nodes || row.ownedTurn.size() != nodes)
            {
                return ObjectiveStatus::InvalidInput;
            }
            MilliFunds rowMaximum = 0;
            for (std::size_t node = 0; node < nodes; ++node)
            {
                if (row.ownedTurn[node] != NO_CAPTURE_TURNS &&
                    (row.ownedTurn[node] < 1 || row.ownedTurn[node] >= input.horizon))
                {
                    return ObjectiveStatus::InvalidInput;
                }
                MilliFunds magnitude = 0;
                if (!addMagnitude(magnitude, row.weight[node]))
                {
                    return ObjectiveStatus::ArithmeticFailure;
                }
                rowMaximum = std::max(rowMaximum, magnitude);
            }
            if (!RelativeStockDetail::checkedAdd(envelope, rowMaximum))
            {
                return ObjectiveStatus::ArithmeticFailure;
            }
        }
        if (input.horizon > 0 && envelope > std::numeric_limits<MilliFunds>::max() / input.horizon)
        {
            return ObjectiveStatus::ArithmeticFailure;
        }
        return ObjectiveStatus::Exact;
    }

    class Enumeration
    {
    public:
        Enumeration(const SequentialInstance & instance, std::uint64_t limit)
            : m_instance(instance), m_limit(limit), m_used(instance.capturedNodes)
        {
        }

        SequentialExactResult run()
        {
            m_result.status = ObjectiveStatus::Exact;
            nextRow(0, 0);
            if (m_result.status == ObjectiveStatus::Exact &&
                !replaySequentialWitness(m_instance, m_result.witness, m_result.value))
            {
                m_result.status = ObjectiveStatus::WitnessFailure;
            }
            return m_result;
        }

    private:
        bool spend()
        {
            if (m_result.status != ObjectiveStatus::Exact)
            {
                return false;
            }
            if (m_result.work >= m_limit)
            {
                m_result.status = ObjectiveStatus::Incomplete;
                return false;
            }
            ++m_result.work;
            return true;
        }

        void nextRow(std::size_t row, MilliFunds total)
        {
            if (!spend())
            {
                return;
            }
            if (row == m_instance.rows.size())
            {
                ++m_result.completePackings;
                if (total > m_result.value)
                {
                    m_result.value = total;
                    m_result.witness = m_witness;
                }
                return;
            }
            nextRow(row + 1, total);
            m_witness.push_back(SequentialRowWitness{.row = static_cast<std::int32_t>(row)});
            extend(row, total, NO_SEQUENTIAL_NODE, NO_CAPTURE_TURNS);
            m_witness.pop_back();
        }

        void extend(std::size_t rowIndex, MilliFunds total, std::int32_t previous, std::int32_t previousTurn)
        {
            const auto & row = m_instance.rows[rowIndex];
            const auto & table = m_instance.classes[row.classIndex];
            for (std::int32_t node = 0; node < m_instance.nodeCount(); ++node)
            {
                if (m_used[node] || !spend())
                {
                    continue;
                }
                const auto turn = previous == NO_SEQUENTIAL_NODE
                    ? row.ownedTurn[node] : table.continuationOwnedTurn(previous, previousTurn, node);
                if (turn == NO_CAPTURE_TURNS &&
                    (previous != NO_SEQUENTIAL_NODE || row.weight[node] == 0))
                {
                    continue;
                }
                const auto gain = previous == NO_SEQUENTIAL_NODE ? row.weight[node]
                    : SequentialDetail::chainPrize(m_instance, node, turn);
                MilliFunds nextTotal = total;
                MilliFunds rowValue = m_witness.back().value;
                if (!RelativeStockDetail::checkedAdd(nextTotal, gain) ||
                    !RelativeStockDetail::checkedAdd(rowValue, gain))
                {
                    m_result.status = ObjectiveStatus::ArithmeticFailure;
                    return;
                }
                const auto oldValue = m_witness.back().value;
                m_witness.back().value = rowValue;
                m_witness.back().steps.push_back({node, turn, gain});
                m_used[node] = true;
                nextRow(rowIndex + 1, nextTotal);
                if (turn != NO_CAPTURE_TURNS)
                {
                    extend(rowIndex, nextTotal, node, turn);
                }
                m_used[node] = false;
                m_witness.back().steps.pop_back();
                m_witness.back().value = oldValue;
            }
        }

        const SequentialInstance & m_instance;
        std::uint64_t m_limit;
        std::vector<bool> m_used;
        std::vector<SequentialRowWitness> m_witness;
        SequentialExactResult m_result;
    };
}

namespace MasterAssignmentOracle
{
    SequentialExactResult exhaustSequential(const SequentialInput & input, std::uint64_t workLimit)
    {
        SequentialExactResult result;
        result.status = validate(input);
        if (result.status != ObjectiveStatus::Exact)
        {
            return result;
        }
        std::vector<SequentialClassTable> classes;
        for (const auto & entry : input.classes)
        {
            SequentialClassTable table;
            table.build(input.columns, entry.interArrivals, entry.captureTurns, input.horizon);
            classes.push_back(std::move(table));
        }
        const SequentialInstance instance{
            .horizonTurns = input.horizon,
            .nodeColumns = input.columns,
            .classes = classes,
            .rows = input.rows,
            .capturedNodes = input.capturedNodes,
        };
        return Enumeration(instance, workLimit).run();
    }

    Objective evaluateObjective(const ReferencePlan & plan, const ContinuationInput & input,
                                std::uint64_t workLimit)
    {
        Objective result;
        result.seated = plan.seatedTotal;
        if (!plan.complete || input.mode == ContinuationMode::Unavailable)
        {
            return result;
        }
        if (input.mode == ContinuationMode::Sequential)
        {
            const ContinuationCase* match = nullptr;
            for (const auto & entry : input.cases)
            {
                if (entry.key == plan.key)
                {
                    if (match != nullptr)
                    {
                        result.status = ObjectiveStatus::InvalidInput;
                        return result;
                    }
                    match = &entry;
                }
            }
            if (match == nullptr)
            {
                return result;
            }
            if (!std::is_sorted(match->capturedColumns.begin(), match->capturedColumns.end()) ||
                std::adjacent_find(match->capturedColumns.begin(), match->capturedColumns.end()) !=
                    match->capturedColumns.end() ||
                std::any_of(match->capturedColumns.begin(), match->capturedColumns.end(),
                            [](auto column) { return column < 0; }))
            {
                result.status = ObjectiveStatus::InvalidInput;
                return result;
            }
            result.owned = match->owned;
            result.capturedColumns = match->capturedColumns;
            result.friendlyProof = exhaustSequential(match->friendly, workLimit);
            result.enemyProof = exhaustSequential(match->enemy, workLimit);
            if (result.friendlyProof.status != ObjectiveStatus::Exact ||
                result.enemyProof.status != ObjectiveStatus::Exact)
            {
                result.status = result.friendlyProof.status != ObjectiveStatus::Exact
                    ? result.friendlyProof.status : result.enemyProof.status;
                return result;
            }
            result.friendly = result.friendlyProof.value;
            result.enemy = result.enemyProof.value;
        }
        else if (input.mode != ContinuationMode::None)
        {
            result.status = ObjectiveStatus::InvalidInput;
            return result;
        }
        else
        {
            result.friendlyProof.status = ObjectiveStatus::Exact;
            result.enemyProof.status = ObjectiveStatus::Exact;
        }
        result.total = result.seated;
        result.status = RelativeStockDetail::checkedAdd(result.total, result.owned) &&
                        RelativeStockDetail::checkedAdd(result.total, result.friendly) &&
                        RelativeStockDetail::checkedSubtract(result.total, result.enemy)
            ? ObjectiveStatus::Exact : ObjectiveStatus::ArithmeticFailure;
        return result;
    }
}
