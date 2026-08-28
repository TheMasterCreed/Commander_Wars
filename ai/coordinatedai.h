#ifndef COORDINATEDAI_H
#define COORDINATEDAI_H

#include "ai/normalai.h"

class CoordinatedAi;
using spCoordinatedAi = std::shared_ptr<CoordinatedAi>;

class CoordinatedAi final : public NormalAi
{
    Q_OBJECT
public:
    explicit CoordinatedAi(GameMap* pMap);
    ~CoordinatedAi() override = default;
public slots:
    void process() override;
};

#endif // COORDINATEDAI_H
