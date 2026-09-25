#include "homepage.h"
#include "ui_homepage.h"
#include "windows/mainwindow_child/homepage/pages/homepage_page_trackfiles.h"
#include "windows/mainwindow_child/homepage/trackfiles/homepagechild_trackfile.h"
#include "windows/mainwindow_presentation.h"
#include <QAction>
#include <QResizeEvent>

HomePage::HomePage(BackupService *service, QWidget *parent, BackupListModel *model, BackupUiActions *actions)
    : QWidget(parent), ui(new Ui::HomePage), m_service(service), m_model(model ? model : new BackupListModel(this)),
      m_filter(new BackupFilterModel(this)), m_actions(actions ? actions : new BackupUiActions(service, this)), m_ownsModel(!model)
{
    ui->setupUi(this);
    m_filter->setSourceModel(m_model);
    m_filter->sort(0);
    ui->backupList->setModel(m_filter);
    UiStyle::flatView(ui->backupList);
    auto *delegate = new BackupItemDelegate(ui->backupList, false);
    ui->backupList->setItemDelegate(delegate);
    ui->backupList->setAccessibleName("全部备份");
    ui->emptyAddButton->setMenu(m_actions->addMenu());
    ui->emptyAddButton->setFlat(true);
    UiStyle::text(ui->countLabel, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->emptyLabel, UiStyle::FontRole::Object);
    UiStyle::text(ui->emptyHint, UiStyle::FontRole::Body, true);
    connect(delegate, &BackupItemDelegate::menuRequested, this, [this](const QString &id, const QPoint &position)
            { m_actions->showObjectMenu(id, position, false); });
    const auto open = [this](const QModelIndex &index)
    {
        if (index.isValid())
            emit navigate({PageId::History, index.data(BackupListModel::IdRole).toString()});
    };
    connect(ui->backupList, &QListView::clicked, this, open);
    connect(ui->backupList, &QListView::activated, this, open);
    connect(ui->filter, &QLineEdit::textChanged, this, [this](const QString &text)
            {
        m_filter->setFilterFixedString(text);
        updateEmptyState(); });
    connect(m_filter, &QAbstractItemModel::rowsInserted, this, &HomePage::updateEmptyState);
    connect(m_filter, &QAbstractItemModel::rowsRemoved, this, &HomePage::updateEmptyState);
    connect(m_filter, &QAbstractItemModel::modelReset, this, &HomePage::updateEmptyState);
    if (m_ownsModel)
        connect(service, &BackupService::trackedItemsChanged, this, &HomePage::refresh);
    if (!actions)
    {
        connect(m_actions, &BackupUiActions::notification, this, &HomePage::notification);
        connect(m_actions, &BackupUiActions::navigate, this, &HomePage::navigate);
    }
    refresh();
}
HomePage::~HomePage() = default;
QList<QAction *> HomePage::toolbarActions() const { return {m_actions->addAction()}; }
void HomePage::refresh()
{
    if (m_ownsModel)
        m_model->setItems(m_service->trackedItems());
    updateEmptyState();
}
void HomePage::updateEmptyState()
{
    const bool empty = m_model->rowCount() == 0;
    ui->countLabel->setText(ui->filter->text().isEmpty() ? QString("%1 个备份").arg(m_model->rowCount())
                                                         : QString("%1 / %2 个备份").arg(m_filter->rowCount()).arg(m_model->rowCount()));
    ui->listStack->setCurrentWidget(m_filter->rowCount() ? ui->listPage : ui->emptyPage);
    ui->emptyLabel->setText(empty ? "还没有备份" : "没有匹配的备份");
    ui->emptyHint->setText(empty ? "添加文件或文件夹，开始自动保存历史版本。" : "试试其他名称或路径，或清除筛选条件。");
    ui->emptyAddButton->setVisible(empty);
}
void HomePage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const auto margin = width() < 640 ? 16 : 24;
    ui->pageLayout->setContentsMargins(margin, 12, margin, 16);
    ui->filter->setFixedWidth(width() < 640 ? 200 : 260);
}
