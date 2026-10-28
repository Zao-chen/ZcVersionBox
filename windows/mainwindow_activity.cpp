#include "windows/mainwindow_activity.h"
#include "windows/mainwindow_presentation.h"
#include <QHBoxLayout>
#include <QLabel>
#include <oclero/qlementine/widgets/LoadingSpinner.hpp>

ActivityIndicator::ActivityIndicator(QWidget *parent, int spinnerSize) : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    m_spinner = new oclero::qlementine::LoadingSpinner(this);
    m_spinner->setFixedSize(spinnerSize, spinnerSize);
    m_spinner->setSpinning(true);
    m_caption = new QLabel(this);
    m_caption->setTextFormat(Qt::PlainText);
    UiStyle::text(m_caption, UiStyle::FontRole::Caption, true);
    layout->addWidget(m_spinner, 0, Qt::AlignVCenter);
    layout->addWidget(m_caption, 0, Qt::AlignVCenter);
    m_delay.setSingleShot(true);
    m_delay.setInterval(250);
    connect(&m_delay, &QTimer::timeout, this, &QWidget::show);
    setAccessibleName("正在处理");
    hide();
}
void ActivityIndicator::setActivity(bool active, const QString &label)
{
    const auto text = label.isEmpty() ? QString() : label + "…";
    m_caption->setText(text);
    m_caption->setVisible(m_captionVisible && !text.isEmpty());
    setToolTip(text);
    setAccessibleDescription(text);
    if (active == m_active)
        return;
    m_active = active;
    if (active)
        m_delay.start();
    else
    {
        m_delay.stop();
        hide();
    }
}
void ActivityIndicator::setCaptionVisible(bool visible)
{
    m_captionVisible = visible;
    m_caption->setVisible(visible && !m_caption->text().isEmpty());
}
