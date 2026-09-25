#pragma once
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

enum class PageId
{
    Backups,
    Dashboard,
    History,
    Diff,
    GeneralSettings,
    AiSettings,
    About
};
struct Route
{
    PageId page{PageId::Backups};
    QString backupId;
    QString commit;
    QString provider;
    bool operator==(const Route &other) const
    {
        return page == other.page && backupId == other.backupId && commit == other.commit && provider == other.provider;
    }
};
Q_DECLARE_METATYPE(Route)

inline bool isObjectPage(PageId page)
{
    return page == PageId::Dashboard || page == PageId::History || page == PageId::Diff;
}
inline bool isSettingsPage(PageId page)
{
    return page == PageId::GeneralSettings || page == PageId::AiSettings || page == PageId::About;
}

class Navigation : public QObject
{
    Q_OBJECT
  public:
    explicit Navigation(QObject *parent = nullptr) : QObject(parent) { m_routes.append(Route{}); }
    Route current() const { return m_routes.at(m_index); }
    bool canBack() const { return m_index > 0; }
    bool canForward() const { return m_index + 1 < m_routes.size(); }
    void go(const Route &route);
    void back();
    void forward();
    void removeBackup(const QString &id);
    void retainBackups(const QSet<QString> &ids);
    void invalidateRevisions(const QString &id);
  signals:
    void changed(const Route &route);

  private:
    QVector<Route> m_routes;
    int m_index{0};
};
