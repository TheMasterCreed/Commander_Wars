#include "tests/coordinator_ai/masterstockimport.h"

#include <array>
#include <limits>
#include <numeric>

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>

#include "ai/coordinator/masterassignmenttrace.h"
#include "tests/coordinator_ai/masterassignmentstockfixture.h"

namespace
{
    using namespace Coordinator;
    using namespace MasterAssignmentTest;
    using MasterAssignmentOracle::SequentialInput;

    constexpr qint64 MAX_TRACE_BYTES = 16 * 1024 * 1024;
    constexpr qint64 MAX_RECORD_BYTES = 2 * 1024 * 1024;
    constexpr qint64 MAX_REQUEST_BYTES = 65536;
    constexpr std::int32_t MAX_ROWS = 64;
    constexpr std::int32_t MAX_COLUMNS = 128;
    constexpr std::int32_t MAX_SIDE = 128;
    constexpr std::int32_t MAX_TILES = 4096;
    constexpr std::int32_t MAX_GRIDS = 8;
    constexpr std::int32_t MAX_HORIZON = 32;
    constexpr std::int32_t MAX_STEPS = 128;
    constexpr std::int32_t MAX_FAMILY = 8;
    constexpr std::int32_t SOLVE_COUNT = 4;
    constexpr std::int64_t MAX_STATE_CAP = INT64_C(4000000000);
    constexpr std::int64_t MAX_VALUE = INT64_C(1000000000000);
    constexpr std::size_t MAX_COLLECTED_OPTIONS = 1000000;
    constexpr std::size_t MAX_COLLECTED_INCIDENCES = 8000000;
    constexpr std::array<const char*, 6> CONFIG_KEYS{
        "trace", "expectedSHA256", "output", "familyIndices", "perSolveStateCap", "maximumSolves"};
    constexpr std::array<const char*, 2> TERM_NAMES{"friendly", "enemy"};
    constexpr const char* BEGIN = "begin";
    constexpr const char* LEGALITY = "legality";
    constexpr const char* SNAPSHOT = "snapshot";
    constexpr const char* END = "end";
    constexpr const char* SOLVE = "solve";
    constexpr const char* REFERENCE_COMPLETE = "STOCK_REFERENCE_COMPLETE";
    constexpr const char* OFFLINE_COMPLETE = "OFFLINE_STOCK_REFERENCE_COMPLETE";
    constexpr const char* OFFLINE_FAILED = "OFFLINE_STOCK_REFERENCE_FAILED";
    constexpr const char* POSITIVE = "CertifiedPositive";
    constexpr const char* NON_IMPROVEMENT = "CertifiedNonImprovement";
    constexpr const char* UNRESOLVED = "Unresolved";

    enum class Term : std::int8_t
    {
        Friendly,
        Enemy,
    };

    QString hash(const QByteArray & bytes)
    {
        return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    }

    QByteArray compact(const QJsonObject & object)
    {
        return QJsonDocument(object).toJson(QJsonDocument::Compact);
    }

    bool fail(QString & error, const QString & message)
    {
        error = message;
        return false;
    }

    bool integer(const QJsonValue & value, std::int32_t & result,
                 std::int32_t minimum, std::int32_t maximum)
    {
        result = value.toInt(std::numeric_limits<std::int32_t>::min());
        return value.isDouble() && value.toDouble() == result && result >= minimum && result <= maximum;
    }

    bool amount(const QJsonValue & value, std::int64_t & result,
                std::int64_t minimum = -MAX_VALUE, std::int64_t maximum = MAX_VALUE)
    {
        bool valid = false;
        const auto text = value.toString();
        result = text.toLongLong(&valid);
        return value.isString() && valid && text == QString::number(result) &&
            result >= minimum && result <= maximum;
    }

    bool boolean(const QJsonValue & value, bool expected)
    {
        return value.isBool() && value.toBool() == expected;
    }

    bool digest(const QString & text)
    {
        return text.size() == 64 && std::all_of(text.begin(), text.end(), [](QChar value) {
            return (value >= QLatin1Char('0') && value <= QLatin1Char('9')) ||
                (value >= QLatin1Char('a') && value <= QLatin1Char('f'));
        });
    }

    bool readBytes(const QString & path, qint64 maximum, QByteArray & bytes, QString & error)
    {
        QFile file(path);
        if (!QDir::isAbsolutePath(path) || !file.open(QIODevice::ReadOnly) ||
            file.size() <= 0 || file.size() > maximum)
        {
            return fail(error, QStringLiteral("input path, size or open failure: ") + path);
        }
        bytes = file.read(maximum + 1);
        return (bytes.size() == file.size() && bytes.size() <= maximum && file.error() == QFile::NoError) ||
            fail(error, QStringLiteral("input read failed: ") + path);
    }

    bool uniqueConfigKeys(const QByteArray & bytes)
    {
        QSet<QByteArray> names;
        for (qsizetype position = 0; position < bytes.size(); ++position)
        {
            if (bytes[position] != '"')
            {
                continue;
            }
            const auto start = position++;
            while (position < bytes.size() && bytes[position] != '"')
            {
                if (bytes[position] == '\\')
                {
                    ++position;
                }
                ++position;
            }
            if (position >= bytes.size())
            {
                return false;
            }
            auto next = position + 1;
            while (next < bytes.size() && QByteArray(" \r\n\t").contains(bytes[next]))
            {
                ++next;
            }
            if (next < bytes.size() && bytes[next] == ':')
            {
                const auto name = bytes.mid(start + 1, position - start - 1);
                if (names.contains(name) ||
                    std::none_of(CONFIG_KEYS.begin(), CONFIG_KEYS.end(),
                        [&](const char* key) { return name == key; }))
                {
                    return false;
                }
                names.insert(name);
            }
        }
        return names.size() == static_cast<qsizetype>(CONFIG_KEYS.size());
    }

    struct Config
    {
        QString trace;
        QString expectedSHA256;
        QString output;
        std::int64_t stateCap{0};
    };

    bool readConfig(const QByteArray & bytes, Config & result, QString & error)
    {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(bytes, &parseError);
        if (bytes.size() > MAX_REQUEST_BYTES || parseError.error != QJsonParseError::NoError ||
            !document.isObject() || !uniqueConfigKeys(bytes))
        {
            return fail(error, QStringLiteral("invalid or duplicate offline configuration"));
        }
        const auto object = document.object();
        result.trace = object.value(CONFIG_KEYS[0]).toString();
        result.expectedSHA256 = object.value(CONFIG_KEYS[1]).toString().toLower();
        result.output = object.value(CONFIG_KEYS[2]).toString();
        std::int32_t solves = 0;
        if (object.size() != static_cast<qsizetype>(CONFIG_KEYS.size()) ||
            !QDir::isAbsolutePath(result.trace) || !QDir::isAbsolutePath(result.output) ||
            result.trace.size() > 4096 || result.output.size() > 4096 ||
            !digest(result.expectedSHA256) ||
            object.value(CONFIG_KEYS[3]) != QJsonArray{0, 1} ||
            !amount(object.value(CONFIG_KEYS[4]), result.stateCap, 1, MAX_STATE_CAP) ||
            !integer(object.value(CONFIG_KEYS[5]), solves, SOLVE_COUNT, SOLVE_COUNT) ||
            QFileInfo(result.output).exists() || !QFileInfo(result.output).dir().exists())
        {
            return fail(error, QStringLiteral("offline configuration exceeds the fixed two-plan/four-solve scope"));
        }
        return true;
    }

    bool indices(const QJsonValue & value, std::vector<std::int32_t> & result,
                 std::size_t maximumSize, std::int32_t minimum, std::int32_t maximum)
    {
        if (!value.isArray() || static_cast<std::size_t>(value.toArray().size()) > maximumSize)
        {
            return false;
        }
        result.clear();
        for (const auto entry : value.toArray())
        {
            std::int32_t number = 0;
            if (!integer(entry, number, minimum, maximum))
            {
                return false;
            }
            result.push_back(number);
        }
        return true;
    }

    bool amounts(const QJsonValue & value, std::vector<std::int64_t> & result, std::size_t expected)
    {
        if (!value.isArray() || static_cast<std::size_t>(value.toArray().size()) != expected)
        {
            return false;
        }
        result.clear();
        for (const auto entry : value.toArray())
        {
            std::int64_t number = 0;
            if (!amount(entry, number))
            {
                return false;
            }
            result.push_back(number);
        }
        return true;
    }

    bool point(const QJsonValue & value, TilePoint & result, std::int32_t width, std::int32_t height)
    {
        const auto pair = value.toArray();
        return value.isArray() && pair.size() == 2 &&
            integer(pair[0], result.x, 0, width - 1) && integer(pair[1], result.y, 0, height - 1);
    }

    bool readColumns(const QJsonValue & value, std::vector<PropertyStockColumn> & result,
                     std::int32_t width, std::int32_t height)
    {
        if (!value.isArray() || value.toArray().size() > MAX_COLUMNS)
        {
            return false;
        }
        for (const auto entry : value.toArray())
        {
            const auto object = entry.toObject();
            const auto income = object.value("income").toObject();
            PropertyStockColumn column;
            std::int32_t owner = 0;
            if (!entry.isObject() ||
                !integer(object.value("slot"), column.slot, 0, MAX_COLUMNS - 1) ||
                (!result.empty() && result.back().slot >= column.slot) ||
                !point(object.value("tile"), column.tile, width, height) ||
                !amount(income.value("oursPerTurn"), column.income.oursPerTurn, 0) ||
                !amount(income.value("enemyPerTurn"), column.income.enemyPerTurn, 0) ||
                !integer(object.value("ownerBefore"), owner, -1, 1) ||
                !integer(object.value("buildingIndex"), column.buildingIndex, 0, MAX_TILES - 1))
            {
                return false;
            }
            column.ownerBefore = static_cast<OwnerSign>(owner);
            result.push_back(column);
        }
        return true;
    }

