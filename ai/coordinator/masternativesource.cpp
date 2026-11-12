#include "ai/coordinator/masternativesource.h"
#include "ai/coordinator/guardedmethodrequirements.h"

#include <array>
#include <algorithm>
#include <type_traits>
#include <cmath>
#include <limits>

#include <QPointer>
#include <QThread>
#include <QCryptographicHash>
#include <QDataStream>

#include "ai/coordinator/guardednativecode.h"
#include "ai/coordinator/guardednativecalendar.h"
#include "ai/coordinator/guardedcommandpolicy.h"
#include "ai/coordinator/guardednativepresentation.h"
#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinatedai.h"
#include "ai/normalai.h"
#include "coreengine/globalutils.h"
#include "coreengine/mainapp.h"
#include "coreengine/settings.h"
#include "coreengine/interpreter.h"
#include "coreengine/jsthis.h"
#include "game/GameEnums.h"
#include "game/building.h"
#include "game/co.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"
#include "game/victoryrule.h"
#include "game/weather.h"
#include "game/weaponrangecheck.h"
#include "game/gameanimation/gameanimationfactory.h"
#include "gameinput/basegameinputif.h"

namespace Coordinator
{
    namespace
    {
        constexpr std::int64_t SOURCE_CONTROL_WORK = 1024;
        constexpr std::int64_t DISPATCH_ENTRY_WORK = 128;
        constexpr std::int64_t RECEIVER_ENTRY_WORK = 256;
        const QString GLOBAL_ALIAS = QStringLiteral("Global");
        const QString MAP_ALIAS = QStringLiteral("map");
        const QByteArray COMMAND_POLICY_REVISION = QByteArrayLiteral("command-active-empty-co-units-qv4-6.8.3-v13");
        const QString GLOBALS_ALIAS = QStringLiteral("globals");
        const QString ANIMATION_FACTORY_ALIAS = QStringLiteral("GameAnimationFactory");
        const std::array RECEIVER_GLOBAL_ALIASES{
            MAP_ALIAS, GLOBALS_ALIAS, ANIMATION_FACTORY_ALIAS, QString::fromLatin1(GUARDED_NATIVE_SETTINGS_ALIAS)};
        const QString DAMAGE_TABLE = QStringLiteral("damageTable");
        const QString INFANTRY_WEAPON = QStringLiteral("WEAPON_INFANTRY_MG");
        const QString ANDY_CO = QStringLiteral("CO_ANDY");
        const QString JESS_CO = QStringLiteral("CO_JESS");
        const QString ANDY_OFFENSE = QStringLiteral("d2dCoZoneOffBonus");
        const QString JESS_ZONE_OFFENSE = QStringLiteral("d2dCoZoneInfBonus");
        const QString JESS_GLOBAL_OFFENSE = QStringLiteral("d2dInfBonus");
        constexpr std::size_t CODE_KIND_COUNT = static_cast<std::size_t>(GuardedNativeCodeKind::Count);
        constexpr std::int64_t NOMINAL_RECORD_WORK = 64 + 8 * CODE_KIND_COUNT;
        constexpr std::size_t MAX_CONSTRUCTION_QUERIES = 512;
        constexpr qsizetype MAX_CONSTRUCTION_TYPE = 128;
        constexpr std::int64_t CONSTRUCTION_QUERY_CONTROL_WORK = 128;
        constexpr std::int64_t CONSTRUCTION_QUERY_CHARACTER_WORK = 8;
        constexpr std::int64_t CONSTRUCTION_INDEX_ENTRY_WORK =
            DISPATCH_ENTRY_WORK + CONSTRUCTION_QUERY_CHARACTER_WORK * MAX_CONSTRUCTION_TYPE;
        const QString HOVERCRAFT_FACTORY_RULE = QStringLiteral("GAMERULE_HOVERCRAFT_FACTORIES");
        constexpr std::int64_t ROOT_PROJECTION_CONTROL_WORK = 1024;
        constexpr std::int64_t ROOT_FIELD_WORK = 64;
        constexpr std::int64_t ROOT_PATH_WORK = 128;
        constexpr std::int32_t OFFENSE_BASE = 100;
        constexpr std::int32_t DEFENSE_BASE = 100;
        constexpr std::int32_t NATIVE_INCOME_MILLI_FACTOR = 1000;
        constexpr std::int64_t MAX_NATIVE_UNIT_COST = 10000000;
        constexpr std::int64_t ROOT_IDENTITY_HEADER_BYTES = 256;
        constexpr std::int64_t ROOT_IDENTITY_FIELD_BYTES = 8;
        constexpr std::int64_t ROOT_ACTOR_IDENTITY_FIELDS = 12;
        constexpr std::int64_t ROOT_PROPERTY_IDENTITY_FIELDS = 4;
        constexpr std::int64_t ROOT_ACTION_IDENTITY_FIELDS = 9;
        constexpr std::int64_t ROOT_POINT_IDENTITY_BYTES = 8;
        constexpr std::int64_t ROOT_OCCUPANCY_COMPARE_WORK = 4;
        constexpr std::int64_t BASIS_COMPARISON_WORK = 524288;
        constexpr std::int64_t IDENTITY_BYTE_PASSES = 8;
        constexpr std::int64_t MODULE_RECORD_WORK = 4096;
        constexpr std::int64_t FRESH_CONTROL_WORK = 4096;
        constexpr std::int64_t COMMAND_AUTHENTICATION_SHARE = 4;
        constexpr std::int64_t DEFAULT_ARMY_REPLAY_WORK = 2 * GUARDED_NATIVE_BUILD_ENTRY_WORK;
        constexpr std::size_t MAX_COLLECTION_CHECKPOINTS = 32;
        constexpr std::int64_t COLLECTION_CHECKPOINT_WORK = 64;
        constexpr std::int64_t COLLECTION_COST_BYTE_PASSES = 4;
        constexpr std::int64_t COLLECTION_COST_EXPORT_WORK = 1024;
        constexpr std::int64_t COLLECTION_COST_WORK =
            MAX_COLLECTION_CHECKPOINTS * COLLECTION_CHECKPOINT_WORK +
            COLLECTION_COST_BYTE_PASSES * sizeof(MasterNativeCollectionCosts) +
            COLLECTION_COST_EXPORT_WORK * static_cast<std::size_t>(MasterNativeCollectionStage::Count);
        const QByteArray NATIVE_AUTHORITY_REVISION{"guarded-native-owned-town-v3/native-terminal-v4"};
        const QByteArray UNLIMITED_AMMUNITION_REVISION{"native-unlimited-ammunition-v1"};
        constexpr std::int32_t AVAILABLE_CONTINUATION_ROUNDS = 1;
        enum class NativeModuleProjection : std::int8_t
        {
            Actor,
            Co,
            StockAction,
            Property,
            Movement,
            Context,
            Terrain,
            Policy,
            CurrentFire,
            Weapon,
            UnitHelper,
        };
        enum class NativeReceiverKind : std::int8_t
        {
            Map,
            Unit,
            Player,
            Co,
            Property,
            Terrain,
            Rules,
            VictoryRule,
            Weather,
            Globals,
            AnimationFactory,
            NormalInput,
            CoordinatedInput,
            Count,
        };
        constexpr std::size_t RECEIVER_KIND_COUNT = static_cast<std::size_t>(NativeReceiverKind::Count);
        constexpr auto & MAP_METHODS = NativeMethodRequirements::Stock::MAP_METHODS;
        constexpr auto & UNIT_METHODS = NativeMethodRequirements::Stock::UNIT_METHODS;
        constexpr auto & PLAYER_METHODS = NativeMethodRequirements::Stock::PLAYER_METHODS;
        constexpr std::array CO_METHODS{
            GuardedNativeMethodRequirement{"getMap", 0},
            GuardedNativeMethodRequirement{"getOwner", 0},
            GuardedNativeMethodRequirement{"getCoID", 0},
            GuardedNativeMethodRequirement{"getPowerMode", 0},
            GuardedNativeMethodRequirement{"getCOUnit", 0},
            GuardedNativeMethodRequirement{"getCORange", 0},
            GuardedNativeMethodRequirement{"inCORange", 2},
            GuardedNativeMethodRequirement{"getIsCO0", 0},
            GuardedNativeMethodRequirement{"getCoRangeEnabled", 0},
            GuardedNativeMethodRequirement{"getGlobalCoZone", 0},
            GuardedNativeMethodRequirement{"getPowerStars", 0},
            GuardedNativeMethodRequirement{"getSuperpowerStars", 0},
            GuardedNativeMethodRequirement{"getPowerUsed", 0},
            GuardedNativeMethodRequirement{"getPowerFilled", 0},
            GuardedNativeMethodRequirement{"setPowerFilled", 1},
        };
        constexpr auto & PROPERTY_METHODS = NativeMethodRequirements::Stock::PROPERTY_METHODS;
        constexpr std::array TERRAIN_METHODS{
            GuardedNativeMethodRequirement{"getX", 0},
            GuardedNativeMethodRequirement{"getY", 0},
            GuardedNativeMethodRequirement{"getID", 0},
            GuardedNativeMethodRequirement{"getTerrainID", 0},
            GuardedNativeMethodRequirement{"getUnit", 0},
            GuardedNativeMethodRequirement{"getBuilding", 0},
            GuardedNativeMethodRequirement{"getBaseTerrainID", 0},
            GuardedNativeMethodRequirement{"getNextBaseTerrain", 0},
            GuardedNativeMethodRequirement{"getDefense", 1},
            GuardedNativeMethodRequirement{"getOffensiveFieldBonus", 7},
            GuardedNativeMethodRequirement{"getDeffensiveFieldBonus", 7},
            GuardedNativeMethodRequirement{"getPalette", 0},
        };
        constexpr auto & RULE_METHODS = NativeMethodRequirements::Stock::RULE_METHODS;
        constexpr std::array VICTORY_METHODS{
            GuardedNativeMethodRequirement{"getMap", 0},
            GuardedNativeMethodRequirement{"getRuleID", 0},
            GuardedNativeMethodRequirement{"getRuleValue", 1},
            GuardedNativeMethodRequirement{"getVariables", 0},
        };
        constexpr std::array WEATHER_METHODS{
            GuardedNativeMethodRequirement{"getWeatherId", 0},
            GuardedNativeMethodRequirement{"getOffensiveModifier", 0},
            GuardedNativeMethodRequirement{"getDefensiveModifier", 0},
            GuardedNativeMethodRequirement{"getMovementCostModifier", 2},
            GuardedNativeMethodRequirement{"getMovementpointModifier", 2},
            GuardedNativeMethodRequirement{"getMovementFuelCostModifier", 2},
            GuardedNativeMethodRequirement{"getFirerangeModifier", 2},
            GuardedNativeMethodRequirement{"getMinFirerangeModifier", 2},
            GuardedNativeMethodRequirement{"getVisionrangeModifier", 2},
        };
        constexpr std::array GLOBAL_METHODS{
            GuardedNativeMethodRequirement{"getCircle", 2},
            GuardedNativeMethodRequirement{"roundUp", 1},
            GuardedNativeMethodRequirement{"randInt", 2},
            GuardedNativeMethodRequirement{"randInt", 3},
        };
        constexpr std::array ANIMATION_FACTORY_METHODS{
            GuardedNativeMethodRequirement{"createWalkingAnimation", 3},
            GuardedNativeMethodRequirement{"createGameAnimationCapture", 6},
            GuardedNativeMethodRequirement{"createBattleAnimation", 12},
            GuardedNativeMethodRequirement{"createAnimation", 3},
            GuardedNativeMethodRequirement{"createAnimation", 4},
            GuardedNativeMethodRequirement{"createAnimation", 5},
        };
        constexpr std::array INPUT_METHODS{
            GuardedNativeMethodRequirement{"getAiType", 0},
        };

        struct NativeReceiverSchema
        {
            NativeReceiverKind kind;
            const QMetaObject* metadata;
            std::span<const GuardedNativeMethodRequirement> methods;
        };
        constexpr std::array RECEIVER_SCHEMAS{
            NativeReceiverSchema{NativeReceiverKind::Map, &GameMap::staticMetaObject, MAP_METHODS},
            NativeReceiverSchema{NativeReceiverKind::Unit, &Unit::staticMetaObject, UNIT_METHODS},
            NativeReceiverSchema{NativeReceiverKind::Player, &Player::staticMetaObject, PLAYER_METHODS},
            NativeReceiverSchema{NativeReceiverKind::Co, &CO::staticMetaObject, CO_METHODS},
            NativeReceiverSchema{NativeReceiverKind::Property, &Building::staticMetaObject, PROPERTY_METHODS},
            NativeReceiverSchema{NativeReceiverKind::Terrain, &Terrain::staticMetaObject, TERRAIN_METHODS},
            NativeReceiverSchema{NativeReceiverKind::Rules, &GameRules::staticMetaObject, RULE_METHODS},
            NativeReceiverSchema{NativeReceiverKind::VictoryRule, &VictoryRule::staticMetaObject, VICTORY_METHODS},
            NativeReceiverSchema{NativeReceiverKind::Weather, &Weather::staticMetaObject, WEATHER_METHODS},
            NativeReceiverSchema{NativeReceiverKind::Globals, &GlobalUtils::staticMetaObject, GLOBAL_METHODS},
            NativeReceiverSchema{NativeReceiverKind::AnimationFactory, &GameAnimationFactory::staticMetaObject,
                                 ANIMATION_FACTORY_METHODS},
            NativeReceiverSchema{NativeReceiverKind::NormalInput, &NormalAi::staticMetaObject, INPUT_METHODS},
            NativeReceiverSchema{NativeReceiverKind::CoordinatedInput, &CoordinatedAi::staticMetaObject, INPUT_METHODS},
        };
        static_assert(RECEIVER_SCHEMAS.size() == RECEIVER_KIND_COUNT);

        constexpr std::array RECEIVER_METHOD_UNIONS{
            GuardedNativeMethodUnion{&GameMap::staticMetaObject, NativeMethodRequirements::MAP.methods()},
            GuardedNativeMethodUnion{&Unit::staticMetaObject, NativeMethodRequirements::UNIT.methods()},
            GuardedNativeMethodUnion{&Player::staticMetaObject, NativeMethodRequirements::PLAYER.methods()},
            GuardedNativeMethodUnion{&Building::staticMetaObject, NativeMethodRequirements::BUILDING.methods()},
            GuardedNativeMethodUnion{&GameRules::staticMetaObject, NativeMethodRequirements::RULES.methods()},
        };

        struct NativeModuleBasis
        {
            GuardedNativeModuleAgreement callbacks;
            std::optional<GuardedNativeArrayAgreement> actions;
            std::optional<GuardedNativeModuleAgreement> consequences;
            std::optional<GuardedNativeModuleAgreement> nominal;
            std::optional<GuardedNativeArrayAgreement> capturableBuildings;
            std::optional<GuardedNativeArrayAgreement> captureArmy;
            std::optional<GuardedNativeTableAgreement> tables;
            std::optional<GuardedNativeAbsentHooks> absentHooks;
            std::optional<GuardedNativeConstantLaw> economicLaw;
            std::optional<GuardedNativeArmyTableAgreement> armyTables;
            std::optional<GuardedNativeWeatherPrefixAgreement> weatherPrefix;
        };

        struct NativeSourceBasis
        {
            GuardedNativeInventory inventory;
            std::optional<GuardedNativeModuleAgreement> defaultArmyCallbacks;
            GuardedScriptAliasInspection globalAlias;
            GuardedNativeIntrinsicAgreement intrinsics;
            GuardedNativeValueConstructorAgreement valueConstructors;
            std::optional<GuardedNativeCalendar> calendar;
            bool commandPolicyVerified{false};
            quint8 commandRepairKinds{0};
            bool unlimitedInfantryInit{false};
            bool awdsInfantryCost{false};
            bool awdsLightTankInit{false};
            bool awdsLightTankCost{false};
            bool commandWeather{false};
            bool commandMovement{false};
            bool aw2ForestDefense{false};
            bool commandRankingBonuses{false};
            quint16 commandCallbacks{0};
            GuardedNativeUnitTypeAgreement unitTypes;
            GuardedNativeEnumAgreement terminalEnums;
            GuardedNativeEnumAgreement stockContextEnums;
            std::array<GuardedNativeMethodContract, RECEIVER_KIND_COUNT> receiverMethods;
            std::int64_t receiverConstructionWork{0};
            std::vector<std::shared_ptr<NativeModuleBasis>> modules;
            std::vector<GuardedNativeConstructionAgreement> constructionQueries;
            std::array<bool, CODE_KIND_COUNT> constructionKinds{};
            std::array<bool, CODE_KIND_COUNT> collected{};
            std::vector<std::array<std::int32_t, GUARDED_STOCK_MAX_TILES>> movementCosts;
            std::vector<std::int64_t> repairCostPerHp;
            std::vector<GuardedNativeCodeKind> actorKinds;
            std::vector<GuardedPropertyKind> propertyKinds;
            std::optional<std::size_t> fireActor;
            std::optional<std::size_t> fireTarget;
            std::optional<std::int32_t> fireWeapon;
            std::optional<GuardedStockDamageEnvelope> fireEnvelope;
            std::array<std::int32_t, 2> inputs{};
            std::optional<std::int32_t> battlePresentation;
            std::optional<GuardedNativeDetailPresentation> detailPresentation;
            MasterNativeCollectionCosts collectionCosts{};
        };

        class NativeCollectionCheckpoint final
        {
        public:
            NativeCollectionCheckpoint(NativeSourceBasis & basis, GuardedNativeCodeCatalog & catalog,
                                       const RefinementLedger & ledger)
                : m_basis(basis), m_catalog(catalog), m_ledger(ledger),
                  m_work(ledger.stats().spent()), m_expected(catalog.expectedPreparationWork().value_or(-1)),
                  m_receiver(basis.receiverConstructionWork)
            {
            }

            void record(MasterNativeCollectionStage stage)
            {
                const auto work = m_ledger.stats().spent();
                const auto expected = m_catalog.expectedPreparationWork().value_or(-1);
                const auto receiver = m_basis.receiverConstructionWork;
                auto & cost = m_basis.collectionCosts[static_cast<std::size_t>(stage)];
                ++cost.observations;
                cost.work += work - m_work;
                if (expected < 0 || m_expected < 0 || cost.expectedConstructionWork < 0)
                {
                    cost.expectedConstructionWork = -1;
                }
                else
                {
                    cost.expectedConstructionWork += expected - m_expected;
                }
                cost.receiverConstructionWork += receiver - m_receiver;
                m_work = work;
                m_expected = expected;
                m_receiver = receiver;
            }

