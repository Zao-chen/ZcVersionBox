#pragma once

#include <QStringList>

struct GitResult
{
    bool started{false};
    bool finished{false};
    int exitCode{-1};
    QString output;
    QString error;
    bool success() const { return started && finished && exitCode == 0; }
};

GitResult runGit(const QString &repository, const QStringList &arguments);
