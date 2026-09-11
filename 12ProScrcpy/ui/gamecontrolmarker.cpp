#include <cmath>

#include <QContextMenuEvent>
#include <QFont>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>

#include "gamecontrolmarker.h"

namespace
{
constexpr int kMarkerSize = 40;
}

GameControlMarker::GameControlMarker(const ControlNode &node, QWidget *parent) : QWidget(parent), m_node(node)
{
    setFixedSize(kMarkerSize, kMarkerSize);
    setCursor(Qt::PointingHandCursor);
    setToolTip(node.label.isEmpty() ? KeyMapProfileStore::actionLabel(node.action) : node.label);
    setAttribute(Qt::WA_Hover, true);
}

void GameControlMarker::setNode(const ControlNode &node)
{
    m_node = node;
    setToolTip(node.label.isEmpty() ? KeyMapProfileStore::actionLabel(node.action) : node.label);
    update();
}

void GameControlMarker::relayout(const QSize &surfaceSize)
{
    if (surfaceSize.isEmpty()) {
        return;
    }
    const int x = qRound(m_node.pos.x() * surfaceSize.width()) - width() / 2;
    const int y = qRound(m_node.pos.y() * surfaceSize.height()) - height() / 2;
    move(x, y);
}

QString GameControlMarker::shortCaption() const
{
    // Shows the *bound key*, not the action name - the glyph in paintEvent()
    // already tells you the action type by shape, so the caption's job is
    // to answer "which key is this" at a glance (same idea as BlueStacks
    // printing the bound key directly on the placed control icon).
    auto shortKeyName = [](const QString &keyString) {
        if (keyString.isEmpty()) {
            return QStringLiteral("?");
        }
        // "Key_A" -> "A", "Key_QuoteLeft" -> "QuoteLeft" (rare edge case,
        // still short enough), "LeftButton" -> "LMB" for mouse buttons.
        if (keyString == QLatin1String("LeftButton")) {
            return QStringLiteral("LMB");
        }
        if (keyString == QLatin1String("RightButton")) {
            return QStringLiteral("RMB");
        }
        if (keyString == QLatin1String("MiddleButton")) {
            return QStringLiteral("MMB");
        }
        QString s = keyString;
        s.remove(QStringLiteral("Key_"));
        return s.size() > 5 ? s.left(5) : s;
    };

    switch (m_node.action) {
    case ControlActionKind::TapSpot:
        return shortKeyName(m_node.key);
    case ControlActionKind::RepeatedTap:
        return QStringLiteral("x%1").arg(qMax(1, m_node.repeatCount));
    case ControlActionKind::DPad:
        return QStringLiteral("%1%2%3%4")
            .arg(shortKeyName(m_node.upKey).left(1), shortKeyName(m_node.leftKey).left(1), shortKeyName(m_node.downKey).left(1),
                 shortKeyName(m_node.rightKey).left(1));
    case ControlActionKind::DragSwipe:
        return shortKeyName(m_node.key);
    case ControlActionKind::FreeLook:
        return m_node.smallEyesKey.isEmpty() ? QString() : shortKeyName(m_node.smallEyesKey);
    case ControlActionKind::AimPanShoot:
        return shortKeyName(m_node.key);
    }
    return QString();
}

QColor GameControlMarker::badgeColor() const
{
    switch (m_node.action) {
    case ControlActionKind::TapSpot:
        return QColor("#3b82f6");
    case ControlActionKind::RepeatedTap:
        return QColor("#8b5cf6");
    case ControlActionKind::DPad:
        return QColor("#22c55e");
    case ControlActionKind::DragSwipe:
        return QColor("#f59e0b");
    case ControlActionKind::FreeLook:
        return QColor("#06b6d4");
    case ControlActionKind::AimPanShoot:
        return QColor("#ef4444");
    }
    return QColor("#64748b");
}

