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
    enum class MarkerRole
    {
        Primary,   // the control's own anchor (node.pos) - every action kind
        FireAnchor // AimPanShoot's independent fire spot (node.fireAnchorPos,
                   // BlueStacks calls it the "fire icon") - always present
                   // as a child of a Primary AimPanShoot marker, meant to be
                   // dragged onto the game's own on-screen fire button.
    };

    explicit GameControlMarker(const ControlNode &node, QWidget *parent = nullptr, MarkerRole role = MarkerRole::Primary,
                                GameControlMarker *owner = nullptr);
    ~GameControlMarker() override;

    const ControlNode &node() const { return m_node; }
    void setNode(const ControlNode &node);

    MarkerRole role() const { return m_role; }

    // Re-place this marker for the given surface size, from its normalized
    // anchor (node.pos, or node.fireAnchorPos for a FireAnchor marker).
    // Also relayouts the fire-anchor child, if this is a Primary marker
    // that currently has one.
    void relayout(const QSize &surfaceSize);

    // Both only meaningful on a FireAnchor marker's owner, called by that
    // child marker itself while/after being dragged - lets the child push
    // its position straight into the owner's own ControlNode and re-emit
    // moved() as if the owner had moved, so GameControlsEditor's existing
    // per-marker dirty-tracking/applyLive() machinery picks up the change
    // without needing to know this child marker exists at all.
    void syncFireAnchorPos(QPointF normPos);
    void notifyMoved();
    void requestEdit();
    // The fire icon is half of the AimPanShoot control rather than an
    // independent marker, so removing from its context menu removes the
    // whole control via the owner - the child itself isn't registered with
    // GameControlsEditor and can't go through removeMarker() on its own.
    void requestRemove();

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
    // Creates/updates the FireAnchor child for an AimPanShoot node, and
    // destroys it if the action changes to something else. No-op unless
    // this is a Primary marker.
    void updateFireAnchorChild();

private:
    ControlNode m_node;
    QPoint m_dragStartMouse;
    QPoint m_dragStartWidgetPos;
    bool m_dragging = false;
    bool m_interactive = true;
    int m_displayOpacityPercent = 100;

    MarkerRole m_role = MarkerRole::Primary;
    GameControlMarker *m_owner = nullptr;           // set only when role() == FireAnchor
    GameControlMarker *m_fireAnchorChild = nullptr; // set only on a Primary AimPanShoot marker, while enabled
};

#endif // GAMECONTROLMARKER_H
