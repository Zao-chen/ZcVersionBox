#include "mainwindow_navigation.h"
#include <QPointer>
#include <QScopedValueRollback>
void Navigation::invalidateRevisions(const QString &id)
{
    bool affected = false;
    for (auto &route : m_routes)
        if (route.backupId == id && (route.page == PageId::Diff || route.page == PageId::Conflict))
        {
            route = {PageId::History, id};
            affected = true;
        }
    if (affected)
        emit changed(current());
}

void Navigation::go(const Route &route)
{
    if (current() == route)
        return;
    if (guarded([this, route] { go(route); })) return;
    m_routes.resize(m_index + 1);
    m_routes.append(route);
    ++m_index;
    emit changed(current());
}
void Navigation::replaceCurrent(const Route &route)
{
    if (current() == route)
        return;
    if (guarded([this, route] { replaceCurrent(route); })) return;
    m_routes[m_index] = route;
    emit changed(current());
}
void Navigation::back()
{
    if (canBack())
    {
        if (guarded([this] { back(); })) return;
        --m_index;
        emit changed(current());
    }
}
void Navigation::forward()
{
    if (canForward())
    {
        if (guarded([this] { forward(); })) return;
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

bool Navigation::guarded(std::function<void()> action)
{
    if (!m_guard || m_guardBypassed) return false;
    const QPointer<Navigation> self(this);
    m_guard([self, action = std::move(action)] {
        if (!self) return;
        QScopedValueRollback<bool> bypass(self->m_guardBypassed, true);
        action();
    });
    return true;
}
