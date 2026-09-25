#include "navigation.h"
void Navigation::go(const Route &route)
{
    if (current() == route)
        return;
    m_routes.resize(m_index + 1);
    m_routes.append(route);
    ++m_index;
    emit changed(current());
}
void Navigation::back()
{
    if (canBack())
    {
        --m_index;
        emit changed(current());
    }
}
void Navigation::forward()
{
    if (canForward())
    {
        ++m_index;
        emit changed(current());
    }
}
void Navigation::removeBackup(const QString &id)
{
    bool affected = false;
    for (auto &route : m_routes)
        if (route.backupId == id)
        {
            route = {};
            affected = true;
        }
    if (affected)
        emit changed(current());
}
void Navigation::retainBackups(const QSet<QString> &ids)
{
    bool affected = false;
    for (auto &route : m_routes)
        if (!route.backupId.isEmpty() && !ids.contains(route.backupId))
        {
            route = {};
            affected = true;
        }
    if (affected)
        emit changed(current());
}
