#include "ai/coordinator/masterassignmentbounds.h"

#include <bit>
#include <limits>

#include "ai/coordinator/assignmentvaluation.h"

namespace
{
    using namespace Coordinator;

    std::optional<MilliFunds> seatedUpper(const MasterCandidate & candidate,
                                         RefinementLedger & ledger)
    {
        if (!candidate.plannable)
        {
            return std::nullopt;
        }
        const auto & components = candidate.source.bundle.components;
        const auto damage = components.size() == 1 && components.front().kind == ComponentKind::Fire
            ? std::min(components.front().fire.damageSteps, components.front().fire.targetHpSteps) : 0;
        std::optional<MilliFunds> upper;
        for (std::int32_t grant = 0; grant <= damage; ++grant)
        {
            if (!chargeMasterWork(ledger, 1, 1, RefinementWork::MasterPreparation))
            {
                return std::nullopt;
            }
            const auto value = checkedSeatedCandidateValue(candidate.source, grant);
            if (!value)
            {
                return std::nullopt;
            }
            upper = upper ? std::max(*upper, *value) : *value;
        }
        return upper;
    }

    MilliFunds unrestrictedUpper(const CheapRowTerms & terms)
    {
        return terms.chains.empty() ? 0 : std::max<MilliFunds>(0, terms.chains.front().value);
    }

    bool exclusive(MasterResourceKind kind)
    {
        return kind == MasterResourceKind::Destination ||
               kind == MasterResourceKind::Capture || kind == MasterResourceKind::Service;
    }

    MasterBoundStatus boundStatus(MasterFutureStatus status)
    {
        switch (status)
        {
            case MasterFutureStatus::Valid:
                return MasterBoundStatus::Valid;
            case MasterFutureStatus::Unavailable:
                return MasterBoundStatus::Unavailable;
            case MasterFutureStatus::Invalid:
                return MasterBoundStatus::InvalidCertificate;
            case MasterFutureStatus::ArithmeticFailure:
                return MasterBoundStatus::ArithmeticFailure;
            case MasterFutureStatus::BudgetExhausted:
                return MasterBoundStatus::BudgetExhausted;
        }
        return MasterBoundStatus::InvalidCertificate;
    }
}

namespace Coordinator
{
    MasterAssignmentBounds::MasterAssignmentBounds(const MasterAssignmentProblem & problem, MasterAssignmentProfile* profile)
        : m_problem(problem),
          m_profile(profile),
          m_problemIdentity(problem.identity())
    {
    }

    MasterAssignmentBounds::PreparedOption MasterAssignmentBounds::prepareOption(
        const MasterActor & actor, const MasterCandidate & candidate, bool included,
        const CheapPricingModel* stock, RefinementLedger & ledger) const
    {
        PreparedOption result;
        auto & option = result.option;
        const auto refusals = ledger.stats().refusals;
        option.economic = m_problem.canChangeActor(actor.canonicalSlot)
            ? seatedUpper(candidate, ledger) : std::optional<MilliFunds>{actor.incumbentSeatedValue};
        if (ledger.stats().refusals != refusals ||
            !chargeMasterWork(ledger, candidate.resources.size(), 1, RefinementWork::MasterPreparation))
        {
            result.status = MasterBoundStatus::BudgetExhausted;
            return result;
        }
        for (const auto & usage : candidate.resources)
        {
            if (m_resources[static_cast<std::size_t>(usage.resourceIndex)].priceable)
            {
                if (usage.demand != 1 || usage.capacity != 1)
                {
                    result.status = MasterBoundStatus::InvalidCertificate;
                    return result;
                }
                option.resources.push_back(usage.resourceIndex);
            }
        }
        if (stock != nullptr && included && actor.stockRow != NO_STOCK_ROW)
        {
            if (!chargeMasterWork(ledger, 1, std::bit_width(static_cast<std::uint64_t>(stock->actionCount())) + 1,
                                  RefinementWork::MasterPreparation))
            {
                result.status = MasterBoundStatus::BudgetExhausted;
                return result;
            }
            const auto key = CanonicalPlanActionKey::fromAction(actor.stockRow,
                candidate.stockEndpoint.x, candidate.stockEndpoint.y, candidate.capture.has_value());
            const auto* terms = stock->actionTerms(key);
            if (terms == nullptr)
            {
                result.status = MasterBoundStatus::Unavailable;
                return result;
            }
            result.stockUpper = unrestrictedUpper(terms->rowTerms);
            if (terms->capturedColumn >= 0)
            {
                option.captureUpper = std::max<MilliFunds>(0,
                    stock->capturedColumnSwings()[static_cast<std::size_t>(terms->capturedColumn)]);
            }
            if (!RelativeStockDetail::checkedAdd(result.stockUpper, option.captureUpper))
            {
                result.status = MasterBoundStatus::ArithmeticFailure;
                return result;
            }
            if (m_future != nullptr)
            {
                if (!chargeMasterWork(ledger, 1, std::bit_width(m_future->actions.size()) + 1,
                                      RefinementWork::MasterPreparation))
                {
                    result.status = MasterBoundStatus::BudgetExhausted;
                    return result;
                }
                const auto found = std::lower_bound(m_future->actions.begin(), m_future->actions.end(), key,
                    [](const MasterFutureAction & action, const CanonicalPlanActionKey & sought)
                    {
                        return action.key < sought;
                    });
                if (found == m_future->actions.end() || found->key != key)
                {
                    result.status = MasterBoundStatus::Unavailable;
                    return result;
                }
                option.futureAction = static_cast<std::size_t>(found - m_future->actions.begin());
            }
        }
        option.combined = option.economic;
        if (option.combined && !RelativeStockDetail::checkedAdd(*option.combined, result.stockUpper))
        {
            result.status = MasterBoundStatus::ArithmeticFailure;
        }
        return result;
    }

