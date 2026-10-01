#include "homepage_page_diff.h"
#include "ui_homepage_page_diff.h"
#include "windows/mainwindow_presentation.h"
#include <QAction>
#include <QButtonGroup>
#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHideEvent>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
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

QColor statusBadgeColor(const QString &status, const UiStyle::Colors &colors)
{
    if (status.startsWith('A'))
        return colors.added;
    if (status.startsWith('D'))
        return colors.removed;
    if (status.startsWith('M'))
        return QColor(220, 130, 20); // 暖橙色
    if (status.startsWith('R') || status.startsWith('C'))
        return QColor(30, 136, 229); // 科技蓝
    return colors.secondary;
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
        return {0, UiStyle::rowHeight(58, UiStyle::font(UiStyle::FontRole::Body), true)};
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
            painter->drawRoundedRect(row, 8, 8);
        }
        if (UiStyle::isKeyboardNavigationActive() && opt.state.testFlag(QStyle::State_HasFocus))
        {
            painter->setPen(colors.secondary);
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(QRectF(row).adjusted(.5, .5, -.5, -.5), 8, 8);
        }

        const auto font = UiStyle::font(UiStyle::FontRole::Body);
        const auto caption = UiStyle::font(UiStyle::FontRole::Caption);
        const auto textRect = row.adjusted(12, 7, -12, -7);

        // 状态徽章 (Badge)
        const auto status = index.data(StatusRole).toString();
        const auto badgeName = statusText(status);
        const auto badgeCol = statusBadgeColor(status, colors);

        QFont badgeFont = caption;
        badgeFont.setPointSize(caption.pointSize() > 2 ? caption.pointSize() - 1 : 9);
        QFontMetrics badgeFm(badgeFont);
        const int badgeTextW = badgeFm.horizontalAdvance(badgeName);
        const int badgeW = badgeTextW + 10;
        const int badgeH = badgeFm.height() + 4;
        const QRect badgeRect(textRect.right() - badgeW, textRect.y() + 1, badgeW, badgeH);

        QColor badgeBg = badgeCol;
        badgeBg.setAlpha(36);
        painter->setPen(Qt::NoPen);
        painter->setBrush(badgeBg);
        painter->drawRoundedRect(badgeRect, 4, 4);

        painter->setFont(badgeFont);
        painter->setPen(badgeCol);
        painter->drawText(badgeRect, Qt::AlignCenter, badgeName);

        // 主标题：文件名
        const int titleW = textRect.width() - badgeW - 8;
        painter->setFont(font);
        painter->setPen(colors.text);
        painter->drawText(QRect(textRect.x(), textRect.y(), titleW, QFontMetrics(font).height()),
                          Qt::AlignVCenter | Qt::AlignLeft,
                          QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideRight, titleW));

        // 副标题：父路径与统计 (如 +8 -2)
        const auto path = index.data(PathRole).toString();
        const auto parent = path.contains('/') ? path.left(path.lastIndexOf('/')) : QString();
        const auto summary = index.data(SummaryRole).toString();

        QString subText = parent.isEmpty() ? QStringLiteral("/") : parent;
        if (!summary.isEmpty() && summary != "-")
            subText += " · " + summary;

        painter->setFont(caption);
        painter->setPen(colors.secondary);
        painter->drawText(QRect(textRect.x(), textRect.y() + QFontMetrics(font).height() + 3, textRect.width(), QFontMetrics(caption).height()),
                          Qt::AlignVCenter | Qt::AlignLeft,
                          QFontMetrics(caption).elidedText(subText, Qt::ElideMiddle, textRect.width()));

        painter->restore();
    }
};
} // namespace

