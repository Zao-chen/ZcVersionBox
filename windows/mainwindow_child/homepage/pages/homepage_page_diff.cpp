#include "homepage_page_diff.h"
#include "ui_homepage_page_diff.h"
#include "windows/mainwindow_presentation.h"
#include <QAction>
#include <QFileInfo>
#include <QHideEvent>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTimer>
#include <oclero/qlementine/widgets/Expander.hpp>
#include <oclero/qlementine/widgets/LoadingSpinner.hpp>

namespace
{
enum FileRole
{
    PathRole = Qt::UserRole + 1,
    StatusRole,
    SummaryRole
};
QString statusText(const QString &status)
{
    const QMap<QChar, QString> labels{{'A', "新增"}, {'D', "删除"}, {'M', "修改"}, {'R', "重命名"}, {'C', "复制"}, {'T', "类型变化"}};
    return status.isEmpty() ? QStringLiteral("变更") : labels.value(status.front(), status);
}
class DiffHighlighter : public QSyntaxHighlighter
{
  public:
    using QSyntaxHighlighter::QSyntaxHighlighter;

  protected:
    void highlightBlock(const QString &line) override
    {
        const auto colors = UiStyle::colors();
        QTextCharFormat format;
        if (line.startsWith("diff --git") || line.startsWith("index ") || line.startsWith("@@") ||
            line.startsWith("--- ") || line.startsWith("+++ ") || line.startsWith("\\ ") || line.startsWith("Binary files "))
            format.setForeground(colors.secondary);
        else if (line.startsWith('+'))
            format.setForeground(colors.added);
        else if (line.startsWith('-'))
            format.setForeground(colors.removed);
        else
            return;
        setFormat(0, line.size(), format);
    }
};
class FileDelegate : public QStyledItemDelegate
{
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return {0, UiStyle::rowHeight(56, UiStyle::font(UiStyle::FontRole::Body), true)};
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &opt, const QModelIndex &index) const override
    {
        const auto colors = UiStyle::colors();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const auto row = opt.rect.adjusted(0, 2, 0, -2);
        if (opt.state.testFlag(QStyle::State_Selected) || opt.state.testFlag(QStyle::State_MouseOver))
        {
            painter->setPen(Qt::NoPen);
            painter->setBrush(opt.state.testFlag(QStyle::State_Selected) ? colors.selected : colors.hover);
            painter->drawRoundedRect(row, 6, 6);
        }
        if (opt.state.testFlag(QStyle::State_HasFocus))
        {
            painter->setPen(colors.secondary);
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(QRectF(row).adjusted(.5, .5, -.5, -.5), 6, 6);
        }
        const auto font = UiStyle::font(UiStyle::FontRole::Body);
        const auto caption = UiStyle::font(UiStyle::FontRole::Caption);
        const auto textRect = row.adjusted(10, 6, -10, -6);
        painter->setFont(font);
        painter->setPen(colors.text);
        painter->drawText(QRect(textRect.x(), textRect.y(), textRect.width(), QFontMetrics(font).height()), Qt::AlignVCenter | Qt::AlignLeft,
                          QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideRight, textRect.width()));
        const auto path = index.data(PathRole).toString();
        const auto parent = path.contains('/') ? path.left(path.lastIndexOf('/')) : QString();
        const auto detail = statusText(index.data(StatusRole).toString()) + " · " + index.data(SummaryRole).toString() + (parent.isEmpty() ? "" : " · " + parent);
        painter->setFont(caption);
        painter->setPen(colors.secondary);
        painter->drawText(QRect(textRect.x(), textRect.y() + QFontMetrics(font).height() + 2, textRect.width(), QFontMetrics(caption).height()), Qt::AlignVCenter | Qt::AlignLeft,
                          QFontMetrics(caption).elidedText(detail, Qt::ElideMiddle, textRect.width()));
        painter->restore();
    }
};
} // namespace

