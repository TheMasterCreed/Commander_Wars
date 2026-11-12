#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>

#include "3rd_party/oxygine-framework/oxygine/actor/Stage.h"
#include "ai/coreai.h"
#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/guardedcommandpolicy.h"
#include "ai/coordinator/guardednativeinventory.h"
#include "ai/coordinator/guardednativepresentation.h"
#include "ai/coordinator/masterassignmentstock.h"
#include "ai/coordinator/masterassignmentshadow.h"
#include "ai/coordinator/masterassignmenttrace.h"
#include "ai/coordinator/masterassignmentinstallation.h"
#include "ai/coordinator/masternativesource.h"
#include "ai/coordinator/mobilityfieldcache.h"
#include "ai/coordinator/propertyeconomicsbuilder.h"
#include "coreengine/memorymanagement.h"
#include "coreengine/interpreter.h"
#include "coreengine/mainapp.h"
#include "coreengine/settings.h"
#include "game/building.h"
#include "game/gameanimation/gameanimationfactory.h"
#include "game/gamemap.h"
#include "game/victoryrule.h"
#include "coreengine/scriptvariables.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"
#include "game/unitpathfindingsystem.h"
#include "game/weather.h"
#include "gameinput/basegameinputif.h"
#include "menue/gamemenue.h"
#include "tests/arena/arenatestsupport.h"
#include "tests/arena/masterassignmentprobe.h"
#include "tests/arena/relativestockreceipts.h"

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
    constexpr std::int64_t OPERATION_FIXTURE_ALLOWANCE = 5000000;
    constexpr std::int64_t INITIAL_OPERATION_STATES = 128;
    constexpr std::int64_t OPERATION_GROWTH = 4;
    constexpr std::int32_t MAXIMUM_OPERATION_ATTEMPTS = 8;
    constexpr std::size_t OPERATION_FRIENDLY_ROWS = 2;
    constexpr qint32 PATH_COST_BOUND_MARGIN = 1;

    struct SharedRowCatalogFixture
    {
        AssignmentInput input;
        ContinuationKey first;
        ContinuationKey second;
        ContinuationKey combined;
        ContinuationKey unadmitted;
    };

    struct CatalogRefinementResult
    {
        std::int32_t attempts{0};
        std::uint64_t revision{0};
        bool complete{false};
        bool earlyCertificateWithoutRows{false};
        bool admitted{false};
        RelativeStockQuote quote;
    };

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

    std::optional<PropertyStockField> buildField(GameMap & map, QString & error, bool includeFriendly = false)
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
        for (std::size_t friendly = 0; includeFriendly && friendly < OPERATION_FRIENDLY_ROWS; ++friendly)
        {
            const auto friendlyProperty = std::find_if(properties.begin(), properties.end(), [&](const PropertyFacts & facts) {
                return facts.capturable && facts.ownerId == NO_OWNER && facts.baseIncome > 0 &&
                    map.getTerrain(facts.x, facts.y)->getUnit() == nullptr;
            });
            if (friendlyProperty == properties.end() ||
                !map.spawnUnit(friendlyProperty->x, friendlyProperty->y, QString(CoreAI::UNIT_INFANTRY),
                               observer, EXACT_SPAWN_RANGE, IGNORE_SPAWN_MOVEMENT))
            {
                error = QStringLiteral("friendly capture fixture could not be constructed");
                return std::nullopt;
            }
        }
        knowledge = BattlefieldKnowledge::capture(map, *observer);
        const std::size_t expectedUnits = includeFriendly ? OPERATION_FRIENDLY_ROWS + 1 : 1;
        const auto enemies = std::count_if(knowledge.units().begin(), knowledge.units().end(), [&](const KnownUnit & unit) {
            return unit.canCapture && knowledge.relation(unit.ownerId) == Relation::Enemy;
        });
        if (knowledge.units().size() != expectedUnits || enemies != 1)
        {
            error = QStringLiteral("fixture must contain only one enemy capturer");
            return std::nullopt;
        }
        properties = buildPropertyEconomics(map, knowledge);
        MobilityFieldCache mobility;
        return buildPropertyStockField(map, knowledge, properties, mobility, DEFAULT_ECONOMIC_RELEVANCE_HORIZON);
    }

    std::optional<SharedRowCatalogFixture> sharedRowFixture(GameMap & map, const PropertyStockField & field)
    {
        auto* observer = map.getPlayer(OBSERVER_PLAYER);
        const auto knowledge = BattlefieldKnowledge::capture(map, *observer);
        SharedRowCatalogFixture result;
        result.input.actionIds = engineActionIds();
        ContinuationKey moved;
        std::vector<TilePoint> destinations;
        for (const auto & known : knowledge.units())
        {
            auto* unit = liveUnitFor(map, known);
            if (unit == nullptr)
            {
                return std::nullopt;
            }
            result.input.unitLinks.push_back({unit->getUniqueID(), {known.x, known.y}});
        }
        for (std::size_t index = 0; index < knowledge.units().size(); ++index)
        {
            const auto & known = knowledge.units()[index];
            if (knowledge.relation(known.ownerId) != Relation::Own)
            {
                continue;
            }
            const auto row = field.ourRowOf(static_cast<std::int32_t>(index));
            auto* unit = liveUnitFor(map, known);
            if (row == NO_STOCK_ROW || unit == nullptr ||
                known.movementPoints > std::numeric_limits<qint32>::max() - PATH_COST_BOUND_MARGIN)
            {
                return std::nullopt;
            }
            UnitPathFindingSystem pathfinder(&map, unit, observer);
            pathfinder.explore();
            auto points = pathfinder.getAllNodePointsFast(known.movementPoints + PATH_COST_BOUND_MARGIN);
            std::sort(points.begin(), points.end(), [](const QPoint & left, const QPoint & right) {
                return std::pair{left.x(), left.y()} < std::pair{right.x(), right.y()};
            });
            CandidateBundle stay;
            stay.bundle.unitId = static_cast<std::int32_t>(index);
            stay.bundle.origin = {known.x, known.y};
            stay.bundle.destination = stay.bundle.origin;
            stay.bundle.path.push_back(stay.bundle.origin);
            std::optional<CandidateBundle> move;
            for (const auto & point : points)
            {
                const TilePoint destination{point.x(), point.y()};
                auto* terrain = map.getTerrain(point.x(), point.y());
                if (terrain == nullptr || terrain->getUnit() != nullptr ||
                    std::find(destinations.begin(), destinations.end(), destination) != destinations.end())
                {
                    continue;
                }
                const auto path = pathfinder.getPathFast(point.x(), point.y());
                if (path.empty())
                {
                    continue;
                }
                const auto cost = pathfinder.getCosts(path);
                if (cost < 0 || cost > known.movementPoints)
                {
                    continue;
                }
                move = stay;
                move->bundle.destination = destination;
                move->bundle.path = pathFromEngineOrder(path);
                move->movementCost = cost;
                destinations.push_back(destination);
                break;
            }
            if (!move)
            {
                return std::nullopt;
            }
            result.input.actors.push_back({static_cast<std::int32_t>(index), unit->getUniqueID(), {stay, *move}});
            result.unadmitted.push_back(CanonicalPlanActionKey::fromAction(row, known.x, known.y, false));
            moved.push_back(CanonicalPlanActionKey::fromAction(
                row, move->bundle.destination.x, move->bundle.destination.y, false));
        }
        if (moved.size() != OPERATION_FRIENDLY_ROWS)
        {
            return std::nullopt;
        }
        std::sort(result.unadmitted.begin(), result.unadmitted.end());
        std::sort(moved.begin(), moved.end());
        result.first = result.unadmitted;
        result.first.front() = moved.front();
        result.second = result.unadmitted;
        result.second.back() = moved.back();
        result.combined = std::move(moved);
        return result;
    }

    bool payOperationQuote(RefinementLedger & ledger, const MasterValuationTariffs & tariffs)
    {
        return ledger.consume(tariffs.quote, RefinementWork::MasterValuation);
    }

    bool payOperationDualEntry(RefinementLedger & ledger, const MasterValuationTariffs & tariffs,
                              const RelativeStockOperationMetadata & metadata,
                              const RelativeStockOperationPreparation & preparation)
    {
        auto work = metadata.certificateEntryWork;
        return RelativeStockDetail::checkedAdd(work, preparation.scanWork) &&
            RelativeStockDetail::checkedAdd(work, tariffs.certificateIdentity) &&
            RelativeStockDetail::checkedAdd(work, tariffs.witness) &&
            ledger.consume(work, RefinementWork::MasterValuation);
    }

    bool checkLocalPreparationRefusal(RelativeStockSession & session, RefinementLedger & ledger,
                                      const RelativeStockQuote & quote,
                                      const RelativeStockOperationMetadata & metadata)
    {
        auto available = metadata.certificateAdmissionWork;
        if (!RelativeStockDetail::checkedAdd(available, metadata.certificateReadinessWork) ||
            ledger.balance() <= available)
        {
            return false;
        }
        const auto heldWork = ledger.balance() - available;
        const auto held = ledger.draw(heldWork, RefinementWork::MasterInstallation);
        if (held == NO_GRANT)
        {
            return false;
        }
        const auto before = ledger.stats();
        const auto preparation = session.prepareOurCertificateOperation(quote);
        const bool localRefusal = preparation.status == RelativeStockPreparationStatus::BudgetExhausted &&
            preparation.readinessChecked &&
            preparation.paidWork > 0 && preparation.paidWork == ledger.stats().spent() - before.spent() &&
            preparation.requiredPreparationWork > ledger.balance() &&
            ledger.stats().refusals == before.refusals && session.operationMetadata(quote).valid;
        return ledger.refund(held, heldWork) && localRefusal;
    }

    CatalogRefinementResult completeSharedRows(RelativeStockSession & session, RefinementLedger & ledger,
                                               const MasterValuationTariffs & tariffs, const ContinuationKey & key,
                                               std::uint64_t requiredRevision, std::vector<ContinuationKey> & nativeCalls)
    {
        CatalogRefinementResult result;
        auto stateCap = INITIAL_OPERATION_STATES;
        while (result.attempts < MAXIMUM_OPERATION_ATTEMPTS)
        {
            if (!payOperationQuote(ledger, tariffs))
            {
                return result;
            }
            nativeCalls.push_back(key);
            const auto refined = session.refineOur(key, stateCap);
            result.admitted = result.admitted || refined.admitted;
            ++result.attempts;
            if (!refined.valid || !refined.admitted || refined.statesUsed < 0 ||
                refined.statesUsed > stateCap || !payOperationQuote(ledger, tariffs))
            {
                return result;
            }
            auto quote = session.quote(key, 0);
            const auto metadata = session.operationMetadata(quote);
            if (!quote.valid || !quote.capturedColumns.empty() || !metadata.valid)
            {
                return result;
            }
            result.revision = metadata.rowCacheRevision;
            result.quote = std::move(quote);
            result.complete = result.revision == requiredRevision;
            if (result.complete)
            {
                return result;
            }
            if (refined.completed || result.revision > requiredRevision)
            {
                result.earlyCertificateWithoutRows = refined.completed;
                return result;
            }
            if (stateCap > std::numeric_limits<std::int64_t>::max() / OPERATION_GROWTH)
            {
                return result;
            }
            stateCap *= OPERATION_GROWTH;
        }
        return result;
    }

    QVariantMap checkOperationService(const PropertyStockField & field, const SharedRowCatalogFixture & fixture)
    {
        RefinementLedger ledger;
        MasterAssignmentStockContext stock;
        if (!ledger.open(OPERATION_FIXTURE_ALLOWANCE) ||
            stock.prepare(field, fixture.input, ledger) != MasterStockSetupStatus::Prepared)
        {
            return {{"ok", false}, {"error", QStringLiteral("operation-service native setup failed")}};
        }
        auto* session = stock.session();
        const auto tariffs = stock.tariffs();
        if (!payOperationQuote(ledger, tariffs))
        {
            return {{"ok", false}, {"error", QStringLiteral("operation-service initial quote refused")}};
        }
        auto quote = session->quote(fixture.combined, 0);
        const auto beforeEnable = ledger.stats();
        const bool defaultOff = quote.valid && !session->operationMetadata(quote).valid &&
            session->prepareOurCertificateOperation(quote).status == RelativeStockPreparationStatus::Invalid &&
            ledger.stats() == beforeEnable;
        if (!ledger.consume(tariffs.witness, RefinementWork::MasterValuation))
        {
            return {{"ok", false}, {"error", QStringLiteral("operation-service tariff reference refused")}};
        }
        const auto coldOur = session->ourRerunWork();
        const auto coldEnemy = session->enemyRerunWork(quote.capturedColumns);
        const auto enableWork = ledger.stats();
        const bool enabled = stock.prepareOperationService() == MasterStockSetupStatus::Prepared &&
            ledger.stats() == enableWork;
        auto metadata = session->operationMetadata(quote);
        bool valid = defaultOff && enabled && metadata.valid && metadata.identity.sameLiveModel(quote.identity) &&
            metadata.ourRerunWork == coldOur && metadata.enemyRerunWork == coldEnemy;
        auto foreign = quote;
        foreign.identity = {};
        const auto beforeForeign = ledger.stats();
        valid = valid && !session->operationMetadata(foreign).valid &&
            session->prepareOurCertificateOperation(foreign).status == RelativeStockPreparationStatus::Invalid &&
            ledger.stats() == beforeForeign;

        const auto beforeReadiness = ledger.stats().spent();
        auto preparation = session->prepareOurCertificateOperation(quote);
        const bool coldMissing = preparation.status == RelativeStockPreparationStatus::MissingRows &&
            preparation.paidWork > 0 && preparation.paidWork == ledger.stats().spent() - beforeReadiness &&
            preparation.rowCacheRevision == 0;
        valid = valid && coldMissing && quote.capturedColumns.empty();
        CatalogRefinementResult first;
        CatalogRefinementResult second;
        std::vector<ContinuationKey> nativeCalls;
        bool missingAfterFirst = false;
        if (valid)
        {
            first = completeSharedRows(*session, ledger, tariffs, fixture.first, OPERATION_FRIENDLY_ROWS, nativeCalls);
            valid = first.complete && payOperationQuote(ledger, tariffs);
            if (valid)
            {
                quote = session->quote(fixture.combined, 0);
                const auto before = ledger.stats().spent();
                preparation = session->prepareOurCertificateOperation(quote);
                missingAfterFirst = preparation.status == RelativeStockPreparationStatus::MissingRows &&
                    preparation.rowCacheRevision == OPERATION_FRIENDLY_ROWS &&
                    preparation.paidWork == ledger.stats().spent() - before;
                valid = missingAfterFirst;
            }
        }
        if (valid)
        {
            second = completeSharedRows(*session, ledger, tariffs, fixture.second,
                                        OPERATION_FRIENDLY_ROWS * 2, nativeCalls);
            valid = second.complete && payOperationQuote(ledger, tariffs);
            if (valid)
            {
                quote = session->quote(fixture.combined, 0);
                const auto before = ledger.stats().spent();
                preparation = session->prepareOurCertificateOperation(quote);
                valid = quote.valid && preparation.status == RelativeStockPreparationStatus::Ready &&
                    preparation.rowCacheRevision == OPERATION_FRIENDLY_ROWS * 2 &&
                    preparation.paidWork == ledger.stats().spent() - before;
            }
        }
        const auto combinedCalls = std::count(nativeCalls.begin(), nativeCalls.end(), fixture.combined);
        const auto unadmittedCalls = std::count(nativeCalls.begin(), nativeCalls.end(), fixture.unadmitted);
        valid = valid && combinedCalls == 0 && unadmittedCalls == 0;
        QVariantMap sharedRows{{"firstCalls", first.attempts}, {"secondCalls", second.attempts},
            {"combinedCalls", static_cast<qint64>(combinedCalls)}, {"unadmittedCalls", static_cast<qint64>(unadmittedCalls)},
            {"initialRevision", static_cast<qulonglong>(metadata.rowCacheRevision)},
            {"firstRevision", static_cast<qulonglong>(first.revision)},
            {"secondRevision", static_cast<qulonglong>(second.revision)},
            {"combinedReadinessRevision", static_cast<qulonglong>(preparation.rowCacheRevision)},
            {"firstEarlyCertificateWithoutRows", first.earlyCertificateWithoutRows},
            {"secondEarlyCertificateWithoutRows", second.earlyCertificateWithoutRows},
            {"missingAfterFirst", missingAfterFirst}, {"first", ArenaStockReceipts::quoteReceipt(first.quote)},
            {"second", ArenaStockReceipts::quoteReceipt(second.quote)},
            {"combined", ArenaStockReceipts::quoteReceipt(quote)}};
        if (!valid || preparation.status != RelativeStockPreparationStatus::Ready || preparation.minimumNativeWork <= 0)
        {
            return {{"ok", false}, {"error", QStringLiteral("operation-service complete native rows unavailable")},
                {"defaultOff", defaultOff}, {"coldMissing", coldMissing}, {"sharedRows", sharedRows},
                {"readinessStatus", static_cast<int>(preparation.status)}};
        }
        if (!payOperationQuote(ledger, tariffs))
        {
            return {{"ok", false}, {"error", QStringLiteral("unadmitted shared-row quote refused")}};
        }
        const auto unadmittedQuote = session->quote(fixture.unadmitted, 0);
        const auto beforeUnadmitted = ledger.stats().spent();
        const auto sharedPreparation = session->prepareOurCertificateOperation(unadmittedQuote);
        const bool unadmittedReady = unadmittedQuote.valid && unadmittedQuote.capturedColumns.empty() &&
            sharedPreparation.status == RelativeStockPreparationStatus::Ready &&
            sharedPreparation.rowCacheRevision == preparation.rowCacheRevision &&
            sharedPreparation.paidWork == ledger.stats().spent() - beforeUnadmitted;
        sharedRows.insert("unadmitted", ArenaStockReceipts::quoteReceipt(unadmittedQuote));
        sharedRows.insert("unadmittedReadinessRevision", static_cast<qulonglong>(sharedPreparation.rowCacheRevision));
        sharedRows.insert("unadmittedReadyWithoutRowConstruction", unadmittedReady);
        valid = valid && unadmittedReady;
        metadata = session->operationMetadata(quote);
        const bool warmTariffIncreased = metadata.ourRerunWork > coldOur;
        const bool localPreparationRefusal = checkLocalPreparationRefusal(*session, ledger, quote, metadata);
        valid = valid && localPreparationRefusal && payOperationDualEntry(ledger, tariffs, metadata, preparation);
        if (!valid)
        {
            return {{"ok", false}, {"error", QStringLiteral("operation-service local preparation refusal failed")},
                {"localPreparationRefusal", localPreparationRefusal}};
        }
        const auto atMinimum = ledger.stats();
        const auto denied = session->tightenOurUpperWithVerifiedDual(quote, preparation.minimumNativeWork);
        const bool strictMinimum = denied.locallyBudgetExhausted && !denied.globalFailure &&
            !denied.admitted && !denied.completed && denied.totalWork == 0 && ledger.stats() == atMinimum;
        metadata = session->operationMetadata(quote);
        valid = valid && strictMinimum && metadata.valid &&
            payOperationDualEntry(ledger, tariffs, metadata, preparation);
        RelativeStockDualTightening entered;
        if (valid)
        {
            entered = session->tightenOurUpperWithVerifiedDual(quote, preparation.minimumNativeWork + 1);
            valid = entered.admitted && !entered.globalFailure && entered.totalWork > 0 &&
                entered.totalWork <= preparation.minimumNativeWork + 1 &&
                entered.completed == (entered.optimizerCompleted && entered.verifierCompleted && entered.certificateValid);
        }
        bool staleRejected = false;
        if (valid && payOperationQuote(ledger, tariffs))
        {
            quote = session->quote(quote.key, 0);
            metadata = session->operationMetadata(quote);
            auto stale = quote;
            if (metadata.valid && stale.ours.upper < std::numeric_limits<MilliFunds>::max())
            {
                ++stale.ours.upper;
                stale.ourSearchCompleted = false;
                const auto before = ledger.stats().spent();
                const auto rejected = session->prepareOurCertificateOperation(stale);
                staleRejected = rejected.status == RelativeStockPreparationStatus::Invalid &&
                    rejected.paidWork == metadata.certificateAdmissionWork &&
                    rejected.paidWork == ledger.stats().spent() - before &&
                    rejected.rowCacheRevision == metadata.rowCacheRevision;
            }
        }
        return {{"ok", valid && first.admitted && second.admitted && staleRejected}, {"defaultOff", defaultOff},
            {"coldMissing", coldMissing}, {"sharedRows", sharedRows},
            {"localPreparationRefusal", localPreparationRefusal},
            {"warmTariffIncreased", warmTariffIncreased}, {"strictNativeMinimum", strictMinimum},
            {"nativeAdmittedAboveMinimum", entered.admitted}, {"nativeCompleted", entered.completed},
            {"staleEntryRejectedBeforeReadiness", staleRejected},
            {"minimumNativeWork", QString::number(preparation.minimumNativeWork)},
            {"masterWork", masterWorkRecord(ledger.stats()).toVariantMap()}};
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

    QVariantMap runAdmissionFixture(const QString & path, bool operationService)
    {
        const auto map = MemoryManagement::create<GameMap>(path, MAP_ONLY_LOAD, FAST_MAP_LOAD, SAVEGAME_LOAD);
        QString error;
        const auto field = buildField(*map, error, operationService);
        if (!field)
        {
            return {{"ok", false}, {"error", error}};
        }
        if (operationService)
        {
            const auto fixture = sharedRowFixture(*map, *field);
            if (!fixture)
            {
                return {{"ok", false}, {"error", QStringLiteral("shared-row legal movement fixture unavailable")}};
            }
            return checkOperationService(*field, *fixture);
        }
        return checkAdmission(*field);
    }

    QVariantMap runAdmission(QObject*, const QVariantMap & arguments)
    {
        const auto path = arguments.value(QStringLiteral("mapPath")).toString();
        if (path.isEmpty())
        {
            return {{"ok", false}, {"error", QStringLiteral("missing fixture map")}};
        }
        auto result = runAdmissionFixture(path, false);
        if (!result.value("ok").toBool())
        {
            return result;
        }
        const auto operation = runAdmissionFixture(path, true);
        result.insert("operationService", operation);
        result.insert("ok", result.value("ok").toBool() && operation.value("ok").toBool());
        return result;
    }

    enum class NativeGuardedCase
    {
        Valid,
        ChangedProgress,
        ChangedRealm,
        InsufficientSearchFunding,
        UnsupportedFireRange,
        UnsupportedMovedSniper,
        UnsupportedActionIds,
        ChangedBattleSpeed,
        ChangedTerrainAnimation,
        DefaultArmy,
        ChangedArmySource,
        ChangedArmyTable,
        ChangedArmyCallback,
        LongerArmyEntry,
        UnlimitedAmmunition,
        ChangedUnlimitedAmmunition,
    };

    enum class NativeGuardedScreen : std::int8_t
    {
        Headless,
        Detail,
    };

    constexpr std::int64_t NATIVE_GUARDED_ALLOWANCE = 60000000;
    constexpr qint32 NATIVE_DETAIL_POLL_MS = 50;
    constexpr qint64 NATIVE_DETAIL_START_LIMIT_MS = 30000;
    constexpr qint32 NATIVE_DETAIL_SEEDED_FOREGROUND = 0;
    constexpr qint32 NATIVE_DETAIL_CHANGED_FOREGROUND = 1;
    constexpr std::size_t NATIVE_GUARDED_UNITS = 5;
    using NativeGuardedUnits = std::array<Unit*, NATIVE_GUARDED_UNITS>;
    const QString NATIVE_DETAIL_OUTPUT = QStringLiteral("master-native-detail.json");
    const QString NATIVE_DETAIL_PROGRESS = QStringLiteral("master-native-detail-progress.json");
    constexpr qint32 NATIVE_GUARDED_FUNDS = 10000;
    constexpr std::size_t NATIVE_FRIENDLY_ACTORS = 3;
    constexpr std::size_t NATIVE_FIRE_ACTOR = 2;
    constexpr std::size_t NATIVE_FIRE_TARGET = 4;
    constexpr std::size_t NATIVE_CAPTURE_ACTOR = 1;
    const QString NATIVE_GUARDED_OUTPUT = QStringLiteral("master-native-guarded.json");
    const QString NATIVE_GUARDED_PROGRESS = QStringLiteral("master-native-guarded-progress.json");
    const QString NATIVE_GUARDED_REALM_PROBE = QStringLiteral("MASTER_NATIVE_GUARDED_REALM_PROBE");

    bool usesDefaultArmy(NativeGuardedCase kind)
    {
        return kind == NativeGuardedCase::DefaultArmy || kind == NativeGuardedCase::ChangedArmySource ||
            kind == NativeGuardedCase::ChangedArmyTable || kind == NativeGuardedCase::ChangedArmyCallback ||
            kind == NativeGuardedCase::LongerArmyEntry;
    }

    spGameMap nativeGuardedMap(NativeGuardedUnits & units, NativeGuardedCase kind, bool movementOnly = false, bool lightTank = false, bool buildingCallbacks = false)
    {
        auto map = MemoryManagement::create<GameMap>(8, 4, 2);
        map->setCurrentDay(4);
        map->setCurrentPlayer(OBSERVER_PLAYER);
        map->setIsHumanMatch(false);
        for (qint32 player = 0; player < 2; ++player)
        {
            auto* owner = map->getPlayer(player);
            const auto type = player == OBSERVER_PLAYER
                ? GameEnums::AiTypes_Coordinated : GameEnums::AiTypes_Normal;
            owner->setBaseGameInput(BaseGameInputIF::createAi(map.get(), type));
            owner->setControlType(type);
            owner->setTeam(player);
            owner->setFunds(NATIVE_GUARDED_FUNDS);
            if (usesDefaultArmy(kind))
            {
                owner->updatePlayerID();
                if (!owner->getPlayerArmy().isEmpty())
                {
                    return {};
                }
            }
            else
            {
                owner->setCO(player == OBSERVER_PLAYER ? QStringLiteral("CO_ANDY") : QStringLiteral("CO_JESS"), 0);
                owner->defineArmy();
                if (owner->getPlayerArmy().isEmpty())
                {
                    return {};
                }
            }
            owner->loadVisionFields();
        }
        const std::array<QPoint, 3> properties{QPoint(0, 0), QPoint(2, 1), QPoint(7, 3)};
        for (std::size_t index = 0; index < properties.size(); ++index)
        {
            const auto position = properties[index];
            auto building = MemoryManagement::create<Building>(
                index == 1 ? QStringLiteral("TOWN") : QStringLiteral("HQ"), map.get());
            if (index != 1)
            {
                building->setOwner(map->getPlayer(index == 0 ? OBSERVER_PLAYER : ENEMY_PLAYER));
            }
            map->getTerrain(position.x(), position.y())->setBuilding(building);
        }
        if (buildingCallbacks)
        {
            auto factory = MemoryManagement::create<Building>(QStringLiteral("FACTORY"), map.get());
            map->getTerrain(1, 3)->setBuilding(factory);
        }
        std::array<QPoint, 5> positions{
            QPoint(0, 0), QPoint(2, 1), QPoint(4, 1), QPoint(7, 3), QPoint(5, 1)};
        if (kind == NativeGuardedCase::UnsupportedMovedSniper)
        {
            positions[NATIVE_FIRE_ACTOR] = QPoint(3, 1);
        }
        constexpr std::array tankFirst{NATIVE_FIRE_ACTOR, std::size_t{0}, NATIVE_CAPTURE_ACTOR, std::size_t{3}, NATIVE_FIRE_TARGET};
        for (std::size_t spawn = 0; spawn < units.size(); ++spawn)
        {
            const auto index = lightTank ? tankFirst[spawn] : spawn;
            const auto position = positions[index];
            auto type = (index == NATIVE_FIRE_TARGET && !movementOnly) ||
                (index == NATIVE_FIRE_ACTOR && kind == NativeGuardedCase::UnsupportedMovedSniper)
                ? QStringLiteral("SNIPER") : QStringLiteral("INFANTRY");
            if (lightTank && index == NATIVE_FIRE_ACTOR) { type = QStringLiteral("LIGHT_TANK"); }
            units[index] = map->spawnUnit(position.x(), position.y(), type,
                map->getPlayer(index < NATIVE_FRIENDLY_ACTORS ? OBSERVER_PLAYER : ENEMY_PLAYER),
                EXACT_SPAWN_RANGE, IGNORE_SPAWN_MOVEMENT);
            if (units[index] == nullptr)
            {
                return {};
            }
            units[index]->setHasMoved(false);
        }
        units[NATIVE_CAPTURE_ACTOR]->setCapturePoints(10, false);
        if (kind == NativeGuardedCase::UnlimitedAmmunition || kind == NativeGuardedCase::ChangedUnlimitedAmmunition)
        {
            units[NATIVE_FIRE_ACTOR]->setMaxAmmo1(-1);
            units[NATIVE_FIRE_ACTOR]->setAmmo1(-2);
            units[NATIVE_FIRE_TARGET]->setMaxAmmo1(-1);
            units[NATIVE_FIRE_TARGET]->setAmmo1(-1);
        }
        if (kind == NativeGuardedCase::UnsupportedFireRange)
        {
            units[NATIVE_FIRE_ACTOR]->setMinRange(2);
            units[NATIVE_FIRE_ACTOR]->setMaxRange(3);
        }
        if (kind == NativeGuardedCase::UnsupportedMovedSniper)
        {
            units[NATIVE_FIRE_ACTOR]->setMaxAmmo2(10);
            units[NATIVE_FIRE_ACTOR]->setAmmo2(10);
            units[NATIVE_FIRE_ACTOR]->setWeapon2ID(QStringLiteral("WEAPON_INFANTRY_MG"));
        }
        // Seeded for every case so the terrain case changes a value rather than adds an entry; headless ignores it.
        const auto* target = units[NATIVE_FIRE_TARGET];
        map->getTerrain(target->getX(), target->getY())->getAnimationVariables()->createVariable(
            GUARDED_NATIVE_ANIMATION_FOREGROUND)->writeDataInt32(NATIVE_DETAIL_SEEDED_FOREGROUND);
        auto* rules = map->getGameRules();
        rules->setFogMode(GameEnums::Fog_Off);
        rules->setAiBehaviorMode(GameEnums::AiBehavior_Standard);
        rules->setRandomWeather(false);
        rules->setRoundTimeMs(0);
        for (qint32 index = 0; index < rules->getWeatherCount(); ++index)
        {
            if (rules->getWeather(index)->getWeatherId() == QStringLiteral("WEATHER_1SUN"))
            {
                rules->setStartWeather(index);
                rules->setCurrentWeather(index);
            }
        }
        rules->addVictoryRule(QStringLiteral("VICTORYRULE_NOHQ"));
        rules->addVictoryRule(QStringLiteral("VICTORYRULE_NOUNITS"));
        rules->init();
        rules->startOfTurn(true);
        if (rules->getDayWeather() != rules->getStartWeather())
        {
            return {};
        }
        GameMap::getMapJsThis(map.get());
        return map;
    }

    bool nativeGuardedAssignment(const NativeGuardedUnits & units, AssignmentInput & input,
                                 AssignmentResult & incoming, std::vector<MasterActorSelection> & selected,
                                 NativeGuardedCase kind, bool movementOnly)
    {
        input.actionIds = {QStringLiteral("ACTION_WAIT"), QStringLiteral("ACTION_FIRE"),
                           QStringLiteral("ACTION_CAPTURE")};
        if (kind == NativeGuardedCase::UnsupportedActionIds)
        {
            input.actionIds.wait = QStringLiteral("UNKNOWN_ACTION");
        }
        for (auto* unit : units)
        {
            input.unitLinks.push_back({unit->getUniqueID(), {unit->getX(), unit->getY()}});
        }
        for (std::size_t slot = 0; slot < NATIVE_FRIENDLY_ACTORS; ++slot)
        {
            const auto link = input.unitLinks[slot];
            CandidateBundle candidate;
            candidate.generationIndex = 0;
            candidate.bundle.unitId = static_cast<std::int32_t>(slot);
            candidate.bundle.origin = link.tile;
            candidate.bundle.destination = link.tile;
            candidate.bundle.path = {link.tile};
            candidate.valuation.valid = true;
            if (slot == NATIVE_FIRE_ACTOR && !movementOnly)
            {
                if (kind == NativeGuardedCase::UnsupportedMovedSniper)
                {
                    candidate.bundle.destination = {4, 1};
                    candidate.bundle.path.push_back(candidate.bundle.destination);
                }
                candidate.bundle.components.push_back(fireComponent({
                    .targetUnitId = static_cast<std::int32_t>(NATIVE_FIRE_TARGET),
                    .targetReplacementCost = 2500000, .targetHpSteps = 10, .damageSteps = 4}));
                candidate.valuation.ledger.enemyCapital = 1000;
            }
            AssignmentActor actor{static_cast<std::int32_t>(slot), link.engineUnitId, {candidate}};
            if (slot == NATIVE_CAPTURE_ACTOR)
            {
                candidate.generationIndex = 1;
                candidate.bundle.components.push_back(captureComponent({
                    .income = {1000}, .ownerBefore = OwnerSign::Neutral,
                    .ownerAfter = OwnerSign::Ours, .turnsUntilOwned = 0}));
                actor.candidates.push_back(std::move(candidate));
            }
            const auto action = incoming.plan.addAction(plannedActionFrom(
                input.actionIds, input.unitLinks, actor.engineUnitId, actor.candidates.front()));
            if (action == NO_ACTION ||
                claimFor(incoming.plan, action, actor.candidates.front()) != ReservationResult::Granted)
            {
                return false;
            }
            selected.push_back({actor.engineUnitId, action, 0,
                slot == NATIVE_FIRE_ACTOR ? MasterActorRole::Fixed : MasterActorRole::Mutable});
            AssignmentResult::Selection selection;
            selection.knowledgeUnitIndex = actor.knowledgeUnitIndex;
            selection.engineUnitId = actor.engineUnitId;
            selection.actionIndex = action;
            selection.candidateIndex = 0;
            selection.seatedValue = actor.candidates.front().valuation.value().economicValue;
            selection.stockCoupled = slot != NATIVE_FIRE_ACTOR;
            incoming.selections.push_back(std::move(selection));
            input.actors.push_back(std::move(actor));
        }
        std::vector<OccupancyEntry> occupancy;
        for (const auto & link : input.unitLinks)
        {
            occupancy.push_back({link.engineUnitId, link.tile});
        }
        if (!incoming.plan.rebuildVacateEdges(occupancy).empty())
        {
            return false;
        }
        incoming.executionOrder = incoming.plan.executionOrder().order;
        return incoming.executionOrder.size() == NATIVE_FRIENDLY_ACTORS &&
            (movementOnly || incoming.executionOrder.front() == static_cast<std::int32_t>(NATIVE_FIRE_ACTOR));
    }

    bool checkNativePreparationControls(const MasterAssignmentProblem & problem,
                                        MasterAssignmentEvaluator & evaluator, MasterGuardedRootSource & source,
                                        RefinementLedger & ledger, QVariantMap & record)
    {
        const MasterAssignmentState state{problem.identity(),
            std::vector<std::int32_t>(problem.incumbentChoices().begin(), problem.incumbentChoices().end())};
        const auto evaluated = evaluator.evaluate(state);
        const auto quote = source.maximumPreparationWork();
        if (!evaluated.replay || !quote || *quote <= 0)
        {
            record.insert("error", QStringLiteral("native root funding fixture has no incoming replay"));
            return false;
        }
        const auto exactBefore = ledger.stats().spent();
        MasterGuardedRootPreparation exact;
        {
            RefinementLedger::WorkLimit limit(ledger, *quote);
            exact = source.prepare(state, *evaluated.replay, ledger);
        }
        const bool exactFunded = exact.status == GuardedRelationStatus::Verified && exact.root &&
            ledger.stats().spent() - exactBefore == *quote;
        auto malformed = state;
        malformed.choices.resize(10000, 0);
        const auto malformedBefore = ledger.stats().spent();
        const auto rejected = source.prepare(malformed, *evaluated.replay, ledger);
        const bool malformedRefused = rejected.status == GuardedRelationStatus::Inapplicable &&
            !rejected.root && ledger.stats().spent() - malformedBefore <= *quote;
        record.insert("exactPreparationFunded", exactFunded);
        record.insert("oversizedIntentRefused", malformedRefused);
        return exactFunded && malformedRefused;
    }

    bool checkNativeOneShortPreparation(const MasterAssignmentEvaluation & evaluated,
                                       MasterGuardedRootSource & source, RefinementLedger & ledger,
                                       QVariantMap & record)
    {
        const auto quote = source.maximumPreparationWork();
        if (!evaluated.replay || !quote || *quote <= 0)
        {
            return false;
        }
        const auto shortBefore = ledger.stats().spent();
        MasterGuardedRootPreparation shortResult;
        {
            RefinementLedger::WorkLimit limit(ledger, *quote - 1);
            shortResult = source.prepare(evaluated.state, *evaluated.replay, ledger);
        }
        const bool shortRefused = shortResult.status == GuardedRelationStatus::BudgetExhausted &&
            !shortResult.root && ledger.stats().spent() - shortBefore <= *quote - 1;
        record.insert("oneShortPreparationRefusedAfterSearch", shortRefused);
        return shortRefused;
    }

    QVariantList nativeCollectionCosts(const MasterNativeSourcePreparation & source, bool & reconciled)
    {
        QVariantList records;
        if (source.collectionCosts[static_cast<std::size_t>(MasterNativeCollectionStage::Bookkeeping)].observations == 0)
        {
            reconciled = source.collectionWork == 0;
            return records;
        }
        std::int64_t work = 0;
        std::int64_t expected = 0;
        std::int64_t receiver = 0;
        reconciled = true;
        for (std::size_t index = 0; index < source.collectionCosts.size(); ++index)
        {
            const auto & cost = source.collectionCosts[index];
            records.push_back(QVariantMap{
                {"stage", masterNativeCollectionStageName(static_cast<MasterNativeCollectionStage>(index))},
                {"observations", static_cast<qulonglong>(cost.observations)},
                {"work", QString::number(cost.work)},
                {"expectedConstructionWork", QString::number(cost.expectedConstructionWork)},
                {"receiverConstructionWork", QString::number(cost.receiverConstructionWork)}});
            if (cost.work < 0 || cost.expectedConstructionWork < 0 || cost.receiverConstructionWork < 0 ||
                !RelativeStockDetail::checkedAdd(work, cost.work) ||
                !RelativeStockDetail::checkedAdd(expected, cost.expectedConstructionWork) ||
                !RelativeStockDetail::checkedAdd(receiver, cost.receiverConstructionWork))
            {
                reconciled = false;
            }
        }
        reconciled = reconciled && work <= source.work;
        if (source.collectionWork > 0)
        {
            reconciled = reconciled && work == source.collectionWork &&
                expected == source.expectedConstructionWork && receiver == source.receiverConstructionWork;
        }
        return records;
    }

    bool changesOnlyDetailFacts(NativeGuardedCase kind)
    {
        return kind == NativeGuardedCase::ChangedBattleSpeed || kind == NativeGuardedCase::ChangedTerrainAnimation;
    }

    // Both foreground values are admitted on plains, so only the value comparison can refuse the change.
    bool changeTerrainAnimation(GameMap & map, const Unit & target)
    {
        auto* variables = map.getTerrain(target.getX(), target.getY())->getAnimationVariables();
        auto* existing = variables->getVariable(GUARDED_NATIVE_ANIMATION_FOREGROUND);
        if (existing == nullptr || existing->readDataInt32() != NATIVE_DETAIL_SEEDED_FOREGROUND)
        {
            return false;
        }
        existing->writeDataInt32(NATIVE_DETAIL_CHANGED_FOREGROUND);
        return true;
    }

    class BattleSpeedRestore final
    {
    public:
        BattleSpeedRestore()
            : m_value(static_cast<quint32>(Settings::getInstance()->getBattleAnimationSpeedValue()))
        {
        }
        BattleSpeedRestore(const BattleSpeedRestore &) = delete;
        BattleSpeedRestore & operator=(const BattleSpeedRestore &) = delete;
        ~BattleSpeedRestore()
        {
            Settings::getInstance()->setBattleAnimationSpeed(m_value);
        }
        // Raw speeds of 100 and above all derive the same speed, so only 1 and 2 are certain to differ.
        void change() const
        {
            Settings::getInstance()->setBattleAnimationSpeed(m_value == 1 ? 2 : 1);
        }

    private:
        quint32 m_value;
    };

    class BattlePresentationRestore final
    {
    public:
        BattlePresentationRestore()
            : m_type(Settings::getInstance()->getBattleAnimationType()),
              m_mode(Settings::getInstance()->getBattleAnimationMode())
        {
        }
        BattlePresentationRestore(const BattlePresentationRestore &) = delete;
        BattlePresentationRestore & operator=(const BattlePresentationRestore &) = delete;
        ~BattlePresentationRestore()
        {
            Settings::getInstance()->setBattleAnimationType(m_type);
            Settings::getInstance()->setBattleAnimationMode(m_mode);
        }

    private:
        GameEnums::BattleAnimationType m_type;
        GameEnums::BattleAnimationMode m_mode;
    };

    // The extraction problem points into the fixture, so it must never be copied or moved.
    struct NativeGuardedFixture
    {
        NativeGuardedFixture() = default;
        NativeGuardedFixture(const NativeGuardedFixture &) = delete;
        NativeGuardedFixture & operator=(const NativeGuardedFixture &) = delete;
        AssignmentInput input;
        AssignmentResult incoming;
        std::vector<MasterActorSelection> selected;
        MasterAssignmentBuildResult extraction;
        RefinementLedger ledger;
    };

    std::unique_ptr<NativeGuardedFixture> nativeGuardedFixture(
        const spGameMap & map, const NativeGuardedUnits & units, NativeGuardedCase kind, QString & error, bool movementOnly = false)
    {
        auto fixture = std::make_unique<NativeGuardedFixture>();
        if (!map || !nativeGuardedAssignment(units, fixture->input, fixture->incoming, fixture->selected, kind, movementOnly))
        {
            error = QStringLiteral("native guarded fixture setup failed");
            return {};
        }
        fixture->extraction = buildMasterAssignmentProblem(fixture->input, {
            .sessionIdentity = 9009, .selections = fixture->selected, .pIncumbentPlan = &fixture->incoming.plan,
            .incumbentExecutionOrder = fixture->incoming.executionOrder});
        if (!fixture->extraction.valid() || !fixture->ledger.open(NATIVE_GUARDED_ALLOWANCE))
        {
            error = QStringLiteral("native guarded fixture extraction failed");
            return {};
        }
        return fixture;
    }

    enum class CommandControl { Off, Install, Action, Repair, HashCallback, Receiver, Setting, StreamError,
                                Delegate, OwnApply, PrototypeApply, ActiveCo, ActorRepair, UnitReceiver, MapReceiver,
                                StockInit, StockCost, PostInit, Movement, Capacity, Vision,
                                WeatherMember, WeatherHelper, WeatherPending, WeatherOrigin, WeatherKey, WeatherVariables,
                                PostAction, TrapCallback, ChessPeer, BarrageHelper, BarrageVariable, ActionMarker, ArrayPush,
                                CallbackHelper, CallbackResolver, CaptureDelegate, CallbackLeaf, SpriteContext,
                                OriginMarker, ArrayIsArray, StringUpper, EmbargoVariable, SpriteSettingStream, AnimationPrototype,
                                TownSprites, TownWeather, FactorySprites, FactoryWeather, HeadquartersSprites, HeadquartersWeather,
                                BadgeAppender, BadgeIcon, BadgeEnabled, BadgeDefaultPolicy,
                                ForeignBuildingSprites, ForeignBuildingWeather, BuildingBadgeKey, BuildingBadgeStream,
                                BuildingReceiver, DirectBuildingWrapper };

    enum class LightTankControls { Off, Definition, Weather, Movement, StockMovement, Fire, Callbacks, Buildings };

    struct BuildingHookControl
    {
        const char* global;
        const char* member;
        CommandControl fault;
    };
    constexpr std::array BUILDING_HOOK_CONTROLS{
        BuildingHookControl{"TOWN", "loadSprites", CommandControl::TownSprites},
        BuildingHookControl{"TOWN", "onWeatherChanged", CommandControl::TownWeather},
        BuildingHookControl{"FACTORY", "loadSprites", CommandControl::FactorySprites},
        BuildingHookControl{"FACTORY", "onWeatherChanged", CommandControl::FactoryWeather},
        BuildingHookControl{"HQ", "loadSprites", CommandControl::HeadquartersSprites},
        BuildingHookControl{"HQ", "onWeatherChanged", CommandControl::HeadquartersWeather}};
    constexpr std::array BUILDING_HELPER_CONTROLS{
        std::pair{CommandControl::BadgeAppender, "appendAssignedBuildingBadge"},
        std::pair{CommandControl::BadgeIcon, "getProductionBuildingBadgeIcon"},
        std::pair{CommandControl::BadgeEnabled, "usesProductionBadges"},
        std::pair{CommandControl::BadgeDefaultPolicy, "isDefaultOnPolicyEnabled"}};

    class CallbackFixture final
    {
    public:
        CallbackFixture() = default;
        CallbackFixture(const CallbackFixture &) = delete;
        CallbackFixture & operator=(const CallbackFixture &) = delete;
        ~CallbackFixture()
        {
            for (auto entry = m_saved.rbegin(); entry != m_saved.rend(); ++entry)
            {
                if (entry->own) { entry->object.setProperty(entry->name, entry->value); }
                else { entry->object.deleteProperty(entry->name); }
            }
        }

        void save(QJSValue object, const QString & name)
        {
            m_saved.push_back({object, name, object.property(name), object.hasOwnProperty(name)});
        }

        QJSValue waitPerform;
        std::array<QJSValue, BUILDING_HOOK_CONTROLS.size()> buildingDelegates;
        bool active{false};
        bool buildings{false};

    private:
        struct SavedProperty
        {
            QJSValue object;
            QString name;
            QJSValue value;
            bool own;
        };
        std::vector<SavedProperty> m_saved;
    };

    struct RepairControl
    {
        QJSValue original;
        QJSValue otherWrapper;
        QJSValue stockInit;
        QJSValue stockCost;
    };

    QVariantMap checkNativeGuarded(NativeGuardedCase kind, const spGameMap & map, const NativeGuardedUnits & units,
                                   NativeGuardedScreen screen, CommandControl command = CommandControl::Off,
                                   const RepairControl* repair = nullptr, bool movementOnly = false, Unit* commandUnit = nullptr,
                                   const CallbackFixture* callbacks = nullptr)
    {
        if (!commandUnit) { commandUnit = units[NATIVE_CAPTURE_ACTOR]; }
        QJSValue directObject, savedWrapper;
        const auto restoreDirect = qScopeGuard([&] {
            if (!savedWrapper.isUndefined()) { directObject.setProperty(QStringLiteral("loadSprites"), savedWrapper); }
        });
        if (command == CommandControl::DirectBuildingWrapper)
        {
            if (!callbacks || !callbacks->buildings) { return {{"ok", false}}; }
            directObject = Interpreter::getInstance()->globalObject().property(QStringLiteral("TOWN"));
            savedWrapper = directObject.property(QStringLiteral("loadSprites"));
            directObject.setProperty(QStringLiteral("loadSprites"), callbacks->buildingDelegates[0]);
            if (savedWrapper.strictlyEquals(callbacks->buildingDelegates[0]) ||
                !directObject.property(QStringLiteral("loadSprites")).strictlyEquals(callbacks->buildingDelegates[0]))
            {
                return {{"ok", false}, {"error", QStringLiteral("direct building setup did not apply")}};
            }
        }
        QString error;
        const auto fixture = nativeGuardedFixture(map, units, kind, error, movementOnly);
        if (!fixture)
        {
            return {{"ok", false}, {"error", error}};
        }
        auto & incoming = fixture->incoming;
        auto & ledger = fixture->ledger;
        const auto & problem = *fixture->extraction.problem;
        auto source = prepareMasterNativeSource(*map, problem, ledger);
        bool collectionCostsReconciled = false;
        const auto collectionCosts = nativeCollectionCosts(source, collectionCostsReconciled);
        QVariantMap record{{"case", static_cast<int>(kind)}, {"nativeSourceStatus", masterNativeSourceStatusName(source.status)},
            {"sourceWork", QString::number(source.work)}, {"boundModules", static_cast<qulonglong>(source.boundModules)},
            {"commandCallbacks", source.commandCallbacks},
            {"sourceCollectionWork", QString::number(source.collectionWork)},
            {"sourceExpectedConstructionWork", QString::number(source.expectedConstructionWork)},
            {"sourceReceiverConstructionWork", QString::number(source.receiverConstructionWork)},
            {"sourceCommonCollectionWork", QString::number(source.commonCollectionWork)},
            {"commandAuthenticationAllowance", QString::number(source.commandAuthenticationAllowance)},
            {"sourceCollectionComplete", source.collectionWork > 0},
            {"sourceCollectionCosts", collectionCosts},
            {"sourceCollectionCostsReconciled", collectionCostsReconciled},
            {"sourceRefusal", masterRefusalRecord(ledger.lastRefusal()).toVariantMap()},
            {"dayWeather", map->getGameRules()->getDayWeather()},
            {"forecastDays", map->getGameRules()->getWeatherForecast().size()},
            {"inJsCall", Interpreter::getInstance()->getInJsCall()},
            {"battlePresentation", static_cast<int>(Settings::getInstance()->getBattleAnimationType())},
            {"detailPresentation", source.detailPresentation}};
        if (source.detailPresentation != (screen == NativeGuardedScreen::Detail))
        {
            record.insert("ok", false);
            record.insert("error", QStringLiteral("detail presentation basis does not match the screen"));
            return record;
        }
        if (kind == NativeGuardedCase::UnsupportedFireRange || kind == NativeGuardedCase::UnsupportedMovedSniper ||
            kind == NativeGuardedCase::UnsupportedActionIds)
        {
            const auto expected = kind == NativeGuardedCase::UnsupportedActionIds
                ? MasterNativeSourceStatus::UnsupportedCallbacks : MasterNativeSourceStatus::UnsupportedInventory;
            const bool rejected = source.status == expected &&
                source.source && !source.source->maximumPreparationWork() &&
                !source.source->maximumAuthenticationWork() && ledger.stats().spent() <= NATIVE_GUARDED_ALLOWANCE;
            record.insert("ok", rejected);
            record.insert("unsupportedAdmissionRejected", rejected);
            record.insert("masterWork", masterWorkRecord(ledger.stats()).toVariantMap());
            return record;
        }
        if (source.status != MasterNativeSourceStatus::Ready || !source.source || !collectionCostsReconciled)
        {
            record.insert("ok", false);
            record.insert("masterWork", masterWorkRecord(ledger.stats()).toVariantMap());
            return record;
        }
        const std::uint16_t expectedCallbackMask = callbacks && callbacks->buildings
            ? (command == CommandControl::DirectBuildingWrapper ? 0x7dff : 0x7fff) : 0x1ff;
        if (callbacks && (!callbacks->active || source.commandCallbacks != expectedCallbackMask))
        {
            record.insert("ok", false);
            record.insert("error", QStringLiteral("callback fixture did not admit the expected families"));
            return record;
        }
        const auto preparationQuote = source.source->maximumPreparationWork();
        record.insert("preparationQuote", QString::number(preparationQuote.value_or(-1)));
        const auto authenticationQuote = source.source->maximumAuthenticationWork();
        record.insert("authenticationQuote", QString::number(authenticationQuote.value_or(-1)));
        const auto largestQuote = masterNativeRootPreparationWork({GUARDED_STOCK_MAX_ACTORS, GUARDED_STOCK_MAX_TILES,
                                                                   GUARDED_STOCK_MAX_TILES, GUARDED_NATIVE_MAX_PATH_POINTS});
        const bool largestQuoted = preparationQuote && largestQuote && *largestQuote > *preparationQuote &&
            *largestQuote < NATIVE_GUARDED_ALLOWANCE && !masterNativeRootPreparationWork({1, 1, 2, 1});
        record.insert("maximumShapePreparationQuote", QString::number(largestQuote.value_or(-1)));
        if (kind == NativeGuardedCase::Valid && !largestQuoted)
        {
            record.insert("ok", false);
            record.insert("error", QStringLiteral("maximum native plan shape quote is unbounded or exceeds the allowance"));
            return record;
        }
        auto* sourcePointer = source.source.get();
        MasterGuardedStockOwner owner(problem, ledger, std::move(source.source));
        MasterAssignmentEvaluator evaluator(problem, ledger, owner);
        MasterAssignmentInstallation installation(problem, evaluator, ledger);
        const auto reserved = installation.prepare(incoming);
        record.insert("reservationStatus", static_cast<int>(reserved));
        record.insert("reservedWork", QString::number(installation.reservedWork()));
        if (reserved != MasterInstallationStatus::Reserved)
        {
            record.insert("ok", false);
            record.insert("masterWork", masterWorkRecord(ledger.stats()).toVariantMap());
            return record;
        }
        if (kind == NativeGuardedCase::Valid &&
            !checkNativePreparationControls(problem, evaluator, *sourcePointer, ledger, record))
        {
            record.insert("ok", false);
            return record;
        }
        if (kind == NativeGuardedCase::InsufficientSearchFunding)
        {
            if (!ledger.consume(ledger.balance(), RefinementWork::MasterExpansion))
            {
                record.insert("ok", false);
                return record;
            }
        }
        const auto before = incoming;
        record.insert("refusalsBeforeSearch", QString::number(ledger.stats().refusals));
        const auto searched = searchMasterAssignment(problem, evaluator, ledger, {.retainDiagnostics = true});
        if (kind == NativeGuardedCase::Valid)
        {
            if (!searched.improvement ||
                !checkNativeOneShortPreparation(searched.improvement->incumbent, *sourcePointer, ledger, record))
            {
                record.insert("ok", false);
                record.insert("search", masterSearchRecord(searched).toVariantMap());
                return record;
            }
        }
        auto* realm = Interpreter::getInstance();
        const auto realmEntries = realm->getRealmEntries();
        const bool realmFenced = realm->isInterrupted();
        if (kind == NativeGuardedCase::ChangedProgress)
        {
            units[NATIVE_CAPTURE_ACTOR]->setCapturePoints(9, false);
        }
        if (kind == NativeGuardedCase::ChangedUnlimitedAmmunition)
        {
            units[NATIVE_FIRE_ACTOR]->setAmmo1(-3);
        }
        if (kind == NativeGuardedCase::ChangedArmySource)
        {
            map->getPlayer(OBSERVER_PLAYER)->setPlayerArmy(QStringLiteral("OS"));
        }
        if (kind == NativeGuardedCase::ChangedRealm)
        {
            realm->pushInt(NATIVE_GUARDED_REALM_PROBE, 1);
        }
        QJSValue changedObject;
        QJSValue previousValue;
        QString changedKey;
        if (kind == NativeGuardedCase::ChangedArmyTable || kind == NativeGuardedCase::ChangedArmyCallback ||
            kind == NativeGuardedCase::LongerArmyEntry)
        {
            const auto player = realm->globalObject().property(QStringLiteral("PLAYER"));
            const bool changeTable = kind != NativeGuardedCase::ChangedArmyCallback;
            changedObject = changeTable ? player.property(QStringLiteral("armies")) : player;
            changedKey = changeTable ? QStringLiteral("0") : QStringLiteral("getDefaultArmy");
            previousValue = changedObject.property(changedKey);
            const auto changedArmy = kind == NativeGuardedCase::LongerArmyEntry ? QStringLiteral("CHANGED") : QStringLiteral("XX");
            const auto replacement = changeTable ? QJSValue(changedArmy) : player.property(QStringLiteral("getArmies"));
            changedObject.setProperty(changedKey, replacement);
            if (previousValue.strictlyEquals(replacement) || !changedObject.property(changedKey).strictlyEquals(replacement))
            {
                changedObject.setProperty(changedKey, previousValue);
                record.insert("ok", false);
                record.insert("error", QStringLiteral("direct default-army mutation did not apply"));
                return record;
            }
        }
        QObject* changedOwnership = nullptr;
        bool removeOwnKey = false;
        QJSValue rootedReceiver;
        const bool commandFault = command != CommandControl::Off && command != CommandControl::Install;
        if (commandFault)
        {
            auto* rule = map->getGameRules()->getVictoryRule(QStringLiteral("CMODE_CO_COMMAND"));
            auto* variable = rule->getVariables()->getVariable(QStringLiteral("SpinboxValue1"));
            if (command == CommandControl::EmbargoVariable)
            {
                auto* variables = map->getPlayer(1)->getVariables();
                const auto key = QStringLiteral("DB_CURSE_EMBARGO_UNTIL");
                if (variables->getVariable(key) || variables->createVariable(key)->getModified())
                {
                    return {{"ok", false}, {"error", QStringLiteral("embargo probe was not an unmodified new key")}};
                }
            }
            else if (command == CommandControl::SpriteSettingStream || command == CommandControl::BuildingBadgeStream)
            {
                auto* sprite = rule->getVariables()->getVariable(QString(command == CommandControl::BuildingBadgeStream
                    ? GUARDED_COMMAND_BUILDING_BADGE_SETTING : GUARDED_COMMAND_SPRITE_SETTING));
                if (!sprite || sprite->getStoredData() != QByteArrayView("\0\0\0\1", 4))
                {
                    return {{"ok", false}, {"error", QStringLiteral("sprite setting fixture value differs")}};
                }
                const auto data = sprite->getStoredData();
                const auto modified = sprite->getModified();
                sprite->readDataListInt32();
                if (sprite->hasUsableStoredDataStream() || data != sprite->getStoredData() || modified != sprite->getModified())
                {
                    return {{"ok", false}, {"error", QStringLiteral("sprite stream corruption changed stored bytes or flags")}};
                }
            }
            else if (command == CommandControl::BarrageVariable)
            {
                auto* variables = map->getPlayer(1)->getVariables();
                if (variables->getVariable(QString(GUARDED_WARDEN_BARRAGES)) ||
                    variables->createVariable(QString(GUARDED_WARDEN_BARRAGES))->getModified())
                {
                    return {{"ok", false}, {"error", QStringLiteral("barrage probe was not an unmodified new key")}};
                }
            }
            else if (command == CommandControl::WeatherOrigin)
            {
                auto* variables = commandUnit->getVariables();
                if (variables->getVariable(QString(GUARDED_COMMAND_UNIT_OWNER)) ||
                    variables->createVariable(QString(GUARDED_COMMAND_UNIT_OWNER))->getModified())
                {
                    return {{"ok", false}, {"error", QStringLiteral("weather origin probe was not an unmodified new key")}};
                }
            }
            else if (command == CommandControl::Capacity)
            {
                commandUnit->setMaxAmmo1(10);
            }
            else if (command == CommandControl::Vision)
            {
                commandUnit->setVision(commandUnit->getBaseVision() + 1);
            }
            else if (command == CommandControl::Setting)
            {
                rule->getVariables()->getVariable(QStringLiteral("SpinboxValue2"))->writeDataInt32(6);
            }
            else if (command == CommandControl::StreamError)
            {
                const auto data = variable->getStoredData();
                variable->readDataListInt32();
                if (variable->hasUsableStoredDataStream() || data != variable->getStoredData() || !variable->getModified())
                {
                    return {{"ok", false}, {"error", QStringLiteral("stored stream fault was not isolated")}};
                }
            }
            else if (command == CommandControl::Receiver || command == CommandControl::UnitReceiver ||
                     command == CommandControl::MapReceiver || command == CommandControl::WeatherVariables || command == CommandControl::BuildingReceiver)
            {
                QObject* receiver = variable;
                if (command == CommandControl::UnitReceiver) { receiver = commandUnit; }
                if (command == CommandControl::MapReceiver) { receiver = map.get(); }
                if (command == CommandControl::BuildingReceiver) { receiver = map->getTerrain(2, 1)->getBuilding(); }
                if (command == CommandControl::WeatherVariables) { receiver = commandUnit->getVariables(); }
                rootedReceiver = realm->newQObject(receiver);
                if (!rootedReceiver.isQObject() || QJSEngine::objectOwnership(receiver) != QJSEngine::CppOwnership)
                {
                    return {{"ok", false}, {"error", QStringLiteral("missing native-owned receiver")}};
                }
                changedOwnership = receiver;
                QJSEngine::setObjectOwnership(receiver, QJSEngine::JavaScriptOwnership);
            }
            else
            {
                auto global = realm->globalObject();
                auto other = global.property(QStringLiteral("ACTION_WAIT")).property(QStringLiteral("perform"));
                if (command == CommandControl::PostAction || command == CommandControl::TrapCallback ||
                    command == CommandControl::ChessPeer || command == CommandControl::BarrageHelper ||
                    command == CommandControl::ActionMarker || command == CommandControl::ArrayPush)
                {
                    if (command == CommandControl::PostAction)
                    {
                        changedObject = global.property(commandUnit->getUnitID());
                        changedKey = QStringLiteral("postAction");
                        other = QJSValue(QJSValue::NullValue);
                    }
                    else if (command == CommandControl::TrapCallback)
                    {
                        changedObject = global.property(QStringLiteral("ACTION_TRAP"));
                        changedKey = QStringLiteral("isTrap");
                    }
                    else if (command == CommandControl::ChessPeer)
                    {
                        changedObject = global.property(QStringLiteral("CMODE_CHESS_LITE")).property(QStringLiteral("peers"));
                        changedKey = QStringLiteral("0");
                        other = QJSValue(QStringLiteral("CMODE_CO_COMMAND"));
                    }
                    else if (command == CommandControl::ArrayPush)
                    {
                        changedObject = global.property(QStringLiteral("Array")).property(QStringLiteral("prototype"));
                        changedKey = QStringLiteral("push");
                        other = changedObject.property(QStringLiteral("pop"));
                    }
                    else
                    {
                        changedObject = global.property(QStringLiteral("WARDEN_BARRAGE"));
                        changedKey = command == CommandControl::BarrageHelper
                            ? QStringLiteral("triggerField") : QStringLiteral("resolvedAction");
                        if (command == CommandControl::ActionMarker) { other = realm->newObject(); }
                    }
                    removeOwnKey = !changedObject.hasOwnProperty(changedKey);
                }
                else if (command == CommandControl::WeatherMember || command == CommandControl::WeatherHelper ||
                    command == CommandControl::WeatherPending || command == CommandControl::WeatherKey)
                {
                    changedObject = global.property(command == CommandControl::WeatherMember
                        ? commandUnit->getUnitID() : QStringLiteral("CO_COMMAND"));
                    if (command == CommandControl::WeatherMember) { changedKey = QStringLiteral("getWeatherImmune"); other = QJSValue(QJSValue::NullValue); }
                    if (command == CommandControl::WeatherHelper) { changedKey = QStringLiteral("getEffectiveUnitOrigin"); }
                    if (command == CommandControl::WeatherPending) { changedKey = QStringLiteral("pendingBuild"); other = QJSValue(QJSValue::UndefinedValue); }
                    if (command == CommandControl::WeatherKey) { changedKey = QStringLiteral("UNIT_OWNER_KEY"); other = QJSValue(QStringLiteral("changed")); }
                    removeOwnKey = !changedObject.hasOwnProperty(changedKey);
                }
                else if (command == CommandControl::StockInit || command == CommandControl::StockCost ||
                    command == CommandControl::PostInit || command == CommandControl::Movement)
                {
                    changedObject = global.property(commandUnit->getUnitID());
                    if (command == CommandControl::StockInit)
                    {
                        changedKey = QStringLiteral("init");
                        other = repair->stockInit;
                    }
                    else if (command == CommandControl::StockCost)
                    {
                        changedKey = QStringLiteral("getBaseCost");
                        other = repair->stockCost;
                    }
                    else
                    {
                        changedKey = command == CommandControl::PostInit ? QStringLiteral("initForMods") : QStringLiteral("getMovementType");
                    }
                    removeOwnKey = !changedObject.hasOwnProperty(changedKey);
                }
                else if (callbacks)
                {
                    const auto hook = std::ranges::find(BUILDING_HOOK_CONTROLS, command, &BuildingHookControl::fault);
                    const auto helper = std::ranges::find_if(BUILDING_HELPER_CONTROLS, [&](const auto & entry) { return entry.first == command; });
                    if (hook != BUILDING_HOOK_CONTROLS.end())
                    {
                        changedObject = global.property(QString::fromLatin1(hook->global));
                        changedKey = QString::fromLatin1(hook->member);
                        other = callbacks->buildingDelegates[std::distance(BUILDING_HOOK_CONTROLS.begin(), hook)];
                    }
                    else if (helper != BUILDING_HELPER_CONTROLS.end())
                    {
                        changedObject = global.property(QStringLiteral("CO_COMMAND"));
                        changedKey = QString::fromLatin1(helper->second);
                        other = QJSValue(7);
                    }
                    else switch (command)
                    {
                        case CommandControl::ForeignBuildingSprites:
                        case CommandControl::ForeignBuildingWeather:
                            changedObject = global.property(QStringLiteral("TOWN"));
                            changedKey = command == CommandControl::ForeignBuildingSprites
                                ? QStringLiteral("loadSprites") : QStringLiteral("onWeatherChanged");
                            other = global.property(QStringLiteral("FACTORY")).property(changedKey);
                            break;
                        case CommandControl::BuildingBadgeKey:
                            changedObject = global.property(QStringLiteral("CO_COMMAND"));
                            changedKey = QStringLiteral("PRODUCTION_BADGE_KEY");
                            other = QJSValue(QStringLiteral("changed"));
                            break;
                        case CommandControl::DirectBuildingWrapper:
                            changedObject = directObject;
                            changedKey = QStringLiteral("loadSprites");
                            other = savedWrapper;
                            break;
                        case CommandControl::CallbackHelper:
                            changedObject = global.property(QStringLiteral("CO_COMMAND"));
                            changedKey = QStringLiteral("getActionPoint");
                            break;
                        case CommandControl::CallbackResolver:
                            changedObject = global.property(QStringLiteral("CO_COMMAND"));
                            changedKey = QStringLiteral("actionTargetContext");
                            other = changedObject.property(QStringLiteral("postAnimationUnitContext"));
                            break;
                        case CommandControl::CaptureDelegate:
                            changedObject = global.property(QStringLiteral("ACTION_CAPTURE"));
                            changedKey = QStringLiteral("newCanBePerformed");
                            other = global.property(QStringLiteral("ACTION_WAIT")).property(QStringLiteral("canBePerformed"));
                            break;
                        case CommandControl::CallbackLeaf:
                            changedObject = global.property(QStringLiteral("ACTION_WAIT"));
                            changedKey = QStringLiteral("perform");
                            other = callbacks->waitPerform;
                            break;
                        case CommandControl::SpriteContext:
                            changedObject = global.property(QStringLiteral("CO_COMMAND"));
                            changedKey = QStringLiteral("spriteContext");
                            other = QJSValue(7);
                            break;
                        case CommandControl::OriginMarker:
                            changedObject = global.property(QStringLiteral("ACTION_CAPTURE"));
                            changedKey = QStringLiteral("__cmode_co_command_origin");
                            other = QJSValue(7);
                            break;
                        case CommandControl::ArrayIsArray:
                            changedObject = global.property(QStringLiteral("Array"));
                            changedKey = QStringLiteral("isArray");
                            other = changedObject.property(QStringLiteral("prototype")).property(QStringLiteral("indexOf"));
                            break;
                        case CommandControl::StringUpper:
                            changedObject = global.property(QStringLiteral("String")).property(QStringLiteral("prototype"));
                            changedKey = QStringLiteral("toUpperCase");
                            other = changedObject.property(QStringLiteral("toLowerCase"));
                            break;
                        case CommandControl::AnimationPrototype:
                            changedObject = global.property(QStringLiteral("Object")).property(QStringLiteral("prototype"));
                            changedKey = QStringLiteral("getGameRules");
                            break;
                        default:
                            return {{"ok", false}, {"error", QStringLiteral("unhandled callback control")}};
                    }
                    removeOwnKey = !changedObject.hasOwnProperty(changedKey);
                }
                else if (repair)
                {
                    const auto proto = global.property(QStringLiteral("Function")).property(QStringLiteral("prototype"));
                    if (command == CommandControl::Delegate || command == CommandControl::ActorRepair)
                    {
                        changedObject = global.property(command == CommandControl::Delegate ? QStringLiteral("UNIT") : QStringLiteral("INFANTRY"));
                        changedKey = QStringLiteral("repairUnit");
                        other = repair->otherWrapper;
                    }
                    else if (command == CommandControl::ActiveCo)
                    {
                        changedObject = global.property(QStringLiteral("CO_COMMAND"));
                        changedKey = QStringLiteral("activeHookCo");
                        other = QJSValue(7);
                    }
                    else
                    {
                        changedObject = command == CommandControl::OwnApply ? repair->original : proto;
                        changedKey = QStringLiteral("apply");
                        other = proto.property(QStringLiteral("call"));
                    }
                    removeOwnKey = !changedObject.hasOwnProperty(changedKey);
                }
                else if (command == CommandControl::ActorRepair)
                {
                    changedObject = global.property(commandUnit->getUnitID());
                    changedKey = QStringLiteral("repairUnit");
                    removeOwnKey = !changedObject.hasOwnProperty(changedKey);
                }
                else if (command == CommandControl::Action)
                {
                    changedObject = global.property(QStringLiteral("ACTION_CAPTURE"));
                    changedKey = QStringLiteral("perform");
                }
                else if (command == CommandControl::Repair)
                {
                    changedObject = global.property(QStringLiteral("UNIT"));
                    changedKey = QStringLiteral("repairUnit");
                }
                else
                {
                    changedObject = global.property(QStringLiteral("CMODE_CO_COMMAND"));
                    changedKey = QStringLiteral("getRuleType");
                }
                previousValue = changedObject.property(changedKey);
                changedObject.setProperty(changedKey, other);
                if (previousValue.strictlyEquals(other) || !changedObject.property(changedKey).strictlyEquals(other))
                {
                    return {{"ok", false}, {"error", QStringLiteral("command mutation did not apply")}};
                }
            }
        }
        const auto restoreProperty = qScopeGuard([&] {
            if (changedOwnership)
            {
                QJSEngine::setObjectOwnership(changedOwnership, QJSEngine::CppOwnership);
            }
            if (removeOwnKey)
            {
                changedObject.deleteProperty(changedKey);
            }
            else if (!changedKey.isEmpty())
            {
                changedObject.setProperty(changedKey, previousValue);
            }
        });
        const BattleSpeedRestore battleSpeed;
        if (kind == NativeGuardedCase::ChangedBattleSpeed)
        {
            battleSpeed.change();
        }
        if (kind == NativeGuardedCase::ChangedTerrainAnimation &&
            !changeTerrainAnimation(*map, *units[NATIVE_FIRE_TARGET]))
        {
            record.insert("ok", false);
            record.insert("error", QStringLiteral("seeded terrain animation variable is missing"));
            return record;
        }
        const bool nativeOnlyChange = kind == NativeGuardedCase::ChangedRealm ||
            realm->getRealmEntries() == realmEntries;
        record.insert("realmFencedBeforeFinalization", realmFenced);
        record.insert("nativeOnlyChange", nativeOnlyChange);
        if (!ledger.consume(ledger.balance(), RefinementWork::MasterExpansion))
        {
            record.insert("ok", false);
            return record;
        }
        const bool quoteStableBefore = sourcePointer->maximumAuthenticationWork() == authenticationQuote;
        const auto installed = installation.finish(searched.improvement, incoming);
        const auto quoteAfter = sourcePointer->maximumAuthenticationWork();
        const bool quoteRequired = installed.status == MasterInstallationStatus::Installed ||
            installed.status == MasterInstallationStatus::BudgetExhausted;
        const bool quoteStable = quoteStableBefore &&
            (quoteAfter == authenticationQuote || (!quoteAfter && !quoteRequired));
        record.insert("authenticationQuoteStable", quoteStable);
        record.insert("authenticationQuoteAvailableAfter", quoteAfter.has_value());
        record.insert("incomingPlanPreserved", incoming.plan.equivalentState(before.plan));
        record.insert("incomingOrderPreserved", incoming.executionOrder == before.executionOrder);
        if (const auto refusal = ledger.lastRefusal())
        {
            record.insert("lastBudgetRefusal", QVariantMap{
                {"requested", QString::number(refusal->requested)}, {"balance", QString::number(refusal->balance)}});
        }
        bool valid = quoteStable && installed.finalizationWork <= installed.reservedWork && realmFenced && nativeOnlyChange &&
            ledger.stats().spent() <= NATIVE_GUARDED_ALLOWANCE &&
            units[NATIVE_FIRE_TARGET]->getHp() == 10 && !units[NATIVE_FIRE_ACTOR]->getHasMoved() &&
            map->getTerrain(2, 1)->getBuilding()->getOwner() == nullptr;
        // Without a detail presentation nothing authenticates these facts, so the headless run must still install.
        const bool expectInstalled = !commandFault && (kind == NativeGuardedCase::Valid || kind == NativeGuardedCase::DefaultArmy ||
            kind == NativeGuardedCase::UnlimitedAmmunition ||
            (changesOnlyDetailFacts(kind) && screen == NativeGuardedScreen::Headless));
        if (expectInstalled)
        {
            valid = valid && searched.improvement && searched.improvement->guardedProof &&
                installed.status == MasterInstallationStatus::Installed &&
                incoming.selections[NATIVE_CAPTURE_ACTOR].candidateIndex == 1 &&
                units[NATIVE_CAPTURE_ACTOR]->getCapturePoints() == 10;
        }
        else
        {
            valid = valid && incoming.plan.equivalentState(before.plan) &&
                incoming.executionOrder == before.executionOrder;
            if (kind == NativeGuardedCase::ChangedProgress || kind == NativeGuardedCase::ChangedRealm ||
                usesDefaultArmy(kind) || kind == NativeGuardedCase::ChangedUnlimitedAmmunition ||
                changesOnlyDetailFacts(kind))
            {
                const bool exceedsReplayGrant = kind == NativeGuardedCase::LongerArmyEntry;
                const auto expectedStatus = exceedsReplayGrant ? MasterInstallationStatus::BudgetExhausted
                                                              : MasterInstallationStatus::ProofFailure;
                valid = valid && searched.improvement && searched.improvement->guardedProof &&
                    installed.status == expectedStatus &&
                    (!usesDefaultArmy(kind) || (ledger.stats().refusals > 0) == exceedsReplayGrant);
            }
            else
            {
                valid = valid && !searched.improvement && installed.status == MasterInstallationStatus::NoImprovement;
            }
        }
        record.insert("ok", valid);
        record.insert("search", masterSearchRecord(searched).toVariantMap());
        record.insert("installationStatus", static_cast<int>(installed.status));
        record.insert("finalizationWork", QString::number(installed.finalizationWork));
        if (installed.proof && installed.proof->guardedProof)
        {
            record.insert("installedProof", masterGuardedProofRecord(*installed.proof->guardedProof).toVariantMap());
            record.insert("installedEvaluation", masterEvaluationRecord(installed.proof->challenger).toVariantMap());
        }
        record.insert("masterWork", masterWorkRecord(ledger.stats()).toVariantMap());
        return record;
    }

    template <typename Check>
    bool runNativeGuardedCases(AiArenaTestSupport* support, const QString & progressFile,
                               std::initializer_list<NativeGuardedCase> kinds, Check check, QVariantList & cases)
    {
        for (const auto kind : kinds)
        {
            if (!support->writeJsonResult(progressFile,
                    {{"case", static_cast<int>(kind)}, {"stage", QStringLiteral("before")}}))
            {
                return false;
            }
            auto result = check(kind);
            const bool realmReleased = !Interpreter::getInstance()->isInterrupted();
            result.insert("realmReleased", realmReleased);
            result.insert("ok", result.value("ok").toBool() && realmReleased);
            cases.push_back(result);
            const bool progressWritten = support->writeJsonResult(progressFile,
                {{"case", static_cast<int>(kind)}, {"stage", QStringLiteral("after")}, {"result", result}});
            if (!result.value("ok").toBool() || !progressWritten)
            {
                return false;
            }
        }
        return true;
    }

    QVariantMap nativeGuardedRunRecord(AiArenaTestSupport* support, bool valid, const QVariantList & cases)
    {
        auto* settings = Settings::getInstance();
        return {{"ok", valid}, {"cases", cases}, {"nativeActionsExecuted", 0},
                {"userPath", settings->getUserPath()}, {"noUi", Mainapp::getInstance()->getNoUi()},
                {"noAudio", Mainapp::getInstance()->getNoAudio()},
                {"spawnAiProcess", settings->getSpawnAiProcess()},
                {"standaloneStartup", support->ownsStandaloneStartup()},
                {"battlePresentation", static_cast<int>(settings->getBattleAnimationType())},
                {"battleAnimationMode", static_cast<int>(settings->getBattleAnimationMode())}};
    }

    void finishNativeGuarded(AiArenaTestSupport* support)
    {
        QVariantList cases;
        bool valid = support->ownsStandaloneStartup() && !Interpreter::getInstance()->getInJsCall() &&
            Settings::getInstance()->getSpawnAiProcess() == 0 &&
            Settings::getInstance()->getBattleAnimationType() == GameEnums::BattleAnimationType_Overworld;
        valid = valid && runNativeGuardedCases(support, NATIVE_GUARDED_PROGRESS,
            {NativeGuardedCase::Valid, NativeGuardedCase::ChangedProgress, NativeGuardedCase::ChangedRealm,
             NativeGuardedCase::InsufficientSearchFunding, NativeGuardedCase::UnsupportedFireRange,
             NativeGuardedCase::UnsupportedMovedSniper, NativeGuardedCase::UnsupportedActionIds,
             NativeGuardedCase::ChangedBattleSpeed, NativeGuardedCase::ChangedTerrainAnimation,
             NativeGuardedCase::DefaultArmy, NativeGuardedCase::ChangedArmySource,
             NativeGuardedCase::ChangedArmyTable, NativeGuardedCase::ChangedArmyCallback, NativeGuardedCase::LongerArmyEntry,
             NativeGuardedCase::UnlimitedAmmunition, NativeGuardedCase::ChangedUnlimitedAmmunition},
            [](NativeGuardedCase kind)
            {
                NativeGuardedUnits units{};
                const auto map = nativeGuardedMap(units, kind);
                if (!map)
                {
                    return QVariantMap{{"ok", false}, {"error", QStringLiteral("native guarded map setup failed")}};
                }
                return checkNativeGuarded(kind, map, units, NativeGuardedScreen::Headless);
            }, cases);
        auto record = nativeGuardedRunRecord(support, valid, cases);
        record.insert("aiInitialized", false);
        valid = support->writeJsonResult(NATIVE_GUARDED_OUTPUT, record) && valid;
        support->finish(valid ? ArenaExitCode::Ok : ArenaExitCode::AssertFailed);
    }

    bool installCommandControlRules(GameMap & map)
    {
        auto* rules = map.getGameRules();
        auto* noUnits = rules->getVictoryRule(QStringLiteral("VICTORYRULE_NOUNITS"));
        if (!noUnits || rules->getVictoryRuleSize() != 2)
        {
            return false;
        }
        noUnits->getVariables()->createVariable(QStringLiteral("SpinboxValue1"))->writeDataInt32(1);
        // Reproduce an already initialized, deserialized rule without installing the mod's unrelated action hooks.
        auto rule = MemoryManagement::create<VictoryRule>(&map);
        QByteArray header;
        QDataStream write(&header, QIODevice::WriteOnly);
        write << rule->getVersion() << QStringLiteral("CMODE_CO_COMMAND");
        rule->getVariables()->serializeObject(write);
        QDataStream input(header);
        rule->deserializeObject(input);
        constexpr std::array<int, 19> values{1, 2, 5, 0, 0, 0, 0, 0, 1, 0, 0, 0, 2, 1, 0, 1, 1, 1, 1};
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            rule->getVariables()->createVariable(QStringLiteral("SpinboxValue%1").arg(i))->writeDataInt32(values[i]);
        }
        rules->addVictoryRule(rule);
        const auto stored = [&] {
            QByteArray snapshot;
            QDataStream stream(&snapshot, QIODevice::WriteOnly);
            rule->getVariables()->serializeObject(stream);
            return snapshot;
        };
        const auto before = stored();
        const auto firstHash = map.getMapHash();
        return write.status() == QDataStream::Ok && input.status() == QDataStream::Ok && rules->getVictoryRuleSize() == 3 &&
               !firstHash.isEmpty() && firstHash == map.getMapHash() && before == stored();
    }

    bool loadCommandFixtureScript(const QDir & root, const char* path, qsizetype size, const char* expected)
    {
        QFile file(root.filePath(QString::fromLatin1(path)));
        if (!file.open(QIODevice::ReadOnly) || file.size() != size)
        {
            return false;
        }
        const auto code = file.read(size + 1);
        return QCryptographicHash::hash(code, QCryptographicHash::Sha256).toHex() == expected &&
            Interpreter::getInstance()->loadScript(QString::fromUtf8(code), file.fileName());
    }

    QVariantList spawnedUnitMaterial(const NativeGuardedUnits & units, const QString & type)
    {
        QVariantList rows;
        for (auto* unit : units)
        {
            if (unit->getUnitID() == type)
            {
                rows.push_back(QVariantList{unit->getAmmo1(), unit->getMaxAmmo1(), unit->hasAmmo1(), unit->getWeapon1ID(),
                    unit->getAmmo2(), unit->getMaxAmmo2(), unit->getWeapon2ID(), unit->getFuel(), unit->getMaxFuel(),
                    unit->getBaseMovementPoints(), unit->getBaseMinRange(), unit->getBaseMaxRange(), unit->getBaseVision(),
                    unit->getBaseCosts(), unit->getUnitCosts()});
            }
        }
        return rows;
    }

    bool installMovementFixtureHooks(const QDir & root, QJSValue unit, QJSValue trap)
    {
        auto* realm = Interpreter::getInstance();
        auto global = realm->globalObject();
        QJSValue postAction;
        QJSValue isTrap;
        {
            const std::array names{"UNIT", "ACTION_TRAP", "Global", "PLAYER", "ACTION_FIRE",
                                   "ACTION_NEXT_PLAYER", "ACTION_BUILD_UNITS", "unitSpriteManager"};
            std::array<QJSValue, names.size()> saved;
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                saved[i] = global.property(QString::fromLatin1(names[i]));
                global.setProperty(QString::fromLatin1(names[i]), realm->newObject());
            }
            const auto restore = qScopeGuard([&] {
                for (std::size_t i = 0; i < names.size(); ++i)
                {
                    global.setProperty(QString::fromLatin1(names[i]), saved[i]);
                }
            });
            global.setProperty(QStringLiteral("unitSpriteManager"), QJSValue(QJSValue::UndefinedValue));
            global.property(QStringLiteral("UNIT")).setProperty(QStringLiteral("postAction"), QJSValue(QJSValue::NullValue));
            global.property(QStringLiteral("ACTION_TRAP")).setProperty(QStringLiteral("isTrap"), trap.property(QStringLiteral("isTrap")));
            struct Module
            {
                const char* path;
                qsizetype size;
                const char* hash;
            };
            const std::array modules{
                Module{"mods/custom_gamemodes/scripts/general/cmode_utils.js", 37192,
                    "c0e3a9279b612c01778b4c7fa2b1410a98b30d341f620d364739923d3fd71189"},
                Module{"mods/custom_gamemodes/scripts/gamerules/victory/cmode_chess_lite.js", 2270,
                    "5270498aa3f6df32be7ebdd66c25d6dccd88d6c42feffcc226b9a3d3b246d13e"},
                Module{"mods/custom_gamemodes/scripts/gamerules/victory/cmode_chess_std.js", 2332,
                    "d40aa7f11fb825c5ce5e6a131b0d718948e6ec5dd4719d8ca995993d09ede306"},
                Module{"mods/custom_gamemodes/scripts/gamerules/victory/cmode_chess_true.js", 2316,
                    "705f0fce882b1bc2a6032b0bbde4479f0a33d89bbdc1ccb5d3787e47eb4f75c1"}};
            for (const auto & module : modules)
            {
                if (!loadCommandFixtureScript(root, module.path, module.size, module.hash)) { return false; }
            }
            if (realm->evaluate(QStringLiteral(
                "Global.CMODE_CHESS_LITE=CMODE_CHESS_LITE; Global.CMODE_CHESS_STD=CMODE_CHESS_STD;"
                "Global.CMODE_CHESS_TRUE=CMODE_CHESS_TRUE;")).isError() ||
                !loadCommandFixtureScript(root, "mods/doctrine_frontiers/scripts/general/WardenBarrage.js", 28043,
                    "8a68eb6c9c7943f1055e1a94432a78e667a7558522bf9baa3358c356bdf3c5ec") ||
                realm->evaluate(QStringLiteral("WARDEN_BARRAGE.installHooks();")).isError() ||
                !loadCommandFixtureScript(root, "mods/custom_gamemodes/scripts/general/cmode_procedural_frontier.js", 29621,
                    "2c6e444bcfe8aad3aba3fd0250e655497ac9f2ff93c916f3079e993596984094") ||
                realm->evaluate(QStringLiteral("CMODE_PROCGEN.installPatches();")).isError())
            {
                return false;
            }
            postAction = global.property(QStringLiteral("UNIT")).property(QStringLiteral("postAction"));
            isTrap = global.property(QStringLiteral("ACTION_TRAP")).property(QStringLiteral("isTrap"));
        }
        if (!postAction.isCallable() || !isTrap.isCallable()) { return false; }
        unit.setProperty(QStringLiteral("postAction"), postAction);
        trap.setProperty(QStringLiteral("isTrap"), isTrap);
        return unit.property(QStringLiteral("postAction")).strictlyEquals(postAction) &&
            trap.property(QStringLiteral("isTrap")).strictlyEquals(isTrap);
    }

    bool nativeMovementPoints(const QJSValue & value, const QVector<QPoint> & expected)
    {
        if (!value.isObject() || value.property(QStringLiteral("length")).toInt() != expected.size()) { return false; }
        for (qsizetype i = 0; i < expected.size(); ++i)
        {
            const auto point = value.property(static_cast<quint32>(i));
            if (point.property(QStringLiteral("x")).toInt() != expected[i].x() ||
                point.property(QStringLiteral("y")).toInt() != expected[i].y())
            {
                return false;
            }
        }
        return true;
    }

    bool nativeMovementSequence(const QJSValue & receiver, const QString & method, const QVector<QPoint> & expected)
    {
        auto value = receiver.property(method).callWithInstance(receiver);
        if (expected.isEmpty() || !nativeMovementPoints(value, expected)) { return false; }
        auto changed = expected;
        changed[0].setX(99);
        // Qt point references write back within the JavaScript statement that created them.
        const auto mutate = Interpreter::getInstance()->evaluate(
            QStringLiteral("(function(points) { points[0].x = 99; return points; })"));
        const auto result = mutate.call({value});
        return !result.isError() && result.strictlyEquals(value) && nativeMovementPoints(value, changed);
    }

    bool checkMovementFixtureHooks(GameMap & map, Unit & unit)
    {
        auto* realm = Interpreter::getInstance();
        const auto priorPath = unit.getAiMovePath();
        const auto restore = qScopeGuard([&] { unit.setAiMovePath(priorPath); });
        const QVector<QPoint> path{{4, 1}, {3, 1}, {2, 1}};
        unit.setAiMovePath(path);
        GameAction action(QStringLiteral("ACTION_WAIT"), &map, 314159);
        action.setMovepath(path, 2);
        action.setMultiTurnPath(path);
        const auto actionJs = JsThis::getJsThis(&action);
        const auto unitJs = JsThis::getJsThis(&unit);
        if (!nativeMovementSequence(actionJs, QStringLiteral("getMovePath"), path) ||
            !nativeMovementSequence(actionJs, QStringLiteral("getMultiTurnPath"), path) ||
            !nativeMovementSequence(unitJs, QStringLiteral("getAiMovePath"), path) ||
            action.getMovePath() != path || action.getMultiTurnPath() != path || unit.getAiMovePath() != path)
        {
            return false;
        }
        const auto hash = map.getMapHash();
        const auto module = realm->globalObject().property(QStringLiteral("UNIT"));
        const auto result = module.property(QStringLiteral("postAction")).callWithInstance(
            module, {unitJs, actionJs, GameMap::getMapJsThis(&map)});
        return !result.isError() && !hash.isEmpty() && hash == map.getMapHash() &&
            realm->globalObject().property(QStringLiteral("WARDEN_BARRAGE"))
                .property(QStringLiteral("resolvedAction")).strictlyEquals(actionJs);
    }

    QVariantList lightTankFireRefusals(QJSValue tank, const RepairControl & stock)
    {
        auto* realm = Interpreter::getInstance();
        const auto unit = realm->globalObject().property(QStringLiteral("UNIT"));
        const auto initializer = tank.property(QStringLiteral("init"));
        const auto cost = tank.property(QStringLiteral("getBaseCost"));
        enum class Alternative { Init, Cost, Both };
        QVariantList cases;
        for (const auto alternative : {Alternative::Init, Alternative::Cost, Alternative::Both})
        {
            tank.setProperty(QStringLiteral("init"), alternative == Alternative::Cost ? stock.stockInit : initializer);
            tank.setProperty(QStringLiteral("getBaseCost"), alternative == Alternative::Init ? stock.stockCost : cost);
            NativeGuardedUnits units{};
            const auto map = nativeGuardedMap(units, NativeGuardedCase::DefaultArmy);
            const bool sharedHooksNull = unit.property(QStringLiteral("postAction")).isNull() &&
                unit.property(QStringLiteral("getWeatherImmune")).isNull();
            if (!map || !sharedHooksNull || !installCommandControlRules(*map) ||
                !map->spawnUnit(6, 3, QStringLiteral("LIGHT_TANK"), map->getPlayer(ENEMY_PLAYER),
                                EXACT_SPAWN_RANGE, IGNORE_SPAWN_MOVEMENT))
            {
                cases.push_back(QVariantMap{{"ok", false}, {"error", QStringLiteral("Light Tank FIRE fixture setup failed")}});
                break;
            }
            QString error;
            const auto fixture = nativeGuardedFixture(map, units, NativeGuardedCase::DefaultArmy, error);
            if (!fixture)
            {
                cases.push_back(QVariantMap{{"ok", false}, {"error", error}});
                break;
            }
            auto source = prepareMasterNativeSource(*map, *fixture->extraction.problem, fixture->ledger);
            const auto & fire = source.collectionCosts[static_cast<std::size_t>(MasterNativeCollectionStage::Fire)];
            const bool refused = source.status == MasterNativeSourceStatus::UnsupportedCallbacks &&
                fire.observations == 1 && fire.work > 0 && fixture->ledger.stats().refusals == 0 &&
                source.source && !source.source->maximumPreparationWork() && !source.source->maximumAuthenticationWork();
            cases.push_back(QVariantMap{{"ok", refused}, {"alternative", static_cast<int>(alternative)},
                {"nativeSourceStatus", masterNativeSourceStatusName(source.status)}, {"sharedHooksNull", sharedHooksNull},
                {"sourceWork", QString::number(source.work)}, {"fireStageWork", QString::number(fire.work)},
                {"budgetRefusals", static_cast<qulonglong>(fixture->ledger.stats().refusals)}});
        }
        return cases;
    }

    bool installCallbackFixtureHooks(const QDir & root, CallbackFixture & fixture, bool buildings)
    {
        auto* realm = Interpreter::getInstance();
        auto global = realm->globalObject();
        const auto unit = global.property(QStringLiteral("UNIT"));
        const auto trap = global.property(QStringLiteral("ACTION_TRAP"));
        const auto post = unit.property(QStringLiteral("postAction"));
        const auto isTrap = trap.property(QStringLiteral("isTrap"));
        fixture.waitPerform = global.property(QStringLiteral("ACTION_WAIT")).property(QStringLiteral("perform"));
        const std::array functions{
            std::pair{"ACTION_WAIT", "perform"}, std::pair{"ACTION_CAPTURE", "canBePerformed"},
            std::pair{"ACTION_CAPTURE", "newCanBePerformed"}, std::pair{"ACTION_CAPTURE", "perform"},
            std::pair{"ACTION_CAPTURE", "performPostAnimation"}, std::pair{"ACTION_NEXT_PLAYER", "canBePerformed"},
            std::pair{"ACTION_NEXT_PLAYER", "perform"}, std::pair{"PLAYER", "startOfTurn"},
            std::pair{"PLAYER", "endOfTurn"}, std::pair{"Global", "getArmyNameFromPlayerTable"},
            std::pair{"ACTION_CAPTURE", "__db_curse_patched"},
            std::pair{"ACTION_NEXT_PLAYER", "__db_pending_pick_patched"},
            std::pair{"ACTION_NEXT_PLAYER", "__cmode_chess_base_patched"},
            std::pair{"PLAYER", "__db_patched"}, std::pair{"PLAYER", "__db_endpatched"},
            std::pair{"UNIT", "__cmode_chess_base_patched"}};
        for (const auto & [object, name] : functions)
        {
            fixture.save(global.property(QString::fromLatin1(object)), QString::fromLatin1(name));
        }
        const auto frontierKey = global.property(QStringLiteral("CMODE_PROCGEN")).property(QStringLiteral("PATCH_SENTINEL")).toString();
        if (frontierKey.isEmpty() || frontierKey.size() > 128) { return false; }
        fixture.save(unit, frontierKey);
        fixture.save(global.property(QStringLiteral("PLAYER")), frontierKey);
        for (const auto* object : {"ACTION_WAIT", "ACTION_CAPTURE"})
        {
            auto action = global.property(QString::fromLatin1(object));
            for (const auto* key : {"__cmode_co_command_origin", "__cmode_co_command_building"})
            {
                fixture.save(action, QString::fromLatin1(key));
                action.setProperty(QString::fromLatin1(key), QJSValue(QJSValue::NullValue));
            }
        }
        auto chess = global.property(QStringLiteral("CMODE_CHESS_BASE"));
        const auto previousNames = chess.property(QStringLiteral("_ZCO_NAMES"));
        const auto build = global.property(QStringLiteral("ACTION_BUILD_UNITS"));
        const auto manager = global.property(QStringLiteral("unitSpriteManager"));
        const auto restore = qScopeGuard([&] {
            global.setProperty(QStringLiteral("ACTION_BUILD_UNITS"), build);
            global.setProperty(QStringLiteral("unitSpriteManager"), manager);
            chess.setProperty(QStringLiteral("_ZCO_NAMES"), previousNames);
        });
        global.setProperty(QStringLiteral("ACTION_BUILD_UNITS"), QJSValue(QJSValue::UndefinedValue));
        global.setProperty(QStringLiteral("unitSpriteManager"), QJSValue(QJSValue::UndefinedValue));
        chess.setProperty(QStringLiteral("_ZCO_NAMES"), realm->newArray());
        global.property(QStringLiteral("UNIT")).setProperty(QStringLiteral("__cmode_chess_base_patched"), true);
        global.property(QStringLiteral("UNIT")).setProperty(frontierKey, true);
        if (realm->evaluate(QStringLiteral("CMODE_CHESS_BASE.installSharedPatches();")).isError() ||
            !loadCommandFixtureScript(root, "mods/zzzzprism_star/scripts/actions/ACTION_CAPTURE.js", 2671,
                "932f9d4270c337b8dec68813b3e0e2871885010110c485bb24022d4835ce78b9") ||
            !loadCommandFixtureScript(root, "mods/custom_gamemodes/scripts/gamerules/victory/cmode_deckbuilder.js", 14896,
                "58c4049e582feac9b19c9abfb36cb3a7d53db437b1f5022c373c64657988a1ab") ||
            realm->evaluate(QStringLiteral(
                "CMODE_PROCGEN.installPatches();"
                "CO_COMMAND.ensureActionPatch(ACTION_WAIT,'ACTION_WAIT');"
                "CO_COMMAND.ensureActionPatch(ACTION_CAPTURE,'ACTION_CAPTURE');"
                "CO_COMMAND.ensureActionPatch(ACTION_NEXT_PLAYER,'ACTION_NEXT_PLAYER');"
                "CO_COMMAND.ensureActionSpriteObject(ACTION_WAIT,'ACTION_WAIT');"
                "CO_COMMAND.ensureActionSpriteObject(ACTION_CAPTURE,'ACTION_CAPTURE');"
                "CO_COMMAND.ensureActionSpriteObject(ACTION_NEXT_PLAYER,'ACTION_NEXT_PLAYER');"
                "CO_COMMAND.ensureArmyNamePatch();")).isError())
        {
            return false;
        }
        if (buildings)
        {
            for (std::size_t index = 0; index < BUILDING_HOOK_CONTROLS.size(); ++index)
            {
                const auto & hook = BUILDING_HOOK_CONTROLS[index];
                auto object = global.property(QString::fromLatin1(hook.global));
                const auto member = QString::fromLatin1(hook.member);
                fixture.save(object, member);
                fixture.buildingDelegates[index] = object.property(member);
                if (!fixture.buildingDelegates[index].isCallable()) { return false; }
            }
            if (realm->evaluate(QStringLiteral(
                "CO_COMMAND.ensureBuildingSpriteHook(TOWN,'TOWN');"
                "CO_COMMAND.ensureBuildingSpriteHook(FACTORY,'FACTORY');"
                "CO_COMMAND.ensureBuildingSpriteHook(HQ,'HQ');")).isError())
            {
                return false;
            }
            for (std::size_t index = 0; index < BUILDING_HOOK_CONTROLS.size(); ++index)
            {
                const auto & hook = BUILDING_HOOK_CONTROLS[index];
                const auto current = global.property(QString::fromLatin1(hook.global)).property(QString::fromLatin1(hook.member));
                if (!current.isCallable() || current.strictlyEquals(fixture.buildingDelegates[index])) { return false; }
            }
            fixture.buildings = true;
        }
        fixture.active = unit.property(QStringLiteral("postAction")).strictlyEquals(post) &&
            trap.property(QStringLiteral("isTrap")).strictlyEquals(isTrap);
        return fixture.active;
    }

    bool checkBuildingBadgeFixture(GameMap & map)
    {
        auto* realm = Interpreter::getInstance();
        const auto command = realm->globalObject().property(QStringLiteral("CO_COMMAND"));
        const auto mapValue = GameMap::getMapJsThis(&map);
        auto* building = map.getTerrain(0, 0)->getBuilding();
        if (!building || building->getOwner() != map.getPlayer(0)) { return false; }
        const auto before = map.getMapHash();
        const auto active = command.property(QStringLiteral("isActive")).call({mapValue});
        const auto mode = command.property(QStringLiteral("getBuilderMode")).call({mapValue});
        const auto enabled = command.property(QStringLiteral("usesProductionBadges")).call({mapValue});
        if (!active.isBool() || !active.toBool() || !mode.isNumber() || mode.toNumber() != 2 ||
            !enabled.isBool() || !enabled.toBool())
        {
            return false;
        }
        for (const bool neutral : {false, true})
        {
            const auto icon = command.property(QStringLiteral("getProductionBuildingBadgeIcon")).call(
                {JsThis::getJsThis(building), mapValue, QJSValue(neutral)});
            if (!icon.isString() || !icon.toString().isEmpty()) { return false; }
        }
        return before == map.getMapHash();
    }

    void finishNativeCommand(AiArenaTestSupport* support, bool wrappedRepair, bool awds, bool weather, bool movement, LightTankControls tankControls)
    {
        QVariantList cases;
        const bool lightTank = tankControls != LightTankControls::Off;
        const bool awdsLightTank = lightTank && tankControls != LightTankControls::StockMovement;
        const bool fireControls = tankControls == LightTankControls::Fire;
        const bool buildingCallbacks = tankControls == LightTankControls::Buildings;
        const bool actionCallbacks = tankControls == LightTankControls::Callbacks || buildingCallbacks;
        CallbackFixture callbackFixture;
        bool ok = support->ownsStandaloneStartup() && !Interpreter::getInstance()->getInJsCall() &&
            Settings::getInstance()->getSpawnAiProcess() == 0 && (!lightTank || (awds && (fireControls || (weather && movement))));
        const QDir root(Settings::getInstance()->getUserPath());
        const std::array paths{"mods/custom_gamemodes/scripts/general/cmode_co_command.js",
                               "mods/custom_gamemodes/scripts/gamerules/victory/cmode_co_command.js"};
        const std::array sizes{374937, 10983};
        const std::array hashes{"4868f249010479b7afb1b913e6d8184223773e2cac242f8b61024b1bddf06e70",
                                "578dc84d9ff6ed5411e6cd5e2ad97a25e6b65907558a2cedfd610b004f6b6826"};
        for (std::size_t i = 0; ok && i < paths.size(); ++i)
        {
            ok = loadCommandFixtureScript(root, paths[i], sizes[i], hashes[i]);
        }
        auto* realm = Interpreter::getInstance();
        auto unit = realm->globalObject().property(QStringLiteral("UNIT"));
        auto infantry = realm->globalObject().property(QStringLiteral("INFANTRY"));
        auto tank = realm->globalObject().property(QStringLiteral("LIGHT_TANK"));
        RepairControl tankRepair{unit.property(QStringLiteral("repairUnit")), {}, tank.property(QStringLiteral("init")),
                                 tank.property(QStringLiteral("getBaseCost"))};
        RepairControl repair{unit.property(QStringLiteral("repairUnit")), {}, infantry.property(QStringLiteral("init")),
                             infantry.property(QStringLiteral("getBaseCost"))};
        if (ok && awds)
        {
            ok = wrappedRepair && loadCommandFixtureScript(root, "mods/awds_unit/scripts/units/infantry.js", 419,
                "e7227b32fc971f3844ae82f0cc931911c2c0b3f8f042117b8117b400790f74cc");
        }
        if (ok && awdsLightTank)
        {
            ok = loadCommandFixtureScript(root, "mods/awds_unit/scripts/units/light_tank.js", 444,
                "02a7a1c8aa4530738765a0ce4f91c82cf759d5211d399ed0f4182cbf452a97cc");
        }
        auto trap = realm->globalObject().property(QStringLiteral("ACTION_TRAP"));
        const auto originalPost = unit.property(QStringLiteral("postAction"));
        const auto originalTrap = trap.property(QStringLiteral("isTrap"));
        if (ok && movement) { ok = weather && installMovementFixtureHooks(root, unit, trap); }
        const auto originalWeather = unit.property(QStringLiteral("getWeatherImmune"));
        if (ok && weather)
        {
            const auto installed = realm->evaluate(QStringLiteral("CO_COMMAND.ensureUnitWeatherObject(UNIT, 'UNIT', true); UNIT.getWeatherImmune;"));
            ok = awds && originalWeather.isNull() && installed.isCallable() && infantry.property(QStringLiteral("getWeatherImmune")).strictlyEquals(installed);
        }
        const auto restoreRepair = qScopeGuard([&] {
            if (movement)
            {
                unit.setProperty(QStringLiteral("postAction"), originalPost);
                trap.setProperty(QStringLiteral("isTrap"), originalTrap);
            }
            if (weather) { unit.setProperty(QStringLiteral("getWeatherImmune"), originalWeather); }
            if (wrappedRepair) { unit.setProperty(QStringLiteral("repairUnit"), repair.original); }
            if (awdsLightTank)
            {
                tank.setProperty(QStringLiteral("init"), tankRepair.stockInit);
                tank.setProperty(QStringLiteral("getBaseCost"), tankRepair.stockCost);
            }
            if (awds)
            {
                infantry.setProperty(QStringLiteral("init"), repair.stockInit);
                infantry.setProperty(QStringLiteral("getBaseCost"), repair.stockCost);
            }
        });
        if (ok && wrappedRepair)
        {
            const auto wait = realm->globalObject().property(QStringLiteral("ACTION_WAIT")).property(QStringLiteral("perform"));
            const auto install = QStringLiteral("CO_COMMAND.ensureRepairPatch(); UNIT.repairUnit;");
            unit.setProperty(QStringLiteral("repairUnit"), wait);
            repair.otherWrapper = realm->evaluate(install);
            unit.setProperty(QStringLiteral("repairUnit"), repair.original);
            const auto actual = realm->evaluate(install);
            ok = wait.isCallable() && repair.original.isCallable() && repair.otherWrapper.isCallable() && actual.isCallable() &&
                !actual.strictlyEquals(repair.original) && !actual.strictlyEquals(repair.otherWrapper) &&
                !repair.otherWrapper.strictlyEquals(wait) && unit.property(QStringLiteral("repairUnit")).strictlyEquals(actual);
        }
        if (ok && actionCallbacks) { ok = installCallbackFixtureHooks(root, callbackFixture, buildingCallbacks); }
        tankRepair.otherWrapper = repair.otherWrapper;
        if (fireControls)
        {
            if (ok) { cases = lightTankFireRefusals(tank, tankRepair); }
            ok = ok && cases.size() == 3 && std::ranges::all_of(cases, [](const auto & item) { return item.toMap().value("ok").toBool(); });
            const auto result = nativeGuardedRunRecord(support, ok, cases);
            ok = support->writeJsonResult(NATIVE_GUARDED_OUTPUT, result) && ok;
            support->finish(ok ? ArenaExitCode::Ok : ArenaExitCode::AssertFailed);
            return;
        }
        const bool selectMovement = movement && tankControls != LightTankControls::Definition && tankControls != LightTankControls::Weather;
        const bool selectWeather = weather && tankControls != LightTankControls::Definition;
        const auto controls = selectMovement
            ? std::array{CommandControl::Install, CommandControl::PostAction, CommandControl::TrapCallback,
                         CommandControl::ChessPeer, CommandControl::BarrageHelper, CommandControl::BarrageVariable,
                         CommandControl::ActionMarker, CommandControl::ArrayPush}
            : selectWeather
            ? std::array{CommandControl::Install, CommandControl::WeatherMember, CommandControl::WeatherHelper,
                         CommandControl::WeatherPending, CommandControl::WeatherOrigin, CommandControl::WeatherKey,
                         CommandControl::WeatherVariables, CommandControl::UnitReceiver}
            : awds
            ? std::array{CommandControl::Install, CommandControl::StockInit, CommandControl::StockCost,
                         CommandControl::PostInit, CommandControl::Movement, CommandControl::Capacity, CommandControl::Vision, CommandControl::UnitReceiver}
            : wrappedRepair
            ? std::array{CommandControl::Install, CommandControl::Delegate, CommandControl::OwnApply,
                         CommandControl::PrototypeApply, CommandControl::ActiveCo, CommandControl::ActorRepair, CommandControl::UnitReceiver, CommandControl::MapReceiver}
            : std::array{CommandControl::Install, CommandControl::Action, CommandControl::Repair,
                         CommandControl::HashCallback, CommandControl::Setting, CommandControl::StreamError, CommandControl::Receiver, CommandControl::ActorRepair};
        const std::array callbackControls{
            CommandControl::Install, CommandControl::CallbackHelper, CommandControl::CallbackResolver,
            CommandControl::CaptureDelegate, CommandControl::CallbackLeaf, CommandControl::SpriteContext,
            CommandControl::OriginMarker, CommandControl::ArrayIsArray, CommandControl::StringUpper,
            CommandControl::EmbargoVariable, CommandControl::SpriteSettingStream, CommandControl::AnimationPrototype};
        std::vector<CommandControl> buildingControls{CommandControl::Install};
        for (const auto & hook : BUILDING_HOOK_CONTROLS) { buildingControls.push_back(hook.fault); }
        for (const auto & helper : BUILDING_HELPER_CONTROLS) { buildingControls.push_back(helper.first); }
        for (const auto control : {CommandControl::ForeignBuildingSprites, CommandControl::ForeignBuildingWeather,
                                   CommandControl::BuildingBadgeKey, CommandControl::BuildingBadgeStream,
                                   CommandControl::BuildingReceiver, CommandControl::DirectBuildingWrapper})
        {
            buildingControls.push_back(control);
        }
        auto selectedControls = actionCallbacks ? std::span<const CommandControl>(callbackControls) : std::span<const CommandControl>(controls);
        if (buildingCallbacks) { selectedControls = buildingControls; }
        for (const auto change : selectedControls)
        {
            if (!ok)
            {
                break;
            }
            NativeGuardedUnits units{};
            const auto map = nativeGuardedMap(units, NativeGuardedCase::DefaultArmy, weather, lightTank, buildingCallbacks);
            ok = map && installCommandControlRules(*map);
            if (ok && buildingCallbacks) { ok = checkBuildingBadgeFixture(*map); }
            if (ok && movement) { ok = checkMovementFixtureHooks(*map, *units[NATIVE_CAPTURE_ACTOR]); }
            if (ok)
            {
                const auto population = awds ? spawnedUnitMaterial(units, QStringLiteral("INFANTRY")) : QVariantList{};
                const auto tankPopulation = lightTank ? spawnedUnitMaterial(units, QStringLiteral("LIGHT_TANK")) : QVariantList{};
                auto record = checkNativeGuarded(NativeGuardedCase::DefaultArmy, map, units, NativeGuardedScreen::Headless,
                                                 change, lightTank ? &tankRepair : wrappedRepair ? &repair : nullptr, weather,
                                                 lightTank ? units[NATIVE_FIRE_ACTOR] : nullptr,
                                                 actionCallbacks ? &callbackFixture : nullptr);
                if (awds)
                {
                    const QVariantList expected{-1, -1, true, QStringLiteral("WEAPON_INFANTRY_MG"), 0, 0, QString{},
                        100, 100, 3, 1, 1, 2, 1000, 1000};
                    record.insert("spawnedInfantry", population);
                    if (population.size() != (weather ? 5 : 4) - int(lightTank) || !std::ranges::all_of(population, [&](const auto & row) { return row.toList() == expected; }))
                    {
                        record.insert("ok", false);
                        record.insert("spawnMismatch", true);
                    }
                }
                if (lightTank)
                {
                    const auto & tanks = tankPopulation;
                    const int primary = awdsLightTank ? 9 : 6;
                    const int secondary = awdsLightTank ? -1 : 10;
                    const QVariantList expected{primary, primary, true, QStringLiteral("WEAPON_LIGHT_TANK_GUN"),
                        secondary, secondary, QStringLiteral("WEAPON_LIGHT_TANK_MG"), 70, 70, 6, 1, 1,
                        awdsLightTank ? 3 : 2, 7000, 7000};
                    record.insert("spawnedLightTanks", tanks);
                    record.insert("lightTankControls", static_cast<int>(tankControls));
                    if (tanks.size() != 1 || tanks.front().toList() != expected ||
                        units[NATIVE_FIRE_ACTOR]->getUniqueID() >= units[NATIVE_CAPTURE_ACTOR]->getUniqueID())
                    {
                        record.insert("ok", false);
                        record.insert("tankSpawnMismatch", true);
                    }
                }
                record.insert("buildingBadgeFixture", buildingCallbacks);
                record.insert("wrappedRepair", wrappedRepair);
                record.insert("commandWeather", weather);
                record.insert("movementHooks", movement);
                record.insert("commandCase", static_cast<int>(change));
                ok = record.value("ok").toBool();
                cases.push_back(record);
            }
        }
        const auto result = nativeGuardedRunRecord(support, ok, cases);
        ok = support->writeJsonResult(NATIVE_GUARDED_OUTPUT, result) && ok;
        support->finish(ok ? ArenaExitCode::Ok : ArenaExitCode::AssertFailed);
    }

    QVariantMap queueNativeGuarded(QObject* context, const QVariantMap & options)
    {
        auto* support = qobject_cast<AiArenaTestSupport*>(context);
        if (support == nullptr || !support->claimStandaloneStartup())
        {
            return {{"ok", false}, {"error", QStringLiteral("native guarded standalone startup unavailable")}};
        }
        const bool command = options.value("commandMode").toBool();
        const bool repair = options.value("wrappedRepair").toBool();
        const bool awds = options.value("awdsInfantry").toBool();
        const bool weather = options.value("commandWeather").toBool();
        const bool movement = options.value("movementHooks").toBool();
        const auto tankControls = static_cast<LightTankControls>(options.value("lightTankControls").toInt());
        QTimer::singleShot(0, support, [support, command, repair, awds, weather, movement, tankControls]() {
            if (command) { finishNativeCommand(support, repair, awds, weather, movement, tankControls); }
            else { finishNativeGuarded(support); }
        });
        return {{"ok", true}, {"queued", true}};
    }

    struct NativeDetailScreen
    {
        spGameMap map;
        NativeGuardedUnits units{};
        spGameMenue menu;
        QElapsedTimer waiting;
    };

    // A detail capture refused by live facts must not leave the source reporting a detail presentation.
    QVariantMap checkFailedDetailCapture(const NativeDetailScreen & screen)
    {
        QString error;
        const auto refusedFixture = nativeGuardedFixture(screen.map, screen.units, NativeGuardedCase::Valid, error);
        const auto overworldFixture = nativeGuardedFixture(screen.map, screen.units, NativeGuardedCase::Valid, error);
        if (!refusedFixture || !overworldFixture)
        {
            return {{"ok", false}, {"error", error}};
        }
        auto* settings = Settings::getInstance();
        const BattlePresentationRestore presentation;
        settings->setBattleAnimationMode(GameEnums::BattleAnimationMode_All);
        const auto refused = prepareMasterNativeSource(
            *screen.map, *refusedFixture->extraction.problem, refusedFixture->ledger);
        settings->setBattleAnimationType(GameEnums::BattleAnimationType_Overworld);
        auto overworld = prepareMasterNativeSource(
            *screen.map, *overworldFixture->extraction.problem, overworldFixture->ledger);
        // A ready source fences the realm until it is destroyed.
        overworld.source.reset();
        // The detail capture is the last fire step, so matching an overworld run's modules places the refusal there.
        const bool interrupted = Interpreter::getInstance()->isInterrupted();
        const bool ok = refused.status == MasterNativeSourceStatus::UnsupportedCallbacks &&
            !refused.detailPresentation && overworld.status == MasterNativeSourceStatus::Ready &&
            !overworld.detailPresentation && refused.boundModules > 0 &&
            refused.boundModules == overworld.boundModules && !interrupted;
        return {{"ok", ok}, {"interrupted", interrupted}, {"overworldDetailPresentation", overworld.detailPresentation},
                {"nativeSourceStatus", masterNativeSourceStatusName(refused.status)},
                {"detailPresentation", refused.detailPresentation},
                {"boundModules", static_cast<qulonglong>(refused.boundModules)},
                {"overworldStatus", masterNativeSourceStatusName(overworld.status)},
                {"overworldBoundModules", static_cast<qulonglong>(overworld.boundModules)}};
    }

    void finishNativeDetail(AiArenaTestSupport* support, const std::shared_ptr<NativeDetailScreen> & screen,
                            bool started)
    {
        QVariantList cases;
        const auto failedCapture = started ? checkFailedDetailCapture(*screen) : QVariantMap{};
        // Each case captures afresh; the terrain case still goes last so the others see the seeded value.
        bool valid = started && failedCapture.value("ok").toBool() && runNativeGuardedCases(support, NATIVE_DETAIL_PROGRESS,
            {NativeGuardedCase::Valid, NativeGuardedCase::ChangedBattleSpeed, NativeGuardedCase::ChangedTerrainAnimation},
            [&screen](NativeGuardedCase kind)
            {
                return checkNativeGuarded(kind, screen->map, screen->units, NativeGuardedScreen::Detail);
            }, cases);
        auto record = nativeGuardedRunRecord(support, valid, cases);
        record.insert("failedDetailCapture", failedCapture);
        record.insert("gameStarted", started);
        record.insert("startWaitMs", QString::number(screen->waiting.elapsed()));
        record.insert("qpaPlatform", QString::fromLocal8Bit(qgetenv("QT_QPA_PLATFORM")));
        valid = support->writeJsonResult(NATIVE_DETAIL_OUTPUT, record) && valid;
        support->finish(valid ? ArenaExitCode::Ok : ArenaExitCode::AssertFailed);
    }

    void awaitNativeDetail(AiArenaTestSupport* support, const std::shared_ptr<NativeDetailScreen> & screen)
    {
        const bool quiescent = screen->menu->getGameStarted() && GameAnimationFactory::getAnimationCount() == 0 &&
            !screen->menu->getActionPerformer()->getActionRunning();
        if (quiescent || screen->waiting.elapsed() > NATIVE_DETAIL_START_LIMIT_MS)
        {
            finishNativeDetail(support, screen, quiescent);
            return;
        }
        QTimer::singleShot(NATIVE_DETAIL_POLL_MS, support, [support, screen]() { awaitNativeDetail(support, screen); });
    }

    void startNativeDetail(AiArenaTestSupport* support)
    {
        auto* settings = Settings::getInstance();
        auto screen = std::make_shared<NativeDetailScreen>();
        screen->waiting.start();
        if (Mainapp::getInstance()->getNoUi() || !Mainapp::getInstance()->getNoAudio() ||
            settings->getSpawnAiProcess() != 0 ||
            settings->getBattleAnimationType() != GameEnums::BattleAnimationType_Detail ||
            settings->getBattleAnimationMode() != GameEnums::BattleAnimationMode_None)
        {
            support->writeJsonResult(NATIVE_DETAIL_OUTPUT, nativeGuardedRunRecord(support, false, {}));
            support->finish(ArenaExitCode::SetupError);
            return;
        }
        screen->map = nativeGuardedMap(screen->units, NativeGuardedCase::Valid);
        bool paused = screen->map != nullptr;
        // A paused AI never acts on its turn, so the started screen stays quiescent for the checks.
        for (qint32 player = 0; paused && player < screen->map->getPlayerCount(); ++player)
        {
            auto* ai = dynamic_cast<CoreAI*>(screen->map->getPlayer(player)->getBaseGameInput());
            if (ai != nullptr && !ai->getisAiPaused())
            {
                ai->toggleAiPause();
            }
            paused = ai != nullptr && ai->getisAiPaused();
        }
        if (!paused)
        {
            support->writeJsonResult(NATIVE_DETAIL_OUTPUT, nativeGuardedRunRecord(support, false, {}));
            support->finish(ArenaExitCode::SetupError);
            return;
        }
        constexpr bool SAVED_GAME = true;
        screen->menu = MemoryManagement::create<GameMenue>(screen->map, SAVED_GAME, spNetworkInterface());
        oxygine::Stage::getStage()->addChild(screen->menu);
        awaitNativeDetail(support, screen);
    }

    QVariantMap queueNativeDetail(QObject* context, const QVariantMap &)
    {
        auto* support = qobject_cast<AiArenaTestSupport*>(context);
        if (support == nullptr || !support->claimStandaloneStartup())
        {
            return {{"ok", false}, {"error", QStringLiteral("native detail standalone startup unavailable")}};
        }
        QTimer::singleShot(0, support, [support]() { startNativeDetail(support); });
        return {{"ok", true}, {"queued", true}};
    }

    [[maybe_unused]] const bool REGISTERED = AiArenaTestSupport::registerOperation(
        QStringLiteral("masterStockAdmission"), runAdmission);
    [[maybe_unused]] const bool NATIVE_REGISTERED = AiArenaTestSupport::registerOperation(
        QStringLiteral("masterNativeGuarded"), queueNativeGuarded);
    [[maybe_unused]] const bool NATIVE_DETAIL_REGISTERED = AiArenaTestSupport::registerOperation(
        QStringLiteral("masterNativeDetail"), queueNativeDetail);
}
