#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSpinBox>
#include <QVBoxLayout>

#include "controlinspectordialog.h"

// ---------------------------------------------------------------- KeyCaptureButton

KeyCaptureButton::KeyCaptureButton(QWidget *parent) : QPushButton(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    refreshText();
}

void KeyCaptureButton::setBoundKeyString(const QString &s)
{
    m_keyString = s;
    refreshText();
}

void KeyCaptureButton::refreshText()
{
    if (m_capturing) {
        setText(tr("Press a key or click..."));
        return;
    }
    setText(m_keyString.isEmpty() ? tr("(unbound) click to set") : m_keyString);
}

void KeyCaptureButton::startCapture()
{
    m_capturing = true;
    refreshText();
    setFocus(Qt::MouseFocusReason);
    grabKeyboard();
    grabMouse();
}

void KeyCaptureButton::finishCapture(const QString &keyString)
{
    m_capturing = false;
    releaseKeyboard();
    releaseMouse();
    if (!keyString.isEmpty()) {
        m_keyString = keyString;
        emit keyChanged(m_keyString);
    }
    refreshText();
}

void KeyCaptureButton::cancelCapture()
{
    m_capturing = false;
    releaseKeyboard();
    releaseMouse();
    refreshText();
}

void KeyCaptureButton::mousePressEvent(QMouseEvent *event)
{
    if (!m_capturing) {
        startCapture();
        return; // this press only starts capture, it is not itself a binding
    }
    const QString s = KeyMapProfileStore::keyToString(static_cast<int>(event->button()), true);
    finishCapture(s);
}

void KeyCaptureButton::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event)
    // swallowed: mousePressEvent already handled start/finish, and we must
    // not let QPushButton's own click machinery fire from these presses.
}

void KeyCaptureButton::keyPressEvent(QKeyEvent *event)
{
    if (!m_capturing) {
        QPushButton::keyPressEvent(event);
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        cancelCapture();
        return;
    }
    const QString s = KeyMapProfileStore::keyToString(event->key(), false);
    finishCapture(s);
}

void KeyCaptureButton::focusOutEvent(QFocusEvent *event)
{
    if (m_capturing) {
        cancelCapture();
    }
    QPushButton::focusOutEvent(event);
}

// ---------------------------------------------------------------- ControlInspectorDialog

ControlInspectorDialog::ControlInspectorDialog(const ControlNode &node, QWidget *parent) : QDialog(parent), m_node(node)
{
    setWindowTitle(KeyMapProfileStore::actionLabel(node.action));
    buildUi();
}

void ControlInspectorDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();
    layout->addLayout(form);

    m_labelEdit = new QLineEdit(m_node.label, this);
    m_labelEdit->setPlaceholderText(KeyMapProfileStore::actionLabel(m_node.action));
    form->addRow(tr("Name"), m_labelEdit);

    QWidget *fields = buildFieldsForAction();
    if (fields) {
        layout->addWidget(fields);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    setMinimumWidth(320);
}

