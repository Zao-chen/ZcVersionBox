#include "presentation.h"
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <oclero/qlementine/style/QlementineStyle.hpp>
#include <oclero/qlementine/style/Theme.hpp>
#include <oclero/qlementine/widgets/Label.hpp>

ThemeController::ThemeController(QObject *parent) : QObject(parent)
{
    m_style = new oclero::qlementine::QlementineStyle(qApp);
    m_style->setAutoIconColor(oclero::qlementine::AutoIconColor::TextColor);
    const auto families = QFontDatabase::families();
    for (const auto &family : {QStringLiteral("Microsoft YaHei UI"), QStringLiteral("PingFang SC"), QStringLiteral("Noto Sans CJK SC")})
    {
        if (families.contains(family))
        {
            QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Han, family);
            break;
        }
    }
    QApplication::setStyle(m_style);
    apply();
}
void ThemeController::apply()
{
    QFile file(m_dark ? ":/themes/dark.json" : ":/themes/light.json");
    if (file.open(QIODevice::ReadOnly))
    {
        auto json = QJsonDocument::fromJson(file.readAll()).object();
        json.insert("useSystemFonts", true);
        const auto theme = oclero::qlementine::Theme::fromJsonDoc(QJsonDocument(json));
        if (theme)
            m_style->setTheme(*theme);
    }
    emit changed();
}
void ThemeController::toggle()
{
    m_dark = !m_dark;
    apply();
}
QIcon ThemeController::icon(const QString &name) const
{
    return m_style->makeThemedIcon(":/icons/" + name + ".svg", QSize(20, 20));
}

NotificationBar::NotificationBar(QWidget *parent) : QWidget(parent)
{
    setObjectName("notificationBar");
    auto *layout = new QHBoxLayout(this);
    m_text = new QLabel(this);
    m_text->setWordWrap(true);
    auto textPolicy = m_text->sizePolicy();
    textPolicy.setHorizontalPolicy(QSizePolicy::Ignored);
    m_text->setSizePolicy(textPolicy);
    m_text->setTextFormat(Qt::PlainText);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *close = new QToolButton(this);
    close->setText("×");
    close->setToolTip("关闭提示");
    layout->addWidget(m_text, 1);
    layout->addWidget(close);
    connect(close, &QToolButton::clicked, this, &QWidget::hide);
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &QWidget::hide);
    hide();
}
void NotificationBar::showResult(const OperationResult &result)
{
    if (result.title.isEmpty() && result.warning.isEmpty() && result.success)
        return;
    m_level = result.warning.isEmpty() ? result.level : NotificationLevel::Warning;
    const QString level = m_level == NotificationLevel::Error ? "错误" : m_level == NotificationLevel::Warning   ? "注意"
                                                                     : m_level == NotificationLevel::Information ? "提示"
                                                                                                                 : "完成";
    m_text->setText(level + " · " + result.title + (result.message.isEmpty() ? QString() : "\n" + result.message) + (result.warning.isEmpty() ? QString() : "\n" + result.warning));
    show();
    update();
    m_timer.start(result.duration);
}
void NotificationBar::paintEvent(QPaintEvent *)
{
    const auto *style = qobject_cast<oclero::qlementine::QlementineStyle *>(qApp->style());
    if (!style)
        return;
    const auto &theme = style->theme();
    const auto color = m_level == NotificationLevel::Error ? theme.statusColorError : m_level == NotificationLevel::Warning   ? theme.statusColorWarning
                                                                                  : m_level == NotificationLevel::Information ? theme.statusColorInfo
                                                                                                                              : theme.statusColorSuccess;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(theme.backgroundColorMain1);
    painter.setPen(color);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), theme.borderRadius, theme.borderRadius);
}
BreadcrumbBar::BreadcrumbBar(QWidget *parent) : QWidget(parent)
{
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
}
void BreadcrumbBar::setRoute(const Route &route, const QString &name)
{
    while (auto *item = m_layout->takeAt(0))
    {
        if (item->widget())
            delete item->widget();
        delete item;
    }
    QVector<QPair<QString, Route>> crumbs;
    if (route.page == PageId::About)
        crumbs.append({"关于", {PageId::About}});
    else if (route.page == PageId::GeneralSettings || route.page == PageId::AiSettings)
    {
        crumbs.append({"设置", {PageId::GeneralSettings}});
        if (route.page == PageId::AiSettings)
            crumbs.append({"AI", route});
    }
    else
    {
        crumbs.append(QPair<QString, Route>{"备份", Route{}});
        if (!route.backupId.isEmpty())
            crumbs.append({name, {PageId::Dashboard, route.backupId}});
        if (route.page == PageId::History || route.page == PageId::Diff)
            crumbs.append({"历史版本", {PageId::History, route.backupId}});
        if (route.page == PageId::Diff)
            crumbs.append({"版本对比", route});
    }
    for (int i = 0; i < crumbs.size(); ++i)
    {
        if (i)
            m_layout->addWidget(new QLabel("/", this));
        if (i + 1 == crumbs.size())
        {
            auto *label = new oclero::qlementine::Label(crumbs[i].first, this);
            label->setTextFormat(Qt::PlainText);
            label->setToolTip(crumbs[i].first);
            label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            m_layout->addWidget(label, 1);
            continue;
        }
        auto *button = new QToolButton(this);
        const auto target = crumbs[i].second;
        button->setText(crumbs[i].first.size() > 20 ? crumbs[i].first.left(17) + "…" : crumbs[i].first);
        button->setToolTip(crumbs[i].first);
        connect(button, &QToolButton::clicked, this, [this, target]
                {
                    emit navigate(target);
                });
        m_layout->addWidget(button);
    }
}
QString formatBytes(qint64 bytes)
{
    const QStringList units{"B", "KB", "MB", "GB", "TB"};
    double value = bytes;
    int index = 0;
    while (value >= 1024 && index < units.size() - 1)
    {
        value /= 1024;
        ++index;
    }
    return QString::number(value, 'f', index == 0 ? 0 : value >= 10 ? 1
                                                                    : 2) +
           " " + units[index];
}
bool confirmAction(QWidget *owner, const QString &text, const QString &action)
{
    QDialog dialog(owner);
    dialog.setWindowTitle(action);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel(text, &dialog);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    layout->addWidget(label);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto *confirm = buttons->addButton(action, QDialogButtonBox::AcceptRole);
    confirm->setAutoDefault(false);
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(480, dialog.sizeHint().height());
    return dialog.exec() == QDialog::Accepted;
}
void openLocalPath(QWidget *owner, const QString &path)
{
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        QMessageBox::warning(owner, "打开失败", "无法打开目标路径");
}
