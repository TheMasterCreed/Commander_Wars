from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any

import pytest


TEST_ROOT = Path(__file__).resolve().parent
REPOSITORY_ROOT = TEST_ROOT.parent.parent
FIXTURE_ROOT = TEST_ROOT / "fixtures"
DRIVER_PATH = TEST_ROOT / "characterization_init.js"
GAME_RULES_FIXTURE = FIXTURE_ROOT / "game_rules_v32.grl"
GAME_RULES_METADATA = FIXTURE_ROOT / "game_rules_v32.json"
PRODUCTION_CASES = FIXTURE_ROOT / "production_cases.json"
USE_BUILDING_CASES = FIXTURE_ROOT / "use_building_cases.json"
SOURCE_MOD_SNAPSHOT = FIXTURE_ROOT / "source_mod_snapshot.json"
COUNTERPOINT_STRATEGY_PATH = (
    REPOSITORY_ROOT / "resources/aidata/normal/__counterpointai.js"
)
COUNTERPOINT_TUNABLES_PATH = (
    REPOSITORY_ROOT
    / "resources/aidata/normal/___counterpoint_user_tunables.js"
)
GENERAL_QRC_PATH = REPOSITORY_ROOT / "general.qrc"
SIMPLE_PRODUCTION_HEADER = (
    REPOSITORY_ROOT / "ai/productionSystem/simpleproductionsystem.h"
)
SIMPLE_PRODUCTION_SOURCE = (
    REPOSITORY_ROOT / "ai/productionSystem/simpleproductionsystem.cpp"
)
CORE_AI_SOURCE = REPOSITORY_ROOT / "ai/coreai.cpp"
NORMAL_AI_SOURCE = REPOSITORY_ROOT / "ai/normalai.cpp"
AI_BEHAVIOR_DISPATCH_PATH = (
    REPOSITORY_ROOT / "resources/aidata/normal/__aibehaviordispatch.js"
)
COUNTERPOINT_PROFILE_INI_PATH = (
    REPOSITORY_ROOT / "resources/aidata/normal/counterpoint.ini"
)
SPRITES_QRC_PATH = REPOSITORY_ROOT / "sprites.qrc"
RULE_SELECTION_XML = REPOSITORY_ROOT / "resources/ui/game/ruleSelection.xml"
RULE_SELECTION_SCRIPT = REPOSITORY_ROOT / "resources/ui/game/ruleSelectionScript.js"
TRANSLATION_CATALOGS = ("lang_en", "lang_de_DE", "lang_zh-tw")
AI_WRAPPER_PATHS = (
    REPOSITORY_ROOT / "resources/aidata/normal/normalai.js",
    REPOSITORY_ROOT / "resources/aidata/normal/normalaioffensive.js",
    REPOSITORY_ROOT / "resources/aidata/normal/normalaidefensive.js",
    REPOSITORY_ROOT / "resources/aidata/very_easy/veryeasyai.js",
)
GAME_ENUMS_HEADER = REPOSITORY_ROOT / "game/GameEnums.h"
GAME_ENUMS_SOURCE = REPOSITORY_ROOT / "game/GameEnums.cpp"

COMMANDER_WARS_EXE_ENV = "COMMANDER_WARS_EXE"
COMMANDER_WARS_OPENSSL_BIN_ENV = "COMMANDER_WARS_OPENSSL_BIN"
COMMANDER_WARS_QT_BIN_ENV = "COMMANDER_WARS_QT_BIN"
COUNTERPOINT_CAPTURE_ENV = "COUNTERPOINT_CAPTURE_V32_DEST"
COUNTERPOINT_SOURCE_MOD_ENV = "COUNTERPOINT_SOURCE_MOD"
COUNTERPOINT_TEST_OPT_IN = "COW_COUNTERPOINT_TEST"

SCENARIO_TOKEN = "__COUNTERPOINT_SCENARIO__"
RESULT_FILE_NAME = "counterpoint-result.json"
PROCESS_STDOUT_FILE_NAME = "process-stdout.log"
PROCESS_STDERR_FILE_NAME = "process-stderr.log"
CAPTURED_RULES_FILE_NAME = "captured_game_rules_v32.grl"
STANDARD_RULES_FILE_NAME = "game_rules_v33_standard.grl"
COUNTERPOINT_RULES_FILE_NAME = "game_rules_v33_counterpoint.grl"
INVALID_RULES_FILE_NAME = "game_rules_v33_invalid.grl"
PASS_MARKER = "COUNTERPOINT_TEST_PASS"
FAIL_MARKER = "COUNTERPOINT_TEST_FAIL:"
ASSERT_MARKER = "COUNTERPOINT_ASSERT_PASS:"
RULES_VERSION_BYTES = 4
GAME_RULES_V32 = 32
GAME_RULES_V33 = 33
INVALID_AI_BEHAVIOR = 99
RULES_UNIT_LIMIT_SENTINEL = 37
PROCESS_TIMEOUT_SECONDS = 120
STRATEGY_PROCESS_TIMEOUT_SECONDS = 60
EXPECTED_SOURCE_FILE_COUNT = 4
EXPECTED_CASE_COUNT = 6
SOURCE_ENEMY_PRUNE_RANGE = 5
BASE_REVISION = "1bff0a282a51781e325683b8fccf952804c8f65d"
COUNTERPOINT_STRATEGY_RESOURCE = "resources/aidata/normal/__counterpointai.js"
COUNTERPOINT_TUNABLES_RESOURCE = (
    "resources/aidata/normal/___counterpoint_user_tunables.js"
)

