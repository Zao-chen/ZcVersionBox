#include "diffPage.h"
#include "ui_diffPage.h"
#include <QApplication>
#include <QFontDatabase>
#include <QHeaderView>
#include <QPointer>
#include <oclero/qlementine/style/QlementineStyle.hpp>

namespace
{
QString statusText(const QString &status)
{
    const QMap<QChar, QString> labels{{'A', "新增"}, {'D', "删除"}, {'M', "修改"}, {'R', "重命名"}, {'C', "复制"}, {'T', "类型变化"}};
    return status.isEmpty() ? QStringLiteral("变更") : labels.value(status.front(), status);
}
} // namespace
DiffPage::DiffPage(BackupService *service, SettingsService *settings, AiGateway *gateway, QWidget *parent)
    : QWidget(parent), ui(new Ui::DiffPage), m_service(service), m_settings(settings), m_gateway(gateway)
{
    ui->setupUi(this);
    ui->files->setModel(&m_model);
    ui->files->verticalHeader()->hide();
    ui->files->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->files->setSelectionMode(QAbstractItemView::SingleSelection);
    ui->files->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->content->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    ui->splitter->setStretchFactor(0, 1);
    ui->splitter->setStretchFactor(1, 2);
    connect(ui->files->selectionModel(), &QItemSelectionModel::currentRowChanged, this, &DiffPage::loadFile);
    connect(ui->backButton, &QPushButton::clicked, this, [this]
            {
                emit navigate({PageId::History, m_id});
            });
    connect(ui->analyzeButton, &QPushButton::clicked, this, &DiffPage::analyze);
    connect(service, &BackupService::repositoryInvalidated, this, [this](const QString &id)
            {
                if (id == m_id)
                {
                    deactivate();
                    ui->analyzeButton->setEnabled(false);
                    ui->analysis->clear();
                }
            });
}
DiffPage::~DiffPage() = default;
void DiffPage::deactivate()
{
    ++m_generation;
    ui->analyzeButton->setEnabled(true);
}
void DiffPage::setRevision(const QString &id, const QString &commit)
{
    deactivate();
    m_id = id;
    m_text.clear();
    ui->analysis->clear();
    ui->analysisGroup->hide();
    m_model.clear();
    const auto result = m_service->diff(id, commit, m_diff);
    m_model.setHorizontalHeaderLabels({"状态", "文件路径", "增删统计"});
    if (!result.success)
    {
        emit notification(result);
        ui->rangeLabel->setText("无法打开版本对比");
        ui->analyzeButton->setEnabled(false);
        refreshTheme();
        return;
    }
    ui->rangeLabel->setText(QString("版本对比：%1 → %2").arg(m_diff.oldCommit.left(7), m_diff.newCommit.left(7)));
    for (const auto &file : m_diff.files)
    {
        auto *path = new QStandardItem(file.path);
        path->setData(file.path, Qt::UserRole);
        m_model.appendRow({new QStandardItem(statusText(file.status)), path, new QStandardItem(file.summary)});
    }
    ui->files->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ui->files->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->files->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    if (m_model.rowCount())
        ui->files->selectRow(0);
    else
        refreshTheme();
}
void DiffPage::loadFile()
{
    const auto current = ui->files->currentIndex();
    if (!current.isValid())
        return;
    const auto path = m_model.index(current.row(), 1).data(Qt::UserRole).toString();
    const auto result = m_service->diffText(m_id, m_diff, path, m_text);
    if (!result.success)
        emit notification(result);
    refreshTheme();
}
void DiffPage::refreshTheme()
{
    const auto *style = qobject_cast<oclero::qlementine::QlementineStyle *>(qApp->style());
    const auto text = palette().color(QPalette::Text).name();
    const auto base = palette().color(QPalette::Base).name();
    const auto add = style ? style->theme().statusColorSuccess.name() : text;
    const auto del = style ? style->theme().statusColorError.name() : text;
    const auto meta = style ? style->theme().statusColorInfo.name() : text;
    QString html = QString("<html><body style='background:%1;color:%2;font-family:monospace;'>").arg(base, text);
    if (m_text.trimmed().isEmpty())
        html += "<p>此版本没有可显示的文本变更。</p>";
    else
        for (const auto &line : m_text.split('\n'))
        {
            auto color = text;
            if (line.startsWith("@@") || line.startsWith("diff --git") || line.startsWith("index "))
                color = meta;
            else if (line.startsWith('+'))
                color = add;
            else if (line.startsWith('-'))
                color = del;
            const auto content = line.startsWith("Binary files ") || line.startsWith("GIT binary patch") ? QStringLiteral("该文件为二进制文件，暂不支持行级预览。") : line;
            html += QString("<pre style='margin:0;color:%1;'>%2</pre>").arg(color, content.toHtmlEscaped());
        }
    ui->content->setHtml(html + "</body></html>");
}
void DiffPage::analyze()
{
    QString diff;
    auto result = m_service->diffText(m_id, m_diff, {}, diff);
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
    ui->analyzeButton->setEnabled(false);
    ui->analysisGroup->show();
    ui->analysis->setPlainText("AI 正在分析版本对比，请稍候…");
    QPointer<DiffPage> guard(this);
    m_gateway->summarize(config, diff, this, [guard, generation, id, repositoryGeneration](const QString &summary, const QString &error)
                         {
                             if (!guard || generation != guard->m_generation || id != guard->m_id || !guard->m_service->contains(id) || repositoryGeneration != guard->m_service->repositoryGeneration(id))
                                 return;
                             guard->ui->analyzeButton->setEnabled(true);
                             guard->ui->analysis->setPlainText(error.isEmpty() ? summary : error);
                             emit guard->notification(error.isEmpty() ? OperationResult::ok("AI 分析完成", "已生成当前版本对比分析") : OperationResult::fail("AI 分析失败", error));
                         });
}
