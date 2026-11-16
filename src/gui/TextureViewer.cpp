#include "TextureViewer.h"

#include <QStyle>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace rs;

TextureViewer::TextureViewer(QWidget* parent) : Viewer(parent)
{
    auto* l = new QHBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    auto* split = new QSplitter(this);
    l->addWidget(split);
    m_list = new QTreeWidget;
    m_list->setHeaderLabels({"Name", "Size", "Format", "Mips"});
    m_list->setRootIsDecorated(false);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setIconSize(QSize(32, 32));
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < 4; ++c) m_list->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    split->addWidget(m_list);

    auto* right = new QWidget;
    auto* rl = new QVBoxLayout(right);
    rl->setContentsMargins(4, 4, 4, 4);
    auto* bar = new QHBoxLayout;
    m_zoom = new QComboBox;
    m_zoom->addItems({"Fit", "25%", "50%", "100%", "200%", "400%"});
    m_zoom->setCurrentIndex(0);
    m_mip = new QSpinBox;
    m_mip->setPrefix("Mip ");
    m_alpha = new QCheckBox("Alpha");
    m_alpha->setChecked(true);
    bar->addWidget(new QLabel("Zoom:"));
    bar->addWidget(m_zoom);
    bar->addWidget(m_mip);
    bar->addWidget(m_alpha);
    bar->addStretch();
    rl->addLayout(bar);
    m_scroll = new QScrollArea;
    m_scroll->setAlignment(Qt::AlignCenter);
    m_image = new QLabel;
    m_image->setAlignment(Qt::AlignCenter);
    m_scroll->setWidget(m_image);
    m_scroll->setWidgetResizable(false);
    rl->addWidget(m_scroll, 1);
    m_info = new QLabel;
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rl->addWidget(m_info);
    split->addWidget(right);
    split->setStretchFactor(1, 3);
    split->setSizes({300, 700});

    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &TextureViewer::showCurrent);
    connect(m_mip, qOverload<int>(&QSpinBox::valueChanged), this, &TextureViewer::showCurrent);
    connect(m_zoom, qOverload<int>(&QComboBox::currentIndexChanged), this, &TextureViewer::showCurrent);
    connect(m_alpha, &QCheckBox::toggled, this, &TextureViewer::showCurrent);
}

bool TextureViewer::load(const QByteArray& ytd, QString* err)
{
    if (!m_dict.load(ytd, err)) return false;
    refreshList();
    return true;
}

void TextureViewer::setDictionary(const TextureDictionary& d, bool editable)
{
    m_dict = d;
    m_editable = editable;
    refreshList();
}

QByteArray TextureViewer::serialize(QString* err) { return m_dict.build(err); }

void TextureViewer::addActions(QToolBar* tb)
{
    tb->addAction("Export DDS...", this, [this]() { exportSelected(false); });
    tb->addAction("Export PNG...", this, [this]() { exportSelected(true); });
    tb->addAction("Export all...", this, &TextureViewer::exportAll);
    if (!m_editable) return;
    tb->addSeparator();
    tb->addAction("Replace...", this, &TextureViewer::replaceSelected);
    tb->addAction("Add...", this, &TextureViewer::addTexture);
    tb->addAction("Rename...", this, &TextureViewer::renameSelected);
    tb->addAction("Remove", this, &TextureViewer::removeSelected);
}

int TextureViewer::current() const
{
    auto* it = m_list->currentItem();
    return it ? it->data(0, Qt::UserRole).toInt() : -1;
}

