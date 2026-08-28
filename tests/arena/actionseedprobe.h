#pragma once

#include <QObject>
#include <QVariantMap>

namespace ActionSeedProbe
{
QVariantMap run(QObject* pContext, const QVariantMap & arguments);
}
