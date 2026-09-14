#include "rawinputdaemonsession.h"
#include "adbprocessimpl.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <QRegularExpression>

// Raw BSD socket headers for the hot-path send (Fix 3).
#ifdef Q_OS_WIN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "Ws2_32.lib")
   using SockFd = SOCKET;
   static constexpr SockFd kInvalidSock = INVALID_SOCKET;
#  define SOCK_SEND(fd, buf, len) ::send((fd), reinterpret_cast<const char*>(buf), static_cast<int>(len), 0)
#  define SOCK_CLOSE(fd)          ::closesocket(fd)
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <fcntl.h>
#  include <sys/ioctl.h>
#  include <linux/sockios.h>
#  include <unistd.h>
   using SockFd = int;
   static constexpr SockFd kInvalidSock = -1;
#  define SOCK_SEND(fd, buf, len) ::send((fd), (buf), (len), MSG_DONTWAIT)
#  define SOCK_CLOSE(fd)          ::close(fd)
#endif

namespace {
constexpr int kPushTimeoutMs         = 5000;
constexpr int kLaunchTimeoutMs       = 3000;
constexpr int kRndisResolveTimeoutMs = 3000;
// connect() timeout in ms — we use a non-blocking connect + select() so we
// don't block the caller longer than this on a dead/wrong IP.
constexpr int kConnectTimeoutMs      = 2000;
} // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Startup sequence
// ---------------------------------------------------------------------------

bool RawInputDaemonSession::pushDaemon(const QString &serial)
{
    const QFileInfo info(localBinaryPath());
    if (!info.isFile()) {
        fail(QString("daemon binary not found at %1 (not built/bundled)")
                 .arg(localBinaryPath()));
        return false;
    }

    auto adb = [&](QStringList extra) -> bool {
        QProcess p;
        QStringList args;
        if (!serial.isEmpty()) args << "-s" << serial;
        args << extra;
        p.start(AdbProcessImpl::getAdbPath(), args);
        if (!p.waitForFinished(kPushTimeoutMs)) { p.kill(); return false; }
        return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    };

    if (!adb({"push", localBinaryPath(), kRemoteBinaryPath})) {
        fail("adb push failed");
        return false;
    }
    // Executable bit doesn't survive adb push on all vendors.
    if (!adb({"shell", "chmod", "755", kRemoteBinaryPath})) {
        fail("chmod on daemon binary failed");
        return false;
    }
    return true;
}

bool RawInputDaemonSession::launchDaemon(const QString &serial)
{
    // The daemon double-forks so this adb shell returns immediately after
    // the first child exits — it does not stay attached for the daemon's life.
    QProcess launch;
    QStringList args;
    if (!serial.isEmpty()) args << "-s" << serial;
    args << "shell" << "su" << "-c" << kRemoteBinaryPath;
    launch.start(AdbProcessImpl::getAdbPath(), args);
    if (!launch.waitForFinished(kLaunchTimeoutMs)) {
        launch.kill();
        fail("daemon launch (adb shell su -c) timed out");
        return false;
    }
    if (launch.exitStatus() != QProcess::NormalExit || launch.exitCode() != 0) {
        fail(QString("daemon launch failed: %1")
                 .arg(QString::fromUtf8(launch.readAllStandardError())));
        return false;
    }
    return true;
}

bool RawInputDaemonSession::resolveRndisAddress(const QString &serial)
{
    // `ip -o addr show rndis0` output:
    //   2: rndis0    inet 192.168.42.129/24 brd ...
    // We try rndis0 (Qualcomm/stock Android), then usb0 (Samsung/MediaTek).
    static const QStringList kCandidates = {"rndis0", "usb0"};
    for (const QString &iface : kCandidates) {
        QProcess probe;
        QStringList args;
        if (!serial.isEmpty()) args << "-s" << serial;
        args << "shell" << "ip" << "-o" << "addr" << "show" << iface;
        probe.start(AdbProcessImpl::getAdbPath(), args);
        if (!probe.waitForFinished(kRndisResolveTimeoutMs)) {
            probe.kill();
            continue;
        }
        const QString out = QString::fromUtf8(probe.readAllStandardOutput());
        const QStringList tokens = out.split(QRegularExpression("\\s+"));
        for (int i = 0; i < tokens.size() - 1; ++i) {
            if (tokens[i] == "inet") {
                const QString addr = tokens[i + 1].section('/', 0, 0);
                if (!addr.isEmpty() && addr != "127.0.0.1") {
                    m_rndisAddress = addr;
                    qDebug() << "RawInputDaemonSession: RNDIS address resolved:"
                             << m_rndisAddress << "via" << iface;
                    return true;
                }
            }
        }
    }
    fail("could not resolve USB-RNDIS IP (tried rndis0, usb0) — "
         "is USB tethering/RNDIS active?");
    return false;
}