        private:
            NativeSourceBasis & m_basis;
            GuardedNativeCodeCatalog & m_catalog;
            const RefinementLedger & m_ledger;
            std::int64_t m_work;
            std::int64_t m_expected;
            std::int64_t m_receiver;
        };

        MasterNativeSourceStatus sourceFailure(GuardedNativeBindingStatus);

        const NativeModuleBasis* nativeModule(const NativeSourceBasis & basis, GuardedNativeCodeKind kind)
        {
            const auto found = std::find_if(basis.modules.begin(), basis.modules.end(),
                [&](const auto & module) { return module->callbacks.kind == kind; });
            return found == basis.modules.end() ? nullptr : found->get();
        }

        const GuardedNativeArrayAgreement* singleTable(const NativeModuleBasis* module)
        {
            if (module == nullptr || !module->tables ||
                module->tables->status != GuardedNativeBindingStatus::Bound || module->tables->tables.size() != 1)
            {
                return nullptr;
            }
            return &module->tables->tables[0].evidence;
        }

        std::optional<std::int32_t> actorCost(const NativeSourceBasis & basis, GuardedNativeCodeKind kind)
        {
            const auto* module = nativeModule(basis, kind);
            if (module == nullptr || !module->economicLaw ||
                module->economicLaw->status != GuardedNativeBindingStatus::Bound)
            {
                return std::nullopt;
            }
            return module->economicLaw->value;
        }

        std::optional<GuardedPropertyKind> propertyKind(GuardedNativeCodeKind kind)
        {
            switch (kind)
            {
                case GuardedNativeCodeKind::Town: return GuardedPropertyKind::Town;
                case GuardedNativeCodeKind::Factory: return GuardedPropertyKind::Factory;
                case GuardedNativeCodeKind::Headquarters: return GuardedPropertyKind::Headquarters;
                default: return std::nullopt;
            }
        }

        std::optional<std::int64_t> rootProjectionWork(const GuardedNativePlanBounds & bounds)
        {
            const auto binding = guardedNativePlanWork(bounds);
            if (!binding)
            {
                return std::nullopt;
            }
            const auto actors = static_cast<std::int64_t>(bounds.actors);
            const auto points = static_cast<std::int64_t>(bounds.pathPoints);
            constexpr auto MAX_ACTORS = static_cast<std::int64_t>(GUARDED_STOCK_MAX_ACTORS);
            const auto properties = static_cast<std::int64_t>(bounds.properties);
            const auto actionBytes = ROOT_IDENTITY_FIELD_BYTES * ROOT_ACTION_IDENTITY_FIELDS +
                2 * static_cast<std::int64_t>(GUARDED_NATIVE_MAX_ID_LENGTH) + ROOT_POINT_IDENTITY_BYTES * points;
            // consequenceIdentity serializes every root actor and property and every action's id and path.
            const auto identityBytes = ROOT_IDENTITY_HEADER_BYTES +
                ROOT_IDENTITY_FIELD_BYTES * (ROOT_ACTOR_IDENTITY_FIELDS * MAX_ACTORS +
                    ROOT_PROPERTY_IDENTITY_FIELDS * properties) + actors * actionBytes;
            // The fire family copies the action prefix twice and every repair cost once.
            const auto familyBytes = 2 * actors * actionBytes + ROOT_IDENTITY_FIELD_BYTES * MAX_ACTORS;
            // projectMovement checks every root actor per path point; each capture scans every property.
            return *binding + ROOT_PROJECTION_CONTROL_WORK + IDENTITY_BYTE_PASSES * (identityBytes + familyBytes) +
                ROOT_FIELD_WORK * (static_cast<std::int64_t>(bounds.tiles) + 2 * MAX_ACTORS) +
                (ROOT_PATH_WORK + ROOT_OCCUPANCY_COMPARE_WORK * MAX_ACTORS) * actors * points +
                ROOT_OCCUPANCY_COMPARE_WORK * actors * properties +
                guardedStockDamageEnvelopeWork();
        }

        MasterNativeSourceStatus constructNativeModel(
            NativeSourceBasis & basis, GuardedNativeCodeCatalog & catalog, RefinementLedger & ledger)
        {
            const auto actors = basis.inventory.actors.size();
            const auto records = basis.modules.size();
            if (!ledger.consume(ROOT_PROJECTION_CONTROL_WORK + ROOT_FIELD_WORK *
                    ((actors + basis.inventory.properties.size() + 1) * (records + 1) +
                     actors * GUARDED_STOCK_MAX_TILES),
                    RefinementWork::MasterPreparation))
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            basis.movementCosts.reserve(actors);
            basis.repairCostPerHp.reserve(actors);
            basis.actorKinds.reserve(actors);
            basis.propertyKinds.reserve(basis.inventory.properties.size());
            std::array<std::optional<std::array<std::int32_t, GUARDED_STOCK_MAX_TILES>>, CODE_KIND_COUNT> fields;
            for (const auto & actor : basis.inventory.actors)
            {
                if (!actor.material || actor.actualHp != UNIT_HP_STEPS ||
                    (basis.commandRankingBonuses && actor.material->rank != GUARDED_NATIVE_UNRANKED))
                {
                    return MasterNativeSourceStatus::UnsupportedInventory;
                }
                const auto kind = catalog.kindForGlobal(actor.type);
                const auto movement = catalog.kindForGlobal(actor.material->movementType);
                if (kind.status != GuardedNativeBindingStatus::Bound ||
                    movement.status != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(kind.status != GuardedNativeBindingStatus::Bound ? kind.status : movement.status);
                }
                const auto cost = actorCost(basis, kind.kind);
                if (!cost || *cost <= 0 || *cost % UNIT_HP_STEPS != 0 || *cost > MAX_NATIVE_UNIT_COST ||
                    ((kind.kind == GuardedNativeCodeKind::Infantry || kind.kind == GuardedNativeCodeKind::Sniper) &&
                     (actor.material->rank != GUARDED_NATIVE_UNRANKED ||
                      actor.material->baseMovement != GUARDED_STOCK_ACTOR_MOVEMENT || movement.kind != GuardedNativeCodeKind::MoveFeet)))
                {
                    return MasterNativeSourceStatus::UnsupportedInventory;
                }
                const auto index = static_cast<std::size_t>(movement.kind);
                if (index >= fields.size())
                {
                    return MasterNativeSourceStatus::UnsupportedCallbacks;
                }
                if (!fields[index])
                {
                    const auto* table = singleTable(nativeModule(basis, movement.kind));
                    if (table == nullptr || table->actual.empty())
                    {
                        return MasterNativeSourceStatus::UnsupportedCallbacks;
                    }
                    auto field = prepareGuardedNativeMovementField(*table, basis.inventory, ledger);
                    if (field.status != GuardedNativeBindingStatus::Bound || !field.costs)
                    {
                        return sourceFailure(field.status);
                    }
                    fields[index] = std::move(*field.costs);
                }
                basis.movementCosts.push_back(*fields[index]);
                basis.repairCostPerHp.push_back(*cost / UNIT_HP_STEPS);
                basis.actorKinds.push_back(kind.kind);
            }
            for (const auto & property : basis.inventory.properties)
            {
                const auto kind = catalog.kindForGlobal(property.type);
                if (kind.status != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(kind.status);
                }
                const auto projected = propertyKind(kind.kind);
                const auto* module = nativeModule(basis, kind.kind);
                if (!projected || module == nullptr || !module->economicLaw ||
                    module->economicLaw->status != GuardedNativeBindingStatus::Bound ||
                    static_cast<std::int64_t>(module->economicLaw->value) * NATIVE_INCOME_MILLI_FACTOR !=
                        GUARDED_STOCK_PROPERTY_PAYMENT)
                {
                    return MasterNativeSourceStatus::UnsupportedCallbacks;
                }
                basis.propertyKinds.push_back(*projected);
            }
            return MasterNativeSourceStatus::Ready;
        }

        bool admittedNativeInventory(const GuardedNativeInventory & inventory)
        {
            if (!inventory.gameRuleIds || !inventory.gameRuleIds->empty() ||
                !inventory.mapScriptLoaded || *inventory.mapScriptLoaded)
            {
                return false;
            }
            const auto incomeCeiling = static_cast<std::int64_t>(inventory.properties.size()) *
                GUARDED_STOCK_HORIZON_PAYMENTS * (GUARDED_STOCK_PROPERTY_PAYMENT / NATIVE_INCOME_MILLI_FACTOR);
            for (const auto funds : inventory.funds)
            {
                if (funds < 0 || funds > std::numeric_limits<qint32>::max() - incomeCeiling)
                {
                    return false;
                }
            }
            return true;
        }

        bool admittedNativeCalendar(const NativeSourceBasis & basis)
        {
            if (!basis.calendar || basis.calendar->commandRuleIndex.has_value() != basis.commandPolicyVerified ||
                !admittedNativeInventory(basis.inventory) ||
                basis.calendar->basis.terminal || basis.calendar->basis.humanMatch ||
                basis.calendar->basis.fog != GameEnums::Fog_Off ||
                basis.calendar->headquartersActive != std::array<bool, 2>{true, true} ||
                basis.calendar->unitsActive != std::array<bool, 2>{true, true})
            {
                return false;
            }
            const auto weather = basis.calendar->basis.currentWeather;
            if (basis.calendar->basis.startWeather != weather || weather < 0 ||
                static_cast<std::size_t>(weather) >= basis.calendar->basis.weatherIds.size() ||
                basis.calendar->basis.weatherIds[weather] != QStringLiteral("WEATHER_1SUN"))
            {
                return false;
            }
            for (const auto & phase : basis.calendar->phases)
            {
                if (phase.weather != weather)
                {
                    return false;
                }
            }
            return true;
        }

        std::optional<std::int64_t> nativeIdentityWork(
            const NativeSourceBasis & basis, RefinementLedger & ledger)
        {
            const auto & inventory = basis.inventory;
            const auto & calendar = basis.calendar->basis;
            auto records = 1 + inventory.actors.size() + inventory.properties.size() +
                inventory.terrains->size() * GUARDED_NATIVE_MAX_TERRAIN_DEPTH +
                4 * GUARDED_NATIVE_MAX_EFFECTIVE_PERKS +
                calendar.weatherIds.size() + calendar.forecast.size() +
                calendar.victoryRules.size() * GUARDED_NATIVE_MAX_RULE_VARIABLES;
            if (basis.commandPolicyVerified)
            {
                records += GUARDED_NATIVE_COMMAND_VARIABLES - GUARDED_NATIVE_MAX_RULE_VARIABLES;
            }
            for (const auto & player : *inventory.playerMaterial)
            {
                records += player.buildList.size();
            }
            if (!ledger.consume(ROOT_FIELD_WORK * records, RefinementWork::MasterPreparation))
            {
                return std::nullopt;
            }
            std::int64_t bytes = ROOT_PROJECTION_CONTROL_WORK + 128 * inventory.actors.size() +
                32 * inventory.properties.size() + 16 * inventory.terrains->size() +
                16 * calendar.forecast.size();
            if (basis.commandPolicyVerified)
            {
                bytes += sizeof(quint32) + COMMAND_POLICY_REVISION.size() + 9 * sizeof(quint8) + sizeof(quint16);
            }
            if (basis.detailPresentation)
            {
                bytes += 64;
            }
            const auto stringBytes = [](const QString & value) {
                return 4 + 2 * static_cast<std::int64_t>(value.size());
            };
            for (const auto & actor : inventory.actors)
            {
                bytes += stringBytes(actor.type) + stringBytes(actor.material->movementType) +
                    stringBytes(actor.material->weapon1) + stringBytes(actor.material->weapon2);
                if (actor.ammo1 < 0 || actor.ammo2 < 0)
                {
                    bytes += sizeof(quint32) + UNLIMITED_AMMUNITION_REVISION.size();
                }
            }
            for (const auto & property : inventory.properties)
            {
                bytes += stringBytes(property.type);
            }
            for (const auto & player : *inventory.playerMaterial)
            {
                bytes += stringBytes(player.army);
                if (player.defaultArmyPlayerId)
                {
                    bytes += 2 * sizeof(qint32);
                }
                for (const auto & type : player.buildList)
                {
                    bytes += stringBytes(type);
                }
                for (const auto & co : player.cos)
                {
                    bytes += 128;
                    if (!co)
                    {
                        continue;
                    }
                    bytes += stringBytes(co->id);
                    for (const auto & perk : co->effectivePerks)
                    {
                        bytes += stringBytes(perk);
                    }
                }
            }
            for (const auto & terrain : *inventory.terrains)
            {
                for (const auto & layer : terrain.layers)
                {
                    bytes += 32 + stringBytes(layer.type);
                }
            }
            for (const auto & rule : *inventory.gameRuleIds)
            {
                bytes += stringBytes(rule);
            }
            for (const auto & weather : calendar.weatherIds)
            {
                bytes += stringBytes(weather);
            }
            for (const auto & rule : calendar.victoryRules)
            {
                bytes += 16 + stringBytes(rule.id);
                for (const auto & variable : rule.variables)
                {
                    bytes += 16 + stringBytes(variable.id) + variable.data.size();
                }
            }
            return IDENTITY_BYTE_PASSES * bytes;
        }

        QByteArray nativeBeforeIdentity(const NativeSourceBasis & basis)
        {
            QByteArray bytes;
            QDataStream stream(&bytes, QIODevice::WriteOnly);
            stream.setVersion(QDataStream::Qt_6_8);
            const auto & inventory = basis.inventory;
            stream << NATIVE_AUTHORITY_REVISION << inventory.width << inventory.height <<
                inventory.day << inventory.player;
            for (std::size_t side = 0; side < 2; ++side)
            {
                stream << qint64(inventory.funds[side]) << inventory.teams[side] << inventory.defeated[side] <<
                    basis.inputs[side];
            }
            stream << basis.battlePresentation.has_value();
            if (basis.battlePresentation)
            {
                stream << *basis.battlePresentation;
            }
            stream << basis.detailPresentation.has_value();
            if (basis.detailPresentation)
            {
                stream << basis.detailPresentation->identity();
            }
            stream << quint32(inventory.actors.size());
            for (const auto & actor : inventory.actors)
            {
                const auto & material = *actor.material;
                stream << actor.id << actor.owner << actor.tile << actor.type << actor.actualHp <<
                    actor.progress << actor.fuel << actor.ammo1 << actor.ammo2 << actor.moved <<
                    actor.hidden << actor.cloaked << material.maxFuel << material.maxAmmo1 <<
                    material.maxAmmo2 << material.baseMovement << material.minRange << material.maxRange <<
                    material.rank << material.movementType << material.weapon1 << material.weapon2;
                if (actor.ammo1 < 0 || actor.ammo2 < 0)
                {
                    stream << UNLIMITED_AMMUNITION_REVISION;
                }
            }
            stream << quint32(inventory.properties.size());
            for (const auto & property : inventory.properties)
            {
                stream << property.tile << property.owner << property.type;
            }
            for (const auto & player : *inventory.playerMaterial)
            {
                stream << player.fundsModifier << player.army << quint32(player.buildList.size());
                for (const auto & type : player.buildList)
                {
                    stream << type;
                }
                for (const auto & co : player.cos)
                {
                    stream << co.has_value();
                    if (!co)
                    {
                        continue;
                    }
                    stream << co->id << co->powerMode << co->unitId << co->rangeEnabled <<
                        co->globalZone << co->powerStars << co->superpowerStars << co->powerUsed <<
                        co->powerFilled << co->isCo0 << quint32(co->effectivePerks.size());
                    for (const auto & perk : co->effectivePerks)
                    {
                        stream << perk;
                    }
                }
            }
            const auto & coRules = *inventory.coRules;
            stream << coRules.parallel << coRules.dayToDay << coRules.globalDayToDay <<
                coRules.coUnits << coRules.noPower << coRules.gainSpeed << coRules.usageReduction <<
                qint32(coRules.gainMode) << qint32(coRules.gainZone);
            stream << *inventory.mapScriptLoaded << quint32(inventory.gameRuleIds->size());
            for (const auto & rule : *inventory.gameRuleIds)
            {
                stream << rule;
            }
            const auto & combat = *inventory.combatRules;
            stream << qint32(combat.formula) << combat.terrainDefense << combat.hpDefenseReduction;
            for (const auto & tile : *inventory.terrains)
            {
                stream << tile.tile << quint32(tile.layers.size());
                for (const auto & layer : tile.layers)
                {
                    stream << layer.type << layer.hp << layer.startOfTurn << layer.flowDirection;
                }
            }
            const auto & calendar = basis.calendar->basis;
            stream << calendar.day << calendar.player << calendar.terminal << calendar.humanMatch <<
                calendar.randomWeather << calendar.fog << calendar.currentWeather <<
                calendar.startWeather << calendar.dayWeather << quint32(calendar.weatherIds.size());
            for (const auto & weather : calendar.weatherIds)
            {
                stream << weather;
            }
            stream << quint32(calendar.forecast.size());
            for (const auto & entry : calendar.forecast)
            {
                stream << entry[0] << entry[1];
            }
            if (basis.commandPolicyVerified)
            {
                stream << COMMAND_POLICY_REVISION << basis.commandRepairKinds << basis.unlimitedInfantryInit << basis.awdsInfantryCost << basis.awdsLightTankInit << basis.awdsLightTankCost << basis.commandWeather << basis.commandMovement << basis.aw2ForestDefense << basis.commandRankingBonuses << basis.commandCallbacks;
            }
            stream << quint32(calendar.victoryRules.size());
            for (const auto & rule : calendar.victoryRules)
            {
                stream << rule.id << quint32(rule.variables.size());
                for (const auto & variable : rule.variables)
                {
                    stream << variable.id << variable.data << variable.modified;
                }
            }
            for (std::size_t side = 0; side < inventory.playerMaterial->size(); ++side)
            {
                const auto & player = inventory.playerMaterial->at(side);
                if (player.defaultArmyPlayerId)
                {
                    stream << qint32(side) << qint32(*player.defaultArmyPlayerId);
                }
            }
            return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        }

        const GuardedNativeArrayAgreement* namedTable(const NativeModuleBasis* module, const QString & name)
        {
            if (module == nullptr || !module->tables)
            {
                return nullptr;
            }
            for (const auto & table : module->tables->tables)
            {
                if (table.name == name)
                {
                    return &table.evidence;
                }
            }
            return nullptr;
        }

