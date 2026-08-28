#include "coreengine/interpreter.h"

#include "gameinput/humanplayerinput.h"
#include "gameinput/basegameinputif.h"
#include "gameinput/moveplannerinput.h"

#include "ai/veryeasyai.h"
#include "ai/proxyai.h"
#include "ai/normalai.h"
#include "ai/coordinatedai.h"
#include "ai/heavyai/heavyai.h"
#include "ai/dummyai.h"

#include "game/gamemap.h"

#include "resource_management/gamemanager.h"

BaseGameInputIF::BaseGameInputIF(GameMap* pMap, GameEnums::AiTypes aiType)
    : m_AiType(aiType),
    m_pMap(pMap)
{
#ifdef GRAPHICSUPPORT
    setObjectName("BaseGameInputIF");
#endif
    Interpreter::setCppOwnerShip(this);
}

void BaseGameInputIF::onGameStart()
{

}

void BaseGameInputIF::setPlayer(Player* pPlayer)
{
    m_pPlayer = pPlayer;
}


bool BaseGameInputIF::getEnableNeutralTerrainAttack() const
{
    return m_enableNeutralTerrainAttack;
}

void BaseGameInputIF::setEnableNeutralTerrainAttack(bool value)
{
    m_enableNeutralTerrainAttack = value;
}

void BaseGameInputIF::serializeInterface(QDataStream& pStream, BaseGameInputIF* input, GameEnums::AiTypes aiType)
{
    CONSOLE_PRINT("Serializing ai " + QString::number(aiType), GameConsole::eDEBUG);
    if (input == nullptr)
    {
        if (aiType == GameEnums::AiTypes_Open ||
            aiType == GameEnums::AiTypes_Closed)
        {
            pStream << static_cast<qint32>(aiType);
        }
        else
        {
            pStream << GameEnums::AiTypes_Open;
        }
    }
    else
    {
        auto type = input->getAiType();
        pStream << static_cast<qint32>(type);
        input->serializeObject(pStream);
    }
}

spBaseGameInputIF BaseGameInputIF::deserializeInterface(GameMap* pMap, QDataStream& pStream, qint32 version)
{
    CONSOLE_PRINT("reading ai", GameConsole::eDEBUG);
    qint32 typeInt = 0;
    pStream >> typeInt;
    const auto type = static_cast<GameEnums::AiTypes>(typeInt);
    if (pStream.status() != QDataStream::Ok ||
        !GameManager::getInstance()->isKnownAiType(type))
    {
        pStream.setStatus(QDataStream::ReadCorruptData);
        return {};
    }

    spBaseGameInputIF ret = createAi(pMap, type);
    if (version > 7 && ret.get() != nullptr)
    {
        ret->deserializeObject(pStream);
    }
    return ret;
}

