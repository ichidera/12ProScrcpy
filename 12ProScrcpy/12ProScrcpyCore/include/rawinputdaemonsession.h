#pragma once

#include <QObject>
#include <QPoint>
#include <QSize>
#include <QString>

#include <cstdint>

// PC-side counterpart to the on-device qtscrcpy_raw_input_daemon.
//
// Wire protocol: fixed 8-byte binary packets, big-endian.
//
//   Byte 0    command   CMD_DOWN=0x01  CMD_MOVE=0x02  CMD_UP=0x03
//                       CMD_FRAME=0x04  CMD_QUIT=0xFF
//   Byte 1    slot      touch slot (0..9); pad for FRAME/QUIT
//   Bytes 2-3 int16_t A trackId (DOWN) / x (MOVE) / width  (FRAME)
//   Bytes 4-5 int16_t B x (DOWN)       / y (MOVE) / height (FRAME)
//   Bytes 6-7 int16_t C y (DOWN)       / unused otherwise
//
// Hot-path write: raw BSD/Winsock ::send() directly on m_sockfd, bypassing
// Qt's write buffer and event loop entirely.  No QString, no UTF-8 encode,
// no QTcpSocket round-trip on the MOVE path.
class RawInputDaemonSession : public QObject
{
    Q_OBJECT

public:
    explicit RawInputDaemonSession(QObject *parent = nullptr);
    ~RawInputDaemonSession() override;

    bool start(const QString &serial);
    void stop();
    bool isRunning() const;

    void touchDown(int slot, int trackId, const QPoint &framePos, const QSize &frameSize);
    void touchMove(int slot, const QPoint &framePos, const QSize &frameSize);
    void touchUp(int slot);
    void ensureFrameSize(const QSize &frameSize);

    // Always false with raw non-blocking sends — Controller::dispatchOrQueueTouchMove()
    // therefore always takes the immediate path.  writesFlushed() is still
    // emitted from sendPacket() so flushPendingTouchMoves() stays wired.
    bool hasPendingWrites() const;

    static const QString &localBinaryPath();

    static constexpr const char *kRemoteBinaryPath = "/data/local/tmp/qtscrcpy_raw_input_daemon";
    static constexpr quint16     kDaemonPort        = 28820;

    // Command bytes — mirror daemon's CMD_* defines exactly.
    static constexpr uint8_t kCmdDown  = 0x01;
    static constexpr uint8_t kCmdMove  = 0x02;
    static constexpr uint8_t kCmdUp    = 0x03;
    static constexpr uint8_t kCmdFrame = 0x04;
    static constexpr uint8_t kCmdQuit  = 0xFF;
    static constexpr int     kPktSize  = 8;

signals:
    void sessionStarted();
    void sessionError(const QString &message);
    void writesFlushed();

private:
    bool pushDaemon(const QString &serial);
    bool launchDaemon(const QString &serial);
    bool resolveRndisAddress(const QString &serial);
    // Fire-and-forget: drops the packet on EAGAIN/EWOULDBLOCK. Fine for
    // MOVE, which Controller::dispatchOrQueueTouchMove() already coalesces
    // ("send the newest position" beats "send every position").
    void sendPacket(const uint8_t pkt[kPktSize]);
    // Same wire send, but retries briefly on EAGAIN/EWOULDBLOCK instead of
    // dropping. Used for DOWN/UP/FRAME: unlike MOVE these are discrete,
    // non-coalescable events - a dropped DOWN never taps at all, and a
    // dropped UP leaves a stuck touch point on the phone until something
    // else touches that slot. See BUG FIX comment at the definition.
    void sendPacketReliable(const uint8_t pkt[kPktSize]);
    void fail(const QString &message);

    // Encode v as big-endian int16_t into dst[0..1].
    static void putI16(uint8_t *dst, int16_t v)
    {
        dst[0] = static_cast<uint8_t>((static_cast<uint16_t>(v) >> 8) & 0xFF);
        dst[1] = static_cast<uint8_t>( static_cast<uint16_t>(v)       & 0xFF);
    }

    QString  m_serial;
    QString  m_rndisAddress;
    // Raw native socket fd (int on POSIX, SOCKET/uintptr_t on Windows).
    // Stored as qintptr so the header compiles without platform socket headers.
    qintptr  m_sockfd  = -1;
    bool     m_started = false;
    QSize    m_lastSentFrameSize;
};