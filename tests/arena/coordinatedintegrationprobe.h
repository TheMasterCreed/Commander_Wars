#pragma once

#include <QVariantMap>

class QObject;

class CoordinatedIntegrationProbe final
{
public:
    static QVariantMap run(QObject* pContext,
                           const QVariantMap & arguments);
};
