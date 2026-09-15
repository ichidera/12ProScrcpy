#include "rawinputdaemonsession.h"
#include "adbprocessimpl.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "Ws2_32.lib")
   using SockFd   = SOCKET;
   using SendLen  = int;
   static constexpr SockFd kInvalidSock = INVALID_SOCKET;
#  define SOCK_CLOSE(fd) ::closesocket(fd)
   // Winsock has no MSG_DONTWAIT. The socket is left non-blocking after
   // connect so send() returns WSAEWOULDBLOCK immediately if the buffer is
   // full — same drop-on-full semantics as MSG_DONTWAIT on Linux.
#  define SOCK_SEND(fd, buf, len) ::send((fd), reinterpret_cast<const char*>(buf), static_cast<int>(len), 0)
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using SockFd   = int;
   using SendLen  = ssize_t;
   static constexpr SockFd kInvalidSock = -1;
#  define SOCK_CLOSE(fd) ::close(fd)
#  define SOCK_SEND(fd, buf, len) ::send((fd), (buf), (len), MSG_DONTWAIT)
#endif

namespace {
constexpr int kPushTimeoutMs         = 5000;
constexpr int kLaunchTimeoutMs       = 3000;
constexpr int kRndisResolveTimeoutMs = 3000;
constexpr int kConnectTimeoutMs      = 2000;

// Cast m_sockfd (stored as qintptr) back to the platform socket type.
inline SockFd toSockFd(qintptr fd) { return static_cast<SockFd>(fd); }
inline bool   sockValid(qintptr fd) { return toSockFd(fd) != kInvalidSock; }
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
        if (path.isEmpty())
            path = QCoreApplication::applicationDirPath() + "/qtscrcpy_raw_input_daemon";
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
    if (!QFileInfo(localBinaryPath()).isFile()) {
        fail(QString("daemon binary not found at %1").arg(localBinaryPath()));
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
    if (!adb({"shell", "chmod", "755", kRemoteBinaryPath})) {
        fail("chmod on daemon binary failed");
        return false;
    }
    return true;
}

bool RawInputDaemonSession::launchDaemon(const QString &serial)
{
    QProcess launch;
    QStringList args;
    if (!serial.isEmpty()) args << "-s" << serial;
    args << "shell" << "su" << "-c" << kRemoteBinaryPath;
    launch.start(AdbProcessImpl::getAdbPath(), args);
    if (!launch.waitForFinished(kLaunchTimeoutMs)) {
        launch.kill();
        fail("daemon launch timed out");
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
    static const QStringList kCandidates = {"rndis0", "usb0"};
    for (const QString &iface : kCandidates) {
        QProcess probe;
        QStringList args;
        if (!serial.isEmpty()) args << "-s" << serial;
        args << "shell" << "ip" << "-o" << "addr" << "show" << iface;
        probe.start(AdbProcessImpl::getAdbPath(), args);
        if (!probe.waitForFinished(kRndisResolveTimeoutMs)) { probe.kill(); continue; }

        const QString out = QString::fromUtf8(probe.readAllStandardOutput());
        const QStringList tokens = out.split(QRegularExpression("\\s+"));
        for (int i = 0; i < tokens.size() - 1; ++i) {
            if (tokens[i] == "inet") {
                const QString addr = tokens[i + 1].section('/', 0, 0);
                if (!addr.isEmpty() && addr != "127.0.0.1") {
                    m_rndisAddress = addr;
                    qDebug() << "RawInputDaemonSession: RNDIS address" << m_rndisAddress << "via" << iface;
                    return true;
                }
            }
        }
    }
    fail("could not resolve USB-RNDIS IP (tried rndis0, usb0) — is USB tethering active?");
    return false;
}

// ---------------------------------------------------------------------------
// Raw TCP connect: non-blocking with timeout, TCP_NODELAY set before connect.
// Returns the native socket fd/SOCKET, or kInvalidSock on failure.
// On Windows the socket stays non-blocking after connect — send() returns
// WSAEWOULDBLOCK instead of blocking, giving us drop-on-full for free.
// On Linux we restore blocking after connect; MSG_DONTWAIT on send() handles
// the drop-on-full case without keeping the socket permanently non-blocking.
// ---------------------------------------------------------------------------

static SockFd connectRawTcp(const QString &host, quint16 port, int timeoutMs)
{
#ifdef Q_OS_WIN
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    SockFd fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSock) return kInvalidSock;

    // TCP_NODELAY: disable Nagle on the send side so every 8-byte packet
    // goes on the wire immediately without waiting for an ACK or more data.
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char *>(&one), sizeof(one));

    // Non-blocking for the connect phase so we can time it out cleanly.
#ifdef Q_OS_WIN
    u_long nb = 1;
    ::ioctlsocket(fd, FIONBIO, &nb);
#else
    int savedFlags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, savedFlags | O_NONBLOCK);
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    ::inet_pton(AF_INET, host.toLatin1().constData(), &addr.sin_addr);

    int rc = ::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));

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
        struct timeval tv;
        tv.tv_sec  = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef Q_OS_WIN
        rc = ::select(0, nullptr, &wfds, nullptr, &tv);
