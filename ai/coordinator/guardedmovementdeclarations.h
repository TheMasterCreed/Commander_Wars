#pragma once

#include "ai/coordinator/guardedcommanddeclarations.h"

namespace Coordinator::MovementDeclaration
{
    using CommandDeclaration::Function;
    using CommandDeclaration::String;

    struct Integer { std::uint32_t index; std::int32_t value; };
    struct Module
    {
        std::string_view global, path;
        std::span<const Function> helpers;
        std::span<const String> strings;
        std::span<const Integer> integers;
    };

    struct Closure
    {
        Function wrapper, creator;
        std::uint32_t wrapperIndex, creatorIndex, slot, locals;
    };

    // custom_gamemodes/scripts/general/cmode_procedural_frontier.js, SHA-256 2c6e444bcfe8aad3aba3fd0250e655497ac9f2ff93c916f3079e993596984094.
    inline constexpr std::array FRONTIER_HELPERS{
        Function{"getRule", 215, "03e1c7dd97b26f5eb4d6af4cd04c330535e6ee46635087f02f329aa03592d3c5"},
        Function{"isEnabled", 250, "f0028b5edbcbfb9cb1d17e4a8f5f2711c9e3b93664a9f8758ded143af8e59815"},
        Function{"onUnitPostAction", 184, "f8a8023d8d413a531cbb55b9764dbffc25e4d250e42dd94a9535c95be6bcaa01"}};
    inline constexpr std::array FRONTIER_STRINGS{
        String{0, ""}, String{1, "CMODE_PROCGEN"}, String{2, "CMODE_PROCEDURAL_FRONTIER"},
        String{20, "undefined"}, String{22, "function"}, String{37, "length"},
        String{58, "getGameRules"}, String{59, "hasVictoryRule"}, String{60, "RULE_ID"},
        String{61, "getVictoryRule"}, String{62, "getRule"}, String{63, "getRuleValue"},
        String{64, "ITEM_ENABLED"}, String{65, "getVariables"}, String{66, "getVariable"},
        String{67, "INITIALIZED_KEY"}, String{68, "readDataBool"}, String{137, "installPatches"},
        String{148, "Object"}, String{149, "prototype"}, String{150, "hasOwnProperty"},
        String{151, "call"}, String{181, "Global"}, String{185, "unitSpriteManager"},
        String{186, "getUnitsSorted"}, String{218, "isEnabled"}, String{220, "exploreFromUnit"},
        String{221, "PLAYER"}, String{222, "PATCH_SENTINEL"}, String{223, "startOfTurn"},
        String{225, "UNIT"}, String{226, "postAction"}, String{227, "onUnitPostAction"},
        String{240, "map"}, String{249, "unit"}, String{260, "action"},
        String{261, "originalStart"}, String{262, "originalUnitPostAction"}};
    inline constexpr Module FRONTIER{"CMODE_PROCGEN", "/mods/custom_gamemodes/scripts/general/cmode_procedural_frontier.js",
        FRONTIER_HELPERS, FRONTIER_STRINGS, {}};
    inline constexpr Closure FRONTIER_POST{
        Function{"postAction", 208, "6852e223af3b9584d7a78bf11d73e14e45476b844c71e5428b183de839f2e8e1"},
        Function{"creator", 733, "aeb85b9cf9df2eb3ce9f49dcc6c1141de39b6d755590a8886cc9f2b6fe2030ba"},
        52, 50, 1, 2};

