#include "ai/coordinator/masterassignmentcontrol.h"

#include <algorithm>
#include <chrono>

#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>

#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/replyfacts.h"
#include "ai/coordinator/masterassignmentstock.h"
#include "ai/coordinator/compatiblepacking.h"
#include "ai/coordinator/stockpackingsibling.h"
#include "ai/coordinator/stocktargetwitness.h"
#include "ai/coordinator/stockprogressscope.h"
#include <QScopeGuard>
#include "ai/coordinator/masterassignmenttrace.h"
#include "ai/coordinator/masterplanpricing.h"

#ifdef COW_MASTER_ASSIGNMENT_PLAYABLE_TRIAL
#include "coreengine/settings.h"
#endif

namespace
{
    using namespace Coordinator;

#ifdef COW_MASTER_ASSIGNMENT_PLAYABLE_TRIAL
    class StaleProbeScope final
    {
    public:
        StaleProbeScope(MasterStaleProbe probe, bool detailPresentation, bool retainedImprovement)
        {
            if (probe == MasterStaleProbe::BattleAnimationSpeed && detailPresentation && retainedImprovement)
            {
                auto* settings = Settings::getInstance();
                m_restore = static_cast<quint32>(settings->getBattleAnimationSpeedValue());
                // Speeds of 100 and above derive the same speed, so use distinct raw values.
                settings->setBattleAnimationSpeed(*m_restore == 1 ? 2 : 1);
            }
        }
        StaleProbeScope(const StaleProbeScope &) = delete;
        StaleProbeScope & operator=(const StaleProbeScope &) = delete;
        bool applied() const
        {
            return m_restore.has_value();
        }
        ~StaleProbeScope()
        {
            if (m_restore)
            {
                Settings::getInstance()->setBattleAnimationSpeed(*m_restore);
            }
        }

    private:
        std::optional<quint32> m_restore;
    };
#endif

    enum class CaptureScreen : std::int8_t
    {
        Inapplicable,
        Eligible,
        BudgetExhausted,
    };

    CaptureScreen screenCapture(const MasterAssignmentProblem & problem, const PropertyStockField & field,
                                std::span<const PropertyFacts> properties, RefinementLedger & ledger)
    {
        for (const auto & actor : problem.actors())
        {
            if (!ledger.consume(1, RefinementWork::MasterPreparation))
            {
                return CaptureScreen::BudgetExhausted;
            }
            if (!problem.canChangeActor(actor.canonicalSlot) || actor.incumbentCandidateIndex < 0)
            {
                continue;
            }
            const auto & incumbent = actor.candidates[actor.incumbentCandidateIndex];
            for (const auto & candidate : actor.candidates)
            {
                if (!ledger.consume(1, RefinementWork::MasterPreparation))
                {
                    return CaptureScreen::BudgetExhausted;
                }
                if (candidate.identity.originalCandidateIndex != actor.incumbentCandidateIndex &&
                    candidate.plannable && candidate.origin == candidate.destination &&
                    candidate.source.actor.hpSteps == UNIT_HP_STEPS && candidate.capture &&
                    candidate.capture->ownerBefore == OwnerSign::Neutral &&
                    candidate.capture->ownerAfter == OwnerSign::Ours &&
                    candidate.capture->turnsUntilOwned == 0 && candidate.destination == incumbent.destination)
                {
                    const auto index = field.propertyIndexAt(candidate.destination);
                    if (index >= 0 && static_cast<std::size_t>(index) < properties.size() &&
                        properties[index].buildingId == QStringLiteral("TOWN"))
                    {
                        return CaptureScreen::Eligible;
                    }
                }
            }
        }
        return CaptureScreen::Inapplicable;
    }

    struct ControlAttempt
    {
        MasterStockAuthority authority;
        MasterStockControlStatus status{MasterStockControlStatus::StockPreparationRefused};
        MasterNativeSourcePreparation native;
        MasterStockSetupStatus stockStatus{MasterStockSetupStatus::Unprepared};
        std::optional<MasterAssignmentSearchResult> search;
        MasterInstallationResult installed;
        std::unique_ptr<MasterAssignmentSearchTrace> trace;
        std::int64_t workBefore{0};
        std::int64_t refusalsBefore{0};
        std::int64_t startedNanos{0};
        std::int64_t searchNanos{0};
        std::int64_t finalizationNanos{0};
        std::int64_t reservedWork{0};
        bool staleProbeApplied{false};
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        std::optional<MasterWitnessCharges> witnessCharges;
#endif
    };

    struct ControlContext
    {
        const MasterShadowConfig & config;
        std::int64_t allowance;
        const DecisionTraceIdentity & identity;
        AssignmentResult & assignment;
        RefinementLedger & ledger;
        MasterAssignmentProfile & profile;
        MasterStockControlTiming & timing;
        QElapsedTimer & total;
        QJsonArray attempts;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        LaterListMeasure laterList;
#endif
    };

