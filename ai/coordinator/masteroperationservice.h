#pragma once

#include <array>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "ai/coordinator/masterassignmentevaluator.h"

namespace Coordinator
{
    struct MasterOperationReadiness;

    constexpr std::size_t MASTER_OPERATION_ROLE_LIMIT = 5;
    constexpr std::int64_t MASTER_OPERATION_REGISTRY_QUOTE_WORK = 32;

    struct MasterOperationDescriptor
    {
        std::shared_ptr<MasterAssignmentEvaluation> evaluation;
        MasterRefinementTerm term{MasterRefinementTerm::Friendly};
        bool lowerRelevant{false};
        bool upperRelevant{false};
    };

    struct MasterOperationIdentity
    {
        ContinuationKey key;
        std::vector<std::int32_t> capturedColumns;
    };

    struct MasterOperationService
    {
        std::shared_ptr<const MasterOperationIdentity> identity;
        MasterRefinementTerm term{MasterRefinementTerm::Friendly};
        std::int64_t allowance{0};
        std::int64_t paidWork{0};
        std::shared_ptr<const MasterOperationReadiness> readiness;
        bool growthExhausted{false};
    };

    class MasterOperationRegistry final
    {
    public:
        static std::optional<std::int64_t> tariff(std::size_t records, std::span<const std::size_t> scalars)
        {
            if (scalars.empty() || scalars.size() > MASTER_OPERATION_ROLE_LIMIT ||
                records > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
            {
                return {};
            }
            std::int64_t width = static_cast<std::int64_t>(scalars.size());
            for (const auto size : scalars)
            {
                if (size >= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ||
                    !RelativeStockDetail::checkedAdd(width, static_cast<std::int64_t>(size) + 1))
                {
                    return {};
                }
            }
            std::int64_t rows = static_cast<std::int64_t>(records);
            if (!RelativeStockDetail::checkedAdd(rows, static_cast<std::int64_t>(scalars.size()) + 1) ||
                rows > (std::numeric_limits<std::int64_t>::max() - 32) / 4 / width)
            {
                return {};
            }
            return 32 + 4 * rows * width;
        }

        bool prepare(const MasterAssignmentIdentity & problem,
                     std::span<const MasterOperationDescriptor> descriptors, RefinementLedger & ledger,
                     std::int64_t searchAllowance, std::int64_t dualAllowance,
                     std::array<std::size_t, MASTER_OPERATION_ROLE_LIMIT> & indices)
        {
            std::array<std::size_t, MASTER_OPERATION_ROLE_LIMIT> sizes{};
            if (!ledger.consume(MASTER_OPERATION_REGISTRY_QUOTE_WORK, RefinementWork::MasterExpansion) ||
                descriptors.empty() || descriptors.size() > sizes.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < descriptors.size(); ++index)
            {
                const auto & descriptor = descriptors[index];
                const auto* quote = descriptor.evaluation ? descriptor.evaluation->quote() : nullptr;
                if (quote == nullptr || quote->key.size() > std::numeric_limits<std::size_t>::max() / 4)
                {
                    return false;
                }
                sizes[index] = descriptor.term == MasterRefinementTerm::Enemy
                    ? quote->capturedColumns.size() : 4 * quote->key.size();
            }
            const auto cost = tariff(m_records.size(), std::span(sizes).first(descriptors.size()));
            if (!cost || !ledger.consume(*cost, RefinementWork::MasterExpansion))
            {
                return false;
            }
            const auto & first = descriptors.front().evaluation->proof->quote;
            if (!problem.sameLiveProblem(problem) || !first.identity.established())
            {
                return false;
            }
            if (!m_problem.sameLiveProblem(problem) || !m_stock.sameLiveModel(first.identity))
            {
                m_records.clear();
                m_problem = problem;
                m_stock = first.identity;
            }
            m_records.reserve(m_records.size() + descriptors.size());
            for (std::size_t index = 0; index < descriptors.size(); ++index)
            {
                const auto & descriptor = descriptors[index];
                const auto & proof = descriptor.evaluation->proof;
                if (!descriptor.evaluation->state.identity.sameLiveProblem(problem) ||
                    !proof->quote.identity.sameLiveModel(m_stock))
                {
                    return false;
                }
                auto found = m_records.size();
                for (std::size_t record = 0; record < m_records.size(); ++record)
                {
                    if (matches(m_records[record], descriptor))
                    {
                        found = record;
                        break;
                    }
                }
                if (found == m_records.size())
                {
                    const auto allowance = descriptor.term == MasterRefinementTerm::FutureDual
                        ? dualAllowance : searchAllowance;
                    auto identity = std::make_shared<MasterOperationIdentity>();
                    if (descriptor.term == MasterRefinementTerm::Enemy)
                    {
                        identity->capturedColumns = proof->quote.capturedColumns;
                    }
                    else
                    {
                        identity->key = proof->quote.key;
                    }
                    m_records.push_back({std::move(identity), descriptor.term, allowance});
                }
                indices[index] = found;
            }
            return true;
        }

        MasterOperationService & at(std::size_t index)
        {
            return m_records.at(index);
        }

        const MasterOperationService & at(std::size_t index) const
        {
            return m_records.at(index);
        }

        std::size_t size() const
        {
            return m_records.size();
        }

        static bool before(const MasterOperationService & left, const MasterOperationService & right)
        {
            if (left.paidWork != right.paidWork)
            {
                return left.paidWork < right.paidWork;
            }
            if (left.term != right.term)
            {
                return left.term < right.term;
            }
            if (left.term == MasterRefinementTerm::Enemy)
            {
                return left.identity->capturedColumns < right.identity->capturedColumns;
            }
            return left.identity->key < right.identity->key;
        }

        static bool credit(MasterOperationService & operation, std::int64_t work)
        {
            return work >= 0 && RelativeStockDetail::checkedAdd(operation.paidWork, work);
        }

        static bool grow(MasterOperationService & operation, RefinementLedger & ledger,
                         std::int64_t growth, bool admitted, bool completed, bool valid)
        {
            if (!admitted || completed || !valid)
            {
                return true;
            }
            if (!ledger.consume(MASTER_OPERATION_QUOTA_WORK, RefinementWork::MasterExpansion))
            {
                return false;
            }
            if (growth < 2 || operation.allowance > std::numeric_limits<std::int64_t>::max() / growth)
            {
                operation.growthExhausted = true;
                return true;
            }
            operation.allowance *= growth;
            return true;
        }

    private:
        static bool matches(const MasterOperationService & operation, const MasterOperationDescriptor & descriptor)
        {
            if (operation.term != descriptor.term)
            {
                return false;
            }
            const auto & quote = descriptor.evaluation->proof->quote;
            if (operation.term == MasterRefinementTerm::Enemy)
            {
                return operation.identity->capturedColumns == quote.capturedColumns;
            }
            return operation.identity->key == quote.key;
        }

        MasterAssignmentIdentity m_problem;
        RelativeStockIdentity m_stock;
        std::vector<MasterOperationService> m_records;
    };
}
