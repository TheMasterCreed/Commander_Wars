#pragma once

#include <QObject>
#include <QVariantMap>

namespace KnowledgeProbe
{
QVariantMap run(QObject* pContext, const QVariantMap & arguments);
}