    MasterBoundStatus MasterAssignmentBounds::prepare(
        const CheapPricingModel* stock, RefinementLedger & ledger, const MasterFutureCatalog* futureCatalog)
    {
        MasterProfileScope timing(m_profile, MasterPhase::BoundPreparation);
        if (m_ready || !m_problemIdentity.sameLiveProblem(m_problem.identity()) ||
            (stock != nullptr && !stock->installed()))
        {
            return MasterBoundStatus::InvalidIdentity;
        }
        if (!chargeMasterWork(ledger, m_problem.actors().size() + m_problem.resources().size() + 1,
                              1, RefinementWork::MasterPreparation))
        {
            return MasterBoundStatus::BudgetExhausted;
        }
        m_options.clear();
        m_resources.clear();
        m_stock = stock;
        m_future = futureCatalog != nullptr && futureCatalog->complete ? futureCatalog : nullptr;
        if (m_future != nullptr && (stock == nullptr || m_future->dayRows.size() != stock->dayRows().size()))
        {
            return MasterBoundStatus::InvalidCertificate;
        }
        m_stockConstant = stock == nullptr ? 0 : stock->ownedBaseline();
        m_ownedBaseline = m_stockConstant;
        m_globalStockUpper = m_stockConstant;
        std::vector<bool> actorRows;
        if (stock != nullptr)
        {
            if (!chargeMasterWork(ledger, stock->dayRows().size(), 1, RefinementWork::MasterPreparation))
            {
                return MasterBoundStatus::BudgetExhausted;
            }
            actorRows.assign(stock->dayRows().size(), false);
        }
        for (const auto & resource : m_problem.resources())
        {
            m_resources.push_back({exclusive(resource.kind)});
        }
        for (const auto & actor : m_problem.actors())
        {
            if (!chargeMasterWork(ledger, actor.candidates.size(), 1, RefinementWork::MasterPreparation))
            {
                return MasterBoundStatus::BudgetExhausted;
            }
            const bool included = m_problem.canChangeActor(actor.canonicalSlot) ||
                                  actor.incumbentCandidateIndex != NO_CANDIDATE;
            if (stock != nullptr && actor.stockRow != NO_STOCK_ROW && included)
            {
                if (actor.stockRow < 0 || static_cast<std::size_t>(actor.stockRow) >= actorRows.size() ||
                    actorRows[static_cast<std::size_t>(actor.stockRow)])
                {
                    return MasterBoundStatus::InvalidCertificate;
                }
                actorRows[static_cast<std::size_t>(actor.stockRow)] = true;
            }
            std::vector<Option> options;
            options.reserve(actor.candidates.size());
            std::optional<MilliFunds> actorStockUpper;
            for (const auto & candidate : actor.candidates)
            {
                auto prepared = prepareOption(actor, candidate, included, stock, ledger);
                if (prepared.status != MasterBoundStatus::Valid)
                {
                    return prepared.status;
                }
                auto & option = prepared.option;
                if (included && (m_problem.canChangeActor(actor.canonicalSlot) ||
                    candidate.identity.originalCandidateIndex == actor.incumbentCandidateIndex))
                {
                    actorStockUpper = actorStockUpper
                        ? std::max(*actorStockUpper, prepared.stockUpper) : prepared.stockUpper;
                }
                for (const auto resource : option.resources)
                {
                    const auto index = static_cast<std::size_t>(resource);
                    if (option.economic)
                    {
                        m_resources[index].economicScale = std::max(m_resources[index].economicScale, *option.economic);
                    }
                    if (option.combined)
                    {
                        m_resources[index].combinedScale = std::max(m_resources[index].combinedScale, *option.combined);
                    }
                }
                options.push_back(std::move(option));
            }
            if (actorStockUpper && !RelativeStockDetail::checkedAdd(m_globalStockUpper, *actorStockUpper))
            {
                return MasterBoundStatus::ArithmeticFailure;
            }
            m_options.push_back(std::move(options));
        }
        if (stock != nullptr)
        {
            for (std::size_t row = 0; row < actorRows.size(); ++row)
            {
                if (!actorRows[row])
                {
                    const auto upper = unrestrictedUpper(stock->dayRows()[row]);
                    if (!RelativeStockDetail::checkedAdd(m_stockConstant, upper) ||
                        !RelativeStockDetail::checkedAdd(m_globalStockUpper, upper))
                    {
                        return MasterBoundStatus::ArithmeticFailure;
                    }
                }
            }
        }
        m_actorRows = std::move(actorRows);
        if (m_future != nullptr)
        {
            if (!chargeMasterWork(ledger, m_future->columns.size(), 1, RefinementWork::MasterPreparation))
            {
                return MasterBoundStatus::BudgetExhausted;
            }
            for (const auto & column : m_future->columns)
            {
                if (column.slot < 0 || static_cast<std::size_t>(column.slot) >= stock->capturedColumnSwings().size())
                {
                    return MasterBoundStatus::InvalidCertificate;
                }
                const auto scale = std::max<MilliFunds>(1, stock->capturedColumnSwings()[static_cast<std::size_t>(column.slot)]);
                m_resources.push_back({true, 1, scale, ResourceScope::Future});
            }
        }
        m_basis = std::make_shared<const std::uint8_t>(0);
        m_ready = true;
        return MasterBoundStatus::Valid;
    }

