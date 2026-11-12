#include "ai/coordinator/masterassignmenttrace.h"
#include "ai/coordinator/masterassignmentshadow.h"
#include "ai/coordinator/masternativesource.h"

#include <QDebug>
#include <QJsonDocument>

namespace
{
    constexpr auto TRACE_OPERATION = "operation";
    constexpr auto TRACE_INCOMING = "incoming";
    constexpr auto TRACE_CHALLENGER = "challenger";
    constexpr auto TRACE_TERM = "term";
    constexpr auto TRACE_ALLOWANCE = "allowance";
    constexpr auto TRACE_WORK_BEFORE = "workBefore";
    constexpr auto TRACE_WORK_AFTER = "workAfter";
    constexpr auto TRACE_STATUS = "status";
    constexpr auto TRACE_COMPLETED = "completed";
    constexpr auto TRACE_ADMITTED = "admitted";
    constexpr auto TRACE_TICKET = "ticket";
    constexpr auto TRACE_LOWER_RELEVANT = "lowerRelevant";
    constexpr auto TRACE_UPPER_RELEVANT = "upperRelevant";
    constexpr auto TRACE_ROW_REVISION = "rowCacheRevision";
    constexpr auto TRACE_PLAN_PRICING = "planPricing";
    constexpr auto TRACE_BOUND_CAP = "boundCap";
    constexpr auto TRACE_WORK = "work";
    constexpr auto TRACE_ROUNDS = "rounds";
    constexpr auto TRACE_SCAN_UPPER = "scanUpper";
    constexpr auto TRACE_SLOT = "slot";
    constexpr auto TRACE_ACTOR_ID = "actorId";
    constexpr auto TRACE_CANDIDATE = "candidate";
    constexpr auto TRACE_GAIN = "gain";
    constexpr auto TRACE_CONFLICT_FREE = "conflictFree";
    constexpr auto TRACE_VERDICT = "verdict";
    constexpr auto TRACE_AUDITED_PLAN = "plan";
    constexpr auto TRACE_NANOS = "nanos";
    constexpr auto TRACE_TRUSTED_FLOOR = "trustedFloor";
    constexpr auto TRACE_WITNESS_VALUE = "witnessValue";
    constexpr auto TRACE_PROVEN_NOT_BETTER = "provenNotBetter";
    constexpr auto TRACE_OPEN = "open";
    constexpr auto TRACE_CONFLICTING = "conflicting";
    constexpr auto TRACE_UNAVAILABLE = "unavailable";
    constexpr auto TRACE_CHANGES = "changes";
    constexpr auto TRACE_INCUMBENT_OURS_CAP = "incumbentOursCap";
    constexpr auto TRACE_CHALLENGER_OURS_CAP = "challengerOursCap";

    QJsonObject planBoundCapRecord(const Coordinator::MasterPlanBoundCapStats & cap)
    {
        return {{TRACE_STATUS, static_cast<int>(cap.status)},
            {TRACE_SCAN_UPPER, Coordinator::masterValueRecord(cap.scanUpper)},
            {"oursCeiling", Coordinator::masterValueRecord(cap.oursCeiling)},
            {TRACE_WORK, QString::number(cap.work)},
            {"incumbentOursUpperBefore", Coordinator::masterValueRecord(cap.incumbentOursUpperBefore)},
            {"improvedAtCap", cap.improvedAtCap},
            {"cappedProofDropped", cap.cappedProofDropped}};
    }

    QJsonObject planNeighborAuditRecord(const Coordinator::MasterPlanNeighborAuditStats & audit)
    {
        QJsonArray changes;
        for (const auto & change : audit.changes)
        {
            changes.append(QJsonObject{{TRACE_ACTOR_ID, change.actorId}, {TRACE_SLOT, change.slot},
                {TRACE_CANDIDATE, change.candidate}, {TRACE_GAIN, Coordinator::masterValueRecord(change.gain)},
                {TRACE_CONFLICT_FREE, change.conflictFree}, {TRACE_VERDICT, static_cast<int>(change.verdict)}});
        }
        return {{TRACE_STATUS, static_cast<int>(audit.status)},
            {TRACE_AUDITED_PLAN, static_cast<int>(audit.plan)},
            {TRACE_WORK, QString::number(audit.work)},
            {TRACE_NANOS, QString::number(audit.nanos)},
            {TRACE_ROUNDS, audit.rounds},
            {TRACE_SCAN_UPPER, Coordinator::masterValueRecord(audit.scanUpper)},
            {TRACE_TRUSTED_FLOOR, Coordinator::masterValueRecord(audit.trustedFloor)},
            {TRACE_WITNESS_VALUE, Coordinator::masterValueRecord(audit.witnessValue)},
            {TRACE_PROVEN_NOT_BETTER, QString::number(audit.provenNotBetter)},
            {TRACE_OPEN, QString::number(audit.open)},
            {TRACE_CONFLICTING, QString::number(audit.conflicting)},
            {TRACE_UNAVAILABLE, QString::number(audit.unavailable)},
            {TRACE_CHANGES, changes}};
    }

    bool liveEvaluation(const Coordinator::MasterShadowObservation & observation,
                        const Coordinator::MasterAssignmentEvaluation & evaluation)
    {
        if (observation.capture == nullptr || !observation.capture->extraction.valid() ||
            !Coordinator::validMasterState(*observation.capture->extraction.problem, evaluation.state))
        {
            return false;
        }
        if (!evaluation.lowerReplayed)
        {
            return true;
        }
        return observation.session != nullptr && evaluation.proof &&
            observation.session->owns(evaluation.proof->quote) &&
            observation.session->replayLowerProof(*evaluation.proof);
    }

    QJsonArray tile(Coordinator::TilePoint point)
    {
        return {point.x, point.y};
    }

    QJsonArray keyRecord(const Coordinator::ContinuationKey & key)
    {
        QJsonArray result;
        for (const auto & action : key)
        {
            result.append(QJsonArray{action.row(), action.x(), action.y(), action.captures()});
        }
        return result;
    }