COUNTERPOINT_MUTABLE_PLANNING_FIELDS = {
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
}

COUNTERPOINT_FORBIDDEN_LEGACY_CALLBACKS = {
    "_borrowFromQueue",
    "_getTransportContext",
    "_refreshAAProbes",
    "_refreshCOContext",
    "_returnBudgetToQueue",
    "_runWeightedPickLoop",
    "_tryForcedBuild",
    "_updateMapContext",
    "_updateTankFerryStats",
}

COUNTERPOINT_REQUIRED_CALLBACKS = {
    "buildUnitSimpleProductionSystem",
    "getFactoryMenuItem",
    "initializeSimpleProductionSystem",
    "onBuildingMenuItemResult",
    "onNewBuildQueue",
    "prepareProduction",
}

COUNTERPOINT_REQUIRED_STATE_FIELDS = {
    "algorithmVersion",
    "day",
    "dynamicBaseline",
    "drawCounter",
    "generation",
    "heldFunds",
    "ordinaryRandomFundingJitter",
    "plans",
    "playerId",
    "schemaVersion",
    "seed",
    "specialRandomFundingJitter",
    "specialPrepared",
    "strategyVersion",
    "ordinaryPrepared",
    "turnTargets",
}

REQUIRED_CASES = {
    "fog_distance_prune",
    "free_black_hole_fallback",
    "free_nest_fallback",
    "modified_cost_boundary",
    "ordinary_factory",
    "seeded_selection",
}

DISPATCH_WRAPPERS = (
    "NORMALAI",
    "NORMALAIOFFENSIVE",
    "NORMALAIDEFENSIVE",
    "VERYEASYAI",
)
DISPATCH_SEAMS = (
    "initializeSimpleProductionSystem",
    "prepareProduction",
    "onNewBuildQueue",
    "buildUnitSimpleProductionSystem",
    "getBuildingMenuItem",
    "onBuildingMenuItemResult",
)
# Section 5.11 of the design; the ini values plain Normal switches to in Counterpoint mode.
COUNTERPOINT_PROFILE_SETTINGS = {
    "MinMovementDamage": 0.25,
    "NotAttackableDamage": 20,
    "EnemyPruneRange": 5,
    "InfluenceUnitRange": 2.5,
    "OwnIndirectAttackValue": 5,
    "EnemyKillBonus": 1.4,
    "EnemyCounterDamageMultiplier": 7,
    "SupportDamageBonus": 1.0,
    "InfluenceMultiplier": 0.6,
}

MODE_DISPATCH_SCENARIOS = (
    "mode_dispatch_standard",
    "mode_dispatch_counterpoint",
    "mode_dispatch_excluded",
)
DISPATCH_SURFACE_ASSERTIONS = (
    {
        "dispatch-counterpoint-no-vanilla-init",
        "dispatch-counterpoint-strategy",
        "dispatch-menu-table-not-shared",
        "dispatch-hidden-enemy-spawned",
        "dispatch-null-map-standard",
        "dispatch-prepare-counterpoint-only",
        "dispatch-prepare-once-per-turn",
        "dispatch-prepare-unpruned-enemies",
        "dispatch-queue-restores-full-army",
        "dispatch-standard-strategy",
        "dispatch-veryeasy-had-no-core-target",
        "profile-controller-present",
    }
    | {
        f"dispatch-seam-{wrapper}-{seam}"
        for wrapper in DISPATCH_WRAPPERS
        for seam in DISPATCH_SEAMS
    }
    | {f"dispatch-menu-routed-{wrapper}" for wrapper in DISPATCH_WRAPPERS}
    | {f"dispatch-menu-table-{wrapper}" for wrapper in DISPATCH_WRAPPERS}
)