    QJsonObject attemptRecord(const ControlContext & context, const ControlAttempt & attempt)
    {
        const auto & search = attempt.search;
        const auto & installed = attempt.installed;
        const auto & measured = context.timing;
        QJsonObject record{
            {"planningAuthority", masterStockAuthorityName(attempt.authority)},
            {"stockRevision", attempt.authority == MasterStockAuthority::GuardedCapture
                ? QJsonValue(static_cast<int>(GuardedStockRevision::CoupledNativeTerminalV4)) : QJsonValue(QJsonValue::Null)},
            {"status", masterStockControlStatusName(attempt.status)},
            {"stockSetupStatus", static_cast<int>(attempt.stockStatus)},
            {"nativeSourceStatus", masterNativeSourceStatusName(attempt.native.status)},
            {"installationStatus", static_cast<int>(installed.status)},
            {"installed", installed.status == MasterInstallationStatus::Installed},
            {"attemptWorkBefore", QString::number(attempt.workBefore)},
            {"attemptWorkAfter", QString::number(context.ledger.stats().spent())},
            {"attemptWork", QString::number(context.ledger.stats().spent() - attempt.workBefore)},
            {"attemptRefusals", QString::number(context.ledger.stats().refusals - attempt.refusalsBefore)},
            {"attemptNanos", QString::number(context.total.nsecsElapsed() - attempt.startedNanos)},
            {"reservedWork", QString::number(attempt.reservedWork)},
            {"searchWorkIncludesReservation", search.has_value()},
            {"firstImprovementWorkExcludesReservation", true},
            {"finalizationWork", QString::number(installed.finalizationWork)},
            {"firstImprovementWork", masterValueRecord(measured.firstImprovementWork)},
            {"firstImprovementNanos", masterValueRecord(measured.firstImprovementNanos)},
            {"firstImprovementLower", masterValueRecord(measured.firstImprovementLower)},
            {"searchNanos", QString::number(attempt.searchNanos)},
            {"finalizationNanos", QString::number(attempt.finalizationNanos)},
            {"detailPresentation", attempt.native.detailPresentation},
            {"staleProbeApplied", attempt.staleProbeApplied}};
        if (context.ledger.stats().refusals != attempt.refusalsBefore)
        {
            record.insert("lastRefusal", masterRefusalRecord(context.ledger.lastRefusal()));
        }
        const auto nativeRecord = masterNativeSourceWorkRecord(attempt.native);
        for (auto field = nativeRecord.begin(); field != nativeRecord.end(); ++field)
        {
            record.insert(field.key(), field.value());
        }
        if (search)
        {
            record.insert("search", masterSearchRecord(*search));
            if (search->incumbent)
            {
                record.insert("incoming", masterEvaluationRecord(*search->incumbent));
            }
            if (search->improvement)
            {
                record.insert("retainedIncumbent", masterEvaluationRecord(search->improvement->incumbent));
                record.insert("retainedChallenger", masterEvaluationRecord(search->improvement->challenger));
            }
        }
        if (installed.proof)
        {
            record.insert("installedIncumbent", masterEvaluationRecord(installed.proof->incumbent));
            record.insert("installedChallenger", masterEvaluationRecord(installed.proof->challenger));
            if (installed.proof->guardedProof)
            {
                record.insert("installedGuardedProof", masterGuardedProofRecord(*installed.proof->guardedProof));
            }
            else
            {
                record.insert("strictMargin", masterValueRecord(installed.proof->comparison.deltaLower));
                record.insert("relativeUpper", masterValueRecord(installed.proof->comparison.deltaUpper));
            }
        }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        if (attempt.witnessCharges)
        {
            record.insert(TRACE_WITNESS_CHARGES, masterWitnessChargesRecord(*attempt.witnessCharges));
        }
#endif
        return record;
    }

    ControlAttempt beginAttempt(ControlContext & context, MasterStockAuthority authority)
    {
        context.profile.firstImprovementNanos.reset();
        ControlAttempt attempt{.authority = authority};
        attempt.workBefore = context.ledger.stats().spent();
        attempt.refusalsBefore = context.ledger.stats().refusals;
        attempt.startedNanos = context.total.nsecsElapsed();
        return attempt;
    }

    MasterStockControlStatus finishAttempt(ControlContext & context, ControlAttempt & attempt)
    {
        auto & measured = context.timing;
        measured.installationStatus = attempt.installed.status;
        measured.searchNanos += attempt.searchNanos;
        measured.finalizationNanos += attempt.finalizationNanos;
        measured.finalizationWork += attempt.installed.finalizationWork;
        measured.reservedWork = std::max(measured.reservedWork, attempt.reservedWork);
        measured.firstImprovementNanos = context.profile.firstImprovementNanos;
        measured.firstImprovementWork.reset();
        measured.firstImprovementLower.reset();
        measured.gap.reset();
        measured.staleProbeApplied = measured.staleProbeApplied || attempt.staleProbeApplied;
        if (attempt.search)
        {
            measured.gap = attempt.search->gap;
            measured.firstImprovementLower = attempt.search->stats.firstImprovementLower;
            if (attempt.search->stats.firstImprovementWork)
            {
                measured.firstImprovementWork = *attempt.search->stats.firstImprovementWork - attempt.reservedWork;
            }
        }
        if (attempt.installed.status == MasterInstallationStatus::Installed)
        {
            measured.selectedAuthority = attempt.authority;
            if (attempt.installed.proof->guardedProof)
            {
                measured.guardedPaymentShift = attempt.installed.proof->guardedProof->relation().minimumPaymentShift;
            }
            else
            {
                measured.improvementLower = attempt.installed.proof->comparison.deltaLower;
            }
        }
        if (context.config.exportMode == MasterShadowExportMode::Full)
        {
            const auto record = attemptRecord(context, attempt);
            if (attempt.trace)
            {
                attempt.trace->finish(record);
                measured.exportSucceeded = attempt.trace->complete() && measured.exportSucceeded;
            }
            context.attempts.append(record);
        }
        return attempt.status;
    }

