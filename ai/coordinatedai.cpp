#include "ai/coordinatedai.h"

CoordinatedAi::CoordinatedAi(GameMap* pMap)
    : NormalAi(
          pMap,
          NormalAi::DEFAULT_INI_FILE,
          GameEnums::AiTypes_Coordinated,
          NormalAi::DEFAULT_JS_NAME)
{
#ifdef GRAPHICSUPPORT
    setObjectName("CoordinatedAi");
#endif
}

void CoordinatedAi::process()
{
    if (holdForPause())
    {
        return;
    }
    NormalAi::process();
}
