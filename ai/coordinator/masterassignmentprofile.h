#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

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
        FriendlySetup,
        FriendlySearch,
        EnemySetup,
        EnemySearch,
        SequentialReplay,
        ProofCapture,
        ProofCopy,
        ProofReplay,
        TierArchiveCopy,
        NativeCertificateSetup,
        NativeCertificateIdentity,
        NativeCertificateReadiness,
        NativeCertificateCatalog,
        NativeCertificateOptimization,
        NativeCertificateVerification,
        StockControl,
        IncomingConstruction,
        Installation,
        Count,
    };

    constexpr std::array<const char*, static_cast<std::size_t>(MasterPhase::Count)> MASTER_PHASE_NAMES{
        "shadow", "extraction", "catalog", "search", "initialIncumbent", "boundPreparation",
        "expansion", "boundScan", "dualOptimization", "certificateVerification", "quote",
        "lowerWitness", "continuationRefinement", "leafReplay",
        "friendlySetup", "friendlySearch", "enemySetup", "enemySearch",
        "sequentialReplay", "proofCapture", "proofCopy", "proofReplay", "tierArchiveCopy",
        "nativeCertificateSetup", "nativeCertificateIdentity", "nativeCertificateReadiness",
        "nativeCertificateCatalog", "nativeCertificateOptimization", "nativeCertificateVerification",
        "stockControl", "incomingConstruction", "installation",
    };

    struct MasterPhaseSample
    {
        std::uint64_t calls{0};
        std::int64_t inclusiveNanos{0};
        std::int64_t exclusiveNanos{0};
    };

    class MasterProfileScope;
    struct SequentialCostAudit;

    class MasterAssignmentProfile final
    {
    public:
        MasterAssignmentProfile() = default;
        MasterAssignmentProfile(const MasterAssignmentProfile &) = delete;
        MasterAssignmentProfile & operator=(const MasterAssignmentProfile &) = delete;
        void recordFirstImprovement();
        std::array<MasterPhaseSample, MASTER_PHASE_NAMES.size()> samples{};
        std::optional<std::int64_t> firstImprovementNanos;
        SequentialCostAudit* friendlySequentialCost{nullptr};
        SequentialCostAudit* enemySequentialCost{nullptr};

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
            finish();
        }

        void finish()
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
                m_profile = nullptr;
            }
        }

        MasterProfileScope(const MasterProfileScope &) = delete;
        MasterProfileScope & operator=(const MasterProfileScope &) = delete;

    private:
        friend class MasterAssignmentProfile;
        using Clock = std::chrono::steady_clock;
        MasterAssignmentProfile* m_profile;
        MasterPhase m_phase;
        MasterProfileScope* m_parent{nullptr};
        Clock::time_point m_start;
        std::int64_t m_nestedNanos{0};
    };

    inline void MasterAssignmentProfile::recordFirstImprovement()
    {
        if (firstImprovementNanos || m_active == nullptr)
        {
            return;
        }
        const auto* root = m_active;
        while (root->m_parent != nullptr)
        {
            root = root->m_parent;
        }
        firstImprovementNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
            MasterProfileScope::Clock::now() - root->m_start).count();
    }
}
