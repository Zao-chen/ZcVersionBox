#pragma once

#include "backup_types.h"

struct BackupMonitorOptions
{
    qint64 quietPeriodMs{500};
    qint64 maximumCoalesceMs{5000};
    qint64 scanIntervalMs{30000};
    qint64 auditIntervalMs{30000};
    qint64 initialRetryMs{1500};
    qint64 maximumRetryMs{60000};
    int watchBatchSize{256};
};

struct BackupScanRequest
{
    BackupObservationTarget target;
    quint64 token{0}, changeVersion{0};
};

enum class BackupScanStatus
{
    Unchanged,
    Changed,
    Unavailable,
    Cancelled
};

struct BackupScanResult
{
    BackupScanRequest request;
    BackupScanStatus status{BackupScanStatus::Unchanged};
    OperationResult result;
    QStringList watchPaths;
};
