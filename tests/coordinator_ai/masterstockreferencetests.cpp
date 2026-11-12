#include <array>

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include "ai/coordinator/masterassignmentshadow.h"
#include "ai/coordinator/masterassignmenttrace.h"
#include "tests/arena/masterstockreference.h"
#include "tests/coordinator_ai/masterassignmentstockfixture.h"

namespace
{
    using namespace Coordinator;
    using namespace MasterAssignmentTest;
    using ArenaMasterComparisonProbe::StockReferenceResult;
    constexpr MilliFunds INCOME = 1000000;
    constexpr std::int32_t HORIZON = 10;
    constexpr DecisionTraceIdentity IDENTITY{4, 0, 1, HORIZON};

    QByteArray readFile(const QString & path)
    {
        QFile file(path);
        require(file.open(QIODevice::ReadOnly), "read fixed-reference test output");
        return file.readAll();
    }

    QJsonArray records(const QString & path)
    {
        QJsonArray result;
        const auto bytes = readFile(path);
        require(bytes.endsWith('\n'), "reference stream has final record delimiter");
        for (const auto & line : bytes.split('\n'))
        {
            if (line.isEmpty())
            {
                continue;
            }
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(line, &error);
            require(error.error == QJsonParseError::NoError && document.isObject(), "reference record is complete JSON");
            const auto object = document.object();
            expect(object.value("sequence").toString() == QString::number(result.size()), "reference sequence is contiguous");
            result.append(object);
        }
        expect(result.first().toObject().value("event") == "begin" &&
                   result.last().toObject().value("event") == "end", "reference frame begins and ends once");
        return result;
    }