spBaseGameInputIF BaseGameInputIF::createAi(GameMap* pMap, GameEnums::AiTypes type)
{
    CONSOLE_PRINT("Creating AI " + QString::number(type), GameConsole::eDEBUG);
    spBaseGameInputIF ret;
    switch (type)
    {
    case GameEnums::AiTypes_Human:
    {
        ret = MemoryManagement::create<HumanPlayerInput>(pMap);
        break;
    }
    case GameEnums::AiTypes_VeryEasy:
    {
        if (Settings::getInstance()->getSpawnAiProcess() &&
            !Settings::getInstance()->getAiSlave())
        {
            ret = MemoryManagement::create<DummyAi>(pMap, type);
        }
        else
        {
            ret = MemoryManagement::create<VeryEasyAI>(pMap);
        }
        break;
    }
    case GameEnums::AiTypes_Normal:
    {
        if (Settings::getInstance()->getSpawnAiProcess() &&
            !Settings::getInstance()->getAiSlave())
        {
            ret = MemoryManagement::create<DummyAi>(pMap, type);
        }
        else
        {
            ret = MemoryManagement::create<NormalAi>(
                pMap, NormalAi::DEFAULT_INI_FILE, type, NormalAi::DEFAULT_JS_NAME);
        }
        break;
    }
    case GameEnums::AiTypes_NormalOffensive:
    {
        if (Settings::getInstance()->getSpawnAiProcess() &&
            !Settings::getInstance()->getAiSlave())
        {
            ret = MemoryManagement::create<DummyAi>(pMap, type);
        }
        else
        {
            ret = MemoryManagement::create<NormalAi>(pMap, "normalOffensive.ini", type, "NORMALAIOFFENSIVE");
        }
        break;
    }
    case GameEnums::AiTypes_NormalDefensive:
    {
        if (Settings::getInstance()->getSpawnAiProcess() &&
            !Settings::getInstance()->getAiSlave())
        {
            ret = MemoryManagement::create<DummyAi>(pMap, type);
        }
        else
        {
            ret = MemoryManagement::create<NormalAi>(pMap, "normalDefensive.ini", type, "NORMALAIDEFENSIVE");
        }
        break;
    }
    case GameEnums::AiTypes_Coordinated:
    {
        if (Settings::getInstance()->getSpawnAiProcess() &&
            !Settings::getInstance()->getAiSlave())
        {
            ret = MemoryManagement::create<DummyAi>(pMap, type);
        }
        else
        {
            ret = MemoryManagement::create<CoordinatedAi>(pMap);
        }
        break;
    }
    case GameEnums::AiTypes_ProxyAi:
    {
        ret = MemoryManagement::create<ProxyAi>(pMap);
        break;
    }
    case GameEnums::AiTypes_MovePlanner:
    {
        ret = MemoryManagement::create<MoveplannerInput>(pMap);
        break;
    }
    case GameEnums::AiTypes_DummyAi:
    {
        ret = MemoryManagement::create<DummyAi>(pMap, type);
        break;
    }
    case GameEnums::AiTypes_Open:
    case GameEnums::AiTypes_Closed:
    {
        ret.reset();
        break;
    }
    default: // heavy ai case
    {
        GameManager* pGameManager = GameManager::getInstance();
        if (!pGameManager->isHeavyAiType(type))
        {
            CONSOLE_PRINT("Rejected unknown AI type " + QString::number(type), GameConsole::eERROR);
            break;
        }
        if (Settings::getInstance()->getSpawnAiProcess() &&
            !Settings::getInstance()->getAiSlave())
        {
            ret = MemoryManagement::create<DummyAi>(pMap, type);
        }
        else
        {
            const qint32 index = *GameManager::getHeavyAiIndex(type);
            QString id = pGameManager->getHeavyAiID(index);
            ret = MemoryManagement::create<HeavyAi>(pMap, id, type);
        }
        break;
    }
    }
    return ret;
}

GameEnums::AiTypes BaseGameInputIF::getAiType() const
{
    return m_AiType;
}

void BaseGameInputIF::setUnitBuildValue(QString unitID, float value)
{
    m_BuildingChanceModifier.insert(unitID, value);
}

float BaseGameInputIF::getUnitBuildValue(QString unitID)
{
    float modifier = m_pPlayer->getUnitBuildValue(unitID);
    if (m_BuildingChanceModifier.contains(unitID))
    {
        return modifier + m_BuildingChanceModifier[unitID];
    }
    return 1.0f + modifier;
}

void BaseGameInputIF::setMoveCostMapValue(qint32 x, qint32 y, qint32 value)
{
    if ((m_MoveCostMap.size() > x && x >= 0) &&
        (m_MoveCostMap[x].size() > y && y >= 0))
    {
        m_MoveCostMap[x][y] = std::tuple<qint32, bool>(value, true);
    }
}

qint32 BaseGameInputIF::getMoveCostMapValue(qint32 x, qint32 y)
{
    if ((m_MoveCostMap.size() > x && x >= 0) &&
        (m_MoveCostMap[x].size() > y && y >= 0))
    {
        return std::get<0>(m_MoveCostMap[x][y]);
    }
    return 0.0f;
}

bool BaseGameInputIF::getProcessing() const
{
    return m_processing;
}

void BaseGameInputIF::centerCameraOnAction(GameAction* pAction)
{
    if (Settings::getInstance()->getAutoCamera())
    {
        if ((m_pMap != nullptr && m_pMap->getCurrentPlayer() == m_pPlayer) ||
            m_pPlayer == nullptr)
        {
            if (pAction != nullptr)
            {
                if (m_pMenu != nullptr &&
                    m_pMenu->getActionPerformer() != nullptr)
                {
                    m_pMenu->getActionPerformer()->centerMapOnAction(pAction);
                }
            }
            else
            {
                if (m_pMap != nullptr)
                {
                    m_pMap->centerOnPlayer(m_pPlayer);
                }
            }
        }
    }
}