    QJsonObject operationEvaluationRecord(const Coordinator::MasterAssignmentEvaluation & evaluation)
    {
        QJsonObject record{
            {"choices", Coordinator::masterChoicesRecord(evaluation.state.choices)},
            {TRACE_STATUS, static_cast<int>(evaluation.status)},
            {"objectiveStatus", static_cast<int>(evaluation.objectiveStatus)},
            {"lowerReplayed", evaluation.lowerReplayed}, {"upperVerified", evaluation.upperVerified},
            {"seatedEconomic", Coordinator::masterValueRecord(evaluation.replay
                ? std::optional<Coordinator::MilliFunds>{evaluation.replay->seatedEconomicTotal} : std::nullopt)},
            {"lower", Coordinator::masterValueRecord(evaluation.objective
                ? std::optional<Coordinator::MilliFunds>{evaluation.objective->lower} : std::nullopt)},
            {"upper", Coordinator::masterValueRecord(evaluation.objective
                ? std::optional<Coordinator::MilliFunds>{evaluation.objective->upper} : std::nullopt)}};
        if (const auto* quote = evaluation.quote())
        {
            record.insert("quote", QJsonObject{
                {"key", keyRecord(quote->key)},
                {"capturedColumns", Coordinator::masterChoicesRecord(quote->capturedColumns)},
                {"owned", QString::number(quote->owned)},
                {"ours", QJsonArray{QString::number(quote->ours.lower), QString::number(quote->ours.upper)}},
                {"enemy", QJsonArray{QString::number(quote->enemy.lower), QString::number(quote->enemy.upper)}},
                {"ourSearchCompleted", quote->ourSearchCompleted},
                {"enemySearchCompleted", quote->enemySearchCompleted},
                {"ourUpperCertificateId", QString::number(quote->ourUpperCertificateId)}});
        }
        return record;
    }

    QJsonValue operationEvaluationRecord(const Coordinator::MasterAssignmentEvaluation* evaluation)
    {
        return evaluation == nullptr ? QJsonValue(QJsonValue::Null) : operationEvaluationRecord(*evaluation);
    }

    QJsonObject operationOfferRecord(const Coordinator::MasterOperationOffer & offer)
    {
        const auto & metadata = offer.metadata;
        const auto & preparation = offer.preparation;
        return {{TRACE_STATUS, static_cast<int>(offer.status)}, {TRACE_TERM, static_cast<int>(offer.term)},
            {TRACE_ALLOWANCE, QString::number(offer.allowance)},
            {"remainingRequiredWork", QString::number(offer.remainingRequiredWork)},
            {"inspectionWork", QString::number(offer.inspectionWork)},
            {"preparationWork", QString::number(offer.preparationWork)},
            {"quotaWork", QString::number(offer.quotaWork)},
            {"metadata", QJsonObject{{"valid", metadata.valid},
                {TRACE_ROW_REVISION, QString::number(metadata.rowCacheRevision)},
                {"ourRerunWork", QString::number(metadata.ourRerunWork)},
                {"enemyRerunWork", QString::number(metadata.enemyRerunWork)},
                {"certificateAdmissionWork", QString::number(metadata.certificateAdmissionWork)},
                {"certificateReadinessWork", QString::number(metadata.certificateReadinessWork)},
                {"certificateEntryWork", QString::number(metadata.certificateEntryWork)}}},
            {"preparation", QJsonObject{{TRACE_STATUS, static_cast<int>(preparation.status)},
                {TRACE_ROW_REVISION, QString::number(preparation.rowCacheRevision)},
                {"readinessChecked", preparation.readinessChecked},
                {"paidWork", QString::number(preparation.paidWork)},
                {"requiredPreparationWork", QString::number(preparation.requiredPreparationWork)},
                {"scanWork", QString::number(preparation.scanWork)},
                {"minimumNativeWork", QString::number(preparation.minimumNativeWork)}}}};
    }

    QJsonObject ticketRecord(const Coordinator::MasterRefinementTicketReceipt & ticket)
    {
        return {{TRACE_TICKET, operationEvaluationRecord(ticket.ticket)},
            {"cursorBefore", static_cast<int>(ticket.cursorBefore)}, {"cursorAfter", static_cast<int>(ticket.cursorAfter)},
            {"aggregateBefore", QString::number(ticket.aggregateBefore)},
            {"aggregateAfter", QString::number(ticket.aggregateAfter)},
            {"refinementsBefore", QString::number(ticket.refinementsBefore)},
            {"refinementsAfter", QString::number(ticket.refinementsAfter)},
            {"focusBefore", QString::number(ticket.focusBefore)}, {"focusAfter", QString::number(ticket.focusAfter)},
            {"ticketWasFocus", ticket.ticketWasFocus}, {"workBalancedSelection", ticket.workBalancedSelection},
            {"proofBefore", ticket.proofBefore}, {"proofAfter", ticket.proofAfter},
            {TRACE_COMPLETED, ticket.completed}, {"fundingParked", ticket.fundingParked}};
    }

    QJsonValue operationIntervalRecord(const std::optional<Coordinator::StockInterval> & interval)
    {
        return interval ? QJsonValue(QJsonArray{QString::number(interval->lower), QString::number(interval->upper)})
                        : QJsonValue(QJsonValue::Null);
    }

    QJsonArray valuesRecord(std::span<const std::int64_t> values)
    {
        QJsonArray result;
        for (const auto value : values)
        {
            result.append(QString::number(value));
        }
        return result;
    }

    QJsonArray stockRowsRecord(std::span<const Coordinator::PropertyStockRow> rows)
    {
        QJsonArray result;
        for (const auto & row : rows)
        {
            result.append(QJsonObject{{"knowledgeIndex", row.knowledgeIndex}, {"gridIdentity", row.gridIdentity},
                {"movementPoints", row.movementPoints}, {"capturePoints", row.capturePoints},
                {"captureRate", row.captureRate}, {"tile", tile(row.tile)}});
        }
        return result;
    }

    QJsonArray stockColumnsRecord(std::span<const Coordinator::PropertyStockColumn> columns)
    {
        QJsonArray result;
        for (const auto & column : columns)
        {
            result.append(QJsonObject{{TRACE_SLOT, column.slot}, {"tile", tile(column.tile)},
                {"income", QJsonObject{{"oursPerTurn", QString::number(column.income.oursPerTurn)},
                    {"enemyPerTurn", QString::number(column.income.enemyPerTurn)}}},
                {"ownerBefore", static_cast<int>(column.ownerBefore)}, {"buildingIndex", column.buildingIndex}});
        }
        return result;
    }

