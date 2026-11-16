#include "TextViewer.h"
#include "Gxt2.h"

#include <QStyle>
#include <QApplication>
#include <QDomDocument>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QToolBar>
#include <QVBoxLayout>

namespace {
class XmlHighlighter : public QSyntaxHighlighter {
public:
    explicit XmlHighlighter(QTextDocument* d) : QSyntaxHighlighter(d)
    {
        bool dark = QPalette().color(QPalette::Base).lightness() < 128;
        tag.setForeground(dark ? QColor(110, 170, 255) : QColor(20, 60, 170));
        attr.setForeground(dark ? QColor(230, 160, 90) : QColor(160, 70, 0));
        value.setForeground(dark ? QColor(140, 210, 120) : QColor(20, 120, 20));
        comment.setForeground(QColor(128, 128, 128));
    }
protected:
    void highlightBlock(const QString& t) override
    {
        static const QRegularExpression rxTag("</?[A-Za-z_][\\w:.-]*|/?>"), rxAttr("\\b[A-Za-z_][\\w:.-]*(?==)"),
            rxVal("\"[^\"]*\"|'[^']*'"), rxCom("<!--.*?-->");
        for (auto* p : {&rxTag, &rxAttr, &rxVal, &rxCom}) {
            const QTextCharFormat& f = p == &rxTag ? tag : p == &rxAttr ? attr : p == &rxVal ? value : comment;
            auto it = p->globalMatch(t);
            while (it.hasNext()) { auto m = it.next(); setFormat(int(m.capturedStart()), int(m.capturedLength()), f); }
        }
    }
private:
    QTextCharFormat tag, attr, value, comment;
};
}

TextViewer::TextViewer(QWidget* parent) : Viewer(parent)
{
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    m_edit = new QPlainTextEdit(this);
    m_edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    l->addWidget(m_edit);
    connect(m_edit, &QPlainTextEdit::modificationChanged, this, [this](bool m) { setModified(m); });
    m_find = new QLineEdit;
    m_find->setPlaceholderText("Find (Enter = next, Shift+Enter = previous)");
    m_find->setClearButtonEnabled(true);
    connect(m_find, &QLineEdit::returnPressed, this, [this]() { find(QApplication::keyboardModifiers() & Qt::ShiftModifier); });
}

void TextViewer::load(const QByteArray& dIn, const QString& ext)
{
    QByteArray d = dIn;
    m_bom = d.startsWith("\xEF\xBB\xBF");
    if (m_bom) d.remove(0, 3);
    QString text;
    if (d.startsWith("\xFF\xFE")) text = QString::fromUtf16(reinterpret_cast<const char16_t*>(d.constData() + 2), (d.size() - 2) / 2);
    else text = QString::fromUtf8(d);
    m_crlf = text.contains("\r\n");
    if (m_crlf) text.replace("\r\n", "\n");
    m_edit->setPlainText(text);
    if (ext == "xml" || ext == "meta" || text.trimmed().startsWith('<')) new XmlHighlighter(m_edit->document());
    m_edit->document()->setModified(false);
}

bool TextViewer::loadRbf(const QByteArray& d, QString* err)
{
    QString xml;
    if (!rs::rbfToXml(d, xml, err)) return false;
    m_readOnly = true;
    m_edit->setReadOnly(true);
    m_edit->setPlainText(xml);
    new XmlHighlighter(m_edit->document());
    return true;
}

QByteArray TextViewer::serialize(QString* err)
{
    QString t = m_edit->toPlainText();
    if (t.trimmed().startsWith("<?xml") || t.trimmed().startsWith("<")) {
        QDomDocument doc;
        QString perr;
        int line = 0, col = 0;
        if (!doc.setContent(t, &perr, &line, &col)) {
            auto r = QMessageBox::question(this, "XML", QStringLiteral("The XML is not well-formed (line %1, column %2: %3).\nSave anyway?")
                                                        .arg(line).arg(col).arg(perr));
            if (r != QMessageBox::Yes) { if (err) *err = "Save cancelled"; return {}; }
        }
    }
    if (m_crlf) t.replace("\n", "\r\n");
    QByteArray out = t.toUtf8();
    if (m_bom) out.prepend("\xEF\xBB\xBF");
    return out;
}

void TextViewer::markSaved()
{
    m_edit->document()->setModified(false);
    setModified(false);
}

void TextViewer::addActions(QToolBar* tb)
{
    tb->addWidget(new QLabel(" Find: "));
    m_find->setMaximumWidth(300);
    tb->addWidget(m_find);
    if (m_readOnly) tb->addWidget(new QLabel("  (binary XML, read-only view)"));
}

void TextViewer::find(bool backwards)
{
    QString s = m_find->text();
    if (s.isEmpty()) return;
    QTextDocument::FindFlags f;
    if (backwards) f |= QTextDocument::FindBackward;
    if (!m_edit->find(s, f)) {
        QTextCursor c = m_edit->textCursor();
        c.movePosition(backwards ? QTextCursor::End : QTextCursor::Start);
        m_edit->setTextCursor(c);
        m_edit->find(s, f);
    }
}
