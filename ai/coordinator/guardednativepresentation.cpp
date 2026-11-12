#include "ai/coordinator/guardednativepresentation.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

#include <QCryptographicHash>
#include <QDataStream>
#include <QPointer>
#include <QThread>

#include "ai/coordinator/guardednativecode.h"
#include "ai/coordinator/relativestockcomparison.h"
#include "ai/coordinator/turnplan.h"
#include "3rd_party/oxygine-framework/oxygine/actor/Stage.h"
#include "coreengine/mainapp.h"
#include "coreengine/scriptvariables.h"
#include "coreengine/settings.h"
#include "game/gameanimation/battleanimation.h"
#include "game/gameanimation/battleanimationsprite.h"
#include "game/gameanimation/gameanimationfactory.h"
#include "game/gamemap.h"
#include "game/terrain.h"
#include "menue/gamemenue.h"

namespace Coordinator
{
    namespace
    {
        constexpr std::int64_t PRESENTATION_CONTROL_WORK = 4096;
        constexpr std::int64_t PRESENTATION_IDENTITY_WORK = 131072;
        constexpr std::int64_t PRESENTATION_MODULE_WORK = 2048;
        constexpr std::int64_t VARIABLE_HEADER_WORK = 256;
        constexpr std::int64_t VARIABLE_COPY_WORK = 256 + 8 * GUARDED_NATIVE_MAX_ID_LENGTH +
            8 * GUARDED_NATIVE_MAX_RULE_BYTES;
        constexpr std::int64_t ENDPOINT_INSPECTION_WORK = 64 * (GUARDED_STOCK_MAX_TILES + GUARDED_STOCK_MAX_ACTORS);
        constexpr std::size_t MAX_PRESENTATION_MODULES = 11;
        const QString SETTINGS_ALIAS = QString::fromLatin1(GUARDED_NATIVE_SETTINGS_ALIAS);
        const QByteArray PRESENTATION_REVISION{"guarded-detail-none-constructor-v1"};

        constexpr std::array SPRITE_METHODS{
            GuardedNativeMethodRequirement{"getMaxUnitCount", 0},
            GuardedNativeMethodRequirement{"getUnitCount", 1},
            GuardedNativeMethodRequirement{"existResAnim", 1},
            GuardedNativeMethodRequirement{"loadSpriteV2", 4},
            GuardedNativeMethodRequirement{"loadSpriteV2", 5},
            GuardedNativeMethodRequirement{"loadSpriteV2", 7},
            GuardedNativeMethodRequirement{"loadMovingSprite", 9},
            GuardedNativeMethodRequirement{"loadMovingSpriteV2", 9},
            GuardedNativeMethodRequirement{"loadSound", 3},
            GuardedNativeMethodRequirement{"loadCoMini", 9},
        };
        constexpr std::array SETTINGS_METHODS{GuardedNativeMethodRequirement{"getUseCoMinis", 0}};
        constexpr std::array TERRAIN_METHODS{GuardedNativeMethodRequirement{"getAnimationVariables", 0}};
        constexpr std::array MAP_METHODS{GuardedNativeMethodRequirement{"onMap", 2}};
        constexpr std::array VARIABLES_METHODS{
            GuardedNativeMethodRequirement{"getVariable", 1},
            GuardedNativeMethodRequirement{"createVariable", 1},
        };
        constexpr std::array VARIABLE_METHODS{
            GuardedNativeMethodRequirement{"readDataInt32", 0},
            GuardedNativeMethodRequirement{"writeDataInt32", 1},
        };
        enum class Receiver : std::int8_t
        {
            Sprite,
            Settings,
            Terrain,
            Map,
            Variables,
            Variable,
            Count,
        };
        struct Schema
        {
            Receiver kind;
            const QMetaObject* metadata;
            std::span<const GuardedNativeMethodRequirement> methods;
        };
        constexpr std::array SCHEMAS{
            Schema{Receiver::Sprite, &BattleAnimationSprite::staticMetaObject, SPRITE_METHODS},
            Schema{Receiver::Settings, &Settings::staticMetaObject, SETTINGS_METHODS},
            Schema{Receiver::Terrain, &Terrain::staticMetaObject, TERRAIN_METHODS},
            Schema{Receiver::Map, &GameMap::staticMetaObject, MAP_METHODS},
            Schema{Receiver::Variables, &ScriptVariables::staticMetaObject, VARIABLES_METHODS},
            Schema{Receiver::Variable, &ScriptVariable::staticMetaObject, VARIABLE_METHODS},
        };
        static_assert(SCHEMAS.size() == static_cast<std::size_t>(Receiver::Count));
        static_assert(std::is_final_v<BattleAnimation> && std::is_final_v<BattleAnimationSprite>);

