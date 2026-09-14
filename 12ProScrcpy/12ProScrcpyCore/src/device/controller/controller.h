#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <QObject>
#include <QPointer>
#include <QSize>
#include <QMap>
#include <QTimer>

#include "adbsendeventsession.h"
#include "rawinputdaemonsession.h"
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

    // Master enable/disable for the persistent raw-input daemon path
    // (docs/daemon-implementation-plan.md). Must be called before the first
    // touch event to have effect on the daemon-attempt latch; defaults to
    // enabled. When disabled (or when the daemon fails to start), touch
    // events are dropped with a warning rather than falling back to
    // AdbSendEventSession - see sendRealTouch().
    void setRawInputDaemonEnabled(bool enabled);

    // Real-device touch injection via the persistent raw-input daemon
    // (docs/daemon-implementation-plan.md, RawInputDaemonSession) writing
    // directly into the touchscreen's kernel input node via a long-lived
    // socket. This is now the ONLY touch path: if the daemon isn't
    // available (disabled, or ensureRawInputDaemon() failed to start it),
    // the event is dropped and a qWarning is logged - no more silent
    // AdbSendEventSession sendevent fallback. That fallback was removed
    // because it masked daemon-start failures: sendevent never talks to the
    // daemon binary, so nothing about a failed daemon start ever showed up
    // in the daemon's own on-device log, making failures invisible.
    void sendRealTouch(int slot, AndroidMotioneventAction action, QPoint framePos, const QSize &frameSize);

    // Scroll wheel, translated into a short synthetic swipe on the same raw
    // touch channel as sendRealTouch(). Same no-fallback behaviour: dropped
    // with a warning if the daemon isn't available.
    void sendRealScroll(QPoint framePos, const QSize &frameSize, float hScroll, float vScroll);

    // Root-elevated `input keyevent` fallback (same su session as touch),
    // used for keys with no confirmed raw-sendevent hardware node on this
    // device. postKeyCodeClick() routes through this internally; exposed
    // publicly so InputConvertGame's keymap "android key" bindings can call
    // it directly too, instead of going through the old control-socket path.
    void sendRealKeyEvent(int androidKeycode);

    void updateScript(QString gameScript = "");
    bool isCurrentCustomKeymap();
    // See InputConvertBase::setForceCustomKeymap() - no-op when the current
    // InputConvert isn't keymap-capable (i.e. no script was ever loaded).
    void setForceCustomKeymap(bool enabled);

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

    // Persistent raw-input daemon path (docs/daemon-implementation-plan.md).
    // Touch only in v1 - hardware keys always stay on m_realTouchSession
    // (AdbSendEventSession), see plan §0. Tried once per Controller
    // lifetime (i.e. once per device connection); on any startup failure,
    // touch is simply dropped (with a qWarning) for the rest of this
    // connection - never retried mid-connection, and no sendevent fallback
    // (removed - see sendRealTouch()).
    void ensureRawInputDaemon();

    // MOVE dispatch for sendRealTouch(): sent the instant it arrives, not
    // on a periodic tick - see RawInputDaemonSession::hasPendingWrites()/
    // writesFlushed(). A fixed-interval timer was tried first but adds a
    // constant phase lag (up to one tick period) between the real mouse
    // position and what's on the wire regardless of link speed - invisible
    // at low mouse speed, clearly visible as "catching up" at high speed,
    // since the position gap for a given time-lag scales with how fast the
    // mouse is moving. Real backpressure (the OS socket send buffer still
    // draining) is a better signal than a guessed interval: only the
    // latest position per slot is kept if a write is still in flight, and
    // it's sent the moment the previous one finishes flushing.
    struct PendingTouchMove
    {
        bool valid = false;
        QPoint framePos;
        QSize frameSize;
    };
    void dispatchOrQueueTouchMove(int slot, const QPoint &framePos, const QSize &frameSize);
    void flushPendingTouchMoves();

private:
    QPointer<Receiver> m_receiver;
    QPointer<InputConvertBase> m_inputConvert;
    std::function<qint64(const QByteArray&)> m_sendData = Q_NULLPTR;
    QSize m_pendingResize;
    bool m_resizeQueued = false;
    bool m_cameraMode = false;
    QString m_serial;
    // Hardware-key / `input keyevent` path only now (postPower/postVolumeUp/
    // postVolumeDown/sendRealKeyEvent) - no longer a touch fallback, see
    // sendRealTouch()/sendRealScroll().
    QPointer<AdbSendEventSession> m_realTouchSession;

    // Daemon-backed touch path - see ensureRawInputDaemon(). This is now the
    // *only* touch path: sendRealTouch()/sendRealScroll() drop the event and
    // log a warning if the daemon isn't available, instead of silently
    // falling back to AdbSendEventSession's sendevent-based touch (removed -
    // that fallback was masking daemon-start failures, since sendevent never
    // touches the daemon's own on-device log).
    QPointer<RawInputDaemonSession> m_rawInputDaemonSession;
    bool m_rawInputDaemonAttempted = false; // tried-once latch, see ensureRawInputDaemon()
    bool m_rawInputDaemonAvailable = false; // true only if the daemon is up and usable for touch
    bool m_rawInputDaemonEnabled = true;    // master toggle, see setRawInputDaemonEnabled()
    bool m_touchFlushConnected = false;     // guards connecting to writesFlushed() more than once

    QMap<int, PendingTouchMove> m_pendingTouchMoves;
};

#endif // CONTROLLER_H