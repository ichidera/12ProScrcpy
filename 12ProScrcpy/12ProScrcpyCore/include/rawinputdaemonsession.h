#pragma once

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QSize>
#include <QString>

class QTcpSocket;

// PC-side counterpart to the on-device qtscrcpy_raw_input_daemon
// (src/rawinputdaemon/raw_input_daemon.c), implementing
// docs/daemon-implementation-plan.md section 2.
//
// Parallel to AdbSendEventSession, not a replacement of it - see plan §2.2.
// Controller tries this first for touch and falls back to
// AdbSendEventSession's raw-panel sendevent path on any startup failure.
// Hardware keys (Power/Volume/Home/Back/Menu) stay on AdbSendEventSession
// unconditionally in v1 - see plan §0 scope.
class RawInputDaemonSession : public QObject
{
    Q_OBJECT

public:
    explicit RawInputDaemonSession(QObject *parent = nullptr);
    virtual ~RawInputDaemonSession();

    // Pushes the daemon binary, launches it (su-elevated), sets up
    // `adb forward` to its abstract-namespace socket, and connects.
    // Returns false on ANY failure along that chain - push fails, exec
    // fails, wrong ABI, `adb forward` fails, socket connect fails. Callers
    // (Controller::sendRealTouch/sendRealScroll) drop the touch event and
    // log a warning on failure - there is no AdbSendEventSession sendevent
    // fallback for touch anymore (removed: it silently masked daemon-start
    // failures, since sendevent never touches this daemon or its on-device
    // log). Never throws, never blocks longer than a few seconds (bounded
    // by the waitFor*() timeouts on each step).
    bool start(const QString &serial);
    void stop();
    bool isRunning() const;

    // Wire protocol (plan §3). Coordinates are frame-space (mirrored-window
    // pixels) - the daemon does frame->panel scaling and the rotation
    // transform itself.
    void touchDown(int slot, int trackId, const QPoint &framePos, const QSize &frameSize);
    void touchMove(int slot, const QPoint &framePos, const QSize &frameSize);
    void touchUp(int slot);

    // Explicit push, e.g. from Controller::resizeDisplay() - also called
    // lazily by touchDown()/touchMove() whenever frameSize changes, so
    // callers don't strictly need to call this themselves.
    void ensureFrameSize(const QSize &frameSize);

    // True once we've written a line the OS socket send buffer hasn't
    // fully accepted yet. Lets Controller dispatch every MOVE the instant
    // it arrives (no periodic-timer lag) while still coalescing to "latest
    // position wins" if the link is ever the actual bottleneck, instead of
    // an arbitrary fixed tick that adds phase lag on every single move
    // regardless of whether the link needed it.
    bool hasPendingWrites() const;

    // Local path the daemon binary is expected to live at (next to
    // adb.exe/scrcpy-server, i.e. QCoreApplication::applicationDirPath()),
    // overridable via the QTSCRCPY_RAW_INPUT_DAEMON_PATH env var - same
    // pattern Dialog::getServerPath() already uses for scrcpy-server.
    static const QString &localBinaryPath();

    static constexpr const char *kRemoteBinaryPath = "/data/local/tmp/qtscrcpy_raw_input_daemon";
    static constexpr const char *kAbstractSocketName = "qtscrcpy_raw_input_daemon";

signals:
    void sessionStarted();
    void sessionError(const QString &message);
    // Fires once the socket has fully flushed everything handed to it so
    // far (bytesToWrite() back to 0) - Controller uses this to send any
    // MOVE that arrived while a previous write was still draining, instead
    // of polling on a timer.
    void writesFlushed();

private:
    bool pushDaemon(const QString &serial);
    bool launchDaemon(const QString &serial);
    bool setupForward(const QString &serial);
    void teardownForward();
    void writeLine(const QString &line);
    void fail(const QString &message);

    QString m_serial;
    quint16 m_localPort = 0;
    QPointer<QTcpSocket> m_socket;
    bool m_started = false;
    QSize m_lastSentFrameSize;
};