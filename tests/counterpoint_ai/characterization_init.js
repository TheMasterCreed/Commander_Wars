var COUNTERPOINT_SCENARIO = "__COUNTERPOINT_SCENARIO__";
var CAPTURED_RULES_FILE = "captured_game_rules_v32.grl";
var COMMITTED_RULES_FILE = "game_rules_v32.grl";
var STANDARD_RULES_FILE = "game_rules_v33_standard.grl";
var COUNTERPOINT_RULES_FILE = "game_rules_v33_counterpoint.grl";
var INVALID_RULES_FILE = "game_rules_v33_invalid.grl";
var RESULT_FILE = "counterpoint-result.json";
var TEST_MAP_FOLDER = "maps/test/";
var TEST_MAP_FILE = "co_test.map";
var TEST_BUILDING = "FACTORY";
var TEST_UNIT = "INFANTRY";
var TEST_CO = "CO_ANDY";
var FIRST_PLAYER = 0;
var SECOND_PLAYER = 1;
var FACTORY_X = 2;
var FACTORY_Y = 2;
var RULES_VERSION_V32 = 32;
var RULES_VERSION_V33 = 33;
var RULES_UNIT_LIMIT_SENTINEL = 37;
var INVALID_AI_BEHAVIOR = 99;
var ORDINARY_STARTING_FUNDS = 2000;
var SEEDED_STARTING_FUNDS = 10000;
var PRODUCTION_SEED = 12648430;
var SOURCE_BUILDING_DISTANCE = 10;
var SOURCE_ENEMY_PRUNE_RANGE = 5;
var MINIMUM_MAP_SIZE = 4;
var DISCOVERY_X = 2;
var DISCOVERY_Y = 3;
var DISCOVERY_MINIMUM_MAP_HEIGHT = 5;
var DISCOVERY_STARTING_FUNDS = 20000;
var DISCOVERY_BUILDING_COST_DELTA = 137;
var DISCOVERY_CUSTOM_COST = 3210;
var DISCOVERY_BASE_OVERRIDE_COST = 4321;
var DISCOVERY_GLOBAL_SENTINEL = 4242;
var DISCOVERY_BLACK_HOLE_READY = 7;
var DISCOVERY_BLACK_HOLE_DOOR1_ONLY = 1;
var DISCOVERY_ACTION = "COUNTERPOINT_TEST_PRODUCTION_ACTION";
var DISCOVERY_UNSUPPORTED_ACTION = "COUNTERPOINT_TEST_UNSUPPORTED_ACTION";
var DISCOVERY_NON_UNIT_ACTION = "COUNTERPOINT_TEST_NON_UNIT_ACTION";
var DISCOVERY_NON_UNIT_ID = "COUNTERPOINT_TEST_NOT_A_UNIT";
var DISCOVERY_NEST = "ZNEST_FACTORY";
var DISCOVERY_BLACK_HOLE = "ZBLACKHOLE_FACTORY";
var DISCOVERY_TEMPORARY_AIRPORT = "TEMPORARY_AIRPORT";
var DISCOVERY_TEMPORARY_HARBOUR = "TEMPORARY_HARBOUR";
var DISCOVERY_DOOR1 = "ACTION_BLACKHOLEFACTORY_DOOR1";
var DISCOVERY_DOOR2 = "ACTION_BLACKHOLEFACTORY_DOOR2";
var DISCOVERY_DOOR3 = "ACTION_BLACKHOLEFACTORY_DOOR3";
var DISCOVERY_NEST_ACTION = "ACTION_NEST_FACTORY_DOOR";
var DISCOVERY_BUILD_LIST = ["INFANTRY", "RECON", "TANK", "LANDER", "T_HELI"];
var PLANNER_AIRPORT = "AIRPORT";
var PLANNER_BUILD_LIST = ["INFANTRY", "MECH", "RECON", "TANK", "T_HELI", "FIGHTER"];
var PLANNER_STARTING_FUNDS = 12000;
var PLANNER_FACTORY_X = 0;
var PLANNER_FACTORY_Y = 0;
var PLANNER_AIRPORT_X = 1;
var PLANNER_AIRPORT_Y = 0;
var PLANNER_NEST_X = 2;
var PLANNER_NEST_Y = 3;
var PLANNER_BLACK_HOLE_X = 5;
var PLANNER_BLACK_HOLE_Y = 3;
var PLANNER_MINIMUM_MAP_WIDTH = 6;
var PLANNER_MINIMUM_MAP_HEIGHT = 6;
var PLANNER_MAX_SERIALIZED_BYTES = 1000000;
var PLANNER_DUPLICATE_X = 3;
var PLANNER_DUPLICATE_Y = 4;
var PLANNER_DUPLICATE_FIRST_COST = 1000;
var PLANNER_DUPLICATE_SECOND_COST = 2000;
var PLANNER_RECYCLE_SOURCE_BUDGET = 5000;
var PLANNER_RECYCLE_SOURCE_SPEND = 1000;
var PLANNER_RECYCLE_TARGET_BUDGET = 1000;
var PLANNER_RECYCLE_TARGET_COST = 4000;
var PLANNER_BORROW_DONOR_ONE_MARGIN = 1000;
var PLANNER_BORROW_DONOR_TWO_MARGIN = 500;
var PLANNER_BORROW_REQUIRED_MARGIN = 2000;
var PLANNER_LIVE_PAID_COST = 3000;
var PLANNER_AVOID_RESERVED_BUDGET = 1000;
var PLANNER_AVOID_EXPENSIVE_COST = 3000;
var PLANNER_AVOID_AFFORDABLE_COST = 1000;
var PLANNER_AVOID_AVAILABLE_FUNDS = 5000;
var PLANNER_FLOOR_CHEAP_COST = 1000;
var PLANNER_FLOOR_PRICEY_COST = 4000;
var PLANNER_FLOOR_SHORTFALL_FUNDS = 5500;
var PLANNER_BORROW_DONOR_FLOOR = 1000;
var PLANNER_FERRY_HULL_COST = 12000;
var PLANNER_FERRY_SAVE_FUNDS = 6000;
var PLANNER_FERRY_PARTIAL_FUNDS = 11000;
var PLANNER_FERRY_RICH_FUNDS = 14000;
var PLANNER_FERRY_GOOD_INCOME = 5000;
var PLANNER_FERRY_POOR_INCOME = 1200;
var PLANNER_FERRY_MANY_CAPPERS = 20;
var PLANNER_FERRY_SAVE_TURNS = 2;
var PLANNER_COUNTER_FUNDS = 5000;
var PLANNER_COUNTER_INCOME = 5000;
var PLANNER_COUNTER_TANK_COST = 7000;
var PLANNER_COUNTER_NEOTANK_COST = 22000;
var PLANNER_COUNTER_RICH_INCOME = 12000;
var PLANNER_SKIP_BANK_FUNDS = 16000;
var PLANNER_COUNTER_ENEMY_HP = 10;
var PLANNER_COUNTER_LOW_COVERAGE = 100;
var PLANNER_COUNTER_HIGH_COVERAGE = 900;
var PLANNER_COUNTER_WEAK_DAMAGE = 5;
var PLANNER_COUNTER_MARGINAL_DAMAGE = 50;
var PLANNER_COUNTER_STRONG_DAMAGE = 55;
var PLANNER_COUNTER_ELITE_DAMAGE = 90;
var PLANNER_COUNTER_FERRY_ELITE_SCORE = 300;
var PLANNER_COUNTER_GAP_RATIO = 0.5;
var PLANNER_COUNTER_DAY = 5;
var PLANNER_COUNTER_MIN_DAY = 3;
var PLANNER_COUNTER_EARLY_DAY = 1;
var PLANNER_COUNTER_WORTH_RATIO = 1.5;
var PLANNER_FERRY_ADMIT_DRAWS = 20;
var PLANNER_FERRY_STRANDED = 10;
var PLANNER_FERRY_LATE_TURN = 100;
var PLANNER_AVOID_TOO_EXPENSIVE_COST = 6000;
var PLANNER_SECOND_SPECIAL_ACTION = "COUNTERPOINT_TEST_SECOND_PRODUCTION_ACTION";
var USE_BUILDING_ACTION_A = "COUNTERPOINT_TEST_MENU_ACTION_A";
var USE_BUILDING_ACTION_B = "COUNTERPOINT_TEST_MENU_ACTION_B";
var USE_BUILDING_FIRST_X = 2;
var USE_BUILDING_FIRST_Y = 2;
var USE_BUILDING_SECOND_X = 3;
var USE_BUILDING_SECOND_Y = 2;
var USE_BUILDING_STARTING_FUNDS = 20000;
var USE_BUILDING_ROW_ENABLED_FIRST = "ALPHA";
var USE_BUILDING_ROW_ENABLED_SECOND = "BETA";
var USE_BUILDING_ROW_DISABLED = "GAMMA";
var USE_BUILDING_ROW_COST = 0;
var USE_BUILDING_MAX_PERFORMS = 3;
var USE_BUILDING_MAX_EVENTS = 64;
var DISPATCH_HIDDEN_ENEMY_COUNT = 1;
var DISPATCH_PROFILE_INI = "normal/counterpoint.ini";
var DISPATCH_NORMAL_INI = "normal/normal.ini";
var DISPATCH_OFFENSIVE_INI = "normal/normalOffensive.ini";
var DISPATCH_SENTINEL_BUILDING = "COUNTERPOINT_TEST_SENTINEL_BUILDING";
var DISPATCH_SEAMS = ["initializeSimpleProductionSystem",
                      "prepareProduction",
                      "onNewBuildQueue",
                      "buildUnitSimpleProductionSystem",
                      "getBuildingMenuItem",
                      "onBuildingMenuItemResult"];
// Section 5.11 of the design, keyed by the ini entry names NormalAi registers.
var DISPATCH_COUNTERPOINT_PROFILE = [["MinMovementDamage", 0.25],
                                     ["NotAttackableDamage", 20],
                                     ["EnemyPruneRange", 5],
                                     ["InfluenceUnitRange", 2.5],
                                     ["OwnIndirectAttackValue", 5],
                                     ["EnemyKillBonus", 1.4],
                                     ["EnemyCounterDamageMultiplier", 7],
                                     ["SupportDamageBonus", 1.0],
                                     ["InfluenceMultiplier", 0.6]];
var DISPATCH_STANDARD_PROFILE = [["MinMovementDamage", 0.15],
                                 ["NotAttackableDamage", 30],
                                 ["EnemyPruneRange", 3],
                                 ["InfluenceUnitRange", 2.25],
                                 ["OwnIndirectAttackValue", 4],
                                 ["EnemyKillBonus", 1.2],
                                 ["EnemyCounterDamageMultiplier", 10],
                                 ["SupportDamageBonus", 0.8],
                                 ["InfluenceMultiplier", 0.8]];
var STRATEGY_STANDARD_BUILDINGS = "FACTORY,AIRPORT,AMPHIBIOUSFACTORY,HARBOUR";
var STRATEGY_STANDARD_ISLAND_SIZE = 0.025;
var STRATEGY_EXPECTED_CAPTURE_CHANCE = 30;
var STRATEGY_FLOAT_EPSILON = 0.000001;
var STRATEGY_HALF_HP = 0.5;
var STRATEGY_QINT_MAX = 2147483647;
var STRATEGY_LARGE_ROSTER_SIZE = 4096;
var STRATEGY_LARGE_ROSTER_COST_BUCKETS = 127;
var STRATEGY_HELPER_TIMEOUT_MS = 5000;
var STRATEGY_COST_STEP = 1000;
var STRATEGY_TANK_VALUE = 7000;
var STRATEGY_TANK_MOVEMENT = 6;
var STRATEGY_TANK_DAMAGE = 80;
var STRATEGY_AA_DAMAGE = 15;
var STRATEGY_CAPPER_MOVEMENT = 3;
var STRATEGY_INDIRECT_VALUE = 6000;
var STRATEGY_INDIRECT_MAX_RANGE = 3;
var STRATEGY_TRANSPORT_VALUE = 12000;
var STRATEGY_ENEMY_DAMAGE_COUNTER = 20;
var STRATEGY_ENEMY_DAMAGE_WEAK = 90;
var STRATEGY_ENEMY_DAMAGE_SECOND_WEAK = 80;
var STRATEGY_WEAK_DAMAGE = 5;
var STRATEGY_SECOND_WEAK_DAMAGE = 10;
var STRATEGY_IGNORED_TRANSACTION_COST = 999999;
var STRATEGY_HP_FULL = 10;
var STRATEGY_HP_HALF_PERCENT = 50;
var STRATEGY_OWN_DAMAGE_AIR = 40;
var STRATEGY_OWN_DAMAGE_NAVAL = 10;
var STRATEGY_TOP_CONTRIBUTION = 4;
var STRATEGY_SECOND_CONTRIBUTION = 3;
var STRATEGY_HALF_WEIGHT = 0.5;
var STRATEGY_DILUTION_TARGET_VALUE = 8000;
var STRATEGY_DILUTION_OFFENSE = 10;
var STRATEGY_DILUTION_COUNTER_HP = 2;
var STRATEGY_DILUTION_HARD_DAMAGE = 75;
var STRATEGY_DILUTION_CHAFF_HP = 8;
var STRATEGY_DILUTION_CHAFF_DAMAGE = 5;
var STRATEGY_COUNTER_ID = "COUNTERPOINT_TEST_COUNTER";
var STRATEGY_DILUTION_TARGET_ID = "COUNTERPOINT_TEST_DILUTION_TARGET";
var STRATEGY_DILUTION_COUNTER_ID = "COUNTERPOINT_TEST_DILUTION_COUNTER";
var STRATEGY_DILUTION_CHAFF_ID = "COUNTERPOINT_TEST_DILUTION_CHAFF";
var STRATEGY_WEAK_ID = "COUNTERPOINT_TEST_WEAK";
var STRATEGY_SECOND_WEAK_ID = "COUNTERPOINT_TEST_SECOND_WEAK";
var STRATEGY_ENEMY_ID = "COUNTERPOINT_TEST_ENEMY";
var STRATEGY_TANK_ID = "COUNTERPOINT_TEST_TANK";
var STRATEGY_AA_ID = "COUNTERPOINT_TEST_AA";
var STRATEGY_CAPPER_ID = "COUNTERPOINT_TEST_CAPPER";
var STRATEGY_INDIRECT_ID = "COUNTERPOINT_TEST_INDIRECT";
var STRATEGY_TRANSPORT_ID = "COUNTERPOINT_TEST_TRANSPORT";
var STRATEGY_AIR_ID = "COUNTERPOINT_TEST_AIR";
var STRATEGY_COLLISION_CONSTRUCTOR_ID = "constructor";
var STRATEGY_COLLISION_PROTO_ID = "__proto__";
var STRATEGY_SHARED_UNIT_ID = "COUNTERPOINT_TEST_SHARED_UNIT";
var STRATEGY_EXPLICIT_COMPOSITION_KEY = "COUNTERPOINT_TEST_EXPLICIT_COMPOSITION";
var STRATEGY_LARGE_ID_PREFIX = "COUNTERPOINT_TEST_MOD_UNIT_";
var STRATEGY_FORBIDDEN_FIELDS = [
    "_aaCacheKey",
    "_aaFieldProbes",
    "_aaSpecialistCache",
    "_baseSkipPlans",
    "_budgetQueue",
    "_buildingQueue",
    "_builtThisTurn",
    "_coContext",
    "_enemyComp",
    "_lastTurn",
    "_mapContext",
    "_mapContextTurn",
    "_ownComp",
    "_ownCoverage",
    "_rejections",
    "_scoreCache",
    "_sessionSig",
    "_tankFerryStatsTurn",
    "_threatProfile",
    "_turnStrikes",
    "_turnUnbuildable",
    "_borrowFromQueue",
    "_getTransportContext",
    "_refreshAAProbes",
    "_refreshCOContext",
    "_returnBudgetToQueue",
    "_runWeightedPickLoop",
    "_tryForcedBuild",
    "_updateMapContext",
    "_updateTankFerryStats"
];

var COUNTERPOINT_TEST_PRODUCTION_ACTION =
{
    canBePerformed : function(action, map)
    {
        return true;
    },

    getProductionMenuData : function(action, data, map)
    {
        data.addData("Infantry", TEST_UNIT, TEST_UNIT, DISCOVERY_CUSTOM_COST, true);
        data.addData("Not a unit", DISCOVERY_NON_UNIT_ID, DISCOVERY_NON_UNIT_ID, 0, true);
    }
};

var COUNTERPOINT_TEST_UNSUPPORTED_ACTION =
{
    canBePerformed : function(action, map)
    {
        return true;
    },

    getStepData : function(action, data, map)
    {
        data.addData("Infantry", TEST_UNIT, TEST_UNIT, DISCOVERY_CUSTOM_COST, true);
    }
};

var COUNTERPOINT_TEST_NON_UNIT_ACTION =
{
    canBePerformed : function(action, map)
    {
        return true;
    },

    getProductionMenuData : function(action, data, map)
    {
        data.addData("Not a unit", DISCOVERY_NON_UNIT_ID, DISCOVERY_NON_UNIT_ID, 0, true);
    }
};

// Records useBuilding decisions because release builds omit AI debug logging.
var UseBuildingRecorder =
{
    events : [],
    performCounts : {},
    menuCalls : 0,
    finished : false,

    reset : function()
    {
        UseBuildingRecorder.events = [];
        UseBuildingRecorder.performCounts = {};
        UseBuildingRecorder.menuCalls = 0;
        UseBuildingRecorder.finished = false;
    },

    record : function(event)
    {
        if (UseBuildingRecorder.finished)
        {
            return;
        }
        UseBuildingRecorder.events.push(event);
        GameConsole.print("COUNTERPOINT_USEBUILDING:" + JSON.stringify(event), 1);
        if (UseBuildingRecorder.events.length >= USE_BUILDING_MAX_EVENTS)
        {
            UseBuildingRecorder.complete("event-cap");
        }
    },

    performCount : function(actionId)
    {
        var count = UseBuildingRecorder.performCounts[actionId];
        return count === undefined ? 0 : count;
    },

    notePerform : function(actionId)
    {
        UseBuildingRecorder.performCounts[actionId] =
            UseBuildingRecorder.performCount(actionId) + 1;
    },

    // Retire actions after bounded performs so the turn reaches ordinary production.
    exhausted : function()
    {
        return UseBuildingRecorder.performCount(USE_BUILDING_ACTION_A) >= USE_BUILDING_MAX_PERFORMS &&
            UseBuildingRecorder.performCount(USE_BUILDING_ACTION_B) >= USE_BUILDING_MAX_PERFORMS;
    },

    complete : function(reason)
    {
        if (UseBuildingRecorder.finished)
        {
            return;
        }
        UseBuildingRecorder.finished = true;
        GameConsole.print("COUNTERPOINT_USEBUILDING_END:" + reason, 1);
        counterpointTest.finish(0);
    }
};

var COUNTERPOINT_TEST_MENU_ACTION_A =
{
    canBePerformed : function(action, map)
    {
        return UseBuildingRecorder.performCount(USE_BUILDING_ACTION_A) < USE_BUILDING_MAX_PERFORMS;
    },

    isFinalStep : function(action, map)
    {
        return action.getInputStep() !== 0;
    },

    getStepInputType : function(action, map)
    {
        return action.getInputStep() === 0 ? "MENU" : "";
    },

    getStepData : function(action, data, map)
    {
        data.addData("Alpha", USE_BUILDING_ROW_ENABLED_FIRST, USE_BUILDING_ROW_ENABLED_FIRST, USE_BUILDING_ROW_COST, true);
        data.addData("Beta", USE_BUILDING_ROW_ENABLED_SECOND, USE_BUILDING_ROW_ENABLED_SECOND, USE_BUILDING_ROW_COST, true);
        data.addData("Gamma", USE_BUILDING_ROW_DISABLED, USE_BUILDING_ROW_DISABLED, USE_BUILDING_ROW_COST, false);
    },

    perform : function(action, map)
    {
        UseBuildingRecorder.notePerform(USE_BUILDING_ACTION_A);
        var target = action.getTarget();
        UseBuildingRecorder.record({
            kind : "perform",
            actionId : USE_BUILDING_ACTION_A,
            x : target.x,
            y : target.y
        });
    }
};

var COUNTERPOINT_TEST_MENU_ACTION_B =
{
    canBePerformed : function(action, map)
    {
        return UseBuildingRecorder.performCount(USE_BUILDING_ACTION_B) < USE_BUILDING_MAX_PERFORMS;
    },

    isFinalStep : function(action, map)
    {
        return action.getInputStep() !== 0;
    },

    getStepInputType : function(action, map)
    {
        return action.getInputStep() === 0 ? "MENU" : "";
    },

    getStepData : function(action, data, map)
    {
        data.addData("Alpha", USE_BUILDING_ROW_ENABLED_FIRST, USE_BUILDING_ROW_ENABLED_FIRST, USE_BUILDING_ROW_COST, true);
        data.addData("Gamma", USE_BUILDING_ROW_DISABLED, USE_BUILDING_ROW_DISABLED, USE_BUILDING_ROW_COST, false);
    },

    perform : function(action, map)
    {
        UseBuildingRecorder.notePerform(USE_BUILDING_ACTION_B);
        var target = action.getTarget();
        UseBuildingRecorder.record({
            kind : "perform",
            actionId : USE_BUILDING_ACTION_B,
            x : target.x,
            y : target.y
        });
    }
};

