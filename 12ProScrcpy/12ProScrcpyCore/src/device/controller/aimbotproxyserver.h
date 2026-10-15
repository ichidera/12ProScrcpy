#pragma once

// AimbotProxyServer
//
// Listens on localhost:28821 for 8-byte binary packets in the same format
// as the on-device qtscrcpy_raw_input_daemon wire protocol, and forwards
// them directly to the RawInputDaemonSession that Controller already owns.
//
// This lets aimbot.py (or any external tool) talk to the daemon through
// 12ProScrcpy without taking the daemon's single client slot away from it,
// keeping the mirror alive while the aimbot runs.
//
// Protocol (identical to rawinputdaemonsession.h):
//   8 bytes, big-endian
//   Byte 0: command  0x01=DOWN  0x02=MOVE  0x03=UP  0x04=FRAME  0xFF=QUIT
//   Byte 1: slot
//   Bytes 2-3: int16 A  (trackId for DOWN, x for MOVE, width for FRAME)
//   Bytes 4-5: int16 B  (x for DOWN, y for MOVE, height for FRAME)
//   Bytes 6-7: int16 C  (y for DOWN, unused otherwise)
//
// Only one aimbot client is accepted at a time. If a second connects while
// one is already connected, the second is rejected immediately. On client
// disconnect the server keeps listening for the next connection.
//
// CMD_QUIT (0xFF) from the client closes that client connection only —
// it does NOT shut down the proxy server or the daemon.

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QPointer>

#include "rawinputdaemonsession.h"

class AimbotProxyServer : public QObject
{
    Q_OBJECT

public:
    static constexpr quint16 kProxyPort = 28821;
    static constexpr int     kPktSize   = 8;

    explicit AimbotProxyServer(QObject *parent = nullptr);
    ~AimbotProxyServer() override;

    // Start listening. daemonSession must outlive this object (Controller
    // owns both, so that's guaranteed). Returns true if the listen socket
    // bound successfully.
    bool start(RawInputDaemonSession *daemonSession);
    void stop();

    bool isListening() const;

signals:
    void clientConnected();
    void clientDisconnected();

private slots:
    void onNewConnection();
    void onClientReadyRead();
    void onClientDisconnected();

private:
    void processPacket(const uint8_t *pkt);
    void dropClient();

    QTcpServer                  *m_server        = nullptr;
    QPointer<QTcpSocket>         m_client;
    QPointer<RawInputDaemonSession> m_daemon;
    QByteArray                   m_readBuf;
};