    QJsonObject mobilityGridRecord(const Coordinator::MobilityCostGrid & grid)
    {
        QJsonArray directions;
        for (const auto direction : Coordinator::ENTRY_DIRECTIONS)
        {
            directions.append(static_cast<int>(direction));
        }
        QJsonArray costs;
        for (std::int32_t y = 0; y < grid.height(); ++y)
        {
            QJsonArray row;
            for (std::int32_t x = 0; x < grid.width(); ++x)
            {
                QJsonArray cell;
                for (const auto direction : Coordinator::ENTRY_DIRECTIONS)
                {
                    cell.append(grid.cost(x, y, direction));
                }
                row.append(cell);
            }
            costs.append(row);
        }
        return {{"width", grid.width()}, {"height", grid.height()}, {"entryDirections", directions}, {"costs", costs}};
    }

    QJsonObject packingFieldRecord(const Coordinator::PackingDualFieldIdentity & field)
    {
        QJsonArray columnTiles;
        for (const auto point : field.columnTiles)
        {
            columnTiles.append(tile(point));
        }
        QJsonArray grids;
        for (const auto & grid : field.mobilityGrids)
        {
            grids.append(mobilityGridRecord(grid));
        }
        return {{"horizonTurns", field.horizonTurns}, {"width", field.width}, {"height", field.height},
            {"capturePointsToCapture", field.capturePointsToCapture},
            {"ownedBaseline", QString::number(field.ownedBaseline)},
            {"ourRows", stockRowsRecord(field.ourRows)}, {"ourColumns", stockColumnsRecord(field.ourColumns)},
            {"ourWeights", valuesRecord(field.ourWeights)}, {"enemyRows", stockRowsRecord(field.enemyRows)},
            {"enemyColumns", stockColumnsRecord(field.enemyColumns)}, {"enemyWeights", valuesRecord(field.enemyWeights)},
            {"columnTiles", columnTiles}, {"columnCapturers", Coordinator::masterChoicesRecord(field.columnCapturers)},
            {"columnCapturePoints", Coordinator::masterChoicesRecord(field.columnCapturePoints)},
            {"enemyArrivals", Coordinator::masterChoicesRecord(field.enemyArrivals)}, {"mobilityGrids", grids}};
    }

    QJsonObject packingIdentityRecord(const Coordinator::PackingDualProblemIdentity & identity)
    {
        QJsonArray rowDomains;
        for (const auto & domain : identity.rowDomains)
        {
            rowDomains.append(valuesRecord(domain));
        }
        QJsonArray usedNodes;
        for (std::int32_t node = 0; node < identity.usedNodes.nodeCount(); ++node)
        {
            if (identity.usedNodes.contains(node))
            {
                usedNodes.append(node);
            }
        }
        return {{"session", QString::number(identity.session)}, {"immutableField", packingFieldRecord(identity.immutableField)},
            {"key", keyRecord(identity.key)}, {"capturedColumns", Coordinator::masterChoicesRecord(identity.capturedColumns)},
            {"rowDomains", rowDomains}, {"nodeColumns", stockColumnsRecord(identity.nodeColumns)},
            {"usedNodes", QJsonObject{{"nodeCount", identity.usedNodes.nodeCount()}, {"nodes", usedNodes}}},
            {"economic", QString::number(identity.economic)}, {"owned", QString::number(identity.owned)},
            {"incumbentFloor", QString::number(identity.incumbentFloor)}, {"fixedValue", QString::number(identity.fixedValue)}};
    }

    QJsonObject packingCertificateRecord(const Coordinator::PackingDualCertificate & certificate)
    {
        return {{"identity", packingIdentityRecord(certificate.identity)},
            {"rows", Coordinator::masterChoicesRecord(certificate.rows)},
            {"nodePrices", valuesRecord(certificate.nodePrices)}, {"rowReducedMaxima", valuesRecord(certificate.rowReducedMaxima)},
            {"dualObjective", QString::number(certificate.dualObjective)}};
    }

    QJsonArray sequentialWitnessRecord(std::span<const Coordinator::SequentialRowWitness> witness)
    {
        QJsonArray result;
        for (const auto & row : witness)
        {
            QJsonArray visits;
            for (const auto & visit : row.steps)
            {
                visits.append(QJsonArray{visit.node, visit.turn});
            }
            result.append(QJsonObject{{"row", row.row}, {"visits", visits}, {"value", QString::number(row.value)}});
        }
        return result;
    }
}

namespace Coordinator
{
    MasterAssignmentSearchTrace::MasterAssignmentSearchTrace(
        const QString & path, const DecisionTraceIdentity & identity,
        const MasterAssignmentProblem & problem, const RefinementLedger & ledger, std::int64_t reservedWork,
        const QString & attempt)
        : m_output(path),
          m_ledger(ledger),
          m_identity{{"day", identity.day}, {"player", identity.playerId},
              {"planSequence", QString::number(identity.planSequence)}, {"horizon", identity.horizonTurns}}
    {
        m_identity.insert("attempt", attempt);
        m_timer.start();
        if (!m_output.open(QIODevice::WriteOnly | QIODevice::Append))
        {
            failed();
        }
        write(Event::Begin, ledger.stats().spent(), {{"schemaVersion", 1},
            {"model", masterModelRecord(problem)}, {"reservedWork", QString::number(reservedWork)},
            {"initialChoices", masterChoicesRecord(problem.incumbentChoices())},
            {"ledger", masterWorkRecord(ledger.stats())}});
    }

    void MasterAssignmentSearchTrace::failed()
    {
        if (!m_failed)
        {
            qWarning() << "Master operation trace incomplete:" << m_output.fileName() << m_output.errorString();
        }
        m_failed = true;
    }

    void MasterAssignmentSearchTrace::write(Event event, std::int64_t work, QJsonObject record)
    {
        if (m_finished)
        {
            failed();
        }
        if (m_failed)
        {
            return;
        }
        constexpr std::array names{"begin", "evaluated", "compared", "ticketStarted", "offered",
            "operationStarted", "executed", "serviced", "ticketFinished", "end", "guardedCompared"};
        record.insert("event", QLatin1String(names[static_cast<std::size_t>(event)]));
        record.insert("identity", m_identity);
        record.insert("sequence", QString::number(++m_sequence));
        record.insert("ticketSequence", m_ticketActive ? QJsonValue(QString::number(m_ticket))
                                                      : QJsonValue(QJsonValue::Null));
        record.insert(TRACE_WORK, QString::number(work));
        record.insert("elapsedNanos", QString::number(m_timer.nsecsElapsed()));
        const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
        if (m_output.write(bytes) != bytes.size() || !m_output.flush())
        {
            failed();
        }
    }

    void MasterAssignmentSearchTrace::evaluated(const MasterAssignmentEvaluation & evaluation, std::int64_t work)
    {
        write(Event::Evaluated, work, {{"evaluation", operationEvaluationRecord(evaluation)}});
    }

