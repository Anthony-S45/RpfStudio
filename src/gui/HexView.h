#pragma once
#include "Viewers.h"
#include <QAbstractScrollArea>

class HexArea : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit HexArea(const QByteArray& d, QWidget* parent = nullptr);
    void gotoOffset(qint64 off);
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    void updateScroll();
    QByteArray m_d;
    int m_lineH = 16, m_charW = 8;
};

class HexViewer : public Viewer {
    Q_OBJECT
public:
    explicit HexViewer(const QByteArray& d, QWidget* parent = nullptr);
    void addActions(QToolBar* tb) override;
private:
    HexArea* m_area;
};
