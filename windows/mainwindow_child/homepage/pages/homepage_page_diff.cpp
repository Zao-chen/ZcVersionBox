#include "homepage_page_diff.h"
#include "homepage_diff_delegate.h"
#include "homepage_diff_view.h"
#include "ui_homepage_page_diff.h"
#include "windows/mainwindow_presentation.h"
#include <QAction>
#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHideEvent>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QTimer>
#include <oclero/qlementine/widgets/Expander.hpp>
#include <oclero/qlementine/widgets/LoadingSpinner.hpp>

HomePageDiffPage::HomePageDiffPage(BackupService *service, SettingsService *settings, AiGateway *gateway, QWidget *parent)
    : QWidget(parent), ui(new Ui::HomePageDiffPage), m_service(service), m_settings(settings), m_gateway(gateway)
{
    ui->setupUi(this);

    // 文件列表模型与委托
    ui->files->setModel(&m_model);
    UiStyle::flatView(ui->files);
    ui->files->setItemDelegate(new DiffFileDelegate(ui->files));
    ui->files->setAccessibleName("变更文件");

    // 字体与样式初始化
    UiStyle::text(ui->filesLabel, UiStyle::FontRole::Section, false);
    UiStyle::text(ui->filesCount, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->currentFilePath, UiStyle::FontRole::Body, false);
    UiStyle::text(ui->analysisStatus, UiStyle::FontRole::Caption, true);

    // 顶部操作动作与 Spinner
    m_analyze = UiStyle::action(this, "analyzeAction", "AI 智能速读", "sparkles");
    m_spinner = new oclero::qlementine::LoadingSpinner(this);
    m_spinner->setObjectName("analysisSpinner");
    m_spinner->setAccessibleName("AI 正在分析");
    m_spinner->setFixedSize(16, 16);
    ui->editorToolbarLayout->insertWidget(ui->editorToolbarLayout->indexOf(ui->expandAnalysisButton), m_spinner);

    // 底部 Expander 折叠动画容器（收纳 AI 分析结果，平时不占用主区高度）
    m_expander = new oclero::qlementine::Expander(this);
    m_expander->setObjectName("analysisExpander");
    ui->editorLayout->removeWidget(ui->analysisContent);
    m_expander->setContent(ui->analysisContent);
    ui->editorLayout->addWidget(m_expander);

    ui->expandAnalysisButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    ui->expandAnalysisButton->setIcon(UiStyle::icon("chevron_right"));
    connect(ui->expandAnalysisButton, &QToolButton::clicked, m_expander, &oclero::qlementine::Expander::toggleExpanded);
    connect(m_expander, &oclero::qlementine::Expander::expandedChanged, this, [this] {
        ui->expandAnalysisButton->setIcon(UiStyle::icon(m_expander->expanded() ? "arrow_down" : "chevron_right"));
        ui->expandAnalysisButton->setAccessibleDescription(m_expander->expanded() ? "已展开" : "已折叠");
        updateResponsiveLayout();
    });

    ui->btnCopyPath->setIcon(UiStyle::icon("copy"));

    // 页面 Splitter
    ui->splitter->setChildrenCollapsible(false);
    ui->splitter->setStretchFactor(0, 0);
    ui->splitter->setStretchFactor(1, 1);
    ui->editorPane->installEventFilter(this);

    // 信号槽连接
    connect(ui->files->selectionModel(), &QItemSelectionModel::currentChanged, this, &HomePageDiffPage::loadFile);
    connect(m_analyze, &QAction::triggered, this, &HomePageDiffPage::analyze);
    connect(ui->btnCopyPath, &QToolButton::clicked, this, &HomePageDiffPage::copyCurrentPath);

    if (settings)
        connect(settings, &SettingsService::changed, this, &HomePageDiffPage::updateLoadingState);

    connect(service, &BackupService::repositoryInvalidated, this, [this](const QString &id) {
        if (id == m_id)
        {
            deactivate();
            m_valid = false;
            m_hasAnalysis = false;
            m_analysisText.clear();
            ui->analysis->clear();
            updateLoadingState();
        }
        for (auto it = m_states.begin(); it != m_states.end();)
            it = it.key().startsWith(id + '\n') ? m_states.erase(it) : ++it;
    });

    updateLoadingState();
    refreshTheme();
}

