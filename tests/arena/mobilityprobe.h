#pragma once

#include <QObject>
#include <QVariantMap>

namespace MobilityProbe
{
QVariantMap run(QObject* pContext, const QVariantMap & arguments);
}
