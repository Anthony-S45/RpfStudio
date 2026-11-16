#include "Gxt2.h"
#include "BinaryIO.h"
#include "Crypto.h"

#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <vector>

namespace rs {

static constexpr uint32_t Gxt2Magic = 1196971058;  // "2TXG"

bool Gxt2::load(const QByteArray& data, QString* err)
{
    entries.clear();
    try {
        Reader r(data);
        if (r.u32() != Gxt2Magic) throw FormatError("not a GXT2 file");
        uint32_t count = r.u32();
        if (count > 1000000) throw FormatError("corrupt GXT2 entry count");
        std::vector<std::pair<uint32_t, uint32_t>> idx(count);
        for (auto& e : idx) { e.first = r.u32(); e.second = r.u32(); }
        if (r.u32() != Gxt2Magic) throw FormatError("missing second GXT2 marker");
        uint32_t end = r.u32();
        for (auto& e : idx) {
            if (e.second >= uint32_t(data.size())) throw FormatError("GXT2 string offset out of range");
            int stop = data.indexOf('\0', int(e.second));
            if (stop < 0 || uint32_t(stop) > std::max<uint32_t>(end, uint32_t(data.size()))) stop = data.size();
            entries.push_back({e.first, QString::fromUtf8(data.constData() + e.second, stop - int(e.second))});
        }
    } catch (const FormatError& e) {
        if (err) *err = e.qmessage();
        return false;
    }
    return true;
}

QByteArray Gxt2::build() const
{
    std::vector<Gxt2Entry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(), [](const Gxt2Entry& a, const Gxt2Entry& b) { return a.hash < b.hash; });
    Writer w;
    w.u32(Gxt2Magic);
    w.u32(uint32_t(sorted.size()));
    uint32_t off = uint32_t(8 + sorted.size() * 8 + 8);
    std::vector<QByteArray> strs;
    for (auto& e : sorted) {
        QByteArray s = e.text.toUtf8();
        w.u32(e.hash);
        w.u32(off);
        off += uint32_t(s.size() + 1);
        strs.push_back(s);
    }
    w.u32(Gxt2Magic);
    w.u32(off);
    for (auto& s : strs) w.cstr(s);
    return w.data;
}

QString Gxt2::toText() const
{
    QStringList lines;
    std::vector<Gxt2Entry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(), [](const Gxt2Entry& a, const Gxt2Entry& b) { return a.hash < b.hash; });
    for (auto& e : sorted) {
        QString t = e.text;
        t.replace("\\", "\\\\").replace("\n", "\\n");
        lines << QStringLiteral("0x%1 = %2").arg(e.hash, 8, 16, QChar('0')).arg(t);
    }
    return lines.join('\n') + '\n';
}

bool Gxt2::fromText(const QString& text, QString* err)
{
    std::vector<Gxt2Entry> out;
    int lineNo = 0;
    for (const QString& rawLine : text.split('\n')) {
        ++lineNo;
        QString line = rawLine;
        if (line.endsWith('\r')) line.chop(1);
        if (line.trimmed().isEmpty() || line.trimmed().startsWith('#')) continue;
        int eq = line.indexOf('=');
        if (eq < 0) { if (err) *err = QStringLiteral("line %1: expected 'key = text'").arg(lineNo); return false; }
        QString key = line.left(eq).trimmed();
        QString val = line.mid(eq + 1);
        if (val.startsWith(' ')) val.remove(0, 1);
        QString unesc;
        for (int i = 0; i < val.size(); ++i) {
            if (val[i] == '\\' && i + 1 < val.size()) {
                QChar n = val[++i];
                unesc += n == 'n' ? QChar('\n') : n;
            } else unesc += val[i];
        }
        uint32_t hash;
        bool ok = false;
        if (key.startsWith("0x", Qt::CaseInsensitive)) hash = key.mid(2).toUInt(&ok, 16);
        else { hash = joaatLower(key); ok = !key.isEmpty(); }
        if (!ok) { if (err) *err = QStringLiteral("line %1: bad key").arg(lineNo); return false; }
        out.push_back({hash, unesc});
    }
    entries = out;
    return true;
}

// ------------------------------------------------------------------ RBF