    void searchAndInstall(ControlContext & context, ControlAttempt & attempt,
                          const MasterAssignmentProblem & problem, MasterAssignmentEvaluator & evaluator)
    {
        MasterAssignmentInstallation installation(problem, evaluator, context.ledger);
        attempt.installed.status = installation.prepare(context.assignment);
        attempt.reservedWork = installation.reservedWork();
        if (attempt.installed.status != MasterInstallationStatus::Reserved)
        {
            attempt.status = MasterStockControlStatus::FinalizationRefused;
            return;
        }
        MasterAssignmentSearchOptions options{.profile = &context.profile};
        if (attempt.authority == MasterStockAuthority::RelativeStock)
        {
            options.schedule.initialIncumbentAlternatives = true;
            options.schedule.initialContributionCompletion = true;
            options.schedule.allocation = MasterRefinementAllocation::OperationService;
            options.schedule.planPricingRounds = MASTER_PLAN_PRICING_ROUNDS;
            options.schedule.planPricingWork = MASTER_PLAN_PRICING_WORK;
            options.schedule.planPricingWitnessWork = MASTER_PLAN_PRICING_WITNESS_WORK;
            options.schedule.planBoundCap = MasterPlanBoundCap::Verified;
            options.schedule.planNeighborAudit = context.config.planNeighborAudit;
        }
        attempt.trace = attachMasterStockTrace(
            context.config, context.identity, problem, context.ledger, attempt.reservedWork,
            options, masterStockAuthorityName(attempt.authority));
        const auto searchStart = context.total.nsecsElapsed();
        attempt.search = searchMasterAssignment(problem, evaluator, context.ledger, options);
        attempt.searchNanos = context.total.nsecsElapsed() - searchStart;
        if (const auto & audit = attempt.search->stats.planPricing.neighborAudit)
        {
            attempt.searchNanos -= audit->nanos;
        }
        const auto finalizationStart = context.total.nsecsElapsed();
        {
            MasterProfileScope timing(&context.profile, MasterPhase::Installation);
#ifdef COW_MASTER_ASSIGNMENT_PLAYABLE_TRIAL
            StaleProbeScope stale(context.config.staleProbe, attempt.native.detailPresentation,
                                  attempt.search->improvement.has_value());
            attempt.staleProbeApplied = stale.applied();
#endif
            attempt.installed = installation.finish(attempt.search->improvement, context.assignment);
        }
        attempt.finalizationNanos = context.total.nsecsElapsed() - finalizationStart;
        attempt.status = MasterStockControlStatus::Retained;
        if (attempt.installed.status == MasterInstallationStatus::Installed)
        {
            attempt.status = MasterStockControlStatus::Installed;
        }
    }

    MasterStockControlStatus attemptCapture(
        ControlContext & context, const MasterAssignmentProblem & problem, GameMap & map)
    {
        auto attempt = beginAttempt(context, MasterStockAuthority::GuardedCapture);
        attempt.native = prepareMasterNativeSource(map, problem, context.ledger);
        context.timing.nativeSourceStatus = attempt.native.status;
        context.timing.detailPresentation = attempt.native.detailPresentation;
        if (attempt.native.status != MasterNativeSourceStatus::Ready || !attempt.native.source)
        {
            return finishAttempt(context, attempt);
        }
        MasterGuardedStockOwner owner(problem, context.ledger, std::move(attempt.native.source));
        MasterAssignmentEvaluator evaluator(problem, context.ledger, owner, &context.profile);
        searchAndInstall(context, attempt, problem, evaluator);
        return finishAttempt(context, attempt);
    }

    std::optional<SequentialInstance> stockInstance(const MasterFutureCatalog &catalog, const RelativeStockQuote &quote)
    {
        SequentialInstance result{catalog.horizon, catalog.columns, catalog.classes, catalog.dayRows, {}};
        for (const auto &column : catalog.columns)
            result.capturedNodes.push_back(std::ranges::binary_search(quote.capturedColumns, column.slot));
        for (const auto &entry : quote.key)
        {
            const auto *action = findMasterFutureAction(catalog, entry);
            if (!action || entry.row() < 0 || std::size_t(entry.row()) >= result.rows.size()) return std::nullopt;
            result.rows[entry.row()] = action->row;
        }
        return result;
    }

    std::optional<std::vector<SequentialDetail::RowOption>> stockRowOptions(
        const SequentialInstance &instance, int rowIndex, RefinementLedger &ledger)
    {
        const auto &row = instance.rows[rowIndex];
        if (!ledger.consume(instance.nodeCount() + 1, RefinementWork::MasterWitness)) return std::nullopt;
        std::vector<MilliFunds> enriched(instance.nodeCount());
        for (int node = 0; node < instance.nodeCount(); ++node)
        {
            if (instance.capturedNodes[node]) continue;
            enriched[node] = row.weight[node];
            if (!RelativeStockDetail::checkedAdd(enriched[node],
                SequentialDetail::continuationGain(instance, row, node))) return std::nullopt;
        }
        SequentialDetail::EnumerationBudget budget{0, ledger.balance()};
        if (budget.stateCap <= 0) return std::nullopt;
        std::int64_t routes = 0;
        auto options = SequentialDetail::enumerateRowOptions(instance, rowIndex, enriched, budget, routes);
        if (!ledger.consume(budget.states, RefinementWork::MasterWitness) || budget.truncated) return std::nullopt;
        return options;
    }