SCENARIO_ASSERTIONS = {
    "use_building_actions": {
        "map-width",
        "map-height",
        "empty-army",
    },
    "counterpoint_strategy": {
        "strategy-adjacency-dilution",
        "strategy-classification",
        "strategy-mixed-pool-ordering",
        "strategy-classification-cost-isolation",
        "strategy-composition",
        "strategy-composition-grouping",
        "strategy-coreai-baseline",
        "strategy-coverage",
        "strategy-inverse-cost",
        "strategy-isolated-load",
        "strategy-large-roster",
        "strategy-namespaces-unchanged",
        "strategy-negative-fallback",
        "strategy-no-enemy-fallback",
        "strategy-normalization",
        "strategy-null-enemies",
        "strategy-scoring",
        "strategy-static-score-own-state-independent",
        "strategy-stateless-surface",
        "strategy-threat-analysis",
        "strategy-weighted-pick",
    },
    "fog_distance_prune": {
        "distance-pruned",
        "full-enemy-snapshot",
        "hidden-enemy",
    },
    "game_rules_v32": {
        "rules-sentinels",
        "rules-v32-standard",
        "rules-v32-version",
    },
    "game_rules_v33": {
        "rules-counterpoint-round-trip",
        "rules-default-standard",
        "rules-hash-mode",
        "rules-invalid-setter",
        "rules-invalid-stream",
        "rules-reset-standard",
        "rules-standard-round-trip",
        "rules-version",
    },
    "production_action_discovery": {
        "discovery-action-membership",
        "discovery-black-hole-available",
        "discovery-black-hole-occupied-door",
        "discovery-black-hole-shared-fire",
        "discovery-base-override-contract",
        "discovery-custom-contract",
        "discovery-factory-available",
        "discovery-funds-availability",
        "discovery-future-legality",
        "discovery-modified-cost",
        "discovery-nest-available",
        "discovery-non-unit-menu",
        "discovery-occupied-output",
        "discovery-read-only",
        "discovery-repeatable",
        "discovery-temporary-airport",
        "discovery-temporary-harbour",
        "discovery-unit-limit",
        "discovery-unsupported-action",
    },
    "counterpoint_production": {
        "planner-authoritative-actions",
        "planner-avoid-budget-base-skips",
        "planner-black-hole-door-plans",
        "planner-blocked-capturer-reserve",
        "planner-borrow-state-validation",
        "planner-bounded-state",
        "planner-callback-surface",
        "planner-carrier-needs-ground-cargo",
        "planner-cheapest-floors-first",
        "planner-counter-buys-better-now",
        "planner-counter-explicit-future-legality",
        "planner-counter-future-legal-unaffordable",
        "planner-counter-ground-aa-turn-saturation",
        "planner-counter-heavy-remains-valid",
        "planner-counter-indirect-turn-saturation",
        "planner-counter-plan-order-invariant",
        "planner-counter-relevant-future-coverage",
        "planner-counter-saving-cases",
        "planner-counter-shared-future-budget",
        "planner-coverage-before-defense",
        "planner-cross-type-indirect-saturation",
        "planner-canonical-ferry-modifiers",
        "planner-deferred-selection",
        "planner-deployment-unknown-fallback",
        "planner-deployment-order-independent",
        "planner-deployment-turn-boundaries",
        "planner-deterministic-state",
        "planner-disabled-candidate-revival",
        "planner-duplicate-demand-survives",
        "planner-duplicate-menu-identity",
        "planner-exact-coordinate-build",
        "planner-exact-indirect-stack",
        "planner-executed-counted-once",
        "planner-factory-specific-deployment",
        "planner-failed-borrow-rollback",
        "planner-ferry-binding-promotes",
        "planner-ferry-hulls-capped",
        "planner-ferry-outranks-counter",
        "planner-ferry-saving-cases",
        "planner-fighter-aa-specialist",
        "planner-floor-ground-capturers-only",
        "planner-floor-ignores-affordability",
        "planner-free-budget-isolation",
        "planner-global-rng-isolation",
        "planner-ground-aa-quota",
        "planner-handled-skip",
        "planner-indirect-setup-delay",
        "planner-late-rescan-baseline",
        "planner-live-cost-kinds",
        "planner-max-indirect-cap",
        "planner-modified-cost",
        "planner-negative-scorer-skip",
        "planner-negative-cost-ordering",
        "planner-offense-duplicate-composition-slots",
        "planner-offense-fourth-enters-top-three",
        "planner-offense-neutral-gap-reconstruction",
        "planner-offense-phantoms-one-slot",
        "planner-ordinary-rescan-unlocks",
        "planner-player-isolation",
        "planner-phase-action-validation",
        "planner-projected-aa-coverage",
        "planner-projected-round-trip",
        "planner-recycled-budget-unlocks",
        "planner-roster-all-facilities",
        "planner-round-trip",
        "planner-satisfied-ferry-refused",
        "planner-same-turn-duplicate-damping",
        "planner-skip-banks-ferry-money",
        "planner-special-result-handshake",
        "planner-successful-build-tracking",
        "planner-terrain-aware-deployment",
        "planner-terminal-live-failure",
        "planner-turn-one-capturer-precedence",
        "planner-third-indirect-penalty",
        "planner-unaffordable-candidate-order",
        "planner-unsupported-fallback",
        "planner-urgent-ferry-precedence",
    },
    **{
        scenario: DISPATCH_SURFACE_ASSERTIONS
        | (
            {"profile-offensive-controller",
             "profile-offensive-excluded",
             "profile-offensive-unchanged"}
            if scenario == "mode_dispatch_excluded"
            else {"profile-normal-controller", "profile-normal-ini-count"}
            | {f"profile-normal-{setting}" for setting in COUNTERPOINT_PROFILE_SETTINGS}
        )
        for scenario in MODE_DISPATCH_SCENARIOS
    },
}


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


