#include "rawinputdaemonsession.h"
#include "adbprocessimpl.h" // for AdbProcessImpl::getAdbPath()

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcess>
#include <QRegularExpression>
#include <QTcpSocket>

namespace {
// Bounded timeouts for every blocking step - the daemon start sequence must
// never hang the caller indefinitely (Controller::ensureRealTouchSession()
// is on a hot-ish path). A slow/dead adb link just means "startup failed,
// fall back" per plan §2.2, not "wait forever".
constexpr int kPushTimeoutMs = 5000;
constexpr int kLaunchTimeoutMs = 3000;
constexpr int kRndisResolveTimeoutMs = 3000;
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

bool RawInputDaemonSession::resolveRndisAddress(const QString &serial)
{
    // Ask the device for its USB-RNDIS interface IP directly.
    // `ip -o addr show rndis0` prints lines like:
    //   2: rndis0    inet 192.168.42.129/24 ...
    // We try rndis0 first (most Qualcomm/Android RNDIS), then usb0
    // (some Samsung/MediaTek variants use that name instead).
    static const QStringList kCandidates = {"rndis0", "usb0"};
    for (const QString &iface : kCandidates) {
        QProcess probe;
        QStringList args;
        if (!serial.isEmpty()) {
            args << "-s" << serial;
        }
        args << "shell" << "ip" << "-o" << "addr" << "show" << iface;
        probe.start(AdbProcessImpl::getAdbPath(), args);
        if (!probe.waitForFinished(kRndisResolveTimeoutMs)) {
            probe.kill();
            continue;
        }
        const QString out = QString::fromUtf8(probe.readAllStandardOutput());
        // Parse the first "inet A.B.C.D/prefix" token from the output.
        const QStringList tokens = out.split(QRegularExpression("\\s+"));
        for (int i = 0; i < tokens.size() - 1; ++i) {
            if (tokens[i] == "inet") {
                const QString cidr = tokens[i + 1]; // e.g. "192.168.42.129/24"
                const QString addr = cidr.section('/', 0, 0);
                if (!addr.isEmpty() && addr != "127.0.0.1") {
                    m_rndisAddress = addr;
                    return true;
                }
            }
        }
    }
    fail("could not resolve USB-RNDIS interface IP (tried rndis0, usb0) - "
         "is USB tethering / RNDIS active on the device?");
    return false;
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
    if (!resolveRndisAddress(serial)) {
        return false;
    }

    m_socket = new QTcpSocket(this);
    // Nagle's algorithm batches small writes (our DOWN/MOVE/UP lines are a
    // handful of bytes each) waiting for more data or an ACK before sending.
    // Disable it so every write goes out on the wire immediately - we want
    // each touch event dispatched the instant it's written, not held waiting
    // for the next ACK or a buffer to fill.
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connect(m_socket, &QAbstractSocket::bytesWritten, this, [this]() {
        if (m_socket && m_socket->bytesToWrite() == 0) {
            emit writesFlushed();
        }
    });
    // Connect directly to the phone's USB-RNDIS IP. No adb forward - no
    // ADB server process in this path at all after launch.
    m_socket->connectToHost(m_rndisAddress, kDaemonPort);
    if (!m_socket->waitForConnected(kConnectTimeoutMs)) {
        fail(QString("could not connect to daemon at %1:%2 over RNDIS: %3")
                 .arg(m_rndisAddress)
                 .arg(kDaemonPort)
                 .arg(m_socket->errorString()));
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
    m_rndisAddress.clear();
    m_started = false;
}

bool RawInputDaemonSession::isRunning() const
{
    return m_started && m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
}

bool RawInputDaemonSession::hasPendingWrites() const
{
    return m_socket && m_socket->bytesToWrite() > 0;
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