    // Each neutral site can be claimed by at most one row in any complete
    // packing. Match every old route with the same row's route on the new
    // board, removing only the newly owned site. No match introduces a claim
    // on a teammate's site. Consequently the largest single loss on that site
    // bounds the entire team's loss, even if its best assignment is unknown.
    std::optional<MilliFunds> stockRouteMargin(const MasterFutureCatalog &catalog,
        const RelativeStockQuote &before, const RelativeStockQuote &after,
        const RelativeStockComparison &comparison, RefinementLedger &ledger)
    {
        if (!catalog.complete || !comparison.valid() || !comparison.commonEnemyCancelled) return std::nullopt;
        const auto left = stockInstance(catalog, before), right = stockInstance(catalog, after);
        if (!left || !right) return std::nullopt;
        int claimed = NO_SEQUENTIAL_NODE;
        for (int n = 0; n < left->nodeCount(); ++n)
        {
            if (left->capturedNodes[n] == right->capturedNodes[n]) continue;
            if (left->capturedNodes[n] || claimed != NO_SEQUENTIAL_NODE) return std::nullopt;
            claimed = n;
        }
        if (claimed == NO_SEQUENTIAL_NODE) return std::nullopt;
        MilliFunds maxLoss = 0;
        for (std::size_t row = 0; row < left->rows.size(); ++row)
        {
            const auto old = stockRowOptions(*left, static_cast<int>(row), ledger);
            const auto next = stockRowOptions(*right, static_cast<int>(row), ledger);
            if (!old || !next) return std::nullopt;
            std::map<std::vector<int>, MilliFunds> matched{{{}, 0}};
            for (const auto &option : *next)
                matched.emplace(option.nodes, option.value);
            for (const auto &option : *old)
            {
                if (!ledger.consume(option.nodes.size() + 1, RefinementWork::MasterWitness)) return std::nullopt;
                auto sites = option.nodes;
                const bool usesSite = std::erase(sites, claimed) != 0;
                const auto found = matched.find(sites);
                if (found == matched.end()) return std::nullopt;
                MilliFunds loss = option.value;
                if (!RelativeStockDetail::checkedSubtract(loss, found->second) || (!usesSite && loss > 0))
                    return std::nullopt;
                if (usesSite) maxLoss = std::max(maxLoss, loss);
            }
        }
        MilliFunds gain = comparison.ownedDelta;
        if (!RelativeStockDetail::checkedAdd(gain, comparison.economicDelta) ||
            !RelativeStockDetail::checkedSubtract(gain, maxLoss)) return std::nullopt;
        return gain;
    }


    // Compare two complete teams on the stock planner's own objective. Both
    // use the same field and fixed enemy term. The later combat comparison
    // still owns replies, buying, and terminal outcomes.
    std::optional<AssignmentResult> compatibleStockActions(
        const MasterAssignmentProblem &problem, const AssignmentResult &incumbent,
        const std::vector<std::int32_t> &proposal, MasterAssignmentEvaluation before,
        MasterAssignmentEvaluator &evaluator, RefinementLedger &ledger)
    {
        auto state = std::vector<std::int32_t>(problem.incumbentChoices().begin(), problem.incumbentChoices().end());
        if (proposal.size() != state.size()) return std::nullopt;
        bool changed = false;
        for (const auto &actor : problem.actors())
        {
            const auto slot = static_cast<std::size_t>(actor.canonicalSlot);
            if (state[slot] == proposal[slot] || !problem.canChangeActor(actor.canonicalSlot)) continue;
            const auto &old = actor.candidates.at(state[slot]);
            const auto &next = actor.candidates.at(proposal[slot]);
            const auto &facts = next.source.replyFacts;
            if ((old.actionKind != PlanBundleKind::Wait && old.actionKind != PlanBundleKind::Move) ||
                !next.capture || next.destination != old.destination || next.source.bundle.path != old.source.bundle.path ||
                !facts || !facts->tile || !facts->tile->shooters.empty()) continue;
            state[slot] = proposal[slot];
            changed = true;
        }
        if (!changed) return std::nullopt;
        const auto retained = replayMasterAssignment(problem, state, ReplyPredictionSetup{});
        if (!retained.complete() || retained.ordering.order != incumbent.executionOrder) return std::nullopt;
        auto after = evaluator.evaluate({problem.identity(), state});
        if (!after.quote() || !before.quote() || !evaluator.futureCatalog()) return std::nullopt;
        const auto comparison = evaluator.compare(before, after);
        const auto startWork = ledger.balance();
        const auto margin = stockRouteMargin(*evaluator.futureCatalog(), *before.quote(), *after.quote(), comparison, ledger);
        qInfo() << "Compatible stock routes" << margin.value_or(0) << "verified" << margin.has_value()
                << "pairedWork" << startWork - ledger.balance();
        if (!margin || *margin <= 0) return std::nullopt;
        auto result = incumbent;
        for (auto &selection : result.selections)
        {
            const auto actors = problem.actors();
            const auto actor = std::ranges::find(actors, selection.engineUnitId, &MasterActor::engineUnitId);
            if (actor == actors.end()) return std::nullopt;
            const auto slot = static_cast<std::size_t>(actor->canonicalSlot);
            selection.seatedValue = retained.seatedValues[slot];
            if (state[slot] == selection.candidateIndex) continue;
            selection.actionIndex = retained.actionIndices[slot];
            selection.candidateIndex = state[slot];
            selection.phase = AssignmentResult::SelectionPhase::Master;
            selection.completeValues.clear();
        }
        result.plan = retained.plan;
        result.executionOrder = retained.ordering.order;
        return result;
    }

