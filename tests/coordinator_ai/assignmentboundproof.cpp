#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <vector>

#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/propertystock.h"

namespace
{
using Coordinator::assignmentUpperBound;
using Coordinator::isPruneOrderLicensed;
using Coordinator::MilliFunds;

constexpr std::int32_t NO_RESOURCE = -1;
constexpr std::int32_t ROW_COUNT = 4;
constexpr std::int32_t RESOURCE_COUNT = 4;
constexpr std::int32_t INSTANCE_COUNT = 300;
constexpr std::int32_t VALUE_MINIMUM = -40;
constexpr std::int32_t VALUE_MAXIMUM = 120;
constexpr std::int32_t OPTION_INCLUDED_IN = 4;
constexpr std::uint32_t GENERATOR_SEED = 0x4B3B0A5Du;
constexpr std::uint32_t XORSHIFT_LEFT = 13u;
constexpr std::uint32_t XORSHIFT_RIGHT = 17u;
constexpr std::uint32_t XORSHIFT_LAST = 5u;
constexpr std::int32_t PROPERTY_HORIZON = 6;
constexpr std::int32_t PROPERTY_INSTANCE_COUNT = 120;
constexpr std::int32_t ENEMY_ROW_COUNT = 2;

int failures = 0;

void expect(bool condition, const char* description)
{
    if (!condition)
    {
        std::printf("FAILED: %s\n", description);
        ++failures;
    }
}

struct Option
{
    std::int32_t resource{NO_RESOURCE};
    MilliFunds value{0};
};

struct Instance
{
    std::vector<std::vector<Option>> rows;
};

std::uint32_t nextRandom(std::uint32_t & state)
{
    state ^= state << XORSHIFT_LEFT;
    state ^= state >> XORSHIFT_RIGHT;
    state ^= state << XORSHIFT_LAST;
    return state;
}

std::int32_t nextInRange(std::uint32_t & state, std::int32_t low, std::int32_t high)
{
    const std::uint32_t span = static_cast<std::uint32_t>(high - low + 1);
    return low + static_cast<std::int32_t>(nextRandom(state) % span);
}

Instance generateInstance(std::uint32_t & state)
{
    Instance instance;
    for (std::int32_t row = 0; row < ROW_COUNT; ++row)
    {
        std::vector<Option> options;
        for (std::int32_t resource = 0; resource < RESOURCE_COUNT; ++resource)
        {
            if (nextInRange(state, 0, OPTION_INCLUDED_IN - 1) != 0)
            {
                options.push_back(Option{
                    resource,
                    nextInRange(state, VALUE_MINIMUM, VALUE_MAXIMUM),
                });
            }
        }
        options.push_back(Option{});
        for (std::size_t slot = options.size(); slot > 1; --slot)
        {
            const std::size_t other =
                static_cast<std::size_t>(nextRandom(state) % slot);
            std::swap(options[slot - 1], options[other]);
        }
        instance.rows.push_back(std::move(options));
    }
    return instance;
}

bool canTake(const std::vector<bool> & taken, const Option & option)
{
    return option.resource == NO_RESOURCE ||
           !taken[static_cast<std::size_t>(option.resource)];
}

void setTaken(std::vector<bool> & taken, const Option & option, bool value)
{
    if (option.resource != NO_RESOURCE)
    {
        taken[static_cast<std::size_t>(option.resource)] = value;
    }
}

MilliFunds remainingCeiling(const Instance & instance, std::size_t depth)
{
    MilliFunds ceiling = 0;
    for (std::size_t row = depth; row < instance.rows.size(); ++row)
    {
        MilliFunds best = 0;
        for (const Option & option : instance.rows[row])
        {
            best = std::max(best, option.value);
        }
        ceiling += best;
    }
    return ceiling;
}

MilliFunds exactBestAdditional(const Instance & instance, std::size_t depth,
                               std::vector<bool> & taken)
{
    if (depth == instance.rows.size())
    {
        return 0;
    }
    MilliFunds best = std::numeric_limits<MilliFunds>::min();
    for (const Option & option : instance.rows[depth])
    {
        if (!canTake(taken, option))
        {
            continue;
        }
        setTaken(taken, option, true);
        best = std::max(best, option.value +
                                 exactBestAdditional(instance, depth + 1, taken));
        setTaken(taken, option, false);
    }
    return best;
}

void verifyEveryReachableBound(const Instance & instance, std::size_t depth,
                               MilliFunds seated, std::vector<bool> & taken,
                               std::int32_t & nodes)
{
    if (depth == instance.rows.size())
    {
        return;
    }
    for (const Option & option : instance.rows[depth])
    {
        if (!canTake(taken, option))
        {
            continue;
        }
        const MilliFunds bound = assignmentUpperBound(
            seated, option.value, remainingCeiling(instance, depth + 1));
        setTaken(taken, option, true);
        const MilliFunds exact = seated + option.value +
                                 exactBestAdditional(instance, depth + 1, taken);
        expect(bound >= exact,
               "the suffix bound dominates every legal completion");
        ++nodes;
        verifyEveryReachableBound(
            instance, depth + 1, seated + option.value, taken, nodes);
        setTaken(taken, option, false);
    }
}

struct Search
{
    MilliFunds best{0};
    bool hasBest{false};
};

std::vector<Option> orderedOptions(const std::vector<Option> & source)
{
    std::vector<Option> options = source;
    std::stable_sort(options.begin(), options.end(),
                     [](const Option & left, const Option & right)
                     {
                         return left.value > right.value;
                     });
    return options;
}

std::vector<MilliFunds> valuesOf(std::span<const Option> options)
{
    std::vector<MilliFunds> values;
    values.reserve(options.size());
    for (const Option & option : options)
    {
        values.push_back(option.value);
    }
    return values;
}

void boundedSearch(const Instance & instance, std::size_t depth, MilliFunds seated,
                   std::vector<bool> & taken, Search & search)
{
    if (depth == instance.rows.size())
    {
        if (!search.hasBest || seated > search.best)
        {
            search.best = seated;
            search.hasBest = true;
        }
        return;
    }
    const std::vector<Option> options = orderedOptions(instance.rows[depth]);
    const std::vector<MilliFunds> values = valuesOf(options);
    expect(isPruneOrderLicensed(std::span<const MilliFunds>(values)),
           "the searched option list licenses an early return");
    for (const Option & option : options)
    {
        const MilliFunds bound = assignmentUpperBound(
            seated, option.value, remainingCeiling(instance, depth + 1));
        if (search.hasBest && bound <= search.best)
        {
            return;
        }
        if (!canTake(taken, option))
        {
            continue;
        }
        setTaken(taken, option, true);
        boundedSearch(instance, depth + 1, seated + option.value, taken, search);
        setTaken(taken, option, false);
    }
}

struct PropertyBoundInstance
{
    Instance base;
    std::vector<Coordinator::PropertyStockColumn> columns;
    std::vector<MilliFunds> ourWeights;
    std::vector<MilliFunds> enemyWeights;
    MilliFunds ownedBaseline{0};
};

MilliFunds matchingFrom(const std::vector<std::int32_t> & rows,
                        const std::vector<std::int32_t> & columns,
                        std::span<const MilliFunds> weights, std::size_t rowCursor,
                        std::vector<bool> & taken)
{
    if (rowCursor == rows.size())
    {
        return 0;
    }
    MilliFunds best = matchingFrom(rows, columns, weights, rowCursor + 1, taken);
    for (std::size_t slot = 0; slot < columns.size(); ++slot)
    {
        if (taken[slot])
        {
            continue;
        }
        taken[slot] = true;
        const std::size_t offset =
            static_cast<std::size_t>(rows[rowCursor] * RESOURCE_COUNT + columns[slot]);
        best = std::max(best, weights[offset] +
                                 matchingFrom(rows, columns, weights, rowCursor + 1, taken));
        taken[slot] = false;
    }
    return best;
}

MilliFunds matching(const std::vector<std::int32_t> & rows,
                    const std::vector<std::int32_t> & columns,
                    std::span<const MilliFunds> weights)
{
    std::vector<bool> taken(columns.size(), false);
    return matchingFrom(rows, columns, weights, 0, taken);
}

MilliFunds propertyPhi(const PropertyBoundInstance & instance,
                       std::span<const std::int32_t> choices)
{
    std::vector<bool> captured(RESOURCE_COUNT, false);
    std::vector<std::int32_t> idleRows;
    for (std::size_t row = 0; row < choices.size(); ++row)
    {
        if (choices[row] == NO_RESOURCE)
        {
            idleRows.push_back(static_cast<std::int32_t>(row));
        }
        else
        {
            captured[static_cast<std::size_t>(choices[row])] = true;
        }
    }
    MilliFunds owned = instance.ownedBaseline;
    std::vector<std::int32_t> openColumns;
    for (std::int32_t column = 0; column < RESOURCE_COUNT; ++column)
    {
        if (captured[static_cast<std::size_t>(column)])
        {
            owned += Coordinator::ownershipFlipSwing(
                instance.columns[static_cast<std::size_t>(column)], PROPERTY_HORIZON);
        }
        else
        {
            openColumns.push_back(column);
        }
    }
    std::vector<std::int32_t> enemyRows;
    for (std::int32_t row = 0; row < ENEMY_ROW_COUNT; ++row)
    {
        enemyRows.push_back(row);
    }
    return owned + matching(idleRows, openColumns, instance.ourWeights) -
           matching(enemyRows, openColumns, instance.enemyWeights);
}

MilliFunds exactPropertyCompletion(const PropertyBoundInstance & instance, std::size_t depth,
                                   MilliFunds seated, std::vector<std::int32_t> & choices,
                                   std::vector<bool> & taken, MilliFunds atStart)
{
    if (depth == instance.base.rows.size())
    {
        return seated + propertyPhi(instance, choices) - atStart;
    }
    MilliFunds best = std::numeric_limits<MilliFunds>::min();
    for (const Option & option : instance.base.rows[depth])
    {
        if (!canTake(taken, option))
        {
            continue;
        }
        setTaken(taken, option, true);
        choices.push_back(option.resource);
        best = std::max(best, exactPropertyCompletion(
                                  instance, depth + 1, seated + option.value,
                                  choices, taken, atStart));
        choices.pop_back();
        setTaken(taken, option, false);
    }
    return best;
}

void verifyPropertyBounds(const PropertyBoundInstance & instance, std::size_t depth,
                          MilliFunds seated, std::vector<std::int32_t> & choices,
                          std::vector<bool> & taken, MilliFunds ceiling,
                          MilliFunds atStart, std::int32_t & nodes)
{
    if (depth == instance.base.rows.size())
    {
        return;
    }
    for (const Option & option : instance.base.rows[depth])
    {
        if (!canTake(taken, option))
        {
            continue;
        }
        const MilliFunds bound = assignmentUpperBound(
            seated, option.value, remainingCeiling(instance.base, depth + 1),
            ceiling, atStart);
        setTaken(taken, option, true);
        choices.push_back(option.resource);
        const MilliFunds exact = exactPropertyCompletion(
            instance, depth + 1, seated + option.value, choices, taken, atStart);
        expect(bound >= exact,
               "property ceiling bound dominates every reachable completion");
        ++nodes;
        verifyPropertyBounds(instance, depth + 1, seated + option.value,
                             choices, taken, ceiling, atStart, nodes);
        choices.pop_back();
        setTaken(taken, option, false);
    }
}

PropertyBoundInstance generatePropertyInstance(std::uint32_t & state)
{
    PropertyBoundInstance instance;
    instance.base = generateInstance(state);
    instance.ownedBaseline = nextInRange(state, -50, 50);
    for (std::int32_t column = 0; column < RESOURCE_COUNT; ++column)
    {
        const MilliFunds perTurn = nextInRange(state, -4, 12);
        instance.columns.push_back(Coordinator::PropertyStockColumn{
            .slot = column,
            .income = Coordinator::PropertyIncome{
                .oursPerTurn = perTurn,
                .enemyPerTurn = 0,
            },
            .ownerBefore = Coordinator::OwnerSign::Neutral,
        });
        const MilliFunds swing = std::max(
            MilliFunds{0}, Coordinator::ownershipFlipSwing(
                               instance.columns.back(), PROPERTY_HORIZON));
        for (std::int32_t row = 0; row < ROW_COUNT; ++row)
        {
            instance.ourWeights.push_back(nextInRange(
                state, 0, static_cast<std::int32_t>(swing)));
        }
        for (std::int32_t row = 0; row < ENEMY_ROW_COUNT; ++row)
        {
            instance.enemyWeights.push_back(nextInRange(
                state, 0, static_cast<std::int32_t>(swing)));
        }
    }
    std::vector<MilliFunds> rowMajorOur;
    std::vector<MilliFunds> rowMajorEnemy;
    rowMajorOur.reserve(instance.ourWeights.size());
    rowMajorEnemy.reserve(instance.enemyWeights.size());
    for (std::int32_t row = 0; row < ROW_COUNT; ++row)
    {
        for (std::int32_t column = 0; column < RESOURCE_COUNT; ++column)
        {
            rowMajorOur.push_back(instance.ourWeights[
                static_cast<std::size_t>(column * ROW_COUNT + row)]);
        }
    }
    for (std::int32_t row = 0; row < ENEMY_ROW_COUNT; ++row)
    {
        for (std::int32_t column = 0; column < RESOURCE_COUNT; ++column)
        {
            rowMajorEnemy.push_back(instance.enemyWeights[
                static_cast<std::size_t>(column * ENEMY_ROW_COUNT + row)]);
        }
    }
    instance.ourWeights = std::move(rowMajorOur);
    instance.enemyWeights = std::move(rowMajorEnemy);
    return instance;
}

void provePropertyCeilingBoundsEveryCompletion()
{
    std::uint32_t state = GENERATOR_SEED ^ 0xA5B20000u;
    std::int32_t nodes = 0;
    for (std::int32_t sample = 0; sample < PROPERTY_INSTANCE_COUNT; ++sample)
    {
        const PropertyBoundInstance instance = generatePropertyInstance(state);
        const MilliFunds ceiling = Coordinator::propertyStockCeiling(
            instance.columns, instance.ownedBaseline, PROPERTY_HORIZON);
        const std::vector<std::int32_t> origin(ROW_COUNT, NO_RESOURCE);
        const MilliFunds atStart = propertyPhi(instance, origin);
        std::vector<std::int32_t> choices;
        std::vector<bool> taken(RESOURCE_COUNT, false);
        verifyPropertyBounds(instance, 0, 0, choices, taken, ceiling, atStart, nodes);
    }
    expect(nodes > PROPERTY_INSTANCE_COUNT,
           "property proof visits non-root reachable states");
}

void proveGeneratedInstances()
{
    std::uint32_t state = GENERATOR_SEED;
    std::int32_t nodes = 0;
    for (std::int32_t index = 0; index < INSTANCE_COUNT; ++index)
    {
        const Instance instance = generateInstance(state);
        std::vector<bool> proofTaken(RESOURCE_COUNT, false);
        verifyEveryReachableBound(instance, 0, 0, proofTaken, nodes);

        std::vector<bool> truthTaken(RESOURCE_COUNT, false);
        const MilliFunds exact = exactBestAdditional(instance, 0, truthTaken);
        std::vector<bool> searchTaken(RESOURCE_COUNT, false);
        Search search;
        boundedSearch(instance, 0, 0, searchTaken, search);
        expect(search.hasBest, "the bounded search finds a legal completion");
        expect(search.best == exact,
               "the bounded search matches exhaustive enumeration");
    }
    expect(nodes > INSTANCE_COUNT,
           "the proof visited non-root states across the generated sweep");
}

void proveOrderingLicenseIsNecessary()
{
    const std::vector<MilliFunds> licensed{80, 60, 5};
    const std::vector<MilliFunds> unlicensed{60, 5, 80};
    expect(isPruneOrderLicensed(std::span<const MilliFunds>(licensed)),
           "descending terms license abandoning the remaining list");
    expect(!isPruneOrderLicensed(std::span<const MilliFunds>(unlicensed)),
           "a later larger term forbids abandoning the remaining list");

    MilliFunds best = unlicensed.front();
    for (std::size_t slot = 1; slot < unlicensed.size(); ++slot)
    {
        if (assignmentUpperBound(0, unlicensed[slot], 0) <= best)
        {
            break;
        }
        best = std::max(best, unlicensed[slot]);
    }
    expect(best == 60,
           "the frozen unlicensed order demonstrates the lost optimum");
    expect(*std::max_element(unlicensed.begin(), unlicensed.end()) == 80,
           "the same list has a strictly better omitted option");
}
}

int main()
{
    proveGeneratedInstances();
    provePropertyCeilingBoundsEveryCompletion();
    proveOrderingLicenseIsNecessary();
    return failures == 0 ? 0 : 1;
}
