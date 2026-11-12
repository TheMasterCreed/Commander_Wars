#pragma once

#include <cstdint>
#include <vector>

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include "ai/coordinator/packingdualcertificate.h"
#include "tests/coordinator_ai/masterassignmentoracle.h"

namespace MasterAssignmentTest
{
    int runOfflineStockReference(const QString & requestPath);
    void testOfflineStockReference();

    bool readStockField(const QJsonObject & object, Coordinator::PackingDualFieldIdentity & field, QString & error);
    bool readStockKey(const QJsonValue & value, const Coordinator::PackingDualFieldIdentity & field,
                      Coordinator::ContinuationKey & key);
    std::vector<std::int32_t> capturedStockColumns(const Coordinator::PackingDualFieldIdentity & field,
                                                   const Coordinator::ContinuationKey & key);
    MasterAssignmentOracle::SequentialInput friendlyStockInput(const Coordinator::PackingDualFieldIdentity & field,
                                                               const Coordinator::ContinuationKey & key);
}
