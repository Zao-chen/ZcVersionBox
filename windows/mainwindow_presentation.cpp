#include "windows/mainwindow_presentation.h"
#include "utils/settingsservice.h"
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFile>
#include <QFocusFrame>
#include <QFontDatabase>
#include <QFontInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QStyleOptionToolButton>
#include <QStyleHints>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <oclero/qlementine/style/QlementineStyle.hpp>
#include <oclero/qlementine/style/Theme.hpp>
#include <oclero/qlementine/utils/IconUtils.hpp>
#include <oclero/qlementine/utils/PrimitiveUtils.hpp>
#include <oclero/qlementine/widgets/Label.hpp>
#include <oclero/qlementine/widgets/Popover.hpp>

namespace
{
using namespace oclero::qlementine;
static bool s_keyboardNavigationActive = false;

// Keep public Qlementine drawing and metrics, with neutral navigation colors
// and the application's lighter button typography.
class AppStyle final : public QlementineStyle
{
  public:
    using QlementineStyle::polish;
    using QlementineStyle::QlementineStyle;
    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget = nullptr) const override
    {
        if (element == CE_FocusFrame)
        {
            if (const auto *focusFrame = qobject_cast<const QFocusFrame *>(widget))
            {
                if (const auto *monitored = focusFrame->widget())
                {
                    const bool isTextInput = qobject_cast<const QLineEdit *>(monitored)
                                          || qobject_cast<const QTextEdit *>(monitored)
                                          || qobject_cast<const QPlainTextEdit *>(monitored);
                    if (!isTextInput && !s_keyboardNavigationActive)
                        return;
                }
            }
        }
        if (element == CE_ToolButtonLabel && widget && widget->property("navigationItem").toBool())
        {
            if (const auto *button = qstyleoption_cast<const QStyleOptionToolButton *>(option))
            {
                auto aligned = *button;
                // Keep Qlementine's icon, text and state drawing, aligned to the start of a full-width navigation row.
                aligned.rect.setWidth(qMin(aligned.rect.width(), widget->sizeHint().width()));
                QlementineStyle::drawControl(element, &aligned, painter, widget);
                return;
            }
        }
        QlementineStyle::drawControl(element, option, painter, widget);
    }
    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget = nullptr) const override
    {
        if (element == PE_FrameFocusRect && !s_keyboardNavigationActive)
        {
            bool allow = false;
            if (const auto *focusFrame = qobject_cast<const QFocusFrame *>(widget))
            {
                if (const auto *monitored = focusFrame->widget())
                {
                    if (qobject_cast<const QLineEdit *>(monitored)
                        || qobject_cast<const QTextEdit *>(monitored)
                        || qobject_cast<const QPlainTextEdit *>(monitored))
                    {
                        allow = true;
                    }
                }
            }
            if (!allow)
                return;
        }
        QlementineStyle::drawPrimitive(element, option, painter, widget);
        if (element == PE_FrameButtonBevel)
        {
            if (const auto *optButton = qstyleoption_cast<const QStyleOptionButton *>(option))
            {
                if (!optButton->features.testFlag(QStyleOptionButton::Flat))
                {
                    const auto &borderColor = optButton->state.testFlag(QStyle::State_Enabled)
                        ? theme().borderColor
                        : theme().borderColorDisabled;
                    drawRoundedRectBorder(painter, optButton->rect, borderColor, 1.0, theme().borderRadius);
                }
            }
        }
    }
    void polish(QWidget *widget) override
    {
        QlementineStyle::polish(widget);
        if (auto *button = qobject_cast<QAbstractButton *>(widget))
        {
            if (button->focusPolicy() == Qt::StrongFocus || button->focusPolicy() == Qt::ClickFocus)
                button->setFocusPolicy(Qt::TabFocus);
            auto font = widget->font();
            const auto *push = qobject_cast<QPushButton *>(widget);
            font.setWeight(push && push->isDefault() ? QFont::DemiBold : QFont::Normal);
            widget->setFont(font);
        }
    }
    const QColor &toolButtonBackgroundColor(MouseState mouse, ColorRole role) const override
    {
        if (mouse == MouseState::Pressed || role == ColorRole::Primary)
            return theme().neutralColorPressed;
        if (mouse == MouseState::Hovered)
            return theme().neutralColorHovered;
        return theme().neutralColorTransparent;
    }
    const QColor &toolButtonForegroundColor(MouseState mouse, ColorRole) const override
    {
        return mouse == MouseState::Disabled ? theme().secondaryColorDisabled : theme().secondaryColor;
    }
    QColor listItemBackgroundColor(MouseState mouse, SelectionState selected, FocusState, ActiveState,
                                   const QModelIndex &, const QWidget *) const override
    {
        if (selected == SelectionState::Selected)
            return theme().neutralColorPressed;
        return mouse == MouseState::Hovered ? theme().neutralColorHovered : theme().neutralColorTransparent;
    }
    const QColor &listItemForegroundColor(MouseState mouse, SelectionState, FocusState, ActiveState) const override
    {
        return mouse == MouseState::Disabled ? theme().secondaryColorDisabled : theme().secondaryColor;
    }
    const QColor &listItemCaptionForegroundColor(MouseState, SelectionState, FocusState, ActiveState) const override
    {
        return theme().secondaryAlternativeColor;
    }
    const QColor &tableHeaderBgColor(MouseState mouse, CheckState) const override
    {
        return mouse == MouseState::Hovered ? theme().neutralColorHovered : theme().backgroundColorMain1;
    }
    const QColor &tableHeaderFgColor(MouseState, CheckState) const override
    {
        return theme().secondaryAlternativeColor;
    }
    const QColor &tableLineColor() const override { return theme().borderColorTransparent; }
    const QColor &menuBackgroundColor() const override
    {
        static const QColor light("#FFFFFF"), dark("#252527");
        return theme().backgroundColorMain1.lightness() < 128 ? dark : light;
    }
    const QColor &menuItemBackgroundColor(MouseState mouse) const override
    {
        return mouse == MouseState::Pressed   ? theme().neutralColorPressed
               : mouse == MouseState::Hovered ? theme().neutralColorHovered
                                              : theme().neutralColorTransparent;
    }
    const QColor &menuItemForegroundColor(MouseState mouse) const override
    {
        return mouse == MouseState::Disabled ? theme().secondaryColorDisabled : theme().secondaryColor;
    }
    const QColor &menuItemSecondaryForegroundColor(MouseState mouse) const override
    {
        return mouse == MouseState::Disabled ? theme().secondaryAlternativeColorDisabled : theme().secondaryAlternativeColor;
    }
};