var CounterpointCharacterization =
{
    finishFailure : function(reason)
    {
        GameConsole.print("COUNTERPOINT_REASON:" + reason, 1);
        counterpointTest.finish(4);
        throw new Error("COUNTERPOINT_REASON:" + reason);
    },

    _productionArmed : false,

    armSeededProduction : function()
    {
        if (CounterpointCharacterization._productionArmed)
        {
            return;
        }
        CounterpointCharacterization._productionArmed = true;
        var resetInitialProduction = COUNTERPOINT_SCENARIO === "seeded_selection";
        if (typeof game === "undefined" ||
            !counterpointTest.enableSeededProduction(
                game,
                FIRST_PLAYER,
                PRODUCTION_SEED,
                resetInitialProduction,
                RESULT_FILE))
        {
            CounterpointCharacterization.finishFailure("production-hook");
        }
    },

    // Hook initialization inside startGame before ACTION_NEXT_PLAYER.
    installProductionArmHook : function()
    {
        CounterpointCharacterization._productionArmed = false;
        var originalInitialize = NORMALAI.initializeSimpleProductionSystem;
        NORMALAI.initializeSimpleProductionSystem = function(system, ai, map)
        {
            var result = originalInitialize(system, ai, map);
            CounterpointCharacterization.armSeededProduction();
            return result;
        };
    },

    assertValue : function(value, name)
    {
        if (!value)
        {
            CounterpointCharacterization.finishFailure(name);
        }
        GameConsole.print("COUNTERPOINT_ASSERT_PASS:" + name, 1);
    },

    runSafely : function(callback)
    {
        try
        {
            callback();
        }
        catch (error)
        {
            if (String(error).indexOf("COUNTERPOINT_REASON:") < 0)
            {
                CounterpointCharacterization.finishFailure(String(error));
            }
        }
    },

    almostEqual : function(left, right)
    {
        return Math.abs(left - right) <= STRATEGY_FLOAT_EPSILON;
    },

    makeStrategyDescriptor : function(id, domain, strategicValue, movement, minRange, maxRange)
    {
        return {
            id : id,
            domain : domain,
            strategicValue : strategicValue,
            movement : movement,
            minRange : minRange,
            maxRange : maxRange,
            canCapture : false,
            isTransporter : false,
            canTransportTank : false,
            maxDamageVsArmored : 0,
            damageById : {}
        };
    },

    normalizedWeightsValid : function(normalized, expectedLength)
    {
        if (normalized === null || normalized === undefined ||
            normalized.weights.length !== expectedLength ||
            normalized.total < 0 ||
            normalized.total > COUNTERPOINTAI.MAX_RANDOM_WEIGHT_TOTAL ||
            normalized.total > STRATEGY_QINT_MAX ||
            Math.floor(normalized.total) !== normalized.total)
        {
            return false;
        }
        var total = 0;
        for (var index = 0; index < normalized.weights.length; ++index)
        {
            var weight = normalized.weights[index];
            if (weight < 0 || Math.floor(weight) !== weight)
            {
                return false;
            }
            total += weight;
        }
        return total === normalized.total;
    },

    coreAiSnapshot : function()
    {
        return Object.keys(COREAI).sort().join("|") + ":" +
            COREAI.productionBuildings.join(",") + ":" +
            COREAI.minAverageIslandSize;
    },

    strategySnapshot : function()
    {
        return Object.keys(COUNTERPOINTAI).sort().join("|") + ":" +
            COUNTERPOINTAI.TOP_N_WEIGHTS.join(",") + ":" +
            COUNTERPOINTAI.TEMPERATURE;
    },

    compositionSignature : function(composition)
    {
        var rows = [];
        for (var index = 0; index < composition.length; ++index)
        {
            rows.push({
                compositionKey : composition[index].compositionKey,
                id : composition[index].id,
                count : composition[index].count,
                hpSum : composition[index].hpSum,
                strategicValue : composition[index].strategicValue
            });
        }
        rows.sort(function(left, right)
        {
            var leftKey = String(left.compositionKey);
            var rightKey = String(right.compositionKey);
            if (leftKey < rightKey)
            {
                return -1;
            }
            if (leftKey > rightKey)
            {
                return 1;
            }
            return 0;
        });
        return JSON.stringify(rows);
    },

    characterizeCounterpointStrategy : function()
    {
        CounterpointCharacterization.assertValue(
            typeof COREAI === "object" && COREAI !== null &&
                typeof COUNTERPOINTAI === "object" && COUNTERPOINTAI !== null &&
                COUNTERPOINTAI !== COREAI,
            "strategy-isolated-load"
        );
        CounterpointCharacterization.assertValue(
            COREAI.productionBuildings.join(",") === STRATEGY_STANDARD_BUILDINGS &&
                COREAI.minAverageIslandSize === STRATEGY_STANDARD_ISLAND_SIZE &&
                typeof COREAI.CAPTURE_BASE_CHANCE === "undefined" &&
                COUNTERPOINTAI.CAPTURE_BASE_CHANCE === STRATEGY_EXPECTED_CAPTURE_CHANCE,
            "strategy-coreai-baseline"
        );
        var coreBefore = CounterpointCharacterization.coreAiSnapshot();
        var strategyBefore = CounterpointCharacterization.strategySnapshot();
        var stateless = true;
        for (var fieldIndex = 0; fieldIndex < STRATEGY_FORBIDDEN_FIELDS.length; ++fieldIndex)
        {
            if (Object.prototype.hasOwnProperty.call(
                    COUNTERPOINTAI,
                    STRATEGY_FORBIDDEN_FIELDS[fieldIndex]
                ))
            {
                stateless = false;
                break;
            }
        }
        CounterpointCharacterization.assertValue(stateless, "strategy-stateless-surface");

        var tank = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_TANK_ID,
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_TANK_VALUE,
            STRATEGY_TANK_MOVEMENT,
            1,
            1
        );
        tank.maxDamageVsArmored = STRATEGY_TANK_DAMAGE;
        var antiAir = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_AA_ID,
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_TANK_VALUE,
            STRATEGY_TANK_MOVEMENT,
            1,
            1
        );
        antiAir.maxDamageVsArmored = STRATEGY_AA_DAMAGE;
        antiAir.isAASpecialist = true;
        var capper = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_CAPPER_ID,
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_COST_STEP,
            STRATEGY_CAPPER_MOVEMENT,
            1,
            1
        );
        capper.canCapture = true;
        var indirect = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_INDIRECT_ID,
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_INDIRECT_VALUE,
            COUNTERPOINTAI.TANK_MIN_MOVEMENT,
            2,
            STRATEGY_INDIRECT_MAX_RANGE
        );
        var transport = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_TRANSPORT_ID,
            COUNTERPOINTAI.DOMAIN_NAVAL,
            STRATEGY_TRANSPORT_VALUE,
            COUNTERPOINTAI.TANK_MIN_MOVEMENT,
            1,
            1
        );
        transport.isTransporter = true;
        transport.canTransportTank = true;
        var air = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_AIR_ID,
            COUNTERPOINTAI.DOMAIN_AIR,
            STRATEGY_TANK_VALUE,
            STRATEGY_TANK_MOVEMENT,
            1,
            1
        );
        var tankClass = COUNTERPOINTAI._classifyUnit(tank);
        var antiAirClass = COUNTERPOINTAI._classifyUnit(antiAir);
        var capperClass = COUNTERPOINTAI._classifyUnit(capper);
        var indirectClass = COUNTERPOINTAI._classifyUnit(indirect);
        var transportClass = COUNTERPOINTAI._classifyUnit(transport);
        var airClass = COUNTERPOINTAI._classifyUnit(air);
        CounterpointCharacterization.assertValue(
            tankClass.isTank && !tankClass.isAASpecialist &&
                antiAirClass.isAASpecialist && !antiAirClass.isTank &&
                capperClass.canCapture && !capperClass.isTank &&
                indirectClass.isIndirect && !indirectClass.isTank &&
                transportClass.isTransporter && transportClass.canTransportTank &&
                !transportClass.isTank &&
                airClass.domain === COUNTERPOINTAI.DOMAIN_AIR && !airClass.isTank,
            "strategy-classification"
        );
        var costOnlyUnit = CounterpointCharacterization.makeStrategyDescriptor(
            "COUNTERPOINT_TEST_COST_ONLY_TANK",
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_COST_STEP,
            STRATEGY_TANK_MOVEMENT,
            1,
            1
        );
        costOnlyUnit.maxDamageVsArmored = STRATEGY_TANK_DAMAGE;
        costOnlyUnit.cost = STRATEGY_IGNORED_TRANSACTION_COST;
        var highTransactionClassification = COUNTERPOINTAI._classifyUnit(costOnlyUnit);
        costOnlyUnit.cost = 1;
        var lowTransactionClassification = COUNTERPOINTAI._classifyUnit(costOnlyUnit);
        tank.cost = 1;
        var lowCostTankClassification = COUNTERPOINTAI._classifyUnit(tank);
        tank.cost = STRATEGY_IGNORED_TRANSACTION_COST;
        var highCostTankClassification = COUNTERPOINTAI._classifyUnit(tank);
        CounterpointCharacterization.assertValue(
            !highTransactionClassification.isTank &&
                !lowTransactionClassification.isTank &&
                JSON.stringify(highTransactionClassification) ===
                    JSON.stringify(lowTransactionClassification) &&
                lowCostTankClassification.isTank && highCostTankClassification.isTank &&
                JSON.stringify(lowCostTankClassification) ===
                    JSON.stringify(highCostTankClassification),
            "strategy-classification-cost-isolation"
        );

        var sampleUnits = [
            { id : STRATEGY_COLLISION_CONSTRUCTOR_ID, hp : STRATEGY_HP_FULL },
            { id : STRATEGY_COLLISION_PROTO_ID, hp : STRATEGY_HP_HALF_PERCENT },
            { id : STRATEGY_COLLISION_CONSTRUCTOR_ID, hp : STRATEGY_HP_FULL / 2 }
        ];
        var sampleBefore = JSON.stringify(sampleUnits);
        var sampled = COUNTERPOINTAI._sampleComposition(sampleUnits);
        CounterpointCharacterization.assertValue(
            sampled.length === 2 &&
                sampled[0].id === STRATEGY_COLLISION_CONSTRUCTOR_ID &&
                sampled[0].count === 2 &&
                CounterpointCharacterization.almostEqual(
                    sampled[0].hpSum,
                    1 + STRATEGY_HALF_HP
                ) &&
                sampled[1].id === STRATEGY_COLLISION_PROTO_ID &&
                sampled[1].count === 1 &&
                CounterpointCharacterization.almostEqual(
                    sampled[1].hpSum,
                    STRATEGY_HALF_HP
                ) &&
                JSON.stringify(sampleUnits) === sampleBefore,
            "strategy-composition"
        );
        var firstOwnerUnit = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_SHARED_UNIT_ID,
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_COST_STEP,
            STRATEGY_CAPPER_MOVEMENT,
            1,
            1
        );
        firstOwnerUnit.ownerId = 0;
        firstOwnerUnit.hp = STRATEGY_HP_FULL;
        var firstOwnerDamagedUnit = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_SHARED_UNIT_ID,
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_COST_STEP,
            STRATEGY_CAPPER_MOVEMENT,
            1,
            1
        );
        firstOwnerDamagedUnit.ownerId = 0;
        firstOwnerDamagedUnit.hp = STRATEGY_HP_FULL / 2;
        var secondOwnerUnit = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_SHARED_UNIT_ID,
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_COST_STEP,
            STRATEGY_CAPPER_MOVEMENT,
            1,
            1
        );
        secondOwnerUnit.ownerId = 1;
        secondOwnerUnit.hp = STRATEGY_HP_FULL;
        var multiOwnerComposition = COUNTERPOINTAI._sampleComposition([
            firstOwnerUnit,
            secondOwnerUnit,
            firstOwnerDamagedUnit
        ]);
        var reorderedMultiOwnerComposition = COUNTERPOINTAI._sampleComposition([
            secondOwnerUnit,
            firstOwnerDamagedUnit,
            firstOwnerUnit
        ]);
        var firstOwnerEntry = null;
        var secondOwnerEntry = null;
        for (var ownerIndex = 0; ownerIndex < multiOwnerComposition.length; ++ownerIndex)
        {
            if (multiOwnerComposition[ownerIndex].compositionKey ===
                firstOwnerUnit.ownerId + ":" + STRATEGY_SHARED_UNIT_ID)
            {
                firstOwnerEntry = multiOwnerComposition[ownerIndex];
            }
            else if (multiOwnerComposition[ownerIndex].compositionKey ===
                     secondOwnerUnit.ownerId + ":" + STRATEGY_SHARED_UNIT_ID)
            {
                secondOwnerEntry = multiOwnerComposition[ownerIndex];
            }
        }
        var fallbackLowValue = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_SHARED_UNIT_ID,
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_COST_STEP,
            STRATEGY_CAPPER_MOVEMENT,
            1,
            1
        );
        var fallbackHighValue = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_SHARED_UNIT_ID,
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_TANK_VALUE,
            STRATEGY_TANK_MOVEMENT,
            1,
            1
        );
        var fallbackComposition = COUNTERPOINTAI._sampleComposition([
            fallbackLowValue,
            fallbackHighValue
        ]);
        var explicitFirst = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_SHARED_UNIT_ID,
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_COST_STEP,
            STRATEGY_CAPPER_MOVEMENT,
            1,
            1
        );
        explicitFirst.ownerId = 0;
        explicitFirst.compositionKey = STRATEGY_EXPLICIT_COMPOSITION_KEY;
        explicitFirst.hp = STRATEGY_HP_FULL;
        var explicitSecond = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_SHARED_UNIT_ID,
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_COST_STEP,
            STRATEGY_CAPPER_MOVEMENT,
            1,
            1
        );
        explicitSecond.ownerId = 1;
        explicitSecond.compositionKey = STRATEGY_EXPLICIT_COMPOSITION_KEY;
        explicitSecond.hp = STRATEGY_HP_FULL;
        var explicitComposition = COUNTERPOINTAI._sampleComposition([
            explicitFirst,
            explicitSecond
        ]);
        CounterpointCharacterization.assertValue(
            multiOwnerComposition.length === 2 &&
                firstOwnerEntry !== null && firstOwnerEntry.count === 2 &&
                CounterpointCharacterization.almostEqual(
                    firstOwnerEntry.hpSum,
                    1 + STRATEGY_HALF_HP
                ) &&
                secondOwnerEntry !== null && secondOwnerEntry.count === 1 &&
                CounterpointCharacterization.almostEqual(secondOwnerEntry.hpSum, 1) &&
                CounterpointCharacterization.compositionSignature(multiOwnerComposition) ===
                    CounterpointCharacterization.compositionSignature(
                        reorderedMultiOwnerComposition
                    ) &&
                fallbackComposition.length === 2 &&
                fallbackComposition[0].compositionKey !==
                    fallbackComposition[1].compositionKey &&
                explicitComposition.length === 1 &&
                explicitComposition[0].compositionKey ===
                    STRATEGY_EXPLICIT_COMPOSITION_KEY &&
                explicitComposition[0].count === 2,
            "strategy-composition-grouping"
        );

        var enemyAir = CounterpointCharacterization.makeStrategyDescriptor(
            "COUNTERPOINT_TEST_ENEMY_AIR",
            COUNTERPOINTAI.DOMAIN_AIR,
            STRATEGY_TANK_VALUE,
            STRATEGY_TANK_MOVEMENT,
            1,
            1
        );
        enemyAir.hpSum = 2;
        var enemyNaval = CounterpointCharacterization.makeStrategyDescriptor(
            "COUNTERPOINT_TEST_ENEMY_NAVAL",
            COUNTERPOINTAI.DOMAIN_NAVAL,
            STRATEGY_TRANSPORT_VALUE,
            COUNTERPOINTAI.TANK_MIN_MOVEMENT,
            2,
            STRATEGY_INDIRECT_MAX_RANGE
        );
        enemyNaval.hpSum = 1;
        var analyzedEnemies = [enemyAir, enemyNaval];
        var profile = COUNTERPOINTAI._analyzeEnemyComp(analyzedEnemies);
        var nullSafeProfile = COUNTERPOINTAI._analyzeEnemyComp([
            null,
            enemyAir,
            undefined,
            enemyNaval
        ]);
        var nullSafeSample = COUNTERPOINTAI._sampleComposition([
            null,
            sampleUnits[0],
            undefined
        ]);
        var profileTotal = enemyAir.hpSum + enemyNaval.hpSum;
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.almostEqual(profile.total, profileTotal) &&
                CounterpointCharacterization.almostEqual(
                    profile.airShare,
                    enemyAir.hpSum / profileTotal
                ) &&
                CounterpointCharacterization.almostEqual(
                    profile.navalShare,
                    enemyNaval.hpSum / profileTotal
                ) &&
                CounterpointCharacterization.almostEqual(
                    profile.indirectShare,
                    enemyNaval.hpSum / profileTotal
                ),
            "strategy-threat-analysis"
        );

        var ownCounter = CounterpointCharacterization.makeStrategyDescriptor(
            "COUNTERPOINT_TEST_OWN_COUNTER",
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_TANK_VALUE,
            STRATEGY_TANK_MOVEMENT,
            1,
            1
        );
        ownCounter.hpSum = 2;
        ownCounter.damageById[enemyAir.id] = STRATEGY_OWN_DAMAGE_AIR;
        ownCounter.damageById[enemyNaval.id] = STRATEGY_OWN_DAMAGE_NAVAL;
        var coverage = COUNTERPOINTAI._computeOwnCoverage([ownCounter], analyzedEnemies);
        var qualityDamage = COUNTERPOINTAI.COVERAGE_QUALITY_DAMAGE;
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.almostEqual(
                coverage[enemyAir.id],
                ownCounter.hpSum * COUNTERPOINTAI._coverageDamage(
                    STRATEGY_OWN_DAMAGE_AIR,
                    qualityDamage
                )
            ) &&
                CounterpointCharacterization.almostEqual(
                    coverage[enemyNaval.id],
                    ownCounter.hpSum * COUNTERPOINTAI._coverageDamage(
                        STRATEGY_OWN_DAMAGE_NAVAL,
                        qualityDamage
                    )
                ),
            "strategy-coverage"
        );

        var counter = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_COUNTER_ID,
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_TANK_VALUE,
            STRATEGY_TANK_MOVEMENT,
            1,
            1
        );
        counter.maxDamageVsArmored = STRATEGY_TANK_DAMAGE;
        counter.damageById[STRATEGY_ENEMY_ID] = STRATEGY_TANK_DAMAGE;
        var weak = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_WEAK_ID,
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_COST_STEP,
            COUNTERPOINTAI.TANK_MIN_MOVEMENT,
            1,
            1
        );
        weak.damageById[STRATEGY_ENEMY_ID] = STRATEGY_WEAK_DAMAGE;
        var secondWeak = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_SECOND_WEAK_ID,
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_COST_STEP,
            COUNTERPOINTAI.TANK_MIN_MOVEMENT,
            1,
            1
        );
        secondWeak.damageById[STRATEGY_ENEMY_ID] = STRATEGY_SECOND_WEAK_DAMAGE;
        var enemy = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_ENEMY_ID,
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_TANK_VALUE,
            STRATEGY_TANK_MOVEMENT,
            1,
            1
        );
        enemy.hpSum = 1;
        enemy.maxDamageVsArmored = STRATEGY_TANK_DAMAGE;
        enemy.damageById[STRATEGY_COUNTER_ID] = STRATEGY_ENEMY_DAMAGE_COUNTER;
        enemy.damageById[STRATEGY_WEAK_ID] = STRATEGY_ENEMY_DAMAGE_WEAK;
        enemy.damageById[STRATEGY_SECOND_WEAK_ID] = STRATEGY_ENEMY_DAMAGE_SECOND_WEAK;
        var scoringContext = {
            islandMode : false,
            ownCounts : {},
            ownCoverage : {},
            indirectRangeDeltas : {},
            threatProfile : COUNTERPOINTAI._analyzeEnemyComp([enemy]),
            tankFerryStats : { tanks : 0, capacity : 0 }
        };
        scoringContext.ownCoverage[STRATEGY_ENEMY_ID] = 0;
        var contextBefore = JSON.stringify(scoringContext);
        counter.cost = 1;
        var counterScore = COUNTERPOINTAI._scoreUnitAgainstEnemies(
            counter,
            [enemy],
            scoringContext
        );
        counter.cost = STRATEGY_IGNORED_TRANSACTION_COST;
        var repeatedCounterScore = COUNTERPOINTAI._scoreUnitAgainstEnemies(
            counter,
            [enemy],
            scoringContext
        );
        var nullSafeCounterScore = COUNTERPOINTAI._scoreUnitAgainstEnemies(
            counter,
            [null, enemy, undefined],
            scoringContext
        );
        var weakScore = COUNTERPOINTAI._scoreUnitAgainstEnemies(
            weak,
            [enemy],
            scoringContext
        );
        var secondWeakScore = COUNTERPOINTAI._scoreUnitAgainstEnemies(
            secondWeak,
            [enemy],
            scoringContext
        );
        var contributions = [
            1,
            STRATEGY_TOP_CONTRIBUTION,
            2,
            STRATEGY_SECOND_CONTRIBUTION
        ];
        var contributionsBefore = JSON.stringify(contributions);
        var topScore = COUNTERPOINTAI._sumTopN(
            contributions,
            [1, STRATEGY_HALF_WEIGHT]
        );
        CounterpointCharacterization.assertValue(
            isFinite(counterScore) && counterScore > 0 &&
                weakScore < 0 && secondWeakScore < 0 &&
                secondWeakScore > weakScore &&
                CounterpointCharacterization.almostEqual(
                    counterScore,
                    repeatedCounterScore
                ) &&
                CounterpointCharacterization.almostEqual(
                    topScore,
                    STRATEGY_TOP_CONTRIBUTION +
                        STRATEGY_SECOND_CONTRIBUTION * STRATEGY_HALF_WEIGHT
                ) &&
                JSON.stringify(contributions) === contributionsBefore &&
                JSON.stringify(scoringContext) === contextBefore,
            "strategy-scoring"
        );
        var changedOwnState = JSON.parse(JSON.stringify(scoringContext));
        changedOwnState.ownCounts[STRATEGY_COUNTER_ID] = 99;
        changedOwnState.ownCoverage[STRATEGY_ENEMY_ID] = 99999;
        CounterpointCharacterization.assertValue(
            JSON.stringify(COUNTERPOINTAI._evaluateStaticCombat(
                counter,
                [enemy],
                scoringContext
            )) ===
                JSON.stringify(COUNTERPOINTAI._evaluateStaticCombat(
                    counter,
                    [enemy],
                    changedOwnState
                )),
            "strategy-static-score-own-state-independent"
        );
        CounterpointCharacterization.assertValue(
            JSON.stringify(nullSafeProfile) === JSON.stringify(profile) &&
                nullSafeSample.length === 1 &&
                nullSafeSample[0].id === sampleUnits[0].id &&
                CounterpointCharacterization.almostEqual(
                    nullSafeCounterScore,
                    counterScore
                ),
            "strategy-null-enemies"
        );

        // Harmless chaff on the board must not make a hard countered candidate look safer.
        var dilutionTarget = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_DILUTION_TARGET_ID,
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_DILUTION_TARGET_VALUE,
            STRATEGY_TANK_MOVEMENT,
            1,
            1
        );
        var dilutionCounter = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_DILUTION_COUNTER_ID,
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_TANK_VALUE,
            STRATEGY_TANK_MOVEMENT,
            1,
            1
        );
        dilutionCounter.hpSum = STRATEGY_DILUTION_COUNTER_HP;
        dilutionCounter.maxDamageVsArmored = STRATEGY_TANK_DAMAGE;
        dilutionCounter.damageById[STRATEGY_DILUTION_TARGET_ID] =
            STRATEGY_DILUTION_HARD_DAMAGE;
        var dilutionChaff = CounterpointCharacterization.makeStrategyDescriptor(
            STRATEGY_DILUTION_CHAFF_ID,
            COUNTERPOINTAI.DOMAIN_GROUND,
            STRATEGY_COST_STEP,
            STRATEGY_CAPPER_MOVEMENT,
            1,
            1
        );
        dilutionChaff.canCapture = true;
        dilutionChaff.hpSum = STRATEGY_DILUTION_CHAFF_HP;
        dilutionChaff.damageById[STRATEGY_DILUTION_TARGET_ID] =
            STRATEGY_DILUTION_CHAFF_DAMAGE;
        dilutionTarget.damageById[STRATEGY_DILUTION_COUNTER_ID] =
            STRATEGY_DILUTION_OFFENSE;
        var dilutionAloneContext = {
            islandMode : false,
            ownCounts : {},
            ownCoverage : {},
            indirectRangeDeltas : {},
            threatProfile : COUNTERPOINTAI._analyzeEnemyComp([dilutionCounter]),
            tankFerryStats : { tanks : 0, capacity : 0 }
        };
        var dilutionChaffContext = {
            islandMode : false,
            ownCounts : {},
            ownCoverage : {},
            indirectRangeDeltas : {},
            threatProfile : COUNTERPOINTAI._analyzeEnemyComp(
                [dilutionCounter, dilutionChaff]
            ),
            tankFerryStats : { tanks : 0, capacity : 0 }
        };
        var dilutionAloneScore = COUNTERPOINTAI._scoreUnitAgainstEnemies(
            dilutionTarget,
            [dilutionCounter],
            dilutionAloneContext
        );
        var dilutionChaffScore = COUNTERPOINTAI._scoreUnitAgainstEnemies(
            dilutionTarget,
            [dilutionCounter, dilutionChaff],
            dilutionChaffContext
        );
        CounterpointCharacterization.assertValue(
            isFinite(dilutionAloneScore) && isFinite(dilutionChaffScore) &&
                dilutionChaffScore <= dilutionAloneScore,
            "strategy-adjacency-dilution"
        );

        var overflowRaw = [Number.MAX_VALUE, Number.MAX_VALUE / 2, 1, 0];
        var overflowNormalized = COUNTERPOINTAI._normalizeWeights(overflowRaw);
        var invalidNormalized = COUNTERPOINTAI._normalizeWeights([NaN, -1]);
        var fractionalNormalized = COUNTERPOINTAI._normalizeWeights([
            COUNTERPOINTAI.CHEAPEST_CAPPER_BIAS_TIGHT,
            1 - COUNTERPOINTAI.CHEAPEST_CAPPER_BIAS_TIGHT
        ]);
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.normalizedWeightsValid(
                overflowNormalized,
                overflowRaw.length
            ) &&
                overflowNormalized.weights[0] > overflowNormalized.weights[1] &&
                overflowNormalized.weights[1] > overflowNormalized.weights[2] &&
                overflowNormalized.weights[2] > 0 &&
                overflowNormalized.weights[overflowNormalized.weights.length - 1] === 0 &&
                CounterpointCharacterization.normalizedWeightsValid(invalidNormalized, 2) &&
                invalidNormalized.weights[0] > invalidNormalized.weights[1] &&
                CounterpointCharacterization.normalizedWeightsValid(
                    fractionalNormalized,
                    2
                ) &&
                fractionalNormalized.weights[0] > fractionalNormalized.weights[1] &&
                fractionalNormalized.weights[1] > 0,
            "strategy-normalization"
        );

        var costCandidates = [capper, tank, transport];
        var costCandidatesBefore = JSON.stringify(costCandidates);
        var costWeights = COUNTERPOINTAI._costWeights(costCandidates);
        var noEnemyWeights = COUNTERPOINTAI._netDamageWeights(
            costCandidates,
            [],
            scoringContext
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.normalizedWeightsValid(
                costWeights,
                costCandidates.length
            ) &&
                costWeights.weights[0] < costWeights.weights[1] &&
                costWeights.weights[1] < costWeights.weights[2] &&
                JSON.stringify(costWeights) === JSON.stringify(noEnemyWeights) &&
                JSON.stringify(costCandidates) === costCandidatesBefore,
            "strategy-no-enemy-fallback"
        );

        var inverseWeights = COUNTERPOINTAI._inverseCostWeights(costCandidates, 1);
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.normalizedWeightsValid(
                inverseWeights,
                costCandidates.length
            ) &&
                inverseWeights.weights[0] > inverseWeights.weights[1] &&
                inverseWeights.weights[1] > inverseWeights.weights[2] &&
                inverseWeights.weights[2] > 0,
            "strategy-inverse-cost"
        );

        var negativeCandidates = [weak, secondWeak];
        var negativeWeights = COUNTERPOINTAI._netDamageWeights(
            negativeCandidates,
            [enemy],
            scoringContext
        );
        var repeatedNegativeWeights = COUNTERPOINTAI._netDamageWeights(
            negativeCandidates,
            [enemy],
            scoringContext
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.normalizedWeightsValid(
                negativeWeights,
                negativeCandidates.length
            ) &&
                negativeWeights.weights[1] > negativeWeights.weights[0] &&
                JSON.stringify(negativeWeights) === JSON.stringify(repeatedNegativeWeights),
            "strategy-negative-fallback"
        );

        // Negative candidates retain rank while the positive candidate dominates the pool.
        var mixedCandidates = [weak, counter, secondWeak];
        var mixedWeights = COUNTERPOINTAI._netDamageWeights(
            mixedCandidates,
            [enemy],
            scoringContext
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.normalizedWeightsValid(
                mixedWeights,
                mixedCandidates.length
            ) &&
                mixedWeights.weights[1] > mixedWeights.weights[2] &&
                mixedWeights.weights[2] > mixedWeights.weights[0],
            "strategy-mixed-pool-ordering"
        );

        var lastPositive = -1;
        for (var weightIndex = 0;
             weightIndex < overflowNormalized.weights.length;
             ++weightIndex)
        {
            if (overflowNormalized.weights[weightIndex] > 0)
            {
                lastPositive = weightIndex;
            }
        }
        var firstPick = COUNTERPOINTAI._pickWeightedIndex(overflowNormalized, 0);
        var lastPick = COUNTERPOINTAI._pickWeightedIndex(
            overflowNormalized,
            overflowNormalized.total - 1
        );
        CounterpointCharacterization.assertValue(
            firstPick === 0 && lastPick === lastPositive &&
                firstPick === COUNTERPOINTAI._pickWeightedIndex(overflowNormalized, 0) &&
                lastPick === COUNTERPOINTAI._pickWeightedIndex(
                    overflowNormalized,
                    overflowNormalized.total - 1
                ) &&
                COUNTERPOINTAI._pickWeightedIndex({ weights : [], total : 0 }, 0) === -1,
            "strategy-weighted-pick"
        );

        var largeRoster = [];
        for (var rosterIndex = 0; rosterIndex < STRATEGY_LARGE_ROSTER_SIZE; ++rosterIndex)
        {
            largeRoster.push(CounterpointCharacterization.makeStrategyDescriptor(
                STRATEGY_LARGE_ID_PREFIX + rosterIndex,
                COUNTERPOINTAI.DOMAIN_GROUND,
                (rosterIndex % STRATEGY_LARGE_ROSTER_COST_BUCKETS + 1) *
                    STRATEGY_COST_STEP,
                0,
                1,
                1
            ));
        }
        var largeStart = Date.now();
        var largeWeights = COUNTERPOINTAI._netDamageWeights(largeRoster, [], {});
        var largeElapsed = Date.now() - largeStart;
        var largePick = COUNTERPOINTAI._pickWeightedIndex(
            largeWeights,
            Math.floor(largeWeights.total / 2)
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.normalizedWeightsValid(
                largeWeights,
                STRATEGY_LARGE_ROSTER_SIZE
            ) &&
                largePick >= 0 && largePick < STRATEGY_LARGE_ROSTER_SIZE &&
                largeElapsed <= STRATEGY_HELPER_TIMEOUT_MS,
            "strategy-large-roster"
        );

        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.coreAiSnapshot() === coreBefore &&
                CounterpointCharacterization.strategySnapshot() === strategyBefore,
            "strategy-namespaces-unchanged"
        );
        counterpointTest.finish(0);
    },

    configureSettings : function()
    {
        settings.setOverworldAnimations(false);
        settings.setBattleAnimationMode(GameEnums.BattleAnimationMode_None);
        settings.setDialogAnimation(false);
        settings.setCaptureAnimation(false);
        settings.setMovementAnimations(false);
        settings.setDay2dayScreen(false);
    },

    enterMapSelection : function(menu)
    {
        menu.enterSingleplayer([".map"]);
    },

    selectedMap : function(menu)
    {
        menu.selectMap(TEST_MAP_FOLDER, TEST_MAP_FILE);
        menu.buttonNext();
        menu.buttonNext();
        return menu.getPlayerSelection();
    },

    clearMap : function(map)
    {
        var width = map.getMapWidth();
        var height = map.getMapHeight();
        for (var x = 0; x < width; ++x)
        {
            for (var y = 0; y < height; ++y)
            {
                var terrain = map.getTerrain(x, y);
                var unit = terrain.getUnit();
                if (unit !== null)
                {
                    unit.removeUnit(false);
                }
                terrain.removeBuilding();
                map.replaceTerrainOnly("PLAINS", x, y);
            }
        }
    },

    prepareProduction : function(menu)
    {
        var selection = CounterpointCharacterization.selectedMap(menu);
        var map = selection.getMap();
        var width = map.getMapWidth();
        var height = map.getMapHeight();
        CounterpointCharacterization.assertValue(width >= MINIMUM_MAP_SIZE, "map-width");
        CounterpointCharacterization.assertValue(height >= MINIMUM_MAP_SIZE, "map-height");
        CounterpointCharacterization.clearMap(map);

        var firstPlayer = map.getPlayer(FIRST_PLAYER);
        var secondPlayer = map.getPlayer(SECOND_PLAYER);
        firstPlayer.setTeam(FIRST_PLAYER);
        secondPlayer.setTeam(SECOND_PLAYER);

        map.replaceBuilding("HQ", 0, 0);
        map.getTerrain(0, 0).getBuilding().setOwner(firstPlayer);
        map.replaceBuilding(TEST_BUILDING, FACTORY_X, FACTORY_Y);
        map.getTerrain(FACTORY_X, FACTORY_Y).getBuilding().setOwner(firstPlayer);
        map.replaceBuilding("HQ", width - 1, height - 1);
        map.getTerrain(width - 1, height - 1).getBuilding().setOwner(secondPlayer);

        var buildList = [TEST_UNIT];
        var startingFunds = ORDINARY_STARTING_FUNDS;
        if (COUNTERPOINT_SCENARIO === "seeded_selection")
        {
            buildList = ["INFANTRY", "MECH", "RECON"];
            startingFunds = SEEDED_STARTING_FUNDS;
        }
        firstPlayer.setBuildList(buildList);
        selection.playerIncomeChanged(0, FIRST_PLAYER);
        selection.playerStartFundsChanged(startingFunds, FIRST_PLAYER);
        selection.playerCO1Changed(TEST_CO, FIRST_PLAYER);
        selection.playerCO1Changed(TEST_CO, SECOND_PLAYER);
        selection.selectPlayerAi(FIRST_PLAYER, GameEnums.AiTypes_Normal);
        selection.selectPlayerAi(SECOND_PLAYER, GameEnums.AiTypes_Human);
        CounterpointCharacterization.assertValue(firstPlayer.getUnitCount() === 0, "empty-army");
        CounterpointCharacterization.installProductionArmHook();
        menu.startGame();
    },

    // Intercepts only the two test actions, so every other building keeps stock NORMALAI routing.
    installUseBuildingHooks : function()
    {
        UseBuildingRecorder.reset();
        var originalMenuItem = NORMALAI.getBuildingMenuItem;
        NORMALAI.getBuildingMenuItem = function(ai, action, ids, costs, enabled, units, buildings, owner, map)
        {
            var actionId = action.getActionID();
            if (actionId !== USE_BUILDING_ACTION_A && actionId !== USE_BUILDING_ACTION_B)
            {
                return originalMenuItem(ai, action, ids, costs, enabled, units, buildings, owner, map);
            }
            var target = action.getTarget();
            var response = CounterpointCharacterization.useBuildingResponse(actionId);
            UseBuildingRecorder.record({
                kind : "menuItem",
                actionId : actionId,
                x : target.x,
                y : target.y,
                ids : CounterpointCharacterization.toPlainArray(ids),
                enabled : CounterpointCharacterization.toPlainArray(enabled),
                response : response
            });
            return response;
        };

        var originalMenuResult = NORMALAI.onBuildingMenuItemResult;
        NORMALAI.onBuildingMenuItemResult = function(ai, action, succeeded, x, y, actionId, map)
        {
            if (actionId !== USE_BUILDING_ACTION_A && actionId !== USE_BUILDING_ACTION_B)
            {
                if (originalMenuResult !== undefined && originalMenuResult !== null)
                {
                    return originalMenuResult(ai, action, succeeded, x, y, actionId, map);
                }
                return false;
            }
            UseBuildingRecorder.record({
                kind : "menuResult",
                actionId : actionId,
                x : x,
                y : y,
                succeeded : succeeded === true
            });
            return false;
        };
    },

    toPlainArray : function(source)
    {
        var result = [];
        if (source === null || source === undefined)
        {
            return result;
        }
        for (var i = 0; i < source.length; i++)
        {
            result.push(source[i]);
        }
        return result;
    },

    // Action A covers accept, skip, restart, and decline while action B survives scan restarts.
    useBuildingResponse : function(actionId)
    {
        if (actionId === USE_BUILDING_ACTION_B)
        {
            return GameEnums.MenuSelection_Skip;
        }
        var call = UseBuildingRecorder.menuCalls;
        UseBuildingRecorder.menuCalls = call + 1;
        if (call === 0)
        {
            return 0;
        }
        if (call === 1)
        {
            return GameEnums.MenuSelection_Skip;
        }
        if (call === 2)
        {
            return GameEnums.MenuSelection_Restart;
        }
        if (call === 3)
        {
            return false;
        }
        return 1;
    },

    prepareUseBuildingActions : function(menu)
    {
        var selection = CounterpointCharacterization.selectedMap(menu);
        var map = selection.getMap();
        var width = map.getMapWidth();
        var height = map.getMapHeight();
        CounterpointCharacterization.assertValue(width >= MINIMUM_MAP_SIZE, "map-width");
        CounterpointCharacterization.assertValue(height >= MINIMUM_MAP_SIZE, "map-height");
        CounterpointCharacterization.clearMap(map);

        var firstPlayer = map.getPlayer(FIRST_PLAYER);
        var secondPlayer = map.getPlayer(SECOND_PLAYER);
        firstPlayer.setTeam(FIRST_PLAYER);
        secondPlayer.setTeam(SECOND_PLAYER);

        map.replaceBuilding("HQ", 0, 0);
        map.getTerrain(0, 0).getBuilding().setOwner(firstPlayer);
        map.replaceBuilding(TEST_BUILDING, USE_BUILDING_FIRST_X, USE_BUILDING_FIRST_Y);
        map.getTerrain(USE_BUILDING_FIRST_X, USE_BUILDING_FIRST_Y).getBuilding().setOwner(firstPlayer);
        map.replaceBuilding(TEST_BUILDING, USE_BUILDING_SECOND_X, USE_BUILDING_SECOND_Y);
        map.getTerrain(USE_BUILDING_SECOND_X, USE_BUILDING_SECOND_Y).getBuilding().setOwner(firstPlayer);
        map.replaceBuilding("HQ", width - 1, height - 1);
        map.getTerrain(width - 1, height - 1).getBuilding().setOwner(secondPlayer);

        // Two special actions across two factories, so the bounded scan restart has somewhere to go.
        FACTORY.actionList = ["ACTION_BUILD_UNITS", USE_BUILDING_ACTION_A, USE_BUILDING_ACTION_B];
        // Register custom actions because GameAction rejects actions absent from the rules.
        var rules = map.getGameRules();
        var allowed = CounterpointCharacterization.toPlainArray(rules.getAllowedActions());
        allowed.push(USE_BUILDING_ACTION_A);
        allowed.push(USE_BUILDING_ACTION_B);
        rules.setAllowedActions(allowed);

        firstPlayer.setBuildList([TEST_UNIT]);
        selection.playerIncomeChanged(0, FIRST_PLAYER);
        selection.playerStartFundsChanged(USE_BUILDING_STARTING_FUNDS, FIRST_PLAYER);
        selection.playerCO1Changed(TEST_CO, FIRST_PLAYER);
        selection.playerCO1Changed(TEST_CO, SECOND_PLAYER);
        selection.selectPlayerAi(FIRST_PLAYER, GameEnums.AiTypes_Normal);
        selection.selectPlayerAi(SECOND_PLAYER, GameEnums.AiTypes_Human);
        CounterpointCharacterization.assertValue(firstPlayer.getUnitCount() === 0, "empty-army");
        CounterpointCharacterization.installUseBuildingHooks();
        UseBuildingRecorder.record({
            kind : "actionList",
            resolved : CounterpointCharacterization.toPlainArray(
                map.getTerrain(USE_BUILDING_FIRST_X, USE_BUILDING_FIRST_Y).getBuilding().getActionList())
        });
        CounterpointCharacterization.installProductionArmHook();
        menu.startGame();
    },

    captureRulesV32 : function(rules)
    {
        CounterpointCharacterization.assertValue(
            counterpointTest.getGameRulesVersion(rules) === RULES_VERSION_V32,
            "rules-version"
        );
        rules.setFogMode(GameEnums.Fog_OfShroud);
        rules.setUnitLimit(RULES_UNIT_LIMIT_SENTINEL);
        CounterpointCharacterization.assertValue(
            counterpointTest.writeGameRules(rules, CAPTURED_RULES_FILE),
            "rules-write"
        );
        CounterpointCharacterization.assertValue(
            counterpointTest.readGameRulesVersion(CAPTURED_RULES_FILE) === RULES_VERSION_V32,
            "serialized-version"
        );
    },

    characterizeRulesV32 : function(rules)
    {
        CounterpointCharacterization.assertValue(
            counterpointTest.getGameRulesVersion(rules) === RULES_VERSION_V33,
            "rules-current-version"
        );
        CounterpointCharacterization.assertValue(
            counterpointTest.readGameRulesVersion(COMMITTED_RULES_FILE) === RULES_VERSION_V32,
            "rules-v32-version"
        );
        rules.setAiBehaviorMode(GameEnums.AiBehavior_Counterpoint);
        CounterpointCharacterization.assertValue(
            counterpointTest.readGameRules(rules, COMMITTED_RULES_FILE),
            "rules-read"
        );
        CounterpointCharacterization.assertValue(
            rules.getFogMode() === GameEnums.Fog_OfShroud &&
                rules.getUnitLimit() === RULES_UNIT_LIMIT_SENTINEL,
            "rules-sentinels"
        );
        CounterpointCharacterization.assertValue(
            rules.getAiBehaviorMode() === GameEnums.AiBehavior_Standard,
            "rules-v32-standard"
        );
    },

    characterizeRulesV33 : function(map, rules)
    {
        CounterpointCharacterization.assertValue(
            counterpointTest.getGameRulesVersion(rules) === RULES_VERSION_V33,
            "rules-version"
        );
        CounterpointCharacterization.assertValue(
            rules.getAiBehaviorMode() === GameEnums.AiBehavior_Standard,
            "rules-default-standard"
        );

        rules.setAiBehaviorMode(GameEnums.AiBehavior_Standard);
        counterpointTest.gameMapHash(map);
        var standardHash = counterpointTest.gameMapHash(map);
        rules.setAiBehaviorMode(GameEnums.AiBehavior_Counterpoint);
        var counterpointHash = counterpointTest.gameMapHash(map);
        rules.setAiBehaviorMode(GameEnums.AiBehavior_Standard);
        var restoredStandardHash = counterpointTest.gameMapHash(map);
        CounterpointCharacterization.assertValue(
            standardHash.length > 0 &&
                standardHash === restoredStandardHash &&
                standardHash !== counterpointHash,
            "rules-hash-mode"
        );

        CounterpointCharacterization.assertValue(
            counterpointTest.writeGameRules(rules, STANDARD_RULES_FILE),
            "rules-standard-write"
        );
        rules.setAiBehaviorMode(GameEnums.AiBehavior_Counterpoint);
        CounterpointCharacterization.assertValue(
            counterpointTest.readGameRules(rules, STANDARD_RULES_FILE) &&
                rules.getAiBehaviorMode() === GameEnums.AiBehavior_Standard,
            "rules-standard-round-trip"
        );

        rules.setAiBehaviorMode(GameEnums.AiBehavior_Counterpoint);
        CounterpointCharacterization.assertValue(
            counterpointTest.writeGameRules(rules, COUNTERPOINT_RULES_FILE),
            "rules-counterpoint-write"
        );
        rules.setAiBehaviorMode(GameEnums.AiBehavior_Standard);
        CounterpointCharacterization.assertValue(
            counterpointTest.readGameRules(rules, COUNTERPOINT_RULES_FILE) &&
                rules.getAiBehaviorMode() === GameEnums.AiBehavior_Counterpoint,
            "rules-counterpoint-round-trip"
        );

        rules.setAiBehaviorMode(INVALID_AI_BEHAVIOR);
        CounterpointCharacterization.assertValue(
            rules.getAiBehaviorMode() === GameEnums.AiBehavior_Standard,
            "rules-invalid-setter"
        );

        rules.setAiBehaviorMode(GameEnums.AiBehavior_Counterpoint);
        CounterpointCharacterization.assertValue(
            counterpointTest.writeGameRules(rules, INVALID_RULES_FILE) &&
                counterpointTest.overwriteLastInt32(INVALID_RULES_FILE, INVALID_AI_BEHAVIOR),
            "rules-invalid-write"
        );
        rules.setAiBehaviorMode(GameEnums.AiBehavior_Counterpoint);
        CounterpointCharacterization.assertValue(
            counterpointTest.readGameRules(rules, INVALID_RULES_FILE) &&
                rules.getAiBehaviorMode() === GameEnums.AiBehavior_Standard,
            "rules-invalid-stream"
        );

        rules.setAiBehaviorMode(GameEnums.AiBehavior_Counterpoint);
        rules.reset();
        CounterpointCharacterization.assertValue(
            rules.getAiBehaviorMode() === GameEnums.AiBehavior_Standard,
            "rules-reset-standard"
        );
    },

    characterizeRules : function(menu)
    {
        var selection = CounterpointCharacterization.selectedMap(menu);
        var map = selection.getMap();
        var rules = map.getGameRules();
        if (COUNTERPOINT_SCENARIO === "capture_game_rules_v32")
        {
            CounterpointCharacterization.captureRulesV32(rules);
        }
        else if (COUNTERPOINT_SCENARIO === "game_rules_v32")
        {
            CounterpointCharacterization.characterizeRulesV32(rules);
        }
        else
        {
            CounterpointCharacterization.characterizeRulesV33(map, rules);
        }
        counterpointTest.finish(0);
    },

    replaceDiscoveryBuilding : function(map, buildingId, owner)
    {
        var terrain = map.getTerrain(DISCOVERY_X, DISCOVERY_Y);
        terrain.removeBuilding();
        map.replaceBuilding(buildingId, DISCOVERY_X, DISCOVERY_Y);
        var building = terrain.getBuilding();
        CounterpointCharacterization.assertValue(building !== null, buildingId + "-placed");
        building.setOwner(owner);
        return building;
    },

    snapshotDiscoveryData : function(data, actionId)
    {
        CounterpointCharacterization.assertValue(data !== null, actionId + "-data");
        var unitIds = data.getUnitIds();
        var transactionCosts = data.getTransactionCosts();
        var strategicValues = data.getStrategicValues();
        var enabledList = data.getEnabledList();
        var snapshot =
        {
            x : data.getX(),
            y : data.getY(),
            actionId : data.getActionId(),
            actionAvailable : data.getActionAvailable(),
            unitIds : [],
            transactionCosts : [],
            strategicValues : [],
            enabledList : []
        };
        for (var index = 0; index < unitIds.length; ++index)
        {
            snapshot.unitIds.push(unitIds[index]);
            snapshot.transactionCosts.push(transactionCosts[index]);
            snapshot.strategicValues.push(strategicValues[index]);
            snapshot.enabledList.push(enabledList[index]);
        }
        CounterpointCharacterization.assertValue(
            snapshot.x === DISCOVERY_X &&
                snapshot.y === DISCOVERY_Y &&
                snapshot.actionId === actionId,
            actionId + "-identity"
        );
        CounterpointCharacterization.assertValue(
            unitIds.length > 0 &&
                unitIds.length === transactionCosts.length &&
                transactionCosts.length === strategicValues.length &&
                strategicValues.length === enabledList.length,
            actionId + "-vectors"
        );
        return snapshot;
    },

    snapshotFreeDiscoveryData : function(data, actionId)
    {
        var snapshot = CounterpointCharacterization.snapshotDiscoveryData(data, actionId);
        for (var index = 0; index < snapshot.transactionCosts.length; ++index)
        {
            CounterpointCharacterization.assertValue(
                snapshot.transactionCosts[index] === 0 && snapshot.strategicValues[index] > 0,
                actionId + "-free-value"
            );
        }
        return snapshot;
    },

    replacePlannerBuilding : function(map, buildingId, x, y, owner)
    {
        var terrain = map.getTerrain(x, y);
        terrain.removeBuilding();
        map.replaceBuilding(buildingId, x, y);
        var building = terrain.getBuilding();
        CounterpointCharacterization.assertValue(building !== null, buildingId + "-planner-placed");
        building.setOwner(owner);
        return building;
    },

    snapshotPlannerData : function(data)
    {
        if (data === null || data === undefined)
        {
            return null;
        }
        var ids = data.getUnitIds();
        var costs = data.getTransactionCosts();
        var values = data.getStrategicValues();
        var enabled = data.getEnabledList();
        var snapshot = {
            x : data.getX(),
            y : data.getY(),
            actionId : String(data.getActionId()),
            ids : [],
            costs : [],
            values : [],
            enabled : []
        };
        for (var index = 0; index < ids.length; ++index)
        {
            snapshot.ids.push(String(ids[index]));
            snapshot.costs.push(costs[index]);
            snapshot.values.push(values[index]);
            snapshot.enabled.push(enabled[index] === true);
        }
        return snapshot;
    },

    findPlannerPlan : function(state, x, y, actionId)
    {
        var key = String(x) + "," + String(y) + ":" + String(actionId);
        for (var index = 0; index < state.plans.length; ++index)
        {
            if (state.plans[index].key === key)
            {
                return state.plans[index];
            }
        }
        return null;
    },

    plannerPlanMatchesData : function(plan, data)
    {
        if (plan === null || data === null ||
            plan.x !== data.x || plan.y !== data.y ||
            plan.actionId !== data.actionId ||
            plan.candidates.length !== data.ids.length)
        {
            return false;
        }
        var ordinals = Object.create(null);
        for (var index = 0; index < data.ids.length; ++index)
        {
            var ordinalKey = "#" + data.ids[index];
            var ordinal = ordinals[ordinalKey] || 0;
            ordinals[ordinalKey] = ordinal + 1;
            var candidate = plan.candidates[index];
            if (candidate.id !== data.ids[index] ||
                candidate.ordinal !== ordinal ||
                candidate.transactionCost !== data.costs[index] ||
                candidate.strategicValue !== data.values[index] ||
                candidate.enabled !== data.enabled[index])
            {
                return false;
            }
        }
        return true;
    },

    plannerPhasePlans : function(state, phase)
    {
        var plans = [];
        for (var index = 0; index < state.plans.length; ++index)
        {
            if (state.plans[index].phase === phase)
            {
                plans.push(state.plans[index]);
            }
        }
        return plans;
    },

    plannerPaidBudgets : function(state)
    {
        var budgets = [];
        var plans = CounterpointCharacterization.plannerPhasePlans(
            state,
            COUNTERPOINTAI.PHASE_ORDINARY
        );
        for (var index = 0; index < plans.length; ++index)
        {
            if (plans[index].hasPaid)
            {
                budgets.push(plans[index].reservedBudget);
            }
        }
        budgets.sort(function(left, right) { return left - right; });
        return budgets;
    },

    plannerPaidBudgetTotal : function(state)
    {
        var budgets = CounterpointCharacterization.plannerPaidBudgets(state);
        var total = 0;
        for (var index = 0; index < budgets.length; ++index)
        {
            total += budgets[index];
        }
        return total;
    },

    plannerSelectionsDeferred : function(state)
    {
        for (var index = 0; index < state.plans.length; ++index)
        {
            if (!state.plans[index].complete && state.plans[index].selected !== -1)
            {
                return false;
            }
        }
        return true;
    },

    plannerHasDeferredUnaffordableCandidate : function()
    {
        var configured = COUNTERPOINTAI.AVOID_BUDGET_BASE_SKIPS;
        COUNTERPOINTAI.AVOID_BUDGET_BASE_SKIPS = false;
        var candidate = CounterpointCharacterization.plannerTestCandidate(
            "deferred-unaffordable-unit",
            PLANNER_RECYCLE_TARGET_COST,
            false
        );
        candidate.domain = COUNTERPOINTAI.DOMAIN_AIR;
        var plan = CounterpointCharacterization.plannerTestPlan(
            "deferred-unaffordable",
            PLANNER_RECYCLE_TARGET_BUDGET,
            candidate
        );
        var deferred = !CounterpointCharacterization.plannerSelectPlanCandidate(
            [plan],
            0,
            PLANNER_AVOID_AVAILABLE_FUNDS
        );
        COUNTERPOINTAI.AVOID_BUDGET_BASE_SKIPS = configured;
        return deferred && !plan.complete && plan.selected === -1 &&
            plan.rejected.length === 0 &&
            plan.reservedBudget === PLANNER_RECYCLE_TARGET_BUDGET;
    },

    plannerTestCandidate : function(id, transactionCost, canCapture)
    {
        return {
            id : id,
            ordinal : 0,
            transactionCost : transactionCost,
            strategicValue : Math.max(1, transactionCost),
            enabled : true,
            domain : COUNTERPOINTAI.DOMAIN_GROUND,
            canCapture : canCapture,
            isTransporter : false,
            isAA : false,
            isIndirect : false,
            isDirectCombat : !canCapture,
            canTransportTank : false,
            indirectStacked : false,
            turnOnePriority : false,
            urgentFerry : false,
            movement : 0,
            intrinsicMobilityFactor : 1,
            baseCombatScore : canCapture ? 0 : 1,
            coverageProfile : [],
            airCoverageDelta : 0,
            deploymentTurns : 0,
            deploymentKnown : false,
            deploymentUnreachable : false,
            purchaseUtility : canCapture ? 0 : 1
        };
    },

    plannerTestPlan : function(key, reservedBudget, candidate)
    {
        return {
            key : key,
            x : 0,
            y : 0,
            actionId : COUNTERPOINTAI.ACTION_BUILD_UNITS,
            phase : COUNTERPOINTAI.PHASE_ORDINARY,
            reservedBudget : reservedBudget,
            valueReach : reservedBudget,
            hasPaid : candidate.transactionCost > 0,
            hasFree : candidate.transactionCost === 0,
            freeOnly : candidate.transactionCost === 0,
            skipped : false,
            strategicHoldEligible : false,
            strategicHold : false,
            forcedBuild : false,
            randomSurplusFunded : false,
            available : true,
            allowCapperBorrow : candidate.canCapture,
            borrowed : [],
            candidates : [candidate],
            order : [0],
            rejected : [],
            selected : -1,
            executed : false,
            complete : false
        };
    },

    // Reserve each ground factory's capturer before sharing a shortfall with the harbour.
    plannerFundsCheapestFloorsFirst : function()
    {
        var cheapOne = CounterpointCharacterization.plannerTestPlan(
            "floors-cheap-one",
            0,
            CounterpointCharacterization.plannerTestCandidate(
                "floors-infantry-one",
                PLANNER_FLOOR_CHEAP_COST,
                true
            )
        );
        var cheapTwo = CounterpointCharacterization.plannerTestPlan(
            "floors-cheap-two",
            0,
            CounterpointCharacterization.plannerTestCandidate(
                "floors-infantry-two",
                PLANNER_FLOOR_CHEAP_COST,
                true
            )
        );
        var pricey = CounterpointCharacterization.plannerTestPlan(
            "floors-pricey",
            0,
            CounterpointCharacterization.plannerTestCandidate(
                "floors-gunboat",
                PLANNER_FLOOR_PRICEY_COST,
                true
            )
        );
        var plans = [cheapOne, cheapTwo, pricey];
        var state = CounterpointCharacterization.plannerTestState(plans);
        COUNTERPOINTAI._allocatePhaseBudgets(state, plans, PLANNER_FLOOR_SHORTFALL_FUNDS);

        var funded = 0;
        var overspent = 0;
        for (var index = 0; index < plans.length; ++index)
        {
            var floor = COUNTERPOINTAI._planFloor(plans[index]);
            if (floor > 0 && plans[index].reservedBudget >= floor)
            {
                ++funded;
            }
            overspent += plans[index].reservedBudget;
        }
        // Fund each cheap capturer without exceeding available funds.
        return funded === 2 &&
            cheapOne.reservedBudget >= PLANNER_FLOOR_CHEAP_COST &&
            cheapTwo.reservedBudget >= PLANNER_FLOOR_CHEAP_COST &&
            pricey.reservedBudget < PLANNER_FLOOR_PRICEY_COST &&
            overspent <= PLANNER_FLOOR_SHORTFALL_FUNDS;
    },

    // Only ground capturers contribute to the factory floor.
    plannerFloorCountsGroundCapturersOnly : function()
    {
        var navalCapper = CounterpointCharacterization.plannerTestCandidate(
            "floors-gunboat-naval",
            PLANNER_FLOOR_PRICEY_COST,
            true
        );
        navalCapper.domain = COUNTERPOINTAI.DOMAIN_NAVAL;
        var harbour = CounterpointCharacterization.plannerTestPlan(
            "floors-harbour",
            0,
            navalCapper
        );
        return COUNTERPOINTAI._planFloor(harbour) === 0;
    },

    // Base the floor on buildability, not current affordability.
    plannerFloorIgnoresAffordabilityFlag : function()
    {
        var candidate = CounterpointCharacterization.plannerTestCandidate(
            "floors-disabled-capper",
            PLANNER_FLOOR_CHEAP_COST,
            true
        );
        candidate.enabled = false;
        var plan = CounterpointCharacterization.plannerTestPlan("floors-disabled", 0, candidate);
        return COUNTERPOINTAI._planFloor(plan) === PLANNER_FLOOR_CHEAP_COST;
    },

    // Stop ferry urgency once the wanted hull count is owned.
    plannerFerryHullsAreCapped : function()
    {
        var cappers = [];
        for (var index = 0; index < PLANNER_FERRY_MANY_CAPPERS; ++index)
        {
            cappers.push({
                canCapture : true,
                domain : COUNTERPOINTAI.DOMAIN_GROUND
            });
        }
        var ceiling = COUNTERPOINTAI.FERRY_MAX_HULLS;
        return COUNTERPOINTAI._ferryHullsWanted(cappers) === ceiling &&
            COUNTERPOINTAI._ferryHullsWanted([]) === 1;
    },

    // A cruiser transports only copters, so it cannot ferry ground capturers.
    plannerCarrierNeedsGroundCargo : function()
    {
        var groundIds = Object.create(null);
        groundIds["#carrier-infantry"] = true;
        var owned = COUNTERPOINTAI._countOwnTransporters(
            [
                { isTransporter : true, domain : COUNTERPOINTAI.DOMAIN_NAVAL,
                  cargoIds : ["carrier-copter"] },
                { isTransporter : true, domain : COUNTERPOINTAI.DOMAIN_NAVAL,
                  cargoIds : ["carrier-infantry"] }
            ],
            groundIds
        );
        // An owned cruiser must not be mistaken for a ferry already in the water.
        return owned.naval === 1 && owned.ground === 0;
    },

    plannerCapperStubBuilding : function(x, y, unitIds, costs)
    {
        return {
            getOwnerID : function() { return 0; },
            getX : function() { return x; },
            getY : function() { return y; },
            getActionList : function() { return [COUNTERPOINTAI.ACTION_BUILD_UNITS]; },
            unitIds : unitIds,
            costs : costs
        };
    },

    plannerCapperStubSystem : function()
    {
        return {
            getProductionActionData : function(building)
            {
                return {
                    getUnitIds : function() { return building.unitIds; },
                    getTransactionCosts : function() { return building.costs; }
                };
            },
            getDummyUnit : function(id)
            {
                var hulls = { "capper-lander" : ["capper-infantry"],
                              "capper-cruiser" : ["capper-copter"] };
                var naval = hulls[id] !== undefined;
                return {
                    canCapture : function() { return id === "capper-infantry"; },
                    getUnitType : function()
                    {
                        if (naval) { return GameEnums.UnitType_Naval; }
                        return id === "capper-copter" ?
                            GameEnums.UnitType_Air : GameEnums.UnitType_Ground;
                    },
                    isTransporter : function() { return naval; },
                    getMovementType : function() { return naval ? "MOVE_BOAT" : "MOVE_FOOT"; },
                    getTransportUnits : function()
                    {
                        return naval ? hulls[id] : [];
                    }
                };
            }
        };
    },

    // Derive match-wide buildability from facilities because plan batches can be partial.
    plannerRosterReadsAllFacilities : function()
    {
        var factory = CounterpointCharacterization.plannerCapperStubBuilding(
            4,
            4,
            ["capper-infantry", "capper-tank"],
            [PLANNER_FLOOR_CHEAP_COST, PLANNER_FLOOR_PRICEY_COST]
        );
        var harbour = CounterpointCharacterization.plannerCapperStubBuilding(
            2,
            2,
            ["capper-lander", "capper-cruiser"],
            [PLANNER_FERRY_HULL_COST, PLANNER_FERRY_HULL_COST]
        );
        var airport = CounterpointCharacterization.plannerCapperStubBuilding(
            9,
            9,
            ["capper-copter"],
            [PLANNER_FLOOR_PRICEY_COST]
        );
        var roster = COUNTERPOINTAI._productionRoster(
            CounterpointCharacterization.plannerCapperStubSystem(),
            { getPlayer : function() { return { getPlayerID : function() { return 0; } }; } },
            [factory, harbour, airport]
        );
        // The harbour adds hulls and a domain, but no ground-capturer floor.
        return roster.capper !== null &&
            roster.capper.id === "capper-infantry" &&
            roster.capper.cost === PLANNER_FLOOR_CHEAP_COST &&
            roster.homes.length === 1 &&
            roster.homes[0].x === 4 && roster.homes[0].y === 4 &&
            roster.floorTotal === PLANNER_FLOOR_CHEAP_COST &&
            roster.capperIds["#capper-infantry"] === true &&
            roster.capperIds["#capper-tank"] === undefined &&
            roster.groundIds["#capper-tank"] === true &&
            roster.groundIds["#capper-lander"] === undefined &&
            roster.unitIds.length === 5 &&
            roster.cheapestCost["#capper-lander"] === PLANNER_FERRY_HULL_COST &&
            roster.domains.ground === true && roster.domains.naval === true &&
            roster.domains.air === true &&
            // The cruiser is buildable but cannot ferry ground capturers.
            roster.carriers["#capper-cruiser"] === undefined &&
            // Keep unaffordable port hulls visible so ferry saving can start.
            roster.transports.length === 2 &&
            roster.transports[0].id === "capper-lander" &&
            roster.transports[0].cost === PLANNER_FERRY_HULL_COST &&
            roster.transports[0].x === 2 && roster.transports[0].y === 2 &&
            roster.transports[0].key ===
                COUNTERPOINTAI._planKey(2, 2, COUNTERPOINTAI.ACTION_BUILD_UNITS) &&
            roster.carriers["#capper-lander"] === true;
    },

    // Fleet sufficiency must override the late-turn urgency ramp.
    plannerSatisfiedFerryRefused : function()
    {
        var state = CounterpointCharacterization.plannerTestState([]);
        var ferry = {
            stranded : PLANNER_FERRY_STRANDED,
            measured : true,
            urgent : true,
            want : 2,
            cost : 0,
            deliverable : { naval : true },
            needed : { naval : false },
            plans : {}
        };
        var context = {
            islandMode : true,
            ferry : ferry,
            transportContext : { naval : true },
            ownTransporters : { naval : 0 }
        };
        for (var index = 0; index < PLANNER_FERRY_ADMIT_DRAWS; ++index)
        {
            if (COUNTERPOINTAI._admitTransports(
                    context,
                    state,
                    COUNTERPOINTAI.DOMAIN_NAVAL,
                    PLANNER_FERRY_LATE_TURN
                ) !== false)
            {
                return false;
            }
        }
        ferry.needed.naval = true;
        return COUNTERPOINTAI._admitTransports(
            context,
            state,
            COUNTERPOINTAI.DOMAIN_NAVAL,
            PLANNER_FERRY_LATE_TURN
        ) === true;
    },

    // Prioritize an urgent ferry ahead of other transport candidates.
    plannerFerryBindingPromotes : function()
    {
        var cruiser = CounterpointCharacterization.plannerTestCandidate(
            "bind-cruiser",
            PLANNER_FERRY_HULL_COST,
            false
        );
        cruiser.domain = COUNTERPOINTAI.DOMAIN_NAVAL;
        cruiser.isTransporter = true;
        var lander = CounterpointCharacterization.plannerTestCandidate(
            "bind-lander",
            PLANNER_FERRY_HULL_COST,
            false
        );
        lander.domain = COUNTERPOINTAI.DOMAIN_NAVAL;
        lander.isTransporter = true;
        var plan = CounterpointCharacterization.plannerTestPlan("bind-harbour", 0, cruiser);
        plan.candidates.push(lander);
        plan.order = [0, 1];
        var carriers = Object.create(null);
        carriers["#bind-lander"] = true;
        var ferryPlans = Object.create(null);
        ferryPlans["bind-harbour"] = true;
        var context = {
            islandMode : true,
            groundCarriers : carriers,
            ferry : {
                urgent : true,
                plans : ferryPlans,
                needed : { naval : true }
            }
        };
        COUNTERPOINTAI._ferryFirstOrder(plan, context);
        var promoted = plan.order[0] === 1 && plan.order[1] === 0;
        // With the fleet already sufficient the draw's own order must be left alone.
        plan.order = [0, 1];
        context.ferry.needed.naval = false;
        COUNTERPOINTAI._ferryFirstOrder(plan, context);
        return promoted && plan.order[0] === 0 && plan.order[1] === 1;
    },

    plannerCounterRoster : function()
    {
        var cheapest = Object.create(null);
        cheapest["#counter-infantry"] = PLANNER_FLOOR_CHEAP_COST;
        cheapest["#counter-tank"] = PLANNER_COUNTER_TANK_COST;
        cheapest["#counter-neotank"] = PLANNER_COUNTER_NEOTANK_COST;
        return {
            unitIds : ["counter-infantry", "counter-tank", "counter-neotank"],
            cheapestCost : cheapest,
            floorTotal : PLANNER_FLOOR_CHEAP_COST
        };
    },

    plannerCounterContext : function(coverage, weakDamage)
    {
        var ownCoverage = Object.create(null);
        ownCoverage["1:counter-enemy-tank"] = coverage;
        var weak = CounterpointCharacterization.plannerTestCandidate(
            "counter-infantry",
            PLANNER_FLOOR_CHEAP_COST,
            false
        );
        weak.baseCombatScore = weakDamage;
        weak.coverageProfile = [{
            threatIndex : 0,
            coverageDelta : weakDamage,
            staticWeight : 1
        }];
        var strong = CounterpointCharacterization.plannerTestCandidate(
            "counter-tank",
            PLANNER_COUNTER_TANK_COST,
            false
        );
        strong.baseCombatScore = PLANNER_COUNTER_STRONG_DAMAGE;
        strong.coverageProfile = [{
            threatIndex : 0,
            coverageDelta : PLANNER_COUNTER_STRONG_DAMAGE,
            staticWeight : 1
        }];
        var elite = CounterpointCharacterization.plannerTestCandidate(
            "counter-neotank",
            PLANNER_COUNTER_NEOTANK_COST,
            false
        );
        elite.baseCombatScore = PLANNER_COUNTER_ELITE_DAMAGE;
        elite.coverageProfile = [{
            threatIndex : 0,
            coverageDelta : PLANNER_COUNTER_ELITE_DAMAGE,
            staticWeight : 1
        }];
        var plan = CounterpointCharacterization.plannerTestPlan(
            "counter-plan",
            0,
            weak
        );
        plan.candidates.push(strong);
        plan.candidates.push(elite);
        plan.order = [0, 1, 2];
        COUNTERPOINTAI._refreshPlanCostKinds(plan);
        var need = PLANNER_COUNTER_ENEMY_HP *
            COUNTERPOINTAI.COVERAGE_DAMAGE_SCALE;
        return {
            roster : CounterpointCharacterization.plannerCounterRoster(),
            plans : [plan],
            enemyComposition : [{
                id : "counter-enemy-tank",
                compositionKey : "1:counter-enemy-tank",
                hpSum : PLANNER_COUNTER_ENEMY_HP,
                domain : COUNTERPOINTAI.DOMAIN_GROUND,
                canCapture : false
            }],
            ownCoverage : ownCoverage,
            baseline : {
                fieldedCounts : [],
                fieldedRoles : {
                    indirect : 0,
                    antiAir : 0,
                    directCombat : 0,
                    capture : 0,
                    transport : 0
                },
                threats : [{
                    key : "1:counter-enemy-tank",
                    id : "counter-enemy-tank",
                    need : need,
                    baseCoverage : coverage,
                    isAirThreat : false
                }],
                baseAirCoverage : 0,
                airNeed : 0,
                attackAirShare : 0
            }
        };
    },

    plannerCounterProjected : function(context)
    {
        var state = CounterpointCharacterization.plannerTestState(context.plans);
        state.dynamicBaseline = context.baseline;
        return COUNTERPOINTAI._projectedTurnContext(state);
    },

    plannerCounterStubSystem : function(weakDamage)
    {
        return {
            getCounterpointBaseDamage : function(attackerId, defenderId)
            {
                if (defenderId !== "counter-enemy-tank")
                {
                    return 0;
                }
                if (attackerId === "counter-neotank")
                {
                    return PLANNER_COUNTER_ELITE_DAMAGE;
                }
                return attackerId === "counter-tank" ?
                    PLANNER_COUNTER_STRONG_DAMAGE : weakDamage;
            }
        };
    },

    // Bank a one-turn shortfall when no affordable unit answers the new tank.
    plannerCounterSavingCases : function()
    {
        var previousEnabled = COUNTERPOINTAI.SAVE_FOR_COUNTERS;
        var previousGap = COUNTERPOINTAI.COUNTER_GAP_RATIO;
        var previousWorth = COUNTERPOINTAI.COUNTER_UTILITY_WORTH_RATIO;
        var previousTurns = COUNTERPOINTAI.COUNTER_SAVE_MAX_TURNS;
        COUNTERPOINTAI.SAVE_FOR_COUNTERS = true;
        COUNTERPOINTAI.COUNTER_GAP_RATIO = PLANNER_COUNTER_GAP_RATIO;
        COUNTERPOINTAI.COUNTER_UTILITY_WORTH_RATIO = PLANNER_COUNTER_WORTH_RATIO;
        COUNTERPOINTAI.COUNTER_SAVE_MAX_TURNS = 1;
        var previousMinDay = COUNTERPOINTAI.COUNTER_SAVE_MIN_DAY;
        COUNTERPOINTAI.COUNTER_SAVE_MIN_DAY = PLANNER_COUNTER_MIN_DAY;
        var ai = CounterpointCharacterization.plannerFerryStubAi(PLANNER_COUNTER_INCOME);
        var weakSystem = CounterpointCharacterization.plannerCounterStubSystem(
            PLANNER_COUNTER_WEAK_DAMAGE
        );
        var weakContext = CounterpointCharacterization.plannerCounterContext(
            PLANNER_COUNTER_LOW_COVERAGE,
            PLANNER_COUNTER_WEAK_DAMAGE
        );
        var banked = COUNTERPOINTAI._counterSaving(
            weakSystem,
            ai,
            weakContext,
            CounterpointCharacterization.plannerCounterProjected(weakContext),
            PLANNER_COUNTER_FUNDS,
            PLANNER_COUNTER_DAY
        );
        var coveredContext = CounterpointCharacterization.plannerCounterContext(
            PLANNER_COUNTER_HIGH_COVERAGE,
            PLANNER_COUNTER_WEAK_DAMAGE
        );
        var covered = COUNTERPOINTAI._counterSaving(
            weakSystem,
            ai,
            coveredContext,
            CounterpointCharacterization.plannerCounterProjected(coveredContext),
            PLANNER_COUNTER_FUNDS,
            PLANNER_COUNTER_DAY
        );
        var marginalContext = CounterpointCharacterization.plannerCounterContext(
            PLANNER_COUNTER_LOW_COVERAGE,
            PLANNER_COUNTER_MARGINAL_DAMAGE
        );
        var marginal = COUNTERPOINTAI._counterSaving(
            CounterpointCharacterization.plannerCounterStubSystem(
                PLANNER_COUNTER_MARGINAL_DAMAGE
            ),
            ai,
            marginalContext,
            CounterpointCharacterization.plannerCounterProjected(marginalContext),
            PLANNER_COUNTER_FUNDS,
            PLANNER_COUNTER_DAY
        );
        // The same real gap on an early day must not bank at all.
        var early = COUNTERPOINTAI._counterSaving(
            weakSystem,
            ai,
            weakContext,
            CounterpointCharacterization.plannerCounterProjected(weakContext),
            PLANNER_COUNTER_FUNDS,
            PLANNER_COUNTER_EARLY_DAY
        );
        COUNTERPOINTAI.SAVE_FOR_COUNTERS = previousEnabled;
        COUNTERPOINTAI.COUNTER_GAP_RATIO = previousGap;
        COUNTERPOINTAI.COUNTER_UTILITY_WORTH_RATIO = previousWorth;
        COUNTERPOINTAI.COUNTER_SAVE_MAX_TURNS = previousTurns;
        COUNTERPOINTAI.COUNTER_SAVE_MIN_DAY = previousMinDay;
        // Existing coverage or a near-equivalent answer must prevent banking.
        var perTurn = PLANNER_COUNTER_INCOME - PLANNER_FLOOR_CHEAP_COST;
        return covered === 0 && marginal === 0 && early === 0 &&
            banked === PLANNER_COUNTER_TANK_COST - perTurn &&
            banked < PLANNER_COUNTER_FUNDS - PLANNER_FLOOR_CHEAP_COST;
    },

    // Counter saving must not take funds reserved for an urgent ferry.
    plannerFerryOutranksCounter : function()
    {
        var previousEnabled = COUNTERPOINTAI.SAVE_FOR_COUNTERS;
        var previousGap = COUNTERPOINTAI.COUNTER_GAP_RATIO;
        var previousWorth = COUNTERPOINTAI.COUNTER_UTILITY_WORTH_RATIO;
        var previousTurns = COUNTERPOINTAI.COUNTER_SAVE_MAX_TURNS;
        var previousMinDay = COUNTERPOINTAI.COUNTER_SAVE_MIN_DAY;
        COUNTERPOINTAI.SAVE_FOR_COUNTERS = true;
        COUNTERPOINTAI.COUNTER_GAP_RATIO = PLANNER_COUNTER_GAP_RATIO;
        COUNTERPOINTAI.COUNTER_UTILITY_WORTH_RATIO = PLANNER_COUNTER_WORTH_RATIO;
        COUNTERPOINTAI.COUNTER_SAVE_MAX_TURNS = 1;
        COUNTERPOINTAI.COUNTER_SAVE_MIN_DAY = PLANNER_COUNTER_MIN_DAY;
        var system = CounterpointCharacterization.plannerCounterStubSystem(
            PLANNER_COUNTER_WEAK_DAMAGE
        );
        var ai = CounterpointCharacterization.plannerFerryStubAi(PLANNER_COUNTER_RICH_INCOME);
        var context = CounterpointCharacterization.plannerCounterContext(
            PLANNER_COUNTER_LOW_COVERAGE,
            PLANNER_COUNTER_WEAK_DAMAGE
        );
        var elite = context.plans[0].candidates[2];
        elite.baseCombatScore = PLANNER_COUNTER_FERRY_ELITE_SCORE;
        elite.coverageProfile[0].coverageDelta =
            PLANNER_COUNTER_FERRY_ELITE_SCORE;
        var projected = CounterpointCharacterization.plannerCounterProjected(context);
        // Do not bank ferry funds once the hull is immediately affordable.
        context.ferry = { urgent : true, cost : PLANNER_FERRY_HULL_COST };
        var withFerry = COUNTERPOINTAI._savingDecision(
            system,
            ai,
            context,
            projected,
            PLANNER_FERRY_RICH_FUNDS,
            PLANNER_COUNTER_DAY,
            0
        );
        // With no ferry in play the same funds must still bank for the counter.
        context.ferry = { urgent : false, cost : 0 };
        var withoutFerry = COUNTERPOINTAI._savingDecision(
            system,
            ai,
            context,
            projected,
            PLANNER_FERRY_RICH_FUNDS,
            PLANNER_COUNTER_DAY,
            0
        );
        COUNTERPOINTAI.SAVE_FOR_COUNTERS = previousEnabled;
        COUNTERPOINTAI.COUNTER_GAP_RATIO = previousGap;
        COUNTERPOINTAI.COUNTER_UTILITY_WORTH_RATIO = previousWorth;
        COUNTERPOINTAI.COUNTER_SAVE_MAX_TURNS = previousTurns;
        COUNTERPOINTAI.COUNTER_SAVE_MIN_DAY = previousMinDay;
        var perTurn = PLANNER_COUNTER_RICH_INCOME - PLANNER_FLOOR_CHEAP_COST;
        return withFerry === 0 &&
            withoutFerry === PLANNER_COUNTER_NEOTANK_COST - perTurn;
    },

    // Skipped urgent ferry funding stays banked for the bound harbour.
    plannerSkippedFerryMoneyStaysBanked : function()
    {
        var previousChance = COUNTERPOINTAI.RANDOM_BASE_SKIP_CHANCE;
        COUNTERPOINTAI.RANDOM_BASE_SKIP_CHANCE = COUNTERPOINTAI.PERCENT_MAX;
        var run = function(ferry)
        {
            var harbour = CounterpointCharacterization.plannerTestPlan(
                "skip-bank-harbour",
                0,
                CounterpointCharacterization.plannerTestCandidate(
                    "skip-bank-lander",
                    PLANNER_FERRY_HULL_COST,
                    false
                )
            );
            var factory = CounterpointCharacterization.plannerTestPlan(
                "skip-bank-factory",
                0,
                CounterpointCharacterization.plannerTestCandidate(
                    "skip-bank-infantry",
                    PLANNER_FLOOR_CHEAP_COST,
                    true
                )
            );
            var state = CounterpointCharacterization.plannerTestState([]);
            state.day = COUNTERPOINTAI.RANDOM_BASE_SKIP_MIN_DAY;
            COUNTERPOINTAI._allocatePhaseBudgets(
                state,
                [harbour, factory],
                PLANNER_SKIP_BANK_FUNDS,
                ferry
            );
            return state.heldFunds;
        };
        var ferryPlans = Object.create(null);
        ferryPlans["skip-bank-harbour"] = true;
        var banked = run({ urgent : true, cost : PLANNER_FERRY_HULL_COST,
                           plans : ferryPlans, needed : { naval : true } });
        var unbanked = run(null);
        COUNTERPOINTAI.RANDOM_BASE_SKIP_CHANCE = previousChance;
        return banked === PLANNER_FERRY_HULL_COST && unbanked === 0;
    },

    plannerFerryStubAi : function(income)
    {
        return {
            getPlayer : function()
            {
                return {
                    calcIncome : function()
                    {
                        return income;
                    }
                };
            }
        };
    },

    plannerFerrySavingCases : function()
    {
        // One capturer factory's worth of committed budget, which is what saving must never touch.
        var roster = { floorTotal : PLANNER_FLOOR_CHEAP_COST };
        var ferry = { urgent : true, cost : PLANNER_FERRY_HULL_COST };
        var previous = COUNTERPOINTAI.SAVE_FOR_FERRY;
        var previousTurns = COUNTERPOINTAI.FERRY_SAVE_MAX_TURNS;
        COUNTERPOINTAI.SAVE_FOR_FERRY = true;
        // Pin the tested saving horizon instead of inheriting a tunable default.
        COUNTERPOINTAI.FERRY_SAVE_MAX_TURNS = PLANNER_FERRY_SAVE_TURNS;
        var rich = COUNTERPOINTAI._ferrySaving(
            CounterpointCharacterization.plannerFerryStubAi(PLANNER_FERRY_GOOD_INCOME),
            roster,
            ferry,
            PLANNER_FERRY_RICH_FUNDS
        );
        var saving = COUNTERPOINTAI._ferrySaving(
            CounterpointCharacterization.plannerFerryStubAi(PLANNER_FERRY_GOOD_INCOME),
            roster,
            ferry,
            PLANNER_FERRY_SAVE_FUNDS
        );
        var partial = COUNTERPOINTAI._ferrySaving(
            CounterpointCharacterization.plannerFerryStubAi(PLANNER_FERRY_GOOD_INCOME),
            roster,
            ferry,
            PLANNER_FERRY_PARTIAL_FUNDS
        );
        var unreachable = COUNTERPOINTAI._ferrySaving(
            CounterpointCharacterization.plannerFerryStubAi(PLANNER_FERRY_POOR_INCOME),
            roster,
            ferry,
            PLANNER_FERRY_SAVE_FUNDS
        );
        var calm = COUNTERPOINTAI._ferrySaving(
            CounterpointCharacterization.plannerFerryStubAi(PLANNER_FERRY_GOOD_INCOME),
            roster,
            { urgent : false, cost : PLANNER_FERRY_HULL_COST },
            PLANNER_FERRY_SAVE_FUNDS
        );
        COUNTERPOINTAI.SAVE_FOR_FERRY = previous;
        COUNTERPOINTAI.FERRY_SAVE_MAX_TURNS = previousTurns;
        // Save only the spare shortfall that can accumulate within the horizon.
        var perTurn = PLANNER_FERRY_GOOD_INCOME - PLANNER_FLOOR_CHEAP_COST;
        return rich === 0 && unreachable === 0 && calm === 0 &&
            saving === PLANNER_FERRY_SAVE_FUNDS - PLANNER_FLOOR_CHEAP_COST &&
            partial === PLANNER_FERRY_HULL_COST - perTurn &&
            partial < PLANNER_FERRY_PARTIAL_FUNDS - PLANNER_FLOOR_CHEAP_COST;
    },

    plannerTestState : function(plans)
    {
        return {
            schemaVersion : COUNTERPOINTAI.PLANNER_STATE_SCHEMA_VERSION,
            algorithmVersion : COUNTERPOINTAI.RNG_ALGORITHM_VERSION,
            strategyVersion : COUNTERPOINTAI.STRATEGY_VERSION,
            playerId : 0,
            day : 1,
            generation : 1,
            seed : 0,
            drawCounter : 0,
            heldFunds : 0,
            specialPrepared : true,
            ordinaryPrepared : true,
            specialRandomFundingJitter : null,
            ordinaryRandomFundingJitter : null,
            dynamicBaseline : CounterpointCharacterization.plannerTestBaseline(),
            turnTargets : CounterpointCharacterization.plannerTurnTargets(),
            plans : plans
        };
    },

    plannerTestBaseline : function()
    {
        return {
            fieldedCounts : [],
            fieldedRoles : {
                indirect : 0,
                antiAir : 0,
                directCombat : 0,
                capture : 0,
                transport : 0
            },
            threats : [],
            baseAirCoverage : 0,
            airNeed : 0,
            attackAirShare : 0
        };
    },

    plannerTurnTargets : function()
    {
        return { aaPerTurn : -1, indirectRemaining : -1 };
    },

    plannerSelectPlanCandidate : function(plans, planIndex, funds, turnTargets)
    {
        var state = CounterpointCharacterization.plannerTestState(plans);
        if (turnTargets !== undefined)
        {
            state.turnTargets = turnTargets;
        }
        return COUNTERPOINTAI._selectPlanCandidate(
            state,
            planIndex,
            funds,
            state.turnTargets
        );
    },

    plannerRecycledBudgetUnlocks : function()
    {
        var source = CounterpointCharacterization.plannerTestPlan(
            "recycle-source",
            PLANNER_RECYCLE_SOURCE_BUDGET,
            CounterpointCharacterization.plannerTestCandidate(
                "recycle-source-unit",
                PLANNER_RECYCLE_SOURCE_SPEND,
                false
            )
        );
        source.complete = true;
        var target = CounterpointCharacterization.plannerTestPlan(
            "recycle-target",
            PLANNER_RECYCLE_TARGET_BUDGET,
            CounterpointCharacterization.plannerTestCandidate(
                "recycle-target-unit",
                PLANNER_RECYCLE_TARGET_COST,
                false
            )
        );
        var plans = [source, target];
        var initiallyDeferred = target.selected === -1 &&
            target.candidates[0].transactionCost > target.reservedBudget;
        COUNTERPOINTAI._releasePlanBudget(
            plans,
            0,
            PLANNER_RECYCLE_SOURCE_SPEND
        );
        return initiallyDeferred &&
            target.reservedBudget >= PLANNER_RECYCLE_TARGET_COST &&
            CounterpointCharacterization.plannerSelectPlanCandidate(
                plans,
                1,
                PLANNER_RECYCLE_TARGET_COST
            ) &&
            target.selected === 0 && !target.complete;
    },

    plannerOrdinaryRescanUnlocks : function()
    {
        var configured = COUNTERPOINTAI.AVOID_BUDGET_BASE_SKIPS;
        COUNTERPOINTAI.AVOID_BUDGET_BASE_SKIPS = false;
        var result = CounterpointCharacterization._plannerOrdinaryRescanUnlocks();
        COUNTERPOINTAI.AVOID_BUDGET_BASE_SKIPS = configured;
        return result;
    },

    // A deferred non-ground plan is retried after an invalid sibling releases its budget.
    _plannerOrdinaryRescanUnlocks : function()
    {
        var encoded = [];
        var variable = {
            readDataListString : function() { return encoded; },
            writeDataListString : function(values) { encoded = values; }
        };
        var targetCandidate = CounterpointCharacterization.plannerTestCandidate(
            "ordinary-rescan-target",
            PLANNER_RECYCLE_TARGET_COST,
            false
        );
        targetCandidate.domain = COUNTERPOINTAI.DOMAIN_AIR;
        var target = CounterpointCharacterization.plannerTestPlan(
            COUNTERPOINTAI._planKey(0, 0, COUNTERPOINTAI.ACTION_BUILD_UNITS),
            PLANNER_RECYCLE_TARGET_BUDGET,
            targetCandidate
        );
        target.x = 0;
        target.y = 0;
        target.actionId = COUNTERPOINTAI.ACTION_BUILD_UNITS;
        var source = CounterpointCharacterization.plannerTestPlan(
            COUNTERPOINTAI._planKey(1, 0, COUNTERPOINTAI.ACTION_BUILD_UNITS),
            PLANNER_RECYCLE_SOURCE_BUDGET,
            CounterpointCharacterization.plannerTestCandidate(
                "ordinary-rescan-source",
                PLANNER_RECYCLE_SOURCE_SPEND,
                false
            )
        );
        source.x = 1;
        source.y = 0;
        source.actionId = COUNTERPOINTAI.ACTION_BUILD_UNITS;
        var state = CounterpointCharacterization.plannerTestState([target, source]);
        var builtId = "";
        var data = {
            getActionAvailable : function() { return true; },
            getUnitIds : function() { return [targetCandidate.id]; },
            getTransactionCosts : function()
            {
                return [targetCandidate.transactionCost];
            },
            getStrategicValues : function()
            {
                return [targetCandidate.strategicValue];
            },
            getEnabledList : function() { return [true]; }
        };
        var system = {
            getVariables : function()
            {
                return {
                    createVariable : function() { return variable; }
                };
            },
            getProductionActionData : function() { return data; },
            executeCounterpointBuild : function(x, y, id)
            {
                builtId = id;
                return true;
            }
        };
        var player = {
            getPlayerID : function() { return 0; },
            getFunds : function() { return PLANNER_AVOID_AVAILABLE_FUNDS; }
        };
        var ai = { getPlayer : function() { return player; } };
        var building = { getOwnerID : function() { return 0; } };
        var map = {
            getCurrentDay : function() { return 1; },
            onMap : function(x) { return x === 0; },
            getTerrain : function()
            {
                return { getBuilding : function() { return building; } };
            }
        };
        if (!COUNTERPOINTAI._savePlannerState(system, state) ||
            !COUNTERPOINTAI.buildUnitSimpleProductionSystem(
                system,
                ai,
                [],
                [],
                [],
                [],
                map
            ))
        {
            return false;
        }
        var settled = COUNTERPOINTAI._loadPlannerState(system);
        return builtId === targetCandidate.id && settled !== null &&
            settled.plans[0].complete && settled.plans[1].complete;
    },

    plannerFailedBorrowRestoresDonors : function()
    {
        // Give donors a nonzero candidate cost so borrowing reaches rollback.
        var floor = PLANNER_BORROW_DONOR_FLOOR;
        var requester = CounterpointCharacterization.plannerTestPlan(
            "borrow-requester",
            floor,
            CounterpointCharacterization.plannerTestCandidate(
                "borrow-capper",
                floor + PLANNER_BORROW_REQUIRED_MARGIN,
                true
            )
        );
        // Donors retain their capturer floor and lend only surplus.
        var donorOne = CounterpointCharacterization.plannerTestPlan(
            "borrow-donor-one",
            floor + PLANNER_BORROW_DONOR_ONE_MARGIN,
            CounterpointCharacterization.plannerTestCandidate(
                "borrow-donor-one-unit",
                floor,
                true
            )
        );
        var donorTwo = CounterpointCharacterization.plannerTestPlan(
            "borrow-donor-two",
            floor + PLANNER_BORROW_DONOR_TWO_MARGIN,
            CounterpointCharacterization.plannerTestCandidate(
                "borrow-donor-two-unit",
                floor,
                true
            )
        );
        var plans = [requester, donorOne, donorTwo];
        var requesterBudget = requester.reservedBudget;
        var donorOneBudget = donorOne.reservedBudget;
        var donorTwoBudget = donorTwo.reservedBudget;
        var borrowed = COUNTERPOINTAI._borrowForCandidate(
            plans,
            0,
            requester.candidates[0].transactionCost
        );
        return !borrowed && requester.reservedBudget === requesterBudget &&
            donorOne.reservedBudget === donorOneBudget &&
            donorTwo.reservedBudget === donorTwoBudget &&
            requester.borrowed.length === 0 &&
            !donorOne.complete && !donorTwo.complete;
    },

    plannerActionData : function(enabled)
    {
        return {
            getActionAvailable : function() { return true; },
            getUnitIds : function() { return ["live-free", "live-paid"]; },
            getTransactionCosts : function()
            {
                return [0, PLANNER_LIVE_PAID_COST];
            },
            getStrategicValues : function()
            {
                return [1, PLANNER_LIVE_PAID_COST];
            },
            getEnabledList : function() { return enabled; }
        };
    },

    plannerLiveCostKindsRefresh : function()
    {
        var plan = CounterpointCharacterization.plannerTestPlan(
            "live-cost-kinds",
            PLANNER_LIVE_PAID_COST,
            CounterpointCharacterization.plannerTestCandidate(
                "live-free",
                0,
                false
            )
        );
        plan.candidates.push(CounterpointCharacterization.plannerTestCandidate(
            "live-paid",
            PLANNER_LIVE_PAID_COST,
            false
        ));
        COUNTERPOINTAI._refreshPlanCostKinds(plan);
        var mixed = plan.hasFree && plan.hasPaid && !plan.freeOnly;
        COUNTERPOINTAI._updatePlanFromActionData(
            plan,
            CounterpointCharacterization.plannerActionData([true, false])
        );
        var firstRefresh = plan.hasFree && plan.hasPaid && !plan.freeOnly &&
            plan.candidates[0].enabled && !plan.candidates[1].enabled;
        COUNTERPOINTAI._updatePlanFromActionData(
            plan,
            CounterpointCharacterization.plannerActionData([false, true])
        );
        return mixed && firstRefresh && plan.hasFree && plan.hasPaid &&
            !plan.freeOnly && !plan.candidates[0].enabled &&
            plan.candidates[1].enabled;
    },

    plannerAvoidBudgetBaseSkips : function()
    {
        var configured = COUNTERPOINTAI.AVOID_BUDGET_BASE_SKIPS;
        var expensive = CounterpointCharacterization.plannerTestPlan(
            "avoid-expensive",
            PLANNER_AVOID_RESERVED_BUDGET,
            CounterpointCharacterization.plannerTestCandidate(
                "avoid-expensive-unit",
                PLANNER_AVOID_EXPENSIVE_COST,
                false
            )
        );
        var affordable = CounterpointCharacterization.plannerTestCandidate(
            "avoid-affordable-unit",
            PLANNER_AVOID_AFFORDABLE_COST,
            false
        );
        expensive.candidates[0].domain = COUNTERPOINTAI.DOMAIN_AIR;
        affordable.domain = COUNTERPOINTAI.DOMAIN_AIR;
        expensive.candidates.push(affordable);
        expensive.order.push(1);
        COUNTERPOINTAI._refreshPlanCostKinds(expensive);
        COUNTERPOINTAI.AVOID_BUDGET_BASE_SKIPS = true;
        var normalPriority = CounterpointCharacterization.plannerSelectPlanCandidate(
            [expensive],
            0,
            PLANNER_AVOID_AVAILABLE_FUNDS
        ) && expensive.selected === 1;

        var fallback = CounterpointCharacterization.plannerTestPlan(
            "avoid-fallback",
            PLANNER_AVOID_RESERVED_BUDGET,
            CounterpointCharacterization.plannerTestCandidate(
                "avoid-fallback-unit",
                PLANNER_AVOID_EXPENSIVE_COST,
                false
            )
        );
        fallback.candidates[0].domain = COUNTERPOINTAI.DOMAIN_AIR;
        var fallbackSelected = CounterpointCharacterization.plannerSelectPlanCandidate(
            [fallback],
            0,
            PLANNER_AVOID_AVAILABLE_FUNDS
        ) && fallback.selected === 0 && fallback.reservedBudget ===
            PLANNER_AVOID_RESERVED_BUDGET;

        var tooExpensive = CounterpointCharacterization.plannerTestPlan(
            "avoid-too-expensive",
            PLANNER_AVOID_RESERVED_BUDGET,
            CounterpointCharacterization.plannerTestCandidate(
                "avoid-too-expensive-unit",
                PLANNER_AVOID_TOO_EXPENSIVE_COST,
                false
            )
        );
        tooExpensive.candidates[0].domain = COUNTERPOINTAI.DOMAIN_AIR;
        var totalFundsDeferred = !CounterpointCharacterization.plannerSelectPlanCandidate(
            [tooExpensive],
            0,
            PLANNER_AVOID_AVAILABLE_FUNDS
        ) && !tooExpensive.complete;

        COUNTERPOINTAI.AVOID_BUDGET_BASE_SKIPS = false;
        var disabled = CounterpointCharacterization.plannerTestPlan(
            "avoid-disabled",
            PLANNER_AVOID_RESERVED_BUDGET,
            CounterpointCharacterization.plannerTestCandidate(
                "avoid-disabled-unit",
                PLANNER_AVOID_EXPENSIVE_COST,
                false
            )
        );
        disabled.candidates[0].domain = COUNTERPOINTAI.DOMAIN_AIR;
        var configuredOffDeferred =
            !CounterpointCharacterization.plannerSelectPlanCandidate(
            [disabled],
            0,
            PLANNER_AVOID_AVAILABLE_FUNDS
        ) && !disabled.complete;
        COUNTERPOINTAI.AVOID_BUDGET_BASE_SKIPS = configured;
        return normalPriority && fallbackSelected && totalFundsDeferred &&
            configuredOffDeferred;
    },

    plannerDisabledCandidateRevives : function()
    {
        var enabled = false;
        var data = {
            getX : function() { return 0; },
            getY : function() { return 0; },
            getActionId : function() { return COUNTERPOINTAI.ACTION_BUILD_UNITS; },
            getActionAvailable : function() { return true; },
            getUnitIds : function() { return ["revived-unit"]; },
            getTransactionCosts : function() { return [PLANNER_LIVE_PAID_COST]; },
            getStrategicValues : function() { return [PLANNER_LIVE_PAID_COST]; },
            getEnabledList : function() { return [enabled]; }
        };
        var plan = COUNTERPOINTAI._newPlan(data, COUNTERPOINTAI.PHASE_ORDINARY);
        plan.reservedBudget = PLANNER_LIVE_PAID_COST;
        plan.order = [0];
        var deferred = plan.rejected.length === 0 &&
            !CounterpointCharacterization.plannerSelectPlanCandidate(
                [plan],
                0,
                PLANNER_AVOID_AVAILABLE_FUNDS
            ) && !plan.complete;
        enabled = true;
        COUNTERPOINTAI._updatePlanFromActionData(plan, data);
        return deferred &&
            CounterpointCharacterization.plannerSelectPlanCandidate(
            [plan],
            0,
            PLANNER_AVOID_AVAILABLE_FUNDS
        ) && plan.selected === 0;
    },

    plannerPhaseActionValidation : function()
    {
        var data = {
            getX : function() { return 0; },
            getY : function() { return 0; },
            getActionId : function() { return COUNTERPOINTAI.ACTION_BUILD_UNITS; },
            getActionAvailable : function() { return true; },
            getUnitIds : function() { return ["validation-unit"]; },
            getTransactionCosts : function() { return [PLANNER_LIVE_PAID_COST]; },
            getStrategicValues : function() { return [PLANNER_LIVE_PAID_COST]; },
            getEnabledList : function() { return [true]; }
        };
        var ordinary = COUNTERPOINTAI._newPlan(
            data,
            COUNTERPOINTAI.PHASE_ORDINARY
        );
        ordinary.order = [0];
        ordinary.reservedBudget = PLANNER_LIVE_PAID_COST;
        var special = JSON.parse(JSON.stringify(ordinary));
        special.phase = COUNTERPOINTAI.PHASE_SPECIAL;
        special.actionId = DISCOVERY_ACTION;
        special.key = COUNTERPOINTAI._planKey(special.x, special.y, special.actionId);
        var invalidOrdinary = JSON.parse(JSON.stringify(ordinary));
        invalidOrdinary.actionId = DISCOVERY_ACTION;
        invalidOrdinary.key = COUNTERPOINTAI._planKey(
            invalidOrdinary.x,
            invalidOrdinary.y,
            invalidOrdinary.actionId
        );
        var invalidSpecial = JSON.parse(JSON.stringify(special));
        invalidSpecial.actionId = COUNTERPOINTAI.ACTION_BUILD_UNITS;
        invalidSpecial.key = COUNTERPOINTAI._planKey(
            invalidSpecial.x,
            invalidSpecial.y,
            invalidSpecial.actionId
        );
        return COUNTERPOINTAI._validPlannerPlan(ordinary, 0) &&
            COUNTERPOINTAI._validPlannerPlan(special, 0) &&
            !COUNTERPOINTAI._validPlannerPlan(invalidOrdinary, 0) &&
            !COUNTERPOINTAI._validPlannerPlan(invalidSpecial, 0);
    },

    plannerRejectsNonConservingBorrow : function()
    {
        var borrower = CounterpointCharacterization.plannerTestPlan(
            COUNTERPOINTAI._planKey(0, 0, COUNTERPOINTAI.ACTION_BUILD_UNITS),
            PLANNER_AVOID_RESERVED_BUDGET,
            CounterpointCharacterization.plannerTestCandidate(
                "validation-capper",
                PLANNER_AVOID_RESERVED_BUDGET,
                true
            )
        );
        borrower.x = 0;
        borrower.y = 0;
        borrower.actionId = COUNTERPOINTAI.ACTION_BUILD_UNITS;
        borrower.selected = 0;
        var donor = CounterpointCharacterization.plannerTestPlan(
            COUNTERPOINTAI._planKey(1, 0, COUNTERPOINTAI.ACTION_BUILD_UNITS),
            PLANNER_AVOID_RESERVED_BUDGET,
            CounterpointCharacterization.plannerTestCandidate(
                "validation-donor",
                PLANNER_AVOID_RESERVED_BUDGET,
                false
            )
        );
        donor.x = 1;
        donor.y = 0;
        donor.actionId = COUNTERPOINTAI.ACTION_BUILD_UNITS;
        borrower.borrowed = [{
            key : donor.key,
            amount : PLANNER_AVOID_RESERVED_BUDGET
        }];
        var state = CounterpointCharacterization.plannerTestState([
            borrower,
            donor
        ]);
        var valid = COUNTERPOINTAI._validPlannerState(state);
        borrower.borrowed[0].amount = PLANNER_AVOID_RESERVED_BUDGET * 2;
        return valid && !COUNTERPOINTAI._validPlannerState(state);
    },

    plannerTerminalLiveFailureReleases : function()
    {
        var plan = CounterpointCharacterization.plannerTestPlan(
            "terminal-live-failure",
            PLANNER_AVOID_RESERVED_BUDGET,
            CounterpointCharacterization.plannerTestCandidate(
                "terminal-live-failure-unit",
                PLANNER_AVOID_RESERVED_BUDGET,
                false
            )
        );
        plan.selected = 0;
        var state = CounterpointCharacterization.plannerTestState([plan]);
        var resolved = COUNTERPOINTAI._resolveLivePlanCandidate(
            state,
            0,
            [],
            [],
            [],
            PLANNER_AVOID_AVAILABLE_FUNDS,
            state.turnTargets
        );
        return resolved === null && plan.complete && plan.selected === -1 &&
            plan.rejected.length === 1 && plan.rejected[0] === 0 &&
            plan.reservedBudget === 0 && !plan.executed;
    },

    plannerSpecialResultHandshake : function()
    {
        var encoded = [];
        var variable = {
            readDataListString : function() { return encoded; },
            writeDataListString : function(values) { encoded = values; }
        };
        var system = {
            getVariables : function()
            {
                return {
                    createVariable : function() { return variable; }
                };
            }
        };
        var player = {
            getPlayerID : function() { return 0; },
            getFunds : function() { return PLANNER_AVOID_AVAILABLE_FUNDS; }
        };
        var ai = {
            getSimpleProductionSystem : function() { return system; },
            getPlayer : function() { return player; }
        };
        var map = {
            getCurrentDay : function() { return 1; },
            onMap : function() { return false; }
        };
        var first = CounterpointCharacterization.plannerTestPlan(
            COUNTERPOINTAI._planKey(0, 0, DISCOVERY_ACTION),
            PLANNER_AVOID_RESERVED_BUDGET,
            CounterpointCharacterization.plannerTestCandidate(
                "special-first-unit",
                PLANNER_AVOID_RESERVED_BUDGET,
                false
            )
        );
        first.x = 0;
        first.y = 0;
        first.actionId = DISCOVERY_ACTION;
        first.phase = COUNTERPOINTAI.PHASE_SPECIAL;
        first.candidates.push(CounterpointCharacterization.plannerTestCandidate(
            "special-second-unit",
            PLANNER_AVOID_RESERVED_BUDGET,
            false
        ));
        first.order.push(1);
        first.selected = 0;
        COUNTERPOINTAI._refreshPlanCostKinds(first);
        var sibling = CounterpointCharacterization.plannerTestPlan(
            COUNTERPOINTAI._planKey(0, 0, PLANNER_SECOND_SPECIAL_ACTION),
            PLANNER_AVOID_RESERVED_BUDGET,
            CounterpointCharacterization.plannerTestCandidate(
                "special-sibling-unit",
                PLANNER_AVOID_RESERVED_BUDGET,
                false
            )
        );
        sibling.x = 0;
        sibling.y = 0;
        sibling.actionId = PLANNER_SECOND_SPECIAL_ACTION;
        sibling.phase = COUNTERPOINTAI.PHASE_SPECIAL;
        var state = CounterpointCharacterization.plannerTestState([
            first,
            sibling
        ]);
        if (!COUNTERPOINTAI._savePlannerState(system, state) ||
            !COUNTERPOINTAI.onBuildingMenuItemResult(
                ai,
                {},
                false,
                0,
                0,
                DISCOVERY_ACTION,
                map
            ))
        {
            return false;
        }
        var retried = COUNTERPOINTAI._loadPlannerState(system);
        if (retried === null || retried.plans[0].complete ||
            retried.plans[0].executed ||
            retried.plans[0].selected !== 1 ||
            retried.plans[0].rejected.indexOf(0) < 0)
        {
            return false;
        }
        if (COUNTERPOINTAI.onBuildingMenuItemResult(
                ai,
                {},
                true,
                0,
                0,
                DISCOVERY_ACTION,
                map
            ))
        {
            return false;
        }
        var settled = COUNTERPOINTAI._loadPlannerState(system);
        if (settled === null || !settled.plans[0].complete ||
            !settled.plans[0].executed || !settled.plans[1].complete ||
            settled.plans[1].executed || settled.plans[1].reservedBudget !== 0)
        {
            return false;
        }
        var terminal = CounterpointCharacterization.plannerTestPlan(
            COUNTERPOINTAI._planKey(0, 0, DISCOVERY_ACTION),
            PLANNER_AVOID_RESERVED_BUDGET,
            CounterpointCharacterization.plannerTestCandidate(
                "special-terminal-unit",
                PLANNER_AVOID_RESERVED_BUDGET,
                false
            )
        );
        terminal.x = 0;
        terminal.y = 0;
        terminal.actionId = DISCOVERY_ACTION;
        terminal.phase = COUNTERPOINTAI.PHASE_SPECIAL;
        terminal.selected = 0;
        state = CounterpointCharacterization.plannerTestState([terminal]);
        if (!COUNTERPOINTAI._savePlannerState(system, state) ||
            COUNTERPOINTAI.onBuildingMenuItemResult(
                ai,
                {},
                false,
                0,
                0,
                DISCOVERY_ACTION,
                map
            ) !== GameEnums.MenuSelection_Restart)
        {
            return false;
        }
        var terminalState = COUNTERPOINTAI._loadPlannerState(system);
        return terminalState !== null && terminalState.plans[0].complete &&
            !terminalState.plans[0].executed &&
            terminalState.plans[0].reservedBudget === 0;
    },

    plannerMarginalCandidate : function(id, cost, score)
    {
        var candidate = CounterpointCharacterization.plannerTestCandidate(
            id,
            cost,
            false
        );
        candidate.baseCombatScore = score;
        candidate.purchaseUtility = COUNTERPOINTAI._purchaseUtility(
            score,
            cost,
            candidate
        );
        candidate.movement = 5;
        return candidate;
    },

    plannerExecutedTestPlan : function(key, candidate)
    {
        var plan = CounterpointCharacterization.plannerTestPlan(key, 0, candidate);
        plan.selected = 0;
        plan.executed = true;
        plan.complete = true;
        return plan;
    },

    plannerValidTestPlan : function(x, y, candidate)
    {
        var key = COUNTERPOINTAI._planKey(
            x,
            y,
            COUNTERPOINTAI.ACTION_BUILD_UNITS
        );
        var plan = CounterpointCharacterization.plannerTestPlan(
            key,
            candidate.transactionCost,
            candidate
        );
        plan.x = x;
        plan.y = y;
        plan.valueReach = candidate.transactionCost;
        return plan;
    },

    plannerAirBaseline : function(airNeed)
    {
        var baseline = CounterpointCharacterization.plannerTestBaseline();
        baseline.fieldedCounts = [{ id : "fighter-like", count : 1 }];
        baseline.threats = [{
            key : "1:air-threat",
            id : "air-threat",
            need : airNeed,
            baseCoverage : 0,
            isAirThreat : true
        }];
        baseline.airNeed = airNeed;
        baseline.attackAirShare = 1;
        return baseline;
    },

    plannerAirCandidate : function(score, airCoverage)
    {
        var candidate = CounterpointCharacterization.plannerMarginalCandidate(
            "fighter-like",
            10000,
            score
        );
        candidate.isAA = true;
        candidate.airCoverageDelta = airCoverage;
        candidate.coverageProfile = [{
            threatIndex : 0,
            coverageDelta : airCoverage,
            staticWeight : 1
        }];
        return candidate;
    },

    plannerMarginalRegressions : function()
    {
        var first = CounterpointCharacterization.plannerAirCandidate(100, 100);
        var second = CounterpointCharacterization.plannerAirCandidate(100, 100);
        var alternative = CounterpointCharacterization.plannerMarginalCandidate(
            "alternative-like",
            10000,
            60
        );
        var executed = CounterpointCharacterization.plannerExecutedTestPlan(
            "air-first",
            first
        );
        var choice = CounterpointCharacterization.plannerTestPlan(
            "air-second",
            10000,
            second
        );
        choice.candidates.push(alternative);
        choice.order = [0, 1];
        choice.valueReach = 10000;
        COUNTERPOINTAI._refreshPlanCostKinds(choice);
        var state = CounterpointCharacterization.plannerTestState([
            executed,
            choice
        ]);
        state.dynamicBaseline =
            CounterpointCharacterization.plannerAirBaseline(100);
        var beforeState = JSON.parse(JSON.stringify(state));
        beforeState.plans[0].executed = false;
        beforeState.plans[0].complete = false;
        beforeState.plans[0].selected = -1;
        var beforeProjected = COUNTERPOINTAI._projectedTurnContext(beforeState);
        var afterProjected = COUNTERPOINTAI._projectedTurnContext(state);
        var aaBefore = COUNTERPOINTAI._evaluateMarginalPurchase(
            second,
            choice,
            beforeProjected,
            second.transactionCost
        );
        var aaAfter = COUNTERPOINTAI._evaluateMarginalPurchase(
            second,
            choice,
            afterProjected,
            second.transactionCost
        );
        var selectedAlternative = COUNTERPOINTAI._selectPlanCandidate(
            state,
            1,
            10000,
            state.turnTargets
        ) && choice.selected === 1 &&
            choice.rejected.indexOf(0) < 0;

        var demandedFirst =
            CounterpointCharacterization.plannerAirCandidate(500, 100);
        var demandedSecond =
            CounterpointCharacterization.plannerAirCandidate(500, 100);
        var weakAlternative =
            CounterpointCharacterization.plannerMarginalCandidate(
                "weak-alternative-like",
                10000,
                10
            );
        var demandedChoice = CounterpointCharacterization.plannerTestPlan(
            "air-demand-second",
            10000,
            demandedSecond
        );
        demandedChoice.candidates.push(weakAlternative);
        demandedChoice.order = [0, 1];
        demandedChoice.valueReach = 10000;
        COUNTERPOINTAI._refreshPlanCostKinds(demandedChoice);
        var demandedState = CounterpointCharacterization.plannerTestState([
            CounterpointCharacterization.plannerExecutedTestPlan(
                "air-demand-first",
                demandedFirst
            ),
            demandedChoice
        ]);
        demandedState.dynamicBaseline =
            CounterpointCharacterization.plannerAirBaseline(1000);
        var duplicateStillSelected = COUNTERPOINTAI._selectPlanCandidate(
            demandedState,
            1,
            10000,
            demandedState.turnTargets
        ) && demandedChoice.selected === 0;

        var roleBaseline = CounterpointCharacterization.plannerTestBaseline();
        roleBaseline.fieldedRoles.indirect =
            COUNTERPOINTAI.INDIRECT_ROLE_FREE_COUNT;
        var artillery = CounterpointCharacterization.plannerMarginalCandidate(
            "artillery-like",
            6000,
            100
        );
        artillery.isIndirect = true;
        var missile = CounterpointCharacterization.plannerMarginalCandidate(
            "missile-like",
            6000,
            100
        );
        missile.isIndirect = true;
        var roleState = CounterpointCharacterization.plannerTestState([
            CounterpointCharacterization.plannerExecutedTestPlan(
                "cross-indirect-first",
                artillery
            )
        ]);
        roleState.dynamicBaseline = roleBaseline;
        var roleBeforeState = JSON.parse(JSON.stringify(roleState));
        roleBeforeState.plans[0].executed = false;
        roleBeforeState.plans[0].complete = false;
        roleBeforeState.plans[0].selected = -1;
        var roleBefore = COUNTERPOINTAI._evaluateMarginalPurchase(
            missile,
            null,
            COUNTERPOINTAI._projectedTurnContext(roleBeforeState),
            missile.transactionCost
        );
        var roleAfterProjected = COUNTERPOINTAI._projectedTurnContext(roleState);
        var roleAfter = COUNTERPOINTAI._evaluateMarginalPurchase(
            missile,
            null,
            roleAfterProjected,
            missile.transactionCost
        );

        var exactFirst = CounterpointCharacterization.plannerMarginalCandidate(
            "battery-like",
            6000,
            100
        );
        exactFirst.isIndirect = true;
        exactFirst.indirectStacked = true;
        var exactSecond = JSON.parse(JSON.stringify(exactFirst));
        var exactState = CounterpointCharacterization.plannerTestState([
            CounterpointCharacterization.plannerExecutedTestPlan(
                "exact-indirect-first",
                exactFirst
            )
        ]);
        var exactProjected = COUNTERPOINTAI._projectedTurnContext(exactState);
        var exactAfter = COUNTERPOINTAI._evaluateMarginalPurchase(
            exactSecond,
            null,
            exactProjected,
            exactSecond.transactionCost
        );
        var exactExpected = exactSecond.baseCombatScore *
            COUNTERPOINTAI.SAME_TURN_DUPLICATE_FACTOR *
            COUNTERPOINTAI.INDIRECT_STACK_PENALTY;

        return {
            duplicateDamped : selectedAlternative,
            duplicatePossible : duplicateStillSelected,
            projectedAA : afterProjected.airCoverage === 100 &&
                aaAfter.adjustedCombatScore < aaBefore.adjustedCombatScore,
            crossIndirect : roleAfterProjected.builtThisTurnById[
                    "#missile-like"
                ] === undefined &&
                roleAfter.adjustedCombatScore < roleBefore.adjustedCombatScore,
            exactIndirect : exactProjected.builtThisTurnById[
                    "#battery-like"
                ] === 1 &&
                CounterpointCharacterization.almostEqual(
                    exactAfter.adjustedCombatScore,
                    exactExpected
                )
        };
    },

    plannerSavingContext : function(futureScore, futureDeploymentTurns)
    {
        var immediate = CounterpointCharacterization.plannerMarginalCandidate(
            "move-six-immediate",
            22000,
            300
        );
        immediate.domain = COUNTERPOINTAI.DOMAIN_AIR;
        immediate.movement = 6;
        immediate.deploymentKnown = true;
        immediate.deploymentTurns = 1;
        immediate.coverageProfile = [{
            threatIndex : 0,
            coverageDelta : 30,
            staticWeight : 1
        }];
        var future = CounterpointCharacterization.plannerMarginalCandidate(
            "move-four-future",
            28000,
            futureScore
        );
        future.domain = COUNTERPOINTAI.DOMAIN_AIR;
        future.movement = 4;
        future.deploymentKnown = true;
        future.deploymentTurns = futureDeploymentTurns;
        future.coverageProfile = [{
            threatIndex : 0,
            coverageDelta : 100,
            staticWeight : 1
        }];
        var plan = CounterpointCharacterization.plannerTestPlan(
            "saving-lineup",
            22000,
            immediate
        );
        plan.candidates.push(future);
        plan.order = [1, 0];
        plan.valueReach = 28000;
        COUNTERPOINTAI._refreshPlanCostKinds(plan);
        var state = CounterpointCharacterization.plannerTestState([plan]);
        var baseline = CounterpointCharacterization.plannerTestBaseline();
        baseline.threats = [{
            key : "1:armored-threat",
            id : "armored-threat",
            need : 100,
            baseCoverage : 0,
            isAirThreat : false
        }];
        state.dynamicBaseline = baseline;
        return {
            state : state,
            context : {
                roster : { floorTotal : 0 },
                plans : state.plans,
                ferry : null
            },
            projected : COUNTERPOINTAI._projectedTurnContext(state)
        };
    },

    plannerCanonicalSavingRegressions : function()
    {
        var previousEnabled = COUNTERPOINTAI.SAVE_FOR_COUNTERS;
        var previousMinDay = COUNTERPOINTAI.COUNTER_SAVE_MIN_DAY;
        var previousTurns = COUNTERPOINTAI.COUNTER_SAVE_MAX_TURNS;
        var previousWorth = COUNTERPOINTAI.COUNTER_UTILITY_WORTH_RATIO;
        var previousDelay = COUNTERPOINTAI.SAVE_DELAY_UTILITY_TAX;
        COUNTERPOINTAI.SAVE_FOR_COUNTERS = true;
        COUNTERPOINTAI.COUNTER_SAVE_MIN_DAY = 1;
        COUNTERPOINTAI.COUNTER_SAVE_MAX_TURNS = 1;
        COUNTERPOINTAI.COUNTER_UTILITY_WORTH_RATIO = 1.25;
        COUNTERPOINTAI.SAVE_DELAY_UTILITY_TAX = 0.25;
        var ai = CounterpointCharacterization.plannerFerryStubAi(6000);
        var immediate = CounterpointCharacterization.plannerSavingContext(
            330,
            4
        );
        var immediateHold = COUNTERPOINTAI._counterSaving(
            {},
            ai,
            immediate.context,
            immediate.projected,
            22000,
            5
        );
        var immediateSelected = COUNTERPOINTAI._selectPlanCandidate(
            immediate.state,
            0,
            22000,
            immediate.state.turnTargets
        ) && immediate.state.plans[0].selected === 0;
        var heavy = CounterpointCharacterization.plannerSavingContext(
            1000,
            1
        );
        var heavyHold = COUNTERPOINTAI._counterSaving(
            {},
            ai,
            heavy.context,
            heavy.projected,
            22000,
            5
        );
        COUNTERPOINTAI.SAVE_FOR_COUNTERS = previousEnabled;
        COUNTERPOINTAI.COUNTER_SAVE_MIN_DAY = previousMinDay;
        COUNTERPOINTAI.COUNTER_SAVE_MAX_TURNS = previousTurns;
        COUNTERPOINTAI.COUNTER_UTILITY_WORTH_RATIO = previousWorth;
        COUNTERPOINTAI.SAVE_DELAY_UTILITY_TAX = previousDelay;
        return {
            immediate : immediateHold === 0 && immediateSelected,
            heavy : heavyHold > 0
        };
    },

    plannerStateReconstructionRegressions : function()
    {
        var encoded = [];
        var variable = {
            readDataListString : function() { return encoded; },
            writeDataListString : function(values) { encoded = values; }
        };
        var system = {
            getVariables : function()
            {
                return {
                    createVariable : function() { return variable; }
                };
            }
        };
        var firstCandidate =
            CounterpointCharacterization.plannerMarginalCandidate(
                "serialized-aa",
                1000,
                100
            );
        firstCandidate.isAA = true;
        firstCandidate.airCoverageDelta = 50;
        firstCandidate.coverageProfile = [{
            threatIndex : 0,
            coverageDelta : 50,
            staticWeight : 1
        }];
        var firstPlan = CounterpointCharacterization.plannerValidTestPlan(
            0,
            0,
            firstCandidate
        );
        firstPlan.reservedBudget = 0;
        firstPlan.selected = 0;
        firstPlan.executed = true;
        firstPlan.complete = true;
        var duplicate = JSON.parse(JSON.stringify(firstCandidate));
        var alternative =
            CounterpointCharacterization.plannerMarginalCandidate(
                "serialized-alternative",
                1000,
                80
            );
        var secondPlan = CounterpointCharacterization.plannerValidTestPlan(
            1,
            0,
            duplicate
        );
        secondPlan.candidates.push(alternative);
        secondPlan.order = [0, 1];
        COUNTERPOINTAI._refreshPlanCostKinds(secondPlan);
        var state = CounterpointCharacterization.plannerTestState([
            firstPlan,
            secondPlan
        ]);
        state.dynamicBaseline = CounterpointCharacterization.plannerTestBaseline();
        state.dynamicBaseline.threats = [{
            key : "1:serialized-threat",
            id : "serialized-threat",
            need : 50,
            baseCoverage : 0,
            isAirThreat : true
        }];
        state.dynamicBaseline.airNeed = 50;
        state.dynamicBaseline.attackAirShare = 1;
        var projectedBefore = COUNTERPOINTAI._projectedTurnContext(state);
        if (!COUNTERPOINTAI._savePlannerState(system, state))
        {
            return { roundTrip : false, countedOnce : false };
        }
        var loaded = COUNTERPOINTAI._loadPlannerState(system);
        if (loaded === null)
        {
            return { roundTrip : false, countedOnce : false };
        }
        var projectedAfter = COUNTERPOINTAI._projectedTurnContext(loaded);
        var originalSelection = JSON.parse(JSON.stringify(state));
        var loadedSelection = JSON.parse(JSON.stringify(loaded));
        var originalSelected = COUNTERPOINTAI._selectPlanCandidate(
            originalSelection,
            1,
            1000,
            originalSelection.turnTargets
        );
        var loadedSelected = COUNTERPOINTAI._selectPlanCandidate(
            loadedSelection,
            1,
            1000,
            loadedSelection.turnTargets
        );
        return {
            roundTrip : JSON.stringify(projectedBefore) ===
                    JSON.stringify(projectedAfter) &&
                originalSelected && loadedSelected &&
                originalSelection.plans[1].selected ===
                    loadedSelection.plans[1].selected,
            countedOnce : projectedAfter.builtThisTurnById[
                    "#serialized-aa"
                ] === 1 &&
                projectedAfter.countsById["#serialized-aa"] === 1 &&
                projectedAfter.airCoverage === 50
        };
    },

    plannerLateRescanRegression : function()
    {
        var candidate = CounterpointCharacterization.plannerMarginalCandidate(
            "rescan-unit",
            1000,
            100
        );
        var state = CounterpointCharacterization.plannerTestState([
            CounterpointCharacterization.plannerExecutedTestPlan(
                "rescan-executed",
                candidate
            )
        ]);
        state.dynamicBaseline.fieldedCounts = [{
            id : "rescan-unit",
            count : 1
        }];
        var before = JSON.stringify(state.dynamicBaseline);
        COUNTERPOINTAI._ensureDynamicBaseline(state, {
            staticThreats : [],
            ownComposition : [
                { id : "rescan-unit" },
                { id : "rescan-unit" }
            ],
            ownAirCoverage : 0,
            enemyComposition : [],
            threatProfile : {}
        });
        var projected = COUNTERPOINTAI._projectedTurnContext(state);
        return JSON.stringify(state.dynamicBaseline) === before &&
            projected.countsById["#rescan-unit"] === 2 &&
            projected.builtThisTurnById["#rescan-unit"] === 1;
    },

    plannerPolicyRegressions : function()
    {
        var capper = CounterpointCharacterization.plannerTestCandidate(
            "turn-one-capper",
            1000,
            true
        );
        capper.turnOnePriority = true;
        var combat = CounterpointCharacterization.plannerMarginalCandidate(
            "turn-one-combat",
            1000,
            1000
        );
        var capperPlan = CounterpointCharacterization.plannerTestPlan(
            "turn-one-plan",
            1000,
            capper
        );
        capperPlan.candidates.push(combat);
        capperPlan.order = [1, 0];
        COUNTERPOINTAI._refreshPlanCostKinds(capperPlan);
        var capperState =
            CounterpointCharacterization.plannerTestState([capperPlan]);
        var capperSelected = COUNTERPOINTAI._selectPlanCandidate(
            capperState,
            0,
            1000,
            capperState.turnTargets
        ) && capperPlan.selected === 0;

        var ferry = CounterpointCharacterization.plannerTestCandidate(
            "urgent-ferry",
            1000,
            false
        );
        ferry.isTransporter = true;
        ferry.isDirectCombat = false;
        ferry.urgentFerry = true;
        var ferryCombat = CounterpointCharacterization.plannerMarginalCandidate(
            "urgent-ferry-combat",
            1000,
            1000
        );
        var ferryPlan = CounterpointCharacterization.plannerTestPlan(
            "urgent-ferry-plan",
            1000,
            ferry
        );
        ferryPlan.candidates.push(ferryCombat);
        ferryPlan.order = [1, 0];
        COUNTERPOINTAI._refreshPlanCostKinds(ferryPlan);
        var ferryState =
            CounterpointCharacterization.plannerTestState([ferryPlan]);
        var ferrySelected = COUNTERPOINTAI._selectPlanCandidate(
            ferryState,
            0,
            1000,
            ferryState.turnTargets
        ) && ferryPlan.selected === 0;

        var negative = CounterpointCharacterization.plannerMarginalCandidate(
            "negative-combat",
            1000,
            -100
        );
        var positive = CounterpointCharacterization.plannerMarginalCandidate(
            "positive-combat",
            1000,
            10
        );
        var scorerPlan = CounterpointCharacterization.plannerTestPlan(
            "negative-plan",
            1000,
            negative
        );
        scorerPlan.candidates.push(positive);
        scorerPlan.order = [0, 1];
        COUNTERPOINTAI._refreshPlanCostKinds(scorerPlan);
        var scorerState =
            CounterpointCharacterization.plannerTestState([scorerPlan]);
        var negativeSkipped = COUNTERPOINTAI._selectPlanCandidate(
            scorerState,
            0,
            1000,
            scorerState.turnTargets
        ) && scorerPlan.selected === 1;

        var builtIndirect =
            CounterpointCharacterization.plannerMarginalCandidate(
                "hard-cap-first",
                1000,
                100
            );
        builtIndirect.isIndirect = true;
        var cappedIndirect =
            CounterpointCharacterization.plannerMarginalCandidate(
                "hard-cap-second",
                1000,
                1000
            );
        cappedIndirect.isIndirect = true;
        var direct = CounterpointCharacterization.plannerMarginalCandidate(
            "hard-cap-direct",
            1000,
            10
        );
        var capPlan = CounterpointCharacterization.plannerTestPlan(
            "hard-cap-plan",
            1000,
            cappedIndirect
        );
        capPlan.candidates.push(direct);
        capPlan.order = [0, 1];
        COUNTERPOINTAI._refreshPlanCostKinds(capPlan);
        var capState = CounterpointCharacterization.plannerTestState([
            CounterpointCharacterization.plannerExecutedTestPlan(
                "hard-cap-executed",
                builtIndirect
            ),
            capPlan
        ]);
        capState.turnTargets.indirectRemaining = 1;
        var hardCap = COUNTERPOINTAI._selectPlanCandidate(
            capState,
            1,
            1000,
            capState.turnTargets
        ) && capPlan.selected === 1;

        var previousReserve = COUNTERPOINTAI.RESERVE_BLOCKED_FACTORIES;
        COUNTERPOINTAI.RESERVE_BLOCKED_FACTORIES = true;
        var blockedBuilding = {
            getOwnerID : function() { return 0; },
            getX : function() { return 4; },
            getY : function() { return 4; },
            getActionList : function()
            {
                return [COUNTERPOINTAI.ACTION_BUILD_UNITS];
            },
            getTerrain : function()
            {
                return { getUnit : function() { return {}; } };
            }
        };
        var blockedSystem = {
            getProductionActionData : function()
            {
                return {
                    getUnitIds : function() { return ["blocked-capper"]; },
                    getTransactionCosts : function() { return [1000]; }
                };
            },
            getDummyUnit : function()
            {
                return {
                    canCapture : function() { return true; },
                    getUnitType : function()
                    {
                        return GameEnums.UnitType_Ground;
                    }
                };
            }
        };
        var blockedStatus = COUNTERPOINTAI._blockedProductionStatus(
            blockedSystem,
            {
                getPlayer : function()
                {
                    return { getPlayerID : function() { return 0; } };
                }
            },
            [blockedBuilding],
            CounterpointCharacterization.plannerTestState([]),
            []
        );
        COUNTERPOINTAI.RESERVE_BLOCKED_FACTORIES = previousReserve;
        return {
            capper : capperSelected,
            ferry : ferrySelected,
            negative : negativeSkipped,
            hardCap : hardCap,
            blockedReserve : blockedStatus.count === 1 &&
                blockedStatus.capperReserve === 1000
        };
    },

    plannerDeploymentRegressions : function(system, map)
    {
        var nodeLimit = map.getMapWidth() * map.getMapHeight();
        var rearTurns = system.estimateCounterpointDeploymentTurns(
            0,
            0,
            "NEOTANK",
            map.getMapWidth() - 1,
            map.getMapHeight() - 1,
            1,
            1,
            false,
            nodeLimit
        );
        var frontTurns = system.estimateCounterpointDeploymentTurns(
            map.getMapWidth() - 3,
            map.getMapHeight() - 2,
            "NEOTANK",
            map.getMapWidth() - 1,
            map.getMapHeight() - 1,
            1,
            1,
            false,
            nodeLimit
        );
        var baselineState =
            CounterpointCharacterization.plannerTestState([]);
        var projected = COUNTERPOINTAI._projectedTurnContext(baselineState);
        var rear = CounterpointCharacterization.plannerMarginalCandidate(
            "factory-specific",
            22000,
            300
        );
        rear.deploymentKnown = rearTurns > 0;
        rear.deploymentTurns = Math.max(0, rearTurns);
        var front = JSON.parse(JSON.stringify(rear));
        front.deploymentKnown = frontTurns > 0;
        front.deploymentTurns = Math.max(0, frontTurns);
        var rearUtility = COUNTERPOINTAI._evaluateMarginalPurchase(
            rear,
            null,
            projected,
            rear.transactionCost
        );
        var frontUtility = COUNTERPOINTAI._evaluateMarginalPurchase(
            front,
            null,
            projected,
            front.transactionCost
        );

        var originalMissileInit = Global.MISSILE.init;
        var originalTankInit = Global.LIGHT_TANK.init;
        var tireTurns = -1;
        var tankTurns = -1;
        try
        {
            Global.MISSILE.init = function(unit)
            {
                originalMissileInit(unit);
                unit.setBaseMovementPoints(5);
            };
            Global.LIGHT_TANK.init = function(unit)
            {
                originalTankInit(unit);
                unit.setBaseMovementPoints(5);
            };
            tireTurns = system.estimateCounterpointDeploymentTurns(
                0,
                1,
                "MISSILE",
                map.getMapWidth() - 1,
                1,
                1,
                1,
                false,
                nodeLimit
            );
            tankTurns = system.estimateCounterpointDeploymentTurns(
                0,
                1,
                "LIGHT_TANK",
                map.getMapWidth() - 1,
                1,
                1,
                1,
                false,
                nodeLimit
            );
        }
        finally
        {
            Global.MISSILE.init = originalMissileInit;
            Global.LIGHT_TANK.init = originalTankInit;
        }
        var movedIndirect = system.estimateCounterpointDeploymentTurns(
            0,
            2,
            "ARTILLERY",
            map.getMapWidth() - 1,
            2,
            2,
            3,
            true,
            nodeLimit
        );
        var movedDirect = system.estimateCounterpointDeploymentTurns(
            0,
            2,
            "ARTILLERY",
            map.getMapWidth() - 1,
            2,
            2,
            3,
            false,
            nodeLimit
        );
        var alreadyInRange = system.estimateCounterpointDeploymentTurns(
            map.getMapWidth() - 3,
            2,
            "ARTILLERY",
            map.getMapWidth() - 1,
            2,
            2,
            3,
            true,
            nodeLimit
        );
        var unknown = system.estimateCounterpointDeploymentTurns(
            0,
            1,
            "NEOTANK",
            map.getMapWidth() - 1,
            1,
            1,
            1,
            false,
            1
        );
        var fallback = CounterpointCharacterization.plannerMarginalCandidate(
            "unknown-path",
            10000,
            100
        );
        fallback.intrinsicMobilityFactor = 0.6;
        fallback.deploymentKnown = false;
        var fallbackEvaluation = COUNTERPOINTAI._evaluateMarginalPurchase(
            fallback,
            null,
            projected,
            fallback.transactionCost
        );
        return {
            factorySpecific : rearTurns > frontTurns && frontTurns > 0 &&
                frontUtility.purchaseUtility > rearUtility.purchaseUtility,
            terrainMovement : tireTurns > tankTurns && tankTurns > 0,
            indirectSetup : movedIndirect === movedDirect + 1 &&
                movedDirect > 0 && alreadyInRange === 1,
            unknownFallback : unknown === COUNTERPOINTAI.DEPLOYMENT_UNKNOWN &&
                isFinite(fallbackEvaluation.adjustedCombatScore) &&
                isFinite(fallbackEvaluation.purchaseUtility) &&
                CounterpointCharacterization.almostEqual(
                    fallbackEvaluation.adjustedCombatScore,
                    fallback.baseCombatScore *
                        fallback.intrinsicMobilityFactor
                )
        };
    },

    characterizeCounterpointProduction : function(menu)
    {
        var selection = CounterpointCharacterization.selectedMap(menu);
        var map = selection.getMap();
        CounterpointCharacterization.assertValue(
            map.getMapWidth() >= PLANNER_MINIMUM_MAP_WIDTH &&
                map.getMapHeight() >= PLANNER_MINIMUM_MAP_HEIGHT,
            "planner-map-size"
        );
        CounterpointCharacterization.clearMap(map);
        var firstPlayer = map.getPlayer(FIRST_PLAYER);
        var secondPlayer = map.getPlayer(SECOND_PLAYER);
        firstPlayer.setTeam(FIRST_PLAYER);
        secondPlayer.setTeam(SECOND_PLAYER);
        firstPlayer.setBuildList(PLANNER_BUILD_LIST);
        secondPlayer.setBuildList(PLANNER_BUILD_LIST);
        firstPlayer.setFunds(PLANNER_STARTING_FUNDS);
        secondPlayer.setFunds(PLANNER_STARTING_FUNDS);

        FACTORY.actionList = ["ACTION_BUILD_UNITS", DISCOVERY_UNSUPPORTED_ACTION];
        FACTORY.getCostReduction = function(building, unitId, baseCost, x, y, currentMap)
        {
            return DISCOVERY_BUILDING_COST_DELTA;
        };
        var rules = map.getGameRules();
        rules.setUnitLimit(0);
        var allowedActions = rules.getAllowedActions();
        if (allowedActions.indexOf(DISCOVERY_UNSUPPORTED_ACTION) < 0)
        {
            allowedActions.push(DISCOVERY_UNSUPPORTED_ACTION);
        }
        rules.setAllowedActions(allowedActions);

        var firstFactory = CounterpointCharacterization.replacePlannerBuilding(
            map,
            TEST_BUILDING,
            PLANNER_FACTORY_X,
            PLANNER_FACTORY_Y,
            firstPlayer
        );
        var firstAirport = CounterpointCharacterization.replacePlannerBuilding(
            map,
            PLANNER_AIRPORT,
            PLANNER_AIRPORT_X,
            PLANNER_AIRPORT_Y,
            firstPlayer
        );
        var nest = CounterpointCharacterization.replacePlannerBuilding(
            map,
            DISCOVERY_NEST,
            PLANNER_NEST_X,
            PLANNER_NEST_Y,
            firstPlayer
        );
        nest.setFireCount(DISCOVERY_BLACK_HOLE_DOOR1_ONLY);
        var blackHole = CounterpointCharacterization.replacePlannerBuilding(
            map,
            DISCOVERY_BLACK_HOLE,
            PLANNER_BLACK_HOLE_X,
            PLANNER_BLACK_HOLE_Y,
            firstPlayer
        );
        blackHole.setFireCount(DISCOVERY_BLACK_HOLE_READY);

        var secondFactoryX = map.getMapWidth() - 1;
        var secondAirportX = map.getMapWidth() - 1;
        var secondFactoryY = map.getMapHeight() - 1;
        var secondAirportY = map.getMapHeight() - 2;
        CounterpointCharacterization.replacePlannerBuilding(
            map,
            TEST_BUILDING,
            secondFactoryX,
            secondFactoryY,
            secondPlayer
        );
        CounterpointCharacterization.replacePlannerBuilding(
            map,
            PLANNER_AIRPORT,
            secondAirportX,
            secondAirportY,
            secondPlayer
        );

        selection.playerCO1Changed(TEST_CO, FIRST_PLAYER);
        selection.playerCO1Changed(TEST_CO, SECOND_PLAYER);
        selection.selectPlayerAi(FIRST_PLAYER, GameEnums.AiTypes_Normal);
        selection.selectPlayerAi(SECOND_PLAYER, GameEnums.AiTypes_Normal);
        var firstAi = firstPlayer.getBaseGameInput();
        var secondAi = secondPlayer.getBaseGameInput();
        var firstSystem = firstAi.getSimpleProductionSystem();
        var secondSystem = secondAi.getSimpleProductionSystem();
        CounterpointCharacterization.assertValue(
            firstSystem !== null && secondSystem !== null &&
                COUNTERPOINTAI.initializeSimpleProductionSystem(firstSystem, firstAi, map) &&
                COUNTERPOINTAI.initializeSimpleProductionSystem(secondSystem, secondAi, map),
            "planner-callback-surface"
        );

        var firstBuildings = firstPlayer.getBuildings();
        var firstUnits = firstPlayer.getUnits();
        var firstEnemyUnits = firstPlayer.getEnemyUnits();
        var firstEnemyBuildings = firstPlayer.getEnemyBuildings();
        var secondBuildings = secondPlayer.getBuildings();
        var secondUnits = secondPlayer.getUnits();
        var secondEnemyUnits = secondPlayer.getEnemyUnits();
        var secondEnemyBuildings = secondPlayer.getEnemyBuildings();
        CounterpointCharacterization.assertValue(
            counterpointTest.beginRandomProbe() &&
                COUNTERPOINTAI.prepareProduction(
                    firstSystem,
                    firstAi,
                    firstBuildings,
                    firstUnits,
                    firstEnemyUnits,
                    firstEnemyBuildings,
                    map
                ) &&
                COUNTERPOINTAI.onNewBuildQueue(
                    firstSystem,
                    firstAi,
                    firstBuildings,
                    firstUnits,
                    firstEnemyUnits,
                    firstEnemyBuildings,
                    map,
                    []
                ) &&
                COUNTERPOINTAI.prepareProduction(
                    secondSystem,
                    secondAi,
                    secondBuildings,
                    secondUnits,
                    secondEnemyUnits,
                    secondEnemyBuildings,
                    map
                ) &&
                COUNTERPOINTAI.onNewBuildQueue(
                    secondSystem,
                    secondAi,
                    secondBuildings,
                    secondUnits,
                    secondEnemyUnits,
                    secondEnemyBuildings,
                    map,
                    []
                ) &&
                counterpointTest.finishRandomProbe(),
            "planner-global-rng-isolation"
        );

        var firstState = COUNTERPOINTAI._loadPlannerState(firstSystem);
        var secondState = COUNTERPOINTAI._loadPlannerState(secondSystem);
        CounterpointCharacterization.assertValue(
            firstState !== null && secondState !== null &&
                firstState.playerId === FIRST_PLAYER &&
                secondState.playerId === SECOND_PLAYER &&
                firstState.seed !== secondState.seed &&
                firstState.specialPrepared && firstState.ordinaryPrepared &&
                secondState.specialPrepared && secondState.ordinaryPrepared,
            "planner-player-isolation"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerSelectionsDeferred(firstState) &&
                CounterpointCharacterization.plannerSelectionsDeferred(secondState),
            "planner-deferred-selection"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerHasDeferredUnaffordableCandidate(),
            "planner-unaffordable-candidate-order"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerRecycledBudgetUnlocks(),
            "planner-recycled-budget-unlocks"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerFundsCheapestFloorsFirst(),
            "planner-cheapest-floors-first"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerFloorCountsGroundCapturersOnly(),
            "planner-floor-ground-capturers-only"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerFloorIgnoresAffordabilityFlag(),
            "planner-floor-ignores-affordability"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerFerryHullsAreCapped(),
            "planner-ferry-hulls-capped"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerCarrierNeedsGroundCargo(),
            "planner-carrier-needs-ground-cargo"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerRosterReadsAllFacilities(),
            "planner-roster-all-facilities"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerSatisfiedFerryRefused(),
            "planner-satisfied-ferry-refused"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerFerrySavingCases(),
            "planner-ferry-saving-cases"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerCounterSavingCases(),
            "planner-counter-saving-cases"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerFerryOutranksCounter(),
            "planner-ferry-outranks-counter"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerSkippedFerryMoneyStaysBanked(),
            "planner-skip-banks-ferry-money"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerFerryBindingPromotes(),
            "planner-ferry-binding-promotes"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerOrdinaryRescanUnlocks(),
            "planner-ordinary-rescan-unlocks"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerFailedBorrowRestoresDonors(),
            "planner-failed-borrow-rollback"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerLiveCostKindsRefresh(),
            "planner-live-cost-kinds"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerAvoidBudgetBaseSkips(),
            "planner-avoid-budget-base-skips"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerDisabledCandidateRevives(),
            "planner-disabled-candidate-revival"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerPhaseActionValidation(),
            "planner-phase-action-validation"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerRejectsNonConservingBorrow(),
            "planner-borrow-state-validation"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerTerminalLiveFailureReleases(),
            "planner-terminal-live-failure"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerSpecialResultHandshake(),
            "planner-special-result-handshake"
        );
        var marginal =
            CounterpointCharacterization.plannerMarginalRegressions();
        CounterpointCharacterization.assertValue(
            marginal.duplicateDamped,
            "planner-same-turn-duplicate-damping"
        );
        CounterpointCharacterization.assertValue(
            marginal.duplicatePossible,
            "planner-duplicate-demand-survives"
        );
        CounterpointCharacterization.assertValue(
            marginal.projectedAA,
            "planner-projected-aa-coverage"
        );
        CounterpointCharacterization.assertValue(
            marginal.crossIndirect,
            "planner-cross-type-indirect-saturation"
        );
        CounterpointCharacterization.assertValue(
            marginal.exactIndirect,
            "planner-exact-indirect-stack"
        );
        var saving =
            CounterpointCharacterization.plannerCanonicalSavingRegressions();
        CounterpointCharacterization.assertValue(
            saving.immediate,
            "planner-counter-buys-better-now"
        );
        CounterpointCharacterization.assertValue(
            saving.heavy,
            "planner-counter-heavy-remains-valid"
        );
        var reconstruction =
            CounterpointCharacterization.plannerStateReconstructionRegressions();
        CounterpointCharacterization.assertValue(
            reconstruction.roundTrip,
            "planner-projected-round-trip"
        );
        CounterpointCharacterization.assertValue(
            reconstruction.countedOnce,
            "planner-executed-counted-once"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerLateRescanRegression(),
            "planner-late-rescan-baseline"
        );
        var policy = CounterpointCharacterization.plannerPolicyRegressions();
        CounterpointCharacterization.assertValue(
            policy.capper,
            "planner-turn-one-capturer-precedence"
        );
        CounterpointCharacterization.assertValue(
            policy.ferry,
            "planner-urgent-ferry-precedence"
        );
        CounterpointCharacterization.assertValue(
            policy.blockedReserve,
            "planner-blocked-capturer-reserve"
        );
        CounterpointCharacterization.assertValue(
            policy.negative,
            "planner-negative-scorer-skip"
        );
        CounterpointCharacterization.assertValue(
            policy.hardCap,
            "planner-max-indirect-cap"
        );
        var deployment =
            CounterpointCharacterization.plannerDeploymentRegressions(
                firstSystem,
                map
            );
        CounterpointCharacterization.assertValue(
            deployment.factorySpecific,
            "planner-factory-specific-deployment"
        );
        CounterpointCharacterization.assertValue(
            deployment.terrainMovement,
            "planner-terrain-aware-deployment"
        );
        CounterpointCharacterization.assertValue(
            deployment.indirectSetup,
            "planner-indirect-setup-delay"
        );
        CounterpointCharacterization.assertValue(
            deployment.unknownFallback,
            "planner-deployment-unknown-fallback"
        );

        var factoryData = CounterpointCharacterization.snapshotPlannerData(
            firstSystem.getProductionActionData(firstFactory, "ACTION_BUILD_UNITS")
        );
        var airportData = CounterpointCharacterization.snapshotPlannerData(
            firstSystem.getProductionActionData(firstAirport, "ACTION_BUILD_UNITS")
        );
        var factoryPlan = CounterpointCharacterization.findPlannerPlan(
            firstState,
            PLANNER_FACTORY_X,
            PLANNER_FACTORY_Y,
            "ACTION_BUILD_UNITS"
        );
        var airportPlan = CounterpointCharacterization.findPlannerPlan(
            firstState,
            PLANNER_AIRPORT_X,
            PLANNER_AIRPORT_Y,
            "ACTION_BUILD_UNITS"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization.plannerPlanMatchesData(factoryPlan, factoryData) &&
                CounterpointCharacterization.plannerPlanMatchesData(airportPlan, airportData) &&
                factoryData.ids.indexOf("INFANTRY") >= 0 &&
                factoryData.ids.indexOf("T_HELI") < 0 &&
                airportData.ids.indexOf("T_HELI") >= 0 &&
                airportData.ids.indexOf("INFANTRY") < 0,
            "planner-authoritative-actions"
        );
        var infantryIndex = factoryData.ids.indexOf("INFANTRY");
        CounterpointCharacterization.assertValue(
            infantryIndex >= 0 &&
                factoryData.costs[infantryIndex] ===
                    firstPlayer.getCosts("INFANTRY", firstFactory.getPosition()) &&
                factoryData.costs[infantryIndex] !== Global.INFANTRY.getBaseCost() &&
                factoryPlan.candidates[infantryIndex].transactionCost ===
                    factoryData.costs[infantryIndex],
            "planner-modified-cost"
        );

        var firstSpecialPlans = CounterpointCharacterization.plannerPhasePlans(
            firstState,
            COUNTERPOINTAI.PHASE_SPECIAL
        );
        var freePlansValid = firstSpecialPlans.length > 0;
        for (var freeIndex = 0; freeIndex < firstSpecialPlans.length; ++freeIndex)
        {
            freePlansValid = freePlansValid &&
                firstSpecialPlans[freeIndex].freeOnly &&
                !firstSpecialPlans[freeIndex].hasPaid &&
                firstSpecialPlans[freeIndex].reservedBudget === 0;
        }
        // Compare totals because mirrored plan order can change the per-plan split.
        CounterpointCharacterization.assertValue(
            freePlansValid &&
                CounterpointCharacterization.plannerPaidBudgetTotal(firstState) ===
                    CounterpointCharacterization.plannerPaidBudgetTotal(secondState),
            "planner-free-budget-isolation"
        );

        var blackHolePlans = [];
        for (var blackHoleIndex = 0; blackHoleIndex < firstState.plans.length; ++blackHoleIndex)
        {
            var blackHolePlan = firstState.plans[blackHoleIndex];
            if (blackHolePlan.x === PLANNER_BLACK_HOLE_X &&
                blackHolePlan.y === PLANNER_BLACK_HOLE_Y)
            {
                blackHolePlans.push(blackHolePlan.actionId);
            }
        }
        blackHolePlans.sort();
        CounterpointCharacterization.assertValue(
            blackHolePlans.join(",") ===
                [DISCOVERY_DOOR1, DISCOVERY_DOOR2, DISCOVERY_DOOR3].sort().join(","),
            "planner-black-hole-door-plans"
        );

        var firstStateJson = JSON.stringify(firstState);
        var secondStateJson = JSON.stringify(secondState);
        CounterpointCharacterization.assertValue(
            COUNTERPOINTAI.prepareProduction(
                firstSystem,
                firstAi,
                firstBuildings,
                firstUnits,
                firstEnemyUnits,
                firstEnemyBuildings,
                map
            ) &&
                COUNTERPOINTAI.onNewBuildQueue(
                    firstSystem,
                    firstAi,
                    firstBuildings,
                    firstUnits,
                    firstEnemyUnits,
                    firstEnemyBuildings,
                    map,
                    []
                ) &&
                JSON.stringify(COUNTERPOINTAI._loadPlannerState(firstSystem)) ===
                    firstStateJson,
            "planner-deterministic-state"
        );

        var isolatedMutation = JSON.parse(firstStateJson);
        isolatedMutation.drawCounter += 1;
        CounterpointCharacterization.assertValue(
            COUNTERPOINTAI._savePlannerState(firstSystem, isolatedMutation) &&
                JSON.stringify(COUNTERPOINTAI._loadPlannerState(secondSystem)) ===
                    secondStateJson &&
                COUNTERPOINTAI._savePlannerState(firstSystem, JSON.parse(firstStateJson)),
            "planner-player-isolation"
        );

        var profilesBounded = firstState.dynamicBaseline !== null &&
            firstState.dynamicBaseline.fieldedCounts.length <=
                COUNTERPOINTAI.MAX_DYNAMIC_BASELINE_COUNTS &&
            firstState.dynamicBaseline.threats.length <=
                COUNTERPOINTAI.MAX_DYNAMIC_BASELINE_THREATS;
        for (var boundedPlanIndex = 0;
             boundedPlanIndex < firstState.plans.length;
             ++boundedPlanIndex)
        {
            for (var boundedCandidateIndex = 0;
                 boundedCandidateIndex <
                    firstState.plans[boundedPlanIndex].candidates.length;
                 ++boundedCandidateIndex)
            {
                var boundedCandidate =
                    firstState.plans[boundedPlanIndex].candidates[
                        boundedCandidateIndex
                    ];
                profilesBounded = profilesBounded &&
                    boundedCandidate.coverageProfile.length <=
                        COUNTERPOINTAI.TOP_N_WEIGHTS.length &&
                    boundedCandidate.scoreData === undefined;
            }
        }
        CounterpointCharacterization.assertValue(
            firstState.plans.length <= COUNTERPOINTAI.MAX_PLAN_COUNT &&
                firstState.drawCounter <= COUNTERPOINTAI.MAX_PLANNER_DRAW_COUNT &&
                firstStateJson.length <= COUNTERPOINTAI.MAX_PLANNER_STATE_LENGTH &&
                profilesBounded &&
                counterpointTest.captureProductionSystem(firstSystem) &&
                counterpointTest.getCapturedProductionSystemSize() <
                    PLANNER_MAX_SERIALIZED_BYTES,
            "planner-bounded-state"
        );
        var changedState = JSON.parse(firstStateJson);
        changedState.seed = (changedState.seed + 1) >>> 0;
        changedState.drawCounter += 1;
        CounterpointCharacterization.assertValue(
            COUNTERPOINTAI._savePlannerState(firstSystem, changedState) &&
                counterpointTest.restoreProductionSystem(firstSystem) &&
                JSON.stringify(COUNTERPOINTAI._loadPlannerState(firstSystem)) ===
                    firstStateJson,
            "planner-round-trip"
        );

        blackHole.setFireCount(DISCOVERY_BLACK_HOLE_DOOR1_ONLY);
        var spentDoorData = firstSystem.getProductionActionData(
            blackHole,
            DISCOVERY_DOOR2
        );
        var spentDoorAction = {
            getTargetBuilding : function() { return blackHole; },
            getActionID : function() { return DISCOVERY_DOOR2; }
        };
        var spentDoorSelection = COUNTERPOINTAI.getFactoryMenuItem(
            firstAi,
            spentDoorAction,
            spentDoorData.getUnitIds(),
            spentDoorData.getTransactionCosts(),
            spentDoorData.getEnabledList(),
            firstUnits,
            firstBuildings,
            firstPlayer,
            map
        );
        var refreshedState = COUNTERPOINTAI._loadPlannerState(firstSystem);
        var refreshedDoorPlan = CounterpointCharacterization.findPlannerPlan(
            refreshedState,
            PLANNER_BLACK_HOLE_X,
            PLANNER_BLACK_HOLE_Y,
            DISCOVERY_DOOR2
        );
        CounterpointCharacterization.assertValue(
            spentDoorSelection === GameEnums.MenuSelection_Restart,
            "planner-handled-skip"
        );
        CounterpointCharacterization.assertValue(
            refreshedDoorPlan !== null && !refreshedDoorPlan.available,
            "planner-black-hole-door-plans"
        );

        var beforeBuildState = COUNTERPOINTAI._loadPlannerState(firstSystem);
        var beforeOrdinaryExecuted = Object.create(null);
        for (var beforePlanIndex = 0;
             beforePlanIndex < beforeBuildState.plans.length;
             ++beforePlanIndex)
        {
            var beforePlan = beforeBuildState.plans[beforePlanIndex];
            if (beforePlan.phase === COUNTERPOINTAI.PHASE_ORDINARY)
            {
                beforeOrdinaryExecuted[beforePlan.key] = beforePlan.executed;
            }
        }
        var buildResult = counterpointTest.beginProductionActionProbe(map, FIRST_PLAYER) &&
            COUNTERPOINTAI.buildUnitSimpleProductionSystem(
                firstSystem,
                firstAi,
                firstBuildings,
                firstUnits,
                firstEnemyUnits,
                firstEnemyBuildings,
                map
            );
        var afterBuildState = COUNTERPOINTAI._loadPlannerState(firstSystem);
        var completedPlan = null;
        for (var afterPlanIndex = 0;
             afterPlanIndex < afterBuildState.plans.length;
             ++afterPlanIndex)
        {
            var afterPlan = afterBuildState.plans[afterPlanIndex];
            if (afterPlan.phase === COUNTERPOINTAI.PHASE_ORDINARY &&
                !beforeOrdinaryExecuted[afterPlan.key] &&
                afterPlan.executed && afterPlan.complete)
            {
                completedPlan = afterPlan;
                break;
            }
        }
        CounterpointCharacterization.assertValue(
            buildResult && completedPlan !== null && completedPlan.selected >= 0 &&
                counterpointTest.finishProductionActionProbe(
                    completedPlan.x,
                    completedPlan.y,
                    completedPlan.candidates[completedPlan.selected].id
            ),
            "planner-exact-coordinate-build"
        );
        CounterpointCharacterization.assertValue(
            completedPlan !== null && completedPlan.executed,
            "planner-successful-build-tracking"
        );
        var unsupportedFound = false;
        for (var unsupportedIndex = 0;
             unsupportedIndex < afterBuildState.plans.length;
             ++unsupportedIndex)
        {
            unsupportedFound = unsupportedFound ||
                afterBuildState.plans[unsupportedIndex].actionId ===
                    DISCOVERY_UNSUPPORTED_ACTION;
        }
        CounterpointCharacterization.assertValue(
            !unsupportedFound && buildResult,
            "planner-unsupported-fallback"
        );
        firstPlayer.setFunds(PLANNER_STARTING_FUNDS);
        CounterpointCharacterization.replacePlannerBuilding(
            map,
            TEST_BUILDING,
            PLANNER_DUPLICATE_X,
            PLANNER_DUPLICATE_Y,
            firstPlayer
        );
        var originalBuildStepData = ACTION_BUILD_UNITS.getStepData;
        ACTION_BUILD_UNITS.getStepData = function(action, data, currentMap)
        {
            data.addData(
                "First infantry",
                TEST_UNIT,
                TEST_UNIT,
                PLANNER_DUPLICATE_FIRST_COST,
                true
            );
            data.addData(
                "Second infantry",
                TEST_UNIT,
                TEST_UNIT,
                PLANNER_DUPLICATE_SECOND_COST,
                true
            );
        };
        var duplicateIdentity = counterpointTest.executeCounterpointBuildTargets(
            map,
            FIRST_PLAYER,
            PLANNER_DUPLICATE_X,
            PLANNER_DUPLICATE_Y,
            TEST_UNIT,
            1,
            PLANNER_DUPLICATE_SECOND_COST
        );
        ACTION_BUILD_UNITS.getStepData = originalBuildStepData;
        CounterpointCharacterization.assertValue(
            duplicateIdentity,
            "planner-duplicate-menu-identity"
        );
        counterpointTest.finish(0);
    },

    characterizeProductionDiscovery : function(menu)
    {
        var selection = CounterpointCharacterization.selectedMap(menu);
        var map = selection.getMap();
        CounterpointCharacterization.assertValue(
            map.getMapWidth() >= MINIMUM_MAP_SIZE &&
                map.getMapHeight() >= DISCOVERY_MINIMUM_MAP_HEIGHT,
            "discovery-map-size"
        );
        CounterpointCharacterization.clearMap(map);
        var firstPlayer = map.getPlayer(FIRST_PLAYER);
        var secondPlayer = map.getPlayer(SECOND_PLAYER);
        firstPlayer.setTeam(FIRST_PLAYER);
        secondPlayer.setTeam(SECOND_PLAYER);
        firstPlayer.setBuildList(DISCOVERY_BUILD_LIST);
        firstPlayer.setFunds(DISCOVERY_STARTING_FUNDS);

        FACTORY.actionList = [
            "ACTION_BUILD_UNITS",
            DISCOVERY_ACTION,
            DISCOVERY_UNSUPPORTED_ACTION,
            DISCOVERY_NON_UNIT_ACTION
        ];
        FACTORY.getCostReduction = function(building, unitId, baseCost, x, y, currentMap)
        {
            return DISCOVERY_BUILDING_COST_DELTA;
        };

        var rules = map.getGameRules();
        rules.setUnitLimit(0);
        var allowedActions = rules.getAllowedActions();
        allowedActions.push(DISCOVERY_ACTION);
        allowedActions.push(DISCOVERY_UNSUPPORTED_ACTION);
        allowedActions.push(DISCOVERY_NON_UNIT_ACTION);
        rules.setAllowedActions(allowedActions);

        var factory = CounterpointCharacterization.replaceDiscoveryBuilding(map, TEST_BUILDING, firstPlayer);
        selection.playerCO1Changed("CO_COLIN", FIRST_PLAYER);
        selection.playerCO1Changed(TEST_CO, SECOND_PLAYER);
        selection.selectPlayerAi(FIRST_PLAYER, GameEnums.AiTypes_Normal);
        selection.selectPlayerAi(SECOND_PLAYER, GameEnums.AiTypes_Human);
        var system = firstPlayer.getBaseGameInput().getSimpleProductionSystem();
        CounterpointCharacterization.assertValue(system !== null, "discovery-system");

        i = DISCOVERY_GLOBAL_SENTINEL;
        var factoryData = CounterpointCharacterization.snapshotDiscoveryData(
            system.getProductionActionData(factory, "ACTION_BUILD_UNITS"),
            "ACTION_BUILD_UNITS"
        );
        var factoryUnitIds = factoryData.unitIds;
        var factoryCosts = factoryData.transactionCosts;
        var factoryStrategicValues = factoryData.strategicValues;
        var infantryIndex = factoryUnitIds.indexOf(TEST_UNIT);
        CounterpointCharacterization.assertValue(infantryIndex >= 0, "discovery-factory-infantry");
        CounterpointCharacterization.assertValue(
            factoryCosts[infantryIndex] === firstPlayer.getCosts(TEST_UNIT, factory.getPosition()) &&
                factoryCosts[infantryIndex] !== Global[TEST_UNIT].getBaseCost() &&
                factoryStrategicValues[infantryIndex] === factoryCosts[infantryIndex],
            "discovery-modified-cost"
        );
        CounterpointCharacterization.assertValue(
            factoryData.actionAvailable && factoryData.enabledList[infantryIndex],
            "discovery-factory-available"
        );
        CounterpointCharacterization.assertValue(
            counterpointTest.productionQueryPreservesState(
                map,
                FIRST_PLAYER,
                factory,
                "ACTION_BUILD_UNITS"
            ) && i === DISCOVERY_GLOBAL_SENTINEL,
            "discovery-read-only"
        );

        var repeatedFactoryData = CounterpointCharacterization.snapshotDiscoveryData(
            system.getProductionActionData(factory, "ACTION_BUILD_UNITS"),
            "ACTION_BUILD_UNITS"
        );
        CounterpointCharacterization.assertValue(
            repeatedFactoryData.unitIds.join(",") === factoryUnitIds.join(",") &&
                repeatedFactoryData.transactionCosts.join(",") === factoryCosts.join(",") &&
                repeatedFactoryData.enabledList.join(",") === factoryData.enabledList.join(","),
            "discovery-repeatable"
        );

        ACTION_BUILD_UNITS.getProductionMenuData = function(action, data, currentMap)
        {
            data.addData("Infantry", TEST_UNIT, TEST_UNIT, DISCOVERY_BASE_OVERRIDE_COST, true);
        };
        var overriddenBaseData = CounterpointCharacterization.snapshotDiscoveryData(
            system.getProductionActionData(factory, "ACTION_BUILD_UNITS"),
            "ACTION_BUILD_UNITS"
        );
        CounterpointCharacterization.assertValue(
            overriddenBaseData.unitIds.length === 1 &&
                overriddenBaseData.transactionCosts[0] === DISCOVERY_BASE_OVERRIDE_COST,
            "discovery-base-override-contract"
        );
        ACTION_BUILD_UNITS.getProductionMenuData = null;

        firstPlayer.setFunds(0);
        var noFundsData = CounterpointCharacterization.snapshotDiscoveryData(
            system.getProductionActionData(factory, "ACTION_BUILD_UNITS"),
            "ACTION_BUILD_UNITS"
        );
        CounterpointCharacterization.assertValue(
            !noFundsData.actionAvailable && !noFundsData.enabledList[infantryIndex],
            "discovery-funds-availability"
        );
        firstPlayer.setFunds(DISCOVERY_STARTING_FUNDS);

        rules.setUnitLimit(1);
        var limitUnit = map.spawnUnit(0, 1, TEST_UNIT, firstPlayer, 0, true);
        var unitLimitData = CounterpointCharacterization.snapshotDiscoveryData(
            system.getProductionActionData(factory, "ACTION_BUILD_UNITS"),
            "ACTION_BUILD_UNITS"
        );
        CounterpointCharacterization.assertValue(
            limitUnit !== null && !unitLimitData.actionAvailable,
            "discovery-unit-limit"
        );
        limitUnit.removeUnit(false);
        rules.setUnitLimit(0);

        var occupyingUnit = map.spawnUnit(DISCOVERY_X, DISCOVERY_Y, TEST_UNIT, firstPlayer, 0, true);
        var occupiedFactoryData = CounterpointCharacterization.snapshotDiscoveryData(
            system.getProductionActionData(factory, "ACTION_BUILD_UNITS"),
            "ACTION_BUILD_UNITS"
        );
        CounterpointCharacterization.assertValue(
            occupyingUnit !== null && !occupiedFactoryData.actionAvailable,
            "discovery-occupied-output"
        );
        occupyingUnit.removeUnit(false);

        var customData = CounterpointCharacterization.snapshotDiscoveryData(
            system.getProductionActionData(factory, DISCOVERY_ACTION),
            DISCOVERY_ACTION
        );
        CounterpointCharacterization.assertValue(
            customData.actionAvailable &&
                customData.unitIds.length === 1 &&
                customData.unitIds[0] === TEST_UNIT &&
                customData.transactionCosts[0] === DISCOVERY_CUSTOM_COST &&
                customData.strategicValues[0] === DISCOVERY_CUSTOM_COST,
            "discovery-custom-contract"
        );
        CounterpointCharacterization.assertValue(
            system.getProductionActionData(factory, DISCOVERY_UNSUPPORTED_ACTION) === null,
            "discovery-unsupported-action"
        );
        CounterpointCharacterization.assertValue(
            system.getProductionActionData(factory, DISCOVERY_NON_UNIT_ACTION) === null,
            "discovery-non-unit-menu"
        );
        CounterpointCharacterization.assertValue(
            system.getProductionActionData(factory, "ACTION_OPTIONS") === null,
            "discovery-action-membership"
        );

        var temporaryAirport = CounterpointCharacterization.replaceDiscoveryBuilding(
            map,
            DISCOVERY_TEMPORARY_AIRPORT,
            firstPlayer
        );
        CounterpointCharacterization.assertValue(
            temporaryAirport.getConstructionList().length > 0 &&
                system.getProductionActionData(temporaryAirport, "ACTION_BUILD_UNITS") === null,
            "discovery-temporary-airport"
        );
        var temporaryHarbour = CounterpointCharacterization.replaceDiscoveryBuilding(
            map,
            DISCOVERY_TEMPORARY_HARBOUR,
            firstPlayer
        );
        CounterpointCharacterization.assertValue(
            temporaryHarbour.getConstructionList().length > 0 &&
                system.getProductionActionData(temporaryHarbour, "ACTION_BUILD_UNITS") === null,
            "discovery-temporary-harbour"
        );

        var nest = CounterpointCharacterization.replaceDiscoveryBuilding(map, DISCOVERY_NEST, firstPlayer);
        nest.setFireCount(DISCOVERY_BLACK_HOLE_DOOR1_ONLY);
        var nestData = CounterpointCharacterization.snapshotFreeDiscoveryData(
            system.getProductionActionData(nest, DISCOVERY_NEST_ACTION),
            DISCOVERY_NEST_ACTION
        );
        CounterpointCharacterization.assertValue(nestData.actionAvailable, "discovery-nest-available");
        nest.setFireCount(0);
        var spentNestData = CounterpointCharacterization.snapshotFreeDiscoveryData(
            system.getProductionActionData(nest, DISCOVERY_NEST_ACTION),
            DISCOVERY_NEST_ACTION
        );
        CounterpointCharacterization.assertValue(
            !spentNestData.actionAvailable,
            "discovery-nest-fire-count"
        );

        var blackHole = CounterpointCharacterization.replaceDiscoveryBuilding(
            map,
            DISCOVERY_BLACK_HOLE,
            firstPlayer
        );
        blackHole.setFireCount(DISCOVERY_BLACK_HOLE_READY);
        var door1 = CounterpointCharacterization.snapshotFreeDiscoveryData(
            system.getProductionActionData(blackHole, DISCOVERY_DOOR1),
            DISCOVERY_DOOR1
        );
        var door2 = CounterpointCharacterization.snapshotFreeDiscoveryData(
            system.getProductionActionData(blackHole, DISCOVERY_DOOR2),
            DISCOVERY_DOOR2
        );
        var door3 = CounterpointCharacterization.snapshotFreeDiscoveryData(
            system.getProductionActionData(blackHole, DISCOVERY_DOOR3),
            DISCOVERY_DOOR3
        );
        CounterpointCharacterization.assertValue(
            door1.actionAvailable && door2.actionAvailable && door3.actionAvailable,
            "discovery-black-hole-available"
        );

        blackHole.setFireCount(DISCOVERY_BLACK_HOLE_DOOR1_ONLY);
        var door1Only = CounterpointCharacterization.snapshotFreeDiscoveryData(
            system.getProductionActionData(blackHole, DISCOVERY_DOOR1),
            DISCOVERY_DOOR1
        );
        var door2Spent = CounterpointCharacterization.snapshotFreeDiscoveryData(
            system.getProductionActionData(blackHole, DISCOVERY_DOOR2),
            DISCOVERY_DOOR2
        );
        var door3Spent = CounterpointCharacterization.snapshotFreeDiscoveryData(
            system.getProductionActionData(blackHole, DISCOVERY_DOOR3),
            DISCOVERY_DOOR3
        );
        CounterpointCharacterization.assertValue(
            door1Only.actionAvailable &&
                !door2Spent.actionAvailable &&
                !door3Spent.actionAvailable,
            "discovery-black-hole-shared-fire"
        );
        blackHole.setFireCount(DISCOVERY_BLACK_HOLE_READY);
        var door1Occupant = map.spawnUnit(
            DISCOVERY_X - 2,
            DISCOVERY_Y + 1,
            TEST_UNIT,
            firstPlayer,
            0,
            true
        );
        var occupiedDoor1 = CounterpointCharacterization.snapshotFreeDiscoveryData(
            system.getProductionActionData(blackHole, DISCOVERY_DOOR1),
            DISCOVERY_DOOR1
        );
        var openDoor2 = CounterpointCharacterization.snapshotFreeDiscoveryData(
            system.getProductionActionData(blackHole, DISCOVERY_DOOR2),
            DISCOVERY_DOOR2
        );
        var openDoor3 = CounterpointCharacterization.snapshotFreeDiscoveryData(
            system.getProductionActionData(blackHole, DISCOVERY_DOOR3),
            DISCOVERY_DOOR3
        );
        CounterpointCharacterization.assertValue(
            door1Occupant !== null &&
                !occupiedDoor1.actionAvailable &&
                openDoor2.actionAvailable &&
                openDoor3.actionAvailable,
            "discovery-black-hole-occupied-door"
        );
        door1Occupant.removeUnit(false);
        counterpointTest.finish(0);
    },

    isModeDispatchScenario : function()
    {
        return COUNTERPOINT_SCENARIO === "mode_dispatch_standard" ||
            COUNTERPOINT_SCENARIO === "mode_dispatch_counterpoint" ||
            COUNTERPOINT_SCENARIO === "mode_dispatch_excluded";
    },

    isCounterpointScenario : function()
    {
        return COUNTERPOINT_SCENARIO === "mode_dispatch_counterpoint" ||
            COUNTERPOINT_SCENARIO === "mode_dispatch_excluded";
    },

    isExcludedControllerScenario : function()
    {
        return COUNTERPOINT_SCENARIO === "mode_dispatch_excluded";
    },

    assertDispatchSurface : function(map)
    {
        var wrappers = [["NORMALAI", NORMALAI],
                        ["NORMALAIOFFENSIVE", NORMALAIOFFENSIVE],
                        ["NORMALAIDEFENSIVE", NORMALAIDEFENSIVE],
                        ["VERYEASYAI", VERYEASYAI]];
        for (var wrapperIndex = 0; wrapperIndex < wrappers.length; ++wrapperIndex)
        {
            var name = wrappers[wrapperIndex][0];
            var wrapper = wrappers[wrapperIndex][1];
            for (var seamIndex = 0; seamIndex < DISPATCH_SEAMS.length; ++seamIndex)
            {
                CounterpointCharacterization.assertValue(
                    typeof wrapper[DISPATCH_SEAMS[seamIndex]] === "function",
                    "dispatch-seam-" + name + "-" + DISPATCH_SEAMS[seamIndex]
                );
            }
            var menuFunctions = wrapper.buildingMenuFunctions;
            CounterpointCharacterization.assertValue(
                menuFunctions.length > 0,
                "dispatch-menu-table-" + name
            );
            var routed = true;
            for (var rowIndex = 0; rowIndex < menuFunctions.length; ++rowIndex)
            {
                routed = routed && menuFunctions[rowIndex][1] === AI_BEHAVIOR_DISPATCH.getFactoryMenuItem;
            }
            CounterpointCharacterization.assertValue(routed, "dispatch-menu-routed-" + name);
        }

        // Every wrapper used to scan NORMALAI's table, so a sentinel added to one must stay there.
        VERYEASYAI.buildingMenuFunctions.push([DISPATCH_SENTINEL_BUILDING, AI_BEHAVIOR_DISPATCH.getFactoryMenuItem]);
        var leaked = false;
        for (var leakIndex = 0; leakIndex < NORMALAI.buildingMenuFunctions.length; ++leakIndex)
        {
            leaked = leaked || NORMALAI.buildingMenuFunctions[leakIndex][0] === DISPATCH_SENTINEL_BUILDING;
        }
        VERYEASYAI.buildingMenuFunctions.pop();
        CounterpointCharacterization.assertValue(!leaked, "dispatch-menu-table-not-shared");

        CounterpointCharacterization.assertValue(
            typeof COREAI.getBuildingMenuItem !== "function",
            "dispatch-veryeasy-had-no-core-target"
        );
        CounterpointCharacterization.assertValue(
            typeof COREAI.prepareProduction !== "function" &&
                typeof COUNTERPOINTAI.prepareProduction === "function",
            "dispatch-prepare-counterpoint-only"
        );
        CounterpointCharacterization.assertValue(
            AI_BEHAVIOR_DISPATCH.getStrategy(null) === COREAI,
            "dispatch-null-map-standard"
        );

        var rules = map.getGameRules();
        rules.setAiBehaviorMode(GameEnums.AiBehavior_Standard);
        CounterpointCharacterization.assertValue(
            AI_BEHAVIOR_DISPATCH.getStrategy(map) === COREAI,
            "dispatch-standard-strategy"
        );
        rules.setAiBehaviorMode(GameEnums.AiBehavior_Counterpoint);
        CounterpointCharacterization.assertValue(
            AI_BEHAVIOR_DISPATCH.getStrategy(map) === COUNTERPOINTAI,
            "dispatch-counterpoint-strategy"
        );

        // Counterpoint initialization must not inherit the vanilla initial infantry queue.
        var coreInitializeCalls = 0;
        var originalCoreInitialize = COREAI.initializeSimpleProductionSystem;
        COREAI.initializeSimpleProductionSystem = function()
        {
            coreInitializeCalls += 1;
            return true;
        };
        var counterpointInit = COUNTERPOINTAI.initializeSimpleProductionSystem(null, null, map);
        COREAI.initializeSimpleProductionSystem = originalCoreInitialize;
        CounterpointCharacterization.assertValue(
            counterpointInit === true && coreInitializeCalls === 0,
            "dispatch-counterpoint-no-vanilla-init"
        );
    },

    assertAiProfile : function(ai, label, counterpoint)
    {
        var profile = counterpoint ? DISPATCH_COUNTERPOINT_PROFILE : DISPATCH_STANDARD_PROFILE;
        for (var index = 0; index < profile.length; ++index)
        {
            CounterpointCharacterization.assertValue(
                CounterpointCharacterization.almostEqual(
                    ai.getIniValue(profile[index][0], Number.NaN),
                    profile[index][1]
                ),
                "profile-" + label + "-" + profile[index][0]
            );
        }
    },

    _prepareCalls : 0,
    _prepareEnemySize : -1,

    // Record one prepareProduction call per turn with the full enemy army.
    installPrepareProductionRecorder : function(wrapper)
    {
        CounterpointCharacterization._prepareCalls = 0;
        CounterpointCharacterization._prepareEnemySize = -1;
        var originalPrepare = wrapper.prepareProduction;
        wrapper.prepareProduction = function(system, ai, buildings, units, enemyUnits, enemyBuildings, map)
        {
            CounterpointCharacterization._prepareCalls += 1;
            CounterpointCharacterization._prepareEnemySize =
                CounterpointCharacterization._collectionSize(enemyUnits);
            return originalPrepare(system, ai, buildings, units, enemyUnits, enemyBuildings, map);
        };
    },

    _collectionSize : function(collection)
    {
        return collection === null || collection === undefined ? -1 : collection.size();
    },

    characterizeModeDispatch : function(map)
    {
        var counterpoint = CounterpointCharacterization.isCounterpointScenario();
        var ai = map.getPlayer(FIRST_PLAYER).getBaseGameInput();
        CounterpointCharacterization.assertValue(ai !== null, "profile-controller-present");
        // One call per turn, so the C++ side is not re-planning on every process tick.
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization._prepareCalls === 1,
            "dispatch-prepare-once-per-turn"
        );
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization._prepareEnemySize === DISPATCH_HIDDEN_ENEMY_COUNT,
            "dispatch-prepare-unpruned-enemies"
        );

        // Recover the full army from the player after C++ distance pruning.
        var player = ai.getPlayer();
        CounterpointCharacterization.assertValue(
            CounterpointCharacterization._collectionSize(
                COUNTERPOINTAI._fullEnemyUnits(ai, null)) === DISPATCH_HIDDEN_ENEMY_COUNT &&
                COUNTERPOINTAI._fullEnemyUnits(null, player.getEnemyUnits()) !== null,
            "dispatch-queue-restores-full-army"
        );
        if (CounterpointCharacterization.isExcludedControllerScenario())
        {
            CounterpointCharacterization.assertValue(
                ai.getAiType() === GameEnums.AiTypes_NormalOffensive,
                "profile-offensive-controller"
            );
            CounterpointCharacterization.assertValue(
                ai.getLoadedIniCount(DISPATCH_OFFENSIVE_INI) === 1 &&
                    ai.getLoadedIniCount(DISPATCH_PROFILE_INI) === 0,
                "profile-offensive-excluded"
            );
            CounterpointCharacterization.assertValue(
                !CounterpointCharacterization.almostEqual(
                    ai.getIniValue("MinMovementDamage", Number.NaN),
                    DISPATCH_COUNTERPOINT_PROFILE[0][1]
                ),
                "profile-offensive-unchanged"
            );
        }
        else
        {
            CounterpointCharacterization.assertValue(
                ai.getAiType() === GameEnums.AiTypes_Normal,
                "profile-normal-controller"
            );
            // Exact counts prevent deserialization from duplicating the profile.
            CounterpointCharacterization.assertValue(
                ai.getLoadedIniCount(DISPATCH_NORMAL_INI) === 1 &&
                    ai.getLoadedIniCount(DISPATCH_PROFILE_INI) === (counterpoint ? 1 : 0),
                "profile-normal-ini-count"
            );
            CounterpointCharacterization.assertAiProfile(ai, "normal", counterpoint);
        }
        counterpointTest.finish(0);
    },

    // Wait until gameMenu so the inline first AI turn has unwound.
    prepareModeDispatch : function(menu)
    {
        var selection = CounterpointCharacterization.selectedMap(menu);
        var map = selection.getMap();
        CounterpointCharacterization.clearMap(map);

        var excluded = CounterpointCharacterization.isExcludedControllerScenario();
        var aiPlayer = map.getPlayer(FIRST_PLAYER);
        var humanPlayer = map.getPlayer(SECOND_PLAYER);
        aiPlayer.setTeam(FIRST_PLAYER);
        humanPlayer.setTeam(SECOND_PLAYER);
        map.replaceBuilding("HQ", 0, 0);
        map.getTerrain(0, 0).getBuilding().setOwner(aiPlayer);
        map.replaceBuilding("HQ", map.getMapWidth() - 1, map.getMapHeight() - 1);
        map.getTerrain(map.getMapWidth() - 1, map.getMapHeight() - 1).getBuilding().setOwner(humanPlayer);

        CounterpointCharacterization.assertDispatchSurface(map);
        map.getGameRules().setAiBehaviorMode(
            CounterpointCharacterization.isCounterpointScenario()
                ? GameEnums.AiBehavior_Counterpoint
                : GameEnums.AiBehavior_Standard);

        // Place the hidden enemy beyond the ordinary build-queue prune range.
        var enemy = map.spawnUnit(map.getMapWidth() - 1, map.getMapHeight() - 1, TEST_UNIT, humanPlayer, 0, true);
        CounterpointCharacterization.assertValue(enemy !== null, "dispatch-hidden-enemy-spawned");
        map.getGameRules().setFogMode(GameEnums.Fog_OfShroud);

        aiPlayer.setBuildList([TEST_UNIT]);
        selection.playerIncomeChanged(0, FIRST_PLAYER);
        selection.playerCO1Changed(TEST_CO, FIRST_PLAYER);
        selection.playerCO1Changed(TEST_CO, SECOND_PLAYER);
        selection.selectPlayerAi(
            FIRST_PLAYER,
            excluded ? GameEnums.AiTypes_NormalOffensive : GameEnums.AiTypes_Normal);
        selection.selectPlayerAi(SECOND_PLAYER, GameEnums.AiTypes_Human);
        CounterpointCharacterization.installPrepareProductionRecorder(
            excluded ? NORMALAIOFFENSIVE : NORMALAI);
        menu.startGame();
    },

    characterizeFog : function(menu)
    {
        var selection = CounterpointCharacterization.selectedMap(menu);
        var map = selection.getMap();
        CounterpointCharacterization.clearMap(map);
        var firstPlayer = map.getPlayer(FIRST_PLAYER);
        var secondPlayer = map.getPlayer(SECOND_PLAYER);
        firstPlayer.setTeam(FIRST_PLAYER);
        secondPlayer.setTeam(SECOND_PLAYER);

        var enemyX = map.getMapWidth() - 1;
        var enemyY = map.getMapHeight() - 1;
        var enemy = map.spawnUnit(enemyX, enemyY, TEST_UNIT, secondPlayer, 0, true);
        CounterpointCharacterization.assertValue(enemy !== null, "enemy-spawned");

        var rules = map.getGameRules();
        rules.setFogMode(GameEnums.Fog_OfShroud);
        rules.createFogVision();
        CounterpointCharacterization.assertValue(
            !firstPlayer.getFieldVisible(enemyX, enemyY),
            "hidden-enemy"
        );

        var fullSnapshot = firstPlayer.getEnemyUnits();
        var foundEnemy = false;
        for (var i = 0; i < fullSnapshot.size(); ++i)
        {
            var candidate = fullSnapshot.at(i);
            if (candidate.getX() === enemyX && candidate.getY() === enemyY)
            {
                foundEnemy = true;
                break;
            }
        }
        CounterpointCharacterization.assertValue(foundEnemy, "full-enemy-snapshot");

        var prunedSnapshot = firstPlayer.getEnemyUnits();
        var ownUnits = firstPlayer.getUnits();
        var ownBuildings = firstPlayer.getBuildings();
        prunedSnapshot.pruneEnemies(
            ownUnits,
            ownBuildings,
            SOURCE_BUILDING_DISTANCE,
            SOURCE_ENEMY_PRUNE_RANGE
        );
        CounterpointCharacterization.assertValue(
            prunedSnapshot.size() === 0,
            "distance-pruned"
        );
        counterpointTest.finish(0);
    }
};

var Init =
{
    main : function(menu)
    {
        if (COUNTERPOINT_SCENARIO === "counterpoint_strategy")
        {
            CounterpointCharacterization.runSafely(function()
            {
                CounterpointCharacterization.characterizeCounterpointStrategy();
            });
            return;
        }
        CounterpointCharacterization.configureSettings();
        CounterpointCharacterization.enterMapSelection(menu);
    },

    mapsSelection : function(menu)
    {
        CounterpointCharacterization.runSafely(function()
        {
            if (COUNTERPOINT_SCENARIO === "game_rules_v32" ||
                COUNTERPOINT_SCENARIO === "capture_game_rules_v32" ||
                COUNTERPOINT_SCENARIO === "game_rules_v33")
            {
                CounterpointCharacterization.characterizeRules(menu);
            }
            else if (COUNTERPOINT_SCENARIO === "fog_distance_prune")
            {
                CounterpointCharacterization.characterizeFog(menu);
            }
            else if (COUNTERPOINT_SCENARIO === "production_action_discovery")
            {
                CounterpointCharacterization.characterizeProductionDiscovery(menu);
            }
            else if (COUNTERPOINT_SCENARIO === "counterpoint_production")
            {
                CounterpointCharacterization.characterizeCounterpointProduction(menu);
            }
            else if (COUNTERPOINT_SCENARIO === "use_building_actions")
            {
                CounterpointCharacterization.prepareUseBuildingActions(menu);
            }
            else if (CounterpointCharacterization.isModeDispatchScenario())
            {
                CounterpointCharacterization.prepareModeDispatch(menu);
            }
            else
            {
                CounterpointCharacterization.prepareProduction(menu);
            }
        });
    },

    // Dispatch assertions wait for gameMenu after the inline first AI turn.
    gameMenu : function(menu)
    {
        if (CounterpointCharacterization.isModeDispatchScenario())
        {
            CounterpointCharacterization.runSafely(function()
            {
                CounterpointCharacterization.characterizeModeDispatch(menu.getMap());
            });
            return;
        }
        if (!CounterpointCharacterization._productionArmed)
        {
            CounterpointCharacterization.finishFailure("production-hook-missed");
        }
    }
};
