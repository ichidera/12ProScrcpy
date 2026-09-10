
#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <QObject>
#include <QPointer>
#include <QSize>

#include "adbsendeventsession.h"
#include "inputconvertbase.h"

class QTcpSocket;
class Receiver;
class InputConvertBase;
class DeviceMsg;
class Controller : public QObject
{
    Q_OBJECT
public:
    // Reserved touch slot for the plain-mouse / non-custom-keymap input path.
    // Custom game keymaps use slots 0..(MULTI_TOUCH_MAX_NUM-1) for their own
    // synthetic multi-touch contacts, so this must stay outside that range -
    // see inputconvertgame.h.
    static constexpr int kMouseTouchSlot = 9;

    Controller(std::function<qint64(const QByteArray&)> sendData, const QString &serial, QString gameScript = "", QObject *parent = Q_NULLPTR);
    virtual ~Controller();

    void postControlMsg(ControlMsg *controlMsg);
    void setCameraMode(bool cameraMode);
    void recvDeviceMsg(DeviceMsg *deviceMsg);

    // Real-device touch injection via `adb shell sendevent`, writing directly
    // into the touchscreen's kernel input node instead of scrcpy's
    // control-socket MotionEvent protocol. This is the default (and only)
    // touch/click path now - see docs/real-device-adb-sendevent.md.
    void sendRealTouch(int slot, AndroidMotioneventAction action, QPoint framePos, const QSize &frameSize);

    // Framework-level fallback (root `input keyevent`) for hardware-button
    // keycodes with no confirmed kernel node on this device — see
    // docs/real-device-adb-sendevent.md. postGoHome/postGoBack/postGoMenu/
    // postPower/postVolumeUp/postVolumeDown use the raw sendevent hardware
    // nodes directly instead; this is only used by postKeyCodeClick for the
    // remaining keys (AppSwitch, Copy, Cut).
    void sendRealKeyEvent(int androidKeycode);

    void updateScript(QString gameScript = "");
    bool isCurrentCustomKeymap();

    void postGoBack();
    void postGoHome();
    void postGoMenu();
    void postAppSwitch();
    void postPower();
    void postVolumeUp();
    void postVolumeDown();
    void copy();
    void cut();
    void expandNotificationPanel();
    void expandSettingsPanel();
    void collapsePanel();
    void rotateDevice();
    void startApp(const QString &name);
    void scanFile(const QString &path);
    void resizeDisplay(const QSize &size);
    void setDisplayPower(bool on);
    void setCameraTorch(bool on);
    void cameraZoomIn();
    void cameraZoomOut();

    // for input convert
    void mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize);
    void wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize);
    void keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize);

    // turn the screen on if it was off, press BACK otherwise
    // If the screen is off, it is turned on only on down
    void postBackOrScreenOn(bool down);
    void requestDeviceClipboard();
    void getDeviceClipboard(bool cut = false);
    void setDeviceClipboard(bool pause = true);
    void clipboardPaste();
    void postTextInput(QString &text);

signals:
    void grabCursor(bool grab);

protected:
    bool event(QEvent *event);

private:
    bool sendControl(const QByteArray &buffer);
    void postKeyCodeClick(AndroidKeycode keycode);
    void sendPendingResize();
    void ensureRealTouchSession();

private:
    QPointer<Receiver> m_receiver;
    QPointer<InputConvertBase> m_inputConvert;
    std::function<qint64(const QByteArray&)> m_sendData = Q_NULLPTR;
    QSize m_pendingResize;
    bool m_resizeQueued = false;
    bool m_cameraMode = false;
    QString m_serial;
    QPointer<AdbSendEventSession> m_realTouchSession;
};

#endif // CONTROLLER_H
