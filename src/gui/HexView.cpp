#include "HexView.h"

#include <QStyle>
#include <QFontDatabase>
#include <QInputDialog>
#include <QPainter>
#include <QScrollBar>
#include <QToolBar>
#include <QVBoxLayout>

HexArea::HexArea(const QByteArray& d, QWidget* parent) : QAbstractScrollArea(parent), m_d(d)
{
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    setFont(f);
    QFontMetrics fm(f);
    m_lineH = fm.height();
    m_charW = fm.horizontalAdvance('0');
    updateScroll();
}

void HexArea::updateScroll()
{
    qint64 lines = (m_d.size() + 15) / 16;
    int visible = viewport()->height() / std::max(1, m_lineH);
    verticalScrollBar()->setRange(0, int(std::max<qint64>(0, lines - visible)));
    verticalScrollBar()->setPageStep(visible);
}

void HexArea::resizeEvent(QResizeEvent* e)
{
    QAbstractScrollArea::resizeEvent(e);
    updateScroll();
}

void HexArea::gotoOffset(qint64 off) { verticalScrollBar()->setValue(int(off / 16)); }

void HexArea::paintEvent(QPaintEvent*)
{
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), palette().base());
    const int first = verticalScrollBar()->value();
    const int count = viewport()->height() / m_lineH + 1;
    const QColor offCol = palette().color(QPalette::Disabled, QPalette::Text);
    for (int i = 0; i < count; ++i) {
        qint64 line = first + i;
        qint64 off = line * 16;
        if (off >= m_d.size()) break;
        int y = i * m_lineH + m_lineH - 4;
        p.setPen(offCol);
        p.drawText(4, y, QStringLiteral("%1").arg(off, 8, 16, QChar('0')).toUpper());
        p.setPen(palette().color(QPalette::Text));
        QString hex, asc;
        for (int k = 0; k < 16; ++k) {
            if (off + k < m_d.size()) {
                uchar c = uchar(m_d[int(off + k)]);
                hex += QStringLiteral("%1 ").arg(c, 2, 16, QChar('0')).toUpper();
                asc += (c >= 32 && c < 127) ? QChar(c) : QChar('.');
            } else hex += "   ";
            if (k == 7) hex += ' ';
        }
        p.drawText(4 + m_charW * 10, y, hex);
        p.drawText(4 + m_charW * 61, y, asc);
    }
}

HexViewer::HexViewer(const QByteArray& d, QWidget* parent) : Viewer(parent)
{
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    m_area = new HexArea(d, this);
    l->addWidget(m_area);
}

void HexViewer::addActions(QToolBar* tb)
{
    tb->addAction("Go to offset...", this, [this]() {
        bool ok;
        QString s = QInputDialog::getText(this, "Go to", "Offset (hex):", QLineEdit::Normal, "0", &ok);
        if (ok) m_area->gotoOffset(s.toLongLong(nullptr, 16));
    });
}
