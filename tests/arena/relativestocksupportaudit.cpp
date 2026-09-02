#include "tests/arena/relativestocksupportaudit.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <map>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include "ai/coordinator/propertystockbuilder.h"
#include "tests/arena/relativestockreceipts.h"

namespace
{
using namespace Coordinator;
using ArenaStockReceipts::comparisonReceipt;
using ArenaStockReceipts::indices;
using ArenaStockReceipts::quoteReceipt;
using Audit = ArenaRelativeStockSupportAudit;

constexpr qsizetype MAX_TARGETS = 32;
constexpr qsizetype MAX_POSITIVE_EXAMPLES = 32;

struct Contender
{
    std::vector<std::int32_t> support;
    std::vector<std::int32_t> tuple;
    RelativeStockQuote quote;
    RelativeStockComparison comparison;
};

struct TermWork
{
    std::int64_t work{0};
    std::int64_t states{0};
    std::int64_t maximumCap{0};
};

struct DomainCounts
{
    std::int64_t supports{0};
    std::int64_t productionEligible{0};
    std::int64_t raw{0};
    std::int64_t claimable{0};
    std::int64_t unique{0};
    std::int64_t duplicates{0};
    std::int64_t positive{0};
    std::int64_t nonImproving{0};
    std::int64_t unresolved{0};
};

bool readInteger(const QJsonValue & value, std::int64_t maximum, std::int64_t & result)
{
    const auto number = value.toDouble(-1);
    if (!value.isDouble() || number <= 0 || number > maximum || number != std::floor(number))
    {
        return false;
    }
    result = static_cast<std::int64_t>(number);
    return true;
}

std::vector<std::int32_t> tupleFromJson(const QJsonValue & value)
{
    std::vector<std::int32_t> result;
    if (!ArenaStockReceipts::readTuple(value, result))
    {
        return {};
    }
    return result;
}

bool writeRecord(QFile & file, const QVariantMap & record)
{
    const auto line = QJsonDocument::fromVariant(record).toJson(QJsonDocument::Compact) + '\n';
    return file.write(line) == line.size();
}

QByteArray identity(const AssignmentResult & assignment, const RelativeStockQuote & quote)
{
    auto bytes = Audit::planIdentity(assignment);
    QDataStream stream(&bytes, QIODevice::Append);
    stream.setVersion(QDataStream::Qt_6_5);
    stream << static_cast<qint64>(quote.economic) << static_cast<quint64>(quote.key.size());
    for (const auto & action : quote.key)
    {
        stream << action.row() << action.x() << action.y() << action.captures();
    }
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

QVariantMap workReceipt(const TermWork & work)
{
    return {{"work", static_cast<qint64>(work.work)}, {"states", static_cast<qint64>(work.states)},
            {"maximumRequestedCap", static_cast<qint64>(work.maximumCap)}};
}

QVariantMap actionReceipt(const AssignmentResult & assignment, const AssignmentResult::Selection & selection)
{
    const auto & action = assignment.plan.action(selection.actionIndex);
    QVariantList path;
    for (const auto & tile : action.path)
    {
        path.push_back(QVariantList{tile.x, tile.y});
    }
    return {
        {"actor", selection.engineUnitId}, {"candidate", selection.candidateIndex},
        {"greedyCandidate", selection.greedyCandidateIndex}, {"actionIndex", selection.actionIndex},
        {"stockCoupled", selection.stockCoupled}, {"selectionPhase", static_cast<int>(selection.phase)},
        {"seatedEconomic", static_cast<qint64>(selection.seatedValue)},
        {"kind", static_cast<int>(action.kind)}, {"actionId", action.actionId},
        {"destination", QVariantList{action.destination.x, action.destination.y}}, {"path", path},
        {"target", QVariantList{action.target.x, action.target.y}}, {"targetUnit", action.targetUnitId},
        {"damage", action.plannedDamageSteps}, {"state", static_cast<int>(action.state)},
        {"identity", QString::fromLatin1(QCryptographicHash::hash(
            Audit::actorIdentity(assignment, selection.engineUnitId), QCryptographicHash::Sha256).toHex())},
    };
}

template<class Key>
TermWork workFor(const std::map<Key, TermWork> & accounts, const Key & key)
{
    const auto found = accounts.find(key);
    return found == accounts.end() ? TermWork{} : found->second;
}

class SupportRun
{
public:
    SupportRun(const AssignmentInput & input, const PropertyStockField & field, const AssignmentResult & incumbent,
               const QString & output, const Audit::Limits & limits, const std::vector<Audit::Target> & targets)
        : m_input(input),
          m_field(field),
          m_incumbent(incumbent),
          m_output(output),
          m_limits(limits),
          m_targets(targets),
          m_valuer(m_field, m_input.unitLinks),
          m_catalog(output + "-catalog.jsonl"),
          m_steps(output + "-refinement.jsonl")
    {
        m_input.pStockValuer = &m_valuer;
        m_input.pTrace = nullptr;
        m_input.pRelativeStockObserver = nullptr;
        m_input.measureTimings = false;
    }

    QVariantMap execute()
    {
        QVariantMap result{
            {"event", "DIAGNOSTIC_SUPPORT_AUDIT"}, {"diagnosticOnly", true}, {"installed", false},
            {"productionWorkLimit", static_cast<qint64>(DEFAULT_RELATIVE_STOCK_WORK_LIMIT)},
            {"diagnosticWorkLimit", static_cast<qint64>(m_limits.totalWork)},
            {"discoveryWorkLimit", static_cast<qint64>(m_limits.discoveryWork)},
            {"maximumStateCap", static_cast<qint64>(m_limits.maximumStateCap)},
            {"incumbentStateCap", static_cast<qint64>(m_limits.incumbentStateCap)},
            {"outputPrefix", m_output}, {"valid", false},
        };
        if (!verifyIncumbent(result) || !prepare())
        {
            result.insert("error", m_error);
            return result;
        }
        result.insert("cheapIncumbent", quoteReceipt(m_cheapIncumbent));
        QVariantList actorOrder;
        QVariantList incumbentActions;
        QVariantList dependencies;
        for (const auto & actor : m_actors)
        {
            actorOrder.push_back(actor.pActor->engineUnitId);
        }
        for (const auto & selection : m_incumbent.selections)
        {
            incumbentActions.push_back(actionReceipt(m_incumbent, selection));
        }
        for (std::int32_t index = 0; index < m_incumbent.plan.edgeCount(); ++index)
        {
            const auto & edge = m_incumbent.plan.edge(index);
            dependencies.push_back(QVariantMap{{"from", edge.from}, {"to", edge.to}, {"kind", static_cast<int>(edge.kind)}});
        }
        result.insert("actorOrder", actorOrder);
        result.insert("incumbentActions", incumbentActions);
        result.insert("incumbentDependencies", dependencies);
        result.insert("incumbentExecutionOrder", indices(m_incumbent.executionOrder));
        result.insert("timingIncludesDiagnosticWork", true);
        for (std::size_t size = 1; size <= Audit::MAXIMUM_SUPPORT && m_error.isEmpty(); ++size)
        {
            std::vector<std::int32_t> support;
            enumerateSupports(size, 0, support);
        }
        if (!m_catalog.flush())
        {
            fail("catalog flush failed");
        }
        m_catalog.close();
        m_unique.clear();
        m_unique.squeeze();
        if (!m_error.isEmpty())
        {
            result.insert("error", m_error);
            return result;
        }
        refineTerm(m_incumbentQuote, true, m_limits.incumbentStateCap, m_limits.discoveryWork, "SHARED_INCUMBENT", {});
        for (std::size_t size = 1; size <= Audit::MAXIMUM_SUPPORT && m_error.isEmpty(); ++size)
        {
            if (discover(size))
            {
                break;
            }
        }
        result.insert("targeted", targeted());
        if (m_error.isEmpty())
        {
            finishCatalog(result);
        }
        result.insert("incumbent", quoteReceipt(m_incumbentQuote));
        result.insert("incumbentWork", workReceipt(workFor(m_ourWork, m_incumbentQuote.key)));
        result.insert("diagnosticWorkUsed", static_cast<qint64>(spent()));
        result.insert("diagnosticStatesUsed", static_cast<qint64>(m_states));
        result.insert("diagnosticEnemyStatesUsed", static_cast<qint64>(m_enemyStates));
        result.insert("mandatoryTariff", static_cast<qint64>(m_mandatoryTariff));
        result.insert("ledgerTotal", static_cast<qint64>(m_ledger.stats().total));
        result.insert("ledgerGranted", static_cast<qint64>(m_ledger.stats().granted));
        result.insert("ledgerRefunded", static_cast<qint64>(m_ledger.stats().refunded));
        result.insert("ledgerRemaining", static_cast<qint64>(m_ledger.balance()));
        if (!m_steps.flush())
        {
            fail("refinement receipt flush failed");
        }
        if (spent() < 0 || spent() > m_limits.totalWork ||
            m_ledger.stats().granted - m_ledger.stats().refunded != spent() + m_mandatoryTariff)
        {
            fail("diagnostic ledger accounting mismatch");
        }
        result.insert("valid", m_error.isEmpty());
        if (!m_error.isEmpty())
        {
            result.insert("error", m_error);
        }
        QFile summary(m_output + "-summary.json");
        if (!summary.open(QIODevice::WriteOnly | QIODevice::NewOnly) || !writeRecord(summary, result) || !summary.flush())
        {
            result.insert("valid", false);
            result.insert("error", "support summary write failed");
        }
        std::cout << "SUPPORT_AUDIT_FINISH valid=" << result.value("valid").toBool()
                  << " work=" << spent() << std::endl;
        return result;
    }

private:
    void fail(const QString & error)
    {
        if (m_error.isEmpty())
        {
            m_error = error;
        }
    }

    std::int64_t spent() const
    {
        return m_session == nullptr ? 0 : m_initialBalance - m_session->remainingWork();
    }

    bool verifyIncumbent(QVariantMap & result)
    {
        auto field = m_field;
        auto input = m_input;
        Audit::SelectionTrace trace;
        input.pTrace = &trace;
        JointPlanStockValuer valuer(field, input.unitLinks);
        input.pStockValuer = &valuer;
        input.relativeStockWorkLimit = DEFAULT_RELATIVE_STOCK_WORK_LIMIT;
        input.relativeStockSchedule = RelativeStockSchedule::Current;
        auto catalog = valuer.continuationPricingCatalog(input);
        RefinementLedger ledger;
        if (!catalog.valid || !ledger.open(catalog.ledgerTotal) ||
            !valuer.prepareContinuationPricing(std::move(catalog), ledger))
        {
            fail("production-shaped incumbent preparation failed");
            return false;
        }
        const auto repeated = MaximumValueAssignment::assign(input);
        const bool same = Audit::planIdentity(repeated) == Audit::planIdentity(m_incumbent);
        result.insert("incumbentRepeatedExactly", same);
        result.insert("reproductionWork", static_cast<qint64>(repeated.stats.mixedPairs.workUsed +
            repeated.stats.stockPairs.workUsed + repeated.stats.stockJoints.workUsed));
        result.insert("reproductionReplayFailures", repeated.stats.replayFailures);
        if (!same || repeated.stats.replayFailures != 0)
        {
            fail("copied production-shaped plan does not reproduce the complete live incumbent");
        }
        return m_error.isEmpty();
    }

    bool prepare()
    {
        auto catalog = m_valuer.continuationPricingCatalog(m_input);
        m_mandatoryTariff = catalog.mandatoryTariff;
        if (!catalog.valid || !m_valuer.prepareDiagnosticContinuationPricing(std::move(catalog), m_ledger, m_limits.totalWork))
        {
            fail("diagnostic pricing preparation failed");
            return false;
        }
        m_session = m_valuer.relativeStockSession();
        if (m_session == nullptr)
        {
            fail("diagnostic relative session missing");
            return false;
        }
        m_initialBalance = m_session->remainingWork();
        if (m_initialBalance != m_limits.totalWork ||
            !Audit::replayIncumbent(m_input, m_incumbent, m_incumbentEconomic))
        {
            fail("incumbent replay or diagnostic allowance invalid");
            return false;
        }
        m_actors = Audit::progress(m_input, m_incumbent);
        m_options.resize(m_actors.size());
        for (std::size_t slot = 0; slot < m_actors.size(); ++slot)
        {
            const auto & actor = m_actors[slot];
            m_incumbentTuple.push_back(actor.chosen);
            if (!stockCoupled(m_input, actor.pActor->engineUnitId))
            {
                continue;
            }
            m_stockSlots.push_back(static_cast<std::int32_t>(slot));
            for (std::size_t choice = 0; choice < actor.pActor->candidates.size(); ++choice)
            {
                if (static_cast<std::int32_t>(choice) != actor.chosen &&
                    isPlannableCandidate(m_input.actionIds, m_input.unitLinks, actor.pActor->candidates[choice]))
                {
                    m_options[slot].push_back(static_cast<std::int32_t>(choice));
                }
            }
        }
        std::sort(m_stockSlots.begin(), m_stockSlots.end(), [&](auto left, auto right)
        {
            return m_actors[left].pActor->engineUnitId < m_actors[right].pActor->engineUnitId;
        });
        m_cheapIncumbent = m_session->quote(m_incumbent.plan, m_incumbentEconomic);
        m_incumbentQuote = m_cheapIncumbent;
        if (!m_cheapIncumbent.valid || !m_session->replayOurWitness(m_cheapIncumbent) ||
            !m_catalog.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            !m_steps.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        {
            fail("invalid incumbent witness or existing/unwritable diagnostic outputs");
        }
        return m_error.isEmpty();
    }

    QVariantList supportActors(const std::vector<std::int32_t> & support) const
    {
        QVariantList result;
        for (const auto slot : support)
        {
            result.push_back(m_actors[slot].pActor->engineUnitId);
        }
        return result;
    }

    std::vector<std::int32_t> supportOf(const std::vector<std::int32_t> & tuple) const
    {
        std::vector<std::int32_t> result;
        for (const auto slot : m_stockSlots)
        {
            if (tuple[slot] != m_incumbentTuple[slot])
            {
                result.push_back(slot);
            }
        }
        return result;
    }

    QVariantMap catalogRecord(const Audit::Trial & trial, const Contender & contender)
    {
        QVariantList changed;
        std::int32_t captures = 0;
        for (const auto slot : contender.support)
        {
            const auto & action = trial.assignment.plan.action(m_actors[slot].actionIndex);
            const bool capture = action.kind == PlanBundleKind::Capture || action.kind == PlanBundleKind::MoveAndCapture;
            captures += capture ? 1 : 0;
            const auto selection = std::find_if(trial.assignment.selections.begin(), trial.assignment.selections.end(),
                [&](const auto & selected) { return selected.engineUnitId == action.unitId; });
            changed.push_back(actionReceipt(trial.assignment, *selection));
        }
        return {
            {"support", supportActors(contender.support)}, {"supportSlots", indices(contender.support)},
            {"tuple", indices(contender.tuple)}, {"changedActions", changed}, {"captureCountLabel", captures},
            {"completePlanIdentity", QString::fromLatin1(identity(trial.assignment, contender.quote))},
            {"cheapQuote", quoteReceipt(contender.quote)}, {"cheapComparison", comparisonReceipt(contender.comparison)},
            {"completePlanReplay", true}, {"fixedNonMembersIdentical", true}, {"diagnosticOnly", true},
            {"executionOrder", indices(trial.assignment.executionOrder)},
        };
    }

    void enumerateSupports(std::size_t size, std::size_t first, std::vector<std::int32_t> & support)
    {
        if (!m_error.isEmpty())
        {
            return;
        }
        if (support.size() == size)
        {
            enumerateDomain(support);
            return;
        }
        const auto needed = size - support.size();
        for (std::size_t next = first; next + needed <= m_stockSlots.size(); ++next)
        {
            support.push_back(m_stockSlots[next]);
            enumerateSupports(size, next + 1, support);
            support.pop_back();
        }
    }

    void enumerateDomain(const std::vector<std::int32_t> & support)
    {
        auto & counts = m_counts[support.size()];
        ++counts.supports;
        std::int64_t product = 1;
        for (const auto slot : support)
        {
            const auto count = static_cast<std::int64_t>(m_options[slot].size());
            if (count != 0 && product > std::numeric_limits<std::int64_t>::max() / count)
            {
                fail("diagnostic support product overflow");
                return;
            }
            product *= count;
        }
        if (product <= MaximumValueAssignment::RELATIVE_JOINT_PRODUCT_CAP)
        {
            ++counts.productionEligible;
        }
        const auto rawBefore = counts.raw;
        const auto claimableBefore = counts.claimable;
        auto tuple = m_incumbentTuple;
        std::vector<std::size_t> choices(support.size(), 0);
        for (std::int64_t leaf = 0; leaf < product && m_error.isEmpty(); ++leaf)
        {
            for (std::size_t index = 0; index < support.size(); ++index)
            {
                tuple[support[index]] = m_options[support[index]][choices[index]];
            }
            ++counts.raw;
            const auto trial = Audit::makeTrial(m_input, m_incumbent, m_actors, support, tuple);
            if (trial.valid)
            {
                ++counts.claimable;
                Contender contender{support, tuple, m_session->quote(trial.assignment.plan, trial.economic), {}};
                contender.comparison = m_session->compare(m_cheapIncumbent, contender.quote);
                if (!contender.comparison.valid() || !m_session->replayOurWitness(contender.quote))
                {
                    fail("legal complete challenger has an invalid cheap quote or witness");
                    return;
                }
                const auto planIdentity = identity(trial.assignment, contender.quote);
                if (m_unique.contains(planIdentity))
                {
                    ++counts.duplicates;
                }
                else
                {
                    m_unique.insert(planIdentity);
                    ++counts.unique;
                }
                if (!writeRecord(m_catalog, catalogRecord(trial, contender)))
                {
                    fail("support catalog write failed");
                    return;
                }
            }
            for (std::size_t index = choices.size(); index-- > 0;)
            {
                if (++choices[index] < m_options[support[index]].size())
                {
                    break;
                }
                choices[index] = 0;
            }
        }
        const QVariantMap domain{
            {"event", "SUPPORT_DOMAIN"}, {"support", supportActors(support)}, {"product", static_cast<qint64>(product)},
            {"rawLeaves", static_cast<qint64>(counts.raw - rawBefore)},
            {"claimableLeaves", static_cast<qint64>(counts.claimable - claimableBefore)},
            {"complete", counts.raw - rawBefore == product},
            {"productionProductEligible", product <= MaximumValueAssignment::RELATIVE_JOINT_PRODUCT_CAP},
            {"diagnosticOnly", true},
        };
        if (!writeRecord(m_steps, domain) || !m_steps.flush())
        {
            fail("support domain receipt write failed");
        }
        std::cout << "SUPPORT_DOMAIN size=" << support.size() << " product=" << product
                  << " legal=" << counts.claimable - claimableBefore << std::endl;
    }

    bool refineTerm(RelativeStockQuote & quote, bool ours, std::int64_t cap, std::int64_t workLimit,
                    const QString & phase, const std::vector<std::int32_t> & tuple)
    {
        auto & account = ours ? m_ourWork[quote.key] : m_enemyWork[quote.capturedColumns];
        const auto before = ours ? quote.ours : quote.enemy;
        if (before.exact() || account.maximumCap >= cap)
        {
            return true;
        }
        const auto tariff = ours ? m_session->ourRerunWork() : m_session->enemyRerunWork(quote.capturedColumns);
        const auto available = std::min(workLimit - spent(), m_session->remainingWork());
        if (tariff < 0 || available <= tariff)
        {
            return false;
        }
        const auto requested = std::min(cap, available - tariff);
        const auto balance = m_session->remainingWork();
        const auto refined = ours ? m_session->refineOur(quote.key, requested) :
            m_session->refineEnemy(quote.capturedColumns, requested);
        const auto refreshed = m_session->quote(quote.key, quote.economic);
        const auto interval = ours ? refreshed.ours : refreshed.enemy;
        const auto completed = ours ? refreshed.ourSearchCompleted : refreshed.enemySearchCompleted;
        const bool bounded = refined.statesUsed >= 0 && refined.statesUsed <= requested &&
            refined.workUsed >= refined.statesUsed && refined.workUsed <= requested + tariff &&
            m_session->remainingWork() == balance - refined.workUsed;
        const bool adopted = bounded && RelativeStockDetail::monotone(before, refined) &&
            RelativeStockDetail::sameProblem(quote, refreshed) && refreshed.valid &&
            interval.lower == refined.interval.lower && interval.upper == refined.interval.upper &&
            completed == refined.searchCompleted;
        if (bounded)
        {
            account.work += refined.workUsed;
            account.states += refined.statesUsed;
            account.maximumCap = std::max(account.maximumCap, requested);
            m_states += refined.statesUsed;
            if (!ours)
            {
                m_enemyStates += refined.statesUsed;
            }
        }
        if (!writeRecord(m_steps, {
                {"event", "DIAGNOSTIC_REFINEMENT"}, {"phase", phase}, {"tuple", indices(tuple)}, {"ours", ours},
                {"before", quoteReceipt(quote)}, {"after", quoteReceipt(refreshed)},
                {"requestedCap", static_cast<qint64>(requested)}, {"tariff", static_cast<qint64>(tariff)},
                {"work", static_cast<qint64>(refined.workUsed)}, {"states", static_cast<qint64>(refined.statesUsed)},
                {"completed", refined.searchCompleted}, {"witnessReplays", refined.lowerWitnessReplays},
                {"adopted", adopted}, {"diagnosticOnly", true}, {"cumulativeWork", static_cast<qint64>(spent())},
            }) || !m_steps.flush())
        {
            fail("refinement step write failed");
        }
        if (!adopted)
        {
            fail("diagnostic refinement failed identity, bounds, witness or accounting validation");
            return false;
        }
        quote = refreshed;
        m_incumbentQuote = m_session->quote(m_incumbentQuote.key, m_incumbentEconomic);
        return true;
    }

    bool refineComparison(Contender & contender, std::int64_t workLimit, const QString & phase)
    {
        std::array<bool, 4> exhausted{};
        while (m_error.isEmpty())
        {
            contender.quote = m_session->quote(contender.quote.key, contender.quote.economic);
            m_incumbentQuote = m_session->quote(m_incumbentQuote.key, m_incumbentEconomic);
            contender.comparison = m_session->compare(m_incumbentQuote, contender.quote);
            if (!contender.comparison.valid())
            {
                fail("diagnostic comparison became invalid");
                return false;
            }
            if (!contender.comparison.unresolved())
            {
                return true;
            }
            const auto target = RelativeStockDetail::widestTarget(
                m_incumbentQuote, contender.quote, contender.comparison.commonEnemyCancelled, exhausted);
            if (!target)
            {
                return true;
            }
            exhausted[static_cast<std::size_t>(*target)] = true;
            auto & quote = RelativeStockDetail::incumbentTarget(*target) ? m_incumbentQuote : contender.quote;
            if (!refineTerm(quote, RelativeStockDetail::ourTarget(*target), m_limits.maximumStateCap,
                            workLimit, phase, contender.tuple))
            {
                return m_error.isEmpty();
            }
        }
        return false;
    }

    bool readCatalog(const std::function<bool(const QJsonObject &)> & visit)
    {
        QFile file(m_catalog.fileName());
        if (!file.open(QIODevice::ReadOnly))
        {
            fail("support catalog cannot be reopened");
            return false;
        }
        while (!file.atEnd() && m_error.isEmpty())
        {
            const auto document = QJsonDocument::fromJson(file.readLine());
            if (!document.isObject())
            {
                fail("invalid support catalog record");
                return false;
            }
            if (!visit(document.object()))
            {
                return m_error.isEmpty();
            }
        }
        return m_error.isEmpty();
    }

    Contender readContender(const QJsonObject & record)
    {
        Contender contender;
        contender.support = tupleFromJson(record.value("supportSlots"));
        contender.tuple = tupleFromJson(record.value("tuple"));
        const auto initial = record.value("cheapQuote").toObject();
        ContinuationKey key;
        for (const auto & value : initial.value("key").toArray())
        {
            const auto action = value.toObject();
            key.push_back(CanonicalPlanActionKey::fromAction(action.value("row").toInt(), action.value("x").toInt(),
                action.value("y").toInt(), action.value("captures").toBool()));
        }
        contender.quote = m_session->quote(key, static_cast<MilliFunds>(initial.value("economic").toDouble()));
        contender.comparison = m_session->compare(m_incumbentQuote, contender.quote);
        if (contender.tuple.size() != m_actors.size() || contender.support.empty() ||
            contender.support.size() > Audit::MAXIMUM_SUPPORT || !contender.comparison.valid())
        {
            fail("invalid reconstructed support contender");
        }
        return contender;
    }

    bool discover(std::size_t size)
    {
        std::vector<Contender> contenders;
        readCatalog([&](const QJsonObject & record)
        {
            if (static_cast<std::size_t>(record.value("supportSlots").toArray().size()) == size)
            {
                auto contender = readContender(record);
                if (contender.comparison.deltaUpper > 0)
                {
                    contenders.push_back(std::move(contender));
                }
            }
            return true;
        });
        std::sort(contenders.begin(), contenders.end(), [](const auto & left, const auto & right)
        {
            if (left.comparison.improves() != right.comparison.improves())
            {
                return left.comparison.improves();
            }
            return relativeStockUpperFirst(left.comparison, left.tuple, right.comparison, right.tuple);
        });
        for (auto & contender : contenders)
        {
            if (!m_error.isEmpty())
            {
                return false;
            }
            refineComparison(contender, m_limits.discoveryWork, "MINIMUM_SUPPORT");
            if (contender.comparison.improves())
            {
                const auto trial = Audit::makeTrial(m_input, m_incumbent, m_actors, contender.support, contender.tuple);
                if (!trial.valid || !replayRelativeStockProof(*m_session, m_incumbentQuote, contender.quote,
                        m_session->quote(m_incumbent.plan, m_incumbentEconomic),
                        m_session->quote(trial.assignment.plan, trial.economic)))
                {
                    fail("positive diagnostic support failed full replay");
                    return false;
                }
                std::cout << "SUPPORT_POSITIVE size=" << size << " lower=" << contender.comparison.deltaLower
                          << " upper=" << contender.comparison.deltaUpper << " work=" << spent() << std::endl;
                return true;
            }
        }
        return false;
    }

    QVariantList targeted()
    {
        struct Evaluation
        {
            Audit::Target target;
            Audit::Trial trial;
            std::int64_t workAdded{0};
        };
        std::vector<Evaluation> evaluations;
        for (const auto & target : m_targets)
        {
            if (!m_error.isEmpty())
            {
                break;
            }
            if (target.tuple.size() != m_incumbentTuple.size())
            {
                fail("targeted tuple size differs from the complete incumbent");
                break;
            }
            const auto support = supportOf(target.tuple);
            Audit::Trial trial;
            if (target.tuple == m_incumbentTuple)
            {
                trial = {m_incumbent, m_incumbentEconomic, true};
            }
            else
            {
                trial = Audit::makeTrial(m_input, m_incumbent, m_actors, support, target.tuple);
            }
            if (!trial.valid)
            {
                evaluations.push_back({target, std::move(trial), 0});
                continue;
            }
            Contender contender{support, target.tuple, m_session->quote(trial.assignment.plan, trial.economic), {}};
            const auto before = spent();
            if (target.tuple != m_incumbentTuple)
            {
                refineComparison(contender, m_limits.totalWork, "TARGETED_FULL_PLAN");
            }
            evaluations.push_back({target, std::move(trial), spent() - before});
        }
        QVariantList result;
        m_incumbentQuote = m_session->quote(m_incumbent.plan, m_incumbentEconomic);
        for (const auto & evaluation : evaluations)
        {
            const auto & target = evaluation.target;
            const auto & trial = evaluation.trial;
            if (!trial.valid)
            {
                result.push_back(QVariantMap{{"name", target.name}, {"tuple", indices(target.tuple)},
                                             {"legal", false}, {"diagnosticOnly", true}});
                continue;
            }
            const auto support = supportOf(target.tuple);
            Contender contender{support, target.tuple, m_session->quote(trial.assignment.plan, trial.economic), {}};
            contender.comparison = m_session->compare(m_incumbentQuote, contender.quote);
            const bool witnesses = m_session->replayOurWitness(m_incumbentQuote) &&
                m_session->replayOurWitness(contender.quote) && (contender.comparison.commonEnemyCancelled ||
                (m_session->replayEnemyWitness(m_incumbentQuote) && m_session->replayEnemyWitness(contender.quote)));
            result.push_back(QVariantMap{
                {"name", target.name}, {"tuple", indices(target.tuple)}, {"support", supportActors(support)},
                {"legal", true}, {"incumbent", quoteReceipt(m_incumbentQuote)}, {"challenger", quoteReceipt(contender.quote)},
                {"comparison", comparisonReceipt(contender.comparison)}, {"witnessesReplay", witnesses},
                {"completePlanReplay", trial.valid}, {"workAdded", static_cast<qint64>(evaluation.workAdded)},
                {"challengerSharedWork", workReceipt(workFor(m_ourWork, contender.quote.key))},
                {"incumbentSharedWork", workReceipt(workFor(m_ourWork, m_incumbentQuote.key))},
                {"diagnosticOnly", true}, {"installed", false},
            });
            if (!witnesses || !contender.comparison.valid())
            {
                fail("targeted full-plan valuation failed witness replay");
            }
        }
        return result;
    }

    void finishCatalog(QVariantMap & result)
    {
        QFile output(m_output + "-final.jsonl");
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        {
            fail("final support catalog already exists or is unwritable");
            return;
        }
        m_incumbentQuote = m_session->quote(m_incumbent.plan, m_incumbentEconomic);
        const bool incumbentWitness = m_session->replayOurWitness(m_incumbentQuote);
        QVariantList positives;
        std::int32_t minimum = 0;
        readCatalog([&](const QJsonObject & record)
        {
            auto contender = readContender(record);
            const auto trial = Audit::makeTrial(m_input, m_incumbent, m_actors, contender.support, contender.tuple);
            if (!trial.valid || identity(trial.assignment, contender.quote) !=
                record.value("completePlanIdentity").toString().toLatin1() ||
                !sameRelativeStockQuote(contender.quote, m_session->quote(trial.assignment.plan, trial.economic)))
            {
                fail("final complete-plan identity or replay mismatch");
                return false;
            }
            const bool witnesses = incumbentWitness &&
                m_session->replayOurWitness(contender.quote) && (contender.comparison.commonEnemyCancelled ||
                (m_session->replayEnemyWitness(m_incumbentQuote) && m_session->replayEnemyWitness(contender.quote)));
            if (!witnesses)
            {
                fail("final support witness replay failed");
                return false;
            }
            auto & counts = m_counts[contender.support.size()];
            if (contender.comparison.improves())
            {
                ++counts.positive;
                if (minimum == 0 || contender.support.size() < static_cast<std::size_t>(minimum))
                {
                    minimum = static_cast<std::int32_t>(contender.support.size());
                }
                if (positives.size() < MAX_POSITIVE_EXAMPLES)
                {
                    positives.push_back(QVariantMap{
                        {"support", supportActors(contender.support)}, {"tuple", indices(contender.tuple)},
                        {"comparison", comparisonReceipt(contender.comparison)},
                    });
                }
            }
            else if (contender.comparison.unresolved())
            {
                ++counts.unresolved;
            }
            else
            {
                ++counts.nonImproving;
            }
            auto final = record.toVariantMap();
            final.insert("strongestQuote", quoteReceipt(contender.quote));
            final.insert("strongestComparison", comparisonReceipt(contender.comparison));
            final.insert("witnessesReplay", witnesses);
            final.insert("challengerSharedWork", workReceipt(workFor(m_ourWork, contender.quote.key)));
            final.insert("challengerEnemySharedWork", workReceipt(workFor(m_enemyWork, contender.quote.capturedColumns)));
            final.insert("incumbentSharedWork", workReceipt(workFor(m_ourWork, m_incumbentQuote.key)));
            final.insert("incumbentEnemySharedWork", workReceipt(workFor(m_enemyWork, m_incumbentQuote.capturedColumns)));
            final.insert("completePlanReplay", true);
            if (!writeRecord(output, final))
            {
                fail("final support catalog write failed");
                return false;
            }
            return true;
        });
        if (!output.flush())
        {
            fail("final catalog flush failed");
        }
        QVariantList domains;
        for (std::size_t size = 1; size < m_counts.size(); ++size)
        {
            const auto & counts = m_counts[size];
            domains.push_back(QVariantMap{
                {"size", static_cast<qint64>(size)}, {"supports", static_cast<qint64>(counts.supports)},
                {"productionProductEligible", static_cast<qint64>(counts.productionEligible)},
                {"rawLeaves", static_cast<qint64>(counts.raw)}, {"claimableLeaves", static_cast<qint64>(counts.claimable)},
                {"uniqueCompletePlans", static_cast<qint64>(counts.unique)},
                {"duplicateCompletePlans", static_cast<qint64>(counts.duplicates)},
                {"positive", static_cast<qint64>(counts.positive)}, {"nonImproving", static_cast<qint64>(counts.nonImproving)},
                {"unresolved", static_cast<qint64>(counts.unresolved)},
            });
        }
        result.insert("domains", domains);
        result.insert("minimumCertifiedSupport", minimum);
        result.insert("positiveExamples", positives);
        result.insert("classification", minimum > 0 && minimum <= 2 ? "POSITIVE_SINGLE_OR_PAIR_EXISTS" :
            minimum > 2 ? "PAIR_NEIGHBORHOOD_INSUFFICIENT" : "MODEL_PREFERS_WAIT_OR_REMAINS_UNRESOLVED");
        result.insert("incumbentTuple", indices(m_incumbentTuple));
        result.insert("allDomainsFullyEnumerated", m_error.isEmpty());
        result.insert("productionSelectionChanged", false);
    }

    AssignmentInput m_input;
    PropertyStockField m_field;
    const AssignmentResult & m_incumbent;
    QString m_output;
    Audit::Limits m_limits;
    const std::vector<Audit::Target> & m_targets;
    JointPlanStockValuer m_valuer;
    RefinementLedger m_ledger;
    RelativeStockSession* m_session{nullptr};
    QFile m_catalog;
    QFile m_steps;
    QString m_error;
    std::vector<ActorProgress> m_actors;
    std::vector<std::int32_t> m_incumbentTuple;
    std::vector<std::int32_t> m_stockSlots;
    std::vector<std::vector<std::int32_t>> m_options;
    std::array<DomainCounts, Audit::MAXIMUM_SUPPORT + 1> m_counts{};
    QSet<QByteArray> m_unique;
    std::map<ContinuationKey, TermWork> m_ourWork;
    std::map<std::vector<std::int32_t>, TermWork> m_enemyWork;
    RelativeStockQuote m_cheapIncumbent;
    RelativeStockQuote m_incumbentQuote;
    MilliFunds m_incumbentEconomic{0};
    std::int64_t m_mandatoryTariff{0};
    std::int64_t m_initialBalance{0};
    std::int64_t m_states{0};
    std::int64_t m_enemyStates{0};
};
}

bool ArenaRelativeStockSupportAudit::load(const QJsonObject & object, const QString & directory, QString & error)
{
    const auto name = object.value("outputPrefix").toString();
    static const QRegularExpression safeName(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$"));
    constexpr std::int64_t MAX_DIAGNOSTIC_WORK = 1000000000000;
    constexpr std::int64_t MAX_DIAGNOSTIC_STATE_CAP = 100000000;
    Limits limits;
    if (!safeName.match(name).hasMatch() ||
        !readInteger(object.value("workLimit"), MAX_DIAGNOSTIC_WORK, limits.totalWork) ||
        !readInteger(object.value("discoveryWorkLimit"), limits.totalWork, limits.discoveryWork) ||
        !readInteger(object.value("maximumStateCap"), MAX_DIAGNOSTIC_STATE_CAP, limits.maximumStateCap) ||
        !readInteger(object.value("incumbentStateCap"), MAX_DIAGNOSTIC_STATE_CAP, limits.incumbentStateCap))
    {
        error = "support audit requires a safe output prefix and positive integral diagnostic limits";
        return false;
    }
    std::vector<Target> targets;
    const auto values = object.value("targets").toArray();
    QSet<QString> names;
    if (values.size() > MAX_TARGETS)
    {
        error = "support audit accepts at most 32 explicit full-plan targets";
        return false;
    }
    for (const auto & value : values)
    {
        const auto target = value.toObject();
        Target request{target.value("name").toString(), tupleFromJson(target.value("tuple"))};
        if (!value.isObject() || request.name.isEmpty() || request.name.size() > 80 || names.contains(request.name) ||
            request.tuple.empty())
        {
            error = "invalid support-audit full-plan target";
            return false;
        }
        names.insert(request.name);
        targets.push_back(std::move(request));
    }
    std::sort(targets.begin(), targets.end(), [](const auto & left, const auto & right)
    {
        return left.tuple < right.tuple;
    });
    m_outputPath = QDir(directory).absoluteFilePath(name);
    m_limits = limits;
    m_targets = std::move(targets);
    return true;
}

QVariantMap ArenaRelativeStockSupportAudit::run(const Coordinator::AssignmentInput & input,
    const Coordinator::PropertyStockField & field, const Coordinator::AssignmentResult & incumbent) const
{
    return SupportRun(input, field, incumbent, m_outputPath, m_limits, m_targets).execute();
}
