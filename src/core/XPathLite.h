// Small XPath subset for package XML edits:
//   /a/b/c, //c, *, @attr (final step), [n], [@attr='v'], [child='v'], [text()='v']
#pragma once
#include <QDomDocument>
#include <QList>
#include <QString>

namespace rs {
QList<QDomNode> xpathSelect(const QDomNode& context, const QString& expr, QString* err);
}
