#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace Coordinator::CommandDeclaration
{
    // QV4 6.8.3, little-endian unit version 66. Each digest covers the full
    // compiled function record through the last instruction. The following
    // tables bind all referenced strings, formal names and literal shapes.
    // Approved UTF-8 sources, general/cmode_co_command.js and its victory rule:
    // 4868f249010479b7afb1b913e6d8184223773e2cac242f8b61024b1bddf06e70
    // 578dc84d9ff6ed5411e6cd5e2ad97a25e6b65907558a2cedfd610b004f6b6826
    struct Function
    {
        std::string_view name;
        std::uint32_t recordBytes;
        std::string_view sha256;
    };

    struct String
    {
        std::uint32_t index;
        std::string_view value;
    };

    struct Shape
    {
        std::uint32_t index;
        std::span<const std::uint32_t> members;
    };

    // AWDS infantry, pinned UTF-8 source e7227b32fc971f3844ae82f0cc931911c2c0b3f8f042117b8117b400790f74cc.
    inline constexpr Function INFANTRY_INIT{"init", 320, "71d7379d90fe09253c24b6272514a3ee9911ab8444810ba49bf755d1fee3deb8"};
    inline constexpr std::array INFANTRY_INIT_STRINGS{
        String{0, ""},
        String{3, "setAmmo1"},
        String{4, "setMaxAmmo1"},
        String{5, "setWeapon1ID"},
        String{6, "WEAPON_INFANTRY_MG"},
        String{7, "setAmmo2"},
        String{8, "setMaxAmmo2"},
        String{9, "setWeapon2ID"},
        String{10, "setFuel"},
        String{11, "setMaxFuel"},
        String{12, "setBaseMovementPoints"},
        String{13, "setMinRange"},
        String{14, "setMaxRange"},
        String{15, "setVision"},
        String{19, "unit"}};
    inline constexpr Function INFANTRY_COST{"getBaseCost", 74, "ccfbda5520de23776a75610d018d29d232ab4428514812455ae8fdd7f053da1b"};
    inline constexpr std::array INFANTRY_COST_STRINGS{
        String{0, ""}};
    inline constexpr std::array<std::int32_t, 6> INFANTRY_INIT_INTEGERS{-1, 0, 100, 3, 1, 2};

    // AWDS Light Tank, pinned UTF-8 source 02a7a1c8aa4530738765a0ce4f91c82cf759d5211d399ed0f4182cbf452a97cc.
    inline constexpr Function LIGHT_TANK_INIT{"init", 320, "e7518d75b9e067123aba6edffa7048d4763170e7738100e2b5ba833cf4cb5ef0"};
    inline constexpr std::array LIGHT_TANK_INIT_STRINGS{
        String{0, ""},
        String{3, "setAmmo1"},
        String{4, "setMaxAmmo1"},
        String{5, "setWeapon1ID"},
        String{6, "WEAPON_LIGHT_TANK_GUN"},
        String{7, "setAmmo2"},
        String{8, "setMaxAmmo2"},
        String{9, "setWeapon2ID"},
        String{10, "WEAPON_LIGHT_TANK_MG"},
        String{11, "setFuel"},
        String{12, "setMaxFuel"},
        String{13, "setBaseMovementPoints"},
        String{14, "setMinRange"},
        String{15, "setMaxRange"},
        String{16, "setVision"},
        String{20, "unit"}};
    inline constexpr Function LIGHT_TANK_COST{"getBaseCost", 74, "01303918e336e024c99e4f213d1abb451382e5c3a79bd653e7b9487449d9a7c2"};
    inline constexpr std::array LIGHT_TANK_COST_STRINGS{
        String{0, ""}};
    inline constexpr std::array<std::int32_t, 6> LIGHT_TANK_INIT_INTEGERS{9, -1, 70, 6, 1, 3};

    inline constexpr Function WEATHER_WRAPPER{"wrapped", 410, "82ce3ebfa625879260a5fdd5a7b20105ca93ca406e8e193445be8cc655cf26c5"};
    // Qt can assign the third formal name a different string-table slot.
    inline constexpr std::array WEATHER_CREATORS{
        Function{"ensureUnitWeatherObject", 429, "ea284bb03e3ad4d0b8f040400eca1d4d1e973e1430ea943ef9d2be4d0228b3f4"},
        Function{"ensureUnitWeatherObject", 429, "49e59b450b891357312ecd38ceaacd08101e31285f877510744d2aad72cd1cd5"}};
    // The command rule already authenticates getRule, isRuleEnabled and readInt.
    inline constexpr std::array WEATHER_HELPERS{
        Function{"getCoForLineage", 307, "b5567f7789a045ce2b448d8596e7467cb00a6ea289568bbfb8fdfcfd87bb0181"},
        Function{"getCoLineage", 340, "f37bc26c0b13a0bbee411d05c0d7d1ade62d807d3d301b1e93f28bd858bc5a30"},
        Function{"getDefaultLineage", 187, "44c9307cb77023dc54e6beee42917953fdeede1b44dec32f19ac447fffd71f51"},
        Function{"getEffectiveUnitOrigin", 493, "db04db1e38b6cd974f11adf618c3cda1929aa01e0ee6b289fbb7bf0cf6321203"},
        Function{"getUnitCommandCo", 371, "d3f58b388ff670ce44d12529bee4e5986250c9fee626675486966fac76225230"},
        Function{"isActive", 103, "76dea8c3507c6d76965b5f9867c0dfc0c336c6435353856ae1ae66e357d1c5dd"},
        Function{"probeWeatherImmune", 368, "d0de33aac8c280aaa006c36e3b0c44ad60520c886f3990f950d2db491d880dae"},
        Function{"readUnitOrigin", 256, "d692fb50e1c2fa0c0ca38fb2778e7da62e1f63bacb0fc4a45fb67be15cc4a1f3"}};
    inline constexpr std::array WEATHER_STRINGS{
        String{0, ""}, String{1, "CO_COMMAND"}, String{119, "Object"}, String{134, "undefined"},
        String{151, "function"}, String{217, "LINEAGE_1"}, String{218, "LINEAGE_2"}, String{219, "getGameRules"},
        String{220, "hasVictoryRule"}, String{221, "RULE_ID"}, String{222, "getVictoryRule"}, String{223, "isRuleEnabled"},
        String{224, "getRule"}, String{225, "getVariables"}, String{226, "getVariable"}, String{227, "RULE_VALUE_KEY"},
        String{228, "readDataInt32"}, String{229, "readInt"}, String{276, "isValidLineage"}, String{277, "getStoredCoLineage"},
        String{278, "getOwner"}, String{279, "getCO"}, String{283, "getCoLineage"}, String{284, "getEffectiveUnitOrigin"},
        String{285, "ownerId"}, String{286, "getPlayerID"}, String{287, "getCoForLineage"}, String{288, "lineage"},
        String{289, "isActive"}, String{290, "getUnitCommandCo"}, String{291, "readUnitOrigin"}, String{311, "player"},
        String{382, "UNIT_OWNER_KEY"}, String{383, "UNIT_LINEAGE_KEY"}, String{384, "pendingBuild"}, String{385, "isRealUnit"},
        String{386, "getDefaultLineage"}, String{411, "activeHookCo"}, String{494, "apply"}, String{524, "weatherProbeCo"},
        String{563, "call"}, String{564, "prototype"}, String{565, "hasOwnProperty"}, String{627, "unit"},
        String{633, "co"}, String{675, "key"}, String{705, "map"}, String{1015, "WRAPPER_KEY"},
        String{1067, "getWeatherImmune"}, String{1068, ".getWeatherImmune.CO_COMMAND"}, String{1069, "probeWeatherImmune"},
        String{1070, "ensureUnitWeatherObject"}, String{1384, "rule"}, String{1385, "object"}, String{1408, "objectId"},
        String{1409, "original"}, String{1411, "wrapped"}};

    inline constexpr Function REPAIR_CREATOR{"ensureRepairPatch", 320, "85fcf73947c1ed86506d18efe78c090859ad8c590fb3f54eb1249b7a07ef6d9e"};
    inline constexpr Function REPAIR_WRAPPER{"wrapped", 482, "f7d40c2d3b5bbbc7aef255b0581feb7bae5cd8314060fae653772217e4b0d2a4"};
    inline constexpr std::array REPAIR_STRINGS{
        String{0, ""}, String{1, "CO_COMMAND"}, String{125, "UNIT"}, String{134, "undefined"},
        String{151, "function"}, String{263, "getMap"}, String{402, "canSupportUnit"}, String{411, "activeHookCo"},
        String{494, "apply"}, String{627, "unit"}, String{1015, "WRAPPER_KEY"}, String{1106, "repairUnit"},
        String{1107, "UNIT.repairUnit.CO_COMMAND"}, String{1328, "ensureRepairPatch"}, String{1409, "original"},
        String{1411, "wrapped"}};

    inline constexpr std::array COMMAND_FUNCTIONS{
        Function{"ensureCoLineages", 584, "86a3267d522bacea5030dd1aab38cc824320c7843ecafd4f403123732ffed096"},
        Function{"ensureMatchObjects", 522, "e6f81d0676f3d0ee27c66cb9dac7a2c9f990f5998c861db2698f79a026ac8e18"},
        Function{"finishAction", 455, "2daf0fc855d76282595b0e79fb89d526848045334acf78e523f9bdcbaf7acd32"},
        Function{"getBuilderMode", 386, "3fe58fdcb85b8d37e309f632a7c57febd3c93f3d501ff708fb78414833512b8b"},
        Function{"getPendingPowerOrigin", 382, "31fcaf0dbd338b55d6bbdd1fcb2c95b91042a48d07cf4a1aad77df85ee5edf74"},
        Function{"getRule", 354, "4de04250ac539400f83dd92945c4cb71ddfeb97299919d940b3f143c9e8c98e2"},
        Function{"isRuleEnabled", 281, "524e1d6c2a8065d706af1934bc8c5c9e9e8438168de2091521407ef9f7c27695"},
        Function{"migrateLegacyRuleSettings", 563, "7ec22799815f2010642c1c90fee955e8d56a7199fd8afb255c20e6623ae9c376"},
        Function{"readBool", 280, "7ccc298e61e909195a360490afeb381d60a8f5c91285e6c4cacd0e5ec10d55ec"},
        Function{"readInt", 284, "234d66d973243dfc6f0db93b551381f70515c3d2ff6db1be05bfadff19b823f5"},
        Function{"refreshAssignedProductionIfNeeded", 457, "7b8b26d3e99490c678891748c8fcd1c3322a393899c4645408f0f3a1427b7005"},
    };

    inline constexpr std::array COMMAND_STRINGS{
        String{1, "CO_COMMAND"},
        String{134, "undefined"},
        String{151, "function"},
        String{217, "LINEAGE_1"},
        String{218, "LINEAGE_2"},
        String{219, "getGameRules"},
        String{220, "hasVictoryRule"},
        String{221, "RULE_ID"},
        String{222, "getVictoryRule"},
        String{223, "isRuleEnabled"},
        String{224, "getRule"},
        String{225, "getVariables"},
        String{226, "getVariable"},
        String{227, "RULE_VALUE_KEY"},
        String{228, "readDataInt32"},
        String{229, "readInt"},
        String{230, "BUILDER_MODE_KEY"},
        String{231, "BUILDER_MODE_FREE"},
        String{232, "BUILDER_MODE_ACTIVE"},
        String{233, "BUILDER_MODE_ALTERNATING"},
        String{234, "BUILDER_MODE_ASSIGNED"},
        String{235, "LEGACY_BUILDER_MODE_ACTIVE_HALF_CHARGE"},
        String{236, "POWER_CHARGE_MODE_KEY"},
        String{237, "POWER_CHARGE_MODE_FIRST_SLOT"},
        String{271, "createVariable"},
        String{272, "writeDataInt32"},
        String{273, "readDataBool"},
        String{275, "CO_LINEAGE_KEY"},
        String{276, "isValidLineage"},
        String{277, "getStoredCoLineage"},
        String{279, "getCO"},
        String{280, "push"},
        String{281, "length"},
        String{282, "writeInt"},
        String{285, "ownerId"},
        String{286, "getPlayerID"},
        String{288, "lineage"},
        String{293, "getCoID"},
        String{304, "getCurrentPlayer"},
        String{305, "getPlayerCount"},
        String{307, "getPlayer"},
        String{311, "player"},
        String{393, "readBool"},
        String{423, "getBuilderMode"},
        String{466, "assignedProductionRefreshRequested"},
        String{469, "refreshAssignedProductionIfNeeded"},
        String{470, "getAssignedBuildingStateSignature"},
        String{471, "getAssignedProductionStateSignature"},
        String{472, "assignedBuildingStateSignature"},
        String{473, "hasAssignedBuildingObjectChanges"},
        String{474, "assignedProductionStateSignature"},
        String{475, "reconcileAssignedBuildingOwners"},
        String{508, "PENDING_POWER_VALID_KEY"},
        String{509, "PENDING_POWER_OWNER_KEY"},
        String{510, "PENDING_POWER_LINEAGE_KEY"},
        String{512, "ensureCoLineages"},
        String{513, "getPendingPowerOrigin"},
        String{518, "clearPendingPowerOrigin"},
        String{519, "ensureMatchObjects"},
        String{530, "migrateLegacyRuleSettings"},
        String{675, "key"},
        String{705, "map"},
        String{1342, "ensureObjectById"},
        String{1351, "getPerkList"},
        String{1362, "finishAction"},
        String{1384, "rule"},
        String{1385, "object"},
    };

    inline constexpr std::array<std::uint32_t, 0> SHAPE_17{};

    inline constexpr std::array<std::uint32_t, 2> SHAPE_35{285, 288};

    inline constexpr std::array COMMAND_SHAPES{
        Shape{17, SHAPE_17},
        Shape{35, SHAPE_35},
    };

    inline constexpr std::array RULE_FUNCTIONS{
        Function{"checkDefeat", 127, "84ecfbfb422cc6a9f916e0023ce874b26e9fcc94df2a477a8f04e694c02fe11d"},
        Function{"getRuleType", 148, "74b941821ed60fbb6a9b09d59a3fd0dce694d33caaa9eed6c37fa1b97c4f2762"},
        Function{"getRuleValue", 407, "3c4414aee83d8e66a0241d1fd34351ff18711062b6520bcbf342fe7380af078d"},
        Function{"getValidRuleValue", 444, "d95a00c399db8cdac6993173c1814e1a4d9fed503725a312e9c4c011085df3b4"},
        Function{"getInfiniteValue", 123, "d3cf0c00e41168d6c9ec55c42ec90868d7715d5b3d6c921f3a00a2401b327831"},
    };

    inline constexpr std::array RULE_STRINGS{
        String{0, ""},
        String{1, "CMODE_CO_COMMAND"},
        String{3, "SPINBOX_INFINITY"},
        String{4, "getValidRuleValue"},
        String{5, "CO_COMMAND"},
        String{6, "BUILDER_MODE_FREE"},
        String{7, "BUILDER_MODE_ASSIGNED"},
        String{8, "POWER_CHARGE_MODE_FIRST_SLOT"},
        String{9, "POWER_CHARGE_MODE_WEIGHTED_SPLIT"},
        String{10, "JOIN_MODE_DISABLED"},
        String{11, "JOIN_MODE_ACTIVE"},
        String{12, "getDefaultRuleValue"},
        String{14, "migrateLegacyRuleSettings"},
        String{15, "SpinboxValue"},
        String{16, "toString"},
        String{17, "getVariables"},
        String{18, "getVariable"},
        String{19, "undefined"},
        String{20, "createVariable"},
        String{21, "writeDataInt32"},
        String{22, "readDataInt32"},
        String{67, "checkbox"},
        String{68, "spinbox"},
        String{76, "finishAction"},
        String{77, "GameEnums"},
        String{78, "DefeatType_Alive"},
        String{84, "item"},
        String{85, "value"},
        String{86, "rule"},
        String{87, "map"},
        String{89, "player"},
    };
}