    bool MasterAssignmentBounds::ready() const
    {
        return m_ready && m_problemIdentity.sameLiveProblem(m_problem.identity());
    }

    bool MasterAssignmentBounds::covers(const MasterAssignmentIdentity & problem, const CheapPricingModel* stock,
                                        const MasterFutureCatalog* future) const
    {
        return ready() && m_problemIdentity.sameLiveProblem(problem) && stock != nullptr && stock == m_stock &&
               future != nullptr && future == m_future;
    }

    MasterFutureCacheStats MasterAssignmentBounds::futureCacheStats() const
    {
        return m_futureCacheStats;
    }

    MasterFutureStatus MasterAssignmentBounds::futurePrices(
        std::span<const MilliFunds> prices, RefinementLedger & ledger, RefinementWork category, FutureCache cache,
        std::shared_ptr<const MasterFuturePricing> & result) const
    {
        if (!chargeMasterWork(ledger, prices.size() + 1, 2, category))
        {
            return MasterFutureStatus::BudgetExhausted;
        }
        if (cache == FutureCache::Bypass)
        {
            auto computed = priceMasterFuture(*m_future, prices, ledger, category);
            const auto status = computed.status;
            if (status == MasterFutureStatus::Valid)
            {
                result = std::make_shared<const MasterFuturePricing>(std::move(computed));
            }
            return status;
        }
        if (m_cachedFuturePricing &&
            std::equal(prices.begin(), prices.end(), m_cachedFuturePrices.begin(), m_cachedFuturePrices.end()))
        {
            ++m_futureCacheStats.hits;
            result = m_cachedFuturePricing;
            return MasterFutureStatus::Valid;
        }
        ++m_futureCacheStats.recomputations;
        auto computed = priceMasterFuture(*m_future, prices, ledger, category);
        if (computed.status != MasterFutureStatus::Valid)
        {
            return computed.status;
        }
        m_cachedFuturePrices.assign(prices.begin(), prices.end());
        m_cachedFuturePricing = std::make_shared<const MasterFuturePricing>(std::move(computed));
        result = m_cachedFuturePricing;
        return MasterFutureStatus::Valid;
    }