    bool readRows(const QJsonValue & value, std::vector<PropertyStockRow> & result,
                  const PackingDualFieldIdentity & field)
    {
        if (!value.isArray() || value.toArray().size() > MAX_ROWS)
        {
            return false;
        }
        for (const auto entry : value.toArray())
        {
            const auto object = entry.toObject();
            PropertyStockRow row;
            if (!entry.isObject() ||
                !integer(object.value("knowledgeIndex"), row.knowledgeIndex, 0, MAX_TILES - 1) ||
                (!result.empty() && result.back().knowledgeIndex >= row.knowledgeIndex) ||
                !integer(object.value("gridIdentity"), row.gridIdentity, 0,
                         static_cast<std::int32_t>(field.mobilityGrids.size()) - 1) ||
                !integer(object.value("movementPoints"), row.movementPoints, 0, MAX_TILES) ||
                !integer(object.value("captureRate"), row.captureRate, 1, MAX_TILES) ||
                !integer(object.value("capturePoints"), row.capturePoints, 0, field.capturePointsToCapture) ||
                !point(object.value("tile"), row.tile, field.width, field.height))
            {
                return false;
            }
            result.push_back(row);
        }
        return true;
    }

    bool readGrids(const QJsonValue & value, PackingDualFieldIdentity & field)
    {
        if (!value.isArray() || value.toArray().isEmpty() || value.toArray().size() > MAX_GRIDS)
        {
            return false;
        }
        QJsonArray directions;
        for (const auto direction : ENTRY_DIRECTIONS)
        {
            directions.append(static_cast<int>(direction));
        }
        for (const auto entry : value.toArray())
        {
            const auto object = entry.toObject();
            const auto rows = object.value("costs").toArray();
            if (!entry.isObject() || object.value("width") != field.width ||
                object.value("height") != field.height || object.value("entryDirections") != directions ||
                rows.size() != field.height)
            {
                return false;
            }
            MobilityCostGrid grid(field.width, field.height);
            for (std::int32_t y = 0; y < field.height; ++y)
            {
                const auto cells = rows[y].toArray();
                if (cells.size() != field.width)
                {
                    return false;
                }
                for (std::int32_t x = 0; x < field.width; ++x)
                {
                    const auto costs = cells[x].toArray();
                    if (costs.size() != ENTRY_DIRECTION_COUNT)
                    {
                        return false;
                    }
                    for (std::size_t direction = 0; direction < ENTRY_DIRECTIONS.size(); ++direction)
                    {
                        std::int32_t cost = 0;
                        if (!integer(costs[static_cast<qsizetype>(direction)], cost, IMPASSABLE_COST, MAX_TILES))
                        {
                            return false;
                        }
                        grid.setCost(x, y, ENTRY_DIRECTIONS[direction], cost);
                    }
                }
            }
            field.mobilityGrids.push_back(std::move(grid));
        }
        return true;
    }

    bool readField(const QJsonObject & object, PackingDualFieldIdentity & field, QString & error)
    {
        if (!integer(object.value("width"), field.width, 1, MAX_SIDE) ||
            !integer(object.value("height"), field.height, 1, MAX_SIDE) ||
            field.width * field.height > MAX_TILES ||
            !integer(object.value("horizonTurns"), field.horizonTurns, 1, MAX_HORIZON) ||
            !integer(object.value("capturePointsToCapture"), field.capturePointsToCapture, 1, MAX_TILES) ||
            !amount(object.value("ownedBaseline"), field.ownedBaseline) ||
            !readGrids(object.value("mobilityGrids"), field) ||
            !readColumns(object.value("ourColumns"), field.ourColumns, field.width, field.height) ||
            !readColumns(object.value("enemyColumns"), field.enemyColumns, field.width, field.height) ||
            field.ourColumns.size() != field.enemyColumns.size() ||
            !readRows(object.value("ourRows"), field.ourRows, field) ||
            !readRows(object.value("enemyRows"), field.enemyRows, field))
        {
            return fail(error, QStringLiteral("invalid or oversized immutable stock field"));
        }
        const auto columns = field.ourColumns.size();
        const auto tiles = object.value("columnTiles").toArray();
        if (static_cast<std::size_t>(tiles.size()) != columns ||
            !indices(object.value("columnCapturers"), field.columnCapturers, columns, -1, MAX_TILES - 1) ||
            !indices(object.value("columnCapturePoints"), field.columnCapturePoints, columns, 0, field.capturePointsToCapture) ||
            field.columnCapturers.size() != columns || field.columnCapturePoints.size() != columns ||
            !amounts(object.value("ourWeights"), field.ourWeights, columns * field.ourRows.size()) ||
            !amounts(object.value("enemyWeights"), field.enemyWeights, columns * field.enemyRows.size()) ||
            !indices(object.value("enemyArrivals"), field.enemyArrivals,
                     columns * field.enemyRows.size(), UNREACHABLE, field.horizonTurns) ||
            field.enemyArrivals.size() != columns * field.enemyRows.size())
        {
            return fail(error, QStringLiteral("inconsistent immutable stock array shapes"));
        }
        for (std::size_t column = 0; column < columns; ++column)
        {
            TilePoint tile;
            if (field.ourColumns[column].slot != static_cast<std::int32_t>(column) ||
                field.enemyColumns[column] != mirroredColumn(field.ourColumns[column]) ||
                !point(tiles[static_cast<qsizetype>(column)], tile, field.width, field.height) ||
                tile != field.ourColumns[column].tile ||
                std::find(field.columnTiles.begin(), field.columnTiles.end(), tile) != field.columnTiles.end())
            {
                return fail(error, QStringLiteral("column identities, mirrors or tiles disagree"));
            }
            field.columnTiles.push_back(tile);
        }
        return true;
    }

    std::vector<std::int32_t> arrivals(const PackingDualFieldIdentity & field,
                                       const PropertyStockRow & row, TilePoint origin)
    {
        ActivationField reach;
        reach.build(field.mobilityGrids[static_cast<std::size_t>(row.gridIdentity)], origin,
                    row.movementPoints, field.horizonTurns, field.columnTiles);
        std::vector<std::int32_t> result;
        result.reserve(field.columnTiles.size());
        for (const auto tile : field.columnTiles)
        {
            result.push_back(reach.activations(tile.x, tile.y));
        }
        return result;
    }

    std::int32_t carried(const PackingDualFieldIdentity & field, const PropertyStockRow & row,
                         std::size_t column)
    {
        if (field.columnCapturers[column] == row.knowledgeIndex && field.columnTiles[column] == row.tile)
        {
            return field.columnCapturePoints[column];
        }
        return 0;
    }

    bool verifyFieldRows(const PackingDualFieldIdentity & field, Term term, QString & error)
    {
        const auto & rows = term == Term::Friendly ? field.ourRows : field.enemyRows;
        const auto & columns = term == Term::Friendly ? field.ourColumns : field.enemyColumns;
        const auto & weights = term == Term::Friendly ? field.ourWeights : field.enemyWeights;
        for (std::size_t row = 0; row < rows.size(); ++row)
        {
            const auto travel = arrivals(field, rows[row], rows[row].tile);
            for (std::size_t column = 0; column < columns.size(); ++column)
            {
                const auto offset = row * columns.size() + column;
                if ((term == Term::Enemy && travel[column] != field.enemyArrivals[offset]) ||
                    columnWeight(columns[column], {travel[column], carried(field, rows[row], column),
                        rows[row].captureRate}, field.capturePointsToCapture, field.horizonTurns) != weights[offset])
                {
                    return fail(error, QStringLiteral("native field arrivals or first-leg weights do not reconstruct"));
                }
            }
        }
        return true;
    }

    bool readKey(const QJsonValue & value, const PackingDualFieldIdentity & field, ContinuationKey & key)
    {
        if (!value.isArray() || static_cast<std::size_t>(value.toArray().size()) > field.ourRows.size())
        {
            return false;
        }
        for (const auto entry : value.toArray())
        {
            const auto parts = entry.toArray();
            std::int32_t row = -1;
            TilePoint tile;
            if (parts.size() != 4 ||
                !integer(parts[0], row, 0, static_cast<std::int32_t>(field.ourRows.size()) - 1) ||
                (!key.empty() && key.back().row() >= row) ||
                !point(QJsonArray{parts[1], parts[2]}, tile, field.width, field.height) || !parts[3].isBool())
            {
                return false;
            }
            key.push_back(CanonicalPlanActionKey::fromAction(row, tile.x, tile.y, parts[3].toBool()));
        }
        return true;
    }

    std::int32_t movedCarry(const PackingDualFieldIdentity & field, const PropertyStockRow & row,
                            const CanonicalPlanActionKey & action)
    {
        const TilePoint destination{action.x(), action.y()};
        const auto found = std::find(field.columnTiles.begin(), field.columnTiles.end(), destination);
        if (found == field.columnTiles.end())
        {
            return 0;
        }
        const auto column = static_cast<std::size_t>(found - field.columnTiles.begin());
        return carried(field, row, column) + (action.captures() ? row.captureRate : 0);
    }

