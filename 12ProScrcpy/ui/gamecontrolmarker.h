#ifndef GAMECONTROLMARKER_H
#define GAMECONTROLMARKER_H

#include <QColor>
#include <QWidget>

#include "keymapprofilestore.h"

// A small draggable badge placed on top of the mirrored screen, representing
// one ControlNode. Dragging it updates its normalized position; double
// clicking asks the owner to open the property editor for it.
class GameControlMarker : public QWidget
{
    Q_OBJECT
public:
    explicit GameControlMarker(const ControlNode &node, QWidget *parent = nullptr);

    const ControlNode &node() const { return m_node; }
    void setNode(const ControlNode &node);

    // Re-place this marker for the given surface size, from its normalized pos().
    void relayout(const QSize &surfaceSize);

    // Used by the persistent "On-screen controls" display (see
    // GameControlsPanel): a non-interactive marker just shows where a
    // control lives, at an adjustable transparency, without accepting
    // drags/edits/context menus - it exists outside of the full editor.
    void setInteractive(bool interactive);
    bool isInteractive() const { return m_interactive; }

    // 0-100, matches the "Opacity" slider in the Game controls panel.
    void setDisplayOpacityPercent(int percent);

signals:
    void moved(GameControlMarker *self);
    void editRequested(GameControlMarker *self);
    void removeRequested(GameControlMarker *self);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    // The bound key/count shown as a small caption under the glyph (e.g.
    // "W", "x3") - NOT the action name; the glyph itself (see paintEvent)
    // already communicates the action type via shape.
    QString shortCaption() const;
    QColor badgeColor() const;

private:
    ControlNode m_node;
    QPoint m_dragStartMouse;
    QPoint m_dragStartWidgetPos;
    bool m_dragging = false;
    bool m_interactive = true;
    int m_displayOpacityPercent = 100;
};

#endif // GAMECONTROLMARKER_H
