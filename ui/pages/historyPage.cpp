#include "historyPage.h"
#include "ui/components/presentation.h"
#include "ui_historyPage.h"
#include <QHeaderView>
#include <QScopedValueRollback>
#include <QTimer>

HistoryPage::HistoryPage(BackupService *service, QWidget *parent) : QWidget(parent), ui(new Ui::HistoryPage), m_service(service)
{
    ui->setupUi(this);
    ui->table->setModel(&m_model);
    ui->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->table->setSelectionMode(QAbstractItemView::SingleSelection);
    ui->table->verticalHeader()->hide();
    connect(ui->table->selectionModel(), &QItemSelectionModel::selectionChanged, this, &HistoryPage::updateActions);
    connect(ui->refreshButton, &QPushButton::clicked, this, &HistoryPage::refresh);
    connect(ui->dashboardButton, &QPushButton::clicked, this, [this]
            {
                emit navigate({PageId::Dashboard, m_id});
            });
    connect(ui->diffButton, &QPushButton::clicked, this, [this]
            {
                if (!selectedCommit().isEmpty())
                    emit navigate({PageId::Diff, m_id, selectedCommit()});
            });
    connect(ui->previewButton, &QPushButton::clicked, this, [this]
            {
                const auto result = m_service->preview(m_id, selectedCommit());
                emit notification(result);
                if (result.success && !result.path.isEmpty())
                    openLocalPath(this, result.path);
            });
    connect(ui->restoreButton, &QPushButton::clicked, this, [this]
            {
                emit notification(m_service->restore(m_id, selectedCommit()));
            });
    connect(&m_model, &QStandardItemModel::itemChanged, this, [this](QStandardItem *item)
            {
                if (m_loading || item->column() != 1)
                    return;
                const auto hash = m_model.item(item->row(), 0)->text();
                const auto message = item->text();
                const auto result = m_service->editMessage(m_id, hash, message);
                emit notification(result);
                // Let the editor finish committing before replacing its item.
                QTimer::singleShot(0, this, &HistoryPage::refresh);
            });
}
HistoryPage::~HistoryPage() = default;
void HistoryPage::setBackup(const QString &id)
{
    m_id = id;
    refresh();
}
QString HistoryPage::selectedCommit() const
{
    const auto index = ui->table->currentIndex();
    return index.isValid() ? m_model.index(index.row(), 0).data().toString() : QString();
}
void HistoryPage::updateActions()
{
    const bool enabled = !selectedCommit().isEmpty();
    ui->previewButton->setEnabled(enabled);
    ui->restoreButton->setEnabled(enabled);
    ui->diffButton->setEnabled(enabled);
}
void HistoryPage::refresh()
{
    const auto selected = selectedCommit();
    QVector<Revision> revisions;
    const auto result = m_service->history(m_id, revisions);
    // Views need reset/insert signals; only suppress the editing callback.
    QScopedValueRollback<bool> loading(m_loading, true);
    m_model.clear();
    m_model.setHorizontalHeaderLabels({"版本号", "说明"});
    if (!result.success)
        emit notification(result);
    int selectedRow = 0;
    for (const auto &revision : revisions)
    {
        auto *hash = new QStandardItem(revision.hash);
        hash->setEditable(false);
        auto *message = new QStandardItem(revision.message);
        m_model.appendRow({hash, message});
        if (revision.hash == selected)
            selectedRow = m_model.rowCount() - 1;
    }
    ui->table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ui->table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->emptyLabel->setVisible(revisions.isEmpty());
    if (!revisions.isEmpty())
        ui->table->selectRow(selectedRow);
    updateActions();
}
