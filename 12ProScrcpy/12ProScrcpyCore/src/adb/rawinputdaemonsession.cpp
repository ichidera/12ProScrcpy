#include "rawinputdaemonsession.h"
#include "adbprocessimpl.h" // for AdbProcessImpl::getAdbPath()

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcess>
#include <QTcpSocket>

namespace {
// Bounded timeouts for every blocking step - the daemon start sequence must
// never hang the caller indefinitely (Controller::ensureRealTouchSession()
// is on a hot-ish path). A slow/dead adb link just means "startup failed,
// fall back" per plan §2.2, not "wait forever".
constexpr int kPushTimeoutMs = 5000;
constexpr int kLaunchTimeoutMs = 3000;
constexpr int kForwardTimeoutMs = 3000;
constexpr int kConnectTimeoutMs = 2000;
} // namespace

RawInputDaemonSession::RawInputDaemonSession(QObject *parent)
    : QObject(parent)
{
}

RawInputDaemonSession::~RawInputDaemonSession()
{
    stop();
}

const QString &RawInputDaemonSession::localBinaryPath()
{
    static QString path;
    if (path.isEmpty()) {
        path = QString::fromLocal8Bit(qgetenv("QTSCRCPY_RAW_INPUT_DAEMON_PATH"));
        if (path.isEmpty()) {
            path = QCoreApplication::applicationDirPath() + "/qtscrcpy_raw_input_daemon";
        }
    }
    return path;
}

void RawInputDaemonSession::fail(const QString &message)
{
    qWarning() << "RawInputDaemonSession:" << message;
    emit sessionError(message);
}

bool RawInputDaemonSession::pushDaemon(const QString &serial)
{
    const QFileInfo info(localBinaryPath());
    if (!info.isFile()) {
        fail(QString("daemon binary not found at %1 (not built/bundled - see "
                      "src/rawinputdaemon/README.md)").arg(localBinaryPath()));
        return false;
    }

    QProcess push;
    QStringList args;
    if (!serial.isEmpty()) {
        args << "-s" << serial;
    }
    args << "push" << localBinaryPath() << kRemoteBinaryPath;
    push.start(AdbProcessImpl::getAdbPath(), args);
    if (!push.waitForFinished(kPushTimeoutMs)) {
        fail("adb push timed out");
        return false;
    }
    if (push.exitStatus() != QProcess::NormalExit || push.exitCode() != 0) {
        fail(QString("adb push failed: %1").arg(QString::fromUtf8(push.readAllStandardError())));
        return false;
    }

    // Executable bit doesn't survive `adb push` reliably across all vendors.
    QProcess chmod;
    QStringList chmodArgs;
    if (!serial.isEmpty()) {
        chmodArgs << "-s" << serial;
    }
    chmodArgs << "shell" << "chmod" << "755" << kRemoteBinaryPath;
    chmod.start(AdbProcessImpl::getAdbPath(), chmodArgs);
    if (!chmod.waitForFinished(kPushTimeoutMs) || chmod.exitCode() != 0) {
        fail("chmod on pushed daemon binary failed");
        return false;
    }
    return true;
}

bool RawInputDaemonSession::launchDaemon(const QString &serial)
{
    // The daemon double-forks and detaches itself (see daemonize() in
    // raw_input_daemon.c), so this `adb shell` invocation returns as soon as
    // the first-generation child exits - it does not stay attached for the
    // life of the daemon. `su -c` matches how AdbSendEventSession already
    // escalates for sendevent access to these same nodes.
    QProcess launch;
    QStringList args;
    if (!serial.isEmpty()) {
        args << "-s" << serial;
    }
    args << "shell" << "su" << "-c" << kRemoteBinaryPath;
    launch.start(AdbProcessImpl::getAdbPath(), args);
    if (!launch.waitForFinished(kLaunchTimeoutMs)) {
        fail("daemon launch (adb shell su -c) timed out");
        launch.kill();
        return false;
    }
    if (launch.exitStatus() != QProcess::NormalExit || launch.exitCode() != 0) {
        fail(QString("daemon launch failed: %1").arg(QString::fromUtf8(launch.readAllStandardError())));
        return false;
    }
    return true;
}