    // doctrine_frontiers/scripts/general/WardenBarrage.js, SHA-256 8a68eb6c9c7943f1055e1a94432a78e667a7558522bf9baa3358c356bdf3c5ec.
    inline constexpr std::array WARDEN_HELPERS{
        Function{"getMovementFields", 463, "bab795b02fd29598b7e1522dc31a05f454c36727ebce8c323ce9f0098d8c964a"},
        Function{"getPreviousMovementField", 280, "0deef597ffcd36ae5ff10836dbfd44653054047870c30fb3d3c6c8c5fe8fc39f"},
        Function{"isHardStopAt", 632, "fc1d0577716e74b044b6b36b8067a77b648e917b2dbd92c89e15ac504a4870be"},
        Function{"readBarrages", 160, "78dd38775f0dfdf3325854e13a307e596003b116089366996cdfaad0f669df18"},
        Function{"resolveMovement", 344, "0d6c7ff0d2eafaf251dfd951792779691a8f8f550556debd8ce273a7512c6f35"},
        Function{"triggerField", 561, "7efef667f607288e8356c48fdc854ba474211c67ee84f0ed23b82c0976cd90e9"}};
    inline constexpr std::array WARDEN_STRINGS{
        String{0, ""}, String{1, "WARDEN_BARRAGE"}, String{16, "WARDEN_BARRAGE_VARIABLE"},
        String{28, "getVariables"}, String{29, "getVariable"}, String{30, "readDataListString"},
        String{36, "length"}, String{38, "sourceId"}, String{41, "superZone"},
        String{42, "weaponMask"}, String{45, "push"}, String{46, "Qt"},
        String{47, "point"}, String{49, "x"}, String{50, "y"},
        String{51, "readBarrages"}, String{52, "decodeBarrage"}, String{77, "getPlayerCount"},
        String{78, "getPlayer"}, String{79, "getUnit"}, String{83, "Object"},
        String{84, "keys"}, String{86, "Global"}, String{87, "object"},
        String{92, "function"}, String{95, "getMovePath"}, String{98, "isEnemyUnit"},
        String{99, "containsInnerField"}, String{100, "getHp"}, String{101, "getOwner"},
        String{102, "hasCompatibleWeapon"}, String{103, "getActionID"}, String{104, "ACTION_TRAP"},
        String{105, "startReading"}, String{106, "readDataInt32"}, String{107, "ACTION_FIRE"},
        String{108, "getMovementFields"}, String{109, "containsField"}, String{112, "resolvedAction"},
        String{113, "triggerField"}, String{114, "fire"}, String{141, "hooksInstalled"},
        String{142, "UNIT"}, String{143, "postAction"}, String{144, "resolveMovement"},
        String{145, "doWalkingAnimation"}, String{146, "wrapUnitPostAction"}, String{147, "isTrap"},
        String{148, "getPreviousMovementField"}, String{149, "isHardStopAt"}, String{150, "calcBattleDamage4"},
        String{155, "getStepData"}, String{156, "perform"}, String{164, "installHooks"},
        String{168, "player"}, String{176, "map"}, String{177, "action"},
        String{178, "fallbackX"}, String{179, "fallbackY"}, String{181, "moveUnit"},
        String{182, "previousX"}, String{183, "previousY"}, String{184, "targetX"},
        String{185, "targetY"}, String{187, "previousBasePostAction"}, String{188, "previousFireDamage"},
        String{189, "previousFirePerform"}, String{190, "previousFireStepData"}, String{191, "previousIsTrap"},
        String{192, "unit"}, String{193, "targetFieldUnit"}, String{194, "moveCost"}};
    inline constexpr Module WARDEN{"WARDEN_BARRAGE", "/mods/doctrine_frontiers/scripts/general/WardenBarrage.js",
        WARDEN_HELPERS, WARDEN_STRINGS, {}};
    inline constexpr Closure WARDEN_POST{
        Function{"postAction", 221, "8a445f4bc8b78f272a5e9c4080b316f2d9dae275ef3d642d399805e0222fbd91"},
        Function{"creator", 676, "5a7debee9a4878c2ca3368dffe778adb72492cbe0ce29f04c0b8cb10c0f3d42b"},
        33, 32, 0, 5};
    inline constexpr Closure WARDEN_TRAP{
        Function{"isTrap", 338, "4fc0a807359d99263b48897eb8658e81ba5f8485e5ff7cd8590829733862589f"}, WARDEN_POST.creator,
        34, WARDEN_POST.creatorIndex, 4, WARDEN_POST.locals};