#else
        rc = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
#endif
        if (rc <= 0) { SOCK_CLOSE(fd); return kInvalidSock; }

        // select() wakes on error too — confirm success via SO_ERROR.
        int err = 0;
#ifdef Q_OS_WIN
        int errlen = sizeof(err);
#else
        socklen_t errlen = sizeof(err);
#endif
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char *>(&err), &errlen);
        if (err != 0) { SOCK_CLOSE(fd); return kInvalidSock; }
    }

    // Linux: restore blocking — MSG_DONTWAIT on each send() is enough;
    // keeping the socket non-blocking permanently adds complexity for no gain.
    // Windows: leave non-blocking — send() returning WSAEWOULDBLOCK is how
    // we get drop-on-full without MSG_DONTWAIT.
#ifndef Q_OS_WIN
    ::fcntl(fd, F_SETFL, savedFlags);
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

    if (!pushDaemon(serial))          return false;
    if (!launchDaemon(serial))        return false;
    if (!resolveRndisAddress(serial)) return false;

    // Daemon double-forks before bind()/listen(); small settle so we don't
    // race connect() against the daemon's listen socket being ready.
    QThread::msleep(80);

    SockFd rawFd = connectRawTcp(m_rndisAddress, kDaemonPort, kConnectTimeoutMs);
    m_sockfd = static_cast<qintptr>(rawFd);
    if (!sockValid(m_sockfd)) {
        fail(QString("could not connect to daemon at %1:%2 over RNDIS")
                 .arg(m_rndisAddress).arg(kDaemonPort));
        return false;
    }

    m_started = true;
    emit sessionStarted();
    qDebug() << "RawInputDaemonSession: connected to" << m_rndisAddress
             << "port" << kDaemonPort << "(binary protocol, raw fd)";
    return true;
}

void RawInputDaemonSession::stop()
{
    if (!m_started) return;
    uint8_t pkt[kPktSize] = { kCmdQuit, 0, 0, 0, 0, 0, 0, 0 };
    SOCK_SEND(toSockFd(m_sockfd), pkt, kPktSize);
    SOCK_CLOSE(toSockFd(m_sockfd));
    m_sockfd = static_cast<qintptr>(kInvalidSock);
    m_started = false;
    m_rndisAddress.clear();
}

bool RawInputDaemonSession::isRunning() const
{
    return m_started && sockValid(m_sockfd);
}

bool RawInputDaemonSession::hasPendingWrites() const
{
    // Always false — with raw non-blocking/MSG_DONTWAIT sends, the kernel
    // either accepts the packet immediately or drops it; there is no async
    // drain to wait for. Controller::dispatchOrQueueTouchMove() will
    // therefore always take the fast path (send immediately) and never queue.
    // writesFlushed() is emitted synchronously from sendPacket() to keep
    // Controller::flushPendingTouchMoves() wired up correctly for any
    // future case where queuing is re-introduced.
    return false;
}

// ---------------------------------------------------------------------------
// Hot-path packet send — raw ::send(), no Qt write buffer, no event loop
// ---------------------------------------------------------------------------

void RawInputDaemonSession::sendPacket(const uint8_t pkt[kPktSize])
{
    if (!sockValid(m_sockfd)) return;

    SendLen n = SOCK_SEND(toSockFd(m_sockfd), pkt, kPktSize);

    // On Linux: n < 0 with errno == EAGAIN means send buffer momentarily
    // full — packet dropped intentionally (MOVE coalescing handles this).
    // On Windows: n == SOCKET_ERROR with WSAEWOULDBLOCK is the same case.
    // Any other error is unexpected; log it but don't tear down the session.
    if (n != static_cast<SendLen>(kPktSize)) {
#ifdef Q_OS_WIN
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK)
            qWarning() << "RawInputDaemonSession: send failed, WSAError=" << e;
#else
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            qWarning() << "RawInputDaemonSession: send failed, errno=" << errno;
#endif
    }

    // Emit synchronously — hasPendingWrites() is always false so Controller
    // goes straight through to touchMove() on the next MOVE; this signal
    // keeps flushPendingTouchMoves() wired without a polling timer.
    emit writesFlushed();
}

// ---------------------------------------------------------------------------
// Public wire-protocol methods
// ---------------------------------------------------------------------------

void RawInputDaemonSession::ensureFrameSize(const QSize &frameSize)
{
    if (frameSize == m_lastSentFrameSize
            || frameSize.width()  <= 0
            || frameSize.height() <= 0) return;

    uint8_t pkt[kPktSize] = {};
    pkt[0] = kCmdFrame;
    pkt[1] = 0;
    putI16(pkt + 2, static_cast<int16_t>(frameSize.width()));
    putI16(pkt + 4, static_cast<int16_t>(frameSize.height()));
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
    sendPacket(pkt);
}

void RawInputDaemonSession::touchUp(int slot)
{
    uint8_t pkt[kPktSize] = {};
    pkt[0] = kCmdUp;
    pkt[1] = static_cast<uint8_t>(slot);
    sendPacket(pkt);
}
