#pragma once

#include <QObject>
#include <QPoint>
#include <QSize>
#include <QString>

// PC-side counterpart to the on-device qtscrcpy_raw_input_daemon
// (src/rawinputdaemon/raw_input_daemon.c).
//
// Wire protocol: fixed 8-byte binary packets, big-endian (matches daemon's
// PKT_SIZE / CMD_* defines exactly):
//
//   Byte 0   command  CMD_DOWN=0x01 CMD_MOVE=0x02 CMD_UP=0x03
//                     CMD_FRAME=0x04 CMD_QUIT=0xFF
//   Byte 1   slot     touch slot (0..9); pad byte for FRAME/QUIT
//   Bytes 2-3 int16_t A  trackId (DOWN) / x (MOVE) / width (FRAME)
//   Bytes 4-5 int16_t B  x (DOWN)       / y (MOVE) / height (FRAME)
//   Bytes 6-7 int16_t C  y (DOWN)       / unused otherwise
//
// Hot-path write: raw BSD ::send() with MSG_DONTWAIT on m_sockfd, bypassing
// Qt's internal write buffer entirely.  No QString formatting, no UTF-8
// encode, no QTcpSocket event-loop round-trip on MOVE.
class RawInputDaemonSession : public QObject
{
    Q_OBJECT

public:
    explicit RawInputDaemonSession(QObject *parent = nullptr);
    ~RawInputDaemonSession() override;

    // Pushes the daemon binary, launches it (su-elevated), resolves the
    // phone's USB-RNDIS interface IP, and connects directly over TCP.
    // Returns false on ANY failure — push, exec, RNDIS resolve, or connect.
    bool start(const QString &serial);
    void stop();
    bool isRunning() const;

    // Binary wire protocol. Coordinates are frame-space (mirrored-window
    // pixels) — the daemon owns the frame→panel transform.
    void touchDown(int slot, int trackId, const QPoint &framePos, const QSize &frameSize);
    void touchMove(int slot, const QPoint &framePos, const QSize &frameSize);
    void touchUp(int slot);

    // Push frame dimensions eagerly (e.g. from resizeDisplay()). Also called
    // lazily by touchDown/touchMove whenever frameSize changes.
    void ensureFrameSize(const QSize &frameSize);

    // True when the OS socket send buffer has unacknowledged bytes — lets
    // Controller coalesce to "latest MOVE wins" without a polling timer.
    bool hasPendingWrites() const;

    static const QString &localBinaryPath();

    static constexpr const char *kRemoteBinaryPath = "/data/local/tmp/qtscrcpy_raw_input_daemon";
    static constexpr quint16     kDaemonPort        = 28820;

    // Binary packet command bytes (mirror daemon's CMD_* defines).
    static constexpr uint8_t kCmdDown  = 0x01;
    static constexpr uint8_t kCmdMove  = 0x02;
    static constexpr uint8_t kCmdUp    = 0x03;
    static constexpr uint8_t kCmdFrame = 0x04;
    static constexpr uint8_t kCmdQuit  = 0xFF;
    static constexpr int     kPktSize  = 8;

signals:
    void sessionStarted();
    void sessionError(const QString &message);
    // Fires when the OS send buffer drains fully — Controller uses this to
    // flush any MOVE that arrived while a previous packet was still in-flight.
    void writesFlushed();

private:
    bool pushDaemon(const QString &serial);
    bool launchDaemon(const QString &serial);
    // Resolves the phone's USB-RNDIS IP via `adb shell ip -o addr show rndis0`
    // (falls back to usb0). Writes into m_rndisAddress.
    bool resolveRndisAddress(const QString &serial);

    // Fix 3: raw send on the native socket fd — no Qt write buffer, no
    // event-loop round-trip. MSG_DONTWAIT means it never blocks; if the send
    // buffer is momentarily full, the MOVE is dropped (same as the old
    // hasPendingWrites() coalesce path, but without the QTcpSocket overhead).
    void sendPacket(const uint8_t pkt[kPktSize]);

    // Encode big-endian int16_t into two bytes at dst.
    static void putI16(uint8_t *dst, int16_t v) {
        dst[0] = static_cast<uint8_t>((static_cast<uint16_t>(v) >> 8) & 0xFF);
        dst[1] = static_cast<uint8_t>( static_cast<uint16_t>(v)       & 0xFF);
    }

    void fail(const QString &message);

    QString m_serial;
    QString m_rndisAddress;
    int     m_sockfd   = -1;   // raw BSD socket fd — owned by us, closed in stop()
    bool    m_started  = false;
    QSize   m_lastSentFrameSize;
};
