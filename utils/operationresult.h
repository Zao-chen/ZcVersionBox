#pragma once

#include <QMetaType>
#include <QString>

enum class NotificationLevel
{
    Information,
    Success,
    Warning,
    Error
};

struct OperationResult
{
    bool success{false};
    QString title;
    QString message;
    QString warning;
    QString path;
    int duration{3000};
    NotificationLevel level{NotificationLevel::Error};
    bool cancelled{false};

    static OperationResult ok(const QString &title, const QString &message = {}, int duration = 2500)
    {
        return {true, title, message, {}, {}, duration, NotificationLevel::Success};
    }
    static OperationResult fail(const QString &title, const QString &message, int duration = 3000)
    {
        return {false, title, message, {}, {}, duration, NotificationLevel::Error};
    }
    static OperationResult warn(const QString &title, const QString &message, int duration = 3000)
    {
        return {false, title, message, {}, {}, duration, NotificationLevel::Warning};
    }
    static OperationResult info(const QString &title, const QString &message, int duration = 3000)
    {
        return {true, title, message, {}, {}, duration, NotificationLevel::Information};
    }
    static OperationResult cancel(const QString &title, const QString &message = {})
    {
        auto result = warn(title, message);
        result.cancelled = true;
        return result;
    }
};
Q_DECLARE_METATYPE(OperationResult)