@pytest.fixture(scope="session")
def commander_wars_exe() -> Path:
    configured = os.environ.get(COMMANDER_WARS_EXE_ENV)
    if configured is None:
        pytest.skip(f"Set {COMMANDER_WARS_EXE_ENV} to run engine characterization")
    executable = Path(configured).resolve()
    if not executable.is_file():
        pytest.fail(f"Commander Wars executable does not exist: {executable}")
    return executable


def test_production_fixture_matrix() -> None:
    document = load_json(PRODUCTION_CASES)
    cases = document["cases"]

    assert document["schema"] == 1
    assert len(cases) == EXPECTED_CASE_COUNT
    assert set(cases) == REQUIRED_CASES
    assert cases["ordinary_factory"]["expected"]["unit_id"] == "INFANTRY"
    assert cases["modified_cost_boundary"]["menu_cost"] > cases[
        "modified_cost_boundary"
    ]["base_cost"]
    assert cases["free_nest_fallback"]["expected"]["funds_delta"] == 0
    assert cases["free_black_hole_fallback"]["expected"]["funds_delta"] == 0
    assert cases["fog_distance_prune"]["expected"][
        "hidden_enemy_in_full_snapshot"
    ]
    assert cases["seeded_selection"]["expected"]["same_seed_same_first_build"]


def test_source_mod_snapshot() -> None:
    snapshot = load_json(SOURCE_MOD_SNAPSHOT)
    source_root_value = os.environ.get(COUNTERPOINT_SOURCE_MOD_ENV)

    assert snapshot["schema"] == 1
    assert len(snapshot["files"]) == EXPECTED_SOURCE_FILE_COUNT
    assert snapshot["observed"]["enemy_prune_range"] == SOURCE_ENEMY_PRUNE_RANGE

    if source_root_value is None:
        pytest.skip(f"Set {COUNTERPOINT_SOURCE_MOD_ENV} to verify the source snapshot")

    source_root = Path(source_root_value).resolve()
    actual_hashes = {
        relative_path: sha256(source_root / relative_path)
        for relative_path in snapshot["files"]
    }
    assert actual_hashes == snapshot["files"]

    core_source = (source_root / "aidata/normal/__coreai.js").read_text(
        encoding="utf-8"
    )
    normal_ini = (source_root / "aidata/normal/normal.ini").read_text(
        encoding="utf-8"
    )
    assert "EnemyPruneRange=5" in normal_ini
    assert "var buildList = player.getBuildList();" in core_source
    assert "if (costs[i] <= 0) { continue; }" in core_source
    assert "system.addForcedProduction([unitId]);" in core_source
    assert "_turnUnbuildable : {}" in core_source


