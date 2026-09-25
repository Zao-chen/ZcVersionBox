#include "backupsPage.h"
#include "ui/components/presentation.h"
#include "ui_backupsPage.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <oclero/qlementine/widgets/Label.hpp>

BackupsPage::BackupsPage(BackupService *service, QWidget *parent) : QWidget(parent), ui(new Ui::BackupsPage), m_service(service)
{
    ui->setupUi(this);
    ui->addLocalButton->setDefault(true);
    connect(ui->addLocalButton, &QPushButton::clicked, this, &BackupsPage::addLocal);
    connect(ui->importButton, &QPushButton::clicked, this, &BackupsPage::importRemote);
    connect(service, &BackupService::trackedItemsChanged, this, &BackupsPage::refresh);
    refresh();
}
BackupsPage::~BackupsPage() = default;
void BackupsPage::refresh()
{
    while (auto *item = ui->cardsLayout->takeAt(0))
    {
        if (item->widget())
        {
            item->widget()->hide();
            item->widget()->deleteLater();
        }
        delete item;
    }
    const auto items = m_service->trackedItems();
    ui->emptyLabel->setVisible(items.isEmpty());
    for (const auto &item : items)
    {
        auto *card = new QGroupBox(ui->cards);
        card->setAccessibleName(item.name);
        auto *layout = new QVBoxLayout(card);
        auto *name = new oclero::qlementine::Label(item.name, oclero::qlementine::TextRole::H5, card);
        name->setTextFormat(Qt::PlainText);
        name->setToolTip(item.sourcePath);
        name->setWordWrap(true);
        auto namePolicy = name->sizePolicy();
        namePolicy.setHorizontalPolicy(QSizePolicy::Ignored);
        name->setSizePolicy(namePolicy);
        layout->addWidget(name);
        auto *path = new QLabel(item.sourcePath, card);
        path->setTextFormat(Qt::PlainText);
        path->setToolTip(item.sourcePath);
        path->setWordWrap(true);
        auto pathPolicy = path->sizePolicy();
        pathPolicy.setHorizontalPolicy(QSizePolicy::Ignored);
        path->setMinimumWidth(0);
        path->setSizePolicy(pathPolicy);
        layout->addWidget(path);
        auto *row = new QHBoxLayout;
        row->addStretch();
        auto *open = new QPushButton("打开源文件", card);
        auto *history = new QPushButton("历史版本", card);
        auto *dashboard = new QPushButton("仪表盘", card);
        open->setObjectName("openSourceButton");
        history->setObjectName("historyButton");
        dashboard->setObjectName("dashboardButton");
        row->addWidget(open);
        row->addWidget(history);
        row->addWidget(dashboard);
        layout->addLayout(row);
        connect(open, &QPushButton::clicked, this, [this, item]
                {
                    openLocalPath(this, item.sourcePath);
                });
        connect(history, &QPushButton::clicked, this, [this, item]
                {
                    emit navigate({PageId::History, item.id});
                });
        connect(dashboard, &QPushButton::clicked, this, [this, item]
                {
                    emit navigate({PageId::Dashboard, item.id});
                });
        ui->cardsLayout->addWidget(card);
    }
    ui->cardsLayout->addStretch();
}
void BackupsPage::addLocal()
{
    QDialog dialog(this);
    dialog.setWindowTitle("备份本地文件");
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("你要添加单个文件，还是整个文件夹？", &dialog));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    auto *file = buttons->addButton("文件", QDialogButtonBox::ActionRole);
    auto *folder = buttons->addButton("文件夹", QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);
    bool directory = false;
    connect(file, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(folder, &QPushButton::clicked, &dialog, [&]
            {
                directory = true;
                dialog.accept();
            });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto path = directory ? QFileDialog::getExistingDirectory(this, "选择文件夹", QDir::homePath(), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks)
                                : QFileDialog::getOpenFileName(this, "选择文件", QDir::homePath(), "All Files (*.*)");
    if (path.isEmpty())
        return;
    emit notification(m_service->addLocal(path));
}
void BackupsPage::importRemote()
{
    QDialog dialog(this);
    dialog.setWindowTitle("从云端导入备份");
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("请输入云端仓库地址", &dialog));
    auto *url = new QLineEdit(&dialog);
    url->setPlaceholderText("https://github.com/user/repo.git");
    layout->addWidget(url);
    auto *status = new QLabel(&dialog);
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText("导入");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    auto *check = buttons->addButton("检查链接", QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);
    connect(check, &QPushButton::clicked, &dialog, [&]
            {
                const auto result = m_service->checkRemote(url->text());
                status->setText(result.title + "：" + result.message);
            });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(520, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto prepared = m_service->prepareImport(url->text());
    if (!prepared.result.success)
    {
        emit notification(prepared.result);
        return;
    }
    QString target;
    if (!prepared.directory)
        target = QFileDialog::getSaveFileName(this, "选择追踪文件位置", QDir::home().filePath(prepared.entryName), "All Files (*.*)");
    else
    {
        auto folder = QFileDialog::getExistingDirectory(this, "选择追踪文件夹位置", QDir::homePath(), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!folder.isEmpty())
            target = QDir(folder).filePath(prepared.entryName);
    }
    if (target.isEmpty())
    {
        m_service->cancelImport(prepared.temporaryRepo);
        return;
    }
    const auto result = m_service->finishImport(prepared.temporaryRepo, target);
    if (!result.success)
        m_service->cancelImport(prepared.temporaryRepo);
    emit notification(result);
}