        struct Module
        {
            GuardedNativeModuleAgreement callbacks;
            std::optional<GuardedNativeTableAgreement> tables;
        };
        struct Endpoint
        {
            std::int32_t tile{-1};
            GuardedNativeAnimationTerrain kind{GuardedNativeAnimationTerrain::Plains};
            std::vector<GuardedNativeVariable> variables;
        };
        struct Facts
        {
            std::int32_t type{-1};
            std::int32_t mode{-1};
            bool coMinis{false};
            float animationSpeed{0};
            float battleSpeed{0};
            float stageWidth{0};
            float stageHeight{0};
            bool movement{false};
            bool overworld{false};
            bool synchronous{false};
            QPointer<GameMenue> menu;
            ActionPerformer* performer{nullptr};
            std::array<Endpoint, 2> endpoints;
        };

        GuardedNativeBindingStatus receiverStatus(GuardedNativeReceiverStatus status)
        {
            if (status == GuardedNativeReceiverStatus::Bound)
            {
                return GuardedNativeBindingStatus::Bound;
            }
            return status == GuardedNativeReceiverStatus::BudgetExhausted
                ? GuardedNativeBindingStatus::BudgetExhausted : GuardedNativeBindingStatus::Unsupported;
        }

        std::optional<GuardedNativeAnimationTerrain> animationTerrain(GuardedNativeCodeKind kind)
        {
            switch (kind)
            {
                case GuardedNativeCodeKind::Plains: return GuardedNativeAnimationTerrain::Plains;
                case GuardedNativeCodeKind::Forest: return GuardedNativeAnimationTerrain::Forest;
                case GuardedNativeCodeKind::Mountain: return GuardedNativeAnimationTerrain::Mountain;
                case GuardedNativeCodeKind::Street: return GuardedNativeAnimationTerrain::Street;
                case GuardedNativeCodeKind::Town:
                case GuardedNativeCodeKind::Factory:
                case GuardedNativeCodeKind::Headquarters: return GuardedNativeAnimationTerrain::Property;
                default: return std::nullopt;
            }
        }

        QByteArray presentationIdentity(const Facts & facts)
        {
            QByteArray bytes;
            QDataStream stream(&bytes, QIODevice::WriteOnly);
            stream.setVersion(QDataStream::Qt_6_8);
            stream << PRESENTATION_REVISION << facts.type << facts.mode << facts.coMinis <<
                facts.animationSpeed << facts.battleSpeed << facts.movement << facts.overworld << facts.synchronous <<
                facts.stageWidth << facts.stageHeight;
            for (const auto & endpoint : facts.endpoints)
            {
                stream << endpoint.tile << qint32(endpoint.kind) << quint32(endpoint.variables.size());
                for (const auto & variable : endpoint.variables)
                {
                    stream << variable.id << variable.data << variable.modified;
                }
            }
            return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        }
    }

    struct GuardedNativeDetailPresentation::Data
    {
        std::array<GuardedNativeMethodContract, SCHEMAS.size()> contracts;
        std::vector<std::shared_ptr<const Module>> modules;
        Facts facts;
        QByteArray identity;

        const GuardedNativeMethodContract & contract(Receiver kind) const
        {
            return contracts[static_cast<std::size_t>(kind)];
        }

