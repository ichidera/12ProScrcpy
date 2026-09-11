#ifndef EDITMODEOVERLAY_H
#define EDITMODEOVERLAY_H

#include <QPointF>
#include <QWidget>

#include "keymapprofilestore.h"

class QPushButton;
class QLabel;

// Transparent-but-click-eating layer shown over the mirrored screen while a
// control scheme is being edited (BlueStacks-style: while the Controls
// editor panel is open, the phone screen underneath must not receive stray
// clicks/drags, and a Save/Cancel bar floats over it). Sits as a child of
// VideoForm::gameControlsSurface(), same geometry, stacked below any
// GameControlMarker widgets so those stay draggable on top of it.
class EditModeOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit EditModeOverlay(QWidget *parent = nullptr);

    // Enables/disables the Save button and tweaks the hint text.
    void setDirty(bool dirty);

signals:
    void saveRequested();
    void cancelRequested();
    // Re-emitted from dropEvent() when a palette action (see
    // kGameControlMimeType in gamecontrolseditor.h) is dropped on the
    // overlay - it's the topmost widget over the video once shown, so drops
    // land here instead of directly on VideoForm.
    void controlDropped(ControlActionKind kind, QPointF normPos);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void repositionToolbar();

private:
    QWidget *m_toolbar = nullptr;
    QLabel *m_hint = nullptr;
    QPushButton *m_saveBtn = nullptr;
    QPushButton *m_cancelBtn = nullptr;
};

#endif // EDITMODEOVERLAY_H