bool RawInputDaemonSession::setupForward(const QString &serial)
{
    // Dynamic port (`tcp:0`) rather than a fixed one - per the plan's open
    // questions, this is safer against port collisions if the app is ever
    // run twice / multiple devices are connected simultaneously. `adb`
    // prints the assigned port number on stdout.
    QProcess forward;
    QStringList args;
    if (!serial.isEmpty()) {
        args << "-s" << serial;
    }
    args << "forward" << "tcp:0" << QString("localabstract:%1").arg(kAbstractSocketName);
    forward.start(AdbProcessImpl::getAdbPath(), args);
    if (!forward.waitForFinished(kForwardTimeoutMs)) {
        fail("adb forward timed out");
        return false;
    }
    if (forward.exitStatus() != QProcess::NormalExit || forward.exitCode() != 0) {
        fail(QString("adb forward failed: %1").arg(QString::fromUtf8(forward.readAllStandardError())));
        return false;
    }

    bool ok = false;
    const QString out = QString::fromUtf8(forward.readAllStandardOutput()).trimmed();
    const quint16 port = out.toUShort(&ok);
    if (!ok || port == 0) {
        fail(QString("could not parse forwarded port from adb output: '%1'").arg(out));
        return false;
    }
    m_localPort = port;
    return true;
}

void RawInputDaemonSession::teardownForward()
{
    if (m_localPort == 0) {
        return;
    }
    QProcess remove;
    QStringList args;
    if (!m_serial.isEmpty()) {
        args << "-s" << m_serial;
    }
    args << "forward" << "--remove" << QString("tcp:%1").arg(m_localPort);
    remove.start(AdbProcessImpl::getAdbPath(), args);
    remove.waitForFinished(kForwardTimeoutMs);
    m_localPort = 0;
}

bool RawInputDaemonSession::start(const QString &serial)
{
    if (m_started) {
        return true;
    }
    m_serial = serial;
    m_lastSentFrameSize = QSize();

    if (!pushDaemon(serial)) {
        return false;
    }
    if (!launchDaemon(serial)) {
        return false;
    }
    if (!setupForward(serial)) {
        return false;
    }

    m_socket = new QTcpSocket(this);
    // Nagle's algorithm batches small writes (our DOWN/MOVE/UP lines are a
    // handful of bytes each) waiting for more data or an ACK before sending
    // - on a loopback `adb forward` link that shows up as the cursor
    // visibly continuing to drift for tens of ms after the physical mouse
    // has already stopped, since queued-up coalesced writes keep trickling
    // out after the fact. Disable it before connecting so every write goes
    // out immediately.
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_socket->connectToHost(QHostAddress::LocalHost, m_localPort);
    if (!m_socket->waitForConnected(kConnectTimeoutMs)) {
        fail(QString("could not connect to forwarded daemon socket on port %1: %2")
                 .arg(m_localPort)
                 .arg(m_socket->errorString()));
        teardownForward();
        delete m_socket;
        m_socket = nullptr;
        return false;
    }

    m_started = true;
    emit sessionStarted();
    return true;
}

void RawInputDaemonSession::stop()
{
    if (!m_started) {
        return;
    }
    writeLine("QUIT");
    if (m_socket) {
        m_socket->flush();
        m_socket->waitForBytesWritten(200);
        m_socket->disconnectFromHost();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    teardownForward();
    m_started = false;
}

bool RawInputDaemonSession::isRunning() const
{
    return m_started && m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
}

void RawInputDaemonSession::writeLine(const QString &line)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    m_socket->write((line + "\n").toUtf8());
}

void RawInputDaemonSession::ensureFrameSize(const QSize &frameSize)
{
    if (frameSize == m_lastSentFrameSize || frameSize.width() <= 0 || frameSize.height() <= 0) {
        return;
    }
    writeLine(QString("FRAME %1 %2").arg(frameSize.width()).arg(frameSize.height()));
    m_lastSentFrameSize = frameSize;
}

void RawInputDaemonSession::touchDown(int slot, int trackId, const QPoint &framePos, const QSize &frameSize)
{
    ensureFrameSize(frameSize);
    writeLine(QString("DOWN %1 %2 %3 %4").arg(slot).arg(trackId).arg(framePos.x()).arg(framePos.y()));
}

void RawInputDaemonSession::touchMove(int slot, const QPoint &framePos, const QSize &frameSize)
{
    ensureFrameSize(frameSize);
    writeLine(QString("MOVE %1 %2 %3").arg(slot).arg(framePos.x()).arg(framePos.y()));
}

void RawInputDaemonSession::touchUp(int slot)
{
    writeLine(QString("UP %1").arg(slot));
}