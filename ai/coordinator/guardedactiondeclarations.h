#pragma once

#include "ai/coordinator/guardedmovementdeclarations.h"

namespace Coordinator::ActionDeclaration
{
    using CommandDeclaration::String;
    using CommandDeclaration::Shape;
    struct Function
    {
        std::span<const CommandDeclaration::Function> records;
        std::uint32_t index;
        std::span<const std::string_view> formals;
        std::uint32_t locals;
    };
    struct Constant { std::uint32_t index; std::uint64_t encoded; };
    struct CatchBlock { std::uint32_t index, name; };
    struct Module
    {
        std::string_view path;
        std::span<const String> strings;
        std::span<const Constant> constants;
        std::span<const Shape> shapes;
        std::span<const CatchBlock> catches;
    };

    inline constexpr std::array CHESS_STRINGS{
        String{0, ""}, String{2, "CMODE_CHESS_BASE"}, String{52, "undefined"},
        String{56, "length"}, String{81, "getUnits"}, String{82, "size"},
        String{83, "at"}, String{87, "Global"}, String{131, "ACTION_BUILD_UNITS"},
        String{132, "ACTION_NEXT_PLAYER"}, String{139, "_modes"}, String{141, "isEnabled"},
        String{147, "state"}, String{148, "pending"}, String{150, "roundStarted"},
        String{151, "passed"}, String{155, "hasBudget"}, String{156, "budget"},
        String{158, "locked"}, String{165, "e"}, String{177, "installSharedPatches"},
        String{179, "refreshBudgetHud"}, String{201, "getCurrentPlayer"}, String{204, "getPlayerID"},
        String{227, "ensureStateLoaded"}, String{233, "setHasMoved"}, String{234, "saveState"},
        String{262, "__cmode_chess_base_patched"}, String{263, "__cmode_chess_base_build_patched"}, String{264, "UNIT"},
        String{265, "postAction"}, String{266, "call"}, String{267, "activeMode"},
        String{269, "_ZCO_NAMES"}, String{270, "perform"}, String{279, "map"},
        String{294, "action"}, String{301, "origBuild"}, String{302, "origPerf"},
        String{303, "origUnit"}};
    inline constexpr std::array CHESS_ACTIVE_MODE_RECORDS{MovementDeclaration::CHESS_HELPERS[0]};
    inline constexpr std::array CHESS_ACTIVE_MODE_FORMALS{std::string_view{"map"}};
    inline constexpr Function CHESS_ACTIVE_MODE{CHESS_ACTIVE_MODE_RECORDS, 16, CHESS_ACTIVE_MODE_FORMALS, 0};
    inline constexpr std::array CHESS_NEXT_WRAPPER_RECORDS{
        CommandDeclaration::Function{"nextWrapper", 715, "d1ce3f8b1c1b4e0061246776c24ddfd1b693c9a956c43ecb43c4f8f55cf56d58"}};
    inline constexpr std::array CHESS_NEXT_WRAPPER_FORMALS{std::string_view{"action"}, std::string_view{"map"}};
    inline constexpr Function CHESS_NEXT_WRAPPER{CHESS_NEXT_WRAPPER_RECORDS, 47, CHESS_NEXT_WRAPPER_FORMALS, 0};
    inline constexpr std::array CHESS_NEXT_CREATOR_RECORDS{
        CommandDeclaration::Function{"nextCreator", 714, "1ea303cfa23c1a0a7b6452c3d04e2cad1ffe188784b9574edb8759baff493b4f"}};
    inline constexpr Function CHESS_NEXT_CREATOR{CHESS_NEXT_CREATOR_RECORDS, 43, {}, 3};
    inline constexpr std::array CHESS_CONSTANTS{
        Constant{10, UINT64_C(0x0003800000000001)}};
    inline constexpr std::array CHESS_CATCHES{CatchBlock{28, 165}};
    inline constexpr Module CHESS{"/mods/custom_gamemodes/scripts/general/cmode_utils.js", CHESS_STRINGS, CHESS_CONSTANTS, {}, CHESS_CATCHES};