    void MasterAssignmentSearchTrace::refined(
        const MasterAssignmentEvaluation &, MasterRefinementTerm, std::int64_t, MasterRefinementStatus,
        std::int64_t, std::int64_t)
    {
    }

    void MasterAssignmentSearchTrace::compared(
        const MasterAssignmentEvaluation &, const RelativeStockComparison &, std::int64_t)
    {
    }

    void MasterAssignmentSearchTrace::comparisonObserved(
        const MasterAssignmentEvaluation & incoming, const MasterAssignmentEvaluation & challenger,
        const RelativeStockComparison & comparison, std::int64_t work)
    {
        write(Event::Compared, work, {{TRACE_INCOMING, operationEvaluationRecord(incoming)},
            {TRACE_CHALLENGER, operationEvaluationRecord(challenger)},
            {"comparison", QJsonObject{{"valid", comparison.valid()}, {"reason", static_cast<int>(comparison.reason)},
                {"deltaLower", QString::number(comparison.deltaLower)},
                {"deltaUpper", QString::number(comparison.deltaUpper)},
                {"economicDelta", QString::number(comparison.economicDelta)},
                {"ownedDelta", QString::number(comparison.ownedDelta)},
                {"commonEnemyCancelled", comparison.commonEnemyCancelled}}}});
    }

    QJsonObject masterGuardedProofRecord(const MasterGuardedStockProof & proof)
    {
        const auto & relation = proof.relation();
        const auto & roles = relation.roles;
        QJsonObject record{
            {"proofKind", QStringLiteral("guarded-owned-town-payment-v1")},
            {"semanticRevision", QStringLiteral("coupled-stock-native-terminal-v4")},
            {"statement", QStringLiteral("challengerValueGePaymentShiftOfIncomingValue")},
            {"minimumPaymentShift", QString::number(relation.minimumPaymentShift)},
            {"absoluteValuesKnown", false}, {"optimumTerminalClassesKnown", false},
            {"sameClassPolicyTransport", true},
            {"incomingChoices", masterChoicesRecord(proof.incoming().state().choices)},
            {"challengerChoices", masterChoicesRecord(proof.challenger().state().choices)},
            {"authority", QString::fromLatin1(proof.incoming().authority().toHex())},
            {"incomingConsequences", QString::fromLatin1(proof.incoming().consequences().toHex())},
            {"challengerConsequences", QString::fromLatin1(proof.challenger().consequences().toHex())},
            {"roles", QJsonObject{{"town", static_cast<int>(roles.town)},
                {"occupant", static_cast<int>(roles.occupant)}, {"headquarters", static_cast<int>(roles.headquarters)},
                {"guard", static_cast<int>(roles.guard)}}},
        };
        const auto & family = proof.incoming().currentFamily();
        record.insert("currentConsequences", family ? QStringLiteral("uniformSharedNativePrefix")
                                                  : QStringLiteral("actualRoot"));
        record.insert("anchorIsActualRoot", !family.has_value());
        if (family)
        {
            record.insert("sharedCurrentFamily", QJsonObject{
                {"nativeBefore", QString::fromLatin1(family->nativeBefore.toHex())},
                {"targetUnitId", family->targetUnitId}, {"lowerHp", family->lowerHp}, {"upperHp", family->upperHp},
                {"prefixActions", static_cast<int>(family->commonPrefix.size())},
                {"correspondence", QStringLiteral("pointwiseSameNativeRealization")},
                {"currentNatureObjectiveIntroduced", false}});
        }
        return record;
    }

    void MasterAssignmentSearchTrace::guardedCompared(
        const MasterAssignmentEvaluation & incoming, const MasterAssignmentEvaluation & challenger,
        const MasterGuardedComparison & comparison, std::int64_t work)
    {
        QJsonObject record{
            {TRACE_INCOMING, operationEvaluationRecord(incoming)},
            {TRACE_CHALLENGER, operationEvaluationRecord(challenger)},
            {TRACE_STATUS, static_cast<int>(comparison.status)}, {"improves", comparison.improves()},
        };
        if (comparison.proof)
        {
            record.insert("proof", masterGuardedProofRecord(*comparison.proof));
        }
        write(Event::GuardedCompared, work, std::move(record));
    }

    void MasterAssignmentSearchTrace::ticketStarted(
        const MasterRefinementTicketReceipt & ticket, const MasterAssignmentEvaluation & incoming,
        const MasterAssignmentEvaluation* challenger, std::int64_t work)
    {
        if (m_ticketActive)
        {
            failed();
        }
        m_ticketActive = true;
        ++m_ticket;
        auto record = ticketRecord(ticket);
        record.insert(TRACE_INCOMING, operationEvaluationRecord(incoming));
        record.insert(TRACE_CHALLENGER, operationEvaluationRecord(challenger));
        write(Event::TicketStarted, work, record);
    }

    void MasterAssignmentSearchTrace::operationOffered(
        const MasterAssignmentEvaluation & evaluation, const MasterOperationOffer & offer,
        bool lowerRelevant, bool upperRelevant, std::int64_t serviceBefore,
        std::int64_t workBefore, std::int64_t workAfter)
    {
        write(Event::Offered, workAfter, {{TRACE_OPERATION, operationEvaluationRecord(evaluation)},
            {"offer", operationOfferRecord(offer)}, {TRACE_LOWER_RELEVANT, lowerRelevant},
            {TRACE_UPPER_RELEVANT, upperRelevant}, {"serviceBefore", QString::number(serviceBefore)},
            {TRACE_WORK_BEFORE, QString::number(workBefore)}, {TRACE_WORK_AFTER, QString::number(workAfter)}});
    }

    void MasterAssignmentSearchTrace::operationStarted(
        const MasterAssignmentEvaluation & evaluation, MasterRefinementTerm term,
        std::int64_t allowance, std::int64_t work)
    {
        write(Event::OperationStarted, work, {{TRACE_OPERATION, operationEvaluationRecord(evaluation)},
            {TRACE_TERM, static_cast<int>(term)}, {TRACE_ALLOWANCE, QString::number(allowance)},
            {"ledger", masterWorkRecord(m_ledger.stats())}});
    }

