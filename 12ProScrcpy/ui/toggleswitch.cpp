#include <QPainter>

#include "toggleswitch.h"

ToggleSwitch::ToggleSwitch(QWidget *parent) : QAbstractButton(parent)
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(sizeHint());
}

QSize ToggleSwitch::sizeHint() const
{
    return QSize(kWidth, kHeight);
}

void ToggleSwitch::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF track = rect().adjusted(1, 1, -1, -1);
    const qreal radius = track.height() / 2.0;

    QColor trackColor = isChecked() ? QColor("#22c55e") : QColor("#3f4653");
    if (!isEnabled()) {
        trackColor.setAlpha(110);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(trackColor);
    p.drawRoundedRect(track, radius, radius);

    const qreal knobDiameter = track.height() - 4;
    const qreal knobY = track.top() + 2;
    const qreal knobX = isChecked() ? track.right() - knobDiameter - 2 : track.left() + 2;
    QColor knobColor = Qt::white;
    if (!isEnabled()) {
        knobColor.setAlpha(160);
    }
    p.setBrush(knobColor);
    p.drawEllipse(QRectF(knobX, knobY, knobDiameter, knobDiameter));
}