def test_counterpoint_strategy_resources_are_isolated_and_system_scoped() -> None:
    strategy_source = COUNTERPOINT_STRATEGY_PATH.read_text(encoding="utf-8")
    tunables_source = COUNTERPOINT_TUNABLES_PATH.read_text(encoding="utf-8")
    qrc_source = GENERAL_QRC_PATH.read_text(encoding="utf-8")
    production_header = SIMPLE_PRODUCTION_HEADER.read_text(encoding="utf-8")
    production_source = SIMPLE_PRODUCTION_SOURCE.read_text(encoding="utf-8")
    core_ai_source = CORE_AI_SOURCE.read_text(encoding="utf-8")
    combined_source = strategy_source + tunables_source

    assert "var COUNTERPOINTAI" in strategy_source
    assert "var strategy = COUNTERPOINTAI;" in tunables_source
    assert "strategy." in tunables_source
    assert re.search(r"\bCOREAI\b", combined_source) is None
    assert "globals.randInt" not in combined_source
    assert "Math.random" not in combined_source
    assert "productionBuildings" not in strategy_source
    assert "_buildingPools" not in strategy_source
    assert "/*" not in combined_source
    assert "\N{EM DASH}" not in combined_source

    for field in COUNTERPOINT_MUTABLE_PLANNING_FIELDS:
        assert re.search(rf"\b{re.escape(field)}\b", strategy_source) is None
    for callback in COUNTERPOINT_FORBIDDEN_LEGACY_CALLBACKS:
        assert re.search(rf"\b{re.escape(callback)}\b", strategy_source) is None
    for callback in COUNTERPOINT_REQUIRED_CALLBACKS:
        assert re.search(
            rf"\b{re.escape(callback)}\s*:\s*function\b", strategy_source
        )
    for field in COUNTERPOINT_REQUIRED_STATE_FIELDS:
        assert re.search(rf"\b{re.escape(field)}\b", strategy_source)

    assert "PLANNER_STATE_VARIABLE_ID : \"COUNTERPOINT_STATE\"" in strategy_source
    assert re.search(r"\bSTRATEGY_VERSION\s*:\s*16\b", strategy_source)
    assert re.search(r"\bPLANNER_STATE_SCHEMA_VERSION\s*:\s*4\b", strategy_source)
    assert "_loadPlannerState" in strategy_source
    assert "_savePlannerState" in strategy_source
    assert "readDataListString" in strategy_source
    assert "writeDataListString" in strategy_source
    assert "JSON.parse" in strategy_source
    assert "JSON.stringify" in strategy_source

    for native_method in (
        "deriveCounterpointSeed",
        "estimateCounterpointDeploymentTurns",
        "executeCounterpointBuild",
        "getCounterpointBaseDamage",
    ):
        assert native_method in production_header
        assert native_method in production_source

    assert "candidate.ordinal" in strategy_source
    assert re.search(
        r"executeCounterpointBuild\s*\(\s*plan\.x\s*,\s*plan\.y\s*,"
        r"\s*resolved\.candidate\.id\s*,\s*resolved\.candidate\.ordinal\s*,"
        r"\s*resolved\.cost\s*\)",
        strategy_source,
    )
    game_enums_header = GAME_ENUMS_HEADER.read_text(encoding="utf-8")
    game_enums_source = GAME_ENUMS_SOURCE.read_text(encoding="utf-8")

    # The menu sentinels are one shared enum, so C++ and script cannot drift apart.
    assert "MenuSelection_Restart = -3," in game_enums_header
    assert "MenuSelection_Skip = -2," in game_enums_header
    assert 'value.setProperty("MenuSelection_Restart", MenuSelection_Restart);' in game_enums_source
    assert 'value.setProperty("MenuSelection_Skip", MenuSelection_Skip);' in game_enums_source

    assert "BuildingMenuResult::RestartBuildingScan" in core_ai_source
    assert "if (selection == GameEnums::MenuSelection_Restart)" in core_ai_source
    assert "if (selection == GameEnums::MenuSelection_Skip)" in core_ai_source
    assert "if (index == GameEnums::MenuSelection_Skip ||" in core_ai_source

    # Neither side may redefine the sentinel values locally.
    assert "MENU_SELECTION_SKIP" not in core_ai_source
    assert "MENU_SELECTION_RESTART" not in core_ai_source
    assert "MENU_SELECTION_SKIP" not in strategy_source
    assert "MENU_SELECTION_RESTART" not in strategy_source
    assert "GameEnums.MenuSelection_Skip" in strategy_source
    assert "GameEnums.MenuSelection_Restart" in strategy_source

    assert qrc_source.count(f"<file>{COUNTERPOINT_STRATEGY_RESOURCE}</file>") == 1
    assert qrc_source.count(f"<file>{COUNTERPOINT_TUNABLES_RESOURCE}</file>") == 1


def test_game_rules_v32_fixture_metadata() -> None:
    metadata = load_json(GAME_RULES_METADATA)
    fixture_bytes = GAME_RULES_FIXTURE.read_bytes()
    serialized_version = struct.unpack(">i", fixture_bytes[:RULES_VERSION_BYTES])[0]

    assert serialized_version == GAME_RULES_V32
    assert metadata["base_revision"] == BASE_REVISION
    assert metadata["version"] == GAME_RULES_V32
    assert metadata["sha256"] == sha256(GAME_RULES_FIXTURE)
    assert metadata["size"] == len(fixture_bytes)
    assert metadata["sentinels"] == {
        "fog_mode": "Fog_OfShroud",
        "unit_limit": RULES_UNIT_LIMIT_SENTINEL,
    }


def test_capture_game_rules_v32_fixture(
    commander_wars_exe: Path, tmp_path: Path
) -> None:
    destination_value = os.environ.get(COUNTERPOINT_CAPTURE_ENV)
    if destination_value is None:
        pytest.skip(f"Set {COUNTERPOINT_CAPTURE_ENV} to replace the v32 fixture")

    run_dir, _ = run_scenario(
        commander_wars_exe,
        tmp_path,
        "capture_game_rules_v32",
    )
    captured = run_dir / CAPTURED_RULES_FILE_NAME
    destination = Path(destination_value).resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(captured, destination)

    fixture_bytes = destination.read_bytes()
    serialized_version = struct.unpack(">i", fixture_bytes[:RULES_VERSION_BYTES])[0]
    assert serialized_version == GAME_RULES_V32


def test_game_rules_v32_round_trip(
    commander_wars_exe: Path, tmp_path: Path
) -> None:
    run_scenario(commander_wars_exe, tmp_path, "game_rules_v32")


