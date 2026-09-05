#pragma once

#include <array>
#include <chrono>
#include <cstdint>

namespace Coordinator
{
    enum class MasterPhase : std::int8_t
    {
        Shadow,
        Extraction,
        Catalog,
        Search,
        InitialIncumbent,
        BoundPreparation,
        Expansion,
        BoundScan,
        DualOptimization,
        CertificateVerification,
        Quote,
        LowerWitness,
        ContinuationRefinement,
        LeafReplay,
        Count,
    };

    constexpr std::array<const char*, static_cast<std::size_t>(MasterPhase::Count)> MASTER_PHASE_NAMES{
        "shadow", "extraction", "catalog", "search", "initialIncumbent", "boundPreparation",
        "expansion", "boundScan", "dualOptimization", "certificateVerification", "quote",
        "lowerWitness", "continuationRefinement", "leafReplay",
    };

    struct MasterPhaseSample
    {
        std::uint64_t calls{0};
        std::int64_t inclusiveNanos{0};
        std::int64_t exclusiveNanos{0};
    };

    class MasterProfileScope;

    class MasterAssignmentProfile final
    {
    public:
        MasterAssignmentProfile() = default;
        MasterAssignmentProfile(const MasterAssignmentProfile &) = delete;
        MasterAssignmentProfile & operator=(const MasterAssignmentProfile &) = delete;
        std::array<MasterPhaseSample, MASTER_PHASE_NAMES.size()> samples{};

    private:
        friend class MasterProfileScope;
        MasterProfileScope* m_active{nullptr};
    };

    class MasterProfileScope final
    {
    public:
        MasterProfileScope(MasterAssignmentProfile* profile, MasterPhase phase)
            : m_profile(profile),
              m_phase(phase)
        {
            if (m_profile != nullptr)
            {
                m_parent = m_profile->m_active;
                m_profile->m_active = this;
                m_start = Clock::now();
            }
        }

        ~MasterProfileScope()
        {
            if (m_profile != nullptr)
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_start).count();
                auto & sample = m_profile->samples[static_cast<std::size_t>(m_phase)];
                ++sample.calls;
                sample.inclusiveNanos += elapsed;
                sample.exclusiveNanos += elapsed - m_nestedNanos;
                if (m_parent != nullptr)
                {
                    m_parent->m_nestedNanos += elapsed;
                }
                m_profile->m_active = m_parent;
            }
        }

        MasterProfileScope(const MasterProfileScope &) = delete;
        MasterProfileScope & operator=(const MasterProfileScope &) = delete;

    private:
        using Clock = std::chrono::steady_clock;
        MasterAssignmentProfile* m_profile;
        MasterPhase m_phase;
        MasterProfileScope* m_parent{nullptr};
        Clock::time_point m_start;
        std::int64_t m_nestedNanos{0};
    };
}