    // custom_gamemodes/scripts/general/cmode_utils.js, SHA-256 c0e3a9279b612c01778b4c7fa2b1410a98b30d341f620d364739923d3fd71189.
    inline constexpr std::array CHESS_HELPERS{
        Function{"activeMode", 220, "bcc3555af17414a41028ca4d8491fca58681e32dafa5759c8ec246f1dcb59277"},
        Function{"isEnabled", 744, "de0e535efb446b1d35c5d881801ec6dff40312035783db97eab999af289e6a5f"}};
    inline constexpr std::array CHESS_STRINGS{
        String{0, ""}, String{2, "CMODE_CHESS_BASE"}, String{52, "undefined"},
        String{56, "length"}, String{87, "Global"}, String{89, "function"},
        String{131, "ACTION_BUILD_UNITS"}, String{132, "ACTION_NEXT_PLAYER"}, String{139, "_modes"},
        String{140, "id"}, String{141, "isEnabled"}, String{159, "getGameRules"},
        String{160, "peers"}, String{161, "hasVictoryRule"}, String{162, "getVictoryRule"},
        String{163, "getRuleValue"}, String{177, "installSharedPatches"}, String{262, "__cmode_chess_base_patched"},
        String{263, "__cmode_chess_base_build_patched"}, String{264, "UNIT"}, String{265, "postAction"},
        String{266, "call"}, String{267, "activeMode"}, String{268, "onUnitPostAction"},
        String{269, "_ZCO_NAMES"}, String{270, "perform"}, String{279, "map"},
        String{289, "mode"}, String{293, "unit"}, String{294, "action"},
        String{301, "origBuild"}, String{302, "origPerf"}, String{303, "origUnit"}};
    inline constexpr std::array CHESS_INTEGERS{Integer{12, 0}};
    inline constexpr Module CHESS{"CMODE_CHESS_BASE", "/mods/custom_gamemodes/scripts/general/cmode_utils.js",
        CHESS_HELPERS, CHESS_STRINGS, CHESS_INTEGERS};
    inline constexpr Closure CHESS_POST{
        Function{"postAction", 265, "cffa1ceddaff81f81369452b56018523221d06896b19071e027c87e5f2e70ba3"},
        Function{"creator", 714, "1ea303cfa23c1a0a7b6452c3d04e2cad1ffe188784b9574edb8759baff493b4f"},
        44, 43, 2, 3};

    // custom_gamemodes/scripts/gamerules/victory/cmode_chess_lite.js, SHA-256 5270498aa3f6df32be7ebdd66c25d6dccd88d6c42feffcc226b9a3d3b246d13e.
    inline constexpr std::array LITE_HELPERS{
        Function{"isEnabled", 93, "f73503dcdca6d0e5578caea6415b0fd92957e5682eac6f5773412cd00a5d1d73"}};
    inline constexpr std::array LITE_STRINGS{
        String{0, ""}, String{1, "CMODE_CHESS_LITE"}, String{27, "CMODE_CHESS_BASE"},
        String{33, "isEnabled"}, String{47, "map"}};
    inline constexpr Module LITE{"CMODE_CHESS_LITE", "/mods/custom_gamemodes/scripts/gamerules/victory/cmode_chess_lite.js",
        LITE_HELPERS, LITE_STRINGS, {}};

    // custom_gamemodes/scripts/gamerules/victory/cmode_chess_std.js, SHA-256 d40aa7f11fb825c5ce5e6a131b0d718948e6ec5dd4719d8ca995993d09ede306.
    inline constexpr std::array STD_HELPERS{
        Function{"isEnabled", 93, "8919ed878f10e896c43c02df74208b63b7894462d3c07fcf419ff50f14ef96d1"}};
    inline constexpr std::array STD_STRINGS{
        String{0, ""}, String{1, "CMODE_CHESS_STD"}, String{25, "CMODE_CHESS_BASE"},
        String{32, "isEnabled"}, String{46, "map"}};
    inline constexpr Module STD{"CMODE_CHESS_STD", "/mods/custom_gamemodes/scripts/gamerules/victory/cmode_chess_std.js",
        STD_HELPERS, STD_STRINGS, {}};

    // custom_gamemodes/scripts/gamerules/victory/cmode_chess_true.js, SHA-256 705f0fce882b1bc2a6032b0bbde4479f0a33d89bbdc1ccb5d3787e47eb4f75c1.
    inline constexpr std::array TRUE_CHESS_HELPERS{
        Function{"isEnabled", 93, "e634ea753ea08f169f0eb95cb302dcf86876ac424faff7c7c693bce864485690"}};
    inline constexpr std::array TRUE_CHESS_STRINGS{
        String{0, ""}, String{1, "CMODE_CHESS_TRUE"}, String{26, "CMODE_CHESS_BASE"},
        String{33, "isEnabled"}, String{47, "map"}};
    inline constexpr Module TRUE_CHESS{"CMODE_CHESS_TRUE", "/mods/custom_gamemodes/scripts/gamerules/victory/cmode_chess_true.js",
        TRUE_CHESS_HELPERS, TRUE_CHESS_STRINGS, {}};

    struct Mode
    {
        Module module;
        std::span<const std::size_t> peers;
    };
    inline constexpr std::array<std::size_t, 2> LITE_PEERS{1, 2};
    inline constexpr std::array<std::size_t, 1> TRUE_PEERS{1};
    inline constexpr std::array MODES{
        Mode{LITE, LITE_PEERS}, Mode{STD, {}}, Mode{TRUE_CHESS, TRUE_PEERS}};
}
