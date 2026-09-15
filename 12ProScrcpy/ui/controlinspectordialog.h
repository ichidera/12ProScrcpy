#ifndef CONTROLINSPECTORDIALOG_H
#define CONTROLINSPECTORDIALOG_H

#include <QDialog>
#include <QPushButton>

#include "keymapprofilestore.h"

// A button that shows the currently bound key/mouse-button and lets the user
// rebind it by clicking then pressing the desired key or mouse button.
class KeyCaptureButton : public QPushButton
{
    Q_OBJECT
public:
    explicit KeyCaptureButton(QWidget *parent = nullptr);

    // Canonical "Key_Xxx" / "LeftButton" style string, or empty if unbound.
    QString boundKeyString() const { return m_keyString; }
    void setBoundKeyString(const QString &s);

signals:
    void keyChanged(const QString &keyString);

protected:
    bool event(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    void startCapture();
    void finishCapture(const QString &keyString);
    void cancelCapture();
    void refreshText();

private:
    QString m_keyString;
    bool m_capturing = false;
};

class QSpinBox;
class QDoubleSpinBox;
class QLabel;
class QWidget;
class QCheckBox;

class ControlInspectorDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ControlInspectorDialog(const ControlNode &node, QWidget *parent = nullptr);

    ControlNode result() const;

private:
    void buildUi();
    QWidget *buildFieldsForAction();

private:
    ControlNode m_node;

    // shown depending on action
    KeyCaptureButton *m_keyCapture = nullptr;             // TapSpot / RepeatedTap / DragSwipe / AimPanShoot
    KeyCaptureButton *m_upKeyCapture = nullptr;            // DPad
    KeyCaptureButton *m_downKeyCapture = nullptr;          // DPad
    KeyCaptureButton *m_leftKeyCapture = nullptr;          // DPad
    KeyCaptureButton *m_rightKeyCapture = nullptr;         // DPad
    QDoubleSpinBox *m_offsetSpin = nullptr;                // DPad
    QSpinBox *m_repeatCountSpin = nullptr;                 // RepeatedTap
    QSpinBox *m_repeatIntervalSpin = nullptr;               // RepeatedTap
    QDoubleSpinBox *m_endXSpin = nullptr;                  // DragSwipe
    QDoubleSpinBox *m_endYSpin = nullptr;                  // DragSwipe
    QSpinBox *m_dragDelaySpin = nullptr;                   // DragSwipe
    QDoubleSpinBox *m_dragSpeedSpin = nullptr;              // DragSwipe
    QDoubleSpinBox *m_lookSpeedXSpin = nullptr;             // FreeLook / AimPanShoot
    QDoubleSpinBox *m_lookSpeedYSpin = nullptr;             // FreeLook / AimPanShoot
    KeyCaptureButton *m_smallEyesCapture = nullptr;         // FreeLook / AimPanShoot (optional)
    KeyCaptureButton *m_suspendKeyCapture = nullptr;        // FreeLook / AimPanShoot (optional) - hold to pause shoot-mode
    QCheckBox *m_fireAnchorEnabledCheck = nullptr;          // AimPanShoot only - BlueStacks' "Fire with left click"
};

#endif // CONTROLINSPECTORDIALOG_H