HomePageDiffPage::HomePageDiffPage(BackupService *service, SettingsService *settings, AiGateway *gateway, QWidget *parent)
    : QWidget(parent), ui(new Ui::HomePageDiffPage), m_service(service), m_settings(settings), m_gateway(gateway)
{
    ui->setupUi(this);
    ui->files->setModel(&m_model);
    UiStyle::flatView(ui->files);
    ui->files->setItemDelegate(new FileDelegate(ui->files));
    ui->files->setAccessibleName("变更文件");
    UiStyle::text(ui->content, UiStyle::FontRole::Code);
    ui->content->setAccessibleName("版本差异");
    ui->analysis->setAccessibleName("AI 分析结果");
    ui->filePath->setAccessibleName("当前文件路径");
    UiStyle::text(ui->rangeLabel, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->filesLabel, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->filePath, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->analysisStatus, UiStyle::FontRole::Caption, true);
    m_highlighter = new DiffHighlighter(ui->content->document());
    m_analyze = UiStyle::action(this, "analyzeAction", "AI 分析", "sparkles");
    ui->splitter->setChildrenCollapsible(false);
    ui->splitter->setStretchFactor(0, 0);
    ui->splitter->setStretchFactor(1, 1);
    ui->editorPane->installEventFilter(this);
    ui->editorLayout->removeWidget(ui->analysisContent);
    m_expander = new oclero::qlementine::Expander(this);
    m_expander->setObjectName("analysisExpander");
    m_expander->setContent(ui->analysisContent);
    ui->editorLayout->addWidget(m_expander);
    ui->expandAnalysisButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    ui->expandAnalysisButton->setIcon(UiStyle::icon("chevron_right"));
    connect(ui->expandAnalysisButton, &QToolButton::clicked, m_expander, &oclero::qlementine::Expander::toggleExpanded);
    connect(m_expander, &oclero::qlementine::Expander::expandedChanged, this, [this]
            {
        ui->expandAnalysisButton->setIcon(UiStyle::icon(m_expander->expanded() ? "arrow_down" : "chevron_right"));
        ui->expandAnalysisButton->setAccessibleDescription(m_expander->expanded() ? "已展开" : "已折叠");
        updateResponsiveLayout(); });
    m_spinner = new oclero::qlementine::LoadingSpinner(this);
    m_spinner->setObjectName("analysisSpinner");
    m_spinner->setAccessibleName("AI 正在分析");
    m_spinner->setFixedSize(16, 16);
    ui->analysisHeaderLayout->insertWidget(1, m_spinner);
    connect(ui->files->selectionModel(), &QItemSelectionModel::currentChanged, this, &HomePageDiffPage::loadFile);
    connect(m_analyze, &QAction::triggered, this, &HomePageDiffPage::analyze);
    connect(service, &BackupService::repositoryInvalidated, this, [this](const QString &id)
            {
        if (id == m_id)
        {
            deactivate();
            m_valid = false;
            m_hasAnalysis = false;
            ui->analysis->clear();
            updateLoadingState();
        }
        for (auto it = m_states.begin(); it != m_states.end();)
            it = it.key().startsWith(id + '\n') ? m_states.erase(it) : ++it; });
    updateLoadingState();
}
HomePageDiffPage::~HomePageDiffPage() = default;
QList<QAction *> HomePageDiffPage::toolbarActions() const { return {m_analyze}; }
void HomePageDiffPage::rememberState()
{
    if (!m_valid || m_repositoryGeneration != m_service->repositoryGeneration(m_id))
        return;
    if (!m_currentFile.isEmpty())
        m_fileScrolls[m_currentFile] = {ui->content->horizontalScrollBar()->value(), ui->content->verticalScrollBar()->value()};
    m_states[m_id + '\n' + m_diff.newCommit] = {m_repositoryGeneration, m_currentFile,
                                                m_hasAnalysis && !m_loading ? ui->analysis->toPlainText() : QString(),
                                                ui->files->verticalScrollBar()->value(), m_expander->expanded(), m_fileScrolls};
}
void HomePageDiffPage::deactivate()
{
    rememberState();
    ++m_generation;
    m_active = false;
    if (m_loading)
    {
        ui->analysis->clear();
        m_hasAnalysis = false;
    }
    m_loading = false;
    updateLoadingState();
}
void HomePageDiffPage::setRevision(const QString &id, const QString &commit)
{
    deactivate();
    m_id = id;
    m_repositoryGeneration = m_service->repositoryGeneration(id);
    m_active = true;
    m_valid = false;
    m_currentFile.clear();
    m_fileScrolls.clear();
    ui->content->clear();
    ui->analysis->clear();
    m_model.clear();
    const auto result = m_service->diff(id, commit, m_diff);
    m_valid = result.success;
    if (!result.success)
    {
        emit notification(result);
        ui->rangeLabel->setText("无法打开版本对比");
        m_hasAnalysis = false;
        updateLoadingState();
        return;
    }
    auto state = m_states.value(id + '\n' + m_diff.newCommit);
    if (state.generation != m_repositoryGeneration)
        state = {};
    m_fileScrolls = state.scrolls;
    ui->rangeLabel->setText(QString("%1 → %2 · %3 个变更文件").arg(m_diff.oldCommit.left(7), m_diff.newCommit.left(7)).arg(m_diff.files.size()));
    ui->rangeLabel->setToolTip(m_diff.oldCommit + " → " + m_diff.newCommit);
    int selected = 0;
    for (const auto &file : m_diff.files)
    {
        auto *item = new QStandardItem(QFileInfo(file.path).fileName());
        item->setEditable(false);
        item->setData(file.path, PathRole);
        item->setData(file.status, StatusRole);
        item->setData(file.summary, SummaryRole);
        item->setToolTip(file.path + "\n" + statusText(file.status) + " · " + file.summary);
        m_model.appendRow(item);
        if (file.path == state.file)
            selected = m_model.rowCount() - 1;
    }
    if (m_model.rowCount())
        ui->files->setCurrentIndex(m_model.index(selected, 0));
    else
        ui->filePath->clear();
    ui->files->verticalScrollBar()->setValue(state.fileScroll);
    m_hasAnalysis = !state.analysis.isEmpty();
    ui->analysis->setPlainText(state.analysis);
    m_analysisStatus = m_hasAnalysis ? "分析完成" : QString();
    m_expander->setExpanded(m_hasAnalysis && state.expanded);
    updateLoadingState();
    updateResponsiveLayout();
}
void HomePageDiffPage::loadFile()
{
    const auto current = ui->files->currentIndex();
    if (!current.isValid())
        return;
    if (!m_currentFile.isEmpty())
        m_fileScrolls[m_currentFile] = {ui->content->horizontalScrollBar()->value(), ui->content->verticalScrollBar()->value()};
    m_currentFile = current.data(PathRole).toString();
    ui->filePath->setText(m_currentFile);
    ui->filePath->setCursorPosition(0);
    ui->filePath->setToolTip(m_currentFile);
    QString text;
    const auto result = m_service->diffText(m_id, m_diff, m_currentFile, text);
    if (!result.success)
        emit notification(result);
    // Preserve the original patch, including metadata, binary markers and tabs.
    ui->content->setPlainText(text);
    const auto scroll = m_fileScrolls.value(m_currentFile);
    const auto generation = ++m_fileGeneration;
    QTimer::singleShot(0, this, [this, scroll, generation]
                       {
        if (generation == m_fileGeneration)
        {
            ui->content->horizontalScrollBar()->setValue(scroll.x());
            ui->content->verticalScrollBar()->setValue(scroll.y());
        } });
}
void HomePageDiffPage::refreshTheme()
{
    // Recolor the existing document without resetting selection or scroll.
    m_highlighter->rehighlight();
    ui->files->viewport()->update();
}
void HomePageDiffPage::analyze()
{
    if (!m_active || !m_valid || m_loading)
        return;
    QString diff;
    const auto result = m_service->diffText(m_id, m_diff, {}, diff);
    if (!result.success)
    {
        emit notification(result);
        return;
    }
    if (diff.trimmed().isEmpty())
    {
        emit notification(OperationResult::fail("AI 分析失败", "当前版本对比没有可分析的变更"));
        return;
    }
    AiConfigHelper::RuntimeConfig config;
    QString error;
    if (!m_settings->runtimeConfig(config, error))
    {
        emit notification(OperationResult::fail("AI 未配置", error));
        return;
    }
    const auto generation = ++m_generation;
    const auto id = m_id;
    const auto repositoryGeneration = m_service->repositoryGeneration(id);
    m_loading = true;
    m_hasAnalysis = false;
    ui->analysis->clear();
    m_expander->setExpanded(false);
    updateLoadingState();
    QPointer<HomePageDiffPage> guard(this);
    m_gateway->summarize(config, diff, this, [guard, generation, id, repositoryGeneration](const QString &summary, const QString &error)
                         {
        if (!guard || generation != guard->m_generation)
            return;
        guard->m_loading = false;
        if (!guard->m_active || id != guard->m_id || !guard->m_service->contains(id) || repositoryGeneration != guard->m_service->repositoryGeneration(id))
        {
            guard->updateLoadingState();
            return;
        }
        guard->m_hasAnalysis = true;
        guard->m_analysisStatus = error.isEmpty() ? "分析完成" : "分析失败";
        guard->ui->analysis->setPlainText(error.isEmpty() ? summary : error);
        guard->updateLoadingState();
        guard->m_expander->setExpanded(true);
        if (!error.isEmpty())
            emit guard->notification(OperationResult::fail("AI 分析失败", error)); });
}
void HomePageDiffPage::updateLoadingState()
{
    m_analyze->setEnabled(m_active && m_valid && !m_loading);
    m_spinner->setSpinning(m_loading && m_active && isVisible());
    m_spinner->setVisible(m_loading);
    const bool showAnalysis = m_loading || m_hasAnalysis;
    ui->expandAnalysisButton->setVisible(showAnalysis);
    ui->analysisStatus->setVisible(showAnalysis);
    ui->analysisStatus->setText(m_loading ? "正在分析…" : m_analysisStatus);
    m_expander->setVisible(showAnalysis);
}
void HomePageDiffPage::updateResponsiveLayout()
{
    const bool compact = width() < 640;
    const auto orientation = compact ? Qt::Vertical : Qt::Horizontal;
    ui->filesPane->setMinimumWidth(compact ? 0 : 160);
    ui->filesPane->setMinimumHeight(compact ? 112 : 0);
    ui->filesPane->setMaximumWidth(compact ? QWIDGETSIZE_MAX : 280);
    ui->filesPane->setMaximumHeight(compact ? 144 : QWIDGETSIZE_MAX);
    if (ui->splitter->orientation() != orientation)
    {
        ui->splitter->setOrientation(orientation);
        ui->splitter->setSizes(compact ? QList<int>{144, qMax(160, height() - 144)} : QList<int>{220, qMax(240, width() - 220)});
    }
    else if (!compact && ui->splitter->sizes().value(0) < 160)
        ui->splitter->setSizes({220, qMax(240, width() - 220)});
    const auto analysisHeight = qMin(200, qMax(1, ui->editorPane->height() / 3));
    ui->analysisContent->setFixedHeight(analysisHeight);
    // Expander animates its size hint. Bound the host as well as the editor so
    // the content's preferred height cannot reserve unused space at small sizes.
    m_expander->setMaximumHeight(analysisHeight);
}
void HomePageDiffPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const auto margin = width() < 640 ? 16 : 24;
    ui->pageLayout->setContentsMargins(margin, 12, margin, 16);
    updateResponsiveLayout();
}
bool HomePageDiffPage::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->editorPane && event->type() == QEvent::Resize)
        updateResponsiveLayout();
    return QWidget::eventFilter(watched, event);
}
void HomePageDiffPage::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    // MainWindow calls deactivate for a route change. Native window recreation
    // (pinning) or hiding to the tray does not change the revision context.
    m_spinner->setSpinning(false);
}
void HomePageDiffPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_active = true;
    updateLoadingState();
}