    std::optional<MilliFunds> MasterAssignmentBounds::contribution(
        std::size_t actor, std::int32_t candidate) const
    {
        if (!m_ready || !m_problemIdentity.sameLiveProblem(m_problem.identity()) ||
            actor >= m_options.size() || candidate < 0 ||
            static_cast<std::size_t>(candidate) >= m_options[actor].size())
        {
            return std::nullopt;
        }
        return m_options[actor][static_cast<std::size_t>(candidate)].combined;
    }

    bool MasterAssignmentBounds::matches(const MasterBoundCertificate & certificate) const
    {
        const auto binding = certificate.basis.lock();
        return m_ready && binding && binding == m_basis && m_problemIdentity.sameLiveProblem(m_problem.identity()) &&
               validMasterState(m_problem, certificate.state) &&
               certificate.prices.size() == m_resources.size();
    }

    MasterAssignmentBounds::ScanResult MasterAssignmentBounds::scan(
        const MasterBoundCertificate & certificate, RefinementLedger & ledger,
        RefinementWork category, bool collectDemand, FutureCache cache, CandidateTerms* terms) const
    {
        ScanResult result;
        if (!chargeMasterWork(ledger, m_problem.actors().size() + m_resources.size() + 1,
                              1, category))
        {
            result.status = MasterBoundStatus::BudgetExhausted;
            return result;
        }
        if (!matches(certificate))
        {
            result.status = MasterBoundStatus::InvalidIdentity;
            return result;
        }
        const bool pricedFuture = certificate.family == MasterBoundFamily::CombinedCurrentFuture;
        const bool combined = certificate.family == MasterBoundFamily::EndpointCoupled || pricedFuture;
        if (!combined && certificate.family != MasterBoundFamily::Immediate &&
            certificate.family != MasterBoundFamily::CurrentResource)
        {
            result.status = MasterBoundStatus::InvalidCertificate;
            return result;
        }
        std::shared_ptr<const MasterFuturePricing> priced;
        if (pricedFuture)
        {
            if (m_future == nullptr)
            {
                return result;
            }
            const auto prices = std::span<const MilliFunds>(certificate.prices).subspan(m_problem.resources().size());
            const auto status = futurePrices(prices, ledger, category, cache, priced);
            if (status != MasterFutureStatus::Valid)
            {
                result.status = boundStatus(status);
                return result;
            }
        }
        const MasterFuturePricing empty;
        const auto & future = priced ? *priced : empty;
        result.upper = combined ? m_stockConstant : m_globalStockUpper;
        result.demand.assign(m_resources.size(), 0);
        if (pricedFuture)
        {
            result.upper = m_ownedBaseline;
            for (std::size_t row = 0; row < m_actorRows.size(); ++row)
            {
                if (!chargeMasterWork(ledger, future.dayRows[row].nodes.size() + 1, 1, category))
                {
                    result.status = MasterBoundStatus::BudgetExhausted;
                    return result;
                }
                if (!m_actorRows[row])
                {
                    if (!RelativeStockDetail::checkedAdd(result.upper, future.dayRows[row].upper))
                    {
                        result.status = MasterBoundStatus::ArithmeticFailure;
                        return result;
                    }
                    if (collectDemand)
                    {
                        for (const auto node : future.dayRows[row].nodes)
                        {
                            auto & demand = result.demand[m_problem.resources().size() + static_cast<std::size_t>(node)];
                            if (!RelativeStockDetail::checkedAdd(demand, 1))
                            {
                                result.status = MasterBoundStatus::ArithmeticFailure;
                                return result;
                            }
                        }
                    }
                }
            }
        }
        for (std::size_t resource = 0; resource < certificate.prices.size(); ++resource)
        {
            const auto price = certificate.prices[resource];
            if (price < 0 || ((!m_resources[resource].priceable ||
                certificate.family == MasterBoundFamily::Immediate ||
                (!pricedFuture && m_resources[resource].scope == ResourceScope::Future)) && price != 0))
            {
                result.status = MasterBoundStatus::InvalidCertificate;
                return result;
            }
            if (!RelativeStockDetail::checkedAdd(result.upper, price))
            {
                result.status = MasterBoundStatus::ArithmeticFailure;
                return result;
            }
        }
        result.status = scanActors(certificate, future, ledger, category, collectDemand, terms, result);
        return result;
    }