    MasterStockControlStatus attemptRelativeStock(
        ControlContext & context, const MasterAssignmentProblem & problem,
        const AssignmentInput & input, const PropertyStockField & field,
        std::optional<AssignmentResult>* compatible)
    {
        auto attempt = beginAttempt(context, MasterStockAuthority::RelativeStock);
        MasterAssignmentStockContext stock;
        attempt.stockStatus = stock.prepare(field, input, context.ledger, &context.profile, context.config.tierAuditMode);
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        if (const auto* measure = stock.laterListMeasure())
        {
            context.laterList = *measure;
        }
#endif
        if (attempt.stockStatus != MasterStockSetupStatus::Prepared)
        {
            return finishAttempt(context, attempt);
        }
        attempt.stockStatus = stock.prepareOperationService();
        if (attempt.stockStatus != MasterStockSetupStatus::Prepared)
        {
            return finishAttempt(context, attempt);
        }
        MasterAssignmentEvaluator evaluator(problem, context.ledger, *stock.session(), stock.tariffs(), &context.profile);
        searchAndInstall(context, attempt, problem, evaluator);
        if (compatible && *compatible && attempt.installed.proof && stock.session()->futureCatalogMatchesSolver())
        {
            const auto &proof = *attempt.installed.proof;
            auto candidate = compatibleStockActions(problem, **compatible, proof.challenger.state.choices,
                proof.incumbent, evaluator, context.ledger);
            if (candidate && handOffMasterPlan(&problem, *candidate).handoff)
                **compatible = std::move(*candidate);
        }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        attempt.witnessCharges = evaluator.witnessCharges();
#endif
        return finishAttempt(context, attempt);
    }

    QJsonObject controlRecord(
        const ControlContext & context, const std::optional<MasterCaptureResult> & capture,
        CaptureScreen screen, std::int64_t computationNanos)
    {
        const auto & measured = context.timing;
        const auto & assignment = context.assignment;
        QJsonObject record;
        if (!context.attempts.isEmpty())
        {
            record = context.attempts.last().toObject();
        }
        const QJsonObject totalRecord{
            {"schemaVersion", 3}, {"mode", QStringLiteral("stockControl")},
            {"selectedAuthority", masterStockAuthorityName(measured.selectedAuthority)},
            {"status", masterStockControlStatusName(measured.status)},
            {"day", context.identity.day}, {"player", context.identity.playerId},
            {"planSequence", QString::number(context.identity.planSequence)}, {"horizon", context.identity.horizonTurns},
            {"allowance", QString::number(context.allowance)}, {"masterWork", masterWorkRecord(measured.work)},
            {"staleProbe", static_cast<int>(context.config.staleProbe)},
            {"anyStaleProbeApplied", measured.staleProbeApplied},
            {"captureDetailPresentation", measured.detailPresentation},
            {"lastRefusal", masterRefusalRecord(context.ledger.lastRefusal())},
            {"constructorStatus", static_cast<int>(assignment.stockConstructionStatus)},
            {"constructorComplete", measured.constructorComplete},
            {"constructorWork", QString::number(assignment.stockConstructionWork)},
            {"nonStockRefinementRolledBack", assignment.nonStockRefinementRolledBack},
            {"captureSourceStatus", masterNativeSourceStatusName(measured.nativeSourceStatus)},
            {"installationStatus", static_cast<int>(measured.installationStatus)},
            {"installed", measured.status == MasterStockControlStatus::Installed},
            {"peakReservedWork", QString::number(measured.reservedWork)},
            {"totalFinalizationWork", QString::number(measured.finalizationWork)},
            {"constructorNanos", QString::number(measured.constructorNanos)},
            {"searchNanos", QString::number(measured.searchNanos)},
            {"finalizationNanos", QString::number(measured.finalizationNanos)},
            {"computationNanos", QString::number(computationNanos)},
            {"captureScreen", static_cast<int>(screen)},
            {"profile", masterProfileRecord(context.profile)}, {"attempts", context.attempts}};
        for (auto field = totalRecord.begin(); field != totalRecord.end(); ++field)
        {
            record.insert(field.key(), field.value());
        }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        record.insert(TRACE_LATER_LIST, masterLaterListRecord(context.laterList));
#endif
        if (capture)
        {
            record.insert("extractionWork", QString::number(capture->workUsed));
            record.insert("extractionError", static_cast<int>(capture->extraction.error));
            if (capture->extraction.valid())
            {
                record.insert("model", masterModelRecord(*capture->extraction.problem));
            }
        }
        return record;
    }

    MasterStockControlTiming finishControl(
        ControlContext & context, MasterProfileScope & timing,
        const std::optional<MasterCaptureResult> & capture, CaptureScreen screen, MasterStockControlStatus status)
    {
        timing.finish();
        auto & measured = context.timing;
        measured.status = status;
        measured.work = context.ledger.stats();
        if (context.config.exportMode == MasterShadowExportMode::Full)
        {
            const auto exportStart = context.total.nsecsElapsed();
            const auto line = QJsonDocument(controlRecord(context, capture, screen, exportStart)).toJson(QJsonDocument::Compact) + '\n';
            QFile output(context.config.outputPath);
            const bool written = output.open(QIODevice::WriteOnly | QIODevice::Append) &&
                output.write(line) == line.size() && output.flush();
            measured.exportSucceeded = written && measured.exportSucceeded;
            if (!written)
            {
                qWarning() << "Master stock control receipt write failed:" << output.fileName() << output.errorString();
            }
            measured.exportNanos = context.total.nsecsElapsed() - exportStart;
        }
        measured.totalNanos = context.total.nsecsElapsed();
        return measured;
    }