def test_game_rules_v33_modes_round_trip(
    commander_wars_exe: Path, tmp_path: Path
) -> None:
    run_dir, _ = run_scenario(commander_wars_exe, tmp_path, "game_rules_v33")
    standard = (run_dir / STANDARD_RULES_FILE_NAME).read_bytes()
    counterpoint = (run_dir / COUNTERPOINT_RULES_FILE_NAME).read_bytes()
    invalid = (run_dir / INVALID_RULES_FILE_NAME).read_bytes()

    assert struct.unpack(">i", standard[:RULES_VERSION_BYTES])[0] == GAME_RULES_V33
    assert struct.unpack(">i", counterpoint[:RULES_VERSION_BYTES])[0] == GAME_RULES_V33
    assert struct.unpack(">i", standard[-RULES_VERSION_BYTES:])[0] == 0
    assert struct.unpack(">i", counterpoint[-RULES_VERSION_BYTES:])[0] == 1
    assert struct.unpack(">i", invalid[-RULES_VERSION_BYTES:])[0] == INVALID_AI_BEHAVIOR


def test_production_action_discovery_is_authoritative_and_read_only(
    commander_wars_exe: Path, tmp_path: Path
) -> None:
    run_scenario(commander_wars_exe, tmp_path, "production_action_discovery")


def test_counterpoint_strategy_is_isolated_bounded_and_deterministic(
    commander_wars_exe: Path, tmp_path: Path
) -> None:
    run_scenario(commander_wars_exe, tmp_path, "counterpoint_strategy")


def test_counterpoint_production_is_authoritative_and_system_scoped(
    commander_wars_exe: Path, tmp_path: Path
) -> None:
    run_scenario(commander_wars_exe, tmp_path, "counterpoint_production")


def test_ai_behavior_dispatch_resources_are_registered_and_late_bound() -> None:
    dispatch_source = AI_BEHAVIOR_DISPATCH_PATH.read_text(encoding="utf-8")
    profile_ini = COUNTERPOINT_PROFILE_INI_PATH.read_text(encoding="utf-8")
    qrc_source = GENERAL_QRC_PATH.read_text(encoding="utf-8")
    normal_ai_source = NORMAL_AI_SOURCE.read_text(encoding="utf-8")
    production_source = SIMPLE_PRODUCTION_SOURCE.read_text(encoding="utf-8")

    assert "<file>resources/aidata/normal/__aibehaviordispatch.js</file>" in qrc_source
    assert "<file>resources/aidata/normal/counterpoint.ini</file>" in qrc_source
    assert "\N{EM DASH}" not in dispatch_source + profile_ini

    # Resolve per call because save loading restores GameRules later.
    assert "var AI_BEHAVIOR_DISPATCH" in dispatch_source
    assert "rules.getAiBehaviorMode() === GameEnums.AiBehavior_Counterpoint" in dispatch_source
    assert re.search(r"^var\s+\w+\s*=\s*COREAI\b", dispatch_source, re.MULTILINE) is None
    assert re.search(r"^var\s+\w+\s*=\s*COUNTERPOINTAI\b", dispatch_source, re.MULTILINE) is None
    for seam in DISPATCH_SEAMS:
        assert re.search(rf"\b{re.escape(seam)}\s*:\s*function\b", dispatch_source)

    for wrapper_path in AI_WRAPPER_PATHS:
        wrapper_source = wrapper_path.read_text(encoding="utf-8")
        namespace = re.search(r"^var\s+(\w+)\s*=", wrapper_source, re.MULTILINE)
        assert namespace is not None, wrapper_path
        # Every wrapper used to scan NORMALAI's table regardless of its own namespace.
        foreign = {
            name
            for name in DISPATCH_WRAPPERS
            if name != namespace.group(1)
            and re.search(rf"\b{name}\.", wrapper_source)
        }
        assert not foreign, f"{wrapper_path.name} reaches into {sorted(foreign)}"
        assert "COREAI.getFactoryMenuItem" not in wrapper_source
        for seam in DISPATCH_SEAMS:
            assert re.search(rf"\b{re.escape(seam)}\s*:\s*function\b", wrapper_source)
        assert wrapper_source.count("getBuildingMenuItem : function") == 1

    # The profile is an overlay: only the nine documented keys may appear.
    overridden = dict(re.findall(r"^(\w+)\s*=\s*(\S+)$", profile_ini, re.MULTILINE))
    assert {
        key: float(value) for key, value in overridden.items()
    } == COUNTERPOINT_PROFILE_SETTINGS

    strategy_source = COUNTERPOINT_STRATEGY_PATH.read_text(encoding="utf-8")
    assert "COUNTERPOINTAI._fullEnemyUnits(ai, enemyUnits)" in strategy_source
    assert 'QStringLiteral("normal/counterpoint.ini")' in normal_ai_source
    assert "getLoadedIniCount(COUNTERPOINT_PROFILE_INI) == 0" in normal_ai_source
    assert "getAiType() == GameEnums::AiTypes_Normal" in normal_ai_source
    assert 'PREPARE_PRODUCTION_FUNCTION = QStringLiteral("prepareProduction")' in production_source