        std::optional<std::int32_t> consequenceScalar(const NativeModuleBasis* module, const QString & name)
        {
            if (module == nullptr || !module->consequences)
            {
                return std::nullopt;
            }
            for (const auto & member : module->consequences->members)
            {
                if (member.name == name && std::holds_alternative<GuardedNativeScalarAgreement>(member.evidence))
                {
                    GuardedNativeDataNode node;
                    node.scalar = std::get<GuardedNativeScalarAgreement>(member.evidence).actual;
                    return guardedNativeIntegerData(node);
                }
            }
            return std::nullopt;
        }

        MasterNativeSourceStatus constructCurrentFire(
            NativeSourceBasis & basis, const MasterAssignmentProblem & problem, RefinementLedger & ledger)
        {
            const auto work = ROOT_PROJECTION_CONTROL_WORK +
                ROOT_FIELD_WORK * (CODE_KIND_COUNT * 1024 + GUARDED_STOCK_MAX_ACTORS * 32);
            if (!ledger.consume(work, RefinementWork::MasterPreparation))
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            const PlannedAction* fire = nullptr;
            for (const auto & action : problem.actionTemplates())
            {
                if (action.kind == PlanBundleKind::Fire || action.kind == PlanBundleKind::MoveAndFire)
                {
                    if (fire != nullptr)
                    {
                        return MasterNativeSourceStatus::UnsupportedCallbacks;
                    }
                    fire = &action;
                }
            }
            if (fire == nullptr)
            {
                return MasterNativeSourceStatus::Ready;
            }
            for (std::size_t index = 0; index < basis.inventory.actors.size(); ++index)
            {
                if (basis.inventory.actors[index].id == fire->unitId)
                {
                    basis.fireActor = index;
                }
                if (basis.inventory.actors[index].id == fire->targetUnitId)
                {
                    basis.fireTarget = index;
                }
            }
            if (!basis.fireActor || !basis.fireTarget || fire->path.empty())
            {
                return MasterNativeSourceStatus::UnsupportedInventory;
            }
            const auto & attacker = basis.inventory.actors[*basis.fireActor];
            const auto & target = basis.inventory.actors[*basis.fireTarget];
            if (basis.actorKinds[*basis.fireActor] != GuardedNativeCodeKind::Infantry ||
                attacker.material->rank != GUARDED_NATIVE_UNRANKED ||
                target.material->rank != GUARDED_NATIVE_UNRANKED ||
                attacker.material->minRange != 1 || attacker.material->maxRange < 1 ||
                basis.actorKinds[*basis.fireTarget] != GuardedNativeCodeKind::Sniper ||
                attacker.owner != basis.inventory.player || target.owner == attacker.owner ||
                manhattanDistance(fire->path.back(), fire->target) != 1 ||
                target.material->minRange < 2 || target.material->maxRange < target.material->minRange ||
                !target.material->weapon2.isEmpty() || target.material->maxAmmo2 != 0)
            {
                return MasterNativeSourceStatus::UnsupportedInventory;
            }
            const auto counter = WeaponRangeCheck::canAttack(
                { !target.material->weapon1.isEmpty(), WeaponRangeCheck::hasAmmo(target.ammo1, target.material->maxAmmo1) },
                WeaponRangeCheck::WeaponType::Indirect, 1, target.material->minRange,
                target.material->maxRange, WeaponRangeCheck::RangeCheck::All);
            if (counter)
            {
                return MasterNativeSourceStatus::UnsupportedCallbacks;
            }
            std::int32_t baseDamage = -1;
            const std::array weapons{attacker.material->weapon1, attacker.material->weapon2};
            const std::array available{WeaponRangeCheck::hasAmmo(attacker.ammo1, attacker.material->maxAmmo1),
                                       WeaponRangeCheck::hasAmmo(attacker.ammo2, attacker.material->maxAmmo2)};
            for (std::int32_t slot = 0; slot < 2; ++slot)
            {
                if (weapons[slot].isEmpty() || !available[slot])
                {
                    continue;
                }
                const auto kind = weapons[slot] == INFANTRY_WEAPON
                    ? GuardedNativeCodeKind::InfantryMachineGun : GuardedNativeCodeKind::SniperWeapon;
                if (!WeaponRangeCheck::canAttack({true, true}, WeaponRangeCheck::WeaponType::Both, 1,
                        attacker.material->minRange, attacker.material->maxRange,
                        WeaponRangeCheck::RangeCheck::All))
                {
                    continue;
                }
                const auto* table = namedTable(nativeModule(basis, kind), DAMAGE_TABLE);
                if (table == nullptr)
                {
                    return MasterNativeSourceStatus::UnsupportedCallbacks;
                }
                const auto value = guardedNativeIntegerTableValue(*table, target.type, ledger);
                if (value.status == GuardedNativeBindingStatus::BudgetExhausted)
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                const auto damage = value.value;
                if (value.status != GuardedNativeBindingStatus::Bound || !damage || *damage <= 0)
                {
                    return MasterNativeSourceStatus::UnsupportedCallbacks;
                }
                if (*damage > baseDamage)
                {
                    baseDamage = *damage;
                    basis.fireWeapon = slot;
                }
            }
            if (!basis.fireWeapon || baseDamage <= 0 || !basis.inventory.combatRules)
            {
                return MasterNativeSourceStatus::UnsupportedCallbacks;
            }
            auto maximumOffense = OFFENSE_BASE;
            for (const auto & co : (*basis.inventory.playerMaterial)[attacker.owner].cos)
            {
                if (!co)
                {
                    continue;
                }
                for (const auto & perk : co->effectivePerks)
                {
                    if (perk == ANDY_CO)
                    {
                        const auto bonus = consequenceScalar(
                            nativeModule(basis, GuardedNativeCodeKind::Andy), ANDY_OFFENSE);
                        if (!bonus || *bonus < 0 || *bonus > OFFENSE_BASE)
                        {
                            return MasterNativeSourceStatus::UnsupportedCallbacks;
                        }
                        maximumOffense += *bonus;
                    }
                    else if (perk == JESS_CO)
                    {
                        const auto* module = nativeModule(basis, GuardedNativeCodeKind::Jess);
                        const auto zone = consequenceScalar(module, JESS_ZONE_OFFENSE);
                        const auto global = consequenceScalar(module, JESS_GLOBAL_OFFENSE);
                        if (!zone || !global || *zone < 0 || *global < 0 ||
                            *zone > OFFENSE_BASE || *global > OFFENSE_BASE)
                        {
                            return MasterNativeSourceStatus::UnsupportedCallbacks;
                        }
                        maximumOffense += std::max(*zone, *global);
                    }
                }
            }
            const auto bounded = boundGuardedStockDamage(
                {basis.inventory.combatRules->formula, baseDamage, maximumOffense, DEFENSE_BASE,
                 attacker.actualHp, target.actualHp}, ledger);
            if (bounded.status != GuardedNativeBindingStatus::Bound || !bounded.envelope)
            {
                return sourceFailure(bounded.status);
            }
            basis.fireEnvelope = bounded.envelope;
            return MasterNativeSourceStatus::Ready;
        }

        GuardedStockRoot initialStockRoot(const NativeSourceBasis & basis)
        {
            GuardedStockRoot root;
            root.revision = GuardedStockRevision::CoupledNativeTerminalV4;
            root.tiles = basis.inventory.width * basis.inventory.height;
            root.player = basis.inventory.player;
            root.funds = basis.inventory.funds;
            root.actors.reserve(basis.inventory.actors.size());
            root.properties.reserve(basis.inventory.properties.size());
            for (std::size_t index = 0; index < basis.inventory.actors.size(); ++index)
            {
                const auto & actor = basis.inventory.actors[index];
                const auto stock = basis.actorKinds[index] == GuardedNativeCodeKind::Infantry ||
                    basis.actorKinds[index] == GuardedNativeCodeKind::Sniper;
                root.actors.push_back({actor.id, actor.owner, actor.tile, actor.actualHp,
                    static_cast<std::int32_t>(std::ceil(actor.actualHp)), actor.progress, actor.fuel,
                    actor.ammo1, actor.ammo2, actor.material->baseMovement, stock, actor.moved});
            }
            for (std::size_t index = 0; index < basis.inventory.properties.size(); ++index)
            {
                const auto & property = basis.inventory.properties[index];
                root.properties.push_back({property.tile, basis.propertyKinds[index], property.owner,
                    GUARDED_STOCK_PROPERTY_PAYMENT});
            }
            return root;
        }

        bool projectMovement(const NativeSourceBasis & basis, const GuardedNativeAction & action,
                             GuardedStockRoot & root)
        {
            auto & actor = root.actors[action.actor];
            auto cost = 0;
            for (std::size_t point = 1; point < action.path.size(); ++point)
            {
                const auto position = action.path[point];
                const auto tile = position.y * basis.inventory.width + position.x;
                const auto step = basis.movementCosts[action.actor][tile];
                if (step <= 0)
                {
                    return false;
                }
                for (std::size_t other = 0; other < root.actors.size(); ++other)
                {
                    if (other != action.actor && root.actors[other].tile == tile &&
                        (root.actors[other].owner != actor.owner || point + 1 == action.path.size()))
                    {
                        return false;
                    }
                }
                cost += step;
            }
            if (cost > actor.movement || cost > actor.fuel)
            {
                return false;
            }
            if (action.path.size() > 1)
            {
                actor.progress = 0;
            }
            const auto destination = action.path.back();
            actor.tile = destination.y * basis.inventory.width + destination.x;
            actor.fuel -= cost;
            actor.moved = true;
            return true;
        }

        std::optional<GuardedStockCurrentFamily> projectCurrentPlan(
            const NativeSourceBasis & basis, const GuardedNativePlanBinding & plan,
            GuardedStockRoot & root, const QByteArray & before, bool & valid)
        {
            std::optional<GuardedStockCurrentFamily> family;
            bool fireSeen = false;
            std::vector<GuardedNativeAction> prefix;
            for (const auto & action : plan.actions)
            {
                auto & actor = root.actors[action.actor];
                if (!projectMovement(basis, action, root))
                {
                    valid = false;
                    return {};
                }
                if (action.kind == PlanBundleKind::Capture || action.kind == PlanBundleKind::MoveAndCapture)
                {
                    if (basis.fireActor && !fireSeen)
                    {
                        valid = false;
                        return {};
                    }
                    auto property = std::find_if(root.properties.begin(), root.properties.end(),
                        [&](const auto & record) { return record.tile == actor.tile; });
                    if (!actor.stock || property == root.properties.end() || property->owner == actor.owner)
                    {
                        valid = false;
                        return {};
                    }
                    actor.progress += actor.roundedHp;
                    if (actor.progress >= GUARDED_STOCK_CAPTURE_THRESHOLD)
                    {
                        if (property->kind == GuardedPropertyKind::Headquarters)
                        {
                            valid = false;
                            return {};
                        }
                        actor.progress = 0;
                        property->owner = actor.owner;
                    }
                }
                else if (action.kind == PlanBundleKind::Fire || action.kind == PlanBundleKind::MoveAndFire)
                {
                    const auto & original = basis.inventory.actors;
                    if (fireSeen || !basis.fireActor || !basis.fireTarget || !basis.fireWeapon ||
                        !basis.fireEnvelope || action.actor != *basis.fireActor ||
                        action.targetUnitId != original[*basis.fireTarget].id ||
                        action.target != TilePoint{original[*basis.fireTarget].tile % basis.inventory.width,
                                                   original[*basis.fireTarget].tile / basis.inventory.width} ||
                        manhattanDistance(action.path.back(), action.target) != 1)
                    {
                        valid = false;
                        return {};
                    }
                    prefix.push_back(action);
                    auto & ammunition = *basis.fireWeapon == 0 ? actor.ammo1 : actor.ammo2;
                    if (ammunition > 0)
                    {
                        --ammunition;
                    }
                    family = GuardedStockCurrentFamily{before, basis.inventory.width, basis.inventory.height,
                        action.targetUnitId, basis.fireEnvelope->lowerHp, basis.fireEnvelope->upperHp,
                        basis.repairCostPerHp, prefix};
                    fireSeen = true;
                }
                else if (action.kind != PlanBundleKind::Wait && action.kind != PlanBundleKind::Move)
                {
                    valid = false;
                    return {};
                }
                else if (!fireSeen)
                {
                    prefix.push_back(action);
                }
            }
            valid = fireSeen == basis.fireActor.has_value();
            return family;
        }

        QByteArray consequenceIdentity(const GuardedStockRoot & root,
                                       const GuardedNativePlanBinding & plan,
                                       const QByteArray & before)
        {
            QByteArray bytes;
            QDataStream stream(&bytes, QIODevice::WriteOnly);
            stream.setVersion(QDataStream::Qt_6_8);
            stream << NATIVE_AUTHORITY_REVISION << before << quint64(plan.identity.session()) <<
                qint32(root.revision) << root.tiles << root.player << root.terminal << qint64(root.paid);
            for (std::size_t side = 0; side < 2; ++side)
            {
                stream << qint64(root.funds[side]) << root.payments[side];
            }
            for (const auto & actor : root.actors)
            {
                stream << actor.id << actor.owner << actor.tile << actor.actualHp << actor.roundedHp <<
                    actor.progress << actor.fuel << actor.ammo1 << actor.ammo2 << actor.movement <<
                    actor.stock << actor.moved;
            }
            for (const auto & property : root.properties)
            {
                stream << property.tile << qint32(property.kind) << property.owner << qint64(property.payment);
            }
            for (const auto & action : plan.actions)
            {
                stream << quint32(action.actor) << action.actionIndex << qint32(action.kind) << action.actionId <<
                    action.target.x << action.target.y << action.targetUnitId << quint32(action.path.size());
                for (const auto & point : action.path)
                {
                    stream << point.x << point.y;
                }
            }
            return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        }

        bool supportsPlanIntent(const MasterAssignmentProblem & problem,
                                const MasterAssignmentState & state)
        {
            if (state.choices.size() != problem.actors().size() ||
                problem.incumbentChoices().size() != state.choices.size() ||
                !state.identity.sameLiveProblem(problem.identity()) || !state.complete())
            {
                return false;
            }
            std::size_t differences = 0;
            for (std::size_t slot = 0; slot < state.choices.size(); ++slot)
            {
                const auto choice = state.choices[slot];
                const auto incoming = problem.incumbentChoices()[slot];
                const auto & actor = problem.actors()[slot];
                if (choice < 0 || incoming < 0 || static_cast<std::size_t>(choice) >= actor.candidates.size() ||
                    static_cast<std::size_t>(incoming) >= actor.candidates.size())
                {
                    return false;
                }
                if (choice == incoming)
                {
                    continue;
                }
                const auto & before = actor.candidates[incoming];
                const auto & after = actor.candidates[choice];
                if (++differences > 1 || before.actionKind != PlanBundleKind::Wait ||
                    after.actionKind != PlanBundleKind::Capture ||
                    before.source.bundle.path.size() != 1 || after.source.bundle.path.size() != 1 ||
                    before.destination != after.destination ||
                    before.source.bundle.path.front() != after.source.bundle.path.front())
                {
                    return false;
                }
            }
            return true;
        }

        MasterNativeSourceStatus sourceFailure(GuardedNativeBindingStatus status)
        {
            if (status == GuardedNativeBindingStatus::BudgetExhausted)
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            return MasterNativeSourceStatus::UnsupportedCallbacks;
        }

        bool actorKind(GuardedNativeCodeKind kind)
        {
            switch (kind)
            {
                case GuardedNativeCodeKind::Infantry:
                case GuardedNativeCodeKind::Sniper:
                case GuardedNativeCodeKind::Recon:
                case GuardedNativeCodeKind::LightTank:
                    return true;
                default:
                    return false;
            }
        }

        bool coKind(GuardedNativeCodeKind kind)
        {
            return kind == GuardedNativeCodeKind::Andy || kind == GuardedNativeCodeKind::Jess ||
                kind == GuardedNativeCodeKind::TagPower;
        }