void GameControlMarker::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Outer badge: color still keys the action category (kept from before),
    // but the interior is now a real glyph instead of a text abbreviation -
    // shape communicates the gesture the way BlueStacks' icons do (a filled
    // dot reads as "tap" faster than the word "TAP" does).
    const QRectF r = rect().adjusted(1, 1, -1, -1);
    QColor fill = badgeColor();
    fill.setAlpha(210);
    p.setBrush(fill);
    p.setPen(QPen(QColor(255, 255, 255, 220), 2));
    p.drawEllipse(r);

    const QPointF c = r.center();
    const qreal R = r.width() / 2.0 - 6.0; // glyph radius, inset from the badge ring

    p.setPen(QPen(Qt::white, 2));
    p.setBrush(Qt::NoBrush);

    switch (m_node.action) {
    case ControlActionKind::TapSpot: {
        // single filled dot = single tap
        p.setBrush(Qt::white);
        p.drawEllipse(c, R * 0.4, R * 0.4);
        break;
    }
    case ControlActionKind::RepeatedTap: {
        // concentric rings = repeated/multi tap; ring count reads as "more
        // than once", same shorthand as a double/triple-tap icon
        p.drawEllipse(c, R * 0.35, R * 0.35);
        p.drawEllipse(c, R * 0.65, R * 0.65);
        p.drawEllipse(c, R, R);
        break;
    }
    case ControlActionKind::DPad: {
        // 4-point compass/cross with arrowheads on each spoke
        const QPointF dirs[4] = { QPointF(0, -1), QPointF(0, 1), QPointF(-1, 0), QPointF(1, 0) };
        for (const QPointF &d : dirs) {
            const QPointF tip = c + d * R;
            p.drawLine(c, tip);
            const QPointF normal(-d.y(), d.x());
            p.drawLine(tip, tip - d * (R * 0.28) + normal * (R * 0.18));
            p.drawLine(tip, tip - d * (R * 0.28) - normal * (R * 0.18));
        }
        break;
    }
    case ControlActionKind::DragSwipe: {
        // arrow pointing from the anchor toward the swipe's endPos
        QPointF dir = m_node.endPos - m_node.pos;
        const qreal len = std::hypot(dir.x(), dir.y());
        QPointF unit = len > 0.0001 ? dir / len : QPointF(1, 0);
        const QPointF tip = c + unit * R;
        p.drawLine(c - unit * R, tip);
        const QPointF normal(-unit.y(), unit.x());
        p.drawLine(tip, tip - unit * (R * 0.5) + normal * (R * 0.35));
        p.drawLine(tip, tip - unit * (R * 0.5) - normal * (R * 0.35));
        break;
    }
    case ControlActionKind::FreeLook: {
        // eye motif: outer ring (field of view) + pupil
        p.drawEllipse(c, R, R * 0.65);
        p.setBrush(Qt::white);
        p.drawEllipse(c, R * 0.22, R * 0.22);
        break;
    }
    case ControlActionKind::AimPanShoot: {
        // crosshair with a gap at the center, matches the "custom crosshair"
        // BlueStacks shows on its Aim/Pan/Shoot control
        p.drawLine(c + QPointF(0, -R), c + QPointF(0, -R * 0.35));
        p.drawLine(c + QPointF(0, R * 0.35), c + QPointF(0, R));
        p.drawLine(c + QPointF(-R, 0), c + QPointF(-R * 0.35, 0));
        p.drawLine(c + QPointF(R * 0.35, 0), c + QPointF(R, 0));
        p.setBrush(Qt::white);
        p.drawEllipse(c, 2.0, 2.0);
        break;
    }
    }

    const QString caption = shortCaption();
    if (!caption.isEmpty()) {
        QFont f = p.font();
        f.setPixelSize(8);
        f.setBold(true);
        p.setFont(f);
        // Small caption band along the bottom edge of the badge, inside the ring.
        QRectF captionRect = r.adjusted(0, r.height() - 12, 0, -2);
        p.setPen(Qt::white);
        p.drawText(captionRect, Qt::AlignCenter, caption);
    }
}

void GameControlMarker::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragStartMouse = event->globalPosition().toPoint();
        m_dragStartWidgetPos = pos();
    }
}

void GameControlMarker::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging || !parentWidget()) {
        return;
    }
    const QPoint delta = event->globalPosition().toPoint() - m_dragStartMouse;
    QPoint newPos = m_dragStartWidgetPos + delta;

    const QSize surface = parentWidget()->size();
    newPos.setX(qBound(-width() / 2, newPos.x(), surface.width() - width() / 2));
    newPos.setY(qBound(-height() / 2, newPos.y(), surface.height() - height() / 2));
    move(newPos);

    if (surface.width() > 0 && surface.height() > 0) {
        m_node.pos.setX(qreal(newPos.x() + width() / 2) / surface.width());
        m_node.pos.setY(qreal(newPos.y() + height() / 2) / surface.height());
    }
}

void GameControlMarker::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event)
    if (m_dragging) {
        m_dragging = false;
        emit moved(this);
    }
}

void GameControlMarker::mouseDoubleClickEvent(QMouseEvent *event)
{
    Q_UNUSED(event)
    emit editRequested(this);
}

void GameControlMarker::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    QAction *editAction = menu.addAction(tr("Edit..."));
    QAction *removeAction = menu.addAction(tr("Remove"));
    QAction *chosen = menu.exec(event->globalPos());
    if (chosen == editAction) {
        emit editRequested(this);
    } else if (chosen == removeAction) {
        emit removeRequested(this);
    }
}