def test_rule_selection_exposes_the_ai_behavior_setting() -> None:
    rules_xml = RULE_SELECTION_XML.read_text(encoding="utf-8")
    rules_script = RULE_SELECTION_SCRIPT.read_text(encoding="utf-8")
    sprites_qrc = SPRITES_QRC_PATH.read_text(encoding="utf-8")

    ET.parse(RULE_SELECTION_XML)
    selector = re.search(
        r"<DropDownMenu>(?:(?!</DropDownMenu>).)*?getAiBehaviorValue.*?</DropDownMenu>",
        rules_xml,
        re.S,
    )
    assert selector is not None, "no AI behavior dropdown"
    block = selector.group(0)
    assert "RuleSelectionScript.getAiBehaviorRule()" in block
    assert "RuleSelectionScript.setAiBehaviorValue(input);" in block
    # In-game Rules must stay read-only.
    assert "<enabled>currentMenu.getRuleChangeEabled()</enabled>" in block
    ai_tab = re.search(
        r'<Tab>\s*<name>"ai"</name>(?:(?!</Tab>).)*?AI behavior:.*?</Tab>',
        rules_xml,
        re.S,
    )
    assert ai_tab is not None, "AI behavior selector is outside the AI tab"

    for helper in ("getAiBehaviorRule", "getAiBehaviorValue", "setAiBehaviorValue"):
        assert re.search(rf"\b{helper}\s*:\s*function\b", rules_script)
    assert 'qsTr("Standard"), qsTr("Counterpoint")' in rules_script
    assert "getAiBehaviorMode()" in rules_script and "setAiBehaviorMode(input)" in rules_script

    for language in TRANSLATION_CATALOGS:
        catalog = (REPOSITORY_ROOT / "translation" / f"{language}.ts").read_text(
            encoding="utf-8"
        )
        ET.fromstring(catalog)
        for message in ("AI behavior:", "Standard", "Counterpoint"):
            assert f"<source>{message}</source>" in catalog, (language, message)
        assert (REPOSITORY_ROOT / "resources/translation" / f"{language}.qm").is_file()
        assert f"<file>resources/translation/{language}.qm</file>" in sprites_qrc


def test_standard_mode_leaves_wrappers_and_profile_untouched(
    commander_wars_exe: Path, tmp_path: Path
) -> None:
    run_scenario(commander_wars_exe, tmp_path, "mode_dispatch_standard")


def test_counterpoint_mode_applies_the_normal_profile(
    commander_wars_exe: Path, tmp_path: Path
) -> None:
    run_scenario(commander_wars_exe, tmp_path, "mode_dispatch_counterpoint")


def test_counterpoint_profile_skips_excluded_controllers(
    commander_wars_exe: Path, tmp_path: Path
) -> None:
    run_scenario(commander_wars_exe, tmp_path, "mode_dispatch_excluded")


def test_fog_enemy_input_is_visibility_independent_and_distance_pruned(
    commander_wars_exe: Path, tmp_path: Path
) -> None:
    run_scenario(commander_wars_exe, tmp_path, "fog_distance_prune")


def test_standard_factory_characterization_is_repeatable(
    commander_wars_exe: Path, tmp_path: Path
) -> None:
    first = run_production_scenario(
        commander_wars_exe,
        tmp_path / "first",
        "ordinary_factory",
    )
    second = run_production_scenario(
        commander_wars_exe,
        tmp_path / "second",
        "ordinary_factory",
    )
    ordinary_case = load_json(PRODUCTION_CASES)["cases"]["ordinary_factory"]
    expected = ordinary_case["expected"]

    assert first == second
    assert first["unitId"] == expected["unit_id"]
    assert first["funds"] == expected["funds_after"]
    assert first["fundsBefore"] == ordinary_case["starting_funds"]
    assert first["playerId"] == 0
    assert first["buildCountBefore"] == 0
    assert first["buildCountAfter"] == 1
    assert first["seededMode"]


USE_BUILDING_MARKER = "COUNTERPOINT_USEBUILDING:"
USE_BUILDING_FACTORIES = {(2, 2), (3, 2)}


def extract_use_building_events(process_output: str) -> list[dict[str, Any]]:
    events = []
    for line in process_output.splitlines():
        start = line.find(USE_BUILDING_MARKER)
        if start < 0:
            continue
        payload = line[start + len(USE_BUILDING_MARKER):]
        # The engine appends "File: ... Line: ..." after the message.
        end = payload.find(" File: ")
        if end >= 0:
            payload = payload[:end]
        events.append(json.loads(payload.strip()))
    return events