    std::vector<std::int32_t> captured(const PackingDualFieldIdentity & field, const ContinuationKey & key)
    {
        std::vector<std::int32_t> result;
        for (const auto & action : key)
        {
            const auto found = std::find(field.columnTiles.begin(), field.columnTiles.end(),
                                        TilePoint{action.x(), action.y()});
            if (action.captures() && found != field.columnTiles.end() &&
                movedCarry(field, field.ourRows[static_cast<std::size_t>(action.row())], action) >=
                    field.capturePointsToCapture)
            {
                result.push_back(static_cast<std::int32_t>(found - field.columnTiles.begin()));
            }
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    SequentialInput reconstruct(const PackingDualFieldIdentity & field, const ContinuationKey & key,
                                const std::vector<std::int32_t> & mask, Term term)
    {
        SequentialInput result;
        result.horizon = field.horizonTurns;
        // Start-of-turn ownership prices the enemy, so the captured mask only marks friendly nodes.
        const auto & columns = term == Term::Friendly ? field.ourColumns : field.enemyColumns;
        const auto & rows = term == Term::Friendly ? field.ourRows : field.enemyRows;
        std::vector<std::int32_t> nodeColumns;
        for (std::size_t column = 0; column < columns.size(); ++column)
        {
            if (columns[column].ownerBefore != STOCK_OWNER_AFTER)
            {
                nodeColumns.push_back(static_cast<std::int32_t>(column));
                result.columns.push_back(columns[column]);
                result.capturedNodes.push_back(term == Term::Friendly &&
                    std::binary_search(mask.begin(), mask.end(), static_cast<std::int32_t>(column)));
            }
        }
        std::vector<SequentialClassKey> classKeys;
        for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
        {
            const auto & source = rows[rowIndex];
            const SequentialClassKey classKey{source.gridIdentity, source.movementPoints, source.captureRate};
            auto classIndex = std::find(classKeys.begin(), classKeys.end(), classKey) - classKeys.begin();
            if (classIndex == static_cast<std::ptrdiff_t>(classKeys.size()))
            {
                MasterAssignmentOracle::SequentialClassInput definition;
                definition.captureTurns = captureTurnsFor(0, source.captureRate, field.capturePointsToCapture);
                for (const auto & column : result.columns)
                {
                    const auto travel = arrivals(field, source, column.tile);
                    for (const auto target : nodeColumns)
                    {
                        definition.interArrivals.push_back(travel[static_cast<std::size_t>(target)]);
                    }
                }
                result.classes.push_back(std::move(definition));
                classKeys.push_back(classKey);
            }
            const CanonicalPlanActionKey* mover = nullptr;
            if (term == Term::Friendly)
            {
                const auto found = std::find_if(key.begin(), key.end(),
                    [&](const auto & action) { return action.row() == static_cast<std::int32_t>(rowIndex); });
                if (found != key.end())
                {
                    mover = &*found;
                }
            }
            const auto origin = mover != nullptr ? TilePoint{mover->x(), mover->y()} : source.tile;
            const auto travel = arrivals(field, source, origin);
            SequentialRow row;
            row.classIndex = static_cast<std::int32_t>(classIndex);
            for (const auto columnIndex : nodeColumns)
            {
                const auto column = static_cast<std::size_t>(columnIndex);
                auto progress = carried(field, source, column);
                if (mover != nullptr)
                {
                    progress = field.columnTiles[column] == origin ? movedCarry(field, source, *mover) : 0;
                }
                const ArrivalFacts arrival{travel[column], progress, source.captureRate};
                row.weight.push_back(columnWeight(columns[column], arrival,
                                                   field.capturePointsToCapture, field.horizonTurns));
                row.ownedTurn.push_back(sequentialFirstOwnedTurn(ownedTurnsUntil(
                    travel[column], progress, source.captureRate, field.capturePointsToCapture,
                    field.horizonTurns), field.horizonTurns));
            }
            result.rows.push_back(std::move(row));
        }
        return result;
    }

    struct ImportedPlan
    {
        QJsonObject evaluation;
        QJsonObject legality;
        ContinuationKey key;
        std::vector<std::int32_t> mask;
        MilliFunds seated{0};
        MilliFunds owned{0};
    };

    struct Imported
    {
        QJsonObject header;
        QJsonObject terminal;
        QJsonObject fieldRecord;
        PackingDualFieldIdentity field;
        std::array<ImportedPlan, 2> plans;
        std::int32_t records{0};
        std::int32_t snapshots{0};
    };

    bool frameInteger(const QJsonObject & object, const char* key, std::int32_t expected)
    {
        std::int32_t actual = 0;
        return integer(object.value(key), actual, expected, expected);
    }

    bool readHeader(const QJsonObject & header, QString & error)
    {
        const auto model = header.value("model").toObject();
        const auto family = header.value("family").toArray();
        const auto actors = model.value("actors").toArray();
        std::int32_t steps = 0;
        if (!frameInteger(header, "schemaVersion", 1) || actors.isEmpty() || actors.size() > MAX_ROWS ||
            !frameInteger(model, "actorCount", static_cast<std::int32_t>(actors.size())) ||
            header.value("modelSHA256").toString().toLower() != hash(compact(model)) ||
            family.size() < 2 || family.size() > MAX_FAMILY ||
            header.value("incumbent") != model.value("incumbentChoices") ||
            family.first() != header.value("incumbent") ||
            !boolean(header.value("diagnosticOnly"), true) ||
            !boolean(header.value("productionSearchRun"), false) ||
            !boolean(header.value("installed"), false) ||
            !integer(header.value("plannedSteps"), steps, 0, MAX_STEPS))
        {
            return fail(error, QStringLiteral("reference header model, family or diagnostic identity mismatch"));
        }
        QJsonArray previous;
        for (const auto tuple : family)
        {
            std::vector<std::int32_t> choices;
            if (!indices(tuple, choices, MAX_ROWS, NO_CANDIDATE, MAX_TILES) ||
                choices.size() != static_cast<std::size_t>(actors.size()) || previous.contains(tuple))
            {
                return fail(error, QStringLiteral("invalid or duplicated reference tuple"));
            }
            previous.append(tuple);
        }
        return true;
    }

    bool readFrames(const QByteArray & bytes, Imported & result, QString & error)
    {
        if (bytes.isEmpty() || bytes.size() > MAX_TRACE_BYTES || !bytes.endsWith('\n'))
        {
            return fail(error, QStringLiteral("reference stream is oversized or lacks its terminal delimiter"));
        }
        const auto lines = bytes.split('\n');
        if (lines.size() > MAX_STEPS + MAX_FAMILY + 4)
        {
            return fail(error, QStringLiteral("reference framing exceeds its bounded schedule"));
        }
        QJsonObject finalSnapshot;
        std::int32_t legalities = 0;
        std::int32_t familySize = 0;
        for (qsizetype line = 0; line + 1 < lines.size(); ++line)
        {
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(lines[line], &parseError);
            if (lines[line].isEmpty() || lines[line].size() > MAX_RECORD_BYTES ||
                parseError.error != QJsonParseError::NoError || !document.isObject() ||
                document.toJson(QJsonDocument::Compact) != lines[line])
            {
                return fail(error, QStringLiteral("reference frame is malformed, duplicated or noncanonical"));
            }
            const auto record = document.object();
            std::int64_t sequence = -1;
            if (!amount(record.value("sequence"), sequence, 0) || sequence != line)
            {
                return fail(error, QStringLiteral("reference frame sequence is not contiguous"));
            }
            const auto event = record.value("event").toString();
            if (line == 0)
            {
                if (event != QLatin1String(BEGIN) || !readHeader(record, error))
                {
                    return fail(error, QStringLiteral("invalid reference begin frame: ") + error);
                }
                result.header = record;
                familySize = static_cast<std::int32_t>(record.value("family").toArray().size());
            }
            else if (line <= familySize)
            {
                if (event != QLatin1String(LEGALITY) || !frameInteger(record, "plan", legalities))
                {
                    return fail(error, QStringLiteral("missing or reordered reference legality frame"));
                }
                if (legalities < static_cast<std::int32_t>(result.plans.size()))
                {
                    result.plans[static_cast<std::size_t>(legalities)].legality = record;
                }
                ++legalities;
            }
            else if (line + 2 == lines.size())
            {
                if (event != QLatin1String(END))
                {
                    return fail(error, QStringLiteral("missing reference terminal frame"));
                }
                result.terminal = record;
            }
            else
            {
                if (event != QLatin1String(SNAPSHOT) ||
                    !frameInteger(record, "step", result.snapshots - 1) ||
                    record.value("plans").toArray().size() != familySize)
                {
                    return fail(error, QStringLiteral("missing or reordered reference snapshot"));
                }
                const auto plans = record.value("plans").toArray();
                for (std::int32_t plan = 0; plan < familySize; ++plan)
                {
                    if (!frameInteger(plans[plan].toObject(), "plan", plan) ||
                        plans[plan].toObject().value("choices") != result.header.value("family").toArray()[plan])
                    {
                        return fail(error, QStringLiteral("reference snapshot family identity changed"));
                    }
                }
                finalSnapshot = record;
                ++result.snapshots;
            }
            ++result.records;
        }
        const auto end = result.terminal;
        const auto outcome = end.value("outcome").toString();
        if (result.snapshots == 0 || end.value("result") != QLatin1String(REFERENCE_COMPLETE) ||
            (outcome != QLatin1String("StepsExhausted") && outcome != QLatin1String("SignsDecided")) ||
            !end.value("errors").isArray() || !end.value("errors").toArray().isEmpty() ||
            !frameInteger(end, "familySize", familySize) || !frameInteger(end, "references", familySize) ||
            !frameInteger(end, "completedSteps", result.snapshots - 1) ||
            !frameInteger(end, "snapshots", result.snapshots) ||
            result.snapshots - 1 > result.header.value("plannedSteps").toInt() ||
            (outcome == QLatin1String("StepsExhausted") &&
             result.snapshots - 1 != result.header.value("plannedSteps").toInt()))
        {
            return fail(error, QStringLiteral("reference terminal status or coverage is incomplete"));
        }
        for (std::size_t index = 0; index < result.plans.size(); ++index)
        {
            const auto snapshot = finalSnapshot.value("plans").toArray()[static_cast<qsizetype>(index)].toObject();
            if (!frameInteger(snapshot, "disposition", static_cast<int>(MasterAssignmentOracle::TupleStatus::Feasible)) ||
                !boolean(snapshot.value("evaluated"), true) || !boolean(snapshot.value("evaluationCurrent"), true))
            {
                return fail(error, QStringLiteral("selected final reference evaluation is not current and feasible"));
            }
            result.plans[index].evaluation = snapshot.value("evaluation").toObject();
        }
        return true;
    }

    bool verifyModelKey(const QJsonObject & model, const QJsonArray & choices,
                        const ContinuationKey & key, QString & error)
    {
        ContinuationKey expected;
        const auto actors = model.value("actors").toArray();
        if (actors.size() != choices.size())
        {
            return false;
        }
        for (qsizetype index = 0; index < actors.size(); ++index)
        {
            const auto actor = actors[index].toObject();
            std::int32_t row = -1;
            if (!frameInteger(actor, "slot", static_cast<std::int32_t>(index)) ||
                !integer(actor.value("stockRow"), row, -1, MAX_ROWS - 1) ||
                !actor.value("mutable").isBool() ||
                (!actor.value("mutable").toBool() && actor.value("incumbent") != choices[index]))
            {
                return fail(error, QStringLiteral("model actor or fixed choice is invalid"));
            }
            QJsonObject selected;
            const auto candidates = actor.value("candidates").toArray();
            if (candidates.size() > MAX_TILES)
            {
                return false;
            }
            for (const auto entry : candidates)
            {
                const auto candidate = entry.toObject();
                if (candidate.value("index") == choices[index])
                {
                    if (!selected.isEmpty())
                    {
                        return fail(error, QStringLiteral("duplicated model candidate"));
                    }
                    selected = candidate;
                }
            }
            if (selected.isEmpty() || !boolean(selected.value("plannable"), true))
            {
                return fail(error, QStringLiteral("selected model candidate is unavailable"));
            }
            if (row >= 0)
            {
                const auto endpoint = selected.value("stockEndpoint").toArray();
                std::int32_t x = 0;
                std::int32_t y = 0;
                if (endpoint.size() != 2 || !integer(endpoint[0], x, 0, MAX_SIDE - 1) ||
                    !integer(endpoint[1], y, 0, MAX_SIDE - 1) || !selected.value("capture").isBool())
                {
                    return false;
                }
                expected.push_back(CanonicalPlanActionKey::fromAction(row, x, y, selected.value("capture").toBool()));
            }
        }
        std::sort(expected.begin(), expected.end());
        return expected == key || fail(error, QStringLiteral("model choices do not reproduce the saved stock key"));
    }

    bool verifyPlan(const Imported & imported, std::size_t index, ImportedPlan & plan, QString & error)
    {
        const auto quote = plan.evaluation.value("quote").toObject();
        const auto choices = imported.header.value("family").toArray()[static_cast<qsizetype>(index)].toArray();
        const auto reference = plan.legality.value("reference").toObject();
        const auto result = reference.value("result").toObject();
        const auto tuples = result.value("tuples").toArray();
        if (!frameInteger(plan.evaluation, "status", static_cast<int>(MasterEvaluationStatus::Feasible)) ||
            !frameInteger(plan.evaluation, "objectiveStatus", static_cast<int>(MasterObjectiveStatus::Verified)) ||
            !boolean(plan.evaluation.value("lowerReplayed"), true) ||
            !boolean(plan.evaluation.value("upperVerified"), true) ||
            plan.evaluation.value("choices") != choices ||
            !readKey(quote.value("key"), imported.field, plan.key) ||
            !indices(quote.value("capturedColumns"), plan.mask, MAX_COLUMNS, 0,
                     static_cast<std::int32_t>(imported.field.ourColumns.size()) - 1) ||
            plan.mask != captured(imported.field, plan.key) ||
            std::adjacent_find(plan.mask.begin(), plan.mask.end()) != plan.mask.end() ||
            !amount(plan.evaluation.value("seatedEconomic"), plan.seated) ||
            !amount(quote.value("owned"), plan.owned) || quote.value("economic") != QLatin1String("0") ||
            tuples.size() != 1 || result.value("falsePositives") != QLatin1String("0") ||
            result.value("falseNegatives") != QLatin1String("0") ||
            result.value("reconstructionMismatches") != QLatin1String("0") ||
            reference.value("problemSHA256").toString() != hash(compact(reference.value("problem").toObject())))
        {
            return fail(error, QStringLiteral("selected plan, capture consequence or archived reconstruction mismatch"));
        }
        const auto tuple = tuples[0].toObject();
        const auto recordedPlan = tuple.value("reference").toObject();
        if (tuple.value("choices") != choices || tuple.value("key") != quote.value("key") ||
            tuple.value("seatedTotal") != plan.evaluation.value("seatedEconomic") ||
            !frameInteger(tuple, "status", static_cast<int>(MasterAssignmentOracle::TupleStatus::Feasible)) ||
            !boolean(tuple.value("representationValid"), true) || !boolean(tuple.value("complete"), true) ||
            !boolean(tuple.value("productionValid"), true) || !boolean(tuple.value("productionComplete"), true) ||
            !boolean(tuple.value("comparisonApplicable"), true) || !boolean(tuple.value("comparisonEqual"), true) ||
            recordedPlan.value("order") != plan.evaluation.value("executionOrder"))
        {
            return fail(error, QStringLiteral("saved legality and final evaluation do not identify the same complete plan"));
        }
        MilliFunds owned = imported.field.ownedBaseline;
        for (const auto column : plan.mask)
        {
            if (!RelativeStockDetail::checkedAdd(owned, ownershipFlipSwing(
                    imported.field.ourColumns[static_cast<std::size_t>(column)], imported.field.horizonTurns)))
            {
                return fail(error, QStringLiteral("owned baseline arithmetic overflow"));
            }
        }
        return (owned == plan.owned && verifyModelKey(imported.header.value("model").toObject(),
            choices, plan.key, error)) || fail(error, QStringLiteral("owned value or model stock key mismatch"));
    }

    bool readImported(const QByteArray & bytes, Imported & result, QString & error)
    {
        if (!readFrames(bytes, result, error))
        {
            return false;
        }
        result.fieldRecord = result.plans[0].evaluation.value("quote").toObject()
            .value("ownedUpperCertificatePayload").toObject().value("identity").toObject()
            .value("immutableField").toObject();
        if (!readField(result.fieldRecord, result.field, error) ||
            !frameInteger(result.header, "horizon", result.field.horizonTurns) ||
            !verifyFieldRows(result.field, Term::Friendly, error) ||
            !verifyFieldRows(result.field, Term::Enemy, error))
        {
            return false;
        }
        for (std::size_t index = 0; index < result.plans.size(); ++index)
        {
            const auto field = result.plans[index].evaluation.value("quote").toObject()
                .value("ownedUpperCertificatePayload").toObject().value("identity").toObject()
                .value("immutableField").toObject();
            if (field != result.fieldRecord || !verifyPlan(result, index, result.plans[index], error))
            {
                return fail(error, QStringLiteral("selected plan field identities or reconstruction differ: ") + error);
            }
        }
        return true;
    }

    MilliFunds assignmentFloor(const SequentialInput & input)
    {
        std::vector<MilliFunds> weights;
        for (const auto & row : input.rows)
        {
            for (std::size_t node = 0; node < input.columns.size(); ++node)
            {
                weights.push_back(input.capturedNodes[node] ? 0 : row.weight[node]);
            }
        }
        AssignmentSolver solver;
        solver.solve(static_cast<std::int32_t>(input.rows.size()),
                     static_cast<std::int32_t>(input.columns.size()), weights);
        return solver.optimum();
    }

    bool readWitness(const QJsonValue & value, const SequentialInstance & instance, MilliFunds expected,
                     std::vector<SequentialRowWitness> & result, QString & error)
    {
        if (!value.isArray() || static_cast<std::size_t>(value.toArray().size()) > instance.rows.size())
        {
            return fail(error, QStringLiteral("oversized or missing sequential witness"));
        }
        std::size_t visits = 0;
        for (const auto entry : value.toArray())
        {
            const auto object = entry.toObject();
            const auto steps = object.value("visits").toArray();
            SequentialRowWitness witness;
            if (!integer(object.value("row"), witness.row, 0, static_cast<std::int32_t>(instance.rows.size()) - 1) ||
                !amount(object.value("value"), witness.value) || steps.isEmpty() ||
                steps.size() > instance.nodeCount())
            {
                return fail(error, QStringLiteral("invalid sequential witness row"));
            }
            const auto & row = instance.rows[static_cast<std::size_t>(witness.row)];
            for (const auto visit : steps)
            {
                const auto pair = visit.toArray();
                SequentialWitnessStep step;
                if (++visits > static_cast<std::size_t>(instance.nodeCount()) || pair.size() != 2 ||
                    !integer(pair[0], step.node, 0, instance.nodeCount() - 1) ||
                    !integer(pair[1], step.turn, NO_CAPTURE_TURNS, instance.horizonTurns - 1))
                {
                    return fail(error, QStringLiteral("invalid or excessive sequential witness visit"));
                }
                if (witness.steps.empty())
                {
                    step.gain = row.weight[static_cast<std::size_t>(step.node)];
                }
                else
                {
                    if (witness.steps.back().turn == NO_CAPTURE_TURNS)
                    {
                        return fail(error, QStringLiteral("a sentinel first leg cannot have a continuation"));
                    }
                    step.gain = SequentialDetail::chainPrize(instance, step.node, step.turn);
                }
                witness.steps.push_back(step);
            }
            result.push_back(std::move(witness));
        }
        return replaySequentialWitness(instance, result, expected) ||
            fail(error, QStringLiteral("saved witness fails native gains, disjointness or transition replay"));
    }

    QJsonArray money(const std::vector<std::int64_t> & values)
    {
        QJsonArray result;
        for (const auto value : values)
        {
            result.append(QString::number(value));
        }
        return result;
    }

    QJsonArray columnsRecord(const std::vector<PropertyStockColumn> & columns)
    {
        QJsonArray result;
        for (const auto & column : columns)
        {
            result.append(QJsonObject{{"slot", column.slot}, {"tile", QJsonArray{column.tile.x, column.tile.y}},
                {"income", QJsonObject{{"oursPerTurn", QString::number(column.income.oursPerTurn)},
                    {"enemyPerTurn", QString::number(column.income.enemyPerTurn)}}},
                {"ownerBefore", static_cast<int>(column.ownerBefore)}, {"buildingIndex", column.buildingIndex}});
        }
        return result;
    }

    QJsonObject inputRecord(const SequentialInput & input)
    {
        QJsonArray classes;
        for (const auto & value : input.classes)
        {
            classes.append(QJsonObject{{"captureTurns", value.captureTurns},
                {"interArrivals", masterChoicesRecord(value.interArrivals)}});
        }
        QJsonArray rows;
        for (const auto & row : input.rows)
        {
            rows.append(QJsonObject{{"class", row.classIndex}, {"weights", money(row.weight)},
                {"ownedTurns", masterChoicesRecord(row.ownedTurn)}});
        }
        QJsonArray mask;
        for (const auto node : input.capturedNodes)
        {
            mask.append(node);
        }
        return {{"horizon", input.horizon}, {"columns", columnsRecord(input.columns)}, {"classes", classes},
            {"rows", rows}, {"capturedNodes", mask}};
    }

    QJsonArray witnessRecord(const std::vector<SequentialRowWitness> & witness)
    {
        QJsonArray result;
        for (const auto & row : witness)
        {
            QJsonArray steps;
            for (const auto & step : row.steps)
            {
                steps.append(QJsonArray{step.node, step.turn, QString::number(step.gain)});
            }
            result.append(QJsonObject{{"row", row.row}, {"value", QString::number(row.value)}, {"steps", steps}});
        }
        return result;
    }

    struct CollectorLimits
    {
        std::size_t options{MAX_COLLECTED_OPTIONS};
        std::size_t incidences{MAX_COLLECTED_INCIDENCES};
    };

    std::optional<std::int64_t> verificationReservation(
        std::int32_t nodes, CollectorLimits limits, std::int64_t quota)
    {
        constexpr auto maximumHalf = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 2);
        if (nodes < 0 || quota <= 0 || limits.options > maximumHalf || limits.incidences > maximumHalf)
        {
            return std::nullopt;
        }
        std::int64_t required = nodes;
        if (!RelativeStockDetail::checkedAdd(required, 2 * static_cast<std::int64_t>(limits.incidences)) ||
            !RelativeStockDetail::checkedAdd(required, 2 * static_cast<std::int64_t>(limits.options)))
        {
            return std::nullopt;
        }
        if (required >= quota)
        {
            return 0;
        }
        return required;
    }

    class RowCollector final : public SequentialDetail::SequentialRowOptionSource
    {
    public:
        explicit RowCollector(std::size_t rows, CollectorLimits limits = {})
            : options(rows), complete(rows, false), m_limits(limits)
        {
        }

        bool lookup(std::int32_t, std::span<const SequentialDetail::RowOption> &, std::int64_t &) override
        {
            return false;
        }

        void store(std::int32_t row, std::span<const SequentialDetail::RowOption> source, std::int64_t) override
        {
            if (source.size() > m_limits.options - records)
            {
                return;
            }
            std::size_t incidences = 0;
            for (const auto & option : source)
            {
                if (option.nodes.size() > m_limits.incidences - nodes - incidences)
                {
                    return;
                }
                incidences += option.nodes.size();
            }
            records += source.size();
            nodes += incidences;
            options[static_cast<std::size_t>(row)].assign(source.begin(), source.end());
            complete[static_cast<std::size_t>(row)] = true;
        }

        std::vector<std::vector<SequentialDetail::RowOption>> options;
        std::vector<bool> complete;
        std::size_t records{0};
        std::size_t nodes{0};

    private:
        CollectorLimits m_limits;
    };

    bool readCertificate(const Imported & imported, const ImportedPlan & plan, const SequentialInput & input,
                         MilliFunds floor, PackingDualCertificate & result, QString & error)
    {
        const auto saved = plan.evaluation.value("quote").toObject().value("ownedUpperCertificatePayload").toObject();
        const auto identity = saved.value("identity").toObject();
        auto & derived = result.identity;
        std::int64_t session = 0;
        if (!amount(identity.value("session"), session, 1, std::numeric_limits<std::int64_t>::max()) ||
            identity.value("key") != plan.evaluation.value("quote").toObject().value("key") ||
            identity.value("capturedColumns") != masterChoicesRecord(plan.mask) ||
            identity.value("immutableField") != imported.fieldRecord ||
            identity.value("nodeColumns") != columnsRecord(input.columns) ||
            identity.value("economic") != QLatin1String("0") ||
            identity.value("owned") != QString::number(plan.owned) ||
            identity.value("fixedValue") != QLatin1String("0") ||
            identity.value("incumbentFloor") != QString::number(floor))
        {
            return fail(error, QStringLiteral("saved certificate identity does not reconstruct"));
        }
        derived.session = static_cast<std::uint64_t>(session);
        derived.immutableField = imported.field;
        derived.key = plan.key;
        derived.capturedColumns = plan.mask;
        derived.nodeColumns = input.columns;
        derived.owned = plan.owned;
        derived.incumbentFloor = floor;
        derived.usedNodes = PackingDualNodeMask(static_cast<std::int32_t>(input.columns.size()));
        std::vector<std::int32_t> used;
        for (std::size_t node = 0; node < input.columns.size(); ++node)
        {
            if (input.capturedNodes[node])
            {
                derived.usedNodes.set(static_cast<std::int32_t>(node));
                used.push_back(static_cast<std::int32_t>(node));
            }
        }
        const auto savedUsed = identity.value("usedNodes").toObject();
        const auto domains = identity.value("rowDomains").toArray();
        if (!frameInteger(savedUsed, "nodeCount", static_cast<std::int32_t>(input.columns.size())) ||
            savedUsed.value("nodes") != masterChoicesRecord(used) ||
            static_cast<std::size_t>(domains.size()) != input.rows.size())
        {
            return fail(error, QStringLiteral("saved certificate mask or row-domain count differs"));
        }
        for (std::size_t row = 0; row < input.rows.size(); ++row)
        {
            const auto mover = std::find_if(plan.key.begin(), plan.key.end(),
                [&](const auto & action) { return action.row() == static_cast<std::int32_t>(row); });
            std::vector<std::int64_t> domain{static_cast<std::int64_t>(row), -1, -1, 0, 0};
            if (mover != plan.key.end())
            {
                domain[1] = mover->x();
                domain[2] = mover->y();
                domain[3] = movedCarry(imported.field, imported.field.ourRows[row], *mover);
            }
            domain.insert(domain.end(), plan.mask.begin(), plan.mask.end());
            if (domains[static_cast<qsizetype>(row)] != money(domain))
            {
                return fail(error, QStringLiteral("saved certificate row-domain identity differs"));
            }
            derived.rowDomains.push_back(std::move(domain));
        }
        if (!indices(saved.value("rows"), result.rows, input.rows.size(), 0,
                     static_cast<std::int32_t>(input.rows.size()) - 1) ||
            result.rows.size() != input.rows.size() ||
            !amounts(saved.value("nodePrices"), result.nodePrices, input.columns.size()) ||
            !amounts(saved.value("rowReducedMaxima"), result.rowReducedMaxima, input.rows.size()) ||
            std::any_of(result.nodePrices.begin(), result.nodePrices.end(), [](auto value) { return value < 0; }) ||
            std::any_of(result.rowReducedMaxima.begin(), result.rowReducedMaxima.end(), [](auto value) { return value < 0; }) ||
            !amount(saved.value("dualObjective"), result.dualObjective, 0))
        {
            return fail(error, QStringLiteral("invalid saved dual certificate payload"));
        }
        return true;
    }

    struct CertificateCheck
    {
        bool attempted{false};
        bool verified{false};
        std::int64_t work{0};
    };

    CertificateCheck verifyCollected(const RowCollector & rows, const PackingDualCertificate & certificate,
                                      std::int64_t remaining)
    {
        CertificateCheck result;
        if (std::find(rows.complete.begin(), rows.complete.end(), false) != rows.complete.end())
        {
            return result;
        }
        std::int64_t required = static_cast<std::int64_t>(certificate.nodePrices.size());
        if (!RelativeStockDetail::checkedAdd(required, 2 * static_cast<std::int64_t>(rows.nodes)) ||
            !RelativeStockDetail::checkedAdd(required, 2 * static_cast<std::int64_t>(rows.records)) ||
            required > remaining)
        {
            return result;
        }
        std::vector<PackingDualRowSource> source;
        for (std::size_t row = 0; row < rows.options.size(); ++row)
        {
            source.push_back({static_cast<std::int32_t>(row), certificate.identity.rowDomains[row], rows.options[row]});
        }
        result.attempted = true;
        const auto catalog = buildPackingDualCatalog(source, certificate.identity, remaining);
        result.work = catalog.constructionWork;
        if (!catalog.complete)
        {
            return result;
        }
        const auto checked = verifyPackingDualCertificate(catalog, source, certificate.identity,
            certificate.nodePrices, remaining - result.work, &certificate);
        result.work += checked.workUsed;
        result.verified = checked.valid && checked.completed;
        return result;
    }

    class Output
    {
    public:
        explicit Output(const QString & path) : m_file(path)
        {
        }

        bool open()
        {
            return m_file.open(QIODevice::WriteOnly | QIODevice::NewOnly);
        }

        bool write(const char* event, QJsonObject record)
        {
            record.insert("event", QLatin1String(event));
            record.insert("sequence", QString::number(m_sequence));
            const auto bytes = compact(record) + '\n';
            if (m_file.write(bytes) != bytes.size() || !m_file.flush())
            {
                return false;
            }
            ++m_sequence;
            return true;
        }

    private:
        QFile m_file;
        std::int64_t m_sequence{0};
    };

    struct Solved
    {
        StockInterval interval;
        std::int64_t work{0};
    };

    bool solveOne(const Config & config, const Imported & imported, std::size_t planIndex,
                  Term term, Output & output, Solved & solved, QString & error)
    {
        const auto & plan = imported.plans[planIndex];
        const auto input = reconstruct(imported.field, plan.key, plan.mask, term);
        NativeSequentialModel model(input);
        const auto instance = model.instance();
        const auto floor = assignmentFloor(input);
        const auto quote = plan.evaluation.value("quote").toObject();
        const auto interval = quote.value(term == Term::Friendly ? "ours" : "enemy").toArray();
        MilliFunds savedLower = 0;
        MilliFunds savedUpper = 0;
        if (interval.size() != 2 || !amount(interval[0], savedLower, floor) ||
            !amount(interval[1], savedUpper, savedLower) ||
            (term == Term::Friendly && !frameInteger(quote, "ourWitnessKind",
                static_cast<int>(RelativeStockWitnessKind::Sequential))))
        {
            return fail(error, QStringLiteral("saved interval or owned sequential witness kind is invalid"));
        }
        std::vector<SequentialRowWitness> seed;
        if (!readWitness(quote.value(term == Term::Friendly ? "ourWitness" : "enemyWitness"),
                         instance, savedLower, seed, error))
        {
            return false;
        }
        PackingDualCertificate certificate;
        if (term == Term::Friendly && !readCertificate(imported, plan, input, floor, certificate, error))
        {
            return false;
        }
        std::int64_t reserved = 0;
        if (term == Term::Friendly)
        {
            const auto reservation = verificationReservation(instance.nodeCount(), {}, config.stateCap);
            if (!reservation)
            {
                return fail(error, QStringLiteral("offline certificate reservation arithmetic overflow"));
            }
            reserved = *reservation;
        }
        const auto searchGrant = config.stateCap - reserved;
        RowCollector rows(input.rows.size());
        const SequentialWarmStart warm{seed, savedLower};
        QElapsedTimer timer;
        timer.start();
        const auto result = solveSequentialPacking(instance, floor, searchGrant,
            term == Term::Friendly ? &rows : nullptr, true, SequentialWitnessPolicy::BookedValue,
            SequentialTierAuditMode::CountersOnly, &warm, nullptr, SequentialPackingPolicy::CompatibleRows);
        const auto elapsed = timer.nsecsElapsed();
        solved.work = result.searchStates;
        if (!result.warmStartAccepted || result.searchStates < 0 || result.searchStates > searchGrant ||
            result.bookedValue < savedLower || result.bookedValue > result.relaxedValue ||
            (result.certificate && result.bookedValue != result.relaxedValue) ||
            !replaySequentialWitness(instance, result.witness, result.bookedValue))
        {
            return fail(error, QStringLiteral("offline native solve or returned witness failed validation"));
        }
        solved.interval = {result.bookedValue, result.relaxedValue};
        if (result.searchCompleted || result.certificate)
        {
            solved.interval.upper = result.bookedValue;
        }
        CertificateCheck checked;
        if (term == Term::Friendly && reserved > 0)
        {
            checked = verifyCollected(rows, certificate, config.stateCap - result.searchStates);
            solved.work += checked.work;
            if (checked.attempted && !checked.verified)
            {
                return fail(error, QStringLiteral("saved dual certificate failed complete regenerated-catalog verification"));
            }
            if (checked.verified)
            {
                solved.interval.upper = std::min(solved.interval.upper, certificate.dualObjective);
            }
        }
        if (solved.work > config.stateCap || solved.interval.lower > solved.interval.upper)
        {
            return fail(error, QStringLiteral("offline checked interval or total state/verification quota is invalid"));
        }
        const auto domain = inputRecord(input);
        return output.write(SOLVE, {{"plan", static_cast<qint64>(planIndex)},
            {"term", QLatin1String(TERM_NAMES[static_cast<std::size_t>(term)])},
            {"input", domain}, {"inputSHA256", hash(compact(domain))},
            {"savedIntervalHistorical", interval}, {"savedLowerReplayed", true},
            {"savedUpperPreserved", checked.verified && certificate.dualObjective == savedUpper},
            {"savedCertificateVerificationAttempted", checked.attempted},
            {"savedCertificateVerified", checked.verified},
            {"nativeFloor", QString::number(floor)}, {"nativeRelaxedUpper", QString::number(result.relaxedValue)},
            {"searchCompleted", result.searchCompleted}, {"nativeCertificate", result.certificate},
            {"warmStartAccepted", result.warmStartAccepted},
            {"interval", QJsonArray{QString::number(solved.interval.lower), QString::number(solved.interval.upper)}},
            {"searchGrant", QString::number(searchGrant)},
            {"verificationReservation", QString::number(reserved)},
            {"verificationReservationFunded", reserved > 0},
            {"searchStates", QString::number(result.searchStates)},
            {"verificationWork", QString::number(checked.work)}, {"totalQuotaUsed", QString::number(solved.work)},
            {"nativeSolveNanos", QString::number(elapsed)}, {"witness", witnessRecord(result.witness)},
            {"savedWitness", witnessRecord(seed)}, {"returnedWitnessReplayed", true},
            {"completeCollectedRows", static_cast<qint64>(std::count(rows.complete.begin(), rows.complete.end(), true))},
            {"collectedOptions", QString::number(rows.records)}, {"collectedIncidences", QString::number(rows.nodes)}}) ||
            fail(error, QStringLiteral("offline solve output failed"));
    }

    bool difference(const Imported & imported, const std::array<Solved, SOLVE_COUNT> & solved,
                    StockInterval & result)
    {
        MilliFunds constant = imported.plans[1].seated;
        if (!RelativeStockDetail::checkedSubtract(constant, imported.plans[0].seated) ||
            !RelativeStockDetail::checkedAdd(constant, imported.plans[1].owned) ||
            !RelativeStockDetail::checkedSubtract(constant, imported.plans[0].owned))
        {
            return false;
        }
        result = {constant, constant};
        return RelativeStockDetail::checkedAdd(result.lower, solved[2].interval.lower) &&
            RelativeStockDetail::checkedSubtract(result.lower, solved[0].interval.upper) &&
            RelativeStockDetail::checkedSubtract(result.lower, solved[3].interval.upper) &&
            RelativeStockDetail::checkedAdd(result.lower, solved[1].interval.lower) &&
            RelativeStockDetail::checkedAdd(result.upper, solved[2].interval.upper) &&
            RelativeStockDetail::checkedSubtract(result.upper, solved[0].interval.lower) &&
            RelativeStockDetail::checkedSubtract(result.upper, solved[3].interval.lower) &&
            RelativeStockDetail::checkedAdd(result.upper, solved[1].interval.upper) && result.lower <= result.upper;
    }

    int runQuery(const Config & config, const Imported & imported, const QString & requestHash)
    {
        Output output(config.output);
        if (!output.open())
        {
            qWarning() << "Offline reference exclusive output open failed:" << config.output;
            return 4;
        }
        QString error;
        std::array<Solved, SOLVE_COUNT> solved;
        std::int32_t completed = 0;
        bool valid = output.write(BEGIN, {{"schemaVersion", 1}, {"trace", config.trace},
            {"traceSHA256", config.expectedSHA256}, {"requestSHA256", requestHash},
            {"modelSHA256", imported.header.value("modelSHA256")}, {"sourceRevision", imported.header.value("sourceRevision")},
            {"familyIndices", QJsonArray{0, 1}}, {"family", imported.header.value("family")},
            {"perSolveStateCap", QString::number(config.stateCap)}, {"maximumSolves", SOLVE_COUNT},
            {"nativeReferenceFrames", imported.records}, {"nativeReferenceSnapshots", imported.snapshots},
            {"immutableFieldSHA256", hash(compact(imported.fieldRecord))},
            {"immutableField", imported.fieldRecord}, {"fieldFirstLegWeightsAndArrivalsReconstructed", true},
            {"diagnosticOnly", true}, {"liveSessionAuthority", false}, {"productionWorkClaimed", false},
            {"legalityImportedNotReexecuted", true}, {"importSetupReplaySerializationOutsideStateQuota", true},
            {"catalogCopiesOutsideStateQuota", true},
            {"maximumCollectedOptions", QString::number(MAX_COLLECTED_OPTIONS)},
            {"maximumCollectedIncidences", QString::number(MAX_COLLECTED_INCIDENCES)},
            {"collectorCapsBoundOnlyDiagnosticCopies", true},
            {"nativeTransientOrTotalMemoryBoundClaimed", false},
            {"additionalCertificateEnumeration", false}});
        if (!valid)
        {
            error = QStringLiteral("offline begin output failed");
        }
        for (std::size_t plan = 0; valid && plan < imported.plans.size(); ++plan)
        {
            for (const auto term : {Term::Friendly, Term::Enemy})
            {
                valid = solveOne(config, imported, plan, term, output,
                                 solved[static_cast<std::size_t>(completed)], error);
                if (!valid)
                {
                    break;
                }
                ++completed;
            }
        }
        StockInterval relative;
        if (valid && completed != SOLVE_COUNT)
        {
            valid = fail(error, QStringLiteral("offline query did not complete its declared solve count"));
        }
        if (valid && !difference(imported, solved, relative))
        {
            valid = fail(error, QStringLiteral("offline comparison arithmetic or interval consistency failed"));
        }
        const char* reason = UNRESOLVED;
        if (valid && relative.lower > 0)
        {
            reason = POSITIVE;
        }
        else if (valid && relative.upper <= 0)
        {
            reason = NON_IMPROVEMENT;
        }
        QJsonObject end{{"result", QLatin1String(valid ? OFFLINE_COMPLETE : OFFLINE_FAILED)},
            {"completedSolves", completed}, {"reason", QLatin1String(reason)}, {"error", error},
            {"totalQuotaUsed", QString::number(std::accumulate(solved.begin(), solved.end(), std::int64_t{0},
                [](std::int64_t total, const Solved & entry) { return total + entry.work; }))},
            {"globalOptimumClaimed", false}, {"liveCertificateClaimed", false}, {"installed", false}};
        if (valid)
        {
            end.insert("relativeInterval", QJsonArray{QString::number(relative.lower), QString::number(relative.upper)});
        }
        if (!output.write(END, end))
        {
            valid = fail(error, QStringLiteral("offline terminal output failed"));
        }
        if (!valid)
        {
            qWarning() << "Offline stock reference failed:" << error;
        }
        return valid ? 0 : 4;
    }

    PackingDualFieldIdentity smallField()
    {
        constexpr MilliFunds income = 1000000;
        PackingDualFieldIdentity field;
        field.width = 3;
        field.height = 1;
        field.horizonTurns = 10;
        field.capturePointsToCapture = 20;
        field.ourColumns = {{0, {0, 0}, {income, income}, OwnerSign::Neutral, 0}};
        field.enemyColumns = {mirroredColumn(field.ourColumns.front())};
        field.ourRows = {{0, 0, 1, 10, 10, {0, 0}}};
        field.enemyRows = {{1, 0, 1, 0, 10, {2, 0}}};
        field.columnTiles = {{0, 0}};
        field.columnCapturers = {0};
        field.columnCapturePoints = {10};
        MobilityCostGrid grid(field.width, field.height);
        for (std::int32_t x = 0; x < field.width; ++x)
        {
            for (const auto direction : ENTRY_DIRECTIONS)
            {
                grid.setCost(x, 0, direction, 1);
            }
        }
        field.mobilityGrids.push_back(grid);
        field.ourWeights = {columnWeight(field.ourColumns[0], {1, 10, 10}, 20, 10)};
        field.enemyArrivals = arrivals(field, field.enemyRows[0], field.enemyRows[0].tile);
        field.enemyWeights = {columnWeight(field.enemyColumns[0], {field.enemyArrivals[0], 0, 10}, 20, 10)};
        return field;
    }

    MasterAssignmentProblem smallProblem()
    {
        Fixture fixture(1);
        fixture.input.actors[0].candidates[0] = moveCandidate(0, {0, 0}, {0, 0}, 0);
        auto capture = fixture.input.actors[0].candidates[0];
        capture.generationIndex = 1;
        capture.bundle.components.push_back(captureComponent({.income = {1000000, 1000000}, .turnsUntilOwned = 0}));
        fixture.input.actors[0].candidates.push_back(capture);
        fixture.seat();
        fixture.selections[0].stockRow = 0;
        return fixture.extract();
    }

    PackingDualCertificate smallCertificate(const PackingDualFieldIdentity & field, const ContinuationKey & key,
                                            const SequentialInput & input, MilliFunds value)
    {
        PackingDualCertificate certificate;
        auto & identity = certificate.identity;
        identity.session = 1;
        identity.immutableField = field;
        identity.key = key;
        identity.capturedColumns = captured(field, key);
        identity.nodeColumns = input.columns;
        identity.usedNodes = PackingDualNodeMask(static_cast<std::int32_t>(input.columns.size()));
        for (std::size_t node = 0; node < input.columns.size(); ++node)
        {
            if (input.capturedNodes[node])
            {
                identity.usedNodes.set(static_cast<std::int32_t>(node));
            }
        }
        identity.owned = field.ownedBaseline;
        for (const auto column : identity.capturedColumns)
        {
            identity.owned += ownershipFlipSwing(field.ourColumns[static_cast<std::size_t>(column)], field.horizonTurns);
        }
        identity.incumbentFloor = assignmentFloor(input);
        std::vector<std::int64_t> domain{0, 0, 0, movedCarry(field, field.ourRows[0], key[0]), 0};
        domain.insert(domain.end(), identity.capturedColumns.begin(), identity.capturedColumns.end());
        identity.rowDomains = {domain};
        certificate.rows = {0};
        certificate.nodePrices = {0};
        certificate.rowReducedMaxima = {value};
        certificate.dualObjective = value;
        return certificate;
    }

    QByteArray smallTrace()
    {
        const auto field = smallField();
        const auto problem = smallProblem();
        const auto model = masterModelRecord(problem);
        const QJsonArray family{QJsonArray{0}, QJsonArray{1}};
        QJsonArray frames;
        frames.append(QJsonObject{{"event", QLatin1String(BEGIN)}, {"schemaVersion", 1},
            {"model", model}, {"modelSHA256", hash(compact(model))}, {"sourceRevision", "synthetic-offline"},
            {"family", family}, {"incumbent", family[0]}, {"plannedSteps", 0}, {"horizon", field.horizonTurns},
            {"diagnosticOnly", true}, {"productionSearchRun", false}, {"installed", false}});
        QJsonArray plans;
        for (std::int32_t index = 0; index < 2; ++index)
        {
            const std::vector<std::int32_t> choices{index};
            MasterAssignmentOracle::Request request;
            request.sourceRevision = QStringLiteral("synthetic-offline");
            request.fixture = QStringLiteral("offline-import");
            request.domain = {{index}};
            request.restriction = QStringLiteral("One synthetic fixed-plan member for offline import validation.");
            request.continuation.mode = MasterAssignmentOracle::ContinuationMode::Unavailable;
            const auto reference = MasterAssignmentOracle::enumerate(problem, request);
            require(reference.tuples.size() == 1 && reference.tuples[0].reference.complete &&
                    reference.tuples[0].comparisonApplicable && reference.tuples[0].comparisonEqual,
                    "offline synthetic singleton has independent and native reconstruction agreement");
            frames.append(QJsonObject{{"event", QLatin1String(LEGALITY)}, {"plan", index},
                {"reference", QJsonDocument::fromJson(
                    MasterAssignmentOracle::exportDiagnostic(problem, request, reference)).object()}});
            const auto replay = replayMasterAssignment(problem, choices);
            const auto mask = captured(field, replay.continuationKey);
            const auto friendly = reconstruct(field, replay.continuationKey, mask, Term::Friendly);
            const auto enemy = reconstruct(field, replay.continuationKey, mask, Term::Enemy);
            const auto ours = MasterAssignmentOracle::exhaustSequential(friendly, 1000);
            const auto theirs = MasterAssignmentOracle::exhaustSequential(enemy, 1000);
            require(ours.status == MasterAssignmentOracle::ObjectiveStatus::Exact &&
                    theirs.status == MasterAssignmentOracle::ObjectiveStatus::Exact, "offline synthetic independent oracle completes");
            auto proof = std::make_shared<RelativeStockProofSnapshot>();
            const auto certificate = smallCertificate(field, replay.continuationKey, friendly, ours.value);
            proof->quote.key = replay.continuationKey;
            proof->quote.capturedColumns = mask;
            proof->quote.owned = certificate.identity.owned;
            proof->quote.ours = {ours.value, ours.value};
            proof->quote.enemy = {theirs.value, theirs.value};
            proof->ourKind = RelativeStockWitnessKind::Sequential;
            proof->ourWitness = ours.witness;
            proof->enemyWitness = theirs.witness;
            proof->ourUpperCertificate = std::make_shared<PackingDualCertificate>(certificate);
            MasterAssignmentEvaluation evaluation{
                .state = {problem.identity(), choices}, .status = MasterEvaluationStatus::Feasible,
                .objectiveStatus = MasterObjectiveStatus::Verified, .replay = replay, .proof = proof,
                .objective = StockInterval{proof->quote.owned + ours.value - theirs.value,
                                          proof->quote.owned + ours.value - theirs.value},
                .lowerReplayed = true, .upperVerified = true, .exact = true};
            plans.append(QJsonObject{{"plan", index}, {"choices", family[index]},
                {"disposition", static_cast<int>(MasterAssignmentOracle::TupleStatus::Feasible)},
                {"evaluated", true}, {"evaluationCurrent", true}, {"evaluation", masterEvaluationRecord(evaluation)}});
        }
        frames.append(QJsonObject{{"event", QLatin1String(SNAPSHOT)}, {"step", -1}, {"plans", plans}});
        frames.append(QJsonObject{{"event", QLatin1String(END)}, {"result", QLatin1String(REFERENCE_COMPLETE)},
            {"outcome", "StepsExhausted"}, {"familySize", 2}, {"references", 2},
            {"completedSteps", 0}, {"snapshots", 1}, {"errors", QJsonArray{}}});
        QByteArray bytes;
        for (qsizetype index = 0; index < frames.size(); ++index)
        {
            auto frame = frames[index].toObject();
            frame.insert("sequence", QString::number(index));
            bytes += compact(frame) + '\n';
        }
        return bytes;
    }

    void importBoundaries()
    {
        const auto bytes = smallTrace();
        Imported imported;
        QString error;
        const bool importedValid = readImported(bytes, imported, error);
        if (!importedValid)
        {
            qWarning() << "Synthetic offline import failed:" << error;
        }
        require(importedValid, "synthetic native-format field and singleton reference import");
        const auto friendly = reconstruct(imported.field, imported.plans[0].key, {}, Term::Friendly);
        const auto enemy = reconstruct(imported.field, imported.plans[1].key, {0}, Term::Enemy);
        expect(friendly.rows[0].weight == std::vector<MilliFunds>{9000000} &&
                   friendly.rows[0].ownedTurn == std::vector<std::int32_t>{1} &&
                   friendly.classes[0].captureTurns == 2 &&
                   friendly.classes[0].interArrivals == std::vector<std::int32_t>{1} &&
                   enemy.rows[0].weight == std::vector<MilliFunds>{7000000} &&
                   enemy.rows[0].ownedTurn == std::vector<std::int32_t>{3} &&
                   enemy.columns[0].ownerBefore == OwnerSign::Neutral,
               "offline first-leg, carried capture, mirrored reward and class-time parity");
        for (int fault = 0; fault < 7; ++fault)
        {
            auto malformed = bytes;
            if (fault == 0) { malformed.chop(1); }
            if (fault == 1) { malformed.replace("\"sequence\":\"3\"", "\"sequence\":\"8\""); }
            if (fault == 2) { malformed.replace("\"width\":3", "\"width\":129"); }
            if (fault == 3) { malformed.replace("\"captureRate\":10", "\"captureRate\":0"); }
            if (fault == 4) { malformed.replace("\"9000000\"", "\"9223372036854775808\""); }
            if (fault == 5) { malformed.replace("\"sequence\":\"0\"", "\"sequence\":\"0\",\"sequence\":\"0\""); }
            if (fault == 6) { malformed.replace("\"completedSteps\":0", "\"completedSteps\":1"); }
            Imported rejected;
            error.clear();
            expect(!readImported(malformed, rejected, error), "offline malformed framing, shape, overflow or duplicate rejected");
        }
        auto badMask = imported;
        auto quote = badMask.plans[1].evaluation.value("quote").toObject();
        quote.insert("capturedColumns", QJsonArray{});
        badMask.plans[1].evaluation.insert("quote", quote);
        ImportedPlan checked;
        checked.evaluation = badMask.plans[1].evaluation;
        checked.legality = badMask.plans[1].legality;
        expect(!verifyPlan(badMask, 1, checked, error), "offline capture mask is derived, not trusted");
        auto field = imported.field;
        field.enemyWeights[0] += 1;
        expect(!verifyFieldRows(field, Term::Enemy, error), "offline original enemy weights must match native arrivals");
        const NativeSequentialModel model(friendly);
        std::vector<SequentialRowWitness> witness;
        expect(readWitness(QJsonArray{QJsonObject{{"row", 0}, {"value", "9000000"},
                   {"visits", QJsonArray{QJsonArray{0, 1}}}}}, model.instance(), 9000000, witness, error),
               "offline imported gains replay against actual native model");
        witness.clear();
        expect(!readWitness(QJsonArray{QJsonObject{{"row", 0}, {"value", "9000001"},
                   {"visits", QJsonArray{QJsonArray{0, 1}}}}}, model.instance(), 9000001, witness, error),
               "offline corrupted witness total fails native replay");
        auto sentinel = friendly;
        sentinel.columns.push_back({1, {1, 0}, {1000000, 1000000}, OwnerSign::Neutral, 1});
        sentinel.classes[0].interArrivals = {1, 1, 1, 1};
        sentinel.rows[0].weight = {10000000, 9000000};
        sentinel.rows[0].ownedTurn = {NO_CAPTURE_TURNS, 1};
        sentinel.capturedNodes = {false, false};
        const NativeSequentialModel sentinelModel(sentinel);
        witness.clear();
        expect(readWitness(QJsonArray{QJsonObject{{"row", 0}, {"value", "10000000"},
                   {"visits", QJsonArray{QJsonArray{0, -1}}}}}, sentinelModel.instance(), 10000000, witness, error),
               "offline sentinel first leg preserves its nonzero singleton value");
        witness.clear();
        expect(!readWitness(QJsonArray{QJsonObject{{"row", 0}, {"value", "19000000"},
                   {"visits", QJsonArray{QJsonArray{0, -1}, QJsonArray{1, 1}}}}},
                   sentinelModel.instance(), 19000000, witness, error),
               "offline sentinel first leg cannot start a chain");
    }

    void queryBoundaries()
    {
        QTemporaryDir directory;
        require(directory.isValid(), "offline query temporary directory");
        const auto bytes = smallTrace();
        const auto tracePath = directory.filePath("input.jsonl");
        QFile trace(tracePath);
        require(trace.open(QIODevice::WriteOnly | QIODevice::NewOnly) && trace.write(bytes) == bytes.size(),
                "offline synthetic trace write");
        trace.close();
        const auto requestPath = directory.filePath("request.json");
        QJsonObject request{{CONFIG_KEYS[0], tracePath}, {CONFIG_KEYS[1], hash(bytes)},
            {CONFIG_KEYS[2], directory.filePath("output.jsonl")}, {CONFIG_KEYS[3], QJsonArray{0, 1}},
            {CONFIG_KEYS[4], "128"}, {CONFIG_KEYS[5], SOLVE_COUNT}};
        QFile config(requestPath);
        require(config.open(QIODevice::WriteOnly | QIODevice::NewOnly) && config.write(compact(request)) > 0,
                "offline synthetic request write");
        config.close();
        expect(runOfflineStockReference(requestPath) == 0, "offline finite four-solve synthetic query completes");
        QFile output(request.value(CONFIG_KEYS[2]).toString());
        require(output.open(QIODevice::ReadOnly), "offline synthetic query output");
        const auto records = output.readAll().split('\n');
        expect(records.size() == SOLVE_COUNT + 3, "offline exclusive begin/four solves/end framing");
        const auto end = QJsonDocument::fromJson(records[records.size() - 2]).object();
        expect(end.value("result") == QLatin1String(OFFLINE_COMPLETE) && end.value("reason") == QLatin1String(POSITIVE) &&
                   end.value("relativeInterval") == QJsonArray{"1000000", "1000000"},
               "offline exact synthetic sign agrees with the one-turn capture acceleration");
        output.close();
        expect(runOfflineStockReference(requestPath) != 0, "offline query never overwrites an earlier result");
        for (int fault = 0; fault < 4; ++fault)
        {
            auto bad = request;
            bad.insert(CONFIG_KEYS[2], directory.filePath("unused.jsonl"));
            if (fault == 0) { bad.insert(CONFIG_KEYS[3], QJsonArray{0, 2}); }
            if (fault == 1) { bad.insert(CONFIG_KEYS[4], "4000000001"); }
            if (fault == 2) { bad.insert(CONFIG_KEYS[5], SOLVE_COUNT + 1); }
            auto encoded = compact(bad);
            if (fault == 3) { encoded.replace("\"maximumSolves\":4", "\"maximumSolves\":4,\"maximumSolves\":4"); }
            Config rejected;
            QString error;
            expect(!readConfig(encoded, rejected, error), "offline finite config rejects changed family, cap, solve count or duplicate");
        }
    }

    void certificateBoundaries()
    {
        const auto field = smallField();
        const ContinuationKey key{CanonicalPlanActionKey::fromAction(0, 0, 0, false)};
        const auto input = reconstruct(field, key, {}, Term::Friendly);
        const NativeSequentialModel model(input);
        SequentialDetail::EnumerationBudget budget{0, 128, false};
        std::int64_t itineraries = 0;
        const auto options = SequentialDetail::enumerateRowOptions(
            model.instance(), 0, input.rows[0].weight, budget, itineraries);
        RowCollector rows(input.rows.size());
        rows.store(0, options, budget.states);
        const auto certificate = smallCertificate(field, key, input, 9000000);
        const auto verified = verifyCollected(rows, certificate, 128);
        expect(verified.attempted && verified.verified && verified.work > 0 && verified.work <= 128,
               "offline saved dual uses actual complete native catalog and existing verifier");
        const auto refused = verifyCollected(rows, certificate, 0);
        expect(!refused.attempted && !refused.verified && refused.work == 0,
               "offline saved dual remains unknown when its quota is unavailable");
        auto malformed = certificate;
        ++malformed.dualObjective;
        const auto rejected = verifyCollected(rows, malformed, 128);
        expect(rejected.attempted && !rejected.verified, "offline changed dual objective fails existing verifier");
        rows.complete[0] = false;
        const auto incomplete = verifyCollected(rows, certificate, 128);
        expect(!incomplete.attempted && !incomplete.verified, "offline partial catalog cannot authenticate saved upper");
    }

    void reservedCertificateBoundary()
    {
        constexpr CollectorLimits limits{21, 36};
        constexpr std::int64_t enumerationWork = 45;
        constexpr std::int64_t searchGrant = enumerationWork + 1;
        constexpr std::int64_t reserve = 3 + 2 * static_cast<std::int64_t>(limits.incidences) +
            2 * static_cast<std::int64_t>(limits.options);
        constexpr std::int64_t quota = searchGrant + reserve;
        SequentialInput input;
        input.horizon = 6;
        for (std::int32_t node = 0; node < 3; ++node)
        {
            const MilliFunds income = 100 - 10 * node;
            input.columns.push_back({node, {node, 0}, {income, income}, OwnerSign::Neutral, node});
        }
        input.classes = {{1, {-1, 1, 1, 1, -1, 1, 1, 1, -1}}};
        input.rows = {{0, {100, 1, 1}, {1, 1, 1}}, {0, {1, 90, 1}, {1, 1, 1}},
                      {0, {1, 1, 80}, {1, 1, 1}}};
        input.capturedNodes = {false, false, false};
        const auto reserved = verificationReservation(3, limits, quota);
        require(reserved && *reserved == reserve, "offline certificate quota is reserved before search");
        expect(verificationReservation(3, {}, 128) == 0,
               "tiny offline cap explicitly cannot fund the full certificate reservation");
        const auto extreme = verificationReservation(3, {std::numeric_limits<std::size_t>::max(), 0}, quota);
        if constexpr (std::numeric_limits<std::size_t>::digits >= std::numeric_limits<std::int64_t>::digits)
        {
            expect(!extreme, "offline certificate reservation overflow is rejected");
        }
        else
        {
            expect(extreme && *extreme == 0, "wide valid reservation cannot consume a smaller query cap");
        }
        const NativeSequentialModel model(input);
        RowCollector rows(input.rows.size(), limits);
        const auto searched = solveSequentialPacking(model.instance(), assignmentFloor(input), quota - *reserved,
            &rows, true, SequentialWitnessPolicy::BookedValue, SequentialTierAuditMode::CountersOnly,
            nullptr, nullptr, SequentialPackingPolicy::CompatibleRows);
        require(!searched.searchCompleted && searched.searchStates == searchGrant &&
                    std::count(rows.complete.begin(), rows.complete.end(), true) == 3 &&
                    rows.records == limits.options && rows.nodes == limits.incidences,
                "interrupted native packing retains all complete rows within its reduced search grant");
        PackingDualCertificate certificate;
        auto & identity = certificate.identity;
        identity.session = 1;
        identity.immutableField.ourColumns = input.columns;
        identity.nodeColumns = input.columns;
        identity.usedNodes = PackingDualNodeMask(3);
        identity.incumbentFloor = assignmentFloor(input);
        certificate.nodePrices = {0, 0, 0};
        for (std::size_t row = 0; row < rows.options.size(); ++row)
        {
            identity.rowDomains.push_back({static_cast<std::int64_t>(row)});
            certificate.rows.push_back(static_cast<std::int32_t>(row));
            const auto maximum = rows.options[row].front().value;
            certificate.rowReducedMaxima.push_back(maximum);
            certificate.dualObjective += maximum;
        }
        const auto verified = verifyCollected(rows, certificate, quota - searched.searchStates);
        const auto exact = MasterAssignmentOracle::exhaustSequential(input, 1000000);
        expect(verified.attempted && verified.verified && verified.work <= *reserved &&
                   searched.searchStates + verified.work <= quota &&
                   exact.status == MasterAssignmentOracle::ObjectiveStatus::Exact &&
                   certificate.dualObjective >= exact.value &&
                   replaySequentialWitness(model.instance(), searched.witness, searched.bookedValue),
               "reserved verifier validates complete domains after packing interruption without another enumeration");
        RowCollector undersized(input.rows.size(), {0, 0});
        undersized.store(0, rows.options[0], enumerationWork);
        expect(!undersized.complete[0] && undersized.options[0].empty(),
               "collector copy caps do not pretend to bound or complete native transient enumeration");
    }
}

namespace MasterAssignmentTest
{
    int runOfflineStockReference(const QString & requestPath)
    {
        QByteArray requestBytes;
        QByteArray traceBytes;
        QString error;
        Config config;
        Imported imported;
        if (!readBytes(requestPath, MAX_REQUEST_BYTES, requestBytes, error) ||
            !readConfig(requestBytes, config, error) ||
            !readBytes(config.trace, MAX_TRACE_BYTES, traceBytes, error))
        {
            qWarning() << "Offline stock import rejected:" << error;
            return 4;
        }
        if (hash(traceBytes) != config.expectedSHA256)
        {
            qWarning() << "Offline stock import rejected: trace SHA256 mismatch";
            return 4;
        }
        if (!readImported(traceBytes, imported, error))
        {
            qWarning() << "Offline stock import rejected:" << error;
            return 4;
        }
        return runQuery(config, imported, hash(requestBytes));
    }

    void testOfflineStockReference()
    {
        importBoundaries();
        queryBoundaries();
        certificateBoundaries();
        reservedCertificateBoundary();
    }

    bool readStockField(const QJsonObject & object, PackingDualFieldIdentity & field, QString & error)
    {
        return readField(object, field, error);
    }

    bool readStockKey(const QJsonValue & value, const PackingDualFieldIdentity & field, ContinuationKey & key)
    {
        return readKey(value, field, key);
    }

    std::vector<std::int32_t> capturedStockColumns(const PackingDualFieldIdentity & field, const ContinuationKey & key)
    {
        return captured(field, key);
    }

    SequentialInput friendlyStockInput(const PackingDualFieldIdentity & field, const ContinuationKey & key)
    {
        return reconstruct(field, key, captured(field, key), Term::Friendly);
    }
}