HomePageDiffPage::HomePageDiffPage(BackupService *service, SettingsService *settings, AiGateway *gateway, QWidget *parent)
    : QWidget(parent), ui(new Ui::HomePageDiffPage), m_service(service), m_settings(settings), m_gateway(gateway)
{
    ui->setupUi(this);

    // 文件列表模型与委托
    ui->files->setModel(&m_model);
    UiStyle::flatView(ui->files);
    ui->files->setItemDelegate(new FileDelegate(ui->files));
    ui->files->setAccessibleName("变更文件");

    // 字体与样式初始化
    UiStyle::text(ui->filesLabel, UiStyle::FontRole::Section, false);
    UiStyle::text(ui->filesCount, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->currentFilePath, UiStyle::FontRole::Body, false);
    UiStyle::text(ui->fileStatusBadge, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->fileStatsBadge, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->aiTitle, UiStyle::FontRole::Section, false);
    UiStyle::text(ui->aiStatus, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->analysis, UiStyle::FontRole::Body, false);
    UiStyle::text(ui->imageNoticeTitle, UiStyle::FontRole::Section, false);
    UiStyle::text(ui->imageNoticeDesc, UiStyle::FontRole::Caption, true);

    UiStyle::text(ui->content, UiStyle::FontRole::Code);
    ui->content->setAccessibleName("版本原始差异");
    m_highlighter = new DiffHighlighter(ui->content->document());

    // 顶部操作动作与 Spinner
    m_analyze = UiStyle::action(this, "analyzeAction", "AI 智能速读", "sparkles");
    m_spinner = new oclero::qlementine::LoadingSpinner(this);
    m_spinner->setObjectName("analysisSpinner");
    m_spinner->setAccessibleName("AI 正在分析");
    m_spinner->setFixedSize(16, 16);
    ui->aiHeaderLayout->insertWidget(3, m_spinner);

    // AI 卡片装饰与图标
    ui->aiIcon->setPixmap(UiStyle::icon("sparkles").pixmap(20, 20));

    // Expander 折叠动画容器
    m_expander = new oclero::qlementine::Expander(this);
    m_expander->setObjectName("analysisExpander");
    ui->aiCardLayout->removeWidget(ui->analysisContent);
    m_expander->setContent(ui->analysisContent);
    ui->aiCardLayout->addWidget(m_expander);

    ui->expandAnalysisButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    ui->expandAnalysisButton->setIcon(UiStyle::icon("arrow_down"));
    connect(ui->expandAnalysisButton, &QToolButton::clicked, m_expander, &oclero::qlementine::Expander::toggleExpanded);
    connect(m_expander, &oclero::qlementine::Expander::expandedChanged, this, [this] {
        ui->expandAnalysisButton->setText(m_expander->expanded() ? QStringLiteral("收起") : QStringLiteral("展开"));
        ui->expandAnalysisButton->setIcon(UiStyle::icon(m_expander->expanded() ? "arrow_down" : "chevron_right"));
        ui->expandAnalysisButton->setAccessibleDescription(m_expander->expanded() ? "已展开" : "已折叠");
        updateResponsiveLayout();
    });

    // 视图模式单选组
    m_viewModeGroup = new QButtonGroup(this);
    m_viewModeGroup->setExclusive(true);
    m_viewModeGroup->addButton(ui->btnSideBySide);
    m_viewModeGroup->addButton(ui->btnUnified);
    m_viewModeGroup->addButton(ui->btnRaw);

    ui->btnSideBySide->setIcon(UiStyle::icon("compare"));
    ui->btnUnified->setIcon(UiStyle::icon("menu"));
    ui->btnRaw->setIcon(UiStyle::icon("file"));
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

    connect(ui->btnSideBySide, &QToolButton::clicked, this, [this] {
        setViewMode(DiffParser::ViewMode::SideBySide, false);
    });
    connect(ui->btnUnified, &QToolButton::clicked, this, [this] {
        setViewMode(DiffParser::ViewMode::Unified, false);
    });
    connect(ui->btnRaw, &QToolButton::clicked, this, [this] {
        setViewMode(DiffParser::ViewMode::SideBySide, true);
    });

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
        m_fileScrolls[m_currentFile] = {ui->visualBrowser->horizontalScrollBar()->value(),
                                        ui->visualBrowser->verticalScrollBar()->value()};

    m_states[m_id + '\n' + m_diff.newCommit] = {
        m_repositoryGeneration,
        m_currentFile,
        m_hasAnalysis && !m_loading ? m_analysisText : QString(),
        ui->files->verticalScrollBar()->value(),
        m_expander ? m_expander->expanded() : true,
        m_viewMode,
        m_rawMode,
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
    ui->visualBrowser->clear();
    ui->content->clear();
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
        m_aiExpanded = state.aiExpanded;
        m_viewMode = state.viewMode;
        m_rawMode = state.rawMode;

        if (m_rawMode)
            ui->btnRaw->setChecked(true);
        else if (m_viewMode == DiffParser::ViewMode::Unified)
            ui->btnUnified->setChecked(true);
        else
            ui->btnSideBySide->setChecked(true);

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
        {
            ui->currentFilePath->clear();
            ui->fileStatusBadge->clear();
            ui->fileStatsBadge->clear();
        }

        ui->files->verticalScrollBar()->setValue(state.fileScroll);
        m_hasAnalysis = !state.analysis.isEmpty();
        m_analysisText = state.analysis;
        ui->analysis->setPlainText(m_analysisText);
        m_expander->setExpanded(m_hasAnalysis && state.aiExpanded);

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
        m_fileScrolls[m_currentFile] = {ui->visualBrowser->horizontalScrollBar()->value(),
                                        ui->visualBrowser->verticalScrollBar()->value()};

    m_currentFile = current.data(PathRole).toString();
    const QString status = current.data(StatusRole).toString();
    const QString summary = current.data(SummaryRole).toString();

    ui->currentFilePath->setText(m_currentFile);
    ui->currentFilePath->setToolTip(m_currentFile);

    // 状态徽章与变动标签
    const auto colors = UiStyle::colors();
    ui->fileStatusBadge->setText(statusText(status));
    const auto badgeCol = statusBadgeColor(status, colors);
    ui->fileStatusBadge->setStyleSheet(QString("background-color: %1; color: %2; border-radius: 4px; padding: 2px 6px; font-weight: 500;")
                                           .arg(QColor(badgeCol.red(), badgeCol.green(), badgeCol.blue(), 36).name(QColor::HexArgb),
                                                badgeCol.name()));

    if (!summary.isEmpty() && summary != "-")
    {
        ui->fileStatsBadge->setText(summary);
        ui->fileStatsBadge->setVisible(true);
        ui->fileStatsBadge->setStyleSheet(QString("background-color: %1; color: %2; border-radius: 4px; padding: 2px 6px;")
                                              .arg(colors.sidebar.name(), colors.secondary.name()));
    }
    else
    {
        ui->fileStatsBadge->setVisible(false);
    }

    const auto scroll = m_fileScrolls.value(m_currentFile);
    const auto generation = ++m_fileGeneration;
    const auto id = m_id;
    const auto repositoryGeneration = m_repositoryGeneration;

    ui->visualBrowser->clear();
    ui->content->clear();

    m_service->diffText(id, m_diff, m_currentFile, this, [this, id, scroll, generation, repositoryGeneration](const BackupResult<QString> &reply) {
        if (!m_active || generation != m_fileGeneration || id != m_id || repositoryGeneration != m_service->repositoryGeneration(id))
            return;
        if (!reply.result.success)
            emit notification(reply.result);

        m_currentRawDiff = reply.value;
        m_currentParsedDiff = DiffParser::parse(reply.value);

        renderCurrentDiff();

        QTimer::singleShot(0, this, [this, scroll, generation] {
            if (generation == m_fileGeneration)
            {
                ui->visualBrowser->horizontalScrollBar()->setValue(scroll.x());
                ui->visualBrowser->verticalScrollBar()->setValue(scroll.y());
            }
        });
    });
}

void HomePageDiffPage::renderCurrentDiff()
{
    const auto colors = UiStyle::colors();
    const QString codeFontFamily = UiStyle::font(UiStyle::FontRole::Code).family();

    // 检查是否为图片文件
    const QString lowerPath = m_currentFile.toLower();
    const bool isImage = lowerPath.endsWith(".png") || lowerPath.endsWith(".jpg") ||
                         lowerPath.endsWith(".jpeg") || lowerPath.endsWith(".webp") ||
                         lowerPath.endsWith(".svg") || lowerPath.endsWith(".ico") ||
                         lowerPath.endsWith(".bmp");

    if (m_currentParsedDiff.isBinary && isImage)
    {
        ui->diffStack->setCurrentWidget(ui->imagePage);
        ui->imageNoticeTitle->setText(QStringLiteral("图片文件变更 · %1").arg(QFileInfo(m_currentFile).fileName()));
        ui->imageNoticeDesc->setText(QStringLiteral("此文件为图像格式，已记录当前版本变更。"));

        const QString fullPath = m_service->sourcePath(m_id) + "/" + m_currentFile;
        QPixmap pixmap(fullPath);
        if (!pixmap.isNull())
        {
            if (pixmap.width() > 400 || pixmap.height() > 300)
                pixmap = pixmap.scaled(400, 300, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            ui->imagePreview->setPixmap(pixmap);
        }
        else
        {
            ui->imagePreview->setText(QStringLiteral("(无法直接加载本地图像预览)"));
        }
        return;
    }

    if (ui->content->toPlainText() != m_currentRawDiff)
        ui->content->setPlainText(m_currentRawDiff);

    if (m_rawMode)
    {
        ui->diffStack->setCurrentWidget(ui->rawPage);
        return;
    }

    // 正常文本或常规二进制提示
    ui->diffStack->setCurrentWidget(ui->visualPage);

    DiffParser::RenderColors renderColors;
    renderColors.canvas = colors.canvas;
    renderColors.surface = colors.sidebar;
    renderColors.text = colors.text;
    renderColors.secondaryText = colors.secondary;
    renderColors.border = colors.separator;

    // 浅淡柔和背景与高亮背景
    renderColors.addedBg = QColor(colors.added.red(), colors.added.green(), colors.added.blue(), 28);
    renderColors.addedText = colors.added;
    renderColors.addedWordBg = QColor(colors.added.red(), colors.added.green(), colors.added.blue(), 72);

    renderColors.removedBg = QColor(colors.removed.red(), colors.removed.green(), colors.removed.blue(), 28);
    renderColors.removedText = colors.removed;
    renderColors.removedWordBg = QColor(colors.removed.red(), colors.removed.green(), colors.removed.blue(), 72);

    renderColors.headerBg = QColor(colors.secondary.red(), colors.secondary.green(), colors.secondary.blue(), 20);
    renderColors.emptyBg = QColor(colors.secondary.red(), colors.secondary.green(), colors.secondary.blue(), 12);

    const QString html = DiffParser::renderHtml(m_currentParsedDiff, m_viewMode, renderColors, codeFontFamily);
    ui->visualBrowser->setHtml(html);
}

void HomePageDiffPage::setViewMode(DiffParser::ViewMode mode, bool raw)
{
    m_viewMode = mode;
    m_rawMode = raw;
    renderCurrentDiff();
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
    const auto colors = UiStyle::colors();

    // 顶部 AI 卡片边框与背景样式
    const QColor cardBg = QColor(colors.sidebar.red(), colors.sidebar.green(), colors.sidebar.blue(), 180);
    ui->aiCard->setStyleSheet(QString("#aiCard { background-color: %1; border: 1px solid %2; border-radius: 12px; }")
                                  .arg(cardBg.name(QColor::HexArgb), colors.separator.name()));

    // 视图模式分段容器
    ui->viewModeGroup->setStyleSheet(QString("#viewModeGroup { background-color: %1; border: 1px solid %2; border-radius: 8px; }")
                                         .arg(colors.sidebar.name(), colors.separator.name()));

    m_highlighter->rehighlight();
    ui->files->viewport()->update();
    renderCurrentDiff();
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
    ui->aiStatus->setVisible(showAnalysis);
    ui->aiStatus->setText(m_loading ? "正在分析…" : m_analysisStatus);
    m_expander->setVisible(showAnalysis);
}

void HomePageDiffPage::updateResponsiveLayout()
{
    const bool compact = width() < 680;
    const auto orientation = compact ? Qt::Vertical : Qt::Horizontal;

    ui->filesPane->setMinimumWidth(compact ? 0 : 180);
    ui->filesPane->setMinimumHeight(compact ? 120 : 0);
    ui->filesPane->setMaximumWidth(compact ? QWIDGETSIZE_MAX : 300);
    ui->filesPane->setMaximumHeight(compact ? 160 : QWIDGETSIZE_MAX);

    if (ui->splitter->orientation() != orientation)
    {
        ui->splitter->setOrientation(orientation);
        ui->splitter->setSizes(compact ? QList<int>{140, qMax(160, height() - 140)} : QList<int>{240, qMax(240, width() - 240)});
    }
    else if (!compact && ui->splitter->sizes().value(0) < 180)
    {
        ui->splitter->setSizes({240, qMax(240, width() - 240)});
    }

    const auto analysisHeight = qMin(120, qMax(1, ui->editorPane->height() / 3));
    ui->analysisContent->setFixedHeight(analysisHeight);
    m_expander->setMaximumHeight(analysisHeight);
}

void HomePageDiffPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const auto margin = width() < 680 ? 16 : 24;
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
    m_spinner->setSpinning(false);
}

void HomePageDiffPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_active = true;
    updateLoadingState();
}
