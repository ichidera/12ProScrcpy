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

QString GameControlMarker::shortCode() const
{
    switch (m_node.action) {
    case ControlActionKind::TapSpot:
        return QStringLiteral("TAP");
    case ControlActionKind::RepeatedTap:
        return QStringLiteral("x%1").arg(qMax(1, m_node.repeatCount));
    case ControlActionKind::DPad:
        return QStringLiteral("D-P");
    case ControlActionKind::DragSwipe:
        return QStringLiteral("DRG");
    case ControlActionKind::FreeLook:
        return QStringLiteral("LOOK");
    case ControlActionKind::AimPanShoot:
        return QStringLiteral("AIM");
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

    QColor fill = badgeColor();
    fill.setAlpha(210);
    p.setBrush(fill);
    p.setPen(QPen(QColor(255, 255, 255, 220), 2));
    p.drawEllipse(rect().adjusted(1, 1, -1, -1));

    p.setPen(Qt::white);
    QFont f = p.font();
    f.setPixelSize(9);
    f.setBold(true);
    p.setFont(f);
    p.drawText(rect(), Qt::AlignCenter, shortCode());
}

void GameControlMarker::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragStartMouse = event->globalPos();
        m_dragStartWidgetPos = pos();
    }
}

void GameControlMarker::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging || !parentWidget()) {
        return;
    }
    const QPoint delta = event->globalPos() - m_dragStartMouse;
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
