#ifndef HOMEPAGE_H
#define HOMEPAGE_H

#include <QWidget>

namespace Ui {
class HomePage;
}

class HomePage : public QWidget
{
    Q_OBJECT

public:
    explicit HomePage(QWidget *parent = nullptr);
    void openBackup(QString FilePathWithCode);
    ~HomePage();

private slots:
    void on_widget_BreadcrumbBar_breadcrumbClicked(QString breadcrumb, QStringList lastBreadcrumbList);

private:
    Ui::HomePage *ui;
    void LoadBackupFileList();
};

#endif // HOMEPAGE_H