    inline constexpr std::array COMMAND_STRINGS{
        String{0, ""}, String{1, "CO_COMMAND"}, String{103, "Array"},
        String{113, "GameEnums"}, String{114, "Global"}, String{119, "Object"},
        String{134, "undefined"}, String{151, "function"}, String{173, "number"},
        String{217, "LINEAGE_1"}, String{218, "LINEAGE_2"}, String{219, "getGameRules"},
        String{224, "getRule"}, String{225, "getVariables"}, String{229, "readInt"},
        String{231, "BUILDER_MODE_FREE"}, String{232, "BUILDER_MODE_ACTIVE"}, String{233, "BUILDER_MODE_ALTERNATING"},
        String{234, "BUILDER_MODE_ASSIGNED"}, String{242, "isPolicyEnabled"}, String{254, "ARMY_SPRITE_KEY"},
        String{263, "getMap"}, String{276, "isValidLineage"}, String{278, "getOwner"},
        String{279, "getCO"}, String{281, "length"}, String{284, "getEffectiveUnitOrigin"},
        String{285, "ownerId"}, String{286, "getPlayerID"}, String{287, "getCoForLineage"},
        String{288, "lineage"}, String{289, "isActive"}, String{290, "getUnitCommandCo"},
        String{304, "getCurrentPlayer"}, String{311, "player"}, String{312, "y"},
        String{331, "x"}, String{384, "pendingBuild"}, String{385, "isRealUnit"},
        String{386, "getDefaultLineage"}, String{387, "getUniqueID"}, String{392, "writeBool"},
        String{393, "readBool"}, String{423, "getBuilderMode"}, String{424, "getCurrentDay"},
        String{425, "PLAYER_LINEAGE_KEY"}, String{427, "BUILDING_OWNER_KEY"}, String{428, "BUILDING_LINEAGE_KEY"},
        String{429, "getInitialBuildingLineage"}, String{430, "getControlType"}, String{431, "AiTypes_Human"},
        String{432, "getPlayerSelection"}, String{433, "PLAYER_ASSIGNMENTS_LOCKED_KEY"}, String{440, "getX"},
        String{441, "getY"}, String{442, "assignedBuildings"}, String{443, "building"},
        String{444, "onMap"}, String{445, "getTerrain"}, String{446, "getBuilding"},
        String{452, "isCommandProductionBuilding"}, String{455, "writeBuildingSelection"}, String{456, "BUILDING_SETUP_ELIGIBLE_KEY"},
        String{458, "isAssignmentSetupOpen"}, String{460, "writeAssignedBuildingSelection"}, String{461, "getRegisteredBuilding"},
        String{467, "isBaseProductionAction"}, String{468, "requestAssignedProductionRefresh"}, String{469, "refreshAssignedProductionIfNeeded"},
        String{478, "getBuildingSelection"}, String{494, "apply"}, String{503, "string"},
        String{563, "call"}, String{564, "prototype"}, String{565, "hasOwnProperty"},
        String{596, "indexOf"}, String{603, "getTargetUnit"}, String{604, "getActionTarget"},
        String{609, "canBePerformed"}, String{623, "getUnitID"}, String{627, "unit"},
        String{635, "perform"}, String{661, "action"}, String{675, "key"},
        String{705, "map"}, String{724, "getStepData"}, String{832, "."},
        String{897, "origin"}, String{1015, "WRAPPER_KEY"}, String{1060, "findMapArgument"},
        String{1073, "isArray"}, String{1074, "usesCoArmySprites"}, String{1075, "getCOArmy"},
        String{1076, "toLowerCase"}, String{1080, "getCoArmySpriteName"}, String{1082, "getArmyNameFromPlayerTable"},
        String{1083, "ARMY_NAME_PATCH_KEY"}, String{1084, "spriteContext"}, String{1085, "remember"},
        String{1086, "rememberUnitSpriteArmy"}, String{1087, "isUnitLike"}, String{1088, ".CO_COMMAND"},
        String{1089, "UNIT_SPRITE_WRAPPER_KEY"}, String{1090, "runWithSpriteContext"}, String{1093, "postAnimationUnit"},
        String{1094, "ensureSpriteContextFunction"}, String{1097, "actionTargetContext"}, String{1099, "postAnimationUnitContext"},
        String{1100, "ensureArmyNamePatch"}, String{1115, "callbackName"}, String{1162, "productionActionLookup"},
        String{1163, "getIsBaseProductionAction"}, String{1164, "canBuildUnits"}, String{1165, "getStepInputType"},
        String{1166, "getActionPoint"}, String{1167, "getMovementBuilding"}, String{1168, "hasOnMapActionTarget"},
        String{1169, "getTargetBuilding"}, String{1170, "getActionBuilding"}, String{1171, "getTarget"},
        String{1172, "ACTION_BUILDING_KEY"}, String{1173, "getActionBuildingOrigin"}, String{1174, "NEXT_PLAYER_ACTION_ID"},
        String{1175, ".canBePerformed.CO_COMMAND_ALTERNATING"}, String{1176, "ACTION_ORIGIN_KEY"}, String{1177, "reconcileTrackedActionBuilding"},
        String{1178, "ensureNextPlayerPolicyPatch"}, String{1179, ".perform.CO_COMMAND"}, String{1180, "lockProductionAssignments"},
        String{1181, "getActionOrigin"}, String{1182, "trackActionBuilding"}, String{1183, "refreshAfterProductionAction"},
        String{1184, "alternateAiBuilding"}, String{1185, "postanimation"}, String{1186, "ensureActionCallbackPatch"},
        String{1190, "ensureActionPatch"}, String{1385, "object"}, String{1386, "value"},
        String{1405, "actionId"}, String{1408, "objectId"}, String{1409, "original"},
        String{1411, "wrapped"}, String{1419, "args"}, String{1430, "context"},
        String{1450, "name"}};
    inline constexpr std::array COMMAND_IS_UNIT_LIKE_RECORDS{
        CommandDeclaration::Function{"isUnitLike", 188, "a1fca0e69a4d64161b1eaa6db755d3d893d2e8f534b1fd5fa4c79b36f9bed9ed"}};
    inline constexpr std::array COMMAND_IS_UNIT_LIKE_FORMALS{std::string_view{"value"}};
    inline constexpr Function COMMAND_IS_UNIT_LIKE{COMMAND_IS_UNIT_LIKE_RECORDS, 382, COMMAND_IS_UNIT_LIKE_FORMALS, 0};
    inline constexpr std::array COMMAND_IS_REAL_UNIT_RECORDS{
        CommandDeclaration::Function{"isRealUnit", 150, "581cb685486f09f82c7c83a726f874dafa17aeef1a2b3330ca937748fe5d681c"}};
    inline constexpr std::array COMMAND_IS_REAL_UNIT_FORMALS{std::string_view{"unit"}};
    inline constexpr Function COMMAND_IS_REAL_UNIT{COMMAND_IS_REAL_UNIT_RECORDS, 65, COMMAND_IS_REAL_UNIT_FORMALS, 0};
    inline constexpr std::array COMMAND_ACTION_TARGET_CONTEXT_RECORDS{
        CommandDeclaration::Function{"actionTargetContext", 264, "5c106ef733ba73834dbb803f81f18c6df340ad262ce71d3732ebaedf6f9074ef"}};
    inline constexpr std::array COMMAND_ACTION_TARGET_CONTEXT_FORMALS{std::string_view{"object"}, std::string_view{"args"}};
    inline constexpr Function COMMAND_ACTION_TARGET_CONTEXT{COMMAND_ACTION_TARGET_CONTEXT_RECORDS, 390, COMMAND_ACTION_TARGET_CONTEXT_FORMALS, 0};
    inline constexpr std::array COMMAND_POST_ANIMATION_UNIT_CONTEXT_RECORDS{
        CommandDeclaration::Function{"postAnimationUnitContext", 143, "2017a2e1dcb651c77b50926c2ad1bff70a6b7db039fb0899c68da11e3d10b92d"}};
    inline constexpr std::array COMMAND_POST_ANIMATION_UNIT_CONTEXT_FORMALS{std::string_view{"object"}, std::string_view{"args"}};
    inline constexpr Function COMMAND_POST_ANIMATION_UNIT_CONTEXT{COMMAND_POST_ANIMATION_UNIT_CONTEXT_RECORDS, 391, COMMAND_POST_ANIMATION_UNIT_CONTEXT_FORMALS, 0};
    inline constexpr std::array COMMAND_RUN_WITH_SPRITE_CONTEXT_RECORDS{
        CommandDeclaration::Function{"runWithSpriteContext", 241, "1e333d9dcea092efa4e717157b96e9d12725dec80a03eaae7ff732b62dc3b24f"},
        CommandDeclaration::Function{"runWithSpriteContext", 241, "8895eca6a452671d9b72d92e373b10c298bafb451c5358d37d495039d2c3dc32"}};
    inline constexpr std::array COMMAND_RUN_WITH_SPRITE_CONTEXT_FORMALS{std::string_view{"context"}, std::string_view{"fn"}, std::string_view{"thisArg"}, std::string_view{"args"}};
    inline constexpr Function COMMAND_RUN_WITH_SPRITE_CONTEXT{COMMAND_RUN_WITH_SPRITE_CONTEXT_RECORDS, 385, COMMAND_RUN_WITH_SPRITE_CONTEXT_FORMALS, 0};
    inline constexpr std::array COMMAND_GET_ACTION_ORIGIN_RECORDS{
        CommandDeclaration::Function{"getActionOrigin", 393, "ba5f27eb29ce4939e70a92066677f1852ffa64c487b721b396feb24c7de24e3e"}};
    inline constexpr std::array COMMAND_GET_ACTION_ORIGIN_FORMALS{std::string_view{"action"}, std::string_view{"actionId"}, std::string_view{"map"}};
    inline constexpr Function COMMAND_GET_ACTION_ORIGIN{COMMAND_GET_ACTION_ORIGIN_RECORDS, 425, COMMAND_GET_ACTION_ORIGIN_FORMALS, 0};
    inline constexpr std::array COMMAND_GET_ACTION_BUILDING_ORIGIN_RECORDS{
        CommandDeclaration::Function{"getActionBuildingOrigin", 169, "7fe7de945c82ca90b429a9c59a81aae3020ac8bf9ae86556bae74e87a362381c"}};
    inline constexpr std::array COMMAND_GET_ACTION_BUILDING_ORIGIN_FORMALS{std::string_view{"action"}, std::string_view{"map"}};
    inline constexpr Function COMMAND_GET_ACTION_BUILDING_ORIGIN{COMMAND_GET_ACTION_BUILDING_ORIGIN_RECORDS, 421, COMMAND_GET_ACTION_BUILDING_ORIGIN_FORMALS, 0};
    inline constexpr std::array COMMAND_GET_ACTION_BUILDING_RECORDS{
        CommandDeclaration::Function{"getActionBuilding", 424, "0617d37a855c1053d80d08071225e252ee780951fa432dadbfb0a64a7bc14111"}};
    inline constexpr std::array COMMAND_GET_ACTION_BUILDING_FORMALS{std::string_view{"action"}, std::string_view{"map"}};
    inline constexpr Function COMMAND_GET_ACTION_BUILDING{COMMAND_GET_ACTION_BUILDING_RECORDS, 420, COMMAND_GET_ACTION_BUILDING_FORMALS, 0};
    inline constexpr std::array COMMAND_HAS_ON_MAP_ACTION_TARGET_RECORDS{
        CommandDeclaration::Function{"hasOnMapActionTarget", 254, "6c4cc4cac767d9c73aecb2e5ac35021b377959a88df7c50a51ae0ff53c365427"}};
    inline constexpr std::array COMMAND_HAS_ON_MAP_ACTION_TARGET_FORMALS{std::string_view{"action"}, std::string_view{"map"}};
    inline constexpr Function COMMAND_HAS_ON_MAP_ACTION_TARGET{COMMAND_HAS_ON_MAP_ACTION_TARGET_RECORDS, 419, COMMAND_HAS_ON_MAP_ACTION_TARGET_FORMALS, 0};
    inline constexpr std::array COMMAND_GET_ACTION_POINT_RECORDS{
        CommandDeclaration::Function{"getActionPoint", 414, "963b75191f0ca085619ce8f7be57144853a7329281a9764ebc36d03afc98f68d"}};
    inline constexpr std::array COMMAND_GET_ACTION_POINT_FORMALS{std::string_view{"action"}};
    inline constexpr Function COMMAND_GET_ACTION_POINT{COMMAND_GET_ACTION_POINT_RECORDS, 422, COMMAND_GET_ACTION_POINT_FORMALS, 0};
    inline constexpr std::array COMMAND_GET_BUILDING_SELECTION_RECORDS{
        CommandDeclaration::Function{"getBuildingSelection", 978, "c1868704c910732e78f2ee6a6d196a044e61c9c1f7f0f44b665e0b28c7aa437b"}};
    inline constexpr std::array COMMAND_GET_BUILDING_SELECTION_FORMALS{std::string_view{"building"}};
    inline constexpr Function COMMAND_GET_BUILDING_SELECTION{COMMAND_GET_BUILDING_SELECTION_RECORDS, 89, COMMAND_GET_BUILDING_SELECTION_FORMALS, 0};
    inline constexpr std::array COMMAND_GET_PLAYER_SELECTION_RECORDS{
        CommandDeclaration::Function{"getPlayerSelection", 868, "2472281549bb8888001b6e71a453ec766712a2e34d80d02efce1fe4556eaf877"}};
    inline constexpr std::array COMMAND_GET_PLAYER_SELECTION_FORMALS{std::string_view{"player"}};
    inline constexpr Function COMMAND_GET_PLAYER_SELECTION{COMMAND_GET_PLAYER_SELECTION_RECORDS, 87, COMMAND_GET_PLAYER_SELECTION_FORMALS, 0};
    inline constexpr std::array COMMAND_IS_BASE_PRODUCTION_ACTION_RECORDS{
        CommandDeclaration::Function{"isBaseProductionAction", 384, "5600bbebf8d75a7631c948c6477950d65aed0be07fb25f153e2ceafcf7b2e49f"}};
    inline constexpr std::array COMMAND_IS_BASE_PRODUCTION_ACTION_FORMALS{std::string_view{"actionId"}, std::string_view{"map"}};
    inline constexpr Function COMMAND_IS_BASE_PRODUCTION_ACTION{COMMAND_IS_BASE_PRODUCTION_ACTION_RECORDS, 418, COMMAND_IS_BASE_PRODUCTION_ACTION_FORMALS, 0};
    inline constexpr std::array COMMAND_TRACK_ACTION_BUILDING_RECORDS{
        CommandDeclaration::Function{"trackActionBuilding", 766, "912982917906309494c38e20e8e8b4dc69c45da6a4bc66669ff1837f79511823"}};
    inline constexpr std::array COMMAND_TRACK_ACTION_BUILDING_FORMALS{std::string_view{"object"}, std::string_view{"action"}, std::string_view{"origin"}, std::string_view{"map"}};
    inline constexpr Function COMMAND_TRACK_ACTION_BUILDING{COMMAND_TRACK_ACTION_BUILDING_RECORDS, 423, COMMAND_TRACK_ACTION_BUILDING_FORMALS, 0};
    inline constexpr std::array COMMAND_REFRESH_AFTER_PRODUCTION_ACTION_RECORDS{
        CommandDeclaration::Function{"refreshAfterProductionAction", 267, "8356a97392b596cbd21f3303c12515f9e9400ca09ef661d6673a7e5a8af4b26b"}};
    inline constexpr std::array COMMAND_REFRESH_AFTER_PRODUCTION_ACTION_FORMALS{std::string_view{"actionId"}, std::string_view{"map"}};
    inline constexpr Function COMMAND_REFRESH_AFTER_PRODUCTION_ACTION{COMMAND_REFRESH_AFTER_PRODUCTION_ACTION_RECORDS, 109, COMMAND_REFRESH_AFTER_PRODUCTION_ACTION_FORMALS, 0};
    inline constexpr std::array COMMAND_RECONCILE_TRACKED_ACTION_BUILDING_RECORDS{
        CommandDeclaration::Function{"reconcileTrackedActionBuilding", 1064, "1fcb2673b799f16c990159c7f64bddf9816a0f568c109ca1066cbd378dc9ea5a"}};
    inline constexpr std::array COMMAND_RECONCILE_TRACKED_ACTION_BUILDING_FORMALS{std::string_view{"object"}, std::string_view{"map"}};
    inline constexpr Function COMMAND_RECONCILE_TRACKED_ACTION_BUILDING{COMMAND_RECONCILE_TRACKED_ACTION_BUILDING_RECORDS, 424, COMMAND_RECONCILE_TRACKED_ACTION_BUILDING_FORMALS, 0};
    inline constexpr std::array COMMAND_ALTERNATE_AI_BUILDING_RECORDS{
        CommandDeclaration::Function{"alternateAiBuilding", 602, "403eb18813bb85e2a2cc43586bb9ece9a1b84f678f38de7f5847efc5ec67bffa"},
        CommandDeclaration::Function{"alternateAiBuilding", 602, "b750df65091f856f15d0a75d8d35e2ec347ee16708e8e99013745f412e5cdbea"}};
    inline constexpr std::array COMMAND_ALTERNATE_AI_BUILDING_FORMALS{std::string_view{"action"}, std::string_view{"actionId"}, std::string_view{"map"}, std::string_view{"selection"}};
    inline constexpr Function COMMAND_ALTERNATE_AI_BUILDING{COMMAND_ALTERNATE_AI_BUILDING_RECORDS, 427, COMMAND_ALTERNATE_AI_BUILDING_FORMALS, 0};
    inline constexpr std::array COMMAND_FIND_MAP_ARGUMENT_RECORDS{
        CommandDeclaration::Function{"findMapArgument", 244, "69ee5d2f5dba1e0161bce0c3fde416a190173d2eea98b4977b028989b488e801"}};
    inline constexpr std::array COMMAND_FIND_MAP_ARGUMENT_FORMALS{std::string_view{"args"}};
    inline constexpr Function COMMAND_FIND_MAP_ARGUMENT{COMMAND_FIND_MAP_ARGUMENT_RECORDS, 426, COMMAND_FIND_MAP_ARGUMENT_FORMALS, 0};
    inline constexpr std::array COMMAND_LOCK_PRODUCTION_ASSIGNMENTS_RECORDS{
        CommandDeclaration::Function{"lockProductionAssignments", 529, "2b0e10f558c84e7ab915b5b27b0ec668636460b2a021bc578de457559aa29ead"}};
    inline constexpr std::array COMMAND_LOCK_PRODUCTION_ASSIGNMENTS_FORMALS{std::string_view{"player"}};
    inline constexpr Function COMMAND_LOCK_PRODUCTION_ASSIGNMENTS{COMMAND_LOCK_PRODUCTION_ASSIGNMENTS_RECORDS, 104, COMMAND_LOCK_PRODUCTION_ASSIGNMENTS_FORMALS, 0};
    inline constexpr std::array COMMAND_IS_ASSIGNMENT_SETUP_OPEN_RECORDS{
        CommandDeclaration::Function{"isAssignmentSetupOpen", 368, "68a6cdb5367ea6ca66efc2d1eb55d1d744b248538d53006c123958183f28ec8b"}};
    inline constexpr std::array COMMAND_IS_ASSIGNMENT_SETUP_OPEN_FORMALS{std::string_view{"player"}};
    inline constexpr Function COMMAND_IS_ASSIGNMENT_SETUP_OPEN{COMMAND_IS_ASSIGNMENT_SETUP_OPEN_RECORDS, 93, COMMAND_IS_ASSIGNMENT_SETUP_OPEN_FORMALS, 0};
    inline constexpr std::array COMMAND_GET_CO_ARMY_SPRITE_NAME_RECORDS{
        CommandDeclaration::Function{"getCoArmySpriteName", 691, "93182b729ee777220326843334585705f6a1d6ea2680f8c50dab8d7902c0205d"},
        CommandDeclaration::Function{"getCoArmySpriteName", 691, "1ae97a62597871ceb431f68371c09bf9e6197724ea15629f3410943950cf0521"}};
    inline constexpr std::array COMMAND_GET_CO_ARMY_SPRITE_NAME_FORMALS{std::string_view{"unit"}, std::string_view{"player"}, std::string_view{"table"}};
    inline constexpr Function COMMAND_GET_CO_ARMY_SPRITE_NAME{COMMAND_GET_CO_ARMY_SPRITE_NAME_RECORDS, 377, COMMAND_GET_CO_ARMY_SPRITE_NAME_FORMALS, 0};
    inline constexpr std::array COMMAND_USES_CO_ARMY_SPRITES_RECORDS{
        CommandDeclaration::Function{"usesCoArmySprites", 110, "68134ed5bcf58fa7831a88db4c33ce0e1f9993b7251f65b621a8479096df2afe"}};
    inline constexpr std::array COMMAND_USES_CO_ARMY_SPRITES_FORMALS{std::string_view{"map"}};
    inline constexpr Function COMMAND_USES_CO_ARMY_SPRITES{COMMAND_USES_CO_ARMY_SPRITES_RECORDS, 19, COMMAND_USES_CO_ARMY_SPRITES_FORMALS, 0};
    inline constexpr std::array COMMAND_IS_POLICY_ENABLED_RECORDS{
        CommandDeclaration::Function{"isPolicyEnabled", 138, "49543e109b46860b69e4f2a959c6e54724e24ede2da73fb5e769dd647f7e49d4"}};
    inline constexpr std::array COMMAND_IS_POLICY_ENABLED_FORMALS{std::string_view{"map"}, std::string_view{"key"}};
    inline constexpr Function COMMAND_IS_POLICY_ENABLED{COMMAND_IS_POLICY_ENABLED_RECORDS, 7, COMMAND_IS_POLICY_ENABLED_FORMALS, 0};
    inline constexpr std::array COMMAND_APPEND_ASSIGNED_BUILDING_BADGE_RECORDS{
        CommandDeclaration::Function{"appendAssignedBuildingBadge", 753, "7c86b4f48939d1c1348538bcb0f73426c097ea1d7e7b4f4563f26dd54904376b"}};
    inline constexpr std::array COMMAND_APPEND_ASSIGNED_BUILDING_BADGE_FORMALS{std::string_view{"building"}, std::string_view{"neutral"}, std::string_view{"map"}};
    inline constexpr Function COMMAND_APPEND_ASSIGNED_BUILDING_BADGE{COMMAND_APPEND_ASSIGNED_BUILDING_BADGE_RECORDS, 114, COMMAND_APPEND_ASSIGNED_BUILDING_BADGE_FORMALS, 0};
    inline constexpr std::array COMMAND_GET_PRODUCTION_BUILDING_BADGE_ICON_RECORDS{
        CommandDeclaration::Function{"getProductionBuildingBadgeIcon", 678, "7660f2b4dd3ac147674b46f0987b5a53e48bacc001e2f525665933f73e66ceb5"}};
    inline constexpr std::array COMMAND_GET_PRODUCTION_BUILDING_BADGE_ICON_FORMALS{std::string_view{"building"}, std::string_view{"map"}, std::string_view{"neutral"}};
    inline constexpr Function COMMAND_GET_PRODUCTION_BUILDING_BADGE_ICON{COMMAND_GET_PRODUCTION_BUILDING_BADGE_ICON_RECORDS, 113, COMMAND_GET_PRODUCTION_BUILDING_BADGE_ICON_FORMALS, 0};
    inline constexpr std::array COMMAND_USES_PRODUCTION_BADGES_RECORDS{
        CommandDeclaration::Function{"usesProductionBadges", 134, "28f93e3fd62e92652c976747848261bf892362ec7551c0ade006d3240cfd9d1e"}};
    inline constexpr std::array COMMAND_USES_PRODUCTION_BADGES_FORMALS{std::string_view{"map"}};
    inline constexpr Function COMMAND_USES_PRODUCTION_BADGES{COMMAND_USES_PRODUCTION_BADGES_RECORDS, 23, COMMAND_USES_PRODUCTION_BADGES_FORMALS, 0};
    inline constexpr std::array COMMAND_IS_DEFAULT_ON_POLICY_ENABLED_RECORDS{
        CommandDeclaration::Function{"isDefaultOnPolicyEnabled", 155, "dede8c21ba5240bf2852657c9b258944291e0d7d07ee5bdadca10012a1814d14"}};
    inline constexpr std::array COMMAND_IS_DEFAULT_ON_POLICY_ENABLED_FORMALS{std::string_view{"map"}, std::string_view{"key"}};
    inline constexpr Function COMMAND_IS_DEFAULT_ON_POLICY_ENABLED{COMMAND_IS_DEFAULT_ON_POLICY_ENABLED_RECORDS, 21, COMMAND_IS_DEFAULT_ON_POLICY_ENABLED_FORMALS, 0};
    inline constexpr std::array COMMAND_SPRITE_WRAPPER_RECORDS{
        CommandDeclaration::Function{"spriteWrapper", 234, "b33086bf3290c4fed1edbe2489f9421b04391de4ae42b2d9e011f81e39be621f"}};
    inline constexpr Function COMMAND_SPRITE_WRAPPER{COMMAND_SPRITE_WRAPPER_RECORDS, 387, {}, 0};
    inline constexpr std::array COMMAND_SPRITE_CREATOR_RECORDS{
        CommandDeclaration::Function{"spriteCreator", 472, "83e2f87436e2471ca856b3a693768f24a532375e3064a45669c482f57428e5cc"},
        CommandDeclaration::Function{"spriteCreator", 472, "2137c9aa52dab4220ff775b810c36e81c5d8da6c132fb6b8c95b97cfcf4edc06"}};
    inline constexpr std::array COMMAND_SPRITE_CREATOR_FORMALS{std::string_view{"object"}, std::string_view{"objectId"}, std::string_view{"name"}, std::string_view{"resolveContext"}};
    inline constexpr Function COMMAND_SPRITE_CREATOR{COMMAND_SPRITE_CREATOR_RECORDS, 386, COMMAND_SPRITE_CREATOR_FORMALS, 1};
    inline constexpr std::array COMMAND_ACTION_WRAPPER_RECORDS{
        CommandDeclaration::Function{"actionWrapper", 972, "05f996fb241c5d951c8ab7ff9590ff2439c551c715a4bd54a49bffe522d3df18"}};
    inline constexpr std::array COMMAND_ACTION_WRAPPER_FORMALS{std::string_view{"action"}, std::string_view{"map"}};
    inline constexpr Function COMMAND_ACTION_WRAPPER{COMMAND_ACTION_WRAPPER_RECORDS, 433, COMMAND_ACTION_WRAPPER_FORMALS, 0};
    inline constexpr std::array COMMAND_ACTION_CREATOR_RECORDS{
        CommandDeclaration::Function{"actionCreator", 684, "ea3fd377714f31b14629bdcb613221070072895ae00e6f398c6fcdabe9e3a59b"}};
    inline constexpr std::array COMMAND_ACTION_CREATOR_FORMALS{std::string_view{"object"}, std::string_view{"actionId"}};
    inline constexpr Function COMMAND_ACTION_CREATOR{COMMAND_ACTION_CREATOR_RECORDS, 432, COMMAND_ACTION_CREATOR_FORMALS, 1};
    inline constexpr std::array COMMAND_CALLBACK_WRAPPER_RECORDS{
        CommandDeclaration::Function{"callbackWrapper", 481, "9e68e0c6c0519339ab6084adf290f962e78561e9eab5faef4c07664d69259d08"}};
    inline constexpr Function COMMAND_CALLBACK_WRAPPER{COMMAND_CALLBACK_WRAPPER_RECORDS, 431, {}, 0};
    inline constexpr std::array COMMAND_CALLBACK_CREATOR_RECORDS{
        CommandDeclaration::Function{"callbackCreator", 405, "509a5835b0c677e5ca0e27b6cb50b5aad40b65f1f2fbbf950f62b5145bee4ff2"}};
    inline constexpr std::array COMMAND_CALLBACK_CREATOR_FORMALS{std::string_view{"object"}, std::string_view{"actionId"}, std::string_view{"callbackName"}};
    inline constexpr Function COMMAND_CALLBACK_CREATOR{COMMAND_CALLBACK_CREATOR_RECORDS, 430, COMMAND_CALLBACK_CREATOR_FORMALS, 1};
    inline constexpr std::array COMMAND_NEXT_CAN_WRAPPER_RECORDS{
        CommandDeclaration::Function{"nextCanWrapper", 482, "ff7aa6c3c5cff687e0904ba30d52062b3094b063105c0c768f12bf48771b21bb"}};
    inline constexpr std::array COMMAND_NEXT_CAN_WRAPPER_FORMALS{std::string_view{"action"}, std::string_view{"map"}};
    inline constexpr Function COMMAND_NEXT_CAN_WRAPPER{COMMAND_NEXT_CAN_WRAPPER_RECORDS, 429, COMMAND_NEXT_CAN_WRAPPER_FORMALS, 0};
    inline constexpr std::array COMMAND_NEXT_CAN_CREATOR_RECORDS{
        CommandDeclaration::Function{"nextCanCreator", 342, "efdd3b37242c4c1a280960b23cb180071b1ecc400590532c41bd237b55c12521"}};
    inline constexpr std::array COMMAND_NEXT_CAN_CREATOR_FORMALS{std::string_view{"object"}, std::string_view{"actionId"}};
    inline constexpr Function COMMAND_NEXT_CAN_CREATOR{COMMAND_NEXT_CAN_CREATOR_RECORDS, 428, COMMAND_NEXT_CAN_CREATOR_FORMALS, 1};
    inline constexpr std::array COMMAND_ARMY_WRAPPER_RECORDS{
        CommandDeclaration::Function{"armyWrapper", 350, "b00adb35f5c066e66217b1864f397447ca9adc14e5ba2f12935cac0e0ae68258"},
        CommandDeclaration::Function{"armyWrapper", 350, "19498c8d64a6f72ee39bccf567660d54f7dec104c43617db7d973fa8ffcfd459"}};
    inline constexpr std::array COMMAND_ARMY_WRAPPER_FORMALS{std::string_view{"player"}, std::string_view{"table"}};
    inline constexpr Function COMMAND_ARMY_WRAPPER{COMMAND_ARMY_WRAPPER_RECORDS, 381, COMMAND_ARMY_WRAPPER_FORMALS, 0};
    inline constexpr std::array COMMAND_ARMY_CREATOR_RECORDS{
        CommandDeclaration::Function{"armyCreator", 314, "25b32367b859bc91f6e6c94a10e2d086f29b01dd58b0072bc2837d9a5f0d82d7"}};
    inline constexpr Function COMMAND_ARMY_CREATOR{COMMAND_ARMY_CREATOR_RECORDS, 380, {}, 1};
    inline constexpr std::array COMMAND_BUILDING_LOAD_WRAPPER_RECORDS{
        CommandDeclaration::Function{"buildingLoadWrapper", 215, "959e96896002629083b84a0cf418e005e9e3684ca8149f963ce4a041fc59dbe5"}};
    inline constexpr std::array COMMAND_BUILDING_LOAD_WRAPPER_FORMALS{std::string_view{"building"}, std::string_view{"neutral"}, std::string_view{"map"}};
    inline constexpr Function COMMAND_BUILDING_LOAD_WRAPPER{COMMAND_BUILDING_LOAD_WRAPPER_RECORDS, 116, COMMAND_BUILDING_LOAD_WRAPPER_FORMALS, 0};
    inline constexpr std::array COMMAND_BUILDING_WEATHER_WRAPPER_RECORDS{
        CommandDeclaration::Function{"buildingWeatherWrapper", 304, "a8e744e254d55a04070c40893f29b259d177003ddca33bd7fb458e5f0be9dfb5"}};
    inline constexpr std::array COMMAND_BUILDING_WEATHER_WRAPPER_FORMALS{std::string_view{"building"}, std::string_view{"weather"}, std::string_view{"map"}};
    inline constexpr Function COMMAND_BUILDING_WEATHER_WRAPPER{COMMAND_BUILDING_WEATHER_WRAPPER_RECORDS, 117, COMMAND_BUILDING_WEATHER_WRAPPER_FORMALS, 0};
    inline constexpr std::array COMMAND_BUILDING_CREATOR_RECORDS{
        CommandDeclaration::Function{"buildingCreator", 497, "db33f2b4993aac89dfbf59fd1bce23a5245ec717304126447f09b9ecc48518ff"}};
    inline constexpr std::array COMMAND_BUILDING_CREATOR_FORMALS{std::string_view{"object"}, std::string_view{"objectId"}};
    inline constexpr Function COMMAND_BUILDING_CREATOR{COMMAND_BUILDING_CREATOR_RECORDS, 115, COMMAND_BUILDING_CREATOR_FORMALS, 2};
    inline constexpr std::array COMMAND_CONSTANTS{
        Constant{2, UINT64_C(0x0003c00000000001)},
        Constant{9, UINT64_C(0x0003800000000000)},
        Constant{10, UINT64_C(0x0003800000000001)}};
    inline constexpr std::array<std::uint32_t, 2> COMMAND_SHAPE_27{285, 288};
    inline constexpr std::array<std::uint32_t, 2> COMMAND_SHAPE_28{285, 288};
    inline constexpr std::array<std::uint32_t, 2> COMMAND_SHAPE_29{285, 288};
    inline constexpr std::array<std::uint32_t, 2> COMMAND_SHAPE_30{285, 288};
    inline constexpr std::array<std::uint32_t, 2> COMMAND_SHAPE_31{285, 288};
    inline constexpr std::array<std::uint32_t, 2> COMMAND_SHAPE_32{285, 288};
    inline constexpr std::array<std::uint32_t, 2> COMMAND_SHAPE_33{285, 288};
    inline constexpr std::array<std::uint32_t, 2> COMMAND_SHAPE_86{627, 1085};
    inline constexpr std::array<std::uint32_t, 2> COMMAND_SHAPE_87{627, 1085};
    inline constexpr std::array<std::uint32_t, 2> COMMAND_SHAPE_90{331, 312};
    inline constexpr std::array<std::uint32_t, 5> COMMAND_SHAPE_91{443, 331, 312, 285, 897};
    inline constexpr std::array COMMAND_SHAPES{Shape{27, COMMAND_SHAPE_27}, Shape{28, COMMAND_SHAPE_28}, Shape{29, COMMAND_SHAPE_29}, Shape{30, COMMAND_SHAPE_30}, Shape{31, COMMAND_SHAPE_31}, Shape{32, COMMAND_SHAPE_32}, Shape{33, COMMAND_SHAPE_33}, Shape{86, COMMAND_SHAPE_86}, Shape{87, COMMAND_SHAPE_87}, Shape{90, COMMAND_SHAPE_90}, Shape{91, COMMAND_SHAPE_91}};
    inline constexpr Module COMMAND{"/mods/custom_gamemodes/scripts/general/cmode_co_command.js", COMMAND_STRINGS, COMMAND_CONSTANTS, COMMAND_SHAPES, {}};
    inline constexpr std::array COMMAND_HELPERS{COMMAND_IS_UNIT_LIKE, COMMAND_IS_REAL_UNIT, COMMAND_ACTION_TARGET_CONTEXT, COMMAND_POST_ANIMATION_UNIT_CONTEXT, COMMAND_RUN_WITH_SPRITE_CONTEXT, COMMAND_GET_ACTION_ORIGIN, COMMAND_GET_ACTION_BUILDING_ORIGIN, COMMAND_GET_ACTION_BUILDING, COMMAND_HAS_ON_MAP_ACTION_TARGET, COMMAND_GET_ACTION_POINT, COMMAND_GET_BUILDING_SELECTION, COMMAND_GET_PLAYER_SELECTION, COMMAND_IS_BASE_PRODUCTION_ACTION, COMMAND_TRACK_ACTION_BUILDING, COMMAND_REFRESH_AFTER_PRODUCTION_ACTION, COMMAND_RECONCILE_TRACKED_ACTION_BUILDING, COMMAND_ALTERNATE_AI_BUILDING, COMMAND_FIND_MAP_ARGUMENT, COMMAND_LOCK_PRODUCTION_ASSIGNMENTS, COMMAND_IS_ASSIGNMENT_SETUP_OPEN, COMMAND_GET_CO_ARMY_SPRITE_NAME, COMMAND_USES_CO_ARMY_SPRITES, COMMAND_IS_POLICY_ENABLED};
    inline constexpr std::array COMMAND_BUILDING_STRINGS{
        String{120, "Qt"}, String{256, "isDefaultOnPolicyEnabled"}, String{258, "PRODUCTION_BADGE_KEY"}, String{448, "usesProductionBadges"}, String{454, "BUILDING_PRODUCTION_KEY"}, String{479, "BUILDING_ICON_2"}, String{480, "BUILDING_ICON_1"}, String{481, "getProductionBuildingBadgeIcon"}, String{482, "getBuildingWidth"}, String{483, "getBuildingHeigth"}, String{484, "BUILDING_BADGE_MAX_DIMENSION"}, String{485, "_"}, String{486, "toString"}, String{487, "loadWeatherOverlaySpriteV2"}, String{488, "Recoloring_None"}, String{489, "loadSpriteV2"}, String{490, "point"}, String{491, "loadSprites"}, String{492, ".loadSprites.CO_COMMAND"}, String{493, "BUILDING_SPRITE_WRAPPER_KEY"}, String{495, "appendAssignedBuildingBadge"}, String{496, "onWeatherChanged"}, String{497, ".onWeatherChanged.CO_COMMAND"}, String{498, "getNeutralLoaded"}, String{501, "ensureBuildingSpriteHook"}, String{1407, "neutral"}, String{1410, "weatherOriginal"}, String{1412, "weatherWrapped"}, String{1413, "weather"}};
    inline constexpr std::array COMMAND_BUILDING_CONSTANTS{
        Constant{7, UINT64_C(0x0003c00000000000)}, Constant{11, UINT64_C(0x0003c00000000190)}};
    inline constexpr Module COMMAND_BUILDING_EXTRA{COMMAND.path, COMMAND_BUILDING_STRINGS, COMMAND_BUILDING_CONSTANTS, {}, {}};
    inline constexpr std::array COMMAND_BUILDING_HELPERS{COMMAND_APPEND_ASSIGNED_BUILDING_BADGE, COMMAND_GET_PRODUCTION_BUILDING_BADGE_ICON, COMMAND_USES_PRODUCTION_BADGES, COMMAND_IS_DEFAULT_ON_POLICY_ENABLED};