namespace {
QString xmlEsc(const QString& s)
{
    QString o = s;
    o.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;");
    return o;
}

struct RbfNode {
    QString name;
    enum { Struct, Value, Bytes } kind = Struct;
    QString value;   // for Value
    QByteArray bytes;
    int pendingAttributes = 0;
    std::vector<std::unique_ptr<RbfNode>> children;
};

void emitNode(const RbfNode& n, QString& out, int depth)
{
    QString ind(depth * 2, ' ');
    if (n.kind == RbfNode::Value) {
        out += ind + "<" + n.name + " value=\"" + xmlEsc(n.value) + "\" />\n";
        return;
    }
    // first `pendingAttributes` value children become attributes
    QString attrs;
    size_t first = 0;
    for (; first < n.children.size() && int(first) < n.pendingAttributes; ++first) {
        auto& c = n.children[first];
        if (c->kind != RbfNode::Value) break;
        attrs += " " + c->name + "=\"" + xmlEsc(c->value) + "\"";
    }
    QString text;
    std::vector<const RbfNode*> rest;
    for (size_t i = first; i < n.children.size(); ++i) {
        if (n.children[i]->kind == RbfNode::Bytes) text += QString::fromUtf8(n.children[i]->bytes);
        else rest.push_back(n.children[i].get());
    }
    if (rest.empty() && text.isEmpty()) { out += ind + "<" + n.name + attrs + " />\n"; return; }
    if (rest.empty()) { out += ind + "<" + n.name + attrs + ">" + xmlEsc(text) + "</" + n.name + ">\n"; return; }
    out += ind + "<" + n.name + attrs + ">\n";
    if (!text.isEmpty()) out += ind + "  " + xmlEsc(text) + "\n";
    for (auto* c : rest) emitNode(*c, out, depth + 1);
    out += ind + "</" + n.name + ">\n";
}
} // namespace

bool rbfToXml(const QByteArray& data, QString& xml, QString* err)
{
    try {
        Reader r(data);
        if (r.u32() != 0x30464252) throw FormatError("not RBF");
        struct Desc { QString name; uint8_t type; };
        std::vector<Desc> descs;
        std::vector<RbfNode*> stack;
        std::unique_ptr<RbfNode> root;
        RbfNode* cur = nullptr;
        while (!r.atEnd()) {
            uint8_t di = r.u8();
            if (di == 0xFF) {
                if (r.u8() != 0xFF) throw FormatError("bad close tag");
                if (stack.empty()) break;
                cur = stack.back();
                stack.pop_back();
                continue;
            }
            if (di == 0xFD) {
                if (r.u8() != 0xFF) throw FormatError("bad bytes tag");
                int32_t len = r.i32();
                auto n = std::make_unique<RbfNode>();
                n->kind = RbfNode::Bytes;
                n->bytes = r.bytes(len);
                if (!cur) throw FormatError("data outside element");
                cur->children.push_back(std::move(n));
                continue;
            }
            uint8_t type = r.u8();
            if (di == descs.size()) {
                int16_t nl = r.i16();
                descs.push_back({QString::fromLatin1(r.bytes(nl)), type});
            } else if (di > descs.size()) {
                throw FormatError("bad descriptor index");
            }
            const QString& name = descs[di].name;
            auto n = std::make_unique<RbfNode>();
            n->name = name;
            switch (type) {
            case 0: {
                n->kind = RbfNode::Struct;
                r.i16(); r.i16();
                n->pendingAttributes = r.i16();
                RbfNode* raw = n.get();
                if (cur) { cur->children.push_back(std::move(n)); stack.push_back(cur); }
                else root = std::move(n);
                cur = raw;
                continue;
            }
            case 0x10: n->kind = RbfNode::Value; n->value = QStringLiteral("0x%1").arg(r.u32(), 8, 16, QChar('0')); break;
            case 0x20: n->kind = RbfNode::Value; n->value = "true"; break;
            case 0x30: n->kind = RbfNode::Value; n->value = "false"; break;
            case 0x40: n->kind = RbfNode::Value; n->value = QString::number(r.f32()); break;
            case 0x50: {
                n->kind = RbfNode::Value;
                float x = r.f32(), y = r.f32(), z = r.f32();
                n->value = QStringLiteral("%1, %2, %3").arg(x).arg(y).arg(z);
                break;
            }
            case 0x60: { n->kind = RbfNode::Value; int16_t l = r.i16(); n->value = QString::fromLatin1(r.bytes(l)); break; }
            default: throw FormatError(QStringLiteral("unsupported RBF type 0x%1").arg(type, 0, 16));
            }
            if (!cur) throw FormatError("value outside element");
            cur->children.push_back(std::move(n));
        }
        if (!root) throw FormatError("empty RBF");
        xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        emitNode(*root, xml, 0);
    } catch (const FormatError& e) {
        if (err) *err = e.qmessage();
        return false;
    }
    return true;
}

} // namespace rs