        GuardedNativeBindingStatus collectModule(
            GuardedNativeCodeKind kind, QJSEngine & engine, GuardedNativeCodeCatalog & catalog,
            RefinementLedger & ledger)
        {
            if (!ledger.consume(PRESENTATION_MODULE_WORK, RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            if (std::any_of(modules.begin(), modules.end(),
                [kind](const auto & entry) { return entry->callbacks.kind == kind; }))
            {
                return GuardedNativeBindingStatus::Bound;
            }
            if (modules.size() >= MAX_PRESENTATION_MODULES)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            Module module;
            module.callbacks = catalog.authenticateDetailNoneCallbacks(engine, kind);
            if (module.callbacks.status != GuardedNativeBindingStatus::Bound)
            {
                return module.callbacks.status;
            }
            if (kind == GuardedNativeCodeKind::BattleAnimation ||
                kind == GuardedNativeCodeKind::InfantryBattleAnimation ||
                kind == GuardedNativeCodeKind::SniperBattleAnimation)
            {
                module.tables = catalog.authenticateDetailNoneTables(engine, kind);
                if (module.tables->status != GuardedNativeBindingStatus::Bound)
                {
                    return module.tables->status;
                }
            }
            modules.push_back(std::make_shared<Module>(std::move(module)));
            return GuardedNativeBindingStatus::Bound;
        }

        GuardedNativeBindingStatus collectVariables(
            Endpoint & endpoint, Terrain & terrain, QJSEngine & engine, RefinementLedger & ledger)
        {
            auto status = receiverStatus(contract(Receiver::Terrain).authenticateReturned(engine, terrain, ledger));
            if (status != GuardedNativeBindingStatus::Bound)
            {
                return status;
            }
            auto* variables = terrain.getAnimationVariables();
            if (variables == nullptr)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            status = receiverStatus(contract(Receiver::Variables).authenticateReturned(engine, *variables, ledger));
            if (status != GuardedNativeBindingStatus::Bound)
            {
                return status;
            }
            if (!ledger.consume(VARIABLE_HEADER_WORK, RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            const auto & stored = variables->getStoredVariables();
            if (stored.size() > GUARDED_NATIVE_MAX_ANIMATION_VARIABLES)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            if (!ledger.consume(VARIABLE_COPY_WORK * stored.size(), RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            endpoint.variables.reserve(stored.size());
            for (const auto & variable : stored)
            {
                if (!variable || variable->getStoredId().size() > GUARDED_NATIVE_MAX_ID_LENGTH ||
                    variable->getStoredData().size() > GUARDED_NATIVE_MAX_RULE_BYTES)
                {
                    return GuardedNativeBindingStatus::Unsupported;
                }
                status = receiverStatus(contract(Receiver::Variable).authenticateReturned(engine, *variable, ledger));
                if (status != GuardedNativeBindingStatus::Bound)
                {
                    return status;
                }
                endpoint.variables.push_back(
                    {variable->getStoredId(), variable->getStoredData(), variable->getModified()});
            }
            return admitGuardedNativeAnimationVariables(endpoint.kind, endpoint.variables, ledger);
        }
    };

    namespace
    {
        bool readLiveFacts(GameMap & map, Facts & facts)
        {
            auto* settings = Settings::getInstance();
            facts.type = settings->getBattleAnimationType();
            facts.mode = settings->getBattleAnimationMode();
            facts.coMinis = settings->getUseCoMinis();
            facts.animationSpeed = settings->getAnimationSpeed();
            facts.battleSpeed = settings->getBattleAnimationSpeed();
            facts.movement = settings->getMovementAnimations();
            facts.overworld = settings->getOverworldAnimations();
            facts.synchronous = settings->getSyncAnimations();
            auto* app = Mainapp::getInstance();
            app->pauseRendering();
            auto stage = oxygine::Stage::getStage();
            if (stage)
            {
                facts.stageWidth = stage->getWidth();
                facts.stageHeight = stage->getHeight();
            }
            app->continueRendering();
            auto* menu = qobject_cast<GameMenue*>(map.getMenu());
            if (facts.type != GameEnums::BattleAnimationType_Detail ||
                facts.mode != GameEnums::BattleAnimationMode_None ||
                !std::isfinite(facts.animationSpeed) || facts.animationSpeed < 1 ||
                !std::isfinite(facts.battleSpeed) || facts.battleSpeed < 1 ||
                !std::isfinite(facts.stageWidth) || facts.stageWidth <= 0 ||
                !std::isfinite(facts.stageHeight) || facts.stageHeight <= 0 ||
                menu == nullptr || menu->metaObject() != &GameMenue::staticMetaObject || menu->getMap() != &map ||
                menu->thread() != QThread::currentThread() || !menu->hasAnimationCompletionConnection() ||
                !menu->getGameStarted() || GameAnimationFactory::getAnimationCount() != 0)
            {
                return false;
            }
            facts.menu = menu;
            facts.performer = menu->getActionPerformer();
            return facts.performer != nullptr && !facts.performer->getActionRunning();
        }
    }

    GuardedNativeBindingStatus GuardedNativeDetailPresentation::capture(
        GameMap & map, const PlannedAction & fire, const GuardedNativeInventory & inventory,
        QJSEngine & engine, GuardedNativeCodeCatalog & catalog, RefinementLedger & ledger)
    {
        m_data.reset();
        m_receiverConstructionWork = 0;
        if (!ledger.consume(PRESENTATION_CONTROL_WORK + PRESENTATION_IDENTITY_WORK,
                            RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        auto data = std::make_shared<Data>();
        if (!readLiveFacts(map, data->facts) ||
            fire.path.empty() || inventory.width <= 0 ||
            inventory.width > GUARDED_NATIVE_MAX_WIDTH || inventory.height <= 0 ||
            inventory.height > GUARDED_NATIVE_MAX_HEIGHT || !inventory.terrains ||
            inventory.terrains->size() != static_cast<std::size_t>(inventory.width * inventory.height) ||
            inventory.properties.size() > GUARDED_STOCK_MAX_TILES)
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        data->modules.reserve(MAX_PRESENTATION_MODULES);
        for (const auto & schema : SCHEMAS)
        {
            const auto index = static_cast<std::size_t>(schema.kind);
            const auto before = ledger.stats().spent();
            data->contracts[index] = GuardedNativeMethodContract::prepare(*schema.metadata, schema.methods, ledger);
            if (!RelativeStockDetail::checkedAdd(m_receiverConstructionWork, ledger.stats().spent() - before))
            {
                return GuardedNativeBindingStatus::Invalid;
            }
            if (data->contracts[index].status() != GuardedNativeReceiverStatus::Bound)
            {
                return receiverStatus(data->contracts[index].status());
            }
        }
        const auto alias = catalog.globalProperty(engine, SETTINGS_ALIAS);
        if (alias.status != GuardedScriptPropertyStatus::Found)
        {
            return alias.status == GuardedScriptPropertyStatus::BudgetExhausted
                ? GuardedNativeBindingStatus::BudgetExhausted : GuardedNativeBindingStatus::Unsupported;
        }
        auto status = receiverStatus(data->contract(Receiver::Settings).authenticate(
            engine, alias.value, *Settings::getInstance(), ledger));
        if (status == GuardedNativeBindingStatus::Bound)
        {
            status = receiverStatus(data->contract(Receiver::Map).authenticateReturned(engine, map, ledger));
        }
        if (status != GuardedNativeBindingStatus::Bound)
        {
            return status;
        }
        for (const auto kind : {GuardedNativeCodeKind::BattleAnimation, GuardedNativeCodeKind::InfantryBattleAnimation,
                                GuardedNativeCodeKind::SniperBattleAnimation, GuardedNativeCodeKind::Infantry,
                                GuardedNativeCodeKind::Sniper, GuardedNativeCodeKind::Terrain})
        {
            status = data->collectModule(kind, engine, catalog, ledger);
            if (status != GuardedNativeBindingStatus::Bound)
            {
                return status;
            }
        }
        const std::array positions{fire.path.back(), fire.target};
        for (std::size_t role = 0; role < positions.size(); ++role)
        {
            if (!ledger.consume(ENDPOINT_INSPECTION_WORK, RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            const auto position = positions[role];
            if (position.x < 0 || position.y < 0 || position.x >= inventory.width || position.y >= inventory.height)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            auto & endpoint = data->facts.endpoints[role];
            endpoint.tile = position.x + position.y * inventory.width;
            const auto & layers = inventory.terrains->at(endpoint.tile).layers;
            if (layers.empty())
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            const auto top = catalog.kindForGlobal(layers.front().type);
            if (top.status != GuardedNativeBindingStatus::Bound || !animationTerrain(top.kind))
            {
                return top.status == GuardedNativeBindingStatus::BudgetExhausted
                    ? top.status : GuardedNativeBindingStatus::Unsupported;
            }
            status = data->collectModule(top.kind, engine, catalog, ledger);
            if (status == GuardedNativeBindingStatus::Bound && top.kind == GuardedNativeCodeKind::Street)
            {
                status = data->collectModule(GuardedNativeCodeKind::BaseStreet, engine, catalog, ledger);
            }
            if (status != GuardedNativeBindingStatus::Bound)
            {
                return status;
            }
            endpoint.kind = *animationTerrain(top.kind);
            const auto property = std::find_if(inventory.properties.begin(), inventory.properties.end(),
                [&](const auto & entry) { return entry.tile == endpoint.tile; });
            if (property != inventory.properties.end())
            {
                const auto kind = catalog.kindForGlobal(property->type);
                if (kind.status != GuardedNativeBindingStatus::Bound || !animationTerrain(kind.kind))
                {
                    return kind.status == GuardedNativeBindingStatus::BudgetExhausted
                        ? kind.status : GuardedNativeBindingStatus::Unsupported;
                }
                endpoint.kind = *animationTerrain(kind.kind);
                status = data->collectModule(kind.kind, engine, catalog, ledger);
                if (status != GuardedNativeBindingStatus::Bound)
                {
                    return status;
                }
            }
            auto* terrain = map.getTerrain(position.x, position.y);
            if (terrain == nullptr)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            status = data->collectVariables(endpoint, *terrain, engine, ledger);
            if (status != GuardedNativeBindingStatus::Bound)
            {
                return status;
            }
        }
        data->identity = presentationIdentity(data->facts);
        m_data = std::move(data);
        return GuardedNativeBindingStatus::Bound;
    }

    GuardedNativeBindingStatus GuardedNativeDetailPresentation::recheckNative(
        GameMap & map, QJSEngine & engine, RefinementLedger & ledger) const
    {
        if (!m_data)
        {
            return GuardedNativeBindingStatus::Invalid;
        }
        if (!ledger.consume(PRESENTATION_CONTROL_WORK + PRESENTATION_IDENTITY_WORK,
                            RefinementWork::MasterInstallation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        Data fresh;
        fresh.contracts = m_data->contracts;
        if (!readLiveFacts(map, fresh.facts) || fresh.facts.menu != m_data->facts.menu ||
            fresh.facts.performer != m_data->facts.performer)
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        const auto width = map.getMapWidth();
        for (std::size_t role = 0; role < fresh.facts.endpoints.size(); ++role)
        {
            if (!ledger.consume(ENDPOINT_INSPECTION_WORK, RefinementWork::MasterInstallation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            const auto & retained = m_data->facts.endpoints[role];
            auto & endpoint = fresh.facts.endpoints[role];
            endpoint.tile = retained.tile;
            endpoint.kind = retained.kind;
            auto* terrain = width <= 0 || retained.tile < 0 ? nullptr
                : map.getTerrain(retained.tile % width, retained.tile / width);
            if (terrain == nullptr)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            const auto status = fresh.collectVariables(endpoint, *terrain, engine, ledger);
            if (status != GuardedNativeBindingStatus::Bound)
            {
                return status;
            }
        }
        return presentationIdentity(fresh.facts) == m_data->identity
            ? GuardedNativeBindingStatus::Bound : GuardedNativeBindingStatus::Unsupported;
    }

    std::optional<std::int64_t> GuardedNativeDetailPresentation::recheckNativeWork() const
    {
        if (!m_data)
        {
            return std::nullopt;
        }
        const auto terrain = m_data->contract(Receiver::Terrain).returnedWork();
        const auto variables = m_data->contract(Receiver::Variables).returnedWork();
        const auto variable = m_data->contract(Receiver::Variable).returnedWork();
        if (!terrain || !variables || !variable)
        {
            return std::nullopt;
        }
        constexpr auto count = static_cast<std::int64_t>(GUARDED_NATIVE_MAX_ANIMATION_VARIABLES);
        const auto endpoint = ENDPOINT_INSPECTION_WORK + *terrain + *variables + VARIABLE_HEADER_WORK +
            count * (VARIABLE_COPY_WORK + *variable) +
            guardedNativeAnimationAdmissionWork(GUARDED_NATIVE_MAX_ANIMATION_VARIABLES);
        return PRESENTATION_CONTROL_WORK + PRESENTATION_IDENTITY_WORK +
            endpoint * std::ssize(m_data->facts.endpoints);
    }

    const QByteArray & GuardedNativeDetailPresentation::identity() const
    {
        static const QByteArray empty;
        return m_data ? m_data->identity : empty;
    }

    std::int64_t GuardedNativeDetailPresentation::receiverConstructionWork() const
    {
        return m_receiverConstructionWork;
    }
}