    inline constexpr std::array FRONTIER_STRINGS{
        String{0, ""}, String{1, "CMODE_PROCGEN"}, String{2, "CMODE_PROCEDURAL_FRONTIER"},
        String{20, "undefined"}, String{22, "function"}, String{37, "length"},
        String{58, "getGameRules"}, String{59, "hasVictoryRule"}, String{60, "RULE_ID"},
        String{61, "getVictoryRule"}, String{62, "getRule"}, String{63, "getRuleValue"},
        String{64, "ITEM_ENABLED"}, String{65, "getVariables"}, String{66, "getVariable"},
        String{67, "INITIALIZED_KEY"}, String{68, "readDataBool"}, String{137, "installPatches"},
        String{148, "Object"}, String{149, "prototype"}, String{150, "hasOwnProperty"},
        String{151, "call"}, String{181, "Global"}, String{185, "unitSpriteManager"},
        String{186, "getUnitsSorted"}, String{218, "isEnabled"}, String{219, "timedWave"},
        String{221, "PLAYER"}, String{222, "PATCH_SENTINEL"}, String{223, "startOfTurn"},
        String{224, "onPlayerStart"}, String{225, "UNIT"}, String{226, "postAction"},
        String{240, "map"}, String{248, "player"}, String{261, "originalStart"},
        String{262, "originalUnitPostAction"}};
    inline constexpr std::array FRONTIER_GET_RULE_RECORDS{MovementDeclaration::FRONTIER_HELPERS[0]};
    inline constexpr std::array FRONTIER_GET_RULE_FORMALS{std::string_view{"map"}};
    inline constexpr Function FRONTIER_GET_RULE{FRONTIER_GET_RULE_RECORDS, 8, FRONTIER_GET_RULE_FORMALS, 0};
    inline constexpr std::array FRONTIER_IS_ENABLED_RECORDS{MovementDeclaration::FRONTIER_HELPERS[1]};
    inline constexpr std::array FRONTIER_IS_ENABLED_FORMALS{std::string_view{"map"}};
    inline constexpr Function FRONTIER_IS_ENABLED{FRONTIER_IS_ENABLED_RECORDS, 9, FRONTIER_IS_ENABLED_FORMALS, 0};
    inline constexpr std::array FRONTIER_ON_PLAYER_START_RECORDS{
        CommandDeclaration::Function{"onPlayerStart", 199, "da7ac12989bef23e16dc6b85320c89912352dc2ea0dba3bd1a7d1c4a5bb5baea"}};
    inline constexpr std::array FRONTIER_ON_PLAYER_START_FORMALS{std::string_view{"player"}, std::string_view{"map"}};
    inline constexpr Function FRONTIER_ON_PLAYER_START{FRONTIER_ON_PLAYER_START_RECORDS, 48, FRONTIER_ON_PLAYER_START_FORMALS, 0};
    inline constexpr std::array FRONTIER_START_WRAPPER_RECORDS{
        CommandDeclaration::Function{"startWrapper", 194, "64f21f5bb6059f0d917c65d48712610981fe1b8d41ab7ca33eb7c6da5f9cd1cf"}};
    inline constexpr std::array FRONTIER_START_WRAPPER_FORMALS{std::string_view{"player"}, std::string_view{"map"}};
    inline constexpr Function FRONTIER_START_WRAPPER{FRONTIER_START_WRAPPER_RECORDS, 51, FRONTIER_START_WRAPPER_FORMALS, 0};
    inline constexpr std::array FRONTIER_START_CREATOR_RECORDS{
        CommandDeclaration::Function{"startCreator", 733, "aeb85b9cf9df2eb3ce9f49dcc6c1141de39b6d755590a8886cc9f2b6fe2030ba"}};
    inline constexpr Function FRONTIER_START_CREATOR{FRONTIER_START_CREATOR_RECORDS, 50, {}, 2};
    inline constexpr Module FRONTIER{"/mods/custom_gamemodes/scripts/general/cmode_procedural_frontier.js", FRONTIER_STRINGS, {}, {}, {}};
    inline constexpr std::array FRONTIER_HELPERS{FRONTIER_ON_PLAYER_START};

