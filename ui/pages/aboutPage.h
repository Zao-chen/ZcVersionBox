#pragma once
#include <QWidget>
#include <memory>
namespace Ui
{
class AboutPage;
}
class AboutPage : public QWidget
{
    Q_OBJECT
  public:
    explicit AboutPage(QWidget *parent = nullptr);
    ~AboutPage();

  private:
    std::unique_ptr<Ui::AboutPage> ui;
};