        MasterNativeSourceStatus collectModule(
            NativeSourceBasis & basis, GuardedNativeCodeCatalog & catalog, QJSEngine & engine,
            GuardedNativeCodeKind kind, NativeModuleProjection projection, RefinementLedger & ledger)
        {
            if (!ledger.consume(DISPATCH_ENTRY_WORK, RefinementWork::MasterPreparation))
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            const auto index = static_cast<std::size_t>(kind);
            if (index >= basis.collected.size())
            {
                return MasterNativeSourceStatus::UnsupportedCallbacks;
            }
            if (basis.collected[index])
            {
                return MasterNativeSourceStatus::Ready;
            }
            if (!ledger.consume(MODULE_RECORD_WORK + NOMINAL_RECORD_WORK,
                                RefinementWork::MasterPreparation))
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            GuardedNativeModuleAgreement agreement;
            if (projection == NativeModuleProjection::Co)
            {
                agreement = catalog.authenticateStockCoCallbacks(engine, kind);
            }
            else if (projection == NativeModuleProjection::Actor)
            {
                agreement = catalog.authenticateUnitCallbacks(engine, kind, basis.commandPolicyVerified);
            }
            else if (projection == NativeModuleProjection::UnitHelper)
            {
                agreement = catalog.authenticateStockUnitHelpers(engine, basis.commandPolicyVerified);
            }
            else if (projection == NativeModuleProjection::Property)
            {
                agreement = catalog.authenticateStockPropertyCallbacks(engine, kind, basis.commandPolicyVerified);
            }
            else if (projection == NativeModuleProjection::Movement)
            {
                agreement = catalog.authenticateStockMovementCallbacks(engine, kind);
            }
            else if (projection == NativeModuleProjection::Context)
            {
                agreement = catalog.authenticateStockContextCallbacks(engine, kind, basis.commandPolicyVerified);
            }
            else if (projection == NativeModuleProjection::Terrain)
            {
                if (basis.commandPolicyVerified)
                {
                    agreement = catalog.authenticateCommandTerrainCallbacks(engine, kind);
                }
                else
                {
                    agreement = catalog.authenticateStockTerrainCallbacks(engine, kind);
                }
            }
            else if (projection == NativeModuleProjection::Policy)
            {
                agreement = catalog.authenticateStockPolicyCallbacks(engine, kind, basis.commandPolicyVerified);
            }
            else if (projection == NativeModuleProjection::CurrentFire)
            {
                agreement = catalog.authenticateCurrentFireCallbacks(engine);
            }
            else if (projection == NativeModuleProjection::Weapon)
            {
                agreement = catalog.authenticateCurrentWeaponCallbacks(engine, kind);
            }
            else
            {
                agreement = kind == GuardedNativeCodeKind::Trap && basis.commandMovement
                    ? catalog.authenticateMovementTrapCallbacks(engine) : catalog.authenticateStockActionCallbacks(engine, kind, basis.commandPolicyVerified);
            }
            if (basis.commandPolicyVerified &&
                (projection == NativeModuleProjection::Actor || projection == NativeModuleProjection::UnitHelper))
            {
                static_assert(static_cast<unsigned>(GuardedNativeCodeKind::LightTank) < 8);
                if (!ledger.consume(DISPATCH_ENTRY_WORK * (agreement.members.size() + 1), RefinementWork::MasterPreparation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                for (const auto & member : agreement.members)
                {
                    if (member.forwarder == GuardedNativeForwarder::CommandRepair)
                    {
                        basis.commandRepairKinds |= static_cast<quint8>(1u << static_cast<unsigned>(kind));
                    }
                    if (const auto* definition = std::get_if<GuardedCommandUnitDefinition>(&member.evidence))
                    {
                        if (*definition == GuardedCommandUnitDefinition::InfantryInit) { basis.unlimitedInfantryInit = true; }
                        else if (*definition == GuardedCommandUnitDefinition::InfantryCost) { basis.awdsInfantryCost = true; }
                        else if (*definition == GuardedCommandUnitDefinition::LightTankInit) { basis.awdsLightTankInit = true; }
                        else if (*definition == GuardedCommandUnitDefinition::LightTankCost) { basis.awdsLightTankCost = true; }
                        else if (*definition == GuardedCommandUnitDefinition::CommandWeather) { basis.commandWeather = true; }
                        else { basis.commandMovement = true; }
                    }
                }
            }
            if (basis.commandPolicyVerified)
            {
                if (!ledger.consume(DISPATCH_ENTRY_WORK * (agreement.members.size() + 1), RefinementWork::MasterPreparation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                for (const auto & member : agreement.members)
                {
                    basis.commandCallbacks |= guardedCommandCallbackBit(member.forwarder);
                    if (std::holds_alternative<GuardedCommandTerrainDefinition>(member.evidence)) { basis.aw2ForestDefense = true; }
                    if (std::holds_alternative<GuardedCommandRankingBonus>(member.evidence)) { basis.commandRankingBonuses = true; }
                }
            }
            const auto status = agreement.status;
            basis.modules.push_back(std::make_shared<NativeModuleBasis>(
                NativeModuleBasis{std::move(agreement), {}, {}, {}, {}, {}, {}, {}, {}, {}, {}}));
            if (status != GuardedNativeBindingStatus::Bound)
            {
                return sourceFailure(status);
            }
            if (projection == NativeModuleProjection::Co)
            {
                auto consequences = catalog.authenticateStockCoConsequences(engine, kind);
                const auto consequenceStatus = consequences.status;
                basis.modules.back()->consequences = std::move(consequences);
                if (consequenceStatus != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(consequenceStatus);
                }
            }
            else if (projection == NativeModuleProjection::Actor)
            {
                auto actions = catalog.authenticateUnitActions(engine, kind);
                const auto actionStatus = actions.status;
                basis.modules.back()->actions = std::move(actions);
                if (actionStatus != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(actionStatus);
                }
                auto absent = catalog.authenticateCurrentAbsentHooks(engine, kind);
                const auto absentStatus = absent.status;
                basis.modules.back()->absentHooks = std::move(absent);
                if (absentStatus != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(absentStatus);
                }
                auto tables = catalog.authenticateUnitPresentationTables(engine, kind);
                const auto tableStatus = tables.status;
                basis.modules.back()->tables = std::move(tables);
                if (tableStatus != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(tableStatus);
                }
                auto law = catalog.authenticateConstantLaw(engine, kind, GuardedNativeConstantGetter::UnitCost, basis.commandPolicyVerified);
                const auto lawStatus = law.status;
                basis.modules.back()->economicLaw = std::move(law);
                if (lawStatus != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(lawStatus);
                }
            }
            else if (kind == GuardedNativeCodeKind::Capture)
            {
                auto buildings = catalog.authenticateCaptureBuildings(engine);
                const auto buildingStatus = buildings.status;
                basis.modules.back()->capturableBuildings = std::move(buildings);
                if (buildingStatus != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(buildingStatus);
                }
                auto army = catalog.authenticateCaptureArmy(engine);
                const auto armyStatus = army.status;
                basis.modules.back()->captureArmy = std::move(army);
                if (armyStatus != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(armyStatus);
                }
            }
            else if (projection == NativeModuleProjection::Property)
            {
                const bool projectArmies = basis.commandPolicyVerified && kind == GuardedNativeCodeKind::Headquarters;
                auto tables = projectArmies ? catalog.authenticateHeadquartersGameplayTables(engine)
                                            : catalog.authenticateStockPropertyTables(engine, kind);
                const auto tableStatus = tables.status;
                basis.modules.back()->tables = std::move(tables);
                if (tableStatus != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(tableStatus);
                }
                if (projectArmies)
                {
                    if (!basis.inventory.playerMaterial) { return MasterNativeSourceStatus::UnsupportedInventory; }
                    auto armies = catalog.authenticateHeadquartersArmyTables(engine, *basis.inventory.playerMaterial);
                    const auto armyStatus = armies.status;
                    basis.modules.back()->armyTables = std::move(armies);
                    if (armyStatus != GuardedNativeBindingStatus::Bound) { return sourceFailure(armyStatus); }
                }
                auto law = catalog.authenticateConstantLaw(
                    engine, kind, GuardedNativeConstantGetter::PropertyIncome);
                const auto lawStatus = law.status;
                basis.modules.back()->economicLaw = std::move(law);
                if (lawStatus != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(lawStatus);
                }
            }
            else if (projection == NativeModuleProjection::Movement &&
                     kind != GuardedNativeCodeKind::MovementTable)
            {
                auto tables = catalog.authenticateStockMovementTables(engine, kind);
                const auto tableStatus = tables.status;
                basis.modules.back()->tables = std::move(tables);
                if (tableStatus != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(tableStatus);
                }
            }
            else if (projection == NativeModuleProjection::Context &&
                     kind == GuardedNativeCodeKind::Terrain && basis.commandPolicyVerified)
            {
                auto prefix = catalog.authenticateStockWeatherPrefix(engine);
                const auto prefixStatus = prefix.status;
                basis.modules.back()->weatherPrefix = prefix;
                if (prefixStatus != GuardedNativeBindingStatus::Bound) { return sourceFailure(prefixStatus); }
            }
            else if (projection == NativeModuleProjection::Context)
            {
                auto tables = catalog.authenticateStockContextTables(engine, kind);
                const auto tableStatus = tables.status;
                basis.modules.back()->tables = std::move(tables);
                if (tableStatus != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(tableStatus);
                }
            }
            else if (projection == NativeModuleProjection::Policy &&
                     kind == GuardedNativeCodeKind::Ranking)
            {
                auto tables = catalog.authenticateStockPolicyTables(engine, kind);
                const auto tableStatus = tables.status;
                basis.modules.back()->tables = std::move(tables);
                if (tableStatus != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(tableStatus);
                }
            }
            else if (projection == NativeModuleProjection::CurrentFire)
            {
                auto absent = catalog.authenticateCurrentAbsentHooks(engine, kind);
                const auto absentStatus = absent.status;
                basis.modules.back()->absentHooks = std::move(absent);
                if (absentStatus != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(absentStatus);
                }
            }
            else if (projection == NativeModuleProjection::Weapon)
            {
                auto tables = catalog.authenticateCurrentWeaponTables(engine, kind);
                const auto tableStatus = tables.status;
                basis.modules.back()->tables = std::move(tables);
                if (tableStatus != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(tableStatus);
                }
            }
            basis.collected[index] = true;
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus collectActorModules(
            NativeSourceBasis & basis, GuardedNativeCodeCatalog & catalog,
            GameMap & map, QJSEngine & engine, RefinementLedger & ledger, GuardedNativeMethodCache & contracts)
        {
            const auto helperStatus = collectModule(basis, catalog, engine, GuardedNativeCodeKind::Unit,
                NativeModuleProjection::UnitHelper, ledger);
            if (helperStatus != MasterNativeSourceStatus::Ready)
            {
                return helperStatus;
            }
            for (const auto & actor : basis.inventory.actors)
            {
                const auto kind = catalog.kindForGlobal(actor.type);
                if (kind.status != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(kind.status);
                }
                if (!actorKind(kind.kind))
                {
                    return MasterNativeSourceStatus::UnsupportedCallbacks;
                }
                const auto status = collectModule(
                    basis, catalog, engine, kind.kind, NativeModuleProjection::Actor, ledger);
                if (status != MasterNativeSourceStatus::Ready)
                {
                    return status;
                }
            }
            if (basis.commandRepairKinds || basis.unlimitedInfantryInit || basis.awdsLightTankInit || basis.commandWeather || basis.commandMovement)
            {
                const auto receivers = authenticateCommandUnits(map, engine, basis.inventory,
                    {.repair = basis.commandRepairKinds != 0, .infantryInit = basis.unlimitedInfantryInit, .lightTankInit = basis.awdsLightTankInit,
                     .weather = basis.commandWeather, .movement = basis.commandMovement}, ledger, &contracts);
                if (receivers != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(receivers);
                }
            }
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus collectCurrentWeapon(
            NativeSourceBasis & basis, GuardedNativeCodeCatalog & catalog, QJSEngine & engine,
            const QString & name, RefinementLedger & ledger)
        {
            if (!ledger.consume(DISPATCH_ENTRY_WORK, RefinementWork::MasterPreparation))
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            if (name.isEmpty())
            {
                return MasterNativeSourceStatus::Ready;
            }
            const auto weapon = catalog.kindForGlobal(name);
            if (weapon.status != GuardedNativeBindingStatus::Bound)
            {
                return sourceFailure(weapon.status);
            }
            if (weapon.kind != GuardedNativeCodeKind::InfantryMachineGun &&
                weapon.kind != GuardedNativeCodeKind::SniperWeapon)
            {
                return MasterNativeSourceStatus::UnsupportedCallbacks;
            }
            return collectModule(basis, catalog, engine, weapon.kind, NativeModuleProjection::Weapon, ledger);
        }

        bool isolatedInfantryFire(const GameMap & map)
        {
            return Mainapp::getInstance()->getNoUi() && map.getMenu() == nullptr;
        }

        MasterNativeSourceStatus collectCurrentFireModules(
            NativeSourceBasis & basis, GuardedNativeCodeCatalog & catalog, QJSEngine & engine,
            const MasterAssignmentProblem & problem, GameMap & map, RefinementLedger & ledger)
        {
            const auto actions = problem.actionTemplates();
            if (actions.size() > GUARDED_STOCK_MAX_ACTORS)
            {
                return MasterNativeSourceStatus::UnsupportedInventory;
            }
            if (!ledger.consume(DISPATCH_ENTRY_WORK * (actions.size() + 1),
                                RefinementWork::MasterPreparation))
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            const PlannedAction* fire = nullptr;
            for (const auto & action : actions)
            {
                if (action.kind == PlanBundleKind::Fire || action.kind == PlanBundleKind::MoveAndFire)
                {
                    if (fire != nullptr)
                    {
                        return MasterNativeSourceStatus::UnsupportedCallbacks;
                    }
                    fire = &action;
                }
            }
            if (fire == nullptr)
            {
                return MasterNativeSourceStatus::Ready;
            }
            // Presentation may create a temporary infantry and invoke additional sprite callbacks.
            if (basis.commandRankingBonuses || basis.aw2ForestDefense || basis.commandCallbacks || basis.commandMovement || basis.commandWeather || basis.awdsLightTankInit || basis.awdsLightTankCost || (basis.unlimitedInfantryInit && !isolatedInfantryFire(map)))
            {
                return MasterNativeSourceStatus::UnsupportedCallbacks;
            }
            const auto presentation = Settings::getInstance()->getBattleAnimationType();
            if (presentation != GameEnums::BattleAnimationType_Overworld &&
                presentation != GameEnums::BattleAnimationType_Detail)
            {
                return MasterNativeSourceStatus::UnsupportedCallbacks;
            }
            basis.battlePresentation = static_cast<std::int32_t>(presentation);
            if (!ledger.consume(DISPATCH_ENTRY_WORK * (basis.inventory.actors.size() + 1),
                                RefinementWork::MasterPreparation))
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            const auto actor = std::find_if(basis.inventory.actors.begin(), basis.inventory.actors.end(),
                [&](const auto & record) { return record.id == fire->unitId; });
            if (actor == basis.inventory.actors.end() || !actor->material)
            {
                return MasterNativeSourceStatus::UnsupportedInventory;
            }
            auto status = collectModule(
                basis, catalog, engine, GuardedNativeCodeKind::Fire, NativeModuleProjection::CurrentFire, ledger);
            if (status == MasterNativeSourceStatus::Ready)
            {
                status = collectModule(
                    basis, catalog, engine, GuardedNativeCodeKind::Weapon, NativeModuleProjection::Weapon, ledger);
            }
            if (status == MasterNativeSourceStatus::Ready)
            {
                status = collectCurrentWeapon(basis, catalog, engine, actor->material->weapon1, ledger);
            }
            if (status == MasterNativeSourceStatus::Ready)
            {
                status = collectCurrentWeapon(basis, catalog, engine, actor->material->weapon2, ledger);
            }
            if (status == MasterNativeSourceStatus::Ready && presentation == GameEnums::BattleAnimationType_Detail)
            {
                GuardedNativeDetailPresentation captured;
                const auto detail = captured.capture(map, *fire, basis.inventory, engine, catalog, ledger);
                if (!RelativeStockDetail::checkedAdd(basis.receiverConstructionWork,
                        captured.receiverConstructionWork()))
                {
                    return MasterNativeSourceStatus::IncompleteDomain;
                }
                if (detail != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(detail);
                }
                basis.detailPresentation = std::move(captured);
                for (const auto kind : {GuardedNativeCodeKind::BattleAnimation,
                                       GuardedNativeCodeKind::InfantryBattleAnimation,
                                       GuardedNativeCodeKind::SniperBattleAnimation})
                {
                    basis.collected[static_cast<std::size_t>(kind)] = true;
                }
            }
            return status;
        }

        MasterNativeSourceStatus collectNominalCo(
            NativeSourceBasis & basis, GuardedNativeCodeCatalog & catalog, QJSEngine & engine,
            const QString & id, RefinementLedger & ledger)
        {
            const auto kind = catalog.kindForGlobal(id);
            if (kind.status != GuardedNativeBindingStatus::Bound)
            {
                return sourceFailure(kind.status);
            }
            if (kind.kind != GuardedNativeCodeKind::Andy && kind.kind != GuardedNativeCodeKind::Jess)
            {
                return MasterNativeSourceStatus::UnsupportedCallbacks;
            }
            const auto status = collectModule(
                basis, catalog, engine, kind.kind, NativeModuleProjection::Co, ledger);
            if (status != MasterNativeSourceStatus::Ready)
            {
                return status;
            }
            if (!ledger.consume(NOMINAL_RECORD_WORK, RefinementWork::MasterPreparation))
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            if (basis.modules.size() > CODE_KIND_COUNT)
            {
                return MasterNativeSourceStatus::UnsupportedCallbacks;
            }
            const auto nominal = std::find_if(basis.modules.begin(), basis.modules.end(),
                [&](const auto & module) { return module->callbacks.kind == kind.kind; });
            if (nominal == basis.modules.end())
            {
                return MasterNativeSourceStatus::UnsupportedCallbacks;
            }
            if (!(*nominal)->nominal)
            {
                auto agreement = catalog.authenticateStockCoNominal(engine, kind.kind);
                const auto nominalStatus = agreement.status;
                (*nominal)->nominal = std::move(agreement);
                if (nominalStatus != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(nominalStatus);
                }
            }
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus collectCoModules(
            NativeSourceBasis & basis, GuardedNativeCodeCatalog & catalog,
            QJSEngine & engine, RefinementLedger & ledger)
        {
            for (const auto & player : *basis.inventory.playerMaterial)
            {
                for (const auto & co : player.cos)
                {
                    if (!co)
                    {
                        continue;
                    }
                    auto status = collectModule(
                        basis, catalog, engine, GuardedNativeCodeKind::Co, NativeModuleProjection::Co, ledger);
                    if (status != MasterNativeSourceStatus::Ready)
                    {
                        return status;
                    }
                    status = collectNominalCo(basis, catalog, engine, co->id, ledger);
                    if (status != MasterNativeSourceStatus::Ready)
                    {
                        return status;
                    }
                    for (const auto & perk : co->effectivePerks)
                    {
                        const auto kind = catalog.kindForGlobal(perk);
                        if (kind.status != GuardedNativeBindingStatus::Bound)
                        {
                            return sourceFailure(kind.status);
                        }
                        if (!coKind(kind.kind))
                        {
                            return MasterNativeSourceStatus::UnsupportedCallbacks;
                        }
                        status = collectModule(
                            basis, catalog, engine, kind.kind, NativeModuleProjection::Co, ledger);
                        if (status != MasterNativeSourceStatus::Ready)
                        {
                            return status;
                        }
                    }
                }
            }
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus collectStockActionModules(
            NativeSourceBasis & basis, GuardedNativeCodeCatalog & catalog,
            QJSEngine & engine, RefinementLedger & ledger)
        {
            for (const auto kind : {GuardedNativeCodeKind::Action, GuardedNativeCodeKind::Wait,
                                   GuardedNativeCodeKind::Capture, GuardedNativeCodeKind::Trap,
                                   GuardedNativeCodeKind::NextPlayer})
            {
                const auto status = collectModule(
                    basis, catalog, engine, kind, NativeModuleProjection::StockAction, ledger);
                if (status != MasterNativeSourceStatus::Ready)
                {
                    return status;
                }
            }
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus collectPropertyModules(
            NativeSourceBasis & basis, GuardedNativeCodeCatalog & catalog,
            QJSEngine & engine, RefinementLedger & ledger)
        {
            auto status = collectModule(
                basis, catalog, engine, GuardedNativeCodeKind::Building, NativeModuleProjection::Property, ledger);
            if (status != MasterNativeSourceStatus::Ready)
            {
                return status;
            }
            for (const auto & property : basis.inventory.properties)
            {
                const auto kind = catalog.kindForGlobal(property.type);
                if (kind.status != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(kind.status);
                }
                if (kind.kind != GuardedNativeCodeKind::Town && kind.kind != GuardedNativeCodeKind::Factory &&
                    kind.kind != GuardedNativeCodeKind::Headquarters)
                {
                    return MasterNativeSourceStatus::UnsupportedCallbacks;
                }
                status = collectModule(basis, catalog, engine, kind.kind, NativeModuleProjection::Property, ledger);
                if (status != MasterNativeSourceStatus::Ready)
                {
                    return status;
                }
            }
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus collectMovementModules(
            NativeSourceBasis & basis, GuardedNativeCodeCatalog & catalog,
            QJSEngine & engine, RefinementLedger & ledger)
        {
            auto status = collectModule(
                basis, catalog, engine, GuardedNativeCodeKind::MovementTable,
                NativeModuleProjection::Movement, ledger);
            if (status != MasterNativeSourceStatus::Ready)
            {
                return status;
            }
            for (const auto & actor : basis.inventory.actors)
            {
                if (!actor.material)
                {
                    return MasterNativeSourceStatus::UnsupportedInventory;
                }
                const auto kind = catalog.kindForGlobal(actor.material->movementType);
                if (kind.status != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(kind.status);
                }
                if (kind.kind != GuardedNativeCodeKind::MoveFeet &&
                    kind.kind != GuardedNativeCodeKind::MoveTireA &&
                    kind.kind != GuardedNativeCodeKind::MoveTank)
                {
                    return MasterNativeSourceStatus::UnsupportedCallbacks;
                }
                status = collectModule(basis, catalog, engine, kind.kind, NativeModuleProjection::Movement, ledger);
                if (status != MasterNativeSourceStatus::Ready)
                {
                    return status;
                }
            }
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus retainConstructionQuery(
            NativeSourceBasis & basis, GuardedNativeConstructionAgreement agreement)
        {
            if (agreement.status != GuardedNativeBindingStatus::Bound)
            {
                return sourceFailure(agreement.status);
            }
            basis.constructionQueries.push_back(std::move(agreement));
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus appendConstructionType(
            std::vector<QString> & names, const QString & type, RefinementLedger & ledger)
        {
            if (!ledger.consume(CONSTRUCTION_QUERY_CONTROL_WORK, RefinementWork::MasterPreparation))
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            if (type.isEmpty() || type.size() > MAX_CONSTRUCTION_TYPE ||
                names.size() >= MAX_CONSTRUCTION_QUERIES)
            {
                return MasterNativeSourceStatus::UnsupportedCallbacks;
            }
            const auto work = CONSTRUCTION_QUERY_CHARACTER_WORK * MAX_CONSTRUCTION_TYPE *
                (names.size() + 1);
            if (!ledger.consume(work, RefinementWork::MasterPreparation))
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            if (std::find(names.begin(), names.end(), type) != names.end())
            {
                return MasterNativeSourceStatus::Ready;
            }
            names.push_back(type);
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus collectConstructionTableTypes(
            std::vector<QString> & names, const GuardedNativeArrayAgreement & table, RefinementLedger & ledger)
        {
            if (!ledger.consume(DISPATCH_ENTRY_WORK, RefinementWork::MasterPreparation))
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            if (table.status != GuardedNativeBindingStatus::Bound || table.actual.empty() ||
                table.actual.size() > MAX_CONSTRUCTION_QUERIES + 1)
            {
                return MasterNativeSourceStatus::UnsupportedCallbacks;
            }
            const auto & root = table.actual.front();
            if (root.kind != GuardedNativeDataKind::Array || root.elements != table.actual.size() - 1)
            {
                return MasterNativeSourceStatus::UnsupportedCallbacks;
            }
            for (std::size_t index = 1; index < table.actual.size(); ++index)
            {
                if (!ledger.consume(DISPATCH_ENTRY_WORK, RefinementWork::MasterPreparation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                const auto & node = table.actual[index];
                if (node.kind != GuardedNativeDataKind::Scalar ||
                    node.scalar.status != GuardedScriptScalarStatus::Verified ||
                    node.scalar.kind != GuardedScriptValueKind::String ||
                    !std::holds_alternative<QString>(node.scalar.value))
                {
                    return MasterNativeSourceStatus::UnsupportedCallbacks;
                }
                const auto status = appendConstructionType(names, std::get<QString>(node.scalar.value), ledger);
                if (status != MasterNativeSourceStatus::Ready)
                {
                    return status;
                }
            }
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus collectConstructionCoTypes(
            NativeSourceBasis & basis, GuardedNativeCodeCatalog & catalog,
            std::vector<QString> & names, RefinementLedger & ledger)
        {
            for (const auto & player : *basis.inventory.playerMaterial)
            {
                for (const auto & co : player.cos)
                {
                    if (!co)
                    {
                        continue;
                    }
                    for (const auto & perk : co->effectivePerks)
                    {
                        const auto lookup = catalog.kindForGlobal(perk);
                        if (lookup.status != GuardedNativeBindingStatus::Bound)
                        {
                            return sourceFailure(lookup.status);
                        }
                        auto kind = GuardedNativeCodeKind::None;
                        if (lookup.kind == GuardedNativeCodeKind::Andy)
                        {
                            kind = GuardedNativeCodeKind::RepairTank;
                        }
                        else if (lookup.kind == GuardedNativeCodeKind::Jess)
                        {
                            kind = GuardedNativeCodeKind::TankHunter;
                        }
                        else if (lookup.kind != GuardedNativeCodeKind::TagPower)
                        {
                            return MasterNativeSourceStatus::UnsupportedCallbacks;
                        }
                        if (kind == GuardedNativeCodeKind::None)
                        {
                            continue;
                        }
                        if (!ledger.consume(NOMINAL_RECORD_WORK, RefinementWork::MasterPreparation))
                        {
                            return MasterNativeSourceStatus::BudgetExhausted;
                        }
                        const auto index = static_cast<std::size_t>(kind);
                        if (basis.constructionKinds[index])
                        {
                            continue;
                        }
                        const auto target = catalog.constructionType(kind);
                        if (target.status != GuardedNativeBindingStatus::Bound)
                        {
                            return sourceFailure(target.status);
                        }
                        const auto status = appendConstructionType(names, target.type, ledger);
                        if (status != MasterNativeSourceStatus::Ready)
                        {
                            return status;
                        }
                        basis.constructionKinds[index] = true;
                    }
                }
            }
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus collectConstructionQueries(
            NativeSourceBasis & basis, GuardedNativeCodeCatalog & catalog,
            QJSEngine & engine, RefinementLedger & ledger)
        {
            if (!basis.inventory.gameRuleIds || !basis.inventory.playerMaterial)
            {
                return MasterNativeSourceStatus::UnsupportedInventory;
            }
            for (const auto & rule : *basis.inventory.gameRuleIds)
            {
                if (!ledger.consume(DISPATCH_ENTRY_WORK + CONSTRUCTION_QUERY_CHARACTER_WORK * rule.size(),
                                    RefinementWork::MasterPreparation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                if (rule == HOVERCRAFT_FACTORY_RULE)
                {
                    return MasterNativeSourceStatus::UnsupportedCallbacks;
                }
            }
            const auto storageWork = CONSTRUCTION_QUERY_CONTROL_WORK +
                (sizeof(GuardedNativeConstructionAgreement) + sizeof(QString)) * MAX_CONSTRUCTION_QUERIES;
            if (!ledger.consume(storageWork, RefinementWork::MasterPreparation))
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            basis.constructionQueries.reserve(MAX_CONSTRUCTION_QUERIES);
            std::vector<QString> names;
            names.reserve(MAX_CONSTRUCTION_QUERIES);
            if (!ledger.consume(NOMINAL_RECORD_WORK, RefinementWork::MasterPreparation))
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            const auto member = QString::fromLatin1(
                GUARDED_NATIVE_CONSTRUCTION_LIST_MEMBER.data(), GUARDED_NATIVE_CONSTRUCTION_LIST_MEMBER.size());
            for (const auto & module : basis.modules)
            {
                if (!module->tables || module->callbacks.kind == GuardedNativeCodeKind::Building)
                {
                    continue;
                }
                const auto kind = module->callbacks.kind;
                if (kind != GuardedNativeCodeKind::Town && kind != GuardedNativeCodeKind::Factory &&
                    kind != GuardedNativeCodeKind::Headquarters)
                {
                    continue;
                }
                for (const auto & table : module->tables->tables)
                {
                    if (!ledger.consume(DISPATCH_ENTRY_WORK + CONSTRUCTION_QUERY_CHARACTER_WORK * table.name.size(),
                                        RefinementWork::MasterPreparation))
                    {
                        return MasterNativeSourceStatus::BudgetExhausted;
                    }
                    if (table.name == member)
                    {
                        const auto status = collectConstructionTableTypes(names, table.evidence, ledger);
                        if (status != MasterNativeSourceStatus::Ready)
                        {
                            return status;
                        }
                    }
                }
            }
            const auto gathered = collectConstructionCoTypes(basis, catalog, names, ledger);
            if (gathered != MasterNativeSourceStatus::Ready)
            {
                return gathered;
            }
            const auto allNames = std::span<const QString>(names);
            for (std::size_t begin = 0; begin < names.size(); begin += GUARDED_SCRIPT_MAX_INDEXED_GLOBAL_NAMES)
            {
                const auto batch = allNames.subspan(begin,
                    std::min(GUARDED_SCRIPT_MAX_INDEXED_GLOBAL_NAMES, names.size() - begin));
                if (!ledger.consume(CONSTRUCTION_INDEX_ENTRY_WORK * batch.size(),
                                    RefinementWork::MasterPreparation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                const auto prepared = catalog.prepareConstructionQueries(engine, batch);
                if (prepared != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(prepared);
                }
                for (const auto & name : batch)
                {
                    if (!ledger.consume(CONSTRUCTION_QUERY_CONTROL_WORK, RefinementWork::MasterPreparation))
                    {
                        return MasterNativeSourceStatus::BudgetExhausted;
                    }
                    const auto status = retainConstructionQuery(
                        basis, catalog.authenticateConstructionQuery(engine, name));
                    if (status != MasterNativeSourceStatus::Ready)
                    {
                        return status;
                    }
                }
            }
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus collectContextModules(
            NativeSourceBasis & basis, GuardedNativeCodeCatalog & catalog,
            QJSEngine & engine, RefinementLedger & ledger)
        {
            for (const auto kind : {GuardedNativeCodeKind::Terrain, GuardedNativeCodeKind::Player})
            {
                const auto status = collectModule(
                    basis, catalog, engine, kind, NativeModuleProjection::Context, ledger);
                if (status != MasterNativeSourceStatus::Ready)
                {
                    return status;
                }
            }
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus collectTerrainModules(
            NativeSourceBasis & basis, GuardedNativeCodeCatalog & catalog,
            QJSEngine & engine, RefinementLedger & ledger)
        {
            if (!basis.inventory.terrains)
            {
                return MasterNativeSourceStatus::UnsupportedInventory;
            }
            for (const auto & terrain : *basis.inventory.terrains)
            {
                for (const auto & layer : terrain.layers)
                {
                    if (!ledger.consume(DISPATCH_ENTRY_WORK, RefinementWork::MasterPreparation))
                    {
                        return MasterNativeSourceStatus::BudgetExhausted;
                    }
                    const auto kind = catalog.kindForGlobal(layer.type);
                    if (kind.status != GuardedNativeBindingStatus::Bound)
                    {
                        return sourceFailure(kind.status);
                    }
                    switch (kind.kind)
                    {
                        case GuardedNativeCodeKind::Plains:
                        case GuardedNativeCodeKind::Forest:
                        case GuardedNativeCodeKind::Mountain:
                        case GuardedNativeCodeKind::Street:
                        case GuardedNativeCodeKind::Sea:
                        case GuardedNativeCodeKind::Reaf:
                            break;
                        default:
                            return MasterNativeSourceStatus::UnsupportedCallbacks;
                    }
                    const auto status = collectModule(
                        basis, catalog, engine, kind.kind, NativeModuleProjection::Terrain, ledger);
                    if (status != MasterNativeSourceStatus::Ready)
                    {
                        return status;
                    }
                }
            }
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus receiverFailure(GuardedNativeReceiverStatus status)
        {
            if (status == GuardedNativeReceiverStatus::BudgetExhausted)
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            return MasterNativeSourceStatus::UnsupportedReceiver;
        }

        MasterNativeSourceStatus collectWeatherAt(
            NativeSourceBasis & basis, GuardedNativeCodeCatalog & catalog,
            QJSEngine & engine, std::int32_t index, RefinementLedger & ledger)
        {
            if (!ledger.consume(DISPATCH_ENTRY_WORK, RefinementWork::MasterPreparation))
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            if (!basis.calendar || index < 0 ||
                static_cast<std::size_t>(index) >= basis.calendar->basis.weatherIds.size())
            {
                return MasterNativeSourceStatus::UnsupportedCalendar;
            }
            const auto kind = catalog.kindForGlobal(basis.calendar->basis.weatherIds[index]);
            if (kind.status != GuardedNativeBindingStatus::Bound)
            {
                return sourceFailure(kind.status);
            }
            if (kind.kind != GuardedNativeCodeKind::Sun)
            {
                return MasterNativeSourceStatus::UnsupportedCallbacks;
            }
            return collectModule(basis, catalog, engine, kind.kind, NativeModuleProjection::Policy, ledger);
        }

        MasterNativeSourceStatus collectPolicyModules(
            NativeSourceBasis & basis, GuardedNativeCodeCatalog & catalog,
            QJSEngine & engine, RefinementLedger & ledger)
        {
            if (!basis.calendar)
            {
                return MasterNativeSourceStatus::UnsupportedCalendar;
            }
            for (const auto kind : {GuardedNativeCodeKind::Weather, GuardedNativeCodeKind::VictoryRule,
                                    GuardedNativeCodeKind::Ranking, GuardedNativeCodeKind::Global})
            {
                const auto status = collectModule(basis, catalog, engine, kind, NativeModuleProjection::Policy, ledger);
                if (status != MasterNativeSourceStatus::Ready)
                {
                    return status;
                }
            }
            const auto & calendar = *basis.calendar;
            for (const auto index : {calendar.basis.currentWeather, calendar.basis.startWeather,
                                     calendar.basis.dayWeather})
            {
                const auto status = collectWeatherAt(basis, catalog, engine, index, ledger);
                if (status != MasterNativeSourceStatus::Ready)
                {
                    return status;
                }
            }
            for (const auto & phase : calendar.phases)
            {
                const auto status = collectWeatherAt(basis, catalog, engine, phase.weather, ledger);
                if (status != MasterNativeSourceStatus::Ready)
                {
                    return status;
                }
            }
            for (const auto & rule : calendar.basis.victoryRules)
            {
                if (!ledger.consume(DISPATCH_ENTRY_WORK, RefinementWork::MasterPreparation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                if (basis.commandPolicyVerified && rule.id == GUARDED_NATIVE_COMMAND_RULE)
                {
                    continue;
                }
                const auto kind = catalog.kindForGlobal(rule.id);
                if (kind.status != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(kind.status);
                }
                if (kind.kind != GuardedNativeCodeKind::NoHeadquarters && kind.kind != GuardedNativeCodeKind::NoUnits)
                {
                    return MasterNativeSourceStatus::UnsupportedCallbacks;
                }
                const auto status = collectModule(basis, catalog, engine, kind.kind, NativeModuleProjection::Policy, ledger);
                if (status != MasterNativeSourceStatus::Ready)
                {
                    return status;
                }
            }
            return MasterNativeSourceStatus::Ready;
        }

        template<typename Native>
        MasterNativeSourceStatus collectReceiver(
            NativeSourceBasis & basis, NativeReceiverKind kind, Native* receiver,
            QJSEngine & engine, RefinementLedger & ledger)
        {
            if (!ledger.consume(RECEIVER_ENTRY_WORK, RefinementWork::MasterPreparation))
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            if (receiver == nullptr || receiver->thread() != QThread::currentThread())
            {
                return MasterNativeSourceStatus::UnsupportedReceiver;
            }
            const auto index = static_cast<std::size_t>(kind);
            if (index >= basis.receiverMethods.size())
            {
                return MasterNativeSourceStatus::UnsupportedReceiver;
            }
            GuardedNativeReceiverStatus status;
            if constexpr (std::is_base_of_v<JsThis, Native>)
            {
                status = basis.receiverMethods[index].authenticate(
                    engine, JsThis::getJsThis(receiver), *receiver, ledger);
            }
            else
            {
                status = basis.receiverMethods[index].authenticateReturned(engine, *receiver, ledger);
            }
            if (status != GuardedNativeReceiverStatus::Bound)
            {
                return receiverFailure(status);
            }
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus prepareReceiverContracts(
            NativeSourceBasis & basis, RefinementLedger & ledger, GuardedNativeMethodCache & contracts)
        {
            if (!ledger.consume(RECEIVER_ENTRY_WORK * RECEIVER_SCHEMAS.size(), RefinementWork::MasterPreparation))
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            for (const auto & schema : RECEIVER_SCHEMAS)
            {
                const auto before = ledger.stats().spent();
                const bool shared = basis.commandPolicyVerified &&
                    std::ranges::any_of(RECEIVER_METHOD_UNIONS, [&](const auto & entry) { return entry.metadata == schema.metadata; });
                auto contract = shared ? contracts.prepare(*schema.metadata, schema.methods, ledger)
                    : GuardedNativeMethodContract::prepare(*schema.metadata, schema.methods, ledger);
                if (!RelativeStockDetail::checkedAdd(basis.receiverConstructionWork, ledger.stats().spent() - before))
                {
                    return MasterNativeSourceStatus::IncompleteDomain;
                }
                const auto status = contract.status();
                basis.receiverMethods[static_cast<std::size_t>(schema.kind)] = std::move(contract);
                if (status != GuardedNativeReceiverStatus::Bound)
                {
                    return receiverFailure(status);
                }
            }
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus collectActorReceivers(
            NativeSourceBasis & basis, GameMap & map, QJSEngine & engine, RefinementLedger & ledger)
        {
            for (const auto & actor : basis.inventory.actors)
            {
                if (!ledger.consume(RECEIVER_ENTRY_WORK, RefinementWork::MasterPreparation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                auto* terrain = map.getTerrain(actor.tile % basis.inventory.width, actor.tile / basis.inventory.width);
                if (terrain == nullptr)
                {
                    return MasterNativeSourceStatus::UnsupportedReceiver;
                }
                auto* unit = terrain->getUnit();
                if (unit == nullptr || unit->getUniqueID() != actor.id || unit->getMap() != &map)
                {
                    return MasterNativeSourceStatus::UnsupportedReceiver;
                }
                const auto status = collectReceiver(basis, NativeReceiverKind::Unit, unit, engine, ledger);
                if (status != MasterNativeSourceStatus::Ready)
                {
                    return status;
                }
            }
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus collectPlayerReceivers(
            NativeSourceBasis & basis, GameMap & map, QJSEngine & engine, RefinementLedger & ledger)
        {
            if (!basis.inventory.playerMaterial)
            {
                return MasterNativeSourceStatus::UnsupportedInventory;
            }
            for (std::size_t side = 0; side < basis.inventory.playerMaterial->size(); ++side)
            {
                if (!ledger.consume(RECEIVER_ENTRY_WORK, RefinementWork::MasterPreparation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                auto* player = map.getPlayer(static_cast<std::int32_t>(side));
                if (player == nullptr || player->getMap() != &map)
                {
                    return MasterNativeSourceStatus::UnsupportedReceiver;
                }
                auto* input = player->getBaseGameInput();
                if (input == nullptr || input->getMap() != &map || input->getPlayer() != player ||
                    input->getAiType() == GameEnums::AiTypes_Human)
                {
                    return MasterNativeSourceStatus::UnsupportedReceiver;
                }
                basis.inputs[side] = static_cast<std::int32_t>(input->getAiType());
                auto inputKind = NativeReceiverKind::NormalInput;
                if (input->getAiType() == GameEnums::AiTypes_Coordinated)
                {
                    inputKind = NativeReceiverKind::CoordinatedInput;
                }
                else if (input->getAiType() != GameEnums::AiTypes_Normal)
                {
                    return MasterNativeSourceStatus::UnsupportedReceiver;
                }
                auto inputStatus = collectReceiver(basis, inputKind, input, engine, ledger);
                if (inputStatus != MasterNativeSourceStatus::Ready)
                {
                    return inputStatus;
                }
                auto status = collectReceiver(basis, NativeReceiverKind::Player, player, engine, ledger);
                if (status != MasterNativeSourceStatus::Ready)
                {
                    return status;
                }
                const auto & cos = (*basis.inventory.playerMaterial)[side].cos;
                for (std::size_t slot = 0; slot < cos.size(); ++slot)
                {
                    if (!ledger.consume(RECEIVER_ENTRY_WORK, RefinementWork::MasterPreparation))
                    {
                        return MasterNativeSourceStatus::BudgetExhausted;
                    }
                    auto* co = player->getCO(static_cast<quint8>(slot));
                    if (cos[slot].has_value() != (co != nullptr))
                    {
                        return MasterNativeSourceStatus::UnsupportedReceiver;
                    }
                    if (co == nullptr)
                    {
                        continue;
                    }
                    if (co->getMap() != &map || co->getOwner() != player)
                    {
                        return MasterNativeSourceStatus::UnsupportedReceiver;
                    }
                    status = collectReceiver(basis, NativeReceiverKind::Co, co, engine, ledger);
                    if (status != MasterNativeSourceStatus::Ready)
                    {
                        return status;
                    }
                }
            }
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus collectTerrainReceivers(
            NativeSourceBasis & basis, GameMap & map, QJSEngine & engine, RefinementLedger & ledger)
        {
            if (!basis.inventory.terrains)
            {
                return MasterNativeSourceStatus::UnsupportedInventory;
            }
            for (const auto & tile : *basis.inventory.terrains)
            {
                if (!ledger.consume(RECEIVER_ENTRY_WORK, RefinementWork::MasterPreparation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                auto* terrain = map.getTerrain(tile.tile % basis.inventory.width, tile.tile / basis.inventory.width);
                for (std::size_t depth = 0; depth < tile.layers.size(); ++depth)
                {
                    const auto status = collectReceiver(basis, NativeReceiverKind::Terrain, terrain, engine, ledger);
                    if (status != MasterNativeSourceStatus::Ready)
                    {
                        return status;
                    }
                    terrain = terrain->getNextBaseTerrain();
                }
                if (terrain != nullptr)
                {
                    return MasterNativeSourceStatus::UnsupportedReceiver;
                }
            }
            for (const auto & property : basis.inventory.properties)
            {
                if (!ledger.consume(RECEIVER_ENTRY_WORK, RefinementWork::MasterPreparation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                auto* terrain = map.getTerrain(property.tile % basis.inventory.width, property.tile / basis.inventory.width);
                if (terrain == nullptr)
                {
                    return MasterNativeSourceStatus::UnsupportedReceiver;
                }
                const auto status = collectReceiver(
                    basis, NativeReceiverKind::Property, terrain->getBuilding(), engine, ledger);
                if (status != MasterNativeSourceStatus::Ready)
                {
                    return status;
                }
            }
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus collectRuleReceivers(
            NativeSourceBasis & basis, GameMap & map, QJSEngine & engine, RefinementLedger & ledger)
        {
            if (!basis.calendar)
            {
                return MasterNativeSourceStatus::UnsupportedCalendar;
            }
            if (!ledger.consume(RECEIVER_ENTRY_WORK, RefinementWork::MasterPreparation))
            {
                return MasterNativeSourceStatus::BudgetExhausted;
            }
            auto* rules = map.getGameRules();
            if (rules == nullptr || rules->getMap() != &map)
            {
                return MasterNativeSourceStatus::UnsupportedReceiver;
            }
            auto status = collectReceiver(basis, NativeReceiverKind::Rules, rules, engine, ledger);
            if (status != MasterNativeSourceStatus::Ready)
            {
                return status;
            }
            for (std::size_t index = 0; index < basis.calendar->basis.victoryRules.size(); ++index)
            {
                if (!ledger.consume(RECEIVER_ENTRY_WORK, RefinementWork::MasterPreparation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                auto* rule = rules->getVictoryRuleAtIndex(static_cast<std::int32_t>(index));
                if (rule == nullptr || rule->getMap() != &map)
                {
                    return MasterNativeSourceStatus::UnsupportedReceiver;
                }
                status = collectReceiver(basis, NativeReceiverKind::VictoryRule, rule, engine, ledger);
                if (status != MasterNativeSourceStatus::Ready)
                {
                    return status;
                }
            }
            for (std::size_t index = 0; index < basis.calendar->basis.weatherIds.size(); ++index)
            {
                if (!ledger.consume(RECEIVER_ENTRY_WORK, RefinementWork::MasterPreparation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                status = collectReceiver(
                    basis, NativeReceiverKind::Weather, rules->getWeather(static_cast<std::int32_t>(index)),
                    engine, ledger);
                if (status != MasterNativeSourceStatus::Ready)
                {
                    return status;
                }
            }
            return MasterNativeSourceStatus::Ready;
        }

        MasterNativeSourceStatus collectReceivers(
            NativeSourceBasis & basis, GuardedNativeCodeCatalog & catalog,
            GameMap & map, QJSEngine & engine, RefinementLedger & ledger, GuardedNativeMethodCache & contracts)
        {
            auto status = prepareReceiverContracts(basis, ledger, contracts);
            if (status == MasterNativeSourceStatus::Ready)
            {
                status = collectReceiver(basis, NativeReceiverKind::Map, &map, engine, ledger);
            }
            if (status != MasterNativeSourceStatus::Ready)
            {
                return status;
            }
            const auto alias = catalog.globalProperty(engine, MAP_ALIAS);
            if (alias.status != GuardedScriptPropertyStatus::Found)
            {
                if (alias.status == GuardedScriptPropertyStatus::BudgetExhausted)
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                return MasterNativeSourceStatus::UnsupportedReceiver;
            }
            const auto & mapContract = basis.receiverMethods[static_cast<std::size_t>(NativeReceiverKind::Map)];
            const auto aliasStatus = mapContract.authenticate(engine, alias.value, map, ledger);
            if (aliasStatus != GuardedNativeReceiverStatus::Bound)
            {
                return receiverFailure(aliasStatus);
            }
            const std::array globalReceivers{
                std::tuple{GLOBALS_ALIAS, NativeReceiverKind::Globals,
                           static_cast<QObject*>(GlobalUtils::getInstance())},
                std::tuple{ANIMATION_FACTORY_ALIAS, NativeReceiverKind::AnimationFactory,
                           static_cast<QObject*>(GameAnimationFactory::getInstance())},
            };
            for (const auto & [name, kind, receiver] : globalReceivers)
            {
                if (!ledger.consume(RECEIVER_ENTRY_WORK, RefinementWork::MasterPreparation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                const auto property = catalog.globalProperty(engine, name);
                if (property.status != GuardedScriptPropertyStatus::Found || receiver == nullptr)
                {
                    return property.status == GuardedScriptPropertyStatus::BudgetExhausted
                        ? MasterNativeSourceStatus::BudgetExhausted : MasterNativeSourceStatus::UnsupportedReceiver;
                }
                const auto & contract = basis.receiverMethods[static_cast<std::size_t>(kind)];
                const auto receiverStatus = contract.authenticate(engine, property.value, *receiver, ledger);
                if (receiverStatus != GuardedNativeReceiverStatus::Bound)
                {
                    return receiverFailure(receiverStatus);
                }
            }
            status = collectActorReceivers(basis, map, engine, ledger);
            if (status == MasterNativeSourceStatus::Ready)
            {
                status = collectPlayerReceivers(basis, map, engine, ledger);
            }
            if (status == MasterNativeSourceStatus::Ready)
            {
                status = collectTerrainReceivers(basis, map, engine, ledger);
            }
            if (status == MasterNativeSourceStatus::Ready)
            {
                status = collectRuleReceivers(basis, map, engine, ledger);
            }
            return status;
        }

        // No script runs while the live realm stays interrupted; the entry count exposes interpreter writes.
        class LiveRealmFence final
        {
        public:
            LiveRealmFence() = default;
            LiveRealmFence(const LiveRealmFence &) = delete;
            LiveRealmFence & operator=(const LiveRealmFence &) = delete;

            ~LiveRealmFence()
            {
                release();
            }

            bool hold()
            {
                if (m_realm || !Interpreter::isCurrentThreadOwner())
                {
                    return false;
                }
                auto* interpreter = Interpreter::getInstance();
                if (interpreter->getInJsCall() || interpreter->isInterrupted())
                {
                    return false;
                }
                interpreter->setInterrupted(true);
                m_realm = interpreter;
                m_entries = interpreter->getRealmEntries();
                return true;
            }

            bool intact() const
            {
                return m_realm && Interpreter::isCurrentThreadOwner() &&
                    Interpreter::getInstance() == m_realm.data() && m_realm->isInterrupted() &&
                    !m_realm->getInJsCall() && m_realm->getRealmEntries() == m_entries;
            }

            Interpreter* realm() const
            {
                return m_realm.data();
            }


        private:
            void release()
            {
                if (m_realm)
                {
                    m_realm->setInterrupted(false);
                }
                m_realm.clear();
            }

            QPointer<Interpreter> m_realm;
            quint64 m_entries{0};
        };

        class NativeRootSource final : public MasterGuardedRootSource
        {
        public:
            NativeRootSource(GameMap & map, const MasterAssignmentProblem & problem, RefinementLedger & ledger)
                : m_map(map), m_problem(problem), m_ledger(ledger), m_catalog(ledger), m_commandReceivers(ledger, RECEIVER_METHOD_UNIONS)
            {
            }

            MasterNativeSourceStatus prepareBasis()
            {
                const auto & actions = m_problem.actionIds();
                if (actions.wait != QLatin1StringView(CoreAI::ACTION_WAIT) ||
                    actions.fire != QLatin1StringView(CoreAI::ACTION_FIRE) ||
                    actions.capture != QLatin1StringView(CoreAI::ACTION_CAPTURE) ||
                    m_problem.actors().size() > GUARDED_STOCK_MAX_ACTORS ||
                    m_problem.actionTemplates().size() > GUARDED_STOCK_MAX_ACTORS)
                {
                    return MasterNativeSourceStatus::UnsupportedCallbacks;
                }
                const auto before = m_ledger.stats().spent();
                const auto expectedBefore = m_catalog.expectedPreparationWork();
                auto status = collectBasis();
                const auto expectedAfter = m_catalog.expectedPreparationWork();
                if (expectedBefore && expectedAfter && *expectedAfter >= *expectedBefore)
                {
                    m_expectedConstructionWork = *expectedAfter - *expectedBefore;
                }
                m_receiverConstructionWork = m_basis.receiverConstructionWork;
                if (status != MasterNativeSourceStatus::Ready)
                {
                    return status;
                }
                m_collectionWork = m_ledger.stats().spent() - before;
                if (!expectedBefore || !expectedAfter || *expectedAfter < *expectedBefore ||
                    m_expectedConstructionWork > m_collectionWork ||
                    m_receiverConstructionWork > m_collectionWork - m_expectedConstructionWork)
                {
                    return MasterNativeSourceStatus::IncompleteDomain;
                }
                m_commonCollectionWork =
                    m_collectionWork - m_expectedConstructionWork - m_receiverConstructionWork;
                if (m_commonCollectionWork <= 0)
                {
                    return MasterNativeSourceStatus::IncompleteDomain;
                }
                status = constructNativeModel(m_basis, m_catalog, m_ledger);
                if (status == MasterNativeSourceStatus::Ready)
                {
                    status = constructCurrentFire(m_basis, m_problem, m_ledger);
                }
                if (status != MasterNativeSourceStatus::Ready)
                {
                    return status;
                }
                const auto identity = nativeIdentityWork(m_basis, m_ledger);
                if (!identity || !m_ledger.consume(*identity, RefinementWork::MasterPreparation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                m_identityWork = *identity;
                const auto refusals = m_ledger.stats().refusals;
                m_planBounds = guardedNativePlanBounds(m_problem, m_basis.inventory, m_ledger);
                if (!m_planBounds)
                {
                    return m_ledger.stats().refusals != refusals ? MasterNativeSourceStatus::BudgetExhausted
                                                                 : MasterNativeSourceStatus::UnsupportedInventory;
                }
                if (m_basis.defaultArmyCallbacks)
                {
                    const auto start = m_ledger.stats().granted;
                    status = recheckDefaultArmies(*Interpreter::getInstance(), *m_basis.inventory.playerMaterial);
                    if (status != MasterNativeSourceStatus::Ready)
                    {
                        return status;
                    }
                    // Admit one cold replay and enforce its grant as a hard cap during finalization.
                    m_defaultArmyRecheckWork = m_ledger.stats().granted - start;
                }
                if (m_basis.commandPolicyVerified)
                {
                    // Freeze the final attempt's allowance before reservation and search.
                    m_commandAuthenticationAllowance = m_ledger.stats().total / COMMAND_AUTHENTICATION_SHARE;
                }
                m_comparisonWork = BASIS_COMPARISON_WORK;
                if (!RelativeStockDetail::checkedAdd(m_comparisonWork, m_identityWork))
                {
                    return MasterNativeSourceStatus::IncompleteDomain;
                }
                m_before = nativeBeforeIdentity(m_basis);
                m_authority = QCryptographicHash::hash(
                    NATIVE_AUTHORITY_REVISION + m_before, QCryptographicHash::Sha256);
                if (!m_fence.hold())
                {
                    return MasterNativeSourceStatus::InvalidContext;
                }
                m_ready = true;
                return MasterNativeSourceStatus::Ready;
            }

            MasterNativeSourceStatus collectBasis()
            {
                if (m_map.thread() != QThread::currentThread() || !Interpreter::isCurrentThreadOwner())
                {
                    return MasterNativeSourceStatus::InvalidContext;
                }
                auto* interpreter = Interpreter::getInstance();
                if (interpreter->thread() != QThread::currentThread() || interpreter->getInJsCall())
                {
                    return MasterNativeSourceStatus::InvalidContext;
                }
                if (!m_ledger.consume(COLLECTION_COST_WORK, RefinementWork::MasterPreparation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                m_basis.collectionCosts[static_cast<std::size_t>(MasterNativeCollectionStage::Bookkeeping)].work =
                    COLLECTION_COST_WORK;
                m_basis.collectionCosts[static_cast<std::size_t>(MasterNativeCollectionStage::Bookkeeping)].observations = 1;
                NativeCollectionCheckpoint costs(m_basis, m_catalog, m_ledger);
                const auto observation = m_catalog.observe(*interpreter, RECEIVER_GLOBAL_ALIASES);
                costs.record(MasterNativeCollectionStage::Globals);
                if (observation.status() != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(observation.status());
                }
                const auto alias = m_catalog.globalProperty(*interpreter, GLOBAL_ALIAS);
                costs.record(MasterNativeCollectionStage::Globals);
                if (alias.status != GuardedScriptPropertyStatus::Found)
                {
                    if (alias.status == GuardedScriptPropertyStatus::BudgetExhausted)
                    {
                        return MasterNativeSourceStatus::BudgetExhausted;
                    }
                    return MasterNativeSourceStatus::UnsupportedCallbacks;
                }
                m_basis.globalAlias = guardedScriptGlobalAlias(*interpreter, alias.value, m_ledger);
                costs.record(MasterNativeCollectionStage::Globals);
                if (m_basis.globalAlias.status != GuardedScriptPropertyStatus::Found)
                {
                    if (m_basis.globalAlias.status == GuardedScriptPropertyStatus::BudgetExhausted)
                    {
                        return MasterNativeSourceStatus::BudgetExhausted;
                    }
                    return MasterNativeSourceStatus::UnsupportedCallbacks;
                }
                m_basis.intrinsics = m_catalog.authenticateIntrinsics(*interpreter);
                costs.record(MasterNativeCollectionStage::Intrinsics);
                if (m_basis.intrinsics.status != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(m_basis.intrinsics.status);
                }
                m_basis.valueConstructors = guardedScriptQtValueConstructors(*interpreter, m_ledger);
                costs.record(MasterNativeCollectionStage::ValueConstructors);
                if (m_basis.valueConstructors.status != GuardedNativeReceiverStatus::Bound)
                {
                    return receiverFailure(m_basis.valueConstructors.status);
                }
                auto captured = captureGuardedNativeInventory(m_map, m_ledger);
                costs.record(MasterNativeCollectionStage::Inventory);
                if (captured.status != GuardedNativeBindingStatus::Bound || !captured.inventory)
                {
                    if (captured.status == GuardedNativeBindingStatus::BudgetExhausted)
                    {
                        return MasterNativeSourceStatus::BudgetExhausted;
                    }
                    return MasterNativeSourceStatus::UnsupportedInventory;
                }
                m_basis.inventory = std::move(*captured.inventory);
                // Refuse an unadmitted inventory before paying for material, calendar and command authentication.
                if (!admittedNativeInventory(m_basis.inventory))
                {
                    return MasterNativeSourceStatus::UnsupportedCalendar;
                }
                auto & players = *m_basis.inventory.playerMaterial;
                if (std::ranges::any_of(players, [](const auto & player) { return player.defaultArmyPlayerId.has_value(); }))
                {
                    m_basis.defaultArmyCallbacks = m_catalog.authenticateDefaultArmies(*interpreter, players);
                    if (m_basis.defaultArmyCallbacks->status != GuardedNativeBindingStatus::Bound)
                    {
                        costs.record(MasterNativeCollectionStage::Inventory);
                        return sourceFailure(m_basis.defaultArmyCallbacks->status);
                    }
                }
                const auto material = admitGuardedNativeMaterials(m_basis.inventory, m_ledger);
                costs.record(MasterNativeCollectionStage::Inventory);
                if (material != GuardedNativeBindingStatus::Bound)
                {
                    if (material == GuardedNativeBindingStatus::BudgetExhausted)
                    {
                        return MasterNativeSourceStatus::BudgetExhausted;
                    }
                    return MasterNativeSourceStatus::UnsupportedInventory;
                }
                auto calendarBasis = captureGuardedNativeCalendar(m_map, m_ledger);
                costs.record(MasterNativeCollectionStage::Calendar);
                if (calendarBasis.status != GuardedNativeBindingStatus::Bound || !calendarBasis.basis)
                {
                    if (calendarBasis.status == GuardedNativeBindingStatus::BudgetExhausted)
                    {
                        return MasterNativeSourceStatus::BudgetExhausted;
                    }
                    return MasterNativeSourceStatus::UnsupportedCalendar;
                }
                auto calendar = projectGuardedNativeCalendar(*calendarBasis.basis, m_ledger);
                costs.record(MasterNativeCollectionStage::Calendar);
                if (calendar.status != GuardedNativeBindingStatus::Bound || !calendar.calendar ||
                    calendar.calendar->basis.day != m_basis.inventory.day ||
                    calendar.calendar->basis.player != m_basis.inventory.player)
                {
                    if (calendar.status == GuardedNativeBindingStatus::BudgetExhausted)
                    {
                        return MasterNativeSourceStatus::BudgetExhausted;
                    }
                    return MasterNativeSourceStatus::UnsupportedCalendar;
                }
                m_basis.calendar = std::move(calendar.calendar);
                if (m_basis.calendar->commandRuleIndex)
                {
                    const auto command = authenticateGuardedCommandPolicy(
                        m_map, *interpreter, m_catalog, m_basis.inventory, *m_basis.calendar, m_ledger, &m_commandReceivers);
                    costs.record(MasterNativeCollectionStage::Calendar);
                    if (command != GuardedNativeBindingStatus::Bound)
                    {
                        return command == GuardedNativeBindingStatus::BudgetExhausted
                            ? MasterNativeSourceStatus::BudgetExhausted : MasterNativeSourceStatus::UnsupportedCalendar;
                    }
                    m_basis.commandPolicyVerified = true;
                }
                if (!admittedNativeCalendar(m_basis))
                {
                    return MasterNativeSourceStatus::UnsupportedCalendar;
                }
                m_basis.unitTypes = m_catalog.authenticateUnitTypes(
                    *interpreter, GameEnums::nativeInstance(), GameEnums::staticMetaObject);
                costs.record(MasterNativeCollectionStage::Enums);
                if (m_basis.unitTypes.status != GuardedNativeBindingStatus::Bound)
                {
                    if (m_basis.unitTypes.status == GuardedNativeBindingStatus::BudgetExhausted)
                    {
                        return MasterNativeSourceStatus::BudgetExhausted;
                    }
                    return MasterNativeSourceStatus::UnsupportedReceiver;
                }
                m_basis.terminalEnums = m_catalog.authenticateTerminalEnums(
                    *interpreter, GameEnums::nativeInstance(), GameEnums::staticMetaObject);
                costs.record(MasterNativeCollectionStage::Enums);
                if (m_basis.terminalEnums.status != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(m_basis.terminalEnums.status);
                }
                m_basis.stockContextEnums = m_catalog.authenticateStockContextEnums(
                    *interpreter, GameEnums::nativeInstance(), GameEnums::staticMetaObject);
                costs.record(MasterNativeCollectionStage::Enums);
                if (m_basis.stockContextEnums.status != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(m_basis.stockContextEnums.status);
                }
                const auto receivers = collectReceivers(m_basis, m_catalog, m_map, *interpreter, m_ledger, m_commandReceivers);
                costs.record(MasterNativeCollectionStage::Receivers);
                if (receivers != MasterNativeSourceStatus::Ready)
                {
                    return receivers;
                }
                auto status = collectActorModules(m_basis, m_catalog, m_map, *interpreter, m_ledger, m_commandReceivers);
                costs.record(MasterNativeCollectionStage::Actors);
                if (status == MasterNativeSourceStatus::Ready)
                {
                    status = collectCoModules(m_basis, m_catalog, *interpreter, m_ledger);
                    costs.record(MasterNativeCollectionStage::Co);
                }
                if (status == MasterNativeSourceStatus::Ready)
                {
                    status = collectStockActionModules(m_basis, m_catalog, *interpreter, m_ledger);
                    costs.record(MasterNativeCollectionStage::Actions);
                }
                if (status == MasterNativeSourceStatus::Ready)
                {
                    status = collectPropertyModules(m_basis, m_catalog, *interpreter, m_ledger);
                    costs.record(MasterNativeCollectionStage::Properties);
                }
                if (status == MasterNativeSourceStatus::Ready)
                {
                    status = collectConstructionQueries(m_basis, m_catalog, *interpreter, m_ledger);
                    costs.record(MasterNativeCollectionStage::Construction);
                }
                if (status == MasterNativeSourceStatus::Ready)
                {
                    status = collectMovementModules(m_basis, m_catalog, *interpreter, m_ledger);
                    costs.record(MasterNativeCollectionStage::Movement);
                }
                if (status == MasterNativeSourceStatus::Ready)
                {
                    status = collectContextModules(m_basis, m_catalog, *interpreter, m_ledger);
                    costs.record(MasterNativeCollectionStage::Context);
                }
                if (status == MasterNativeSourceStatus::Ready)
                {
                    status = collectTerrainModules(m_basis, m_catalog, *interpreter, m_ledger);
                    costs.record(MasterNativeCollectionStage::Terrain);
                }
                if (status == MasterNativeSourceStatus::Ready)
                {
                    status = collectPolicyModules(m_basis, m_catalog, *interpreter, m_ledger);
                    if (status == MasterNativeSourceStatus::Ready && (m_basis.commandCallbacks || m_basis.commandRankingBonuses))
                    {
                        const auto callbacks = authenticateCommandCallbacks(m_map, *interpreter, m_catalog,
                            m_basis.inventory, *m_basis.calendar, m_ledger, m_commandReceivers,
                            guardedCommandHasBuildingCallbacks(m_basis.commandCallbacks),
                            guardedCommandHasRankingCallbacks(m_basis.commandCallbacks));
                        if (callbacks != GuardedNativeBindingStatus::Bound) { status = sourceFailure(callbacks); }
                    }
                    costs.record(MasterNativeCollectionStage::Policy);
                }
                if (status == MasterNativeSourceStatus::Ready)
                {
                    status = collectCurrentFireModules(m_basis, m_catalog, *interpreter, m_problem, m_map, m_ledger);
                    costs.record(MasterNativeCollectionStage::Fire);
                }
                if (status != MasterNativeSourceStatus::Ready)
                {
                    return status;
                }
                return MasterNativeSourceStatus::Ready;
            }

            bool detailPresentation() const
            {
                return m_basis.detailPresentation.has_value();
            }

            std::optional<MasterNativeCallbackRefusal> callbackRefusal() const
            {
                if (m_basis.modules.empty())
                {
                    return {};
                }
                const auto & callbacks = m_basis.modules.back()->callbacks;
                if (callbacks.status == GuardedNativeBindingStatus::Bound)
                {
                    return {};
                }
                MasterNativeCallbackRefusal result{.module = callbacks.kind, .status = callbacks.status};
                if (!callbacks.members.empty())
                {
                    result.lastCheckedMember = callbacks.members.back().name;
                    result.lastCheckedStatus = callbacks.members.back().status;
                }
                return result;
            }

            std::uint16_t commandCallbackMask() const
            {
                return m_basis.commandCallbacks;
            }

            std::size_t boundModules() const
            {
                return std::count(m_basis.collected.begin(), m_basis.collected.end(), true);
            }

            std::int64_t collectionWork() const
            {
                return m_collectionWork;
            }

            std::int64_t expectedConstructionWork() const
            {
                return m_expectedConstructionWork;
            }

            std::int64_t receiverConstructionWork() const
            {
                return m_receiverConstructionWork;
            }

            std::int64_t commonCollectionWork() const
            {
                return m_commonCollectionWork;
            }

            std::int64_t commandAuthenticationAllowance() const
            {
                return m_commandAuthenticationAllowance;
            }

            const MasterNativeCollectionCosts & collectionCosts() const
            {
                return m_basis.collectionCosts;
            }

            MasterGuardedRootPreparation prepare(
                const MasterAssignmentState & state, const MasterAssignmentReplayResult & replay,
                RefinementLedger & ledger) override
            {
                if (&ledger != &m_ledger || !m_ready)
                {
                    return {GuardedRelationStatus::Invalid};
                }
                const auto maximum = maximumPreparationWork();
                if (!maximum)
                {
                    return {GuardedRelationStatus::Invalid};
                }
                RefinementLedger::WorkLimit limit(ledger, *maximum);
                const auto inspection = DISPATCH_ENTRY_WORK * (m_problem.actors().size() + 1);
                if (!ledger.consume(inspection,
                                    RefinementWork::MasterPreparation))
                {
                    return {GuardedRelationStatus::BudgetExhausted};
                }
                if (!supportsPlanIntent(m_problem, state))
                {
                    return {GuardedRelationStatus::Inapplicable};
                }
                if (ledger.balance() < *maximum - inspection)
                {
                    return {GuardedRelationStatus::BudgetExhausted};
                }
                const auto bound = bindGuardedNativePlan(
                    m_problem, m_basis.inventory, *m_planBounds, state, replay, ledger);
                if (bound.status != GuardedNativeBindingStatus::Bound || !bound.plan)
                {
                    return {bound.status == GuardedNativeBindingStatus::BudgetExhausted
                        ? GuardedRelationStatus::BudgetExhausted : GuardedRelationStatus::Inapplicable};
                }
                const auto binding = guardedNativePlanWork(*m_planBounds);
                if (!binding || !ledger.consume(*maximum - *binding - inspection,
                                                RefinementWork::MasterPreparation))
                {
                    return {GuardedRelationStatus::BudgetExhausted};
                }
                auto root = initialStockRoot(m_basis);
                bool valid = false;
                auto family = projectCurrentPlan(m_basis, *bound.plan, root, m_before, valid);
                if (!valid)
                {
                    return {GuardedRelationStatus::Inapplicable};
                }
                const auto consequences = consequenceIdentity(root, *bound.plan, m_before);
                // The signed native count stays raw. Only the combat-free continuation represents unlimited slots as full.
                for (auto & actor : root.actors)
                {
                    if (actor.ammo1 < 0)
                    {
                        actor.ammo1 = AVAILABLE_CONTINUATION_ROUNDS;
                    }
                    if (actor.ammo2 < 0)
                    {
                        actor.ammo2 = AVAILABLE_CONTINUATION_ROUNDS;
                    }
                }
                return {GuardedRelationStatus::Verified, std::move(root), m_authority,
                        consequences, std::move(family)};
            }

            GuardedRelationStatus authenticate(RefinementLedger & ledger) override
            {
                const auto maximum = maximumAuthenticationWork();
                if (&ledger != &m_ledger || !m_ready || !maximum)
                {
                    return GuardedRelationStatus::Invalid;
                }
                if (ledger.balance() < *maximum)
                {
                    return GuardedRelationStatus::BudgetExhausted;
                }
                RefinementLedger::WorkLimit limit(ledger, *maximum);
                if (!ledger.consume(FRESH_CONTROL_WORK, RefinementWork::MasterInstallation))
                {
                    return GuardedRelationStatus::BudgetExhausted;
                }
                if (!m_fence.intact())
                {
                    m_ready = false;
                    return GuardedRelationStatus::Invalid;
                }
                return authenticateQuiescent(ledger);
            }

            std::optional<std::int64_t> maximumPreparationWork() const override
            {
                if (!m_ready || !m_planBounds)
                {
                    return std::nullopt;
                }
                return masterNativeRootPreparationWork(*m_planBounds);
            }

            std::optional<std::int64_t> maximumAuthenticationWork() const override
            {
                if (!m_ready || m_commonCollectionWork <= 0 || m_comparisonWork <= 0)
                {
                    return std::nullopt;
                }
                auto work = freshNativeWork();
                if (!work || !RelativeStockDetail::checkedAdd(*work, m_comparisonWork) ||
                    !RelativeStockDetail::checkedAdd(*work, m_identityWork) ||
                    !RelativeStockDetail::checkedAdd(*work, FRESH_CONTROL_WORK))
                {
                    return std::nullopt;
                }
                return work;
            }

            std::size_t maximumCurrentPrefixActions() const override
            {
                return m_basis.fireActor ? m_problem.actors().size() : 0;
            }

        private:
            std::optional<std::int64_t> freshNativeWork() const
            {
                auto work = guardedNativeInventoryWork(m_basis.inventory.width, m_basis.inventory.height);
                if (!work || !RelativeStockDetail::checkedAdd(*work, guardedNativeCalendarCaptureWork()) ||
                    !RelativeStockDetail::checkedAdd(*work, guardedNativeCalendarProjectionWork()) ||
                    !RelativeStockDetail::checkedAdd(*work, RECEIVER_ENTRY_WORK * std::ssize(m_basis.inputs)) ||
                    !RelativeStockDetail::checkedAdd(*work, DISPATCH_ENTRY_WORK))
                {
                    return std::nullopt;
                }
                if (!RelativeStockDetail::checkedAdd(*work, m_defaultArmyRecheckWork) ||
                    !RelativeStockDetail::checkedAdd(*work, m_commandAuthenticationAllowance))
                {
                    return std::nullopt;
                }
                if (m_basis.detailPresentation)
                {
                    const auto detail = m_basis.detailPresentation->recheckNativeWork();
                    if (!detail || !RelativeStockDetail::checkedAdd(*work, *detail))
                    {
                        return std::nullopt;
                    }
                }
                return work;
            }

            MasterNativeSourceStatus recheckCommandDependencies(QJSEngine & engine, const GuardedNativeInventory & inventory,
                                                               const GuardedNativeCalendar & calendar, RefinementLedger & ledger)
            {
                if (m_basis.fireActor && (m_basis.commandRankingBonuses || m_basis.aw2ForestDefense || m_basis.commandCallbacks || m_basis.commandMovement || m_basis.commandWeather || m_basis.awdsLightTankInit || m_basis.awdsLightTankCost || (m_basis.unlimitedInfantryInit && !isolatedInfantryFire(m_map))))
                {
                    return MasterNativeSourceStatus::UnsupportedCallbacks;
                }
                const auto observation = m_catalog.observe(engine);
                if (observation.status() != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(observation.status());
                }
                const auto command = authenticateGuardedCommandPolicy(m_map, engine, m_catalog, inventory, calendar, ledger, &m_commandReceivers);
                if (command != GuardedNativeBindingStatus::Bound)
                {
                    return command == GuardedNativeBindingStatus::BudgetExhausted
                        ? MasterNativeSourceStatus::BudgetExhausted : MasterNativeSourceStatus::UnsupportedCalendar;
                }
                if (m_basis.commandCallbacks || m_basis.commandRankingBonuses)
                {
                    const auto callbacks = authenticateCommandCallbacks(m_map, engine, m_catalog, inventory, calendar, ledger, m_commandReceivers,
                        guardedCommandHasBuildingCallbacks(m_basis.commandCallbacks),
                        guardedCommandHasRankingCallbacks(m_basis.commandCallbacks));
                    if (callbacks != GuardedNativeBindingStatus::Bound) { return sourceFailure(callbacks); }
                }
                // A native property write can replace an action wrapper without entering JavaScript.
                // Re-read these bindings from the actual realm even while the realm fence is held.
                for (const auto kind : {GuardedNativeCodeKind::Unit, GuardedNativeCodeKind::Action, GuardedNativeCodeKind::Wait,
                                       GuardedNativeCodeKind::Capture, GuardedNativeCodeKind::NextPlayer, GuardedNativeCodeKind::Trap,
                                       GuardedNativeCodeKind::Global, GuardedNativeCodeKind::Player, GuardedNativeCodeKind::Ranking,
                                       GuardedNativeCodeKind::NoUnits, GuardedNativeCodeKind::Fire})
                {
                    if (!ledger.consume(DISPATCH_ENTRY_WORK * (m_basis.modules.size() + 1), RefinementWork::MasterPreparation))
                    {
                        return MasterNativeSourceStatus::BudgetExhausted;
                    }
                    if (kind == GuardedNativeCodeKind::Fire && !m_basis.fireActor)
                    {
                        continue;
                    }
                    const auto* module = nativeModule(m_basis, kind);
                    if (!module)
                    {
                        return MasterNativeSourceStatus::UnsupportedCallbacks;
                    }
                    const auto status = m_catalog.recheckModule(engine, module->callbacks);
                    if (status != GuardedNativeBindingStatus::Bound)
                    {
                        return sourceFailure(status);
                    }
                }
                for (const auto & module : m_basis.modules)
                {
                    if (!ledger.consume(DISPATCH_ENTRY_WORK, RefinementWork::MasterPreparation))
                    {
                        return MasterNativeSourceStatus::BudgetExhausted;
                    }
                    const auto kind = module->callbacks.kind;
                    if (kind == GuardedNativeCodeKind::Ranking)
                    {
                        if (!module->tables) { return MasterNativeSourceStatus::UnsupportedCallbacks; }
                        const auto tables = m_catalog.recheckTables(engine, *module->tables);
                        if (tables != GuardedNativeBindingStatus::Bound) { return sourceFailure(tables); }
                    }
                    if (kind == GuardedNativeCodeKind::Forest || kind == GuardedNativeCodeKind::BaseForest)
                    {
                        const auto callbacks = m_catalog.recheckModule(engine, module->callbacks);
                        if (callbacks != GuardedNativeBindingStatus::Bound) { return sourceFailure(callbacks); }
                    }
                    if (module->weatherPrefix)
                    {
                        if (kind != GuardedNativeCodeKind::Terrain) { return MasterNativeSourceStatus::UnsupportedCallbacks; }
                        const auto callbacks = m_catalog.recheckModule(engine, module->callbacks);
                        if (callbacks != GuardedNativeBindingStatus::Bound) { return sourceFailure(callbacks); }
                        const auto prefix = m_catalog.recheckStockWeatherPrefix(engine, *module->weatherPrefix);
                        if (prefix != GuardedNativeBindingStatus::Bound) { return sourceFailure(prefix); }
                    }
                    if (kind == GuardedNativeCodeKind::Town || kind == GuardedNativeCodeKind::Factory ||
                        kind == GuardedNativeCodeKind::Headquarters)
                    {
                        const auto status = m_catalog.recheckModule(engine, module->callbacks);
                        if (status != GuardedNativeBindingStatus::Bound) { return sourceFailure(status); }
                        if (module->armyTables)
                        {
                            if (!inventory.playerMaterial || !module->tables) { return MasterNativeSourceStatus::UnsupportedInventory; }
                            const auto tables = m_catalog.recheckTables(engine, *module->tables);
                            if (tables != GuardedNativeBindingStatus::Bound) { return sourceFailure(tables); }
                            const auto armies = m_catalog.recheckHeadquartersArmyTables(
                                engine, *module->armyTables, *inventory.playerMaterial);
                            if (armies != GuardedNativeBindingStatus::Bound) { return sourceFailure(armies); }
                        }
                    }
                }
                if (m_basis.commandRepairKinds || m_basis.unlimitedInfantryInit || m_basis.awdsLightTankInit || m_basis.commandWeather || m_basis.commandMovement)
                {
                    const auto receivers = authenticateCommandUnits(m_map, engine, inventory,
                        {.repair = m_basis.commandRepairKinds != 0, .infantryInit = m_basis.unlimitedInfantryInit, .lightTankInit = m_basis.awdsLightTankInit,
                         .weather = m_basis.commandWeather, .movement = m_basis.commandMovement}, ledger, &m_commandReceivers);
                    if (receivers != GuardedNativeBindingStatus::Bound)
                    {
                        return sourceFailure(receivers);
                    }
                }
                // Actor-specific native writes can replace a previously direct repair function, too.
                for (const auto & module : m_basis.modules)
                {
                    if (!ledger.consume(DISPATCH_ENTRY_WORK, RefinementWork::MasterPreparation))
                    {
                        return MasterNativeSourceStatus::BudgetExhausted;
                    }
                    if (!actorKind(module->callbacks.kind))
                    {
                        continue;
                    }
                    const auto & members = module->callbacks.members;
                    if (!ledger.consume(DISPATCH_ENTRY_WORK * (members.size() + 1), RefinementWork::MasterPreparation))
                    {
                        return MasterNativeSourceStatus::BudgetExhausted;
                    }
                    const bool alternateUnit = module->callbacks.kind == GuardedNativeCodeKind::Infantry ||
                        module->callbacks.kind == GuardedNativeCodeKind::LightTank;
                    unsigned seen = 0;
                    for (const auto & member : members)
                    {
                        if (member.name != QStringLiteral("repairUnit") && member.name != QStringLiteral("getWeatherImmune") &&
                            member.name != QStringLiteral("postAction") &&
                            !(alternateUnit && (member.name == QStringLiteral("init") || member.name == QStringLiteral("initForMods") ||
                                           member.name == QStringLiteral("getMovementType") || member.name == QStringLiteral("getBaseCost"))))
                        {
                            continue;
                        }
                        const auto status = m_catalog.recheckMember(engine, module->callbacks.kind, member);
                        if (status != GuardedNativeBindingStatus::Bound)
                        {
                            return sourceFailure(status);
                        }
                        ++seen;
                    }
                    if (seen != (alternateUnit ? 7u : 3u))
                    {
                        return MasterNativeSourceStatus::UnsupportedCallbacks;
                    }
                }
                const auto intrinsics = m_catalog.recheckIntrinsics(engine, m_basis.intrinsics);
                return intrinsics == GuardedNativeBindingStatus::Bound ? MasterNativeSourceStatus::Ready : sourceFailure(intrinsics);
            }

            MasterNativeSourceStatus recheckDefaultArmies(QJSEngine & engine, std::array<GuardedNativePlayerMaterial, 2> & players)
            {
                if (!m_ledger.consume(DEFAULT_ARMY_REPLAY_WORK, RefinementWork::MasterPreparation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                const auto mapAlias = m_catalog.globalProperty(engine, MAP_ALIAS);
                if (mapAlias.status != GuardedScriptPropertyStatus::Found)
                {
                    return mapAlias.status == GuardedScriptPropertyStatus::BudgetExhausted
                        ? MasterNativeSourceStatus::BudgetExhausted : MasterNativeSourceStatus::UnsupportedReceiver;
                }
                const auto & mapContract = m_basis.receiverMethods[static_cast<std::size_t>(NativeReceiverKind::Map)];
                const auto mapStatus = mapContract.authenticate(engine, mapAlias.value, m_map, m_ledger);
                if (mapStatus != GuardedNativeReceiverStatus::Bound)
                {
                    return receiverFailure(mapStatus);
                }
                for (std::size_t side = 0; side < players.size(); ++side)
                {
                    const auto & previous = m_basis.inventory.playerMaterial->at(side);
                    if (players[side].defaultArmyPlayerId != previous.defaultArmyPlayerId)
                    {
                        return MasterNativeSourceStatus::UnsupportedInventory;
                    }
                    if (players[side].defaultArmyPlayerId)
                    {
                        const auto status = collectReceiver(m_basis, NativeReceiverKind::Player,
                            m_map.getPlayer(static_cast<qint32>(side)), engine, m_ledger);
                        if (status != MasterNativeSourceStatus::Ready)
                        {
                            return status;
                        }
                    }
                }
                std::array<QString, 2> values;
                const auto status = m_catalog.recheckDefaultArmies(engine, *m_basis.defaultArmyCallbacks, players, values);
                if (status != GuardedNativeBindingStatus::Bound)
                {
                    return sourceFailure(status);
                }
                for (std::size_t side = 0; side < players.size(); ++side)
                {
                    if (players[side].defaultArmyPlayerId)
                    {
                        if (values[side] != m_basis.inventory.playerMaterial->at(side).army)
                        {
                            return MasterNativeSourceStatus::UnsupportedInventory;
                        }
                        players[side].army = std::move(values[side]);
                    }
                }
                return MasterNativeSourceStatus::Ready;
            }

            GuardedRelationStatus authenticateQuiescent(RefinementLedger & ledger)
            {
                const auto status = freshNativeBasis(ledger);
                if (status == MasterNativeSourceStatus::BudgetExhausted)
                {
                    return GuardedRelationStatus::BudgetExhausted;
                }
                if (status != MasterNativeSourceStatus::Ready || !m_fence.intact())
                {
                    m_ready = false;
                    return GuardedRelationStatus::Invalid;
                }
                return GuardedRelationStatus::Verified;
            }

            MasterNativeSourceStatus freshNativeBasis(RefinementLedger & ledger)
            {
                if (m_map.thread() != QThread::currentThread() ||
                    m_map.getMapWidth() != m_basis.inventory.width || m_map.getMapHeight() != m_basis.inventory.height)
                {
                    return MasterNativeSourceStatus::InvalidContext;
                }
                auto captured = captureGuardedNativeInventory(m_map, ledger);
                if (captured.status != GuardedNativeBindingStatus::Bound || !captured.inventory)
                {
                    return captured.status == GuardedNativeBindingStatus::BudgetExhausted
                        ? MasterNativeSourceStatus::BudgetExhausted : MasterNativeSourceStatus::UnsupportedInventory;
                }
                auto calendarBasis = captureGuardedNativeCalendar(m_map, ledger);
                if (calendarBasis.status != GuardedNativeBindingStatus::Bound || !calendarBasis.basis)
                {
                    return calendarBasis.status == GuardedNativeBindingStatus::BudgetExhausted
                        ? MasterNativeSourceStatus::BudgetExhausted : MasterNativeSourceStatus::UnsupportedCalendar;
                }
                auto calendar = projectGuardedNativeCalendar(*calendarBasis.basis, ledger);
                if (calendar.status != GuardedNativeBindingStatus::Bound || !calendar.calendar)
                {
                    return calendar.status == GuardedNativeBindingStatus::BudgetExhausted
                        ? MasterNativeSourceStatus::BudgetExhausted : MasterNativeSourceStatus::UnsupportedCalendar;
                }
                NativeSourceBasis fresh;
                fresh.inventory = std::move(*captured.inventory);
                if (m_basis.defaultArmyCallbacks)
                {
                    auto* realm = m_fence.realm();
                    if (realm == nullptr)
                    {
                        return MasterNativeSourceStatus::InvalidContext;
                    }
                    RefinementLedger::WorkLimit limit(ledger, m_defaultArmyRecheckWork);
                    const auto status = recheckDefaultArmies(*realm, *fresh.inventory.playerMaterial);
                    if (status != MasterNativeSourceStatus::Ready)
                    {
                        return status;
                    }
                }
                fresh.calendar = std::move(calendar.calendar);
                if (fresh.calendar->commandRuleIndex.has_value() != m_basis.commandPolicyVerified)
                {
                    return MasterNativeSourceStatus::UnsupportedCalendar;
                }
                if (m_basis.commandPolicyVerified)
                {
                    auto* realm = m_fence.realm();
                    if (realm == nullptr)
                    {
                        return MasterNativeSourceStatus::InvalidContext;
                    }
                    RefinementLedger::WorkLimit cap(ledger, m_commandAuthenticationAllowance);
                    const auto status = recheckCommandDependencies(*realm, fresh.inventory, *fresh.calendar, ledger);
                    if (status != MasterNativeSourceStatus::Ready)
                    {
                        return status;
                    }
                    fresh.commandPolicyVerified = true;
                    fresh.commandRepairKinds = m_basis.commandRepairKinds;
                    fresh.unlimitedInfantryInit = m_basis.unlimitedInfantryInit;
                    fresh.awdsInfantryCost = m_basis.awdsInfantryCost;
                    fresh.awdsLightTankInit = m_basis.awdsLightTankInit;
                    fresh.awdsLightTankCost = m_basis.awdsLightTankCost;
                    fresh.commandWeather = m_basis.commandWeather;
                    fresh.commandMovement = m_basis.commandMovement;
                    fresh.aw2ForestDefense = m_basis.aw2ForestDefense;
                    fresh.commandRankingBonuses = m_basis.commandRankingBonuses;
                    fresh.commandCallbacks = m_basis.commandCallbacks;
                }
                for (std::size_t side = 0; side < fresh.inputs.size(); ++side)
                {
                    if (!ledger.consume(RECEIVER_ENTRY_WORK, RefinementWork::MasterInstallation))
                    {
                        return MasterNativeSourceStatus::BudgetExhausted;
                    }
                    auto* player = m_map.getPlayer(static_cast<std::int32_t>(side));
                    auto* input = player == nullptr ? nullptr : player->getBaseGameInput();
                    if (input == nullptr || player->getMap() != &m_map || input->getMap() != &m_map ||
                        input->getPlayer() != player)
                    {
                        return MasterNativeSourceStatus::UnsupportedReceiver;
                    }
                    fresh.inputs[side] = static_cast<std::int32_t>(input->getAiType());
                }
                if (!ledger.consume(DISPATCH_ENTRY_WORK, RefinementWork::MasterInstallation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                if (m_basis.battlePresentation)
                {
                    fresh.battlePresentation =
                        static_cast<std::int32_t>(Settings::getInstance()->getBattleAnimationType());
                }
                if (m_basis.detailPresentation)
                {
                    auto* realm = m_fence.realm();
                    if (realm == nullptr)
                    {
                        return MasterNativeSourceStatus::InvalidContext;
                    }
                    const auto detail = m_basis.detailPresentation->recheckNative(m_map, *realm, ledger);
                    if (detail != GuardedNativeBindingStatus::Bound)
                    {
                        return detail == GuardedNativeBindingStatus::BudgetExhausted
                            ? MasterNativeSourceStatus::BudgetExhausted : MasterNativeSourceStatus::UnsupportedCallbacks;
                    }
                    fresh.detailPresentation = m_basis.detailPresentation;
                }
                if (!ledger.consume(m_comparisonWork, RefinementWork::MasterInstallation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                if (fresh.inventory != m_basis.inventory || fresh.calendar != m_basis.calendar ||
                    fresh.inputs != m_basis.inputs || fresh.battlePresentation != m_basis.battlePresentation)
                {
                    return MasterNativeSourceStatus::UnsupportedInventory;
                }
                if (!ledger.consume(m_identityWork, RefinementWork::MasterInstallation))
                {
                    return MasterNativeSourceStatus::BudgetExhausted;
                }
                return nativeBeforeIdentity(fresh) == m_before
                    ? MasterNativeSourceStatus::Ready : MasterNativeSourceStatus::UnsupportedInventory;
            }

            GameMap & m_map;
            const MasterAssignmentProblem & m_problem;
            RefinementLedger & m_ledger;
            GuardedNativeCodeCatalog m_catalog;
            GuardedNativeMethodCache m_commandReceivers;
            NativeSourceBasis m_basis;
            QByteArray m_before;
            QByteArray m_authority;
            std::int64_t m_collectionWork{0};
            std::int64_t m_expectedConstructionWork{0};
            std::int64_t m_receiverConstructionWork{0};
            std::int64_t m_commonCollectionWork{0};
            std::int64_t m_defaultArmyRecheckWork{0};
            std::int64_t m_commandAuthenticationAllowance{0};
            std::int64_t m_identityWork{0};
            std::int64_t m_comparisonWork{0};
            std::optional<GuardedNativePlanBounds> m_planBounds;
            LiveRealmFence m_fence;
            bool m_ready{false};
        };
    }

    MasterNativeSourcePreparation prepareMasterNativeSource(
        GameMap & map, const MasterAssignmentProblem & problem, RefinementLedger & ledger)
    {
        MasterNativeSourcePreparation result;
        const auto before = ledger.stats().spent();
        if (!ledger.consume(SOURCE_CONTROL_WORK, RefinementWork::MasterPreparation))
        {
            result.status = MasterNativeSourceStatus::BudgetExhausted;
            return result;
        }
        if (map.thread() != QThread::currentThread() || !Interpreter::isCurrentThreadOwner())
        {
            result.status = MasterNativeSourceStatus::InvalidContext;
            result.work = ledger.stats().spent() - before;
            return result;
        }
        auto source = std::make_unique<NativeRootSource>(map, problem, ledger);
        result.status = source->prepareBasis();
        result.work = ledger.stats().spent() - before;
        result.boundModules = source->boundModules();
        result.commandCallbacks = source->commandCallbackMask();
        result.detailPresentation = source->detailPresentation();
        result.collectionWork = source->collectionWork();
        result.expectedConstructionWork = source->expectedConstructionWork();
        result.receiverConstructionWork = source->receiverConstructionWork();
        result.commonCollectionWork = source->commonCollectionWork();
        result.preparationMaximumWork = source->maximumPreparationWork();
        result.authenticationMaximumWork = source->maximumAuthenticationWork();
        result.commandAuthenticationAllowance = source->commandAuthenticationAllowance();
        result.collectionCosts = source->collectionCosts();
        result.callbackRefusal = source->callbackRefusal();
        result.source = std::move(source);
        return result;
    }

    std::optional<std::int64_t> masterNativeRootPreparationWork(const GuardedNativePlanBounds & bounds)
    {
        auto work = rootProjectionWork(bounds);
        if (work)
        {
            // Bounds carry the problem's actor count, so this is the prepare() dispatch inspection.
            *work += DISPATCH_ENTRY_WORK * static_cast<std::int64_t>(bounds.actors + 1);
        }
        return work;
    }

    QString masterNativeSourceStatusName(MasterNativeSourceStatus status)
    {
        switch (status)
        {
            case MasterNativeSourceStatus::Unprepared:
                return QStringLiteral("UNPREPARED");
            case MasterNativeSourceStatus::Ready:
                return QStringLiteral("READY");
            case MasterNativeSourceStatus::InvalidContext:
                return QStringLiteral("INVALID_CONTEXT");
            case MasterNativeSourceStatus::UnsupportedInventory:
                return QStringLiteral("UNSUPPORTED_INVENTORY");
            case MasterNativeSourceStatus::UnsupportedCallbacks:
                return QStringLiteral("UNSUPPORTED_CALLBACKS");
            case MasterNativeSourceStatus::UnsupportedCalendar:
                return QStringLiteral("UNSUPPORTED_CALENDAR");
            case MasterNativeSourceStatus::UnsupportedReceiver:
                return QStringLiteral("UNSUPPORTED_RECEIVER");
            case MasterNativeSourceStatus::IncompleteDomain:
                return QStringLiteral("INCOMPLETE_DOMAIN");
            case MasterNativeSourceStatus::BudgetExhausted:
                return QStringLiteral("BUDGET_EXHAUSTED");
        }
        return QStringLiteral("INVALID");
    }

    QString masterNativeCollectionStageName(MasterNativeCollectionStage stage)
    {
        switch (stage)
        {
            case MasterNativeCollectionStage::Bookkeeping:
                return QStringLiteral("BOOKKEEPING");
            case MasterNativeCollectionStage::Globals:
                return QStringLiteral("GLOBALS");
            case MasterNativeCollectionStage::Intrinsics:
                return QStringLiteral("INTRINSICS");
            case MasterNativeCollectionStage::ValueConstructors:
                return QStringLiteral("VALUE_CONSTRUCTORS");
            case MasterNativeCollectionStage::Inventory:
                return QStringLiteral("INVENTORY");
            case MasterNativeCollectionStage::Calendar:
                return QStringLiteral("CALENDAR");
            case MasterNativeCollectionStage::Enums:
                return QStringLiteral("ENUMS");
            case MasterNativeCollectionStage::Receivers:
                return QStringLiteral("RECEIVERS");
            case MasterNativeCollectionStage::Actors:
                return QStringLiteral("ACTORS");
            case MasterNativeCollectionStage::Co:
                return QStringLiteral("CO");
            case MasterNativeCollectionStage::Actions:
                return QStringLiteral("ACTIONS");
            case MasterNativeCollectionStage::Properties:
                return QStringLiteral("PROPERTIES");
            case MasterNativeCollectionStage::Construction:
                return QStringLiteral("CONSTRUCTION");
            case MasterNativeCollectionStage::Movement:
                return QStringLiteral("MOVEMENT");
            case MasterNativeCollectionStage::Context:
                return QStringLiteral("CONTEXT");
            case MasterNativeCollectionStage::Terrain:
                return QStringLiteral("TERRAIN");
            case MasterNativeCollectionStage::Policy:
                return QStringLiteral("POLICY");
            case MasterNativeCollectionStage::Fire:
                return QStringLiteral("FIRE");
            case MasterNativeCollectionStage::Count:
                break;
        }
        return QStringLiteral("INVALID");
    }
}
