#include "tests/arena/relativestockprobe.h"

#include <cmath>

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/propertystockbuilder.h"
#include "tests/arena/relativestockreceipts.h"
#include "tests/arena/relativestocksupportaudit.h"

namespace
{
constexpr qint64 MAX_FRONTIER_RECEIPTS = 100000;

using ArenaStockReceipts::indices;
using ArenaStockReceipts::readTuple;
using ArenaStockReceipts::quoteReceipt;
using ArenaStockReceipts::comparisonReceipt;

QVariantMap statsReceipt(const Coordinator::RelativeStockStats & stats)
{
    return {
        {"candidateProduct", static_cast<qint64>(stats.candidateProduct)},
        {"enumerationStates", static_cast<qint64>(stats.enumerationStates)},
        {"claimableLeaves", static_cast<qint64>(stats.claimableLeaves)},
        {"statesUsed", static_cast<qint64>(stats.statesUsed)}, {"enemyStatesUsed", static_cast<qint64>(stats.enemyStatesUsed)},
        {"workUsed", static_cast<qint64>(stats.workUsed)}, {"domains", stats.domains}, {"skippedDomains", stats.skippedDomains},
        {"comparisons", stats.comparisons}, {"improvements", stats.improvements}, {"unresolved", stats.unresolved},
        {"budgetExhaustions", stats.budgetExhaustions},
    };
}

QVariantMap mixedStatsReceipt(const Coordinator::LiveRefinementStats & stats)
{
    return {
        {"domains", stats.domains}, {"calls", stats.calls}, {"keysRefined", stats.keysRefined},
        {"statesUsed", static_cast<qint64>(stats.statesUsed)}, {"enemyStatesUsed", static_cast<qint64>(stats.enemyStatesUsed)},
        {"workUsed", static_cast<qint64>(stats.workUsed)},
        {"budgetExhaustions", stats.budgetExhaustions}, {"failures", stats.failures},
    };
}

QVariantList sequentialRowPhaseReceipts(
    std::span<const Coordinator::SequentialRowPhaseAudit> phases)
{
    QVariantList rows;
    for (const auto & row : phases)
    {
        rows.push_back(QVariantMap{
            {"row", row.row},
            {"cacheLookup", row.cacheLookup},
            {"cacheHit", row.cacheHit},
            {"cacheQuotedStates", static_cast<qint64>(row.cacheQuotedStates)},
            {"cacheChargedStates", static_cast<qint64>(row.cacheChargedStates)},
            {"constructionStates", static_cast<qint64>(row.constructionStates)},
            {"uniqueOptions", static_cast<qint64>(row.uniqueOptions)},
            {"complete", row.complete},
        });
    }
    return rows;
}

QVariantList sequentialNodeReceipts(
    std::span<const Coordinator::PropertyStockColumn> columns)
{
    QVariantList nodes;
    for (std::size_t node = 0; node < columns.size(); ++node)
    {
        const auto & column = columns[node];
        nodes.push_back(QVariantMap{
            {"node", static_cast<qint64>(node)},
            {"slot", column.slot},
            {"x", column.tile.x},
            {"y", column.tile.y},
        });
    }
    return nodes;
}

QVariantList sequentialWitnessReceipts(
    std::span<const Coordinator::SequentialRowWitness> witness)
{
    QVariantList rows;
    for (const auto & row : witness)
    {
        std::vector<std::int32_t> sortedNodes;
        QVariantList steps;
        for (const auto & step : row.steps)
        {
            sortedNodes.push_back(step.node);
            steps.push_back(QVariantMap{
                {"node", step.node},
                {"turn", step.turn},
                {"gain", static_cast<qint64>(step.gain)},
            });
        }
        std::sort(sortedNodes.begin(), sortedNodes.end());
        QVariantList nodes;
        for (const auto node : sortedNodes)
        {
            nodes.push_back(node);
        }
        rows.push_back(QVariantMap{
            {"row", row.row},
            {"nodes", nodes},
            {"steps", steps},
            {"value", static_cast<qint64>(row.value)},
        });
    }
    return rows;
}

QVariantList sequentialCatalogReceipts(
    std::span<const Coordinator::SequentialRowCatalogAudit> rows)
{
    QVariantList result;
    for (const auto & row : rows)
    {
        QVariantList options;
        for (std::size_t index = 0;
             index < row.options.size();
             ++index)
        {
            const auto & option = row.options[index];
            QVariantList nodes;
            QVariantList steps;
            for (const auto node : option.nodes)
            {
                nodes.push_back(node);
            }
            for (const auto & step : option.steps)
            {
                steps.push_back(QVariantMap{
                    {"node", step.node},
                    {"turn", step.turn},
                });
            }
            options.push_back(QVariantMap{
                {"index", static_cast<qint64>(index)},
                {"nodes", nodes},
                {"steps", steps},
                {"value", static_cast<qint64>(option.value)},
            });
        }
        result.push_back(QVariantMap{
            {"row", row.row},
            {"options", options},
        });
    }
    return result;
}

QVariantMap pairPartitionReceipt(
    const Coordinator::SequentialPairPartitionResult & certificate)
{
    QVariantList singles;
    for (const auto option : certificate.singleOptionIndices)
    {
        singles.push_back(option);
    }
    QVariantList losses;
    for (const auto & loss : certificate.losses)
    {
        losses.push_back(QVariantMap{
            {"firstRow", loss.firstRow},
            {"secondRow", loss.secondRow},
            {"firstOptionIndex",
             loss.firstOptionIndex},
            {"secondOptionIndex",
             loss.secondOptionIndex},
            {"firstSingle",
             static_cast<qint64>(loss.firstSingle)},
            {"secondSingle",
             static_cast<qint64>(loss.secondSingle)},
            {"pairOptimum",
             static_cast<qint64>(loss.pairOptimum)},
            {"loss", static_cast<qint64>(loss.loss)},
        });
    }
    QVariantList matching;
    for (const auto & [first, second] :
         certificate.matching)
    {
        matching.push_back(
            QVariantList{first, second});
    }
    const auto usedNodes = certificate.usedNodes.nodes();
    return {
        {"currentUpper",
         static_cast<qint64>(certificate.currentUpper)},
        {"rowSumUpper",
         static_cast<qint64>(certificate.rowSumUpper)},
        {"pairPartitionUpper",
         static_cast<qint64>(
             certificate.pairPartitionUpper)},
        {"certifiedUpper",
         static_cast<qint64>(
             certificate.certifiedUpper)},
        {"matchingLoss",
         static_cast<qint64>(certificate.matchingLoss)},
        {"usedNodes",
         indices(usedNodes)},
        {"singleRows", indices(certificate.singleRows)},
        {"singleOptionIndices", singles},
        {"losses", losses},
        {"matching", matching},
        {"singleCompatibilityChecks",
         static_cast<qint64>(
             certificate.singleCompatibilityChecks)},
        {"pairCompatibilityChecks",
         static_cast<qint64>(
             certificate.pairCompatibilityChecks)},
        {"matchingTransitions",
         static_cast<qint64>(
             certificate.matchingTransitions)},
        {"deterministicOperationCost",
         static_cast<qint64>(
             certificate.deterministicOperationCost)},
        {"checkedArithmetic",
         certificate.checkedArithmetic},
        {"completed", certificate.completed},
        {"valid", certificate.valid},
    };
}

QVariantMap nodePriceDualReceipt(
    const Coordinator::SequentialNodePriceDualResult & certificate)
{
    QVariantList prices;
    for (const auto price : certificate.nodePrices)
    {
        prices.push_back(static_cast<qint64>(price));
    }
    QVariantList rowMaxima;
    for (const auto maximum : certificate.rowReducedMaxima)
    {
        rowMaxima.push_back(static_cast<qint64>(maximum));
    }
    QVariantList options;
    for (const auto option : certificate.rowOptionIndices)
    {
        options.push_back(option);
    }
    return {
        {"currentUpper",
         static_cast<qint64>(certificate.currentUpper)},
        {"initialDualUpper",
         static_cast<qint64>(certificate.initialDualUpper)},
        {"finalDualUpper",
         static_cast<qint64>(certificate.finalDualUpper)},
        {"certifiedUpper",
         static_cast<qint64>(certificate.certifiedUpper)},
        {"priceStep",
         static_cast<qint64>(certificate.priceStep)},
        {"totalPriceSum",
         static_cast<qint64>(certificate.totalPriceSum)},
        {"usedNodes", indices(certificate.usedNodes.nodes())},
        {"rows", indices(certificate.rows)},
        {"rowOptionIndices", options},
        {"nodePrices", prices},
        {"rowReducedMaxima", rowMaxima},
        {"dualIterations", certificate.dualIterations},
        {"priceUpdates",
         static_cast<qint64>(certificate.priceUpdates)},
        {"optimizerOptionRecordsScanned",
         static_cast<qint64>(
             certificate.optimizerOptionRecordsScanned)},
        {"optimizerOptionIncidencesScanned",
         static_cast<qint64>(
             certificate.optimizerOptionIncidencesScanned)},
        {"priceCoordinateWork",
         static_cast<qint64>(
             certificate.priceCoordinateWork)},
        {"averagingWork",
         static_cast<qint64>(
             certificate.averagingWork)},
        {"verificationOptionRecordsScanned",
         static_cast<qint64>(
             certificate.verificationOptionRecordsScanned)},
        {"verificationOptionIncidencesScanned",
         static_cast<qint64>(
             certificate.verificationOptionIncidencesScanned)},
        {"optimizerWork",
         static_cast<qint64>(certificate.optimizerWork)},
        {"verificationWork",
         static_cast<qint64>(certificate.verificationWork)},
        {"deterministicOperationCost",
         static_cast<qint64>(
             certificate.deterministicOperationCost)},
        {"checkedArithmetic", certificate.checkedArithmetic},
        {"optimizerCompleted", certificate.optimizerCompleted},
        {"verificationCompleted",
         certificate.verificationCompleted},
        {"dualCertificateValid",
         certificate.dualCertificateValid},
        {"completed", certificate.completed},
        {"valid", certificate.valid},
    };
}

QVariantList residualNodeReceipts(
    std::span<
        const Coordinator::SequentialThresholdOpenNodeAudit>
        nodes)
{
    QVariantList result;
    for (const auto & node : nodes)
    {
        QVariantList optimistic;
        for (const auto & option :
             node.optimisticOptions)
        {
            QVariantList optionNodes;
            for (const auto property : option.nodes)
            {
                optionNodes.push_back(property);
            }
            optimistic.push_back(QVariantMap{
                {"row", option.row},
                {"optionIndex", option.optionIndex},
                {"value",
                 static_cast<qint64>(option.value)},
                {"nodes", optionNodes},
            });
        }
        result.push_back(QVariantMap{
            {"depth", static_cast<qint64>(node.depth)},
            {"fixedValue",
             static_cast<qint64>(node.fixedValue)},
            {"upper", static_cast<qint64>(node.upper)},
            {"rowSumUpper",
             static_cast<qint64>(node.rowSumUpper)},
            {"usedNodes", indices(node.usedNodes)},
            {"remainingRows", indices(node.remainingRows)},
            {"branches", indices(node.branches)},
            {"optimisticOptions", optimistic},
            {"overlappingNodes",
             indices(node.overlappingNodes)},
            {"overlapCount", node.overlapCount},
            {"optimisticOptionsCompatible",
             node.optimisticOptionsCompatible},
        });
    }
    return result;
}

QVariantMap sequentialPhaseReceipt(
    const Coordinator::SequentialTierResult & tier)
{
    return {
        {"relaxedBoundConstructionStates", 0},
        {"rowCacheLookups", static_cast<qint64>(tier.rowCacheLookups)},
        {"rowCacheQuotedStates", static_cast<qint64>(tier.rowCacheQuotedStates)},
        {"rowCacheChargedStates", static_cast<qint64>(tier.rowCacheChargedStates)},
        {"rowConstructionStates", static_cast<qint64>(tier.rowConstructionStates)},
        {"rowOptionDomainsComplete", tier.rowOptionDomainsComplete},
        {"totalUniqueRowOptions", static_cast<qint64>(tier.totalUniqueRowOptions)},
        {"packingAttempted", tier.packingAttempted},
        {"packingStates", static_cast<qint64>(tier.packingStates)},
        {"packingLeaves", static_cast<qint64>(tier.packingLeaves)},
        {"packingPrunes", static_cast<qint64>(tier.packingPrunes)},
        {"packingIncumbentImprovements", static_cast<qint64>(tier.packingIncumbentImprovements)},
        {"unexploredPackingBranches", static_cast<qint64>(tier.unexploredPackingBranches)},
        {"maximumUnexploredPackingBound", static_cast<qint64>(tier.maximumUnexploredPackingBound)},
        {"packingResidualUpperValid", tier.packingResidualUpperValid},
        {"rows", sequentialRowPhaseReceipts(tier.rowPhases)},
        {"nodes", sequentialNodeReceipts(tier.nodeColumns)},
        {"witness", sequentialWitnessReceipts(tier.witness)},
        {"catalog", sequentialCatalogReceipts(tier.rowCatalogs)},
    };
}

QVariantMap thresholdProofReceipt(
    const Coordinator::DiagnosticSequentialThresholdProof & diagnostic)
{
    const auto & proof = diagnostic.proof;
    return {
        {"valid", diagnostic.valid},
        {"status", static_cast<int>(proof.status)},
        {"stop", static_cast<int>(proof.stop)},
        {"threshold", static_cast<qint64>(proof.threshold)},
        {"feasibleLower",
         static_cast<qint64>(proof.feasibleLower)},
        {"residualUpper",
         static_cast<qint64>(proof.residualUpper)},
        {"originalRelaxedUpper",
         static_cast<qint64>(proof.originalRelaxedUpper)},
        {"states", static_cast<qint64>(proof.statesUsed)},
        {"proofWork", static_cast<qint64>(proof.workUsed)},
        {"tariff", static_cast<qint64>(diagnostic.tariff)},
        {"stateCap", static_cast<qint64>(diagnostic.stateCap)},
        {"totalWork", static_cast<qint64>(diagnostic.workUsed)},
        {"rowConstructionStates",
         static_cast<qint64>(proof.rowConstructionStates)},
        {"cacheQuotedStates",
         static_cast<qint64>(proof.cacheQuotedStates)},
        {"cacheChargedStates",
         static_cast<qint64>(proof.cacheChargedStates)},
        {"packingStates",
         static_cast<qint64>(proof.packingStates)},
        {"packingLeaves",
         static_cast<qint64>(proof.packingLeaves)},
        {"packingPrunes",
         static_cast<qint64>(proof.packingPrunes)},
        {"openNodesAtStop",
         static_cast<qint64>(proof.openNodesAtStop)},
        {"packingCatalogWork",
         static_cast<qint64>(proof.packingCatalogWork)},
        {"packingCatalogOptionRecords",
         static_cast<qint64>(
             proof.packingCatalogOptionRecords)},
        {"packingCatalogNodeIncidences",
         static_cast<qint64>(
             proof.packingCatalogNodeIncidences)},
        {"pairBoundWork",
         static_cast<qint64>(proof.pairBoundWork)},
        {"pairBoundCalls",
         static_cast<qint64>(proof.pairBoundCalls)},
        {"pairBoundCompleted",
         static_cast<qint64>(proof.pairBoundCompleted)},
        {"pairBoundTightenings",
         static_cast<qint64>(proof.pairBoundTightenings)},
        {"pairBoundPrunes",
         static_cast<qint64>(proof.pairBoundPrunes)},
        {"dualWork",
         static_cast<qint64>(proof.dualWork)},
        {"dualCalls",
         static_cast<qint64>(proof.dualCalls)},
        {"dualCompleted",
         static_cast<qint64>(proof.dualCompleted)},
        {"dualTightenings",
         static_cast<qint64>(proof.dualTightenings)},
        {"rootPairUpper",
         static_cast<qint64>(proof.rootPairUpper)},
        {"rootDualUpper",
         static_cast<qint64>(proof.rootDualUpper)},
        {"expansionOrderHash",
         QString::number(proof.expansionOrderHash, 16)},
        {"rowOptionDomainsComplete",
         proof.rowOptionDomainsComplete},
        {"searchCompleted", proof.searchCompleted},
        {"thresholdProven", proof.thresholdProven},
        {"packingCatalogComplete",
         proof.packingCatalogComplete},
        {"witnessReplays",
         proof.witnessReplays &&
             diagnostic.witnessReplays},
        {"rows",
         sequentialRowPhaseReceipts(proof.rowPhases)},
        {"nodes", sequentialNodeReceipts(proof.nodeColumns)},
        {"witness", sequentialWitnessReceipts(proof.witness)},
        {"catalog", sequentialCatalogReceipts(proof.rowCatalogs)},
        {"rootPairCertificate",
         pairPartitionReceipt(
             proof.rootPairCertificate)},
        {"rootDualCertificate",
         nodePriceDualReceipt(
             proof.rootDualCertificate)},
        {"residualNodes",
         residualNodeReceipts(
             proof.residualNodes)},
    };
}

struct ValuationPlan
{
    Coordinator::TurnPlan plan;
    Coordinator::MilliFunds economic{0};
    QVariantList actions;
    bool valid{false};
};

ValuationPlan valuationPlan(const Coordinator::AssignmentInput & input, std::span<const std::int32_t> tuple)
{
    ValuationPlan result;
    if (tuple.size() != input.actors.size())
    {
        return result;
    }
    for (std::size_t slot = 0; slot < tuple.size(); ++slot)
    {
        const auto & actor = input.actors[slot];
        const auto candidate = tuple[slot];
        if (candidate < 0 || static_cast<std::size_t>(candidate) >= actor.candidates.size() ||
            !Coordinator::isPlannableCandidate(input.actionIds, input.unitLinks, actor.candidates[candidate]))
        {
            return result;
        }
        Coordinator::ActorProgress seat;
        seat.pActor = &actor;
        const auto outcome = Coordinator::seatOption(result.plan, input, seat, candidate);
        if (outcome.claim != Coordinator::ReservationResult::Granted ||
            !Coordinator::RelativeStockDetail::checkedAdd(result.economic, seat.seatedValue))
        {
            return result;
        }
        const auto & action = result.plan.action(seat.actionIndex);
        result.actions.push_back(QVariantMap{
            {"actor", actor.engineUnitId}, {"candidate", candidate}, {"kind", static_cast<int>(action.kind)},
            {"destination", QVariantList{action.destination.x, action.destination.y}},
            {"seatedEconomic", static_cast<qint64>(seat.seatedValue)},
        });
    }
    if (!result.plan.rebuildVacateEdges(Coordinator::planOccupancy(result.plan)).empty())
    {
        return result;
    }
    const auto order = result.plan.executionOrder();
    result.valid = order.cycleMembers.empty() && order.blockedByCycle.empty() && order.order.size() == tuple.size();
    return result;
}
}