const QlementineStyle *zcStyle()
{
    return qobject_cast<QlementineStyle *>(qApp->style());
}
void updateWidgetPresentation(QWidget *widget)
{
    const auto role = widget->property("fontRole");
    if (role.isValid())
    {
        auto palette = widget->palette();
        const auto color = widget->property("secondaryText").toBool() ? UiStyle::colors().secondary : UiStyle::colors().text;
        palette.setColor(QPalette::WindowText, color);
        palette.setColor(QPalette::Text, color);
        widget->setPalette(palette);
        widget->setFont(UiStyle::font(static_cast<UiStyle::FontRole>(role.toInt())));
    }
    const auto surface = widget->property("surfaceRole");
    if (surface.isValid())
    {
        const auto colors = UiStyle::colors();
        const auto value = static_cast<UiStyle::Surface>(surface.toInt());
        const auto color = value == UiStyle::Surface::Sidebar ? colors.sidebar
                           : value == UiStyle::Surface::Popup ? colors.popup
                                                              : colors.canvas;
        auto palette = widget->palette();
        palette.setColor(QPalette::Window, color);
        palette.setColor(QPalette::Base, color);
        widget->setPalette(palette);
        widget->setAutoFillBackground(true);
    }
}
} // namespace

namespace UiStyle
{
Colors colors()
{
    const auto *style = zcStyle();
    const bool dark = style && style->theme().backgroundColorMain1.lightness() < 128;
    return dark ? Colors{QColor("#1C1C1E"), QColor("#18181A"), QColor("#252527"), QColor("#EDEDED"),
                         QColor("#A2A2A6"), QColor("#28282B"), QColor("#343438"), QColor("#333337"),
                         QColor("#9DC6AC"), QColor("#D8A0A0")}
                : Colors{QColor("#FAFAFA"), QColor("#F3F3F3"), QColor("#FFFFFF"), QColor("#242424"),
                         QColor("#666666"), QColor("#EFEFEF"), QColor("#E7E7E7"), QColor("#E5E5E5"),
                         QColor("#326B47"), QColor("#9A4141")};
}
QFont font(FontRole role)
{
    const auto *style = zcStyle();
    if (!style)
        return QApplication::font();
    const auto &theme = style->theme();
    switch (role)
    {
    case FontRole::Page:
        return theme.fontH1;
    case FontRole::Object:
        return theme.fontH3;
    case FontRole::Section:
        return theme.fontH5;
    case FontRole::Title:
    {
        auto result = theme.fontBold;
        result.setPointSizeF(result.pointSizeF() * 13. / 14.);
        return result;
    }
    case FontRole::Caption:
        return theme.fontCaption;
    case FontRole::Code:
        return theme.fontMonospace;
    case FontRole::Sidebar:
    {
        auto result = theme.fontRegular;
        result.setPointSizeF(result.pointSizeF() * 13. / 14.);
        return result;
    }
    default:
        return theme.fontRegular;
    }
}
QIcon icon(const QString &name)
{
    const auto *style = zcStyle();
    const auto color = colors().text;
    const auto disabled = style ? style->theme().secondaryColorDisabled : colors().secondary;
    return oclero::qlementine::makeIconFromSvg(":/icons/" + name + ".svg",
                                               oclero::qlementine::IconTheme(color, disabled, color, disabled), QSize(16, 16));
}
void text(QWidget *widget, FontRole role, bool secondary)
{
    if (auto *label = qobject_cast<oclero::qlementine::Label *>(widget))
    {
        using TextRole = oclero::qlementine::TextRole;
        label->setRole(role == FontRole::Page ? TextRole::H1 : role == FontRole::Object ? TextRole::H3
                                                           : (role == FontRole::Section || role == FontRole::Title) ? TextRole::H5
                                                           : role == FontRole::Caption  ? TextRole::Caption
                                                                                        : TextRole::Default);
    }
    widget->setProperty("fontRole", static_cast<int>(role));
    widget->setProperty("secondaryText", secondary);
    updateWidgetPresentation(widget);
}
void surface(QWidget *widget, Surface role)
{
    widget->setProperty("surfaceRole", static_cast<int>(role));
    updateWidgetPresentation(widget);
}
void flatView(QAbstractItemView *view)
{
    view->setFrameShape(QFrame::NoFrame);
    view->setAlternatingRowColors(false);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setMouseTracking(true);
}
QAction *action(QObject *owner, const QString &name, const QString &label, const QString &iconName)
{
    auto *result = new QAction(label, owner);
    result->setObjectName(name);
    if (!iconName.isEmpty())
    {
        result->setProperty("iconName", iconName);
        result->setIcon(icon(iconName));
    }
    return result;
}
QToolButton *toolButton(QWidget *parent, QAction *action, bool iconOnly)
{
    auto *button = new QToolButton(parent);
    button->setDefaultAction(action);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::TabFocus);
    button->setIconSize({16, 16});
    button->setMinimumSize(28, 32);
    button->setToolButtonStyle(iconOnly ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    button->setAccessibleName(action->text());
    return button;
}
int rowHeight(int minimum, const QFont &font, bool twoLines)
{
    return qMax(minimum, QFontMetrics(font).height() * (twoLines ? 2 : 1) + (twoLines ? 16 : 12));
}
bool isKeyboardNavigationActive()
{
    return s_keyboardNavigationActive;
}
void setKeyboardNavigationActive(bool active)
{
    if (s_keyboardNavigationActive == active)
        return;
    s_keyboardNavigationActive = active;
    for (auto *widget : QApplication::allWidgets())
    {
        if (auto *focusFrame = qobject_cast<QFocusFrame *>(widget))
        {
            if (!active)
            {
                if (auto *monitored = focusFrame->widget())
                {
                    if (!qobject_cast<QLineEdit *>(monitored) &&
                        !qobject_cast<QTextEdit *>(monitored) &&
                        !qobject_cast<QPlainTextEdit *>(monitored))
                    {
                        focusFrame->hide();
                    }
                }
            }
            else
            {
                if (auto *monitored = focusFrame->widget())
                {
                    if (monitored->hasFocus())
                        focusFrame->show();
                }
            }
            focusFrame->update();
        }
        else if (auto *itemView = qobject_cast<QAbstractItemView *>(widget))
        {
            itemView->update();
            if (itemView->viewport())
                itemView->viewport()->update();
        }
    }
    if (auto *focusWidget = QApplication::focusWidget())
    {
        focusWidget->update();
    }
}
} // namespace UiStyle

