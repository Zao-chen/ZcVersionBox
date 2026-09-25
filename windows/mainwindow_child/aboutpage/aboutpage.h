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

  protected:
    void resizeEvent(QResizeEvent *event) override;

  private:
    std::unique_ptr<Ui::AboutPage> ui;
};
