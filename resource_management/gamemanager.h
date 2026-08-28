#ifndef GAMEMANAGER_H
#define GAMEMANAGER_H

#include <limits>
#include <optional>

#include "game/GameEnums.h"
#include "resource_management/ressourcemanagement.h"

class GameMap;

class GameManager final : public QObject, public RessourceManagement<GameManager>
{
    Q_OBJECT
public:
    /**
     * @brief loadAll
     */
    virtual void loadAll() override;
    /**
     * @brief reset
     */
    virtual void reset() override;

    QString getActionIcon(const QString & actionID);
    oxygine::spSprite getIcon(GameMap* pMap, const QString & icon);
    /**
     * @brief getDescription
     * @param position
     * @return
     */
    QString getDescription(qint32 position);
    QString getHeavyAiID(qint32 position) const;
    qint32 getHeavyAiCount() const;

    static constexpr std::optional<qint32> getHeavyAiIndex(GameEnums::AiTypes type)
    {
        const qint64 rawType = static_cast<qint32>(type);
        if (rawType == GameEnums::AiTypes_Closed ||
            rawType == GameEnums::AiTypes_Open)
        {
            return std::nullopt;
        }
        const qint64 index = rawType - GameEnums::AiTypes_Heavy;
        if (index < 0 || index > std::numeric_limits<qint32>::max())
        {
            return std::nullopt;
        }
        return static_cast<qint32>(index);
    }

    static constexpr std::optional<GameEnums::AiTypes> getHeavyAiType(qint32 index)
    {
        if (index < 0)
        {
            return std::nullopt;
        }
        const qint64 rawType = static_cast<qint64>(GameEnums::AiTypes_Heavy) + index;
        if (rawType == GameEnums::AiTypes_Closed ||
            rawType == GameEnums::AiTypes_Open ||
            rawType > std::numeric_limits<qint32>::max())
        {
            return std::nullopt;
        }
        return static_cast<GameEnums::AiTypes>(static_cast<qint32>(rawType));
    }

    bool isHeavyAiType(GameEnums::AiTypes type) const;
    static constexpr bool isHeavyAiType(GameEnums::AiTypes type, qint32 loadedHeavyAiCount)
    {
        const std::optional<qint32> index = getHeavyAiIndex(type);
        return loadedHeavyAiCount >= 0 &&
               index.has_value() &&
               *index < loadedHeavyAiCount;
    }

    bool isComputerAiType(GameEnums::AiTypes type) const;
    static constexpr bool isComputerAiType(GameEnums::AiTypes type, qint32 loadedHeavyAiCount)
    {
        return GameEnums::isBuiltInComputerAiType(type) ||
               isHeavyAiType(type, loadedHeavyAiCount);
    }

    bool isKnownAiType(GameEnums::AiTypes type) const;
    static constexpr bool isKnownAiType(GameEnums::AiTypes type, qint32 loadedHeavyAiCount)
    {
        return type == GameEnums::AiTypes_Human ||
               type == GameEnums::AiTypes_Closed ||
               type == GameEnums::AiTypes_Open ||
               GameEnums::isInternalAiType(type) ||
               isComputerAiType(type, loadedHeavyAiCount);
    }
    /**
     * @brief getDefaultActionbBannlist
     * @return
     */
    QStringList getDefaultActionbBannlist();
protected:
    friend MemoryManagement;
    GameManager();
    virtual ~GameManager() = default;
private:
    oxygine::spSprite getIconSprite(const QString & icon);
    QStringList m_loadedHeavyAis;
    QStringList m_loadedNormalAis;
    QStringList m_loadedVeryEasyAis;
};

#endif // GAMEMANAGER_H