    void MasterAssignmentSearchTrace::operationExecuted(
        const MasterAssignmentEvaluation & evaluation, MasterRefinementTerm term,
        const MasterOperationOutcome & outcome, std::int64_t workBefore, std::int64_t workAfter)
    {
        write(Event::Executed, workAfter, {{TRACE_OPERATION, operationEvaluationRecord(evaluation)},
            {TRACE_TERM, static_cast<int>(term)}, {TRACE_STATUS, static_cast<int>(outcome.status)},
            {TRACE_ADMITTED, outcome.admitted}, {TRACE_COMPLETED, outcome.completed},
            {"executionWork", QString::number(outcome.executionWork)},
            {"nativeWork", QString::number(outcome.nativeWork)}, {"nativeStates", QString::number(outcome.nativeStates)},
            {"nativeBefore", operationIntervalRecord(outcome.nativeBefore)},
            {"nativeAfter", operationIntervalRecord(outcome.nativeAfter)},
            {"lowerProgress", outcome.lowerProgress}, {"upperProgress", outcome.upperProgress},
            {"certificateProgress", outcome.certificateProgress},
            {TRACE_WORK_BEFORE, QString::number(workBefore)}, {TRACE_WORK_AFTER, QString::number(workAfter)},
            {"ledger", masterWorkRecord(m_ledger.stats())}});
    }

    void MasterAssignmentSearchTrace::operationServiced(const MasterOperationTransaction & transaction)
    {
        write(Event::Serviced, m_ledger.stats().spent(), {
            {TRACE_TICKET, operationEvaluationRecord(transaction.ticket)},
            {TRACE_CHALLENGER, operationEvaluationRecord(transaction.challenger)},
            {TRACE_OPERATION, operationEvaluationRecord(transaction.operation)},
            {TRACE_TERM, static_cast<int>(transaction.term)}, {TRACE_ALLOWANCE, QString::number(transaction.allowance)},
            {"ticketWork", QString::number(transaction.ticketWork)},
            {"operationWork", QString::number(transaction.operationWork)},
            {TRACE_ADMITTED, transaction.admitted}, {TRACE_COMPLETED, transaction.completed},
            {"relevantProgress", transaction.relevantProgress}, {"oppositeProgress", transaction.oppositeProgress}});
    }

    void MasterAssignmentSearchTrace::ticketFinished(const MasterRefinementTicketReceipt & ticket)
    {
        if (!m_ticketActive)
        {
            failed();
        }
        write(Event::TicketFinished, m_ledger.stats().spent(), ticketRecord(ticket));
        m_ticketActive = false;
    }

    void MasterAssignmentSearchTrace::finish(const QJsonObject & outcome)
    {
        if (m_ticketActive)
        {
            failed();
        }
        write(Event::End, m_ledger.stats().spent(), {{"outcome", outcome},
            {"eventCount", QString::number(m_sequence + 1)}, {"ticketCount", QString::number(m_ticket)}});
        m_finished = true;
        m_output.close();
    }

    bool MasterAssignmentSearchTrace::complete() const
    {
        return m_finished && !m_failed;
    }

    QString MasterAssignmentSearchTrace::path() const
    {
        return m_output.fileName();
    }

    std::unique_ptr<MasterAssignmentSearchTrace> attachMasterStockTrace(
        const MasterShadowConfig & config, const DecisionTraceIdentity & identity,
        const MasterAssignmentProblem & problem, const RefinementLedger & ledger,
        std::int64_t reservedWork, MasterAssignmentSearchOptions & options, const QString & attempt)
    {
        if (!config.controlsStock() || config.exportMode != MasterShadowExportMode::Full ||
            qgetenv(MASTER_OPERATION_TRACE_ENV) != "1")
        {
            return nullptr;
        }
        auto trace = std::make_unique<MasterAssignmentSearchTrace>(
            config.outputPath + QStringLiteral(".operations.jsonl"), identity, problem, ledger, reservedWork, attempt);
        options.observer = trace.get();
        return trace;
    }

    bool masterShadowProofsMatch(const MasterShadowObservation & observation)
    {
        if (observation.search == nullptr)
        {
            return true;
        }
        const auto & result = *observation.search;
        if ((result.incumbent && !liveEvaluation(observation, *result.incumbent)) ||
            (result.best && !liveEvaluation(observation, *result.best)))
        {
            return false;
        }
        if (!result.improvement)
        {
            return true;
        }
        const auto & proof = *result.improvement;
        if (!proof.incumbent.lowerReplayed || !proof.incumbent.upperVerified ||
            !proof.challenger.lowerReplayed || !proof.challenger.upperVerified ||
            !proof.incumbent.replay || !proof.challenger.replay ||
            !proof.incumbent.proof || !proof.challenger.proof ||
            !liveEvaluation(observation, proof.incumbent) || !liveEvaluation(observation, proof.challenger))
        {
            return false;
        }
        if (observation.evaluator == nullptr || proof.incumbent.quote()->economic != 0 ||
            proof.challenger.quote()->economic != 0)
        {
            return false;
        }
        const auto comparison = observation.evaluator->recordedComparison(
            proof.incumbent, proof.challenger, proof.incumbentOursCap, proof.challengerOursCap);
        return comparison.improves() && comparison.deltaLower == proof.comparison.deltaLower &&
            comparison.deltaUpper == proof.comparison.deltaUpper &&
            comparison.economicDelta == proof.comparison.economicDelta &&
            comparison.ownedDelta == proof.comparison.ownedDelta &&
            comparison.commonEnemyCancelled == proof.comparison.commonEnemyCancelled &&
            comparison.reason == proof.comparison.reason;
    }

    QJsonArray masterChoicesRecord(std::span<const std::int32_t> choices)
    {
        QJsonArray result;
        for (const auto value : choices)
        {
            result.append(value);
        }
        return result;
    }

    QJsonValue masterValueRecord(std::optional<MilliFunds> value)
    {
        return value ? QJsonValue(QString::number(*value)) : QJsonValue(QJsonValue::Null);
    }

    QJsonObject masterWorkRecord(const RefinementLedgerStats & work)
    {
        constexpr std::array<const char*, REFINEMENT_WORK_CLASSES> names{
            "mandatoryPrecompute", "ourAdmission", "enemyEntry", "searchSlice", "packingDualCertificate",
            "masterPreparation", "masterExpansion", "masterBoundScan", "masterDualOptimization",
            "masterDualVerification", "masterValuation", "masterWitness", "masterReplay", "masterInstallation",
        };
        QJsonArray net;
        QJsonObject categories;
        for (std::size_t category = 0; category < REFINEMENT_WORK_CLASSES; ++category)
        {
            const auto spent = work.grantedByWork[category] - work.refundedByWork[category];
            net.append(QString::number(spent));
            categories.insert(QLatin1String(names[category]), QJsonObject{
                {"spent", QString::number(spent)}, {"granted", QString::number(work.grantedByWork[category])},
                {"refunded", QString::number(work.refundedByWork[category])},
                {"refusals", QString::number(work.refusalsByWork[category])},
            });
        }
        return {{"allowance", QString::number(work.total)}, {"spent", QString::number(work.spent())},
            {"directConsumed", QString::number(work.consumed)},
            {"netSpentByWork", net}, {"workCategories", categories}, {"refusals", QString::number(work.refusals)},
            {"refundClamps", QString::number(work.refundClamps)}};
    }

