#include "Gxt2Viewer.h"
#include "Crypto.h"

#include <QStyle>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QTableWidget>
#include <QToolBar>
#include <QVBoxLayout>

Gxt2Viewer::Gxt2Viewer(QWidget* parent) : Viewer(parent)
{
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    m_table = new QTableWidget(0, 2, this);
    m_table->setHorizontalHeaderLabels({"Hash", "Text"});
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    l->addWidget(m_table);
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText("Filter by hash or text");
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, &Gxt2Viewer::applyFilter);
    connect(m_table, &QTableWidget::itemChanged, this, [this]() { if (!m_filling) setModified(true); });
}

bool Gxt2Viewer::load(const QByteArray& d, QString* err)
{
    rs::Gxt2 g;
    if (!g.load(d, err)) return false;
    fill(g);
    return true;
}

void Gxt2Viewer::fill(const rs::Gxt2& g)
{
    m_filling = true;
    m_table->setSortingEnabled(false);
    m_table->setRowCount(int(g.entries.size()));
    for (int i = 0; i < int(g.entries.size()); ++i) {
        auto* h = new QTableWidgetItem(QStringLiteral("0x%1").arg(g.entries[size_t(i)].hash, 8, 16, QChar('0')).toUpper().replace("0X", "0x"));
        h->setFlags(h->flags() & ~Qt::ItemIsEditable);
        h->setData(Qt::UserRole, g.entries[size_t(i)].hash);
        m_table->setItem(i, 0, h);
        m_table->setItem(i, 1, new QTableWidgetItem(g.entries[size_t(i)].text));
    }
    m_table->setSortingEnabled(true);
    m_table->resizeColumnToContents(0);
    m_filling = false;
}

rs::Gxt2 Gxt2Viewer::collect() const
{
    rs::Gxt2 g;
    for (int i = 0; i < m_table->rowCount(); ++i)
        g.entries.push_back({m_table->item(i, 0)->data(Qt::UserRole).toUInt(), m_table->item(i, 1)->text()});
    return g;
}

QByteArray Gxt2Viewer::serialize(QString*) { return collect().build(); }

void Gxt2Viewer::addActions(QToolBar* tb)
{
    tb->addAction("Add entry...", this, &Gxt2Viewer::addEntry);
    tb->addAction("Remove", this, &Gxt2Viewer::removeSelected);
    tb->addAction("Export text...", this, &Gxt2Viewer::exportText);
    tb->addAction("Import text...", this, &Gxt2Viewer::importText);
    tb->addWidget(new QLabel("  "));
    m_filter->setMaximumWidth(260);
    tb->addWidget(m_filter);
}

void Gxt2Viewer::addEntry()
{
    bool ok;
    QString key = QInputDialog::getText(this, "Add entry", "Label name (e.g. MY_CAR_NAME) or hash (0x1234ABCD):", QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || key.isEmpty()) return;
    uint32_t h = key.startsWith("0x", Qt::CaseInsensitive) ? key.mid(2).toUInt(nullptr, 16) : rs::joaatLower(key);
    QString text = QInputDialog::getText(this, "Add entry", "Text:", QLineEdit::Normal, QString(), &ok);
    if (!ok) return;
    rs::Gxt2 g = collect();
    bool replaced = false;
    for (auto& e : g.entries) if (e.hash == h) { e.text = text; replaced = true; }
    if (!replaced) g.entries.push_back({h, text});
    fill(g);
    setModified(true);
}

void Gxt2Viewer::removeSelected()
{
    QSet<int> rows;
    for (auto* it : m_table->selectedItems()) rows.insert(it->row());
    QList<int> l = rows.values();
    std::sort(l.begin(), l.end(), std::greater<int>());
    for (int r : l) m_table->removeRow(r);
    if (!l.isEmpty()) setModified(true);
}

void Gxt2Viewer::exportText()
{
    QString p = QFileDialog::getSaveFileName(this, "Export text", "strings.txt", "Text (*.txt)");
    if (p.isEmpty()) return;
    QFile f(p);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(collect().toText().toUtf8());
}

void Gxt2Viewer::importText()
{
    QString p = QFileDialog::getOpenFileName(this, "Import text", QString(), "Text (*.txt);;All files (*)");
    if (p.isEmpty()) return;
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return;
    rs::Gxt2 g;
    QString err;
    if (!g.fromText(QString::fromUtf8(f.readAll()), &err)) { QMessageBox::warning(this, "Import", err); return; }
    fill(g);
    setModified(true);
}

void Gxt2Viewer::applyFilter(const QString& f)
{
    for (int i = 0; i < m_table->rowCount(); ++i) {
        bool show = f.isEmpty() || m_table->item(i, 0)->text().contains(f, Qt::CaseInsensitive) ||
                    m_table->item(i, 1)->text().contains(f, Qt::CaseInsensitive);
        m_table->setRowHidden(i, !show);
    }
}
