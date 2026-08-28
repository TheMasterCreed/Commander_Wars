#pragma once

#include <QObject>
#include <QVariantMap>

namespace FocusFireProbe
{
QVariantMap run(QObject* pContext, const QVariantMap & arguments);
}