    QJsonObject masterNativeSourceWorkRecord(const MasterNativeSourcePreparation & source)
    {
        QJsonValue refusal;
        if (source.callbackRefusal)
        {
            const auto & observed = *source.callbackRefusal;
            QJsonObject detail{{"module", static_cast<int>(observed.module)}, {"status", static_cast<int>(observed.status)},
                {"lastCheckedMember", observed.lastCheckedMember}};
            if (observed.lastCheckedStatus)
            {
                detail.insert("lastCheckedStatus", static_cast<int>(*observed.lastCheckedStatus));
            }
            refusal = detail;
        }
        return {
            {"nativeCallbackRefusal", refusal},
            {"nativeSourceWork", QString::number(source.work)},
            {"nativeCollectionWork", QString::number(source.collectionWork)},
            {"nativeExpectedConstructionWork", QString::number(source.expectedConstructionWork)},
            {"nativeReceiverConstructionWork", QString::number(source.receiverConstructionWork)},
            {"nativeCommonCollectionWork", QString::number(source.commonCollectionWork)},
            {"nativePreparationMaximumWork", masterValueRecord(source.preparationMaximumWork)},
            {"nativeAuthenticationMaximumWork", masterValueRecord(source.authenticationMaximumWork)},
            {"nativeCommandAuthenticationAllowance", QString::number(source.commandAuthenticationAllowance)},
            {"nativeBoundModules", static_cast<int>(source.boundModules)},
            {"nativeCollectionCosts", masterNativeCollectionCostsRecord(source)}};
    }

    QJsonArray masterNativeCollectionCostsRecord(const MasterNativeSourcePreparation & source)
    {
        QJsonArray records;
        for (std::size_t index = 0; index < source.collectionCosts.size(); ++index)
        {
            const auto & cost = source.collectionCosts[index];
            records.append(QJsonObject{
                {"stage", static_cast<int>(index)},
                {"observations", QString::number(cost.observations)},
                {TRACE_WORK, QString::number(cost.work)},
                {"expectedConstructionWork", QString::number(cost.expectedConstructionWork)},
                {"receiverConstructionWork", QString::number(cost.receiverConstructionWork)}});
        }
        return records;
    }

    QJsonObject masterRefusalRecord(const std::optional<RefinementRefusal> & refusal)
    {
        if (!refusal)
        {
            return {};
        }
        return {{TRACE_WORK, static_cast<int>(refusal->work)}, {"reason", static_cast<int>(refusal->reason)},
            {"requested", QString::number(refusal->requested)}, {"balance", QString::number(refusal->balance)},
            {"refundable", refusal->refundable}};
    }

    QJsonObject masterRefinementRefusalRecord(const std::optional<MasterRefinementRefusal> & refusal)
    {
        if (!refusal)
        {
            return {};
        }
        return {{"term", static_cast<int>(refusal->term)}, {"requested", QString::number(refusal->requested)},
            {"balance", QString::number(refusal->balance)}, {"allowance", QString::number(refusal->allowance)},
            {"preparation", QString::number(refusal->preparation)}};
    }

    QJsonObject masterCertificatePreparationRecord(
        const std::optional<RelativeStockCertificatePreparation> & preparation)
    {
        if (!preparation)
        {
            return {};
        }
        return {{"status", static_cast<int>(preparation->status)},
            {"readinessWork", QString::number(preparation->readinessWork)},
            {"scanWork", QString::number(preparation->scanWork)}};
    }