def test_use_building_decision_sequence_is_characterized(
    commander_wars_exe: Path, tmp_path: Path
) -> None:
    _, output = run_scenario(
        commander_wars_exe, tmp_path / "first", "use_building_actions"
    )
    events = extract_use_building_events(output)
    expected = load_json(USE_BUILDING_CASES)["events"]

    for event in events:
        if "x" in event:
            assert (event["x"], event["y"]) in USE_BUILDING_FACTORIES, event

    stripped = [
        {key: value for key, value in event.items() if key not in ("x", "y")}
        for event in events
    ]
    expected_sequences = (expected, expected[:-1])
    assert stripped in expected_sequences

    # Verify every retry branch the scenario covers.
    responses = [e["response"] for e in events if e["kind"] == "menuItem"]
    assert 0 in responses, "no accepted first row"
    assert 1 in responses, "no accepted later row"
    assert -2 in responses, "MenuSelection_Skip never exercised"
    assert -3 in responses, "MenuSelection_Restart never exercised"
    assert False in responses, "declined selection never exercised"

    # Declining clears the script name, so only accepted calls report results.
    assert sum(1 for e in events if e["kind"] == "perform") == 3
    assert sum(1 for e in events if e["kind"] == "menuResult") == 2
    assert all(e["succeeded"] for e in events if e["kind"] == "menuResult")


def test_seeded_standard_selection_is_repeatable(
    commander_wars_exe: Path, tmp_path: Path
) -> None:
    first = run_production_scenario(
        commander_wars_exe,
        tmp_path / "first",
        "seeded_selection",
    )
    second = run_production_scenario(
        commander_wars_exe,
        tmp_path / "second",
        "seeded_selection",
    )
    seeded_case = load_json(PRODUCTION_CASES)["cases"]["seeded_selection"]

    assert first == second
    assert first["seededMode"]
    assert first["resetInitialProduction"]
    assert first["seed"] == seeded_case["seed"]
    assert first["unitId"] in seeded_case["build_list"]


def run_production_scenario(
    executable: Path, run_root: Path, scenario: str
) -> dict[str, Any]:
    run_dir, _ = run_scenario(executable, run_root, scenario)
    result_path = run_dir / RESULT_FILE_NAME
    assert result_path.is_file(), f"Missing production result: {result_path}"
    return load_json(result_path)


def run_scenario(
    executable: Path, run_root: Path, scenario: str
) -> tuple[Path, str]:
    run_dir = run_root / scenario
    run_dir.mkdir(parents=True)
    driver = DRIVER_PATH.read_text(encoding="utf-8")
    assert driver.count(SCENARIO_TOKEN) == 1
    (run_dir / "init.js").write_text(
        driver.replace(SCENARIO_TOKEN, scenario),
        encoding="utf-8",
    )
    if scenario == "game_rules_v32":
        shutil.copyfile(GAME_RULES_FIXTURE, run_dir / GAME_RULES_FIXTURE.name)

    environment = os.environ.copy()
    environment[COUNTERPOINT_TEST_OPT_IN] = "1"
    runtime_paths = [
        path
        for variable in (COMMANDER_WARS_QT_BIN_ENV, COMMANDER_WARS_OPENSSL_BIN_ENV)
        if (path := os.environ.get(variable)) is not None
    ]
    if runtime_paths:
        environment["PATH"] = os.pathsep.join(
            (*runtime_paths, environment.get("PATH", ""))
        )

    command = [
        str(executable),
        "--noUi",
        "--noAudio",
        "--debugLevel",
        "1",
        "--spawnAiProcess",
        "0",
        "--mods",
        "",
        "--userPath",
        str(run_dir),
    ]
    timeout_seconds = (
        STRATEGY_PROCESS_TIMEOUT_SECONDS
        if scenario == "counterpoint_strategy"
        else PROCESS_TIMEOUT_SECONDS
    )
    stdout_path = run_dir / PROCESS_STDOUT_FILE_NAME
    stderr_path = run_dir / PROCESS_STDERR_FILE_NAME
    try:
        with (
            stdout_path.open("w", encoding="utf-8") as stdout_file,
            stderr_path.open("w", encoding="utf-8") as stderr_file,
        ):
            completed = subprocess.run(
                command,
                cwd=executable.parent,
                env=environment,
                stdout=stdout_file,
                stderr=stderr_file,
                text=True,
                timeout=timeout_seconds,
                check=False,
            )
    except subprocess.TimeoutExpired as error:
        pytest.fail(
            f"Scenario {scenario} exceeded {timeout_seconds} seconds: {error}"
        )

    log_paths = sorted(run_dir.glob("console*.log"))
    logs = "\n".join(path.read_text(encoding="utf-8") for path in log_paths)
    process_output = "\n".join(
        (
            stdout_path.read_text(encoding="utf-8"),
            stderr_path.read_text(encoding="utf-8"),
            logs,
        )
    )

    assert completed.returncode == 0, process_output
    assert log_paths, f"Scenario {scenario} did not create a console log"
    assert process_output.count(PASS_MARKER) == 1, process_output
    assert FAIL_MARKER not in process_output, process_output
    for assertion in SCENARIO_ASSERTIONS.get(scenario, set()):
        assert f"{ASSERT_MARKER}{assertion}" in process_output, process_output
    return run_dir, process_output