    MasterBoundStatus MasterAssignmentBounds::optionTerm(
        const Option & option, const MasterBoundCertificate & certificate, const MasterFuturePricing & future,
        RefinementLedger & ledger, RefinementWork category, MilliFunds & term) const
    {
        const bool pricedFuture = certificate.family == MasterBoundFamily::CombinedCurrentFuture;
        const bool combined = certificate.family == MasterBoundFamily::EndpointCoupled || pricedFuture;
        auto value = combined ? option.combined : option.economic;
        if (pricedFuture)
        {
            value = option.economic;
            if (value && (!RelativeStockDetail::checkedAdd(*value, option.captureUpper) ||
                (option.futureAction &&
                 !RelativeStockDetail::checkedAdd(*value, future.actions[*option.futureAction].upper))))
            {
                return MasterBoundStatus::ArithmeticFailure;
            }
        }
        if (!value)
        {
            return MasterBoundStatus::Unavailable;
        }
        if (!chargeMasterWork(ledger, option.resources.size(), 1, category))
        {
            return MasterBoundStatus::BudgetExhausted;
        }
        for (const auto resource : option.resources)
        {
            if (!RelativeStockDetail::checkedSubtract(
                    *value, certificate.prices[static_cast<std::size_t>(resource)]))
            {
                return MasterBoundStatus::ArithmeticFailure;
            }
        }
        term = *value;
        return MasterBoundStatus::Valid;
    }

    MasterBoundStatus MasterAssignmentBounds::scanActors(
        const MasterBoundCertificate & certificate, const MasterFuturePricing & future,
        RefinementLedger & ledger, RefinementWork category, bool collectDemand, CandidateTerms* terms,
        ScanResult & result) const
    {
        const bool pricedFuture = certificate.family == MasterBoundFamily::CombinedCurrentFuture;
        for (std::size_t slot = 0; slot < m_options.size(); ++slot)
        {
            const auto selected = certificate.state.choices[slot];
            if (selected == NO_CANDIDATE)
            {
                continue;
            }
            std::optional<MilliFunds> maximum;
            const Option* maximizing = nullptr;
            for (const auto choice : m_problem.actors()[slot].canonicalCandidates)
            {
                if (!chargeMasterWork(ledger, 1, 1, category))
                {
                    return MasterBoundStatus::BudgetExhausted;
                }
                const bool chosen = selected == MASTER_UNASSIGNED || choice == selected;
                if (!chosen && terms == nullptr)
                {
                    continue;
                }
                const auto & option = m_options[slot][static_cast<std::size_t>(choice)];
                MilliFunds value = 0;
                const auto status = optionTerm(option, certificate, future, ledger, category, value);
                if (status == MasterBoundStatus::BudgetExhausted || (chosen && status != MasterBoundStatus::Valid))
                {
                    return status;
                }
                if (terms != nullptr)
                {
                    (*terms)[slot].push_back({choice, status == MasterBoundStatus::Valid
                        ? std::optional<MilliFunds>{value} : std::nullopt});
                }
                if (!chosen)
                {
                    continue;
                }
                if (!maximum || value > *maximum)
                {
                    maximum = value;
                    maximizing = &option;
                }
            }
            if (!maximum || !RelativeStockDetail::checkedAdd(result.upper, *maximum))
            {
                return MasterBoundStatus::ArithmeticFailure;
            }
            if (collectDemand)
            {
                for (const auto resource : maximizing->resources)
                {
                    ++result.demand[static_cast<std::size_t>(resource)];
                }
                if (pricedFuture && maximizing->futureAction)
                {
                    const auto & nodes = future.actions[*maximizing->futureAction].nodes;
                    if (!chargeMasterWork(ledger, nodes.size(), 1, category))
                    {
                        return MasterBoundStatus::BudgetExhausted;
                    }
                    for (const auto node : nodes)
                    {
                        auto & demand = result.demand[m_problem.resources().size() + static_cast<std::size_t>(node)];
                        if (!RelativeStockDetail::checkedAdd(demand, 1))
                        {
                            return MasterBoundStatus::ArithmeticFailure;
                        }
                    }
                }
            }
        }
        return MasterBoundStatus::Valid;
    }

    MasterBoundResult MasterAssignmentBounds::bound(
        const MasterAssignmentState & state, MasterBoundFamily family, RefinementLedger & ledger) const
    {
        MasterProfileScope timing(m_profile, MasterPhase::BoundScan);
        if (!chargeMasterWork(ledger, state.choices.size() + m_resources.size() + 1,
                              1, RefinementWork::MasterBoundScan))
        {
            return {MasterBoundStatus::BudgetExhausted, {}};
        }
        return boundAt(state, family, std::vector<MilliFunds>(m_resources.size()), ledger);
    }