    QJsonObject masterSearchRecord(const MasterAssignmentSearchResult & result)
    {
        QJsonArray residual;
        for (const auto & region : result.residual)
        {
            residual.append(QJsonObject{{"choices", masterChoicesRecord(region.state.choices)},
                {"upper", masterValueRecord(region.upper)}, {"complete", region.completeTuple},
                {"unsupported", region.unsupported}});
        }
        QJsonArray prunes;
        for (const auto & prune : result.prunes)
        {
            prunes.append(QJsonObject{{"choices", masterChoicesRecord(prune.state.choices)},
                {"upper", QString::number(prune.upper)}, {"feasibleLower", QString::number(prune.feasibleLower)}});
        }
        QJsonObject improvement;
        if (result.improvement)
        {
            const auto & proof = *result.improvement;
            improvement = {{"choices", masterChoicesRecord(proof.challenger.state.choices)},
                {TRACE_WORK, QString::number(proof.work)}};
            if (proof.guardedProof)
            {
                improvement.insert("proof", masterGuardedProofRecord(*proof.guardedProof));
            }
            else
            {
                improvement.insert("deltaLower", QString::number(proof.comparison.deltaLower));
                improvement.insert("deltaUpper", QString::number(proof.comparison.deltaUpper));
                improvement.insert("commonEnemy", proof.comparison.commonEnemyCancelled);
            }
            if (proof.incumbentOursCap)
            {
                improvement.insert(TRACE_INCUMBENT_OURS_CAP, QString::number(*proof.incumbentOursCap));
            }
            if (proof.challengerOursCap)
            {
                improvement.insert(TRACE_CHALLENGER_OURS_CAP, QString::number(*proof.challengerOursCap));
            }
        }
        QJsonObject record{
            {"schedule", QJsonObject{{"allocation", static_cast<int>(result.schedule.allocation)},
                {"currentDualRounds", result.schedule.currentDualRounds},
                {"continuationSlice", QString::number(result.schedule.continuationSlice)},
                {"refinementGrowthFactor", QString::number(result.schedule.refinementGrowthFactor)},
                {"futureDualAllowance", QString::number(result.schedule.futureDualAllowance)},
                {"futureBounds", static_cast<int>(result.schedule.futureBounds)},
                {"incumbentCompletions", result.schedule.incumbentCompletions},
                {"initialIncumbentAlternatives", result.schedule.initialIncumbentAlternatives},
                {"initialContributionCompletion", result.schedule.initialContributionCompletion},
                {"incumbentRefinementStride", QString::number(result.schedule.incumbentRefinementStride)},
                {"challengerRefinementBurst", QString::number(result.schedule.challengerRefinementBurst)},
                {"nodesPerRefinement", QString::number(result.schedule.nodesPerRefinement)}}},
            {"lower", masterValueRecord(result.lower)}, {"upper", masterValueRecord(result.upper)},
            {"residualUpper", masterValueRecord(result.residualUpper)},
            {"gap", masterValueRecord(result.gap)}, {"globalOptimal", result.globalOptimal},
            {"budgetExhausted", result.budgetExhausted}, {"domainAccounted", result.domainAccounted},
            {"fullDomain", result.fullDomain}, {"noFeasibleAssignment", result.noFeasibleAssignment},
            {"invalidInput", result.invalidInput}, {"implicitRoot", result.implicitRoot},
            {"residualRegions", QString::number(result.stats.residualRegions)},
            {"peakQueuedRegions", QString::number(result.stats.peakFrontier)},
            {"peakResidentRegions", QString::number(result.stats.peakResidentRegions)},
            {"incumbentPreserved", result.incumbentPreserved}, {"canonicalRepresentative", result.canonicalRepresentative},
            {"bestChoices", result.best ? masterChoicesRecord(result.best->state.choices) : QJsonArray{}},
            {"expanded", QString::number(result.stats.expanded)}, {"evaluated", QString::number(result.stats.evaluated)},
            {"incumbentCompletions", QString::number(result.stats.incumbentCompletions)},
            {"initialIncumbentAlternatives", QString::number(result.stats.initialIncumbentAlternatives)},
            {"contributionCompletions", QString::number(result.stats.contributionCompletions)},
            {"incumbentRefinements", QString::number(result.stats.incumbentRefinements)},
            {"cancelledEnemyRefinements", QString::number(result.stats.cancelledEnemyRefinements)},
            {"workBalancedSelections", QString::number(result.stats.workBalancedSelections)},
            {"evaluationCacheHits", QString::number(result.stats.evaluationCacheHits)},
            {"infeasible", QString::number(result.stats.infeasible)}, {"pruned", QString::number(result.stats.pruned)},
            {"guardedExcluded", QString::number(result.stats.guardedExcluded)},
            {"unsupported", QString::number(result.stats.unsupported)},
            {"arithmeticFailures", QString::number(result.stats.arithmeticFailures)},
            {"proofFailures", QString::number(result.stats.proofFailures)},
            {"firstImprovementWork", masterValueRecord(result.stats.firstImprovementWork)},
            {"firstImprovementLower", masterValueRecord(result.stats.firstImprovementLower)},
            {"futurePricingCache", QJsonObject{{"hits", QString::number(result.stats.futurePricingCache.hits)},
                {"recomputations", QString::number(result.stats.futurePricingCache.recomputations)}}},
            {"retainedGrants", QString::number(result.stats.retainedGrants)},
            {"refundableStorageBytes", QString::number(result.stats.refundableStorageBytes)},
            {"refinements", QString::number(result.stats.refinements)}, {"residual", residual},
            {"prunes", prunes}, {"improvement", improvement}};
        if (result.schedule.allocation == MasterRefinementAllocation::OperationService)
        {
            QJsonObject operation{
                {"policy", static_cast<int>(result.schedule.allocation)},
                {"records", QString::number(result.stats.operationRecords)},
                {"resolvedTickets", QString::number(result.stats.resolvedOperationTickets)},
                {"fundingParkedTickets", QString::number(result.stats.fundingParkedTickets)},
                {"paidWork", QString::number(result.stats.operationPaidWork)},
                {"screeningWork", QString::number(result.stats.operationScreeningWork)}};
            if (result.operationStop)
            {
                operation.insert("stop", static_cast<int>(*result.operationStop));
            }
            record.insert("operationService", operation);
        }
        if (result.schedule.planPricingRounds > 0)
        {
            const auto & pricing = result.stats.planPricing;
            QJsonObject planPricing{{TRACE_ROUNDS, result.schedule.planPricingRounds},
                {TRACE_WORK, QString::number(result.schedule.planPricingWork)},
                {"witnessWork", QString::number(result.schedule.planPricingWitnessWork)},
                {"outcome", static_cast<int>(pricing.outcome)},
                {"adapterWork", QString::number(pricing.adapterWork)},
                {"restrictionWork", QString::number(pricing.restrictionWork)},
                {"challengerRounds", pricing.challengerRounds},
                {"challengerWork", QString::number(pricing.challengerWork)},
                {"incumbentRounds", pricing.incumbentRounds},
                {"incumbentWork", QString::number(pricing.incumbentWork)},
                {"adoptedValue", masterValueRecord(pricing.adoptedValue)}, {"hintStored", pricing.hintStored},
                {"firstComparisonImproved", pricing.firstComparisonImproved}};
            if (pricing.boundCap)
            {
                planPricing.insert(TRACE_BOUND_CAP, planBoundCapRecord(*pricing.boundCap));
            }
            if (pricing.neighborAudit)
            {
                planPricing.insert(TRACE_NEIGHBOR_AUDIT, planNeighborAuditRecord(*pricing.neighborAudit));
            }
            record.insert(TRACE_PLAN_PRICING, planPricing);
        }
        const auto work = masterWorkRecord(result.stats.work);
        for (auto entry = work.begin(); entry != work.end(); ++entry)
        {
            record.insert(entry.key(), entry.value());
        }
        return record;
    }

