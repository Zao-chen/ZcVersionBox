#pragma once

#include "operationresult.h"
#include <QStringList>

struct LinuxIntegrationPaths
{
    QString configHome, dataHome, executable;
    static LinuxIntegrationPaths defaults();
};

// Paths are injectable so registration tests only touch temporary directories.
class LinuxIntegration
{
  public:
    explicit LinuxIntegration(LinuxIntegrationPaths paths = LinuxIntegrationPaths::defaults());
    OperationResult setAutoStart(bool enabled) const;
    OperationResult setNautilusScript(bool enabled) const;
    static OperationResult nautilusSelection(const QByteArray &uris, QStringList &paths);

  private:
    LinuxIntegrationPaths m_paths;
};
