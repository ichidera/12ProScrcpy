
#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <QObject>
#include <QPointer>
#include <QSize>
#include <QMap>
#include <QTimer>

#include "adbsendeventsession.h"
#include "inputconvertbase.h"

class QTcpSocket;
class QTimer;
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

    // Scroll wheel, translated into a short synthetic swipe on the same raw
    // touch channel as sendRealTouch() - there's no root-shell equivalent
    // of a "scroll" input event, so this reuses the verified touch path
    // instead of any framework fallback, to keep it feeling like a real
    // finger flick.
    void sendRealScroll(QPoint framePos, const QSize &frameSize, float hScroll, float vScroll);

    // Root-elevated `input keyevent` fallback (same su session as touch),
    // used for keys with no confirmed raw-sendevent hardware node on this
    // device. postKeyCodeClick() routes through this internally; exposed
    // publicly so InputConvertGame's keymap "android key" bindings can call
    // it directly too, instead of going through the old control-socket path.
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

    // Android has two landscape rotations (ROTATION_90 and ROTATION_270)
    // that are mirror-image chiralities of each other and need opposite
    // touch pre-rotation formulas in sendRealTouch() - but both produce a
    // frame with width() > height(), so frameSize alone can't tell them
    // apart. These poll the live value via `dumpsys window`'s
    // mCurrentRotation so sendRealTouch() knows which formula to apply.
    enum class DeviceRotation
    {
        Rotation0,
        Rotation90,
        Rotation180,
        Rotation270,
        Unknown
    };
    void ensureRotationPolling();
    void pollDeviceRotation();

    // MOVE-throttling for sendRealTouch(): each raw sendevent call spawns a
    // new process on-device, and the persistent adb shell executes them
    // strictly serially. A live drag generates far more mouse-move samples
    // per second than the device can fork+exec+exit 3 processes per sample,
    // so without throttling, a backlog piles up in the shell's input queue
    // and keeps draining (visibly "sliding") well after the finger lifts.
    // DOWN/UP are dispatched immediately as before - only MOVE is coalesced,
    // always sending the latest known position per slot at each tick and
    // silently dropping the stale intermediate ones rather than queuing all
    // of them.
    struct PendingTouchMove
    {
        bool valid = false;
        int rawX = 0;
        int rawY = 0;
    };
    void ensureTouchMoveThrottle();
    void flushPendingTouchMoves();
    QPoint mapFrameToRawTouch(const QPoint &framePos, const QSize &frameSize) const;

private:
    QPointer<Receiver> m_receiver;
    QPointer<InputConvertBase> m_inputConvert;
    std::function<qint64(const QByteArray&)> m_sendData = Q_NULLPTR;
    QSize m_pendingResize;
    bool m_resizeQueued = false;
    bool m_cameraMode = false;
    QString m_serial;
    QPointer<AdbSendEventSession> m_realTouchSession;
    DeviceRotation m_deviceRotation = DeviceRotation::Unknown;
    QPointer<QTimer> m_rotationPollTimer;
    QMap<int, PendingTouchMove> m_pendingTouchMoves;
    QPointer<QTimer> m_touchMoveFlushTimer;
    // Guards against overlapping `dumpsys window` polls piling up if one
    // call happens to take longer than the poll interval (e.g. a slow/
    // wireless adb connection) - without this, a slow poll could still be
    // in flight when the next timer tick fires another one.
    bool m_rotationPollInFlight = false;
};

#endif // CONTROLLER_H