    MasterStockControlTiming controlWithCapture(
        const MasterShadowConfig & config, GameMap & map, const AssignmentInput & input,
        AssignmentResult & assignment, const PropertyStockField & field, std::span<const PropertyFacts> properties,
        const DecisionTraceIdentity & identity, std::optional<MasterCaptureResult> & capture,
        std::optional<AssignmentResult>* incoming)
    {
        MasterStockControlTiming measured;
        if (!config.controlsStock())
        {
            return measured;
        }
        QElapsedTimer total;
        total.start();
        RefinementLedger ledger;
        MasterAssignmentProfile profile;
        MasterProfileScope controlTiming(&profile, MasterPhase::StockControl);
        CaptureScreen screen{CaptureScreen::Inapplicable};
        ControlContext context{
            config, masterTurnAllowance(config, input.actors.size()), identity, assignment, ledger, profile, measured,
            total, {}};
        if (!ledger.open(context.allowance))
        {
            assignment = AssignmentResult{};
            assignment.stockConstructionStatus = StockConstructionStatus::InvalidInput;
            return finishControl(context, controlTiming, capture, screen, MasterStockControlStatus::ConstructorRefused);
        }
        const auto constructorStart = total.nsecsElapsed();
        {
            MasterProfileScope timing(&profile, MasterPhase::IncomingConstruction);
            assignment = MaximumValueAssignment::constructStockIncumbent(input, ledger);
        }
        measured.constructorNanos = total.nsecsElapsed() - constructorStart;
        measured.constructorComplete = assignment.stockConstructionStatus == StockConstructionStatus::Complete;
        if (!measured.constructorComplete)
        {
            return finishControl(context, controlTiming, capture, screen, MasterStockControlStatus::ConstructorRefused);
        }
        capture = captureMasterAssignment(input, assignment, identity.planSequence, ledger, &profile);
        if (!capture->extraction.valid())
        {
            return finishControl(context, controlTiming, capture, screen, MasterStockControlStatus::ExtractionRefused);
        }
        if (incoming) incoming->emplace(assignment);
        const auto & problem = *capture->extraction.problem;
        screen = screenCapture(problem, field, properties, ledger);
        if (screen == CaptureScreen::BudgetExhausted)
        {
            return finishControl(context, controlTiming, capture, screen, MasterStockControlStatus::StockPreparationRefused);
        }
        if (screen == CaptureScreen::Eligible &&
            attemptCapture(context, problem, map) == MasterStockControlStatus::Installed)
        {
            return finishControl(context, controlTiming, capture, screen, MasterStockControlStatus::Installed);
        }
        const auto status = attemptRelativeStock(context, problem, input, field, incoming);
        return finishControl(context, controlTiming, capture, screen, status);
    }
}

namespace Coordinator
{
    bool StockProgressPricing::prepare(const PropertyStockField &field,const AssignmentInput &input,
                                       std::int64_t offer,std::int64_t &work)
    {
        if(!ledger.open(work))return false;
        RefinementLedger::WorkLimit cap(ledger,std::min(offer,work));
        const auto charge=qScopeGuard([&]{work-=ledger.stats().spent();});
        return stock.prepare(field,input,ledger,nullptr,SequentialTierAuditMode::CountersOnly)==MasterStockSetupStatus::Prepared &&
               stock.prepareOperationService()==MasterStockSetupStatus::Prepared && stock.session()->futureCatalogMatchesSolver();
    }

