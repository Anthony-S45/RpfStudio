#pragma once
#include "Gxt2.h"
#include "Viewers.h"

class QTableWidget;
class QLineEdit;

class Gxt2Viewer : public Viewer {
    Q_OBJECT
public:
    explicit Gxt2Viewer(QWidget* parent = nullptr);
    bool load(const QByteArray& d, QString* err);
    bool isEditable() const override { return true; }
    QByteArray serialize(QString* err) override;
    void addActions(QToolBar* tb) override;
private:
    void fill(const rs::Gxt2& g);
    rs::Gxt2 collect() const;
    void addEntry();
    void removeSelected();
    void exportText();
    void importText();
    void applyFilter(const QString& f);
    QTableWidget* m_table;
    QLineEdit* m_filter;
    bool m_filling = false;
};