    QJsonObject masterModelRecord(const MasterAssignmentProblem & problem)
    {
        QJsonArray actors;
        std::int64_t mutableCount = 0;
        std::int64_t candidateCount = 0;
        for (const auto & actor : problem.actors())
        {
            const bool changeable = problem.canChangeActor(actor.canonicalSlot);
            mutableCount += changeable ? 1 : 0;
            QJsonArray candidates;
            for (const auto & candidate : actor.candidates)
            {
                QJsonArray resources;
                for (const auto & use : candidate.resources)
                {
                    resources.append(QJsonArray{use.resourceIndex, use.demand, use.capacity});
                }
                candidates.append(QJsonObject{
                    {"index", candidate.identity.originalCandidateIndex},
                    {"generation", candidate.identity.generationIndex}, {"kind", static_cast<int>(candidate.actionKind)},
                    {"origin", tile(candidate.origin)}, {"destination", tile(candidate.destination)},
                    {"target", tile(candidate.target)}, {"targetUnit", candidate.targetUnitId},
                    {"stockEndpoint", tile(candidate.stockEndpoint)}, {"capture", candidate.capture.has_value()},
                    {"economic", QString::number(candidate.immediateValue.economicValue)},
                    {"terminal", static_cast<int>(candidate.immediateValue.terminal)},
                    {"plannable", candidate.plannable}, {"resources", resources}});
            }
            candidateCount += static_cast<std::int64_t>(actor.candidates.size());
            actors.append(QJsonObject{
                {"actor", actor.engineUnitId}, {"knowledgeIndex", actor.knowledgeUnitIndex},
                {TRACE_SLOT, actor.canonicalSlot}, {"role", static_cast<int>(actor.role)}, {"mutable", changeable},
                {"stockRow", actor.stockRow}, {"incumbent", actor.incumbentCandidateIndex},
                {"canonicalCandidates", masterChoicesRecord(actor.canonicalCandidates)}, {"candidates", candidates}});
        }
        return {{"actors", actors}, {"actorCount", static_cast<qint64>(problem.actors().size())},
            {"mutableActors", static_cast<qint64>(mutableCount)}, {"candidates", static_cast<qint64>(candidateCount)},
            {"currentResources", static_cast<qint64>(problem.resources().size())},
            {"incumbentChoices", masterChoicesRecord(problem.incumbentChoices())}};
    }

    QJsonObject masterEvaluationRecord(const MasterAssignmentEvaluation & evaluation)
    {
        QJsonArray actions;
        if (evaluation.replay)
        {
            const auto & plan = evaluation.replay->plan;
            for (std::int32_t index = 0; index < plan.actionCount(); ++index)
            {
                const auto & action = plan.action(index);
                QJsonArray path;
                for (const auto & point : action.path)
                {
                    path.append(tile(point));
                }
                actions.append(QJsonObject{
                    {"index", action.actionIndex}, {"actor", action.unitId},
                    {"actionId", action.actionId}, {"kind", static_cast<int>(action.kind)},
                    {"destination", tile(action.destination)}, {"target", tile(action.target)},
                    {"targetUnit", action.targetUnitId}, {"path", path}, {"state", static_cast<int>(action.state)},
                    {"damage", action.plannedDamageSteps}, {"claims", masterClaimsRecord(plan.actionReservations(index))}});
            }
        }
        QJsonObject record{
            {"choices", masterChoicesRecord(evaluation.state.choices)}, {"status", static_cast<int>(evaluation.status)},
            {"objectiveStatus", static_cast<int>(evaluation.objectiveStatus)}, {"actions", actions},
            {"seatedEconomic", masterValueRecord(evaluation.replay
                ? std::optional<MilliFunds>{evaluation.replay->seatedEconomicTotal} : std::nullopt)},
            {"lowerReplayed", evaluation.lowerReplayed}, {"upperVerified", evaluation.upperVerified},
            {"exact", evaluation.exact}, {"lower", masterValueRecord(evaluation.objective
                ? std::optional<MilliFunds>{evaluation.objective->lower} : std::nullopt)},
            {"upper", masterValueRecord(evaluation.objective
                ? std::optional<MilliFunds>{evaluation.objective->upper} : std::nullopt)}};
        if (evaluation.replay)
        {
            QJsonArray edges;
            for (std::int32_t index = 0; index < evaluation.replay->plan.edgeCount(); ++index)
            {
                const auto & edge = evaluation.replay->plan.edge(index);
                edges.append(QJsonArray{edge.from, edge.to, static_cast<int>(edge.kind)});
            }
            record.insert("edges", edges);
            record.insert("executionOrder", masterChoicesRecord(evaluation.replay->ordering.order));
            record.insert("actionIndices", masterChoicesRecord(evaluation.replay->actionIndices));
        }
        if (const auto* quote = evaluation.quote())
        {
            const auto & proof = *evaluation.proof;
            QJsonArray cheapWitness;
            for (const auto & row : proof.cheapOurWitness)
            {
                cheapWitness.append(QJsonObject{{"row", row.row}, {"nodes", masterChoicesRecord(row.nodes)},
                    {"value", QString::number(row.value)}});
            }
            QJsonObject quoted{
                {"key", keyRecord(quote->key)}, {"capturedColumns", masterChoicesRecord(quote->capturedColumns)},
                {"economic", QString::number(quote->economic)}, {"owned", QString::number(quote->owned)},
                {"ours", QJsonArray{QString::number(quote->ours.lower), QString::number(quote->ours.upper)}},
                {"enemy", QJsonArray{QString::number(quote->enemy.lower), QString::number(quote->enemy.upper)}},
                {"ourSearchCompleted", quote->ourSearchCompleted}, {"enemySearchCompleted", quote->enemySearchCompleted},
                {"ourUpperCertificateId", QString::number(quote->ourUpperCertificateId)},
                {"ourWitnessKind", static_cast<int>(proof.ourKind)},
                {"cheapOurWitness", cheapWitness},
                {"ourWitness", sequentialWitnessRecord(proof.ourWitness)},
                {"enemyWitness", sequentialWitnessRecord(proof.enemyWitness)}};
            if (proof.ourUpperCertificate)
            {
                quoted.insert("ownedUpperCertificatePayload", packingCertificateRecord(*proof.ourUpperCertificate));
            }
            record.insert("quote", quoted);
        }
        return record;
    }

    QJsonObject masterClaimsRecord(const ActionReservations & claims)
    {
        QJsonArray destinations;
        QJsonArray captures;
        QJsonArray damage;
        for (const auto point : claims.destinations)
        {
            destinations.append(tile(point));
        }
        for (const auto point : claims.captures)
        {
            captures.append(tile(point));
        }
        for (const auto & share : claims.damage)
        {
            damage.append(QJsonArray{share.targetUnitId, share.targetHpSteps, share.damageSteps});
        }
        return {{"destinations", destinations}, {"captures", captures},
                {"services", masterChoicesRecord(claims.services)}, {"damage", damage}};
    }

    QJsonObject masterProfileRecord(const MasterAssignmentProfile & profile)
    {
        QJsonObject result;
        for (std::size_t phase = 0; phase < profile.samples.size(); ++phase)
        {
            const auto & sample = profile.samples[phase];
            result.insert(QLatin1String(MASTER_PHASE_NAMES[phase]), QJsonObject{
                {"calls", QString::number(sample.calls)}, {"inclusiveNanos", QString::number(sample.inclusiveNanos)},
                {"exclusiveNanos", QString::number(sample.exclusiveNanos)}});
        }
        return result;
    }
}