    inline constexpr std::array DECK_STRINGS{
        String{0, ""}, String{1, "CMODE_DECKBUILDER"}, String{28, "undefined"},
        String{30, "_ensureActionsAllowed"}, String{36, "DB"}, String{42, "getGameRules"},
        String{54, "_isTempCurseActive"}, String{55, "getVariables"}, String{56, "getVariable"},
        String{57, "DB_CURSE_"}, String{58, "toUpperCase"}, String{59, "_UNTIL"},
        String{60, "readDataInt32"}, String{61, "getCurrentDay"}, String{63, "__db_curse_patched"},
        String{64, "canBePerformed"}, String{66, "function"}, String{67, "getOwner"},
        String{69, "call"}, String{70, "ACTION_CAPTURE"}, String{71, "getTargetUnit"},
        String{72, "embargo"}, String{78, "_participates"}, String{80, "getRuleValue"},
        String{81, "_isHumanSeat"}, String{82, "_hasPendingPick"}, String{83, "hasVictoryRule"},
        String{84, "getCurrentPlayer"}, String{85, "pendingDialogState"}, String{86, "hasPick"},
        String{87, "ACTION_NEXT_PLAYER"}, String{88, "__db_pending_pick_patched"}, String{89, "perform"},
        String{90, "_markPendingLocalShow"}, String{91, "PLAYER"}, String{92, "__db_patched"},
        String{93, "startOfTurn"}, String{94, "e"}, String{95, "logErr"},
        String{96, "origStart threw: "}, String{97, "getVictoryRule"}, String{98, "DB_OPTED_OUT"},
        String{99, "ensurePerksAttached"}, String{100, "power_drain"}, String{101, "getCO"},
        String{102, "setPowerFilled"}, String{103, "snapshotDeeds"}, String{104, "PICK_"},
        String{105, "getPlayerID"}, String{106, "shouldTick"}, String{107, "startPick"},
        String{108, "cadence"}, String{109, "DB_BONUS_PENDING"}, String{110, "bonus"},
        String{111, "__db_endpatched"}, String{112, "endOfTurn"}, String{113, "origEnd threw: "},
        String{114, "tallyDeeds"}, String{127, "map"}, String{129, "player"},
        String{130, "cardId"}, String{131, "origCanBePerformed"}, String{132, "action"},
        String{133, "origPerform"}, String{134, "origStart"}, String{135, "origEnd"}};
    inline constexpr std::array DECK_IS_TEMP_CURSE_ACTIVE_RECORDS{
        CommandDeclaration::Function{"_isTempCurseActive", 319, "46292bf773c3863c10448fcac471a74d041512c24166d26019e2ad657b05645d"}};
    inline constexpr std::array DECK_IS_TEMP_CURSE_ACTIVE_FORMALS{std::string_view{"player"}, std::string_view{"cardId"}, std::string_view{"map"}};
    inline constexpr Function DECK_IS_TEMP_CURSE_ACTIVE{DECK_IS_TEMP_CURSE_ACTIVE_RECORDS, 10, DECK_IS_TEMP_CURSE_ACTIVE_FORMALS, 0};
    inline constexpr std::array DECK_HAS_PENDING_PICK_RECORDS{
        CommandDeclaration::Function{"_hasPendingPick", 239, "3b99506c50b4c78884bf4c43324ac64ce9d5f8222a3550db2143bc0e559b3289"}};
    inline constexpr std::array DECK_HAS_PENDING_PICK_FORMALS{std::string_view{"map"}};
    inline constexpr Function DECK_HAS_PENDING_PICK{DECK_HAS_PENDING_PICK_RECORDS, 18, DECK_HAS_PENDING_PICK_FORMALS, 0};
    inline constexpr std::array DECK_CAPTURE_WRAPPER_RECORDS{
        CommandDeclaration::Function{"captureWrapper", 303, "52aa9262603531f0b598b2f89f9cab8fa4a28b04c9fc1a729ea30cafcd80ab08"}};
    inline constexpr std::array DECK_CAPTURE_WRAPPER_FORMALS{std::string_view{"action"}, std::string_view{"map"}};
    inline constexpr Function DECK_CAPTURE_WRAPPER{DECK_CAPTURE_WRAPPER_RECORDS, 14, DECK_CAPTURE_WRAPPER_FORMALS, 0};
    inline constexpr std::array DECK_CAPTURE_CREATOR_RECORDS{
        CommandDeclaration::Function{"captureCreator", 221, "e2029eb8b690b2340ea68725d28b1572cd89318d6536b159fea9605fb87ad1e5"}};
    inline constexpr Function DECK_CAPTURE_CREATOR{DECK_CAPTURE_CREATOR_RECORDS, 13, {}, 1};
    inline constexpr std::array DECK_NEXT_CAN_WRAPPER_RECORDS{
        CommandDeclaration::Function{"nextCanWrapper", 156, "35bb42d4993e31a2088e09e85d6ee3ed858c2dc2ec2a630a71d72d36f6ac29d0"}};
    inline constexpr std::array DECK_NEXT_CAN_WRAPPER_FORMALS{std::string_view{"action"}, std::string_view{"map"}};
    inline constexpr Function DECK_NEXT_CAN_WRAPPER{DECK_NEXT_CAN_WRAPPER_RECORDS, 20, DECK_NEXT_CAN_WRAPPER_FORMALS, 0};
    inline constexpr std::array DECK_NEXT_PERFORM_WRAPPER_RECORDS{
        CommandDeclaration::Function{"nextPerformWrapper", 218, "feb8f31ce12e125c3c96cfa49747da4bd2cdbba5945ce76cce7356040af1ae59"}};
    inline constexpr std::array DECK_NEXT_PERFORM_WRAPPER_FORMALS{std::string_view{"action"}, std::string_view{"map"}};
    inline constexpr Function DECK_NEXT_PERFORM_WRAPPER{DECK_NEXT_PERFORM_WRAPPER_RECORDS, 21, DECK_NEXT_PERFORM_WRAPPER_FORMALS, 0};
    inline constexpr std::array DECK_NEXT_CREATOR_RECORDS{
        CommandDeclaration::Function{"nextCreator", 264, "c24503a44c11541451f9e85a2ce52702990896d512e863c701497e08cd7e664a"}};
    inline constexpr Function DECK_NEXT_CREATOR{DECK_NEXT_CREATOR_RECORDS, 19, {}, 2};
    inline constexpr std::array DECK_START_WRAPPER_RECORDS{
        CommandDeclaration::Function{"startWrapper", 1042, "b06b377ced0481876bfaef3d157997ecbe2a1905bc48b75354b26a74aa439a22"}};
    inline constexpr std::array DECK_START_WRAPPER_FORMALS{std::string_view{"player"}, std::string_view{"map"}};
    inline constexpr Function DECK_START_WRAPPER{DECK_START_WRAPPER_RECORDS, 23, DECK_START_WRAPPER_FORMALS, 0};
    inline constexpr std::array DECK_START_CREATOR_RECORDS{
        CommandDeclaration::Function{"startCreator", 185, "0be1546a3e320eeaf81fa6146102f1cb71018b2a634b5e773a3fe17bd48926aa"}};
    inline constexpr Function DECK_START_CREATOR{DECK_START_CREATOR_RECORDS, 22, {}, 1};
    inline constexpr std::array DECK_END_WRAPPER_RECORDS{
        CommandDeclaration::Function{"endWrapper", 471, "020ce674c7146e0ac83f34926a2e948aecb1795ff62ca70f078d38aa133bef8a"}};
    inline constexpr std::array DECK_END_WRAPPER_FORMALS{std::string_view{"player"}, std::string_view{"map"}};
    inline constexpr Function DECK_END_WRAPPER{DECK_END_WRAPPER_RECORDS, 25, DECK_END_WRAPPER_FORMALS, 0};
    inline constexpr std::array DECK_END_CREATOR_RECORDS{
        CommandDeclaration::Function{"endCreator", 185, "d60bddff9af90fcaca941de3f62f7d7f16aded1afff472a670e09049006bb905"}};
    inline constexpr Function DECK_END_CREATOR{DECK_END_CREATOR_RECORDS, 24, {}, 1};
    inline constexpr std::array DECK_CONSTANTS{
        Constant{1, UINT64_C(0x0003c00000000000)},
        Constant{2, UINT64_C(0x0003c00000000001)}};
    inline constexpr std::array DECK_CATCHES{CatchBlock{5, 94}, CatchBlock{7, 94}};
    inline constexpr Module DECK{"/mods/custom_gamemodes/scripts/gamerules/victory/cmode_deckbuilder.js", DECK_STRINGS, DECK_CONSTANTS, {}, DECK_CATCHES};
    inline constexpr std::array DECK_HELPERS{DECK_IS_TEMP_CURSE_ACTIVE, DECK_HAS_PENDING_PICK};

