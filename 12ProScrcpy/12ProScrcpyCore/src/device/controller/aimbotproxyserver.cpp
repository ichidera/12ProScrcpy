#include "aimbotproxyserver.h"

#include <QDebug>
#include <QHostAddress>

// Command bytes — mirror rawinputdaemonsession.h exactly.
static constexpr uint8_t kCmdDown  = 0x01;
static constexpr uint8_t kCmdMove  = 0x02;
static constexpr uint8_t kCmdUp    = 0x03;
static constexpr uint8_t kCmdFrame = 0x04;
static constexpr uint8_t kCmdQuit  = 0xFF;

static inline int16_t getI16be(const uint8_t *p)
{
    return static_cast<int16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

// ---------------------------------------------------------------------------

AimbotProxyServer::AimbotProxyServer(QObject *parent)
    : QObject(parent)
{
}

AimbotProxyServer::~AimbotProxyServer()
{
    stop();
}

bool AimbotProxyServer::start(RawInputDaemonSession *daemonSession)
{
    if (!daemonSession) {
        qWarning() << "AimbotProxyServer: null daemon session";
        return false;
    }
    m_daemon = daemonSession;

    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection,
            this,     &AimbotProxyServer::onNewConnection);

    if (!m_server->listen(QHostAddress::LocalHost, kProxyPort)) {
        qWarning() << "AimbotProxyServer: failed to listen on port" << kProxyPort
                   << "—" << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }

    qInfo() << "AimbotProxyServer: listening on 127.0.0.1:" << kProxyPort;
    return true;
}

void AimbotProxyServer::stop()
{
    dropClient();
    if (m_server) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
}

bool AimbotProxyServer::isListening() const
{
    return m_server && m_server->isListening();
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void AimbotProxyServer::onNewConnection()
{
    QTcpSocket *incoming = m_server->nextPendingConnection();
    if (!incoming) return;

    if (m_client && m_client->state() == QAbstractSocket::ConnectedState) {
        qWarning() << "AimbotProxyServer: rejecting second client — one already connected";
        incoming->disconnectFromHost();
        incoming->deleteLater();
        return;
    }

    m_client  = incoming;
    m_readBuf.clear();

    connect(m_client, &QTcpSocket::readyRead,
            this,     &AimbotProxyServer::onClientReadyRead);
    connect(m_client, &QTcpSocket::disconnected,
            this,     &AimbotProxyServer::onClientDisconnected);

    qInfo() << "AimbotProxyServer: aimbot client connected from"
            << m_client->peerAddress().toString();
    emit clientConnected();
}

void AimbotProxyServer::onClientReadyRead()
{
    if (!m_client) return;
    m_readBuf.append(m_client->readAll());

    // Consume complete 8-byte packets
    while (m_readBuf.size() >= kPktSize) {
        const uint8_t *pkt = reinterpret_cast<const uint8_t *>(m_readBuf.constData());
        processPacket(pkt);
        m_readBuf.remove(0, kPktSize);
    }
}

void AimbotProxyServer::onClientDisconnected()
{
    qInfo() << "AimbotProxyServer: aimbot client disconnected";
    dropClient();
    emit clientDisconnected();
}

// ---------------------------------------------------------------------------
// Packet dispatch
// ---------------------------------------------------------------------------

void AimbotProxyServer::processPacket(const uint8_t *pkt)
{
    if (!m_daemon || !m_daemon->isRunning()) {
        // Daemon isn't up yet or died — drop silently, same as Controller does.
        return;
    }

    const uint8_t cmd  = pkt[0];
    const uint8_t slot = pkt[1];
    const int16_t a    = getI16be(pkt + 2);  // trackId / x / width
    const int16_t b    = getI16be(pkt + 4);  // x / y / height
    const int16_t c    = getI16be(pkt + 6);  // y (DOWN only)

    switch (cmd) {

    case kCmdFrame: {
        // FRAME: a=width, b=height
        if (a > 0 && b > 0)
            m_daemon->ensureFrameSize(QSize(a, b));
        break;
    }

    case kCmdDown: {
        // DOWN: a=trackId, b=x, c=y
        QSize  lastFrame = m_daemon->lastSentFrameSize();
        m_daemon->touchDown(slot, static_cast<int>(a),
                            QPoint(b, c), lastFrame);
        break;
    }

    case kCmdMove: {
        // MOVE: a=x, b=y
        QSize lastFrame = m_daemon->lastSentFrameSize();
        m_daemon->touchMove(slot, QPoint(a, b), lastFrame);
        break;
    }

    case kCmdUp: {
        m_daemon->touchUp(slot);
        break;
    }

    case kCmdQuit: {
        // Client signals it's done — close this client connection only.
        // Does NOT kill the daemon or the proxy server.
        qInfo() << "AimbotProxyServer: CMD_QUIT from client — closing client connection";
        dropClient();
        break;
    }

    default:
        qWarning() << "AimbotProxyServer: unknown command byte" << Qt::hex << cmd << "— ignored";
        break;
    }
}

void AimbotProxyServer::dropClient()
{
    if (m_client) {
        m_client->disconnect(this);
        m_client->abort();
        m_client->deleteLater();
        m_client = nullptr;
    }
    m_readBuf.clear();
}
