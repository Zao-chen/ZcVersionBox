#include "../homepage.h"
#define HOMEPAGE_UI_HEADER "ui_homepage.h"
#include HOMEPAGE_UI_HEADER
#undef HOMEPAGE_UI_HEADER

#include "../../../../GlobalConstants.h"

#include "ElaMessageBar.h"

#include <QAbstractItemView>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHeaderView>
#include <QMap>
#include <QProcess>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTextEdit>
#include <QUrl>

namespace
{
//Git的空树对象，用于首个提交和“没有父提交”的版本做对比。
constexpr const char *kEmptyTreeCommit = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";

/*获取短提交*/
QString shortCommit(const QString &commit)
{
    return commit.left(qMin(7, commit.length()));
}

/*封装git操作*/
QString runGit(const QString &repoPath, const QStringList &arguments, bool *ok, QString *errorMessage = nullptr)
{
    //Diff页面只读查询 Git，不执行 checkout/reset，避免扰动备份仓库工作区状态。
    QProcess process;
    process.setWorkingDirectory(repoPath);
    process.start("git", arguments);
    if (!process.waitForStarted())
    {
        if (ok)
            *ok = false;
        if (errorMessage)
            *errorMessage = "无法启动 git，请确认 git 已安装";
        return {};
    }

    process.waitForFinished();
    const bool success = (process.exitCode() == 0);
    if (ok)
        *ok = success;
    if (!success && errorMessage)
    {
        const QString errorText = QString::fromUtf8(process.readAllStandardError()).trimmed();
        *errorMessage = errorText.isEmpty() ? "git 命令执行失败" : errorText;
    }

    return QString::fromUtf8(process.readAllStandardOutput());
}

/*状态标签*/
QString statusLabel(const QString &status)
{
    if (status.startsWith("A"))
        return "新增";
    if (status.startsWith("D"))
        return "删除";
    if (status.startsWith("M"))
        return "修改";
    if (status.startsWith("R"))
        return "重命名";
    if (status.startsWith("C"))
        return "复制";
    if (status.startsWith("T"))
        return "类型变化";
    return status.isEmpty() ? "变更" : status;
}

/*拼接html*/
QString htmlLine(const QString &line, const QString &className)
{
    return QString("<div class=\"%1\">%2</div>").arg(className, line.toHtmlEscaped());
}

/*构造html 颜色显示*/
QString buildDiffHtml(const QString &diffText)
{
    //QTextEdit使用HTML展示高亮，所有diff内容都先转义，避免文件内容被当作HTML解析。
    QString html;
    html += "<html><head><style>";
    html += "body{font-family:monospace;font-size:13px;line-height:1.45;margin:0;background:#ffffff;color:#202124;}";
    html += ".line{white-space:pre;padding:0 10px;}";
    html += ".add{white-space:pre;padding:0 10px;background:#e6ffed;color:#116329;}";
    html += ".del{white-space:pre;padding:0 10px;background:#ffebe9;color:#82071e;}";
    html += ".hunk{white-space:pre;padding:0 10px;background:#f1f3f4;color:#0550ae;}";
    html += ".meta{white-space:pre;padding:0 10px;background:#f6f8fa;color:#57606a;}";
    html += ".empty{padding:16px;color:#57606a;}";
    html += "</style></head><body>";

    if (diffText.trimmed().isEmpty())
    {
        html += "<div class=\"empty\">该文件在此版本对比中没有可显示的文本变更。</div>";
        html += "</body></html>";
        return html;
    }

    const QStringList lines = diffText.split('\n');
    for (const QString &line : lines)
    {
        if (line.startsWith("Binary files ") || line.startsWith("GIT binary patch"))
        {
            html += "<div class=\"empty\">该文件为二进制文件，暂不支持行级预览。</div>";
            continue;
        }
        if (line.startsWith("@@"))
            html += htmlLine(line, "hunk");
        else if (line.startsWith("diff --git") || line.startsWith("index ") ||
                 line.startsWith("--- ") || line.startsWith("+++ ") ||
                 line.startsWith("new file mode") || line.startsWith("deleted file mode") ||
                 line.startsWith("similarity index") || line.startsWith("rename from") ||
                 line.startsWith("rename to"))
            html += htmlLine(line, "meta");
        else if (line.startsWith("+"))
            html += htmlLine(line, "add");
        else if (line.startsWith("-"))
            html += htmlLine(line, "del");
        else
            html += htmlLine(line, "line");
    }

    html += "</body></html>";
    return html;
}

/*只读*/
QStandardItem *readOnlyItem(const QString &text)
{
    QStandardItem *item = new QStandardItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

} // namespace

/*初始化*/
void HomePage::SetupDiffPage()
{
    //Diff页只初始化一次；文件列表内容会在每次openDiff时重建。
    ui->label_DiffRange->setStyleSheet("font-size: 16px; font-weight: 600;");
    ui->textEdit_DiffContent->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    QStandardItemModel *model = new QStandardItemModel(ui->tableView_DiffFiles);
    model->setColumnCount(3);
    model->setHorizontalHeaderLabels(QStringList() << "状态" << "文件路径" << "增删统计");
    ui->tableView_DiffFiles->setModel(model);
    ui->tableView_DiffFiles->verticalHeader()->setVisible(false);
    ui->tableView_DiffFiles->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableView_DiffFiles->setSelectionMode(QAbstractItemView::SingleSelection);
    ui->tableView_DiffFiles->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->tableView_DiffFiles->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    ui->tableView_DiffFiles->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->tableView_DiffFiles->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    ui->tableView_DiffFiles->setColumnWidth(0, 70);
    ui->tableView_DiffFiles->setColumnWidth(2, 90);

    connect(ui->tableView_DiffFiles, &QTableView::clicked, this,
            [this](const QModelIndex &index)
            {
                if (!index.isValid())
                    return;
                QStandardItemModel *currentModel = qobject_cast<QStandardItemModel *>(ui->tableView_DiffFiles->model());
                if (!currentModel)
                    return;
                const QString filePath = currentModel->item(index.row(), 1)->data(Qt::UserRole).toString();
                if (!filePath.isEmpty())
                    LoadDiffFile(filePath);
            });
}

/*打开diff*/
void HomePage::openDiff(QString commitHash)
{
    // 每次打开diff都重置当前对比上下文，防止复用上一版本的文件列表。
    m_DiffRepoPath = BackupPath + "/" + m_NowFilePathWithCode;
    m_DiffNewCommit = commitHash.trimmed();
    m_DiffOldCommit.clear();
    m_DiffFilePaths.clear();

    if (m_DiffNewCommit.isEmpty() || !QDir(m_DiffRepoPath).exists())
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "打开对比失败",
                             "本地备份仓库不存在或版本号为空",
                             3000,
                             parentWidget());
        return;
    }

    bool ok = false;
    QString errorMessage;
    const QString fullNewCommit = runGit(m_DiffRepoPath,
                                         QStringList() << "rev-parse" << m_DiffNewCommit,
                                         &ok,
                                         &errorMessage)
                                      .trimmed();
    if (!ok)
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "打开对比失败",
                             errorMessage,
                             3000,
                             parentWidget());
        return;
    }
    m_DiffNewCommit = fullNewCommit;

    //普通提交对比父提交；首个提交没有父提交时，回退为空树对比。
    const QString parentCommit = runGit(m_DiffRepoPath,
                                        QStringList() << "rev-parse" << (m_DiffNewCommit + "^"),
                                        &ok)
                                     .trimmed();
    m_DiffOldCommit = ok ? parentCommit : QString::fromLatin1(kEmptyTreeCommit);

    //numstat提供增删行统计；二进制文件会返回 '-'，只展示摘要不做行级预览。
    QMap<QString, QString> numstatByPath;
    const QString numstatOutput = runGit(m_DiffRepoPath,
                                         QStringList() << "diff" << "--numstat" << m_DiffOldCommit << m_DiffNewCommit,
                                         &ok,
                                         &errorMessage);
    if (!ok)
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "打开对比失败",
                             errorMessage,
                             3000,
                             parentWidget());
        return;
    }

    const QStringList numstatLines = numstatOutput.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : numstatLines)
    {
        const QStringList parts = line.split('\t');
        if (parts.size() < 3)
            continue;
        const QString filePath = parts.mid(2).join("\t");
        const QString summary = (parts.at(0) == "-" || parts.at(1) == "-")
                                    ? "二进制"
                                    : QString("+%1 / -%2").arg(parts.at(0), parts.at(1));
        numstatByPath.insert(filePath, summary);
    }

    //name-status提供文件状态和路径，用作左侧文件列表的主数据源。
    const QString nameStatusOutput = runGit(m_DiffRepoPath,
                                            QStringList() << "diff" << "--name-status" << m_DiffOldCommit << m_DiffNewCommit,
                                            &ok,
                                            &errorMessage);
    if (!ok)
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "打开对比失败",
                             errorMessage,
                             3000,
                             parentWidget());
        return;
    }

    QStandardItemModel *model = new QStandardItemModel(ui->tableView_DiffFiles);
    model->setColumnCount(3);
    model->setHorizontalHeaderLabels(QStringList() << "状态" << "文件路径" << "增删统计");

    const QStringList statusLines = nameStatusOutput.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : statusLines)
    {
        const QStringList parts = line.split('\t');
        if (parts.size() < 2)
            continue;

        const QString status = parts.first();
        const QString filePath = parts.last();
        m_DiffFilePaths << filePath;

        QStandardItem *statusItem = readOnlyItem(statusLabel(status));
        QStandardItem *pathItem = readOnlyItem(filePath);
        QStandardItem *summaryItem = readOnlyItem(numstatByPath.value(filePath, "-"));
        pathItem->setData(filePath, Qt::UserRole);
        model->appendRow(QList<QStandardItem *>() << statusItem << pathItem << summaryItem);
    }

    ui->tableView_DiffFiles->setModel(model);
    ui->tableView_DiffFiles->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    ui->tableView_DiffFiles->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->tableView_DiffFiles->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    ui->tableView_DiffFiles->setColumnWidth(0, 70);
    ui->tableView_DiffFiles->setColumnWidth(2, 90);

    ui->label_DiffRange->setText(QString("版本对比：%1 -> %2")
                                     .arg(shortCommit(m_DiffOldCommit), shortCommit(m_DiffNewCommit)));
    const QString displayName = QFileInfo(QUrl::fromPercentEncoding(m_NowFilePathWithCode.toUtf8())).baseName();
    ui->widget_BreadcrumbBar->setBreadcrumbList(QStringList() << "备份中文件" << displayName << "版本对比");
    ui->stackedWidget->setCurrentIndex(2);

    if (m_DiffFilePaths.isEmpty())
    {
        ui->textEdit_DiffContent->setHtml("<div style=\"padding:16px;color:#57606a;\">此版本没有可显示的文件变更。</div>");
        return;
    }

    ui->tableView_DiffFiles->selectRow(0);
    LoadDiffFile(m_DiffFilePaths.first());
}

/*加载具体的Diff*/
void HomePage::LoadDiffFile(const QString &filePath)
{
    if (m_DiffRepoPath.isEmpty() || m_DiffOldCommit.isEmpty() || m_DiffNewCommit.isEmpty())
        return;

    // 文件内容按需加载，避免一次性渲染大仓库中所有变更。
    bool ok = false;
    QString errorMessage;
    const QString diffText = runGit(m_DiffRepoPath,
                                    QStringList() << "diff" << "--no-color" << "--unified=80"
                                                  << m_DiffOldCommit << m_DiffNewCommit << "--" << filePath,
                                    &ok,
                                    &errorMessage);
    if (!ok)
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "加载对比失败",
                             errorMessage,
                             3000,
                             parentWidget());
        return;
    }

    ui->textEdit_DiffContent->setHtml(buildDiffHtml(diffText));
}

/*返回*/
void HomePage::on_pushButton_DiffBack_clicked()
{
    const QString displayName = QFileInfo(QUrl::fromPercentEncoding(m_NowFilePathWithCode.toUtf8())).baseName();
    ui->widget_BreadcrumbBar->setBreadcrumbList(QStringList() << "备份中文件" << displayName);
    ui->stackedWidget->setCurrentIndex(1);
}
