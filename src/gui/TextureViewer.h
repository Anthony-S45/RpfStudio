#pragma once
#include "Viewers.h"
#include "Ytd.h"

class QTreeWidget;
class QLabel;
class QScrollArea;
class QSpinBox;
class QCheckBox;
class QComboBox;

// Texture dictionary viewer/editor; also used read-only for textures embedded in models.
class TextureViewer : public Viewer {
    Q_OBJECT
public:
    explicit TextureViewer(QWidget* parent = nullptr);
    bool load(const QByteArray& ytd, QString* err);
    void setDictionary(const rs::TextureDictionary& d, bool editable);
    bool isEditable() const override { return m_editable; }
    QByteArray serialize(QString* err) override;
    void addActions(QToolBar* tb) override;
    static bool importImage(QWidget* parent, const QString& path, rs::TextureImage& out, uint32_t preferredFormat, QString* err);
private slots:
    void refreshList();
    void showCurrent();
    void exportSelected(bool png);
    void exportAll();
    void replaceSelected();
    void addTexture();
    void removeSelected();
    void renameSelected();
private:
    int current() const;
    rs::TextureDictionary m_dict;
    bool m_editable = true;
    QTreeWidget* m_list;
    QLabel* m_image;
    QLabel* m_info;
    QScrollArea* m_scroll;
    QSpinBox* m_mip;
    QComboBox* m_zoom;
    QCheckBox* m_alpha;
};