    QJsonObject priceStockProgress(const MasterAssignmentProblem &problem,StockProgressPricing &pricing,
        const std::vector<int> &before,const std::vector<int> &after,StockProgressArms &arms,
        std::int64_t allowance,std::int64_t &work)
    {
        auto &owner=pricing.ledger;
        const auto offer=std::min({allowance,work,owner.balance()});
        if(offer<=0)return {{"status","NO_STOCK_QUOTE_ALLOWANCE"}};
        auto *session=pricing.stock.session();
        if(!session || !session->futureCatalogMatchesSolver())return {{"status","STOCK_INPUTS_UNAVAILABLE"}};
        const auto start=owner.stats();
        RefinementLedger::WorkLimit cap(owner,offer);
        const auto debit=qScopeGuard([&]{work-=owner.stats().spent()-start.spent();});
        MasterAssignmentEvaluator evaluator(problem,owner,*session,pricing.stock.tariffs());
        auto old=evaluator.evaluate({problem.identity(),before}),next=evaluator.evaluate({problem.identity(),after});
        const auto relative=evaluator.compare(old,next);
        if(!old.quote() || !next.quote() || !old.lowerReplayed || !old.upperVerified || !next.lowerReplayed || !next.upperVerified ||
           !old.quote()->ourLowerWitnessReplays || !next.quote()->ourLowerWitnessReplays ||
           !relative.valid() || !relative.commonEnemyCancelled || !evaluator.futureCatalog())
            return {{"status","NO_COMMON_ENEMY_STOCK_PROOF"}};
        std::int64_t siblingWork=0;
        bool usedSibling=false;
        // The relative quotes need their original ledger. The pure packing
        // search instead sees only the actual unspent portion of this offer.
        const auto packingBudget=std::min(owner.balance(),offer-(owner.stats().granted-start.granted));
        if(packingBudget<0)return {{"status","NO_STOCK_QUOTE_ALLOWANCE"}};
        RefinementLedger ledger;
        if(!ledger.open(packingBudget))return {{"status","NO_STOCK_QUOTE_ALLOWANCE"}};
        const auto packingGrant=owner.draw(packingBudget,RefinementWork::MasterWitness);
        if(packingGrant==NO_GRANT)return {{"status","NO_STOCK_QUOTE_ALLOWANCE"}};
        const auto refund=qScopeGuard([&]{owner.refund(packingGrant,ledger.balance());});
        const auto solve=[&](const RelativeStockQuote &quote,StockProgressArmBounds &range,
            std::span<const MilliFunds> prices,std::optional<MilliFunds> required)->std::optional<CompatiblePackingResult>{
            const auto instance=stockInstance(*evaluator.futureCatalog(),quote);
            if(!instance)return {};
            std::vector<std::vector<SequentialDetail::RowOption>> menus;
            for(int i=0;i<int(instance->rows.size());++i)
            {
                auto options=stockRowOptions(*instance,i,ledger);
                if(!options)return {};
                menus.push_back(std::move(*options));
            }
            std::optional<CompatiblePackingResult> constructive;
            if(required && prices.size()==instance->capturedNodes.size()){
                const auto start=ledger.stats().spent();
                std::optional<StockSiblingWitness> witness;
                {
                    RefinementLedger::WorkLimit limit(ledger,std::min<std::int64_t>(300000,ledger.balance()/2));
                    witness=stockSiblingWitness(*instance,menus,prices,*required,ledger);
                    // The shortlist can finish without a witness because its
                    // sixteen selected routes omit a necessary reallocation.
                    // Use only unconsumed credits from the same work limit.
                    if(!witness && ledger.balance()>0)
                        witness=stockTargetWitness(*instance,menus,prices,*required,ledger);
                }
                siblingWork+=ledger.stats().spent()-start;
                if(witness && witness->value>=*required && witness->value<=range.upper){
                    usedSibling=true;
                    if(range.sitePrices.empty())range.sitePrices.assign(prices.begin(),prices.end());
                    constructive=CompatiblePackingResult{witness->value,range.upper,witness->work,std::move(witness->witness)};
                }
            }
            // A successful constructive test is only a lower bound. Preserve
            // it while independently closing the full ceiling; this team can
            // become the incumbent on the very next physical option.
            auto full=compatiblePackingBounds(*instance,ledger,menus,&range.sitePrices);
            if(!full)return constructive;
            if(constructive && constructive->lower>full->lower){
                if(constructive->lower>full->upper)return {};
                full->lower=constructive->lower;
                full->witness=std::move(constructive->witness);
            }
            return full;
        };
        const auto initial=[&](const std::vector<int> &choices,const RelativeStockQuote &quote)->StockProgressArmBounds&{
            auto &range=arms.try_emplace(choices,StockProgressArmBounds{quote.ours.lower,quote.ours.upper,0}).first->second;
            range.lower=std::max(range.lower,quote.ours.lower);range.upper=std::min(range.upper,quote.ours.upper);
            return range;
        };
        const auto refine=[&](StockProgressArmBounds &range,const RelativeStockQuote &quote,std::int64_t share,
                              std::span<const MilliFunds> prices={},std::optional<MilliFunds> required={}) {
            if(range.lower<range.upper && (share>range.previousAllowance || (required && !prices.empty()))){
                RefinementLedger::WorkLimit cap(ledger,share);
                const auto spentBefore=ledger.stats().spent();
                if(const auto bounded=solve(quote,range,prices,required)){
                    range.lower=std::max(range.lower,bounded->lower);range.upper=std::min(range.upper,bounded->upper);
                }
                range.previousAllowance=std::max(range.previousAllowance,ledger.stats().spent()-spentBefore);
            }
        };
        const auto beforeKnown=arms.find(before);
        const bool leftExact=beforeKnown!=arms.end() && beforeKnown->second.lower==beforeKnown->second.upper;
        const auto afterKnown=arms.find(after);
        const bool rightExact=afterKnown!=arms.end() && afterKnown->second.lower==afterKnown->second.upper;
        auto &left=initial(before,*old.quote()),&right=initial(after,*next.quote());
        MilliFunds margin{},ceiling{};
        const auto compare=[&](){
            margin=relative.ownedDelta;ceiling=relative.ownedDelta;
            return left.lower<=left.upper && right.lower<=right.upper &&
                RelativeStockDetail::checkedAdd(margin,relative.economicDelta) &&
                RelativeStockDetail::checkedAdd(margin,right.lower) && RelativeStockDetail::checkedSubtract(margin,left.upper) &&
                RelativeStockDetail::checkedAdd(ceiling,relative.economicDelta) &&
                RelativeStockDetail::checkedAdd(ceiling,right.upper) && RelativeStockDetail::checkedSubtract(ceiling,left.lower);
        };
        if(!compare())return {{"status","INVALID_STOCK_BOUNDS_OR_ARITHMETIC"}};
        if(margin<=0 && ceiling>0){
            refine(left,*old.quote(),rightExact?ledger.balance():ledger.balance()/2);
            if(!compare())return {{"status","INVALID_STOCK_BOUNDS_OR_ARITHMETIC"}};
            if(margin<=0 && ceiling>0){
                MilliFunds target=left.upper;
                if(!RelativeStockDetail::checkedSubtract(target,relative.ownedDelta) ||
                   !RelativeStockDetail::checkedSubtract(target,relative.economicDelta) ||
                   !RelativeStockDetail::checkedAdd(target,1))return {{"status","INVALID_SIBLING_THRESHOLD"}};
                refine(right,*next.quote(),ledger.balance(),left.sitePrices,target);
            }
            if(!compare())return {{"status","INVALID_STOCK_BOUNDS_OR_ARITHMETIC"}};
        }
        return {{"status",margin>0?"STRICT_STOCK_IMPROVEMENT":ceiling<=0?"NO_STRICT_STOCK_MARGIN":"STOCK_INTERVAL_OVERLAP"},
            {"incumbentLower",qint64(left.lower)},{"incumbentUpper",qint64(left.upper)},
            {"candidateLower",qint64(right.lower)},{"candidateUpper",qint64(right.upper)},
            {"cachedIncumbent",leftExact},{"cachedCandidate",rightExact},{"marginUpper",qint64(ceiling)},
            {"commonEnemyCancelled",true},{"ownedDelta",qint64(relative.ownedDelta)},{"economicDelta",qint64(relative.economicDelta)},
            {"margin",qint64(margin)},{"siblingWitness",usedSibling},{"siblingWork",qint64(siblingWork)},
            {"usedWork",qint64(owner.stats().spent()-start.spent()-ledger.balance())}};
    }