// ---------------------------------------------------------------------------
// Fix 3: raw BSD connect with non-blocking timeout, then extract native fd.
//
// We do the connect ourselves (not via QTcpSocket) so we own the fd directly
// and can call ::send() on the hot path without any Qt write-buffer layer.
// ---------------------------------------------------------------------------

static SockFd connectRawTcp(const QString &host, quint16 port, int timeoutMs)
{
#ifdef Q_OS_WIN
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    SockFd fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSock) return kInvalidSock;

    // TCP_NODELAY on the send socket — no Nagle batching on the PC side.
    int one = 1;
    ::setsockopt(fd,
#ifdef Q_OS_WIN
                 IPPROTO_TCP,
#else
                 IPPROTO_TCP,
#endif
                 TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));

    // Set non-blocking so connect() returns immediately and we select() with
    // a real timeout rather than blocking indefinitely.
#ifdef Q_OS_WIN
    u_long nb = 1;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    ::inet_pton(AF_INET, host.toLatin1().constData(), &addr.sin_addr);

    int rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

#ifdef Q_OS_WIN
    bool inProgress = (rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK);
#else
    bool inProgress = (rc < 0 && errno == EINPROGRESS);
#endif

    if (rc != 0 && !inProgress) {
        SOCK_CLOSE(fd);
        return kInvalidSock;
    }

    if (inProgress) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
#ifdef Q_OS_WIN
        rc = ::select(0, nullptr, &wfds, nullptr, &tv);
#else
        rc = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
#endif
        if (rc <= 0) {
            SOCK_CLOSE(fd);
            return kInvalidSock;
        }
        // Confirm the connect actually succeeded (select wakes on error too).
        int err = 0;
        socklen_t errlen = sizeof(err);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&err), &errlen);
        if (err != 0) {
            SOCK_CLOSE(fd);
            return kInvalidSock;
        }
    }

    // Restore blocking mode — read path (not used on hot path) expects it.
#ifdef Q_OS_WIN
    u_long nb2 = 0;
    ioctlsocket(fd, FIONBIO, &nb2);
#else
    ::fcntl(fd, F_SETFL, flags); // restore original flags (blocking)
#endif

    return fd;
}

// ---------------------------------------------------------------------------
// Public lifecycle
// ---------------------------------------------------------------------------

bool RawInputDaemonSession::start(const QString &serial)
{
    if (m_started) return true;
    m_serial = serial;
    m_lastSentFrameSize = QSize();

    if (!pushDaemon(serial))        return false;
    if (!launchDaemon(serial))      return false;
    if (!resolveRndisAddress(serial)) return false;

    // Small settle delay — daemon double-forks and then opens the listen
    // socket; without this, connect() can race against bind()/listen().
    QThread::msleep(80);

    m_sockfd = connectRawTcp(m_rndisAddress, kDaemonPort, kConnectTimeoutMs);
    if (m_sockfd == kInvalidSock) {
        fail(QString("could not connect to daemon at %1:%2 over RNDIS")
                 .arg(m_rndisAddress).arg(kDaemonPort));
        return false;
    }

    m_started = true;
    emit sessionStarted();
    qDebug() << "RawInputDaemonSession: connected to daemon at"
             << m_rndisAddress << "port" << kDaemonPort
             << "(raw fd" << m_sockfd << ", binary protocol)";
    return true;
}

void RawInputDaemonSession::stop()
{
    if (!m_started) return;

    // Send QUIT packet before closing.
    uint8_t pkt[kPktSize] = { kCmdQuit, 0, 0, 0, 0, 0, 0, 0 };
    SOCK_SEND(m_sockfd, pkt, kPktSize);

    SOCK_CLOSE(m_sockfd);
    m_sockfd  = kInvalidSock;
    m_started = false;
    m_rndisAddress.clear();
}

