#pragma once

#include <QString>

#include "tests/arena/arenamatchconfig.h"

enum class PlayableTrialMode
{
    Fresh,
    Save,
    Reload,
    Resume,
};

struct PlayableTrialConfig
{
    static const QString FILE_NAME;
    static const char* const ENVIRONMENT_FLAG;

    static bool load(const QString & profile, PlayableTrialConfig & config, QString & error);

    PlayableTrialMode mode{PlayableTrialMode::Fresh};
    QString sessionId;
    QString fixtureFile;
    QString resultFile;
    QString actionsFile;
    QString saveFile;
    QString checkpointFile;
    QString expectedStop;
    ArenaMatchConfig fixture;
    ArenaMatchConfig checkpointBoundary;
};