    QString masterStockAuthorityName(MasterStockAuthority authority)
    {
        switch (authority)
        {
            case MasterStockAuthority::Constructor:
                return QStringLiteral("constructor");
            case MasterStockAuthority::RelativeStock:
                return QStringLiteral("relativeStock");
            case MasterStockAuthority::GuardedCapture:
                return QStringLiteral("guardedTerminalCapture");
        }
        return QStringLiteral("invalid");
    }

    QString masterStockControlStatusName(MasterStockControlStatus status)
    {
        switch (status)
        {
            case MasterStockControlStatus::Disabled:
                return QStringLiteral("DISABLED");
            case MasterStockControlStatus::ConstructorRefused:
                return QStringLiteral("CONSTRUCTOR_REFUSED");
            case MasterStockControlStatus::ExtractionRefused:
                return QStringLiteral("EXTRACTION_REFUSED");
            case MasterStockControlStatus::StockPreparationRefused:
                return QStringLiteral("STOCK_PREPARATION_REFUSED");
            case MasterStockControlStatus::FinalizationRefused:
                return QStringLiteral("FINALIZATION_REFUSED");
            case MasterStockControlStatus::Retained:
                return QStringLiteral("RETAINED");
            case MasterStockControlStatus::Installed:
                return QStringLiteral("INSTALLED");
        }
        return QStringLiteral("INVALID");
    }

    MasterStockControlTiming controlMasterAssignment(
        const MasterShadowConfig & config, GameMap & map, const AssignmentInput & input,
        AssignmentResult & assignment, const PropertyStockField & field, std::span<const PropertyFacts> properties,
        const DecisionTraceIdentity & identity, MasterPlanHandoffResult* pHandoff, bool requireFullComparison)
    {
        std::optional<MasterCaptureResult> capture;
        std::optional<AssignmentResult> incoming;
        const auto measured = controlWithCapture(config, map, input, assignment, field, properties, identity, capture,
                                                 requireFullComparison && pHandoff ? &incoming : nullptr);
        // After the receipt, so the receipt and the timing are the same with and without a handoff.
        if (pHandoff != nullptr)
        {
            const auto started = std::chrono::steady_clock::now();
            const bool extracted = capture && capture->extraction.valid();
            *pHandoff = handOffMasterPlan(extracted ? &*capture->extraction.problem : nullptr, assignment);
            // A stock improvement can remain the default for a quiet economic turn.
            // A current firing opportunity, an exposed endpoint or a blocked purchase
            // gives the broader comparison authority over both complete teams.
            const auto needsTacticalComparison = [&] {
                for (const auto &actor : input.actors)
                {
                    for (const auto &choice : actor.candidates)
                    {
                        const bool canFire = std::ranges::any_of(choice.bundle.components, [](const auto &part) {
                            return part.kind == ComponentKind::Fire;
                        });
                        if (canFire ||
                            !choice.replyFacts || !choice.replyFacts->tile ||
                            !choice.replyFacts->tile->shooters.empty()) return true;
                    }
                }
                return false;
            };
            const auto needsBroaderValuation = [&](const AssignmentResult &team) {
                for (int i = 0; i < team.plan.actionCount(); ++i)
                {
                    const auto &action = team.plan.action(i);
                    if (action.actionId != "ACTION_WAIT" && action.actionId != "ACTION_CAPTURE") return true;
                    if (action.actionId == "ACTION_WAIT" && std::ranges::any_of(properties, [&](const PropertyFacts &site) {
                        return site.ownerId == identity.playerId && site.canProduce &&
                               site.x == action.destination.x && site.y == action.destination.y;
                    })) return true;
                }
                return false;
            };
            if (extracted && incoming && measured.status == MasterStockControlStatus::Installed &&
                (needsTacticalComparison() || needsBroaderValuation(assignment) || needsBroaderValuation(*incoming)))
            {
                // The stock proof establishes an improvement on its own terms.
                // Preserve the actual constructor orders and submit the stock
                // result as a candidate when a broader valuation owns the turn.
                auto original = handOffMasterPlan(&*capture->extraction.problem, *incoming);
                if (original.handoff)
                {
                    if (pHandoff->handoff && (pHandoff->handoff->choices != original.handoff->choices ||
                                              assignment.executionOrder != incoming->executionOrder))
                    {
                        original.handoff->stockProposal = MasterOrderProposal{
                            std::move(pHandoff->handoff->choices), assignment.executionOrder};
                    }
                    assignment = std::move(*incoming);
                    *pHandoff = std::move(original);
                }
            }
            pHandoff->nanos = nanosSince(started);
        }
        return measured;
    }
}
