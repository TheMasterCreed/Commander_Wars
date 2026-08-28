#pragma once

#include <QObject>
#include <QVariantMap>

namespace PropertyFactsProbe
{
QVariantMap run(QObject* pContext, const QVariantMap & arguments);
}