    inline constexpr std::array PRISM_STRINGS{
        String{0, ""}, String{1, "ACTION_CAPTURE"}, String{2, "newCanBePerformed"},
        String{4, "getTargetUnit"}, String{5, "getActionTarget"}, String{6, "getGameRules"},
        String{7, "getCoGlobalD2D"}, String{8, "getPlayerCount"}, String{9, "getPlayer"},
        String{10, "isEnemy"}, String{11, "getOwner"}, String{12, "getCO"},
        String{13, "getCoID"}, String{14, "CO_SCRAMBLE"}, String{15, "CO"},
        String{16, "isActive"}, String{17, "getPowerMode"}, String{18, "GameEnums"},
        String{19, "PowerMode_Off"}, String{20, "dx"}, String{21, "dy"},
        String{22, "length"}, String{23, "x"}, String{24, "y"},
        String{25, "onMap"}, String{26, "getTerrain"}, String{27, "getUnit"},
        String{28, "getUnitType"}, String{29, "UnitType_Infantry"}, String{30, "UnitType_Ground"},
        String{33, "action"}, String{34, "map"}};
    inline constexpr std::array PRISM_CAPTURE_WRAPPER_RECORDS{
        CommandDeclaration::Function{"captureWrapper", 996, "7ac8b078bee9c9c763434e5f24d7ae7b732d1f36d6a0a1f96310c84ef52b2089"}};
    inline constexpr std::array PRISM_CAPTURE_WRAPPER_FORMALS{std::string_view{"action"}, std::string_view{"map"}};
    inline constexpr Function PRISM_CAPTURE_WRAPPER{PRISM_CAPTURE_WRAPPER_RECORDS, 1, PRISM_CAPTURE_WRAPPER_FORMALS, 0};
    inline constexpr std::array PRISM_CONSTANTS{
        Constant{0, UINT64_C(0x0003c00000000000)},
        Constant{1, UINT64_C(0x0003c00000000001)}};
    inline constexpr std::array<std::uint32_t, 2> PRISM_SHAPE_0{20, 21};
    inline constexpr std::array<std::uint32_t, 2> PRISM_SHAPE_1{20, 21};
    inline constexpr std::array<std::uint32_t, 2> PRISM_SHAPE_2{20, 21};
    inline constexpr std::array<std::uint32_t, 2> PRISM_SHAPE_3{20, 21};
    inline constexpr std::array PRISM_SHAPES{Shape{0, PRISM_SHAPE_0}, Shape{1, PRISM_SHAPE_1}, Shape{2, PRISM_SHAPE_2}, Shape{3, PRISM_SHAPE_3}};
    inline constexpr Module PRISM{"/mods/zzzzprism_star/scripts/actions/ACTION_CAPTURE.js", PRISM_STRINGS, PRISM_CONSTANTS, PRISM_SHAPES, {}};

