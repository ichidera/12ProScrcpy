#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QWheelEvent>

#include "editmodeoverlay.h"
#include "gamecontrolseditor.h" // kGameControlMimeType

EditModeOverlay::EditModeOverlay(QWidget *parent) : QWidget(parent)
{
    // We want this to look transparent (paintEvent draws the dim scrim
    // itself) but still be the widget that receives mouse/drag input -
    // that's the whole point: nothing under it (the mirrored phone screen)
    // should see stray clicks while a scheme is being edited.
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAcceptDrops(true);
    setCursor(Qt::ArrowCursor);
    setMouseTracking(true);

    m_toolbar = new QWidget(this);
    m_toolbar->setStyleSheet("background: rgba(20,20,24,225); border-radius: 8px;");
    auto *row = new QHBoxLayout(m_toolbar);
    row->setContentsMargins(12, 6, 10, 6);
    row->setSpacing(10);

    m_hint = new QLabel(tr("Editing controls — screen input is paused"), m_toolbar);
    m_hint->setStyleSheet("color: #cbd5e1; font-size: 12px;");

    m_cancelBtn = new QPushButton(tr("Cancel"), m_toolbar);
    m_cancelBtn->setStyleSheet("QPushButton { background:#334155; color:white; padding:4px 14px; border-radius:4px; }"
                                "QPushButton:hover { background:#475569; }");
    m_saveBtn = new QPushButton(tr("Save"), m_toolbar);
    m_saveBtn->setStyleSheet("QPushButton { background:#3b82f6; color:white; padding:4px 14px; border-radius:4px; }"
                              "QPushButton:hover { background:#2563eb; }"
                              "QPushButton:disabled { background:#1e3a5f; color:#94a3b8; }");
    m_saveBtn->setEnabled(false);

    row->addWidget(m_hint);
    row->addStretch();
    row->addWidget(m_cancelBtn);
    row->addWidget(m_saveBtn);
    m_toolbar->adjustSize();

    connect(m_saveBtn, &QPushButton::clicked, this, &EditModeOverlay::saveRequested);
    connect(m_cancelBtn, &QPushButton::clicked, this, &EditModeOverlay::cancelRequested);
}

void EditModeOverlay::setDirty(bool dirty)
{
    m_saveBtn->setEnabled(dirty);
    m_hint->setText(dirty ? tr("Unsaved changes — screen input is paused") : tr("Editing controls — screen input is paused"));
}

void EditModeOverlay::repositionToolbar()
{
    m_toolbar->adjustSize();
    m_toolbar->move((width() - m_toolbar->width()) / 2, 12);
}

void EditModeOverlay::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    repositionToolbar();
}

void EditModeOverlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    // Dim scrim so the surface visibly *reads* as "in edit mode", same idea
    // as BlueStacks greying out the game behind its controls editor.
    p.fillRect(rect(), QColor(0, 0, 0, 60));
    repositionToolbar();
}

void EditModeOverlay::mousePressEvent(QMouseEvent *event)
{
    // Swallow it: while editing, clicks on bare screen must NOT reach the
    // phone. Clicks meant for a GameControlMarker still work because those
    // markers are separate child widgets stacked above this overlay.
    event->accept();
}

void EditModeOverlay::mouseMoveEvent(QMouseEvent *event)
{
    event->accept();
}

void EditModeOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    event->accept();
}

void EditModeOverlay::wheelEvent(QWheelEvent *event)
{
    event->accept();
}

void EditModeOverlay::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat(kGameControlMimeType)) {
        event->acceptProposedAction();
    }
}

void EditModeOverlay::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasFormat(kGameControlMimeType)) {
        event->acceptProposedAction();
    }
}

void EditModeOverlay::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasFormat(kGameControlMimeType) || width() <= 0 || height() <= 0) {
        return;
    }
    bool ok = false;
    const int kindValue = event->mimeData()->data(kGameControlMimeType).toInt(&ok);
    if (!ok) {
        return;
    }
    const QPoint localPos = event->position().toPoint();
    const QPointF normPos(qBound(0.0, double(localPos.x()) / width(), 1.0), qBound(0.0, double(localPos.y()) / height(), 1.0));
    emit controlDropped(static_cast<ControlActionKind>(kindValue), normPos);
    event->acceptProposedAction();
}
