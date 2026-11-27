#pragma once
#include <QObject>
#include <QVector>

#include "3rd_party/oxygine-framework/oxygine/actor/Actor.h"

class Player;
class GameMap;
class IslandMap;
using spIslandMap = std::shared_ptr<IslandMap>;

class IslandMap final : public QObject
{
    Q_OBJECT
    static constexpr qint32 UNKNOWN = -1;
public:
    explicit IslandMap(GameMap* pMap, const QString & unitID, Player* pOwner, const QString & movementType = "");
    virtual ~IslandMap() = default;
    /**
     * @brief getValueOnIsland
     * @param island
     * @param ownValue
     * @param enemyValue
     */
    void getValueOnIsland(qint32 island, qint32 &ownValue, qint32 & enemyValue);
    /**
     * @brief show for debugging purpose
     */
    Q_INVOKABLE void show();
    /**
     * @brief hide for debugging purpose
     */
    Q_INVOKABLE void hide();
    Q_INVOKABLE inline bool sameIsland(qint32 x1, qint32 y1,
                           qint32 x2, qint32 y2) const
    {
        if (x1 < 0 || y1 < 0 || x2 < 0 || y2 < 0 ||
            x1 >= static_cast<qint32>(m_Islands.size()) ||
            x2 >= static_cast<qint32>(m_Islands.size()) ||
            y1 >= static_cast<qint32>(m_Islands[x1].size()) ||
            y2 >= static_cast<qint32>(m_Islands[x2].size()))
        {
            return false;
        }
        return (m_Islands[x1][y1] > UNKNOWN &&
                m_Islands[x1][y1] == m_Islands[x2][y2]);
    }
    Q_INVOKABLE inline qint32 getIsland(qint32 x, qint32 y) const
    {
        if (x < 0 || y < 0 ||
            x >= static_cast<qint32>(m_Islands.size()) ||
            y >= static_cast<qint32>(m_Islands[x].size()))
        {
            return UNKNOWN;
        }
        return m_Islands[x][y];
    }
    /**
     * @brief getIslandSize
     * @param island
     * @return
     */
    Q_INVOKABLE qint32 getIslandSize(qint32 island) const;
    /**
     * @brief getMovementType
     * @return
     */
    Q_INVOKABLE QString getMovementType() const;
private:
    std::vector<std::vector<qint32>> m_Islands;
    QString m_MovementType;
    Player* m_pOwner;
    GameMap* m_pMap{nullptr};
    std::vector<oxygine::spActor> m_info;
};
