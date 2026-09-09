#include <algorithm>
#include <array>
#include <limits>

#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include "ai/coreai.h"
#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/masterassignmentstock.h"
#include "ai/coordinator/masterassignmentshadow.h"
#include "ai/coordinator/masterassignmenttrace.h"
#include "ai/coordinator/mobilityfieldcache.h"
#include "ai/coordinator/propertyeconomicsbuilder.h"
#include "coreengine/memorymanagement.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "tests/arena/arenatestsupport.h"
#include "tests/arena/masterassignmentprobe.h"

namespace
{
    using namespace Coordinator;
    constexpr qint32 OBSERVER_PLAYER = 0;
    constexpr qint32 ENEMY_PLAYER = 1;
    constexpr qint32 EXACT_SPAWN_RANGE = 0;
    constexpr bool IGNORE_SPAWN_MOVEMENT = true;
    constexpr bool MAP_ONLY_LOAD = true;
    constexpr bool FAST_MAP_LOAD = false;
    constexpr bool SAVEGAME_LOAD = false;
    constexpr std::int64_t FIXTURE_ALLOWANCE = 1000000;
    constexpr std::int64_t AFFORDABLE_REFINEMENT = 4096;

    class MemoryShadowInspection final : public MasterShadowInspection
    {
    public:
        void completed(const MasterShadowObservation & observation) override
        {
            ++calls;
            record = ArenaMasterAssignmentProbe::inspectShadow(observation);
        }

        std::int32_t calls{0};
        QJsonObject record;
    };

    QVariantMap checkShadowExports(const PropertyStockField & field)
    {
        QTemporaryDir outputDirectory;
        if (!outputDirectory.isValid())
        {
            return {{"ok", false}, {"error", QStringLiteral("could not isolate shadow-export fixture")}};
        }
        bool valid = true;
        std::int32_t observations = 0;
        for (const auto audit : {QByteArray("full"), QByteArray("counters")})
        {
            for (const auto allowance : std::array<std::int64_t, 4>{0, 1, 2, FIXTURE_ALLOWANCE})
            {
                std::optional<QJsonObject> expected;
                for (const auto exportMode : {QByteArray("full"), QByteArray("off")})
                {
                    const auto path = outputDirectory.path() + QStringLiteral("/observation-%1.jsonl").arg(observations);
                    const auto config = parseMasterShadowConfig("shadow", QByteArray::number(allowance), path, audit, exportMode);
                    AssignmentInput input;
                    RefinementLedger ledger;
                    RefinementLedger legacy;
                    valid = legacy.open(0) && config.enabled() && valid;
                    prepareMasterShadow(config, input, ledger);
                    const auto incumbent = MaximumValueAssignment::assign(input);
                    MemoryShadowInspection inspection;
                    const auto measured = observeMasterAssignment(config, input, incumbent, field, legacy, ledger,
                        {.day = 1, .playerId = OBSERVER_PLAYER, .planSequence = 1,
                         .horizonTurns = DEFAULT_ECONOMIC_RELEVANCE_HORIZON}, &inspection);
                    auto record = inspection.record.value("record").toObject();
                    record.remove("profile");
                    valid = valid && inspection.calls == 1 &&
                        inspection.record.value("ownedSnapshotsVerified").toBool() &&
                        inspection.record.value("workUnchanged").toBool() &&
                        measured.legacyPlanUnchanged && measured.legacyLedgerUnchanged;
                    if (expected)
                    {
                        valid = valid && *expected == record;
                    }
                    else
                    {
                        expected = record;
                    }
                    if (exportMode == "off")
                    {
                        valid = valid && measured.exportRecords == 0 && measured.exportBytes == 0 &&
                            measured.recordNanos == 0 && measured.encodingNanos == 0 &&
                            measured.outputNanos == 0 && !QFile::exists(path);
                        AssignmentInput quietInput;
                        RefinementLedger quietLedger;
                        prepareMasterShadow(config, quietInput, quietLedger);
                        const auto quietIncumbent = MaximumValueAssignment::assign(quietInput);
                        const auto quiet = observeMasterAssignment(config, quietInput, quietIncumbent, field,
                            legacy, quietLedger, {.day = 1, .playerId = OBSERVER_PLAYER, .planSequence = 1,
                                                  .horizonTurns = DEFAULT_ECONOMIC_RELEVANCE_HORIZON});
                        valid = valid && quietLedger.stats() == ledger.stats() && quiet.outcome == measured.outcome &&
                            quiet.firstImprovementWork == measured.firstImprovementWork &&
                            quiet.improvementLower == measured.improvementLower && quiet.gap == measured.gap &&
                            quiet.inspectionNanos == 0 && quiet.exportRecords == 0 && quiet.exportBytes == 0 &&
                            quiet.recordNanos == 0 && quiet.encodingNanos == 0 &&
                            quiet.outputNanos == 0 && !QFile::exists(path);
                    }
                    else
                    {
                        QFile file(path);
                        valid = valid && measured.exportRecords == 1 && measured.exportBytes > 0 &&
                            measured.exportSucceeded && file.open(QIODevice::ReadOnly);
                        auto exported = QJsonDocument::fromJson(file.readAll()).object();
                        exported.remove("profile");
                        exported.remove("observeNanosBeforeWrite");
                        valid = valid && exported == record;
                    }
                    ++observations;
                }
            }
        }
        AssignmentInput input;
        AssignmentResult incumbent;
        RefinementLedger legacy;
        RefinementLedger ledger;
        const auto before = ledger.stats();
        MemoryShadowInspection disabledInspection;
        const auto disabled = observeMasterAssignment({}, input, incumbent, field, legacy, ledger, {}, &disabledInspection);
        valid = valid && disabled.totalNanos == 0 && disabled.exportRecords == 0 &&
            disabledInspection.calls == 0 && ledger.stats() == before;
        return {{"ok", valid}, {"observations", observations}, {"disabledNeutral", disabledInspection.calls == 0}};
    }