    inline constexpr std::array COMMAND_RANKING_STRINGS{
        String{123, "String"},
        String{126, "UNITRANKINGSYSTEM"},
        String{256, "isDefaultOnPolicyEnabled"},
        String{257, "UNIT_BADGE_KEY"},
        String{291, "readUnitOrigin"},
        String{296, "ICON_2"},
        String{297, "ICON_1"},
        String{330, "getImageSize"},
        String{346, "icon"},
        String{413, "usesUnitBadges"},
        String{414, "ICON_FOREIGN"},
        String{415, "getUnitBadgeIcon"},
        String{416, "getUnitBadgeKey"},
        String{417, "unitBadgeState"},
        String{418, "inset"},
        String{419, "loadIcon"},
        String{420, "unloadIcon"},
        String{421, "refreshBadge"},
        String{1318, "UNITRANKINGSYSTEM.unloadIcons.CO_COMMAND"},
        String{1319, "unloadIcons"},
        String{1320, "restoreBadge"},
        String{1338, "ensureRankingPatch"},
        String{1398, "activeRule"},
        String{1399, "forceLoad"}};
    inline constexpr std::array COMMAND_RESTORE_BADGE_RECORDS{
        CommandDeclaration::Function{"restoreBadge", 162, "05a2151c4ee72f61e2a4e6adbeb163b476af57a137b4ad4ac3cfcf0ff24178ca"}};
    inline constexpr std::array COMMAND_RESTORE_BADGE_FORMALS{std::string_view{"unit"}, std::string_view{"map"}};
    inline constexpr Function COMMAND_RESTORE_BADGE{COMMAND_RESTORE_BADGE_RECORDS, 85, COMMAND_RESTORE_BADGE_FORMALS, 0};
    inline constexpr std::array COMMAND_REFRESH_BADGE_RECORDS{
        CommandDeclaration::Function{"refreshBadge", 841, "6e8e8f85f6dd30db679f1e87cdc0bae3e5b61c4e4249af7a6328d4b28fe875eb"}};
    inline constexpr std::array COMMAND_REFRESH_BADGE_FORMALS{std::string_view{"unit"}, std::string_view{"map"}, std::string_view{"origin"}, std::string_view{"activeRule"}, std::string_view{"forceLoad"}};
    inline constexpr Function COMMAND_REFRESH_BADGE{COMMAND_REFRESH_BADGE_RECORDS, 83, COMMAND_REFRESH_BADGE_FORMALS, 0};
    inline constexpr std::array COMMAND_GET_UNIT_BADGE_ICON_RECORDS{
        CommandDeclaration::Function{"getUnitBadgeIcon", 506, "6065627ae33d9df633230409034a96f0c7328202cca64b916505f59c76677113"}};
    inline constexpr std::array COMMAND_GET_UNIT_BADGE_ICON_FORMALS{std::string_view{"unit"}, std::string_view{"map"}, std::string_view{"origin"}, std::string_view{"activeRule"}};
    inline constexpr Function COMMAND_GET_UNIT_BADGE_ICON{COMMAND_GET_UNIT_BADGE_ICON_RECORDS, 82, COMMAND_GET_UNIT_BADGE_ICON_FORMALS, 0};
    inline constexpr std::array COMMAND_GET_UNIT_BADGE_KEY_RECORDS{
        CommandDeclaration::Function{"getUnitBadgeKey", 212, "f452e4d80b4cc75816f9982176ec57cfdcce593e6b5b6a10a3770385d3bb912a"}};
    inline constexpr std::array COMMAND_GET_UNIT_BADGE_KEY_FORMALS{std::string_view{"unit"}};
    inline constexpr Function COMMAND_GET_UNIT_BADGE_KEY{COMMAND_GET_UNIT_BADGE_KEY_RECORDS, 81, COMMAND_GET_UNIT_BADGE_KEY_FORMALS, 0};
    inline constexpr std::array COMMAND_USES_UNIT_BADGES_RECORDS{
        CommandDeclaration::Function{"usesUnitBadges", 134, "d791c0945cb28b7394b604f9c9b7f9c273309079f98ae992a197cf5cca49c29e"}};
    inline constexpr std::array COMMAND_USES_UNIT_BADGES_FORMALS{std::string_view{"map"}};
    inline constexpr Function COMMAND_USES_UNIT_BADGES{COMMAND_USES_UNIT_BADGES_RECORDS, 22, COMMAND_USES_UNIT_BADGES_FORMALS, 0};
    inline constexpr std::array COMMAND_RANKING_CREATOR_RECORDS{
        CommandDeclaration::Function{"ensureRankingPatch", 315, "bcb33a28f7bb9c9be52888f6b78d06e6c664ced38df9104f41f1d65c7e170c1c"}};
    inline constexpr Function COMMAND_RANKING_CREATOR{COMMAND_RANKING_CREATOR_RECORDS, 489, {}, 1};
    inline constexpr std::array COMMAND_RANKING_WRAPPER_RECORDS{
        CommandDeclaration::Function{"rankingWrapper", 316, "078a471a7c47228a2e33cbe9a2a189c33c9c6e86c0508a56b6775a65b4f53628"}};
    inline constexpr std::array COMMAND_RANKING_WRAPPER_FORMALS{std::string_view{"unit"}};
    inline constexpr Function COMMAND_RANKING_WRAPPER{COMMAND_RANKING_WRAPPER_RECORDS, 490, COMMAND_RANKING_WRAPPER_FORMALS, 0};
    inline constexpr std::array<std::uint32_t, 2> COMMAND_RANKING_SHAPE_26{346, 418};
    inline constexpr std::array COMMAND_RANKING_SHAPES{Shape{26, COMMAND_RANKING_SHAPE_26}};
    inline constexpr Module COMMAND_RANKING_EXTRA{"/mods/custom_gamemodes/scripts/general/cmode_co_command.js", COMMAND_RANKING_STRINGS, {}, COMMAND_RANKING_SHAPES, {}};
    inline constexpr std::array COMMAND_RANKING_HELPERS{COMMAND_RESTORE_BADGE, COMMAND_REFRESH_BADGE, COMMAND_GET_UNIT_BADGE_ICON, COMMAND_GET_UNIT_BADGE_KEY, COMMAND_USES_UNIT_BADGES, COMMAND_IS_DEFAULT_ON_POLICY_ENABLED};
    inline constexpr std::array LEGACY_RANK_GET_OFFENSIVE_BONUS_RECORDS{
        CommandDeclaration::Function{"getOffensiveBonus", 262, "a84081dc4b194bd2ef5956af4dd7c3a41caaa4d892d1798ab59b4768d8930eb3"}};
    inline constexpr std::array LEGACY_RANK_GET_OFFENSIVE_BONUS_FORMALS{std::string_view{"unit"}};
    inline constexpr Function LEGACY_RANK_GET_OFFENSIVE_BONUS{LEGACY_RANK_GET_OFFENSIVE_BONUS_RECORDS, 8, LEGACY_RANK_GET_OFFENSIVE_BONUS_FORMALS, 0};
    inline constexpr std::array LEGACY_RANK_GET_DEFENSIVE_BONUS_RECORDS{
        CommandDeclaration::Function{"getDefensiveBonus", 196, "205e65e0f4b9332879cc52cc31d4d749e07752296cbc45e44a4f9d51148b0eb0"}};
    inline constexpr std::array LEGACY_RANK_GET_DEFENSIVE_BONUS_FORMALS{std::string_view{"unit"}};
    inline constexpr Function LEGACY_RANK_GET_DEFENSIVE_BONUS{LEGACY_RANK_GET_DEFENSIVE_BONUS_RECORDS, 9, LEGACY_RANK_GET_DEFENSIVE_BONUS_FORMALS, 0};
    inline constexpr std::array LEGACY_RANK_IS_ELIGIBLE_UNIT_RECORDS{
        CommandDeclaration::Function{"isEligibleUnit", 122, "a7f4d85f3bf67e819bf5d71b598090b13f775d112d1f66b1b237f21b351fd669"}};
    inline constexpr std::array LEGACY_RANK_IS_ELIGIBLE_UNIT_FORMALS{std::string_view{"unit"}};
    inline constexpr Function LEGACY_RANK_IS_ELIGIBLE_UNIT{LEGACY_RANK_IS_ELIGIBLE_UNIT_RECORDS, 4, LEGACY_RANK_IS_ELIGIBLE_UNIT_FORMALS, 0};
    inline constexpr std::array LEGACY_RANK_OFFENSIVE_DELEGATE_RECORDS{
        CommandDeclaration::Function{"offensiveDelegate", 94, "d9494de6868e6eb09a8a1b53de875aaa4029847250e465fa09ad0f8a7398bda0"}};
    inline constexpr std::array LEGACY_RANK_OFFENSIVE_DELEGATE_FORMALS{std::string_view{"unit"}, std::string_view{"map"}};
    inline constexpr Function LEGACY_RANK_OFFENSIVE_DELEGATE{LEGACY_RANK_OFFENSIVE_DELEGATE_RECORDS, 16, LEGACY_RANK_OFFENSIVE_DELEGATE_FORMALS, 0};
    inline constexpr std::array LEGACY_RANK_DEFENSIVE_DELEGATE_RECORDS{
        CommandDeclaration::Function{"defensiveDelegate", 94, "170eac2cde9fb7e4d838a43d3c1503e45f60648edf53d96edee2abce1f0590c8"}};
    inline constexpr std::array LEGACY_RANK_DEFENSIVE_DELEGATE_FORMALS{std::string_view{"unit"}, std::string_view{"map"}};
    inline constexpr Function LEGACY_RANK_DEFENSIVE_DELEGATE{LEGACY_RANK_DEFENSIVE_DELEGATE_RECORDS, 17, LEGACY_RANK_DEFENSIVE_DELEGATE_FORMALS, 0};
    inline constexpr std::array LEGACY_RANK_STRINGS{
        String{0, ""},
        String{1, "LEGACY_COMMAND_UNIT_RANKS"},
        String{9, "undefined"},
        String{11, "GameEnums"},
        String{15, "isEligiblePlayer"},
        String{16, "getOwner"},
        String{23, "isEligibleUnit"},
        String{26, "getUnitRank"},
        String{27, "UnitRank_None"},
        String{37, "getOffensiveBonus"},
        String{42, "getDefensiveBonus"},
        String{83, "unit"},
        String{84, "map"}};
    inline constexpr Module LEGACY_RANK{"/mods/Legacy Command/scripts/general/co_unit_ranks.js", LEGACY_RANK_STRINGS, {}, {}, {}};
    inline constexpr std::array COMMAND_RANK_ELIGIBILITY_CREATOR_RECORDS{
        CommandDeclaration::Function{"eligibilityCreator", 504, "2f65af19b7565ce17c2ab198ba24b03a66c04553640e8df4f26a4b1a9acb6078"},
        CommandDeclaration::Function{"eligibilityCreator", 504, "072453ddd53b772c30e689ddbf3068670c7f52e88b1c1a883381fb67b6c07008"}};
    inline constexpr Function COMMAND_RANK_ELIGIBILITY_CREATOR{COMMAND_RANK_ELIGIBILITY_CREATOR_RECORDS, 412, {}, 3};
    inline constexpr std::array COMMAND_RANK_ELIGIBILITY_CREATOR_LOCALS{std::string_view{"object"}, std::string_view{"originalEligible"}, std::string_view{"originalProvider"}};
    inline constexpr std::array COMMAND_RANK_ELIGIBILITY_WRAPPER_RECORDS{
        CommandDeclaration::Function{"eligibilityWrapper", 378, "f8071daa61b43155e1bfa4dc2dfe45a9775dbbb6f55c219377b754434803c512"},
        CommandDeclaration::Function{"eligibilityWrapper", 378, "2cde505dac956a13e368e97f5bc4a1405615402bf5ff1cf66b87bdcf2430bdb9"}};
    inline constexpr std::array COMMAND_RANK_ELIGIBILITY_WRAPPER_FORMALS{std::string_view{"unit"}};
    inline constexpr Function COMMAND_RANK_ELIGIBILITY_WRAPPER{COMMAND_RANK_ELIGIBILITY_WRAPPER_RECORDS, 413, COMMAND_RANK_ELIGIBILITY_WRAPPER_FORMALS, 0};
    inline constexpr std::array COMMAND_RANK_BONUS_STRINGS{
        String{1147, "LEGACY_COMMAND_UNIT_RANKS"},
        String{1148, "LEGACY_COMMAND_UNIT_RANKS.isEligibleUnit.CO_COMMAND"},
        String{1149, "isEligibleUnit"},
        String{1150, "isRankingRosterCO"},
        String{1151, "LEGACY_COMMAND_UNIT_RANKS.isRankProvider.CO_COMMAND"},
        String{1152, "isRankProvider"},
        String{1158, "ensureUnitRankPatches"}};
    inline constexpr Module COMMAND_RANK_BONUS{"/mods/custom_gamemodes/scripts/general/cmode_co_command.js", COMMAND_RANK_BONUS_STRINGS, {}, {}, {}};
}
