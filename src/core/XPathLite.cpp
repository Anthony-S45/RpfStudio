#include "XPathLite.h"

namespace rs {

namespace {

struct Pred {
    enum Kind { Index, Attr, Child, Text } kind;
    int index = 0;
    QString name, value;
};

struct Step {
    bool descendant = false;
    bool attribute = false;
    QString name;       // "*" for any
    QList<Pred> preds;
};

QString unquote(QString v)
{
    v = v.trimmed();
    if (v.size() >= 2 && (v.startsWith('\'') || v.startsWith('"')) && v.endsWith(v[0])) v = v.mid(1, v.size() - 2);
    return v;
}

bool parse(const QString& expr, bool& absolute, QList<Step>& steps, QString* err)
{
    steps.clear();
    QString e = expr.trimmed();
    absolute = e.startsWith('/');
    int i = 0;
    while (i < e.size()) {
        Step s;
        if (e.mid(i, 2) == "//") { s.descendant = true; i += 2; }
        else if (e[i] == '/') { ++i; }
        if (i >= e.size()) break;
        int start = i;
        while (i < e.size() && e[i] != '/' && e[i] != '[') ++i;
        s.name = e.mid(start, i - start).trimmed();
        if (s.name.startsWith('@')) { s.attribute = true; s.name = s.name.mid(1); }
        while (i < e.size() && e[i] == '[') {
            int depth = 0, j = i;
            QChar quote;
            for (; j < e.size(); ++j) {
                QChar c = e[j];
                if (!quote.isNull()) { if (c == quote) quote = QChar(); continue; }
                if (c == '\'' || c == '"') { quote = c; continue; }
                if (c == '[') ++depth;
                else if (c == ']' && --depth == 0) break;
            }
            if (j >= e.size()) { if (err) *err = "unterminated predicate in " + expr; return false; }
            QString body = e.mid(i + 1, j - i - 1).trimmed();
            i = j + 1;
            Pred p;
            bool isNum = false;
            int n = body.toInt(&isNum);
            if (isNum) { p.kind = Pred::Index; p.index = n; }
            else {
                int eq = body.indexOf('=');
                if (eq < 0) { p.kind = Pred::Child; p.name = body; p.value = QString(); p.index = -1; }
                else {
                    QString lhs = body.left(eq).trimmed();
                    p.value = unquote(body.mid(eq + 1));
                    if (lhs.startsWith('@')) { p.kind = Pred::Attr; p.name = lhs.mid(1); }
                    else if (lhs == "text()" || lhs == ".") { p.kind = Pred::Text; }
                    else { p.kind = Pred::Child; p.name = lhs; }
                }
            }
            s.preds << p;
        }
        if (s.name.isEmpty()) { if (err) *err = "empty step in " + expr; return false; }
        steps << s;
    }
    return true;
}

bool matches(const QDomElement& el, const Pred& p)
{
    switch (p.kind) {
    case Pred::Attr: return el.attribute(p.name) == p.value;
    case Pred::Text: return el.text().trimmed() == p.value;
    case Pred::Child: {
        for (QDomElement c = el.firstChildElement(p.name); !c.isNull(); c = c.nextSiblingElement(p.name)) {
            if (p.index == -1) return true;  // existence test
            if (c.text().trimmed() == p.value) return true;
            if (c.attribute("value") == p.value) return true;
        }
        return false;
    }
    case Pred::Index: return true;
    }
    return false;
}

void collectDescendants(const QDomNode& n, const QString& name, QList<QDomElement>& out)
{
    for (QDomElement c = n.firstChildElement(); !c.isNull(); c = c.nextSiblingElement()) {
        if (name == "*" || c.tagName() == name) out << c;
        collectDescendants(c, name, out);
    }
}

} // namespace

QList<QDomNode> xpathSelect(const QDomNode& context, const QString& expr, QString* err)
{
    bool absolute;
    QList<Step> steps;
    QList<QDomNode> result;
    if (!parse(expr, absolute, steps, err)) return result;
    QList<QDomNode> current;
    if (absolute) current << context.ownerDocument().toDocument();
    else current << context;
    if (absolute && context.isDocument()) { current.clear(); current << context; }
    for (int si = 0; si < steps.size(); ++si) {
        const Step& s = steps[si];
        QList<QDomNode> next;
        if (s.attribute) {
            for (auto& n : current) {
                QDomElement el = n.toElement();
                if (!el.isNull() && el.hasAttribute(s.name)) next << el.attributeNode(s.name);
            }
            current = next;
            continue;
        }
        for (auto& n : current) {
            QList<QDomElement> cands;
            if (s.descendant) collectDescendants(n, s.name, cands);
            else
                for (QDomElement c = n.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
                    if (s.name == "*" || c.tagName() == s.name) cands << c;
            for (const Pred& p : s.preds) {
                QList<QDomElement> f;
                if (p.kind == Pred::Index) { if (p.index >= 1 && p.index <= cands.size()) f << cands[p.index - 1]; }
                else for (auto& c : cands) if (matches(c, p)) f << c;
                cands = f;
            }
            for (auto& c : cands) next << c;
        }
        current = next;
    }
    return current;
}

} // namespace rs