    std::optional<PropertyStockField> buildField(GameMap & map, QString & error)
    {
        auto* observer = map.getPlayer(OBSERVER_PLAYER);
        auto* enemy = map.getPlayer(ENEMY_PLAYER);
        if (!observer || !enemy || !observer->isEnemy(enemy) || !map.getGameRules())
        {
            error = QStringLiteral("invalid fixture players");
            return std::nullopt;
        }
        map.getGameRules()->setFogMode(GameEnums::Fog_Off);
        auto knowledge = BattlefieldKnowledge::capture(map, *observer);
        if (!knowledge.units().empty())
        {
            error = QStringLiteral("fixture map must initially contain no units");
            return std::nullopt;
        }
        auto properties = buildPropertyEconomics(map, knowledge);
        const auto property = std::find_if(properties.begin(), properties.end(), [](const PropertyFacts & facts) {
            return facts.capturable && facts.ownerId == NO_OWNER && facts.baseIncome > 0;
        });
        if (property == properties.end())
        {
            error = QStringLiteral("fixture map has no income-producing neutral property");
            return std::nullopt;
        }
        if (!map.spawnUnit(property->x, property->y, QString(CoreAI::UNIT_INFANTRY),
                          enemy, EXACT_SPAWN_RANGE, IGNORE_SPAWN_MOVEMENT))
        {
            error = QStringLiteral("enemy capture fixture could not be constructed");
            return std::nullopt;
        }
        knowledge = BattlefieldKnowledge::capture(map, *observer);
        if (knowledge.units().size() != 1 || !knowledge.units().front().canCapture ||
            knowledge.relation(knowledge.units().front().ownerId) != Relation::Enemy)
        {
            error = QStringLiteral("fixture must contain only one enemy capturer");
            return std::nullopt;
        }
        properties = buildPropertyEconomics(map, knowledge);
        MobilityFieldCache mobility;
        return buildPropertyStockField(map, knowledge, properties, mobility, DEFAULT_ECONOMIC_RELEVANCE_HORIZON);
    }