HomePageDiffPage::~HomePageDiffPage() = default;

QList<QAction *> HomePageDiffPage::toolbarActions() const
{
    return {m_analyze};
}

void HomePageDiffPage::rememberState()
{
    if (!m_valid || m_repositoryGeneration != m_service->repositoryGeneration(m_id))
        return;
    if (!m_currentFile.isEmpty())
        m_fileScrolls[m_currentFile] = ui->diffView->scrollPosition();

    m_states[m_id + '\n' + m_diff.newCommit] = {
        m_repositoryGeneration,
        m_currentFile,
        m_hasAnalysis && !m_loading ? m_analysisText : QString(),
        ui->files->verticalScrollBar()->value(),
        m_expander ? m_expander->expanded() : false,
        m_fileScrolls};
}

void HomePageDiffPage::deactivate()
{
    rememberState();
    ++m_generation;
    ++m_fileGeneration;
    m_active = false;
    if (m_loading)
    {
        ui->analysis->clear();
        m_analysisText.clear();
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
    m_currentParsedDiff = {};
    m_currentRawDiff.clear();
    ui->diffView->clear();
    m_model.clear();
    m_hasAnalysis = false;
    m_analysisText.clear();

    emit titleChanged("正在读取版本差异…");
    updateLoadingState();

    const auto generation = m_generation;
    const auto repositoryGeneration = m_repositoryGeneration;

    m_service->diff(id, commit, this, [this, id, generation, repositoryGeneration](const BackupResult<DiffData> &reply) {
        if (!m_active || generation != m_generation || id != m_id || repositoryGeneration != m_service->repositoryGeneration(id))
            return;
        m_valid = reply.result.success;
        if (!reply.result.success)
        {
            emit notification(reply.result);
            emit titleChanged("无法打开版本对比");
            m_hasAnalysis = false;
            updateLoadingState();
            return;
        }

        m_diff = reply.value;
        auto state = m_states.value(id + '\n' + m_diff.newCommit);
        if (state.generation != m_repositoryGeneration)
            state = {};

        m_fileScrolls = state.scrolls;

        emit titleChanged(QString("版本对比 · %1 → %2 · %3 个变更文件")
                              .arg(m_diff.oldCommit.isEmpty() ? "初始版本" : m_diff.oldCommit.left(8),
                                   m_diff.newCommit.left(8))
                              .arg(m_diff.files.size()));

        ui->filesCount->setText(QString::number(m_diff.files.size()));

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
            ui->currentFilePath->clear();

        ui->files->verticalScrollBar()->setValue(state.fileScroll);
        m_hasAnalysis = !state.analysis.isEmpty();
        m_analysisText = state.analysis;
        ui->analysis->setPlainText(m_analysisText);
        m_analysisStatus = m_hasAnalysis ? "分析完成" : QString();
        m_expander->setExpanded(m_hasAnalysis && state.expanded);

        updateLoadingState();
        updateResponsiveLayout();
    });
}

void HomePageDiffPage::loadFile()
{
    const auto current = ui->files->currentIndex();
    if (!current.isValid())
        return;

    if (!m_currentFile.isEmpty())
        m_fileScrolls[m_currentFile] = ui->diffView->scrollPosition();

    m_currentFile = current.data(PathRole).toString();

    ui->currentFilePath->setText(m_currentFile);
    ui->currentFilePath->setToolTip(m_currentFile);

    const auto scroll = m_fileScrolls.value(m_currentFile);
    const auto generation = ++m_fileGeneration;
    const auto id = m_id;
    const auto repositoryGeneration = m_repositoryGeneration;

    ui->diffView->clear();

    m_service->diffText(id, m_diff, m_currentFile, this, [this, id, scroll, generation, repositoryGeneration](const BackupResult<QString> &reply) {
        if (!m_active || generation != m_fileGeneration || id != m_id || repositoryGeneration != m_service->repositoryGeneration(id))
            return;
        if (!reply.result.success)
            emit notification(reply.result);

        m_currentRawDiff = reply.value;
        m_currentParsedDiff = DiffParser::parse(reply.value);

        ui->diffView->setDiff(m_currentParsedDiff, m_currentRawDiff, m_currentFile, m_service->sourcePath(m_id));

        QTimer::singleShot(0, this, [this, scroll, generation] {
            if (generation == m_fileGeneration)
            {
                ui->diffView->setScrollPosition(scroll);
            }
        });
    });
}

void HomePageDiffPage::copyCurrentPath()
{
    if (m_currentFile.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(m_currentFile);
    emit notification(OperationResult::ok(QStringLiteral("已复制相对路径到剪贴板")));
}

void HomePageDiffPage::refreshTheme()
{
    ui->diffView->refreshTheme();
    ui->files->viewport()->update();
}

void HomePageDiffPage::analyze()
{
    if (!m_active || !m_valid || m_loading || !m_settings || !m_settings->isAiConfigured())
        return;

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
    m_analysisText.clear();
    ui->analysis->clear();
    m_expander->setExpanded(false);
    updateLoadingState();

    QPointer<HomePageDiffPage> guard(this);
    m_service->diffText(id, m_diff, {}, this, [guard, generation, id, repositoryGeneration, config](const BackupResult<QString> &reply) {
        if (!guard || !guard->m_active || generation != guard->m_generation || id != guard->m_id ||
            repositoryGeneration != guard->m_service->repositoryGeneration(id))
            return;

        if (!reply.result.success || reply.value.trimmed().isEmpty())
        {
            guard->m_loading = false;
            guard->ui->analysis->clear();
            guard->updateLoadingState();
            emit guard->notification(reply.result.success ? OperationResult::fail("AI 分析失败", "当前版本对比没有可分析的变更") : reply.result);
            return;
        }

        guard->m_gateway->summarize(config, reply.value, guard, [guard, generation, id, repositoryGeneration](const QString &summary, const QString &error) {
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
            guard->m_analysisText = error.isEmpty() ? summary : error;
            guard->ui->analysis->setPlainText(guard->m_analysisText);
            guard->updateLoadingState();
            guard->m_expander->setExpanded(true);

            if (!error.isEmpty())
                emit guard->notification(OperationResult::fail("AI 分析失败", error));
        });
    });
}

void HomePageDiffPage::updateLoadingState()
{
    const bool configured = m_settings && m_settings->isAiConfigured();
    m_analyze->setEnabled(m_active && m_valid && !m_loading && configured);
    m_analyze->setToolTip(configured ? QStringLiteral("AI 智能速读") : QStringLiteral("AI 未配置：请先在设置中配置 API Key 和模型"));

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

    ui->filesPane->setMinimumWidth(compact ? 0 : 180);
    ui->filesPane->setMinimumHeight(compact ? 120 : 0);
    ui->filesPane->setMaximumWidth(compact ? QWIDGETSIZE_MAX : 280);
    ui->filesPane->setMaximumHeight(compact ? 150 : QWIDGETSIZE_MAX);

    if (ui->splitter->orientation() != orientation)
    {
        ui->splitter->setOrientation(orientation);
        ui->splitter->setSizes(compact ? QList<int>{140, qMax(160, height() - 140)} : QList<int>{220, qMax(240, width() - 220)});
    }
    else if (!compact && ui->splitter->sizes().value(0) < 180)
    {
        ui->splitter->setSizes({220, qMax(240, width() - 220)});
    }

    const auto analysisHeight = qMin(180, qMax(1, ui->editorPane->height() / 3));
    ui->analysisContent->setFixedHeight(analysisHeight);
    m_expander->setMaximumHeight(analysisHeight);
}

void HomePageDiffPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const auto margin = width() < 640 ? 12 : 20;
    ui->pageLayout->setContentsMargins(margin, 8, margin, 12);
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
    m_spinner->setSpinning(false);
}

void HomePageDiffPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_active = true;
    updateLoadingState();
}
