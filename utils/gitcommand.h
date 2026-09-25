#pragma once

#include <QByteArray>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>

struct GitResult
{
    bool started{false};
    bool finished{false};
    int exitCode{-1};
    QString output;
    QString error;
    QByteArray bytes;
    bool cancelled{false};
    bool success() const { return started && finished && exitCode == 0; }
};

struct GitOptions
{
    int timeoutMs{120000};
    QByteArray input;
    std::shared_ptr<std::atomic_bool> cancel;
};
using GitRunner = std::function<GitResult(const QString &, const QStringList &, const GitOptions &)>;
GitResult runGit(const QString &repository, const QStringList &arguments, const GitOptions &options = {});