    QVariantMap checkAdmission(const PropertyStockField & field)
    {
        AssignmentInput input;
        AssignmentResult incumbent;
        const auto extraction = buildMasterAssignmentProblem(input, incumbent, 1);
        RefinementLedger ledger;
        MasterAssignmentStockContext stock;
        if (!extraction.valid() || !ledger.open(FIXTURE_ALLOWANCE) ||
            stock.prepare(field, input, ledger) != MasterStockSetupStatus::Prepared)
        {
            return {{"ok", false}, {"error", QStringLiteral("native stock setup failed")}};
        }
        const auto & problem = *extraction.problem;
        MasterAssignmentEvaluator evaluator(problem, ledger, *stock.session(), stock.tariffs());
        auto evaluation = evaluator.evaluate({problem.identity(), {}});
        const auto* quote = evaluation.quote();
        if (!quote || !evaluation.lowerReplayed || quote->ours.lower != 0 || quote->ours.upper != 0 ||
            quote->enemy.exact() || quote->ourUpperCertificateId != 0)
        {
            return {{"ok", false}, {"error", QStringLiteral("native exact-friendly/open-enemy precondition failed")},
                {"evaluation", masterEvaluationRecord(evaluation).toVariantMap()}};
        }
        const auto initial = masterEvaluationRecord(evaluation);
        const auto proof = evaluation.proof;
        const auto before = ledger.stats().spent();
        const auto affordable = evaluator.refine(evaluation, MasterRefinementTerm::FutureDual, AFFORDABLE_REFINEMENT);
        const auto oversized = evaluator.refine(evaluation, MasterRefinementTerm::FutureDual,
                                                std::numeric_limits<std::int64_t>::max());
        const bool unchanged = affordable == MasterRefinementStatus::Unavailable &&
            oversized == MasterRefinementStatus::Unavailable && ledger.stats().spent() == before &&
            evaluation.proof == proof;
        const bool healthy = evaluator.refresh(evaluation);
        const auto enemy = evaluator.refine(evaluation, MasterRefinementTerm::Enemy, AFFORDABLE_REFINEMENT);
        const bool enemyHealthy = enemy == MasterRefinementStatus::Updated && evaluator.refresh(evaluation);

        RefinementLedger controlLedger;
        MasterAssignmentStockContext control;
        bool directCallFailsClosed = false;
        if (controlLedger.open(FIXTURE_ALLOWANCE) &&
            control.prepare(field, input, controlLedger) == MasterStockSetupStatus::Prepared)
        {
            const auto fresh = control.session()->quote(ContinuationKey{}, 0);
            const auto direct = control.session()->tightenOurUpperWithVerifiedDual(fresh, AFFORDABLE_REFINEMENT);
            directCallFailsClosed = fresh.valid && fresh.ours.exact() && !fresh.enemy.exact() &&
                direct.globalFailure && !direct.certificateValid;
        }
        const auto exports = checkShadowExports(field);
        return {{"ok", unchanged && healthy && enemyHealthy && directCallFailsClosed && exports.value("ok").toBool()},
            {"shadowExports", exports},
            {"initial", initial.toVariantMap()}, {"final", masterEvaluationRecord(evaluation).toVariantMap()},
            {"affordableStatus", static_cast<int>(affordable)}, {"oversizedStatus", static_cast<int>(oversized)},
            {"proofAndWorkUnchanged", unchanged}, {"sessionHealthy", healthy}, {"enemyRefinementHealthy", enemyHealthy},
            {"directUnadmittedNativeCallFailsClosed", directCallFailsClosed},
            {"masterWork", masterWorkRecord(ledger.stats()).toVariantMap()},
            {"separateNativeControlWork", masterWorkRecord(controlLedger.stats()).toVariantMap()}};
    }

    QVariantMap runAdmission(QObject*, const QVariantMap & arguments)
    {
        const auto path = arguments.value(QStringLiteral("mapPath")).toString();
        if (path.isEmpty())
        {
            return {{"ok", false}, {"error", QStringLiteral("missing fixture map")}};
        }
        const auto map = MemoryManagement::create<GameMap>(path, MAP_ONLY_LOAD, FAST_MAP_LOAD, SAVEGAME_LOAD);
        QString error;
        const auto field = buildField(*map, error);
        return field ? checkAdmission(*field) : QVariantMap{{"ok", false}, {"error", error}};
    }

    [[maybe_unused]] const bool REGISTERED = AiArenaTestSupport::registerOperation(
        QStringLiteral("masterStockAdmission"), runAdmission);
}