    MasterAssignmentProblem captureProblem()
    {
        Fixture fixture(1);
        auto wait = moveCandidate(0, {0, 0}, {0, 0}, 0);
        fixture.input.actors[0].candidates[0] = wait;
        auto capture = wait;
        capture.generationIndex = 1;
        capture.bundle.components.push_back(captureComponent({.income = {INCOME, INCOME}, .turnsUntilOwned = 0}));
        fixture.input.actors[0].candidates.push_back(capture);
        for (std::int32_t choice = 2; choice < 8; ++choice)
        {
            fixture.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {0, 0}, choice * INCOME, choice));
        }
        fixture.seat();
        fixture.selections[0].stockRow = 0;
        return fixture.extract();
    }

    // A second capper contending for the town keeps the waiting plan's friendly interval open.
    std::vector<MasterAssignmentOracle::ContinuationCase> captureCases(const MasterAssignmentProblem & problem,
                                                                      bool contested = false)
    {
        using namespace MasterAssignmentOracle;
        const PropertyStockColumn neutral{.slot = 0, .tile = {0, 0},
            .income = {INCOME, INCOME}, .ownerBefore = OwnerSign::Neutral};
        SequentialInput friendly{HORIZON, {neutral}, {{1, {UNREACHABLE}}},
            {{0, {columnStreamValue(neutral, 1, HORIZON)}, {1}}}, {false}};
        if (contested)
        {
            friendly.rows.push_back({0, {columnStreamValue(neutral, 2, HORIZON)}, {2}});
        }
        const SequentialInput enemy{HORIZON, {neutral}, {{1, {UNREACHABLE}}},
            {{0, {columnStreamValue(neutral, 6, HORIZON)}, {6}}}, {false}};
        ContinuationCase waiting{replayMasterAssignment(problem, std::array<std::int32_t, 1>{0}).continuationKey,
            {}, 0, friendly, enemy};
        auto completing = waiting;
        completing.key = replayMasterAssignment(problem, std::array<std::int32_t, 1>{1}).continuationKey;
        completing.capturedColumns = {0};
        completing.owned = ownershipFlipSwing(neutral, HORIZON);
        completing.friendly.capturedNodes = {true};
        for (auto & row : completing.friendly.rows)
        {
            row.weight = {0};
            row.ownedTurn = {NO_CAPTURE_TURNS};
        }
        return {waiting, completing};
    }

    QJsonObject requestObject(const MasterAssignmentProblem & problem, const QJsonArray & family,
                              const QJsonArray & steps = {})
    {
        return {{"day", IDENTITY.day}, {"player", IDENTITY.playerId},
            {"planSequence", QString::number(IDENTITY.planSequence)}, {"sourceRevision", "fixed-reference-test"},
            {"fixture", "fixed-reference-small"}, {"allowance", "1000000"},
            {"modelSHA256", QString::fromLatin1(QCryptographicHash::hash(
                QJsonDocument(masterModelRecord(problem)).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex())},
            {"incumbent", masterChoicesRecord(problem.incumbentChoices())}, {"family", family}, {"steps", steps}};
    }

    ArenaMasterStockReference::Request parseRequest(const MasterAssignmentProblem & problem, const QJsonObject & config)
    {
        QString error;
        const auto parsed = ArenaMasterStockReference::parse(
            QJsonDocument(config).toJson(QJsonDocument::Compact), IDENTITY, problem.actors().size(), error);
        require(parsed.has_value(), "valid fixed-family request parses");
        return *parsed;
    }

    QJsonObject step(int plan, const char* term)
    {
        return {{"plan", plan}, {"term", QLatin1String(term)}, {"allowance", "128"}};
    }

    void parserAndGate()
    {
        MasterShadowConfig config;
        config.mode = MasterAssignmentMode::StockControl;
        config.runtime = MasterStockControlRuntime::PlayableTrial;
        expect(ArenaMasterStockReference::enabled(config, "1"), "authenticated playable stock reference requires explicit gate");
        for (const auto & flag : {QByteArray{}, QByteArray{"0"}, QByteArray{"true"}})
        {
            expect(!ArenaMasterStockReference::enabled(config, flag), "reference gate is exact opt-in");
        }
        for (const auto mode : {MasterAssignmentMode::Disabled, MasterAssignmentMode::Shadow})
        {
            auto other = config;
            other.mode = mode;
            expect(!ArenaMasterStockReference::enabled(other, "1"), "non-stock modes do not invoke reference");
        }
        auto other = config;
        other.runtime = MasterStockControlRuntime::Arena;
        expect(!ArenaMasterStockReference::enabled(other, "1"), "arena does not bypass playable reference admission");
        other = config;
        other.error = MasterShadowConfigError::InvalidTrialUserPath;
        expect(!ArenaMasterStockReference::enabled(other, "1"), "invalid runtime does not invoke reference");

        const auto problem = captureProblem();
        const auto original = requestObject(problem, {QJsonArray{0}, QJsonArray{1}}, {step(0, "enemy")});
        for (int fault = 0; fault < 10; ++fault)
        {
            auto value = original;
            if (fault == 0) { value.insert("day", 5); }
            if (fault == 1) { value.insert("planSequence", 1); }
            if (fault == 2) { value.insert("allowance", "9223372036854775808"); }
            if (fault == 3) { value.insert("family", QJsonArray{QJsonArray{0}, QJsonArray{0}}); }
            if (fault == 4) { value.insert("family", QJsonArray{QJsonArray{1}, QJsonArray{0}}); }
            if (fault == 5) { value.insert("family", QJsonArray{QJsonArray{0, 1}}); }
            if (fault == 6) { value.insert("steps", QJsonArray{step(2, "enemy")}); }
            if (fault == 7) { value.insert("steps", QJsonArray{step(0, "Enemy")}); }
            if (fault == 8) { value.insert("runSearch", true); }
            if (fault == 9)
            {
                QJsonArray excessive;
                for (int count = 0; count < 129; ++count) { excessive.append(step(0, "enemy")); }
                value.insert("steps", excessive);
            }
            QString error;
            expect(!ArenaMasterStockReference::parse(QJsonDocument(value).toJson(QJsonDocument::Compact),
                       IDENTITY, problem.actors().size(), error) && !error.isEmpty(),
                   "malformed identity, family or explicit operation is rejected");
        }
    }

    void nativeFamilyAndCaptureCompletion()
    {
        using namespace MasterAssignmentOracle;
        const auto problem = captureProblem();
        const auto cases = captureCases(problem);
        const auto waitOur = exhaustSequential(cases[0].friendly, 1000);
        const auto waitEnemy = exhaustSequential(cases[0].enemy, 1000);
        const auto capOur = exhaustSequential(cases[1].friendly, 1000);
        const auto capEnemy = exhaustSequential(cases[1].enemy, 1000);
        const auto waitTotal = cases[0].owned + waitOur.value - waitEnemy.value;
        const auto captureTotal = cases[1].owned + capOur.value - capEnemy.value;
        expect(waitOur.status == ObjectiveStatus::Exact && waitEnemy.status == ObjectiveStatus::Exact &&
                   capOur.status == ObjectiveStatus::Exact && capEnemy.status == ObjectiveStatus::Exact &&
                   waitEnemy.value == capEnemy.value && waitTotal == 5 * INCOME &&
                   captureTotal - waitTotal == INCOME,
               "independent native-math reference prices finishing the capture at one turn of income");
        QTemporaryDir directory;
        require(directory.isValid(), "fixed-family temporary directory");
        const QJsonArray family{QJsonArray{0}, QJsonArray{2}, QJsonArray{1}, QJsonArray{3},
            QJsonArray{4}, QJsonArray{5}, QJsonArray{6}, QJsonArray{7}};
        const auto config = requestObject(problem, family, {step(0, "friendly"), step(2, "friendly"), step(1, "friendly")});
        auto request = parseRequest(problem, config);
        RefinementLedger ledger;
        require(ledger.open(request.allowance), "fixed-family native ledger");
        PropertyStockField field;
        PropertyStockInstance enemy;
        NativeStockSession session(field, enemy, cases, ledger);
        MasterAssignmentEvaluator evaluator(problem, ledger, session, {1, 1, 1});
        const auto output = directory.filePath("family.jsonl");
        expect(ArenaMasterStockReference::record(request, problem, evaluator, ledger, output) == StockReferenceResult::Complete,
               "fixed-family native execution completes without production search or installation");
        const auto stream = records(output);
        const auto end = stream.last().toObject();
        expect(end.value("result") == "STOCK_REFERENCE_COMPLETE" && end.value("familySize") == 8 &&
                   end.value("references") == 8 && end.value("applicablePairs") == 7 &&
                   end.value("completedSteps") == 0 && end.value("outcome") == "SignsDecided" &&
                   end.value("allDecided").toBool() && end.value("errors").toArray().isEmpty(),
               "exact friendly prices decide every sign, so no requested step runs");
        QJsonObject lastSnapshot;
        for (const auto & value : stream)
        {
            const auto row = value.toObject();
            if (row.value("event") == "snapshot")
            {
                lastSnapshot = row;
                expect(row.value("plans").toArray().size() == 8, "every snapshot retains the complete requested family");
            }
            if (row.value("event") == "legality")
            {
                const auto reference = row.value("reference").toObject();
                expect(reference.value("result").toObject().value("reducedDomain").toBool() &&
                           reference.value("continuation").toObject().value("mode").toInt() ==
                               static_cast<int>(ContinuationMode::Unavailable),
                       "singleton legality export never advertises a full-domain stock optimum");
            }
        }
        const auto comparison = lastSnapshot.value("plans").toArray()[2].toObject().value("comparison").toObject();
        expect(comparison.value("deltaLower").toString() == QString::number(INCOME) &&
                   comparison.value("deltaUpper").toString() == QString::number(INCOME) &&
                   comparison.value("commonEnemyCancelled").toBool(),
               "fresh changed-mask native pair agrees with the independent one-turn reference");
        const auto preserved = readFile(output);
        const auto before = ledger.stats().spent();
        expect(ArenaMasterStockReference::record(request, problem, evaluator, ledger, output) == StockReferenceResult::Failed &&
                   readFile(output) == preserved && ledger.stats().spent() == before,
               "existing reference output is never overwritten or repriced");

        const auto contestedCases = captureCases(problem, true);
        for (const bool funded : {true, false})
        {
            auto finiteConfig = requestObject(problem, {QJsonArray{0}, QJsonArray{1}}, {step(0, "friendly")});
            if (!funded) { finiteConfig.insert("allowance", "1"); }
            const auto finite = parseRequest(problem, finiteConfig);
            RefinementLedger finiteLedger;
            require(finiteLedger.open(finite.allowance), "finite reference ledger");
            NativeStockSession finiteSession(field, enemy, contestedCases, finiteLedger);
            MasterAssignmentEvaluator finiteEvaluator(problem, finiteLedger, finiteSession, {1, 1, 1});
            const auto path = directory.filePath(funded ? "unresolved.jsonl" : "funding.jsonl");
            expect(ArenaMasterStockReference::record(finite, problem, finiteEvaluator, finiteLedger, path) ==
                       StockReferenceResult::Complete, "finite unresolved and funding outcomes are not malformed requests");
            const auto result = records(path).last().toObject();
            expect(!result.value("allDecided").toBool() &&
                       result.value("outcome") == (funded ? "StepsExhausted" : "FundingRefused") &&
                       result.value("completedSteps") == (funded ? 1 : 0) &&
                       result.value("errors").toArray().isEmpty(),
                   "finite reference result preserves unresolved sign and explicit funding boundary");
        }
    }

    void nonExecutableFamilies()
    {
        for (const bool unsupported : {false, true})
        {
            Fixture fixture(2);
            fixture.input.actors[0].candidates.push_back(
                moveCandidate(0, {0, 0}, unsupported ? TilePoint{0, 1} : TilePoint{1, 0}, 5, 1));
            fixture.seat();
            if (unsupported) { fixture.plan.addEdge(0, 1, PlanEdgeKind::Screen); }
            const auto problem = fixture.extract();
            auto request = parseRequest(problem, requestObject(problem, {QJsonArray{0, 0}, QJsonArray{1, 0}}));
            RefinementLedger ledger;
            require(ledger.open(request.allowance), "non-executable family ledger");
            MasterAssignmentEvaluator evaluator(problem, ledger);
            QTemporaryDir directory;
            const auto output = directory.filePath("reference.jsonl");
            expect(ArenaMasterStockReference::record(request, problem, evaluator, ledger, output) == StockReferenceResult::Complete,
                   "infeasible and unsupported family members remain explicit non-executable reference outcomes");
            const auto stream = records(output);
            const auto disposition = stream[2].toObject().value("disposition").toInt();
            expect(disposition == static_cast<int>(unsupported ? MasterAssignmentOracle::TupleStatus::Unsupported
                                                              : MasterAssignmentOracle::TupleStatus::Infeasible) &&
                       stream.last().toObject().value("applicablePairs") == 0,
                   "non-executable family member does not become a feasible comparison");
            expect(stream.last().toObject().value("allDecided").toBool() == !unsupported &&
                       stream.last().toObject().value("outcome") == (unsupported ? "StepsExhausted" : "SignsDecided"),
                   "unsupported reconstruction remains unresolved while proven infeasibility can close a family member");
        }
    }
}

void testMasterStockReference()
{
    parserAndGate();
    nativeFamilyAndCaptureCompletion();
    nonExecutableFamilies();
}