struct ArenaRelativeStockProbe::Model
{
    Coordinator::AssignmentInput input;
    Coordinator::PropertyStockField field;
};

ArenaRelativeStockProbe::ArenaRelativeStockProbe(bool frontierReceipts, qint32 frontierDay)
    : m_frontierReceipts(frontierReceipts),
      m_frontierDay(frontierDay)
{
}

ArenaRelativeStockProbe::~ArenaRelativeStockProbe() = default;

bool ArenaRelativeStockProbe::loadValuations(const QString & path, QString & error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
    {
        error = "valuation requests are unavailable or oversized";
        return false;
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    const auto root = document.object();
    const auto cases = root.value("cases").toArray();
    const auto supportAudit = root.value("supportAudit");
    if (!document.isObject() || root.value("version").toInt() != 1 ||
        (cases.isEmpty() && !supportAudit.isObject()) || cases.size() > 32)
    {
        error = "valuation requests require version 1 and cases or a support audit";
        return false;
    }
    std::vector<Valuation> requests;
    for (const auto & value : cases)
    {
        const auto object = value.toObject();
        Valuation request;
        request.name = object.value("name").toString();
        const auto prefix = object.value("prefixWork");
        const auto work = prefix.toDouble(-1);
        const auto slice = object.value("slice").toString();
        const auto operation = object.value("operation").toString(
            "comparison");
        const auto cacheAccounting =
            object.value("cacheAccounting").toString("recharge");
        const auto pairUpper =
            object.value("pairPartitionUpper");
        const auto nodePriceDual =
            object.value("nodePriceDual");
        const auto residualNodes =
            object.value("captureResidualNodes");
        if (!value.isObject() || request.name.isEmpty() || request.name.size() > 80 ||
            !readTuple(object.value("incumbent"), request.incumbent) ||
            !readTuple(object.value("challenger"), request.challenger) ||
            request.incumbent.size() != request.challenger.size() ||
            !prefix.isDouble() || work < 0 || work > Coordinator::DEFAULT_RELATIVE_STOCK_WORK_LIMIT ||
            work != std::floor(work) || (slice != "direct" && slice != "doubling") ||
            (operation != "comparison" && operation != "threshold") ||
            (cacheAccounting != "recharge" &&
             cacheAccounting != "once-paid"))
        {
            error = "invalid valuation case";
            return false;
        }
        if ((!pairUpper.isUndefined() && !pairUpper.isBool()) ||
            (!nodePriceDual.isUndefined() &&
             !nodePriceDual.isBool()) ||
            (!residualNodes.isUndefined() &&
             !residualNodes.isBool()))
        {
            error = "threshold options require booleans";
            return false;
        }
        request.prefixWork = static_cast<std::int64_t>(work);
        request.slice = slice == "direct" ? Coordinator::RelativeStockSlice::Direct :
            Coordinator::RelativeStockSlice::Doubling;
        request.operation =
            operation == "threshold"
                ? Valuation::Operation::Threshold
                : Valuation::Operation::Comparison;
        request.cacheAccounting =
            cacheAccounting == "once-paid"
                ? Coordinator::SequentialCacheAccounting::
                    OncePerIdentity
                : Coordinator::SequentialCacheAccounting::
                    RechargeEveryLookup;
        request.thresholdOptions.pairPartitionUpper =
            pairUpper.toBool(false);
        request.thresholdOptions.nodePriceDual =
            nodePriceDual.toBool(false);
        request.thresholdOptions.captureResidualNodes =
            residualNodes.toBool(false);
        if (request.operation == Valuation::Operation::Threshold)
        {
            const auto caps = object.value("challengerCaps").toArray();
            if (caps.isEmpty() || caps.size() > 8)
            {
                error = "threshold valuation requires challenger caps";
                return false;
            }
            for (const auto & capValue : caps)
            {
                const double cap = capValue.toDouble(-1);
                if (!capValue.isDouble() || cap <= 0 ||
                    cap > Coordinator::DEFAULT_RELATIVE_STOCK_WORK_LIMIT ||
                    cap != std::floor(cap))
                {
                    error = "invalid threshold challenger cap";
                    return false;
                }
                request.challengerCaps.push_back(
                    static_cast<std::int64_t>(cap));
            }
        }
        requests.push_back(std::move(request));
    }
    std::unique_ptr<ArenaRelativeStockSupportAudit> audit;
    if (!supportAudit.isUndefined())
    {
        if (!supportAudit.isObject())
        {
            error = "support audit must be an object";
            return false;
        }
        audit = std::make_unique<ArenaRelativeStockSupportAudit>();
        if (!audit->load(supportAudit.toObject(), QFileInfo(path).absolutePath(), error))
        {
            return false;
        }
    }
    m_valuations = std::move(requests);
    m_supportAudit = std::move(audit);
    return true;
}

void ArenaRelativeStockProbe::model(
    const Coordinator::AssignmentInput & input, const Coordinator::PropertyStockField & field)
{
    if ((m_valuations.empty() && !m_supportAudit) || !frontierReceiptsEnabled())
    {
        return;
    }
    m_model = std::make_unique<Model>(Model{input, field});
    m_model->input.pStockValuer = nullptr;
    m_model->input.pTrace = nullptr;
    m_model->input.pRelativeStockObserver = nullptr;
    m_model->input.measureTimings = false;
}

QVariantMap ArenaRelativeStockProbe::evaluate(const Valuation & request) const
{
    using namespace Coordinator;
    QElapsedTimer timer;
    timer.start();
    QVariantMap receipt{
        {"event", "DIAGNOSTIC_VALUATION"}, {"name", request.name}, {"installed", false},
        {"isolatedModel", true}, {"workLimit", static_cast<qint64>(DEFAULT_RELATIVE_STOCK_WORK_LIMIT)},
        {"incumbentTuple", indices(request.incumbent)}, {"challengerTuple", indices(request.challenger)},
        {"requestedPrefixWork", static_cast<qint64>(request.prefixWork)},
        {"slice", static_cast<int>(request.slice)}, {"valid", false},
    };
    auto model = *m_model;
    JointPlanStockValuer valuer(model.field, model.input.unitLinks);
    model.input.pStockValuer = &valuer;
    auto catalog = valuer.continuationPricingCatalog(model.input);
    RefinementLedger ledger;
    if (!catalog.valid || !ledger.open(catalog.ledgerTotal) ||
        !valuer.prepareContinuationPricing(std::move(catalog), ledger))
    {
        receipt.insert("error", "isolated pricing preparation failed");
        return receipt;
    }
    auto* session = valuer.relativeStockSession();
    if (session == nullptr)
    {
        receipt.insert("error", "isolated relative session unavailable");
        return receipt;
    }
    const auto initialBalance = session->remainingWork();
    if (request.prefixWork > 0)
    {
        model.input.relativeStockWorkLimit = request.prefixWork;
        const auto prefix = MaximumValueAssignment::assign(model.input);
        receipt.insert("prefixMixed", mixedStatsReceipt(prefix.stats.mixedPairs));
        receipt.insert("prefixPairs", statsReceipt(prefix.stats.stockPairs));
        receipt.insert("prefixJoint", statsReceipt(prefix.stats.stockJoints));
    }
    const auto prefixWork = initialBalance - session->remainingWork();
    const auto incumbentPlan = valuationPlan(model.input, request.incumbent);
    const auto challengerPlan = valuationPlan(model.input, request.challenger);
    if (!incumbentPlan.valid || !challengerPlan.valid || prefixWork < 0 ||
        prefixWork > DEFAULT_RELATIVE_STOCK_WORK_LIMIT)
    {
        receipt.insert("error", "invalid tuple, claims, ordering or prefix accounting");
        return receipt;
    }
    auto incumbent = session->quote(incumbentPlan.plan, incumbentPlan.economic);
    auto challenger = session->quote(challengerPlan.plan, challengerPlan.economic);
    receipt.insert("initialIncumbent", quoteReceipt(incumbent));
    receipt.insert("initialChallenger", quoteReceipt(challenger));
    receipt.insert("initialComparison", comparisonReceipt(session->compare(incumbent, challenger)));
    receipt.insert("incumbentActions", incumbentPlan.actions);
    receipt.insert("challengerActions", challengerPlan.actions);
    if (request.operation == Valuation::Operation::Threshold)
    {
        QVariantList challengerSteps;
        std::int64_t challengerStates = 0;
        std::int64_t challengerWork = 0;
        bool challengerValid = true;
        for (const std::int64_t cap : request.challengerCaps)
        {
            const auto before = challenger;
            const auto refined = session->refineOur(
                challenger.key, cap);
            challenger = session->quote(
                challengerPlan.plan,
                challengerPlan.economic);
            const bool adopted =
                refined.valid &&
                refined.statesUsed >= 0 &&
                refined.statesUsed <= cap &&
                refined.workUsed >= refined.statesUsed &&
                RelativeStockDetail::monotone(
                    before.ours, refined) &&
                challenger.valid &&
                challenger.ours.lower ==
                    refined.interval.lower &&
                challenger.ours.upper ==
                    refined.interval.upper &&
                session->replayOurWitness(challenger);
            challengerSteps.push_back(QVariantMap{
                {"requestedCap", static_cast<qint64>(cap)},
                {"states",
                 static_cast<qint64>(refined.statesUsed)},
                {"work",
                 static_cast<qint64>(refined.workUsed)},
                {"completed", refined.searchCompleted},
                {"adopted", adopted},
                {"quote", quoteReceipt(challenger)},
                {"phaseAudit",
                 sequentialPhaseReceipt(
                     valuer.diagnosticLastOurRefinement())},
            });
            challengerValid = challengerValid && adopted;
            challengerStates += refined.statesUsed;
            challengerWork += refined.workUsed;
            if (!adopted)
            {
                break;
            }
        }
        const auto threshold =
            commonEnemySequentialThreshold(
                incumbent, challenger);
        DiagnosticSequentialThresholdProof proof;
        const std::int64_t proofAllowance =
            DEFAULT_RELATIVE_STOCK_WORK_LIMIT -
            prefixWork - challengerWork;
        if (challengerValid && threshold)
        {
            proof = valuer.diagnosticProveOurBelow(
                incumbent.key,
                *threshold,
                std::max<std::int64_t>(
                    proofAllowance, 0),
                request.cacheAccounting,
                request.thresholdOptions);
        }
        const auto replayedIncumbent = session->quote(
            incumbentPlan.plan, incumbentPlan.economic);
        const auto replayedChallenger = session->quote(
            challengerPlan.plan, challengerPlan.economic);
        const std::int64_t totalWork =
            initialBalance - session->remainingWork();
        const bool commonEnemyZeroEnemyWork =
            incumbent.capturedColumns ==
                challenger.capturedColumns &&
            incumbent.enemy.lower ==
                replayedIncumbent.enemy.lower &&
            incumbent.enemy.upper ==
                replayedIncumbent.enemy.upper &&
            challenger.enemy.lower ==
                replayedChallenger.enemy.lower &&
            challenger.enemy.upper ==
                replayedChallenger.enemy.upper;
        const bool replays =
            sameRelativeStockQuote(
                incumbent, replayedIncumbent) &&
            sameRelativeStockQuote(
                challenger, replayedChallenger) &&
            session->replayOurWitness(replayedIncumbent) &&
            session->replayOurWitness(replayedChallenger);
        receipt.insert("operation", "threshold");
        receipt.insert(
            "cacheAccounting",
            request.cacheAccounting ==
                    SequentialCacheAccounting::OncePerIdentity
                ? "once-paid"
                : "recharge");
        receipt.insert("challengerSteps", challengerSteps);
        receipt.insert(
            "challengerStates",
            static_cast<qint64>(challengerStates));
        receipt.insert(
            "challengerWork",
            static_cast<qint64>(challengerWork));
        receipt.insert(
            "threshold",
            threshold
                ? QVariant::fromValue(
                      static_cast<qint64>(*threshold))
                : QVariant());
        receipt.insert(
            "thresholdProof",
            thresholdProofReceipt(proof));
        receipt.insert(
            "proofAllowance",
            static_cast<qint64>(
                std::max<std::int64_t>(
                    proofAllowance, 0)));
        receipt.insert("incumbent", quoteReceipt(incumbent));
        receipt.insert("challenger", quoteReceipt(challenger));
        receipt.insert(
            "prefixWork", static_cast<qint64>(prefixWork));
        receipt.insert(
            "totalWork", static_cast<qint64>(totalWork));
        receipt.insert("enemyStates", 0);
        receipt.insert(
            "commonEnemyZeroEnemyWork",
            commonEnemyZeroEnemyWork);
        receipt.insert("witnessesReplay", replays);
        receipt.insert("selectionChanged", false);
        receipt.insert("elapsedNanos", timer.nsecsElapsed());
        receipt.insert(
            "valid",
            challengerValid && threshold.has_value() &&
                proof.valid && commonEnemyZeroEnemyWork &&
                replays &&
                totalWork ==
                    prefixWork + challengerWork +
                        proof.workUsed &&
                totalWork <=
                    DEFAULT_RELATIVE_STOCK_WORK_LIMIT);
        return receipt;
    }
    QVariantList steps;
    const auto refined = refineRelativeStock(*session, incumbent, challenger,
        RelativeStockRefinementPolicy{
            .workLimit = DEFAULT_RELATIVE_STOCK_WORK_LIMIT - prefixWork,
            .slice = request.slice,
        },
        [&](const RelativeStockStep & step, const RelativeStockQuote & oldPlan, const RelativeStockQuote & newPlan)
        {
            QVariantMap stepReceipt{
                {"target", static_cast<int>(step.target)}, {"requestedCap", static_cast<qint64>(step.requestedCap)},
                {"states", static_cast<qint64>(step.refined.statesUsed)}, {"work", static_cast<qint64>(step.refined.workUsed)},
                {"completed", step.refined.searchCompleted}, {"witnessReplays", step.refined.lowerWitnessReplays},
                {"adopted", step.adopted}, {"incumbent", quoteReceipt(oldPlan)}, {"challenger", quoteReceipt(newPlan)},
                {"comparison", comparisonReceipt(step.comparison)},
                {"verifiedDual", step.verifiedDual},
                {"certifiedUpper", static_cast<qint64>(step.dual.certifiedUpper)},
                {"catalogWork", static_cast<qint64>(step.dual.catalogWork)},
                {"optimizerWork", static_cast<qint64>(step.dual.optimizerWork)},
                {"verifierWork", static_cast<qint64>(step.dual.verifierWork)},
                {"tariffWork", static_cast<qint64>(step.dual.tariffWork)},
                {"certificate", static_cast<qulonglong>(step.dual.certificateId)},
                {"localExhaustion", step.dual.locallyBudgetExhausted},
                {"globalFailure", step.dual.globalFailure},
            };
            if (step.target == RelativeStockTarget::IncumbentOurs ||
                step.target == RelativeStockTarget::ChallengerOurs)
            {
                stepReceipt.insert(
                    "phaseAudit",
                    sequentialPhaseReceipt(
                        valuer.diagnosticLastOurRefinement()));
            }
            steps.push_back(std::move(stepReceipt));
        });
    const auto replayedIncumbent = session->quote(incumbentPlan.plan, incumbentPlan.economic);
    const auto replayedChallenger = session->quote(challengerPlan.plan, challengerPlan.economic);
    const auto totalWork = initialBalance - session->remainingWork();
    const bool replays = sameRelativeStockQuote(incumbent, replayedIncumbent) &&
        sameRelativeStockQuote(challenger, replayedChallenger) &&
        session->replayOurWitness(replayedIncumbent) && session->replayOurWitness(replayedChallenger) &&
        (refined.comparison.commonEnemyCancelled ||
         (session->replayEnemyWitness(replayedIncumbent) && session->replayEnemyWitness(replayedChallenger)));
    receipt.insert("incumbent", quoteReceipt(incumbent));
    receipt.insert("challenger", quoteReceipt(challenger));
    receipt.insert("comparison", comparisonReceipt(refined.comparison));
    receipt.insert("steps", steps);
    receipt.insert("prefixWork", static_cast<qint64>(prefixWork));
    receipt.insert("relativeWork", static_cast<qint64>(refined.workUsed));
    receipt.insert("totalWork", static_cast<qint64>(totalWork));
    receipt.insert("states", static_cast<qint64>(refined.statesUsed));
    receipt.insert("enemyStates", static_cast<qint64>(refined.enemyStatesUsed));
    receipt.insert("stop", static_cast<int>(refined.stop));
    receipt.insert("witnessesReplay", replays);
    receipt.insert("elapsedNanos", timer.nsecsElapsed());
    receipt.insert("valid", replays && refined.comparison.valid() && totalWork == prefixWork + refined.workUsed &&
        totalWork <= DEFAULT_RELATIVE_STOCK_WORK_LIMIT);
    return receipt;
}

bool ArenaRelativeStockProbe::frontierReceiptsEnabled() const
{
    return m_frontierReceipts &&
        (m_frontierDay < 0 || m_identity.value("day").toInt() == m_frontierDay);
}

void ArenaRelativeStockProbe::beginFrontier(
    Coordinator::RelativeStockPhase phase, std::int64_t count, std::int64_t work)
{
    ++m_frontierIndex;
    append({{"event", "FRONTIER"}, {"frontier", m_frontierIndex}, {"phase", static_cast<int>(phase)},
            {"contenders", static_cast<qint64>(count)}, {"availableWork", static_cast<qint64>(work)}});
}

void ArenaRelativeStockProbe::contender(const Coordinator::RelativeStockFrontierAudit & audit)
{
    if (audit.selected)
    {
        m_selectedTuple = indices(audit.tuple);
    }
    if (m_frontierRecords >= MAX_FRONTIER_RECEIPTS)
    {
        ++m_droppedFrontierRecords;
        return;
    }
    ++m_frontierRecords;
    QVariantList actions;
    qint32 captures = 0;
    for (const auto & action : audit.actions)
    {
        captures += action.captures ? 1 : 0;
        actions.push_back(QVariantMap{
            {"actor", action.actor}, {"candidate", action.candidate}, {"kind", static_cast<int>(action.kind)},
            {"destination", QVariantList{action.destination.x, action.destination.y}}, {"captures", action.captures},
        });
    }
    append({
        {"event", audit.selected ? "SELECT_CONTENDER" : "CONTENDER"}, {"frontier", m_frontierIndex},
        {"phase", static_cast<int>(audit.phase)}, {"rank", static_cast<qint64>(audit.rank)},
        {"actors", indices(audit.actors)}, {"tuple", indices(audit.tuple)}, {"actions", actions}, {"captureCount", captures},
        {"incumbent", quoteReceipt(audit.incumbent)}, {"challenger", quoteReceipt(audit.challenger)},
        {"comparison", comparisonReceipt(audit.comparison)},
        {"intervalWidths", QVariantList{
            static_cast<qint64>(audit.incumbent.ours.upper - audit.incumbent.ours.lower),
            static_cast<qint64>(audit.challenger.ours.upper - audit.challenger.ours.lower),
            static_cast<qint64>(audit.incumbent.enemy.upper - audit.incumbent.enemy.lower),
            static_cast<qint64>(audit.challenger.enemy.upper - audit.challenger.enemy.lower)}},
    });
}

void ArenaRelativeStockProbe::beginPlan(const Coordinator::RelativeStockPlanIdentity & identity)
{
    m_model.reset();
    m_identity = {
        {"day", identity.day}, {"player", identity.player}, {"sequence", static_cast<qulonglong>(identity.sequence)},
        {"horizon", identity.horizon},
    };
    append({{"event", "BEGIN"}});
}

void ArenaRelativeStockProbe::finishPlan(
    const Coordinator::AssignmentResult & result, const Coordinator::RelativeStockBudgetAudit & budget)
{
    QVariantList actions;
    for (std::int32_t index = 0; index < result.plan.actionCount(); ++index)
    {
        const auto & action = result.plan.action(index);
        QVariantList path;
        for (const auto & tile : action.path)
        {
            path.push_back(QVariantList{tile.x, tile.y});
        }
        actions.push_back(QVariantMap{
            {"index", index}, {"actor", action.unitId}, {"kind", static_cast<int>(action.kind)}, {"actionId", action.actionId},
            {"destination", QVariantList{action.destination.x, action.destination.y}},
            {"target", QVariantList{action.target.x, action.target.y}}, {"targetUnit", action.targetUnitId},
            {"damage", action.plannedDamageSteps}, {"state", static_cast<int>(action.state)}, {"path", path},
        });
    }
    append({
        {"event", "FINISH"}, {"actions", actions}, {"executionOrder", indices(result.executionOrder)},
        {"assignedUnits", result.stats.assignedUnits}, {"unassignedUnits", result.stats.unassignedUnits},
        {"candidates", result.stats.candidates}, {"unsupportedCandidates", result.stats.unsupportedCandidates},
        {"conflicts", result.stats.conflicts}, {"overkillRejections", result.stats.overkillRejections},
        {"staleRejections", result.stats.staleRejections}, {"invalidActions", result.stats.invalidActions},
        {"vacateConflicts", result.stats.vacateConflicts}, {"unorderedActions", result.stats.unorderedActions},
        {"settlingSweeps", result.stats.settlingSweeps}, {"settlingMoves", result.stats.settlingMoves},
        {"swapImprovements", result.stats.swapImprovements}, {"swapStates", result.stats.swapStates},
        {"clustersTotal", result.stats.clustersTotal}, {"clustersEnumerated", result.stats.clustersEnumerated},
        {"clustersCapped", result.stats.clustersCapped}, {"clustersSkippedStockBudget", result.stats.clustersSkippedStockBudget},
        {"enumerationStates", result.stats.enumerationStates}, {"replayFailures", result.stats.replayFailures},
        {"pairs", statsReceipt(result.stats.stockPairs)}, {"mixedPairs", mixedStatsReceipt(result.stats.mixedPairs)},
        {"joint", statsReceipt(result.stats.stockJoints)},
        {"initialAvailable", static_cast<qint64>(budget.initialAvailable)}, {"totalLimit", static_cast<qint64>(budget.totalLimit)},
        {"pairLimit", static_cast<qint64>(budget.pairLimit)}, {"workSpent", static_cast<qint64>(budget.spent)},
        {"workRemaining", static_cast<qint64>(budget.remaining)},
    });
    if (m_model)
    {
        if (m_supportAudit)
        {
            append(m_supportAudit->run(m_model->input, m_model->field, result));
            m_supportAudit.reset();
        }
        for (const auto & valuation : m_valuations)
        {
            append(evaluate(valuation));
        }
        m_model.reset();
    }
}

void ArenaRelativeStockProbe::mixedBoundary(std::int64_t maximumWork, const Coordinator::LiveRefinementResult & result)
{
    append({
        {"event", "MIXED_BOUNDARY"}, {"maximumWork", static_cast<qint64>(maximumWork)},
        {"refined", result.refined}, {"keysRefined", result.keysRefined},
        {"states", static_cast<qint64>(result.statesUsed)}, {"enemyStates", static_cast<qint64>(result.enemyStatesUsed)},
        {"work", static_cast<qint64>(result.workUsed)}, {"budgetExhausted", result.budgetExhausted}, {"valid", result.valid},
    });
}

void ArenaRelativeStockProbe::domain(const Coordinator::RelativeStockDomainAudit & audit)
{
    append({
        {"event", "DOMAIN"}, {"phase", static_cast<int>(audit.phase)}, {"status", static_cast<int>(audit.status)},
        {"actors", indices(audit.actors)}, {"product", static_cast<qint64>(audit.product)},
        {"states", static_cast<qint64>(audit.states)}, {"claimable", static_cast<qint64>(audit.claimable)},
        {"contenders", static_cast<qint64>(audit.contenders)},
    });
}

void ArenaRelativeStockProbe::step(
    Coordinator::RelativeStockPhase phase, const Coordinator::RelativeStockStep & step,
    const Coordinator::RelativeStockQuote & incumbent, const Coordinator::RelativeStockQuote & challenger)
{
    QVariantMap record = {
        {"event", "STEP"}, {"phase", static_cast<int>(phase)}, {"target", static_cast<int>(step.target)},
        {"requestedCap", static_cast<qint64>(step.requestedCap)}, {"states", static_cast<qint64>(step.refined.statesUsed)},
        {"work", static_cast<qint64>(step.refined.workUsed)}, {"completed", step.refined.searchCompleted},
        {"witnessReplays", step.refined.lowerWitnessReplays}, {"adopted", step.adopted},
        {"incumbent", quoteReceipt(incumbent)}, {"challenger", quoteReceipt(challenger)},
        {"comparison", comparisonReceipt(step.comparison)},
        {"verifiedDual", step.verifiedDual},
        {"certifiedUpper", static_cast<qint64>(step.dual.certifiedUpper)},
        {"catalogWork", static_cast<qint64>(step.dual.catalogWork)},
        {"optimizerWork", static_cast<qint64>(step.dual.optimizerWork)},
        {"verifierWork", static_cast<qint64>(step.dual.verifierWork)},
        {"tariffWork", static_cast<qint64>(step.dual.tariffWork)},
        {"certificate", static_cast<qulonglong>(step.dual.certificateId)},
        {"localExhaustion", step.dual.locallyBudgetExhausted},
        {"globalFailure", step.dual.globalFailure},
    };
    if (frontierReceiptsEnabled())
    {
        record.insert("frontier", m_frontierIndex);
        record.insert("tuple", m_selectedTuple);
        record.insert("beforeLower", static_cast<qint64>(step.before.lower));
        record.insert("beforeUpper", static_cast<qint64>(step.before.upper));
    }
    append(std::move(record));
}

void ArenaRelativeStockProbe::comparison(
    Coordinator::RelativeStockPhase phase, std::span<const std::int32_t> tuple,
    const Coordinator::RelativeStockQuote & incumbent, const Coordinator::RelativeStockQuote & challenger,
    const Coordinator::RelativeStockRefinementResult & result, bool accepted)
{
    append({
        {"event", "COMPARISON"}, {"phase", static_cast<int>(phase)}, {"tuple", indices(tuple)},
        {"incumbent", quoteReceipt(incumbent)}, {"challenger", quoteReceipt(challenger)},
        {"comparison", comparisonReceipt(result.comparison)}, {"states", static_cast<qint64>(result.statesUsed)},
        {"enemyStates", static_cast<qint64>(result.enemyStatesUsed)}, {"work", static_cast<qint64>(result.workUsed)},
        {"stop", static_cast<int>(result.stop)}, {"budgetExhausted", result.budgetExhausted}, {"accepted", accepted},
    });
}

void ArenaRelativeStockProbe::append(QVariantMap record)
{
    for (auto item = m_identity.cbegin(); item != m_identity.cend(); ++item)
    {
        record.insert(item.key(), item.value());
    }
    m_records.push_back(std::move(record));
}

QVariantMap ArenaRelativeStockProbe::result() const
{
    if (m_frontierReceipts)
    {
        return {{"version", 2}, {"records", m_records}, {"frontierRecordLimit", MAX_FRONTIER_RECEIPTS},
                {"frontierRecords", m_frontierRecords}, {"droppedFrontierRecords", m_droppedFrontierRecords}};
    }
    return {{"version", 1}, {"records", m_records}};
}