void TextureViewer::refreshList()
{
    int keep = current();
    m_list->clear();
    for (size_t i = 0; i < m_dict.textures.size(); ++i) {
        const auto& t = m_dict.textures[i];
        auto* it = new QTreeWidgetItem(m_list);
        it->setText(0, t.name);
        it->setText(1, QStringLiteral("%1 x %2").arg(t.image.width).arg(t.image.height));
        it->setText(2, texFormatName(t.image.format));
        it->setText(3, QString::number(t.image.levels));
        it->setData(0, Qt::UserRole, int(i));
        // thumbnail from the smallest mip that is at least 32px
        int lvl = 0;
        while (lvl + 1 < t.image.levels && std::max(t.image.width >> (lvl + 1), t.image.height >> (lvl + 1)) >= 32) ++lvl;
        QImage img = decodeTexture(t.image, lvl);
        if (!img.isNull()) it->setIcon(0, QPixmap::fromImage(img.scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    }
    if (m_list->topLevelItemCount()) {
        int sel = (keep >= 0 && keep < m_list->topLevelItemCount()) ? keep : 0;
        m_list->setCurrentItem(m_list->topLevelItem(sel));
    }
    showCurrent();
}

static QPixmap checker(const QImage& img, bool alpha)
{
    QPixmap pm(img.size());
    QPainter p(&pm);
    const int s = 8;
    for (int y = 0; y < img.height(); y += s)
        for (int x = 0; x < img.width(); x += s)
            p.fillRect(x, y, s, s, ((x / s + y / s) & 1) ? QColor(200, 200, 200) : QColor(255, 255, 255));
    if (alpha) p.drawImage(0, 0, img);
    else p.drawImage(0, 0, img.convertToFormat(QImage::Format_RGB888));
    return pm;
}

void TextureViewer::showCurrent()
{
    int i = current();
    if (i < 0 || i >= int(m_dict.textures.size())) { m_image->clear(); m_info->clear(); return; }
    const auto& t = m_dict.textures[size_t(i)];
    m_mip->blockSignals(true);
    m_mip->setRange(0, std::max(0, t.image.levels - 1));
    m_mip->blockSignals(false);
    int lvl = m_mip->value();
    QImage img = decodeTexture(t.image, lvl);
    if (img.isNull()) {
        m_image->setText("Cannot decode format " + texFormatName(t.image.format));
        m_image->adjustSize();
    } else {
        QPixmap pm = checker(img, m_alpha->isChecked());
        QSize vs = m_scroll->viewport()->size() - QSize(4, 4);
        static const double zooms[] = {0, 0.25, 0.5, 1, 2, 4};
        double z = zooms[m_zoom->currentIndex()];
        if (z == 0) {
            if (pm.width() > vs.width() || pm.height() > vs.height()) pm = pm.scaled(vs, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            else if (pm.width() * 2 <= vs.width() && pm.height() * 2 <= vs.height())
                pm = pm.scaled(pm.size() * std::min(vs.width() / pm.width(), vs.height() / pm.height()), Qt::KeepAspectRatio, Qt::FastTransformation);
        } else {
            pm = pm.scaled(pm.size() * z, Qt::KeepAspectRatio, z < 1 ? Qt::SmoothTransformation : Qt::FastTransformation);
        }
        m_image->setPixmap(pm);
        m_image->resize(pm.size());
    }
    m_info->setText(QStringLiteral("%1   %2 x %3 (mip %4: %5 x %6)   %7   %8 mip levels   %9 bytes")
                        .arg(t.name).arg(t.image.width).arg(t.image.height).arg(lvl)
                        .arg(std::max(1, t.image.width >> lvl)).arg(std::max(1, t.image.height >> lvl))
                        .arg(texFormatName(t.image.format)).arg(t.image.levels).arg(t.image.data.size()));
}

void TextureViewer::exportSelected(bool png)
{
    auto items = m_list->selectedItems();
    if (items.isEmpty()) return;
    QString dir;
    if (items.size() == 1) {
        const auto& t = m_dict.textures[size_t(items[0]->data(0, Qt::UserRole).toInt())];
        QString p = QFileDialog::getSaveFileName(this, "Export texture", t.name + (png ? ".png" : ".dds"),
                                                 png ? "PNG image (*.png)" : "DirectDraw Surface (*.dds)");
        if (p.isEmpty()) return;
        bool ok = png ? decodeTexture(t.image).save(p) : [&]() { QFile f(p); return f.open(QIODevice::WriteOnly) && f.write(writeDds(t.image)) > 0; }();
        if (!ok) QMessageBox::warning(this, "Export", "Cannot write " + p);
        return;
    }
    dir = QFileDialog::getExistingDirectory(this, "Export textures to");
    if (dir.isEmpty()) return;
    for (auto* it : items) {
        const auto& t = m_dict.textures[size_t(it->data(0, Qt::UserRole).toInt())];
        QString p = QDir(dir).filePath(t.name + (png ? ".png" : ".dds"));
        if (png) decodeTexture(t.image).save(p);
        else { QFile f(p); if (f.open(QIODevice::WriteOnly)) f.write(writeDds(t.image)); }
    }
}

void TextureViewer::exportAll()
{
    m_list->selectAll();
    if (m_dict.textures.size() == 1) {
        QString dir = QFileDialog::getExistingDirectory(this, "Export textures to");
        if (dir.isEmpty()) return;
        const auto& t = m_dict.textures[0];
        QFile f(QDir(dir).filePath(t.name + ".dds"));
        if (f.open(QIODevice::WriteOnly)) f.write(writeDds(t.image));
        return;
    }
    exportSelected(false);
}

bool TextureViewer::importImage(QWidget* parent, const QString& path, TextureImage& out, uint32_t preferred, QString* err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (err) *err = "Cannot read " + path; return false; }
    QByteArray d = f.readAll();
    if (path.endsWith(".dds", Qt::CaseInsensitive)) return readDds(d, out, err);
    QImage img;
    if (!img.loadFromData(d)) { if (err) *err = "Unsupported image file " + path; return false; }
    // ask for format / mipmaps
    QDialog dlg(parent);
    dlg.setWindowTitle("Import " + QFileInfo(path).fileName());
    auto* form = new QFormLayout(&dlg);
    auto* fmt = new QComboBox;
    fmt->addItem("DXT1 (no/1-bit alpha)", uint32_t(TexDXT1));
    fmt->addItem("DXT5 (alpha)", uint32_t(TexDXT5));
    fmt->addItem("A8R8G8B8 (uncompressed)", uint32_t(TexA8R8G8B8));
    int pi = fmt->findData(preferred);
    fmt->setCurrentIndex(pi >= 0 ? pi : (img.hasAlphaChannel() ? 1 : 0));
    auto* mips = new QCheckBox("Generate mipmaps");
    mips->setChecked(true);
    form->addRow(QStringLiteral("%1 x %2").arg(img.width()).arg(img.height()), new QLabel);
    form->addRow("Format:", fmt);
    form->addRow(mips);
    if ((img.width() & (img.width() - 1)) || (img.height() & (img.height() - 1)))
        form->addRow(new QLabel("Note: the size is not a power of two; the game prefers power-of-two textures."));
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) { if (err) err->clear(); return false; }
    out = encodeImage(img, fmt->currentData().toUInt(), mips->isChecked());
    return true;
}

void TextureViewer::replaceSelected()
{
    int i = current();
    if (i < 0) return;
    auto& t = m_dict.textures[size_t(i)];
    QString p = QFileDialog::getOpenFileName(this, "Replace " + t.name, QString(), "Images (*.dds *.png *.jpg *.jpeg *.bmp *.tga);;All files (*)");
    if (p.isEmpty()) return;
    TextureImage img;
    QString err;
    if (!importImage(this, p, img, t.image.format, &err)) { if (!err.isEmpty()) QMessageBox::warning(this, "Replace", err); return; }
    t.image = img;
    setModified(true);
    refreshList();
}

void TextureViewer::addTexture()
{
    QStringList ps = QFileDialog::getOpenFileNames(this, "Add textures", QString(), "Images (*.dds *.png *.jpg *.jpeg *.bmp *.tga);;All files (*)");
    for (const QString& p : ps) {
        TextureImage img;
        QString err;
        if (!importImage(this, p, img, TexDXT5, &err)) { if (!err.isEmpty()) QMessageBox::warning(this, "Add", err); continue; }
        m_dict.setTexture(QFileInfo(p).completeBaseName(), img);
        setModified(true);
    }
    refreshList();
}

void TextureViewer::removeSelected()
{
    auto items = m_list->selectedItems();
    if (items.isEmpty()) return;
    if (QMessageBox::question(this, "Remove", QStringLiteral("Remove %1 texture(s)?").arg(items.size())) != QMessageBox::Yes) return;
    QStringList names;
    for (auto* it : items) names << it->text(0);
    for (auto& n : names) m_dict.remove(n);
    setModified(true);
    refreshList();
}

void TextureViewer::renameSelected()
{
    int i = current();
    if (i < 0) return;
    bool ok;
    QString n = QInputDialog::getText(this, "Rename", "New name:", QLineEdit::Normal, m_dict.textures[size_t(i)].name, &ok).trimmed();
    if (!ok || n.isEmpty()) return;
    if (m_dict.find(n) >= 0 && m_dict.find(n) != i) { QMessageBox::warning(this, "Rename", "A texture with that name exists."); return; }
    m_dict.textures[size_t(i)].name = n;
    setModified(true);
    refreshList();
}