bool RawInputDaemonSession::isRunning() const
{
    return m_started && m_sockfd != kInvalidSock;
}

bool RawInputDaemonSession::hasPendingWrites() const
{
    // With a raw blocking socket and MSG_DONTWAIT sends, we can't query
    // bytesToWrite() like QTcpSocket. Instead, check the OS send buffer
    // level via ioctl SIOCOUTQ (Linux) / TIOCOUTQ.  If unavailable (Windows
    // or non-Linux), always return false — Controller will send every MOVE
    // immediately, which is the desired behaviour now that the hot path is
    // a direct syscall.
#if defined(Q_OS_LINUX)
    if (m_sockfd == kInvalidSock) return false;
    int pending = 0;
    if (::ioctl(m_sockfd, SIOCOUTQ, &pending) == 0) {
        return pending > 0;
    }
#endif
    return false;
}

// ---------------------------------------------------------------------------
// Fix 3: hot-path packet send — raw ::send(), no Qt involvement
// ---------------------------------------------------------------------------

void RawInputDaemonSession::sendPacket(const uint8_t pkt[kPktSize])
{
    if (m_sockfd == kInvalidSock) return;
    // MSG_DONTWAIT: if the send buffer is momentarily full, drop this packet
    // rather than blocking. For MOVE this is fine — Controller coalesces to
    // "latest position wins" anyway. For DOWN/UP the buffer should never be
    // full since those are rare relative to MOVE.
    ssize_t n = SOCK_SEND(m_sockfd, pkt, kPktSize);
    if (n != kPktSize) {
        qWarning() << "RawInputDaemonSession: sendPacket short/failed, cmd="
                   << Qt::hex << pkt[0];
    }
    // With a raw send there is no async drain callback. Emit writesFlushed()
    // immediately — hasPendingWrites() is almost always false on a USB link
    // so Controller will just call touchMove() directly anyway, but emitting
    // here keeps the rare queued-MOVE path in Controller::flushPendingTouchMoves()
    // draining correctly without any timer.
    emit writesFlushed();
}

// ---------------------------------------------------------------------------
// Public wire-protocol methods
// ---------------------------------------------------------------------------

void RawInputDaemonSession::ensureFrameSize(const QSize &frameSize)
{
    if (frameSize == m_lastSentFrameSize
            || frameSize.width()  <= 0
            || frameSize.height() <= 0) {
        return;
    }
    uint8_t pkt[kPktSize] = {};
    pkt[0] = kCmdFrame;
    pkt[1] = 0; // pad
    putI16(pkt + 2, static_cast<int16_t>(frameSize.width()));
    putI16(pkt + 4, static_cast<int16_t>(frameSize.height()));
    // bytes 6-7 unused for FRAME
    sendPacket(pkt);
    m_lastSentFrameSize = frameSize;
}

void RawInputDaemonSession::touchDown(int slot, int trackId,
                                      const QPoint &framePos,
                                      const QSize  &frameSize)
{
    ensureFrameSize(frameSize);
    uint8_t pkt[kPktSize] = {};
    pkt[0] = kCmdDown;
    pkt[1] = static_cast<uint8_t>(slot);
    putI16(pkt + 2, static_cast<int16_t>(trackId));
    putI16(pkt + 4, static_cast<int16_t>(framePos.x()));
    putI16(pkt + 6, static_cast<int16_t>(framePos.y()));
    sendPacket(pkt);
}

void RawInputDaemonSession::touchMove(int slot,
                                      const QPoint &framePos,
                                      const QSize  &frameSize)
{
    ensureFrameSize(frameSize);
    uint8_t pkt[kPktSize] = {};
    pkt[0] = kCmdMove;
    pkt[1] = static_cast<uint8_t>(slot);
    putI16(pkt + 2, static_cast<int16_t>(framePos.x()));
    putI16(pkt + 4, static_cast<int16_t>(framePos.y()));
    // bytes 6-7 unused for MOVE
    sendPacket(pkt);
}

void RawInputDaemonSession::touchUp(int slot)
{
    uint8_t pkt[kPktSize] = {};
    pkt[0] = kCmdUp;
    pkt[1] = static_cast<uint8_t>(slot);
    // bytes 2-7 unused for UP
    sendPacket(pkt);
}