namespace
{
bool systemPrefersDarkColor()
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}
ThemeMode themeModeFromString(const QString &value)
{
    return value == QStringLiteral("light")   ? ThemeMode::Light
           : value == QStringLiteral("dark") ? ThemeMode::Dark
                                             : ThemeMode::System;
}
QString themeModeToString(ThemeMode mode)
{
    switch (mode)
    {
    case ThemeMode::Light:
        return QStringLiteral("light");
    case ThemeMode::Dark:
        return QStringLiteral("dark");
    case ThemeMode::System:
        break;
    }
    return QStringLiteral("system");
}
} // namespace

ThemeController::ThemeController(SettingsService *settings, QObject *parent)
    : QObject(parent), m_settings(settings)
{
    m_style = new AppStyle(qApp);
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
    if (m_settings)
        m_mode = themeModeFromString(m_settings->themeMode());
    m_dark = m_mode == ThemeMode::Light ? false : m_mode == ThemeMode::Dark || systemPrefersDarkColor();
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme)
    {
        if (m_mode != ThemeMode::System)
            return;
        const bool dark = systemPrefersDarkColor();
        if (dark == m_dark)
            return;
        m_dark = dark;
        apply();
    });
    apply();
}
void ThemeController::apply()
{
    QFile file(m_dark ? ":/themes/dark.json" : ":/themes/light.json");
    if (file.open(QIODevice::ReadOnly))
    {
        auto parsed = oclero::qlementine::Theme::fromJsonDoc(QJsonDocument::fromJson(file.readAll()));
        if (parsed)
        {
            auto &theme = *parsed;
            auto system = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
            auto families = QStringList{system.family()};
            const auto available = QFontDatabase::families();
            for (const auto &family : {QStringLiteral("Microsoft YaHei UI"), QStringLiteral("PingFang SC"), QStringLiteral("Noto Sans CJK SC")})
                if (available.contains(family) && !families.contains(family))
                    families.append(family);
            system.setFamilies(families);
#ifdef Q_OS_MACOS
            constexpr double normalSystemPoints = 13.;
#else
            constexpr double normalSystemPoints = 9.;
#endif
            const auto scale = qMax(1., system.pointSizeF() / normalSystemPoints);
            const auto *screen = QGuiApplication::primaryScreen();
            const auto pointsPerPixel = 72. / (screen ? screen->logicalDotsPerInchY() : 96.);
            const auto sized = [system, scale, pointsPerPixel](int px, QFont::Weight weight = QFont::Normal)
            {
                auto result = system;
                result.setPointSizeF(px * pointsPerPixel * scale);
                result.setWeight(weight);
                return result;
            };
            // Theme::useSystemFonts initializes fonts from the platform. Set both
            // its fonts and size fields afterwards so Label and native Qt agree.
            theme.fontRegular = sized(14);
            theme.fontBold = sized(14, QFont::DemiBold);
            theme.fontH1 = theme.fontH2 = sized(20, QFont::DemiBold);
            theme.fontH3 = theme.fontH4 = sized(16, QFont::DemiBold);
            theme.fontH5 = sized(14, QFont::DemiBold);
            theme.fontCaption = sized(12);
            theme.fontMonospace = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            // Prefer the platform's contemporary monospace face when installed.
            for (const auto &family : {QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"), QStringLiteral("Menlo")})
                if (available.contains(family))
                {
                    auto monoFamilies = families;
                    monoFamilies.prepend(family);
                    theme.fontMonospace.setFamilies(monoFamilies);
                    break;
                }
            theme.fontMonospace.setPointSizeF(13 * pointsPerPixel * scale);
            theme.fontSize = qRound(14 * scale);
            theme.fontSizeH1 = theme.fontSizeH2 = qRound(20 * scale);
            theme.fontSizeH3 = theme.fontSizeH4 = qRound(16 * scale);
            theme.fontSizeH5 = qRound(14 * scale);
            theme.fontSizeS1 = qRound(12 * scale);
            theme.fontSizeMonospace = qRound(13 * scale);
            theme.controlHeightLarge = qMax(32, QFontMetrics(theme.fontRegular).height() + 12);
            theme.controlHeightMedium = qMax(28, QFontMetrics(theme.fontRegular).height() + 8);
            theme.palette.setColor(QPalette::All, QPalette::Window, theme.backgroundColorMain1);
            theme.palette.setColor(QPalette::All, QPalette::Highlight, theme.neutralColorPressed);
            theme.palette.setColor(QPalette::All, QPalette::HighlightedText, theme.secondaryColor);
            theme.palette.setColor(QPalette::All, QPalette::PlaceholderText, theme.secondaryAlternativeColor);
            theme.palette.setColor(QPalette::All, QPalette::ButtonText, theme.secondaryColor);
            m_style->setTheme(theme);
            QApplication::setFont(theme.fontRegular);
            QApplication::setPalette(theme.palette);
        }
    }
    for (auto *widget : QApplication::allWidgets())
        updateWidgetPresentation(widget);
    emit changed();
}
void ThemeController::setMode(ThemeMode mode)
{
    if (mode == m_mode)
        return;
    m_mode = mode;
    if (m_settings)
        m_settings->setThemeMode(themeModeToString(mode));
    m_dark = mode == ThemeMode::Light ? false : mode == ThemeMode::Dark || systemPrefersDarkColor();
    apply();
}
void ThemeController::toggle()
{
    setMode(m_dark ? ThemeMode::Light : ThemeMode::Dark);
}

NotificationBar::NotificationBar(QWidget *parent) : QWidget(parent)
{
    setObjectName("notificationBar");
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(16, 12, 12, 12);
    layout->setSpacing(12);
    m_symbol = new QLabel(this);
    m_symbol->setFixedWidth(20);
    m_symbol->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    layout->addWidget(m_symbol);
    auto *body = new QVBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(4);
    m_title = new QLabel(this);
    m_title->setTextFormat(Qt::PlainText);
    m_title->setWordWrap(true);
    m_text = new QLabel(this);
    m_text->setWordWrap(true);
    m_text->setTextFormat(Qt::PlainText);
    auto textPolicy = m_text->sizePolicy();
    textPolicy.setHorizontalPolicy(QSizePolicy::Ignored);
    m_text->setSizePolicy(textPolicy);
    m_detailsButton = new QPushButton("查看详情", this);
    m_detailsButton->setObjectName("notificationDetailsButton");
    m_detailsButton->setFlat(true);
    m_detailsButton->setAutoDefault(false);
    body->addWidget(m_title);
    body->addWidget(m_text);
    body->addWidget(m_detailsButton, 0, Qt::AlignLeft);
    layout->addLayout(body, 1);
    auto *closeAction = UiStyle::action(this, "dismissNotification", "关闭提示", "close");
    m_close = UiStyle::toolButton(this, closeAction, true);
    layout->addWidget(m_close, 0, Qt::AlignTop);
    connect(closeAction, &QAction::triggered, this, &NotificationBar::dismiss);
    connect(m_detailsButton, &QPushButton::clicked, this, &NotificationBar::openDetails);
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &NotificationBar::dismiss);
    parent->installEventFilter(this);
    UiStyle::text(m_title, UiStyle::FontRole::Section);
    UiStyle::text(m_text, UiStyle::FontRole::Caption, true);
    hide();
}
void NotificationBar::closeDetails()
{
    if (m_popover)
    {
        disconnect(m_popover, nullptr, this, nullptr);
        m_popover->closePopover();
        m_popover->hide();
        m_popover->deleteLater();
        m_popover = nullptr;
    }
}
void NotificationBar::dismiss()
{
    ++m_serial;
    m_timer.stop();
    closeDetails();
    hide();
    m_details.clear();
    m_title->clear();
    m_text->clear();
}
void NotificationBar::showResult(const OperationResult &result)
{
    if (result.title.isEmpty() && result.warning.isEmpty() && result.success)
        return;
    ++m_serial;
    m_timer.stop();
    closeDetails();
    m_level = result.warning.isEmpty() ? result.level : NotificationLevel::Warning;
    m_title->setText(result.title);
    const auto body = result.message + (result.warning.isEmpty() ? QString() : "\n" + result.warning);
    m_details = result.title + "\n\n" + body;
    const auto compact = body.simplified();
    m_text->setText(compact.size() > 110 ? compact.left(107) + "…" : compact);
    m_text->setVisible(!compact.isEmpty());
    m_detailsButton->setVisible(compact.size() > 110 || body.contains('\n'));
    m_remaining = result.duration;
    updateColors();
    positionOverlay();
    show();
    raise();
    if (m_remaining > 0)
        m_timer.start(m_remaining);
}
void NotificationBar::positionOverlay()
{
    const auto *host = parentWidget();
    if (!host)
        return;
    const auto width = qMin(420, qMax(180, host->width() - 32));
    setFixedWidth(width);
    layout()->invalidate();
    resize(width, layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(width) : layout()->totalSizeHint().height());
    move(qMax(8, host->width() - this->width() - 16), qMax(8, host->height() - height() - 16));
}
void NotificationBar::openDetails()
{
    if (m_popover)
        return;
    m_remaining = m_timer.isActive() ? qMax(1, m_timer.remainingTime()) : 0;
    m_timer.stop();
    const auto serial = m_serial;
    using Popover = oclero::qlementine::Popover;
    auto *popover = new Popover(this);
    m_popover = popover;
    popover->setObjectName("notificationPopover");
    popover->setAnchorWidget(m_detailsButton);
    popover->setPreferredPosition(Popover::Position::Top);
    popover->setPreferredAlignment(Popover::Alignment::End);
    popover->setPadding(QMargins(16, 16, 16, 16));
    popover->setBorderWidth(0);
    popover->setRadius(8);
    popover->setDropShadowRadius(12);
    popover->setDropShadowColor(QColor(0, 0, 0, 28));
    popover->setBackgroundColor(UiStyle::colors().popup);
    auto *details = new QPlainTextEdit(popover);
    details->setObjectName("notificationDetails");
    details->setReadOnly(true);
    details->setFrameShape(QFrame::NoFrame);
    details->setPlainText(m_details);
    UiStyle::text(details);
    UiStyle::surface(details, UiStyle::Surface::Popup);
    details->setFixedSize(qMin(460, window()->width() - 96), qMin(260, window()->height() - 120));
    popover->setContentWidget(details);
    connect(new QShortcut(QKeySequence(Qt::Key_Escape), details), &QShortcut::activated, popover, &Popover::closePopover);
    connect(popover, &Popover::opened, details, [details]
            { details->setFocus(Qt::PopupFocusReason); });
    connect(popover, &Popover::closed, this, [this, popover, serial]
            {
        if (serial != m_serial || m_popover != popover)
            return;
        m_popover = nullptr;
        popover->deleteLater();
        if (isVisible())
        {
            // Popover emits closed before it hides. Restore focus after Qt has
            // released the popup grab, otherwise hideEvent overwrites it.
            QTimer::singleShot(0, this, [this, serial]
            {
                if (serial == m_serial && isVisible())
                    m_detailsButton->setFocus(Qt::PopupFocusReason);
            });
            if (m_remaining > 0)
                m_timer.start(m_remaining);
        } });
    popover->openPopover();
}
bool NotificationBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::Show))
        positionOverlay();
    else if (watched == parentWidget() && event->type() == QEvent::Hide)
        dismiss();
    return QWidget::eventFilter(watched, event);
}
void NotificationBar::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && m_symbol)
        updateColors();
}
void NotificationBar::updateColors()
{
    const auto colors = UiStyle::colors();
    const auto color = m_level == NotificationLevel::Error     ? colors.removed
                       : m_level == NotificationLevel::Success ? colors.added
                                                               : colors.secondary;
    m_symbol->setText(m_level == NotificationLevel::Success ? "✓" : m_level == NotificationLevel::Information ? "i"
                                                                                                              : "!");
    auto palette = m_symbol->palette();
    palette.setColor(QPalette::WindowText, color);
    m_symbol->setPalette(palette);
    if (m_popover)
        m_popover->setBackgroundColor(colors.popup);
    update();
}
void NotificationBar::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(UiStyle::colors().popup);
    painter.setPen(UiStyle::colors().separator);
    painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 8, 8);
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
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(24);
    auto *label = new QLabel(text, &dialog);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    layout->addWidget(label);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto *confirm = buttons->addButton(action, QDialogButtonBox::AcceptRole);
    confirm->setAutoDefault(false);
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setFocus();
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