    MasterBoundResult MasterAssignmentBounds::boundAt(
        const MasterAssignmentState & state, MasterBoundFamily family, std::vector<MilliFunds> prices,
        RefinementLedger & ledger) const
    {
        MasterBoundCertificate certificate{state, m_basis, family, std::move(prices)};
        const auto result = scan(certificate, ledger, RefinementWork::MasterBoundScan, false);
        if (result.status != MasterBoundStatus::Valid)
        {
            return {result.status, {}};
        }
        certificate.upper = result.upper;
        return {MasterBoundStatus::Valid, std::move(certificate)};
    }

    MasterTermTable MasterAssignmentBounds::termsAt(
        const MasterAssignmentState & state, std::vector<MilliFunds> prices, RefinementLedger & ledger) const
    {
        MasterTermTable table;
        if (!state.complete())
        {
            table.status = MasterBoundStatus::InvalidCertificate;
            return table;
        }
        const MasterBoundCertificate certificate{state, m_basis, MasterBoundFamily::CombinedCurrentFuture, std::move(prices)};
        table.actorTerms.resize(state.choices.size());
        const auto result = scan(certificate, ledger, RefinementWork::MasterBoundScan, false,
                                 FutureCache::Bypass, &table.actorTerms);
        table.status = result.status;
        if (result.status != MasterBoundStatus::Valid)
        {
            table.actorTerms.clear();
            return table;
        }
        table.upper = result.upper;
        return table;
    }

    MasterBoundResult MasterAssignmentBounds::verify(
        const MasterBoundCertificate & certificate, RefinementLedger & ledger) const
    {
        MasterProfileScope timing(m_profile, MasterPhase::CertificateVerification);
        const auto result = scan(certificate, ledger, RefinementWork::MasterDualVerification, false);
        if (result.status != MasterBoundStatus::Valid)
        {
            return {result.status, {}};
        }
        if (result.upper != certificate.upper)
        {
            return {MasterBoundStatus::InvalidCertificate, {}};
        }
        return {MasterBoundStatus::Valid, certificate};
    }

    MasterBoundResult MasterAssignmentBounds::tighten(
        const MasterBoundCertificate & initial, std::int32_t rounds, RefinementLedger & ledger) const
    {
        MasterProfileScope timing(m_profile, MasterPhase::DualOptimization);
        auto best = verify(initial, ledger);
        if (!best.valid() || initial.family == MasterBoundFamily::Immediate || rounds < 0)
        {
            return best;
        }
        auto proposal = initial;
        for (std::int32_t round = 0; round < rounds; ++round)
        {
            const auto relaxed = scan(proposal, ledger, RefinementWork::MasterDualOptimization, true);
            if (relaxed.status != MasterBoundStatus::Valid)
            {
                return {relaxed.status, best.certificate};
            }
            proposal.upper = relaxed.upper;
            auto verified = verify(proposal, ledger);
            if (!verified.valid())
            {
                return {verified.status, best.certificate};
            }
            if (verified.certificate->upper < best.certificate->upper)
            {
                best = std::move(verified);
            }
            if (!chargeMasterWork(ledger, proposal.prices.size(), 1,
                                  RefinementWork::MasterDualOptimization))
            {
                return {MasterBoundStatus::BudgetExhausted, best.certificate};
            }
            bool changed = false;
            for (std::size_t resource = 0; resource < proposal.prices.size(); ++resource)
            {
                if (!m_resources[resource].priceable || relaxed.demand[resource] == 1 ||
                    (m_resources[resource].scope == ResourceScope::Future &&
                     proposal.family != MasterBoundFamily::CombinedCurrentFuture))
                {
                    continue;
                }
                MilliFunds step = proposal.family == MasterBoundFamily::EndpointCoupled ||
                                  proposal.family == MasterBoundFamily::CombinedCurrentFuture
                    ? m_resources[resource].combinedScale : m_resources[resource].economicScale;
                step = std::max<MilliFunds>(1, step / (static_cast<MilliFunds>(round) + 2));
                if (relaxed.demand[resource] > 1)
                {
                    if (!RelativeStockDetail::checkedAdd(proposal.prices[resource], step))
                    {
                        return {MasterBoundStatus::ArithmeticFailure, best.certificate};
                    }
                    changed = true;
                }
                else if (proposal.prices[resource] > 0)
                {
                    proposal.prices[resource] = std::max<MilliFunds>(0, proposal.prices[resource] - step);
                    changed = true;
                }
            }
            if (!changed)
            {
                break;
            }
        }
        return best;
    }
}
