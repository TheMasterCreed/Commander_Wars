#pragma once

#include <QObject>
#include <QVariantMap>

namespace AttackFactsProbe
{
QVariantMap run(QObject* pContext, const QVariantMap & arguments);
}