QWidget *ControlInspectorDialog::buildFieldsForAction()
{
    auto *w = new QWidget(this);
    auto *form = new QFormLayout(w);
    form->setContentsMargins(0, 0, 0, 0);

    switch (m_node.action) {
    case ControlActionKind::TapSpot: {
        m_keyCapture = new KeyCaptureButton(w);
        m_keyCapture->setBoundKeyString(m_node.key);
        form->addRow(tr("Key / mouse button"), m_keyCapture);
        break;
    }
    case ControlActionKind::RepeatedTap: {
        m_keyCapture = new KeyCaptureButton(w);
        m_keyCapture->setBoundKeyString(m_node.key);
        form->addRow(tr("Key / mouse button"), m_keyCapture);

        m_repeatCountSpin = new QSpinBox(w);
        m_repeatCountSpin->setRange(1, 50);
        m_repeatCountSpin->setValue(m_node.repeatCount);
        form->addRow(tr("Tap count"), m_repeatCountSpin);

        m_repeatIntervalSpin = new QSpinBox(w);
        m_repeatIntervalSpin->setRange(10, 5000);
        m_repeatIntervalSpin->setSuffix(" ms");
        m_repeatIntervalSpin->setValue(m_node.repeatIntervalMs);
        form->addRow(tr("Interval"), m_repeatIntervalSpin);
        break;
    }
    case ControlActionKind::DPad: {
        m_upKeyCapture = new KeyCaptureButton(w);
        m_upKeyCapture->setBoundKeyString(m_node.upKey);
        form->addRow(tr("Up key"), m_upKeyCapture);

        m_downKeyCapture = new KeyCaptureButton(w);
        m_downKeyCapture->setBoundKeyString(m_node.downKey);
        form->addRow(tr("Down key"), m_downKeyCapture);

        m_leftKeyCapture = new KeyCaptureButton(w);
        m_leftKeyCapture->setBoundKeyString(m_node.leftKey);
        form->addRow(tr("Left key"), m_leftKeyCapture);

        m_rightKeyCapture = new KeyCaptureButton(w);
        m_rightKeyCapture->setBoundKeyString(m_node.rightKey);
        form->addRow(tr("Right key"), m_rightKeyCapture);

        m_offsetSpin = new QDoubleSpinBox(w);
        m_offsetSpin->setRange(0.02, 0.45);
        m_offsetSpin->setSingleStep(0.01);
        m_offsetSpin->setDecimals(3);
        m_offsetSpin->setValue(m_node.offset);
        form->addRow(tr("Travel distance"), m_offsetSpin);
        break;
    }
    case ControlActionKind::DragSwipe: {
        m_keyCapture = new KeyCaptureButton(w);
        m_keyCapture->setBoundKeyString(m_node.key);
        form->addRow(tr("Key / mouse button"), m_keyCapture);

        m_endXSpin = new QDoubleSpinBox(w);
        m_endXSpin->setRange(0.0, 1.0);
        m_endXSpin->setDecimals(3);
        m_endXSpin->setSingleStep(0.01);
        m_endXSpin->setValue(m_node.endPos.x());
        form->addRow(tr("Drag end X"), m_endXSpin);

        m_endYSpin = new QDoubleSpinBox(w);
        m_endYSpin->setRange(0.0, 1.0);
        m_endYSpin->setDecimals(3);
        m_endYSpin->setSingleStep(0.01);
        m_endYSpin->setValue(m_node.endPos.y());
        form->addRow(tr("Drag end Y"), m_endYSpin);

        m_dragDelaySpin = new QSpinBox(w);
        m_dragDelaySpin->setRange(0, 5000);
        m_dragDelaySpin->setSuffix(" ms");
        m_dragDelaySpin->setValue(static_cast<int>(m_node.dragStartDelayMs));
        form->addRow(tr("Start delay"), m_dragDelaySpin);

        m_dragSpeedSpin = new QDoubleSpinBox(w);
        m_dragSpeedSpin->setRange(0.05, 1.0);
        m_dragSpeedSpin->setSingleStep(0.05);
        m_dragSpeedSpin->setValue(m_node.dragSpeed);
        form->addRow(tr("Speed"), m_dragSpeedSpin);
        break;
    }
    case ControlActionKind::FreeLook:
    case ControlActionKind::AimPanShoot: {
        if (m_node.action == ControlActionKind::AimPanShoot) {
            m_keyCapture = new KeyCaptureButton(w);
            m_keyCapture->setBoundKeyString(m_node.key.isEmpty() ? QStringLiteral("LeftButton") : m_node.key);
            form->addRow(tr("Shoot button"), m_keyCapture);
        }

        m_lookSpeedXSpin = new QDoubleSpinBox(w);
        m_lookSpeedXSpin->setRange(1.0, 100.0);
        m_lookSpeedXSpin->setValue(m_node.lookSpeedX);
        form->addRow(tr("Look speed X"), m_lookSpeedXSpin);

        m_lookSpeedYSpin = new QDoubleSpinBox(w);
        m_lookSpeedYSpin->setRange(1.0, 100.0);
        m_lookSpeedYSpin->setValue(m_node.lookSpeedY);
        form->addRow(tr("Look speed Y"), m_lookSpeedYSpin);

        m_smallEyesCapture = new KeyCaptureButton(w);
        m_smallEyesCapture->setBoundKeyString(m_node.smallEyesKey);
        form->addRow(tr("Precision-aim toggle (optional)"), m_smallEyesCapture);
        break;
    }
    }

    return w;
}

ControlNode ControlInspectorDialog::result() const
{
    ControlNode node = m_node;
    node.label = m_labelEdit->text().trimmed();

    switch (node.action) {
    case ControlActionKind::TapSpot:
        node.key = m_keyCapture->boundKeyString();
        break;
    case ControlActionKind::RepeatedTap:
        node.key = m_keyCapture->boundKeyString();
        node.repeatCount = m_repeatCountSpin->value();
        node.repeatIntervalMs = m_repeatIntervalSpin->value();
        break;
    case ControlActionKind::DPad:
        node.upKey = m_upKeyCapture->boundKeyString();
        node.downKey = m_downKeyCapture->boundKeyString();
        node.leftKey = m_leftKeyCapture->boundKeyString();
        node.rightKey = m_rightKeyCapture->boundKeyString();
        node.offset = m_offsetSpin->value();
        break;
    case ControlActionKind::DragSwipe:
        node.key = m_keyCapture->boundKeyString();
        node.endPos = QPointF(m_endXSpin->value(), m_endYSpin->value());
        node.dragStartDelayMs = static_cast<quint32>(m_dragDelaySpin->value());
        node.dragSpeed = static_cast<float>(m_dragSpeedSpin->value());
        break;
    case ControlActionKind::FreeLook:
    case ControlActionKind::AimPanShoot:
        if (m_keyCapture) {
            node.key = m_keyCapture->boundKeyString();
        }
        node.lookSpeedX = static_cast<float>(m_lookSpeedXSpin->value());
        node.lookSpeedY = static_cast<float>(m_lookSpeedYSpin->value());
        node.smallEyesKey = m_smallEyesCapture->boundKeyString();
        break;
    }

    return node;
}
