#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QPointF>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QtMath>

#include "adbprocessimpl.h" // for AdbProcessImpl::getAdbPath()
#include "controller.h"
#include "controlmsg.h"
#include "inputconvertgame.h"
#include "receiver.h"
#include "videosocket.h"

Controller::Controller(std::function<qint64(const QByteArray&)> sendData, const QString &serial, QString gameScript, QObject *parent)
    : QObject(parent)
    , m_sendData(sendData)
    , m_serial(serial)
{
    m_receiver = new Receiver(this);
    Q_ASSERT(m_receiver);

    updateScript(gameScript);
}

Controller::~Controller() {}

void Controller::postControlMsg(ControlMsg *controlMsg)
{
    if (!controlMsg) {
        return;
    }

    if (m_cameraMode) {
        const auto type = controlMsg->type();
        const bool isCameraControl = type == ControlMsg::CMT_CAMERA_SET_TORCH
                || type == ControlMsg::CMT_CAMERA_ZOOM_IN
                || type == ControlMsg::CMT_CAMERA_ZOOM_OUT;
        if (!isCameraControl) {
            qWarning() << "Ignoring display control message in camera mode:" << type;
            delete controlMsg;
            return;
        }
    }

    QCoreApplication::postEvent(this, controlMsg);
}

void Controller::setCameraMode(bool cameraMode)
{
    m_cameraMode = cameraMode;
}

void Controller::setRawInputDaemonEnabled(bool enabled)
{
    m_rawInputDaemonEnabled = enabled;
}

void Controller::recvDeviceMsg(DeviceMsg *deviceMsg)
{
    if (!m_receiver) {
        return;
    }

    m_receiver->recvDeviceMsg(deviceMsg);
}

void Controller::updateScript(QString gameScript)
{
    if (m_inputConvert) {
        delete m_inputConvert;
    }
    if (!gameScript.isEmpty()) {
        InputConvertGame *convertgame = new InputConvertGame(this);
        convertgame->loadKeyMap(gameScript);
        m_inputConvert = convertgame;
    } else {
        m_inputConvert = new InputConvertNormal(this);
    }
    Q_ASSERT(m_inputConvert);
    connect(m_inputConvert, &InputConvertBase::grabCursor, this, &Controller::grabCursor);
}

bool Controller::isCurrentCustomKeymap()
{
    if (!m_inputConvert) {
        return false;
    }

    return m_inputConvert->isCurrentCustomKeymap();
}

void Controller::setForceCustomKeymap(bool enabled)
{
    if (!m_inputConvert) {
        return;
    }
    m_inputConvert->setForceCustomKeymap(enabled);
}

void Controller::postBackOrScreenOn(bool down)
{
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_BACK_OR_SCREEN_ON);
    controlMsg->setBackOrScreenOnData(down);
    if (!controlMsg) {
        return;
    }
    postControlMsg(controlMsg);
}

void Controller::postGoHome()
{
    postKeyCodeClick(AKEYCODE_HOME);
}

void Controller::postGoMenu()
{
    postKeyCodeClick(AKEYCODE_MENU);
}

void Controller::postGoBack()
{
    postKeyCodeClick(AKEYCODE_BACK);
}

void Controller::postAppSwitch()
{
    postKeyCodeClick(AKEYCODE_APP_SWITCH);
}

void Controller::postPower()
{
    ensureRealTouchSession();
    if (m_realTouchSession && m_realTouchSession->isRunning()) {
        m_realTouchSession->pressPower();
    }
}

void Controller::postVolumeUp()
{
    ensureRealTouchSession();
    if (m_realTouchSession && m_realTouchSession->isRunning()) {
        m_realTouchSession->pressVolumeUp();
    }
}

void Controller::postVolumeDown()
{
    ensureRealTouchSession();
    if (m_realTouchSession && m_realTouchSession->isRunning()) {
        m_realTouchSession->pressVolumeDown();
    }
}

void Controller::copy()
{
    postKeyCodeClick(AKEYCODE_COPY);
}

void Controller::cut()
{
    postKeyCodeClick(AKEYCODE_CUT);
}

void Controller::expandNotificationPanel()
{
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_EXPAND_NOTIFICATION_PANEL);
    if (!controlMsg) {
        return;
    }
    postControlMsg(controlMsg);
}

void Controller::expandSettingsPanel()
{
    postControlMsg(new ControlMsg(ControlMsg::CMT_EXPAND_SETTINGS_PANEL));
}

void Controller::collapsePanel()
{
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_COLLAPSE_PANELS);
    if (!controlMsg) {
        return;
    }
    postControlMsg(controlMsg);
}

void Controller::rotateDevice()
{
    postControlMsg(new ControlMsg(ControlMsg::CMT_ROTATE_DEVICE));
}

void Controller::startApp(const QString &name)
{
    if (name.isEmpty()) {
        return;
    }
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_START_APP);
    controlMsg->setStartAppData(name);
    postControlMsg(controlMsg);
}

void Controller::scanFile(const QString &path)
{
    if (path.isEmpty()) {
        return;
    }
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_SCAN_FILE);
    controlMsg->setScanFileData(path);
    postControlMsg(controlMsg);
}

void Controller::resizeDisplay(const QSize &size)
{
    if (size.width() <= 0 || size.height() <= 0) {
        return;
    }
    m_pendingResize = size;
    if (m_resizeQueued) {
        return;
    }
    m_resizeQueued = true;
    QTimer::singleShot(0, this, &Controller::sendPendingResize);

    // Keep the daemon's frame->panel scaling in sync proactively (plan §3:
    // "add a FRAME command, sent on connect and on every
    // Controller::resizeDisplay()") - touchDown()/touchMove() also push this
    // lazily whenever the frameSize they're called with changes, so this is
    // belt-and-suspenders for the gap between a resize and the next touch.
    if (m_rawInputDaemonAvailable && m_rawInputDaemonSession && m_rawInputDaemonSession->isRunning()) {
        m_rawInputDaemonSession->ensureFrameSize(size);
    }
}

void Controller::sendPendingResize()
{
    m_resizeQueued = false;
    if (m_pendingResize.isEmpty()) {
        return;
    }
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_RESIZE_DISPLAY);
    controlMsg->setResizeDisplayData(m_pendingResize);
    m_pendingResize = QSize();
    postControlMsg(controlMsg);
}

void Controller::requestDeviceClipboard()
{
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_GET_CLIPBOARD);
    if (!controlMsg) {
        return;
    }
    postControlMsg(controlMsg);
}

void Controller::getDeviceClipboard(bool cut)
{
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_GET_CLIPBOARD);
    if (!controlMsg) {
        return;
    }
    ControlMsg::GetClipboardCopyKey copyKey = cut ? ControlMsg::GCCK_CUT : ControlMsg::GCCK_COPY;
    controlMsg->setGetClipboardMsgData(copyKey);
    postControlMsg(controlMsg);
}

void Controller::setDeviceClipboard(bool pause)
{
    QClipboard *board = QApplication::clipboard();
    QString text = board->text();
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_SET_CLIPBOARD);
    if (!controlMsg) {
        return;
    }
    controlMsg->setSetClipboardMsgData(text, pause);
    postControlMsg(controlMsg);
}

void Controller::clipboardPaste()
{
    QClipboard *board = QApplication::clipboard();
    QString text = board->text();
    postTextInput(text);
}

void Controller::postTextInput(QString &text)
{
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_INJECT_TEXT);
    if (!controlMsg) {
        return;
    }
    controlMsg->setInjectTextMsgData(text);
    postControlMsg(controlMsg);
}

void Controller::setDisplayPower(bool on)
{
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_SET_DISPLAY_POWER);
    if (!controlMsg) {
        return;
    }
    controlMsg->setDisplayPowerData(on);
    postControlMsg(controlMsg);
}

void Controller::setCameraTorch(bool on)
{
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_CAMERA_SET_TORCH);
    controlMsg->setCameraTorchData(on);
    postControlMsg(controlMsg);
}

void Controller::cameraZoomIn()
{
    postControlMsg(new ControlMsg(ControlMsg::CMT_CAMERA_ZOOM_IN));
}

void Controller::cameraZoomOut()
{
    postControlMsg(new ControlMsg(ControlMsg::CMT_CAMERA_ZOOM_OUT));
}

void Controller::ensureRealTouchSession()
{
    if (m_cameraMode || m_serial.isEmpty()) {
        return;
    }
    if (!m_realTouchSession) {
        m_realTouchSession = new AdbSendEventSession(this);
    }
    if (!m_realTouchSession->isRunning()) {
        m_realTouchSession->start(m_serial);
    }
    ensureRawInputDaemon();
    if (!m_rawInputDaemonAvailable) {
        // Only needed for mapFrameToRawTouch()'s fallback formula - the
        // daemon polls `dumpsys window` itself when it's the one driving
        // touch (design doc "Decision"), so skip the redundant PC-side poll
        // entirely once the daemon path is confirmed up.
        ensureRotationPolling();
    }
}

void Controller::ensureRawInputDaemon()
{
    if (m_cameraMode || m_serial.isEmpty() || !m_rawInputDaemonEnabled) {
        return;
    }
    if (m_rawInputDaemonAttempted) {
        // Tried-once latch (plan open question: "leaning toward
        // per-connection" - Controller itself is already scoped to one
        // device connection, so "once per Controller" satisfies that).
        // Deliberately not retried mid-connection even if it failed: a
        // flaky half-succeeded daemon retried on every touch event would be
        // worse than a clean, permanent fallback for this connection.
        return;
    }
    m_rawInputDaemonAttempted = true;

    if (!m_rawInputDaemonSession) {
        m_rawInputDaemonSession = new RawInputDaemonSession(this);
        connect(m_rawInputDaemonSession, &RawInputDaemonSession::sessionError, this, [](const QString &message) {
            qWarning() << "Controller: raw input daemon unavailable, falling back to AdbSendEventSession for touch:" << message;
        });
    }

    m_rawInputDaemonAvailable = m_rawInputDaemonSession->start(m_serial);
    if (m_rawInputDaemonAvailable) {
        qInfo() << "Controller: raw input daemon connected, touch will use the daemon-backed path";
    }
}

void Controller::ensureRotationPolling()
{
    if (m_cameraMode || m_serial.isEmpty() || m_rotationPollTimer) {
        return;
    }

    // Polled rather than queried per-touch: blocking on `dumpsys window`
    // synchronously inside sendRealTouch() would add visible touch latency.
    // Polling async in the background at a short interval instead gives
    // near-seamless detection without ever blocking a touch: `dumpsys
    // window` normally completes in tens of ms, so a 200ms interval keeps
    // the cached m_deviceRotation at most ~200ms (worst case, a bit more if
    // adb is slow - see m_rotationPollInFlight) stale after a physical
    // rotation. sendRealTouch() falls back to the ROTATION_90 formula if no
    // poll has completed yet.
    m_rotationPollTimer = new QTimer(this);
    connect(m_rotationPollTimer, &QTimer::timeout, this, &Controller::pollDeviceRotation);
    m_rotationPollTimer->start(200);
    pollDeviceRotation(); // seed immediately instead of waiting for the first tick
}

void Controller::pollDeviceRotation()
{
    if (m_cameraMode || m_serial.isEmpty()) {
        return;
    }
    if (m_rotationPollInFlight) {
        // Previous poll (e.g. over a slow/wireless adb connection) hasn't
        // finished yet - skip this tick rather than piling up overlapping
        // `dumpsys` processes.
        return;
    }
    m_rotationPollInFlight = true;

    // Fire-and-forget: whichever poll lands most recently just updates the
    // cache that sendRealTouch() reads; nothing here blocks touch delivery.
    QProcess *proc = new QProcess(this);
    connect(proc, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
            [this, proc](int, QProcess::ExitStatus) {
        static const QRegularExpression kRotationRegex("mCurrentRotation=ROTATION_(\\d+)");
        const QString out = QString::fromUtf8(proc->readAllStandardOutput());
        const QRegularExpressionMatch match = kRotationRegex.match(out);
        if (match.hasMatch()) {
            switch (match.captured(1).toInt()) {
            case 0:
                m_deviceRotation = DeviceRotation::Rotation0;
                break;
            case 90:
                m_deviceRotation = DeviceRotation::Rotation90;
                break;
            case 180:
                m_deviceRotation = DeviceRotation::Rotation180;
                break;
            case 270:
                m_deviceRotation = DeviceRotation::Rotation270;
                break;
            default:
                break;
            }
        }
        m_rotationPollInFlight = false;
        proc->deleteLater();
    });
    proc->start(AdbProcessImpl::getAdbPath(), {"-s", m_serial, "shell", "dumpsys", "window"});
}

void Controller::ensureTouchMoveThrottle()
{
    if (m_touchMoveFlushTimer) {
        return;
    }
    m_touchMoveFlushTimer = new QTimer(this);
    // ~60Hz cap. Faster than this and the remote adb shell can't fork+exec
    // `sendevent` processes as fast as Qt delivers mouse-move samples,
    // building a backlog that keeps draining (visibly "sliding") after the
    // finger has already lifted. At each tick, only the latest coalesced
    // position per slot is sent - intermediate samples are dropped, not
    // queued.
    connect(m_touchMoveFlushTimer, &QTimer::timeout, this, &Controller::flushPendingTouchMoves);
    m_touchMoveFlushTimer->start(16);
}

void Controller::flushPendingTouchMoves()
{
    for (auto it = m_pendingTouchMoves.begin(); it != m_pendingTouchMoves.end(); ++it) {
        if (!it.value().valid) {
            continue;
        }
        if (it.value().useDaemon) {
            if (m_rawInputDaemonAvailable && m_rawInputDaemonSession && m_rawInputDaemonSession->isRunning()) {
                m_rawInputDaemonSession->touchMove(it.key(), it.value().framePos, it.value().frameSize);
            }
        } else if (m_realTouchSession && m_realTouchSession->isRunning()) {
            m_realTouchSession->touchMove(it.key(), it.value().rawX, it.value().rawY);
        }
        it.value().valid = false; // consumed - don't resend the same position again next tick
    }
}

QPoint Controller::mapFrameToRawTouch(const QPoint &framePos, const QSize &frameSize) const
{
    if (!m_realTouchSession || frameSize.width() <= 0 || frameSize.height() <= 0) {
        return QPoint(0, 0);
    }

    const AdbSendEventSession::TouchProfile &profile = m_realTouchSession->touchProfile();
    const bool frameIsLandscape = frameSize.width() > frameSize.height();
    // Touch panel's native coordinate space is portrait. A landscape
    // mirrored frame (width() > height()) needs pre-rotation before
    // scaling to native panel coords.
    //
    // ROTATION_270 was verified against on-device tap tests and uses the
    // swap formula below. ROTATION_90 is the mirror-image chirality of
    // ROTATION_270 (180 degrees apart from it, not from portrait) - rather
    // than an independently-derived formula, it's computed as the
    // point-reflection of the 270 formula through the panel's center
    // (xMax - rx270, yMax - ry270). This is a reasonable hypothesis given
    // the relationship between the two rotations, but has NOT yet been
    // independently confirmed against real tap data the way 270 was -
    // recalibrate from fresh tap samples if this doesn't line up on-device.
    double rx = 0.0;
    double ry = 0.0;
    if (frameIsLandscape) {
        const double rx270 = framePos.y() * static_cast<double>(profile.xMax) / frameSize.height();
        const double ry270 = profile.yMax - (framePos.x() * static_cast<double>(profile.yMax) / frameSize.width());

        if (m_deviceRotation == DeviceRotation::Rotation90) {
            rx = profile.xMax - rx270;
            ry = profile.yMax - ry270;
        } else {
            // ROTATION_270, and the fallback for Rotation0/180/Unknown
            // reported while the frame is still landscape (e.g. before the
            // first poll completes) - matches the verified formula.
            rx = rx270;
            ry = ry270;
        }
    } else {
        rx = framePos.x() * static_cast<double>(profile.xMax) / frameSize.width();
        ry = framePos.y() * static_cast<double>(profile.yMax) / frameSize.height();
    }

    const int x = qBound(0, static_cast<int>(qRound(rx)), profile.xMax);
    const int y = qBound(0, static_cast<int>(qRound(ry)), profile.yMax);
    return QPoint(x, y);
}

void Controller::sendRealTouch(int slot, AndroidMotioneventAction action, QPoint framePos, const QSize &frameSize)
{
    if (m_cameraMode) {
        // no on-screen touch target in camera-only mode
        return;
    }

    ensureRealTouchSession();

    // Daemon-backed path (plan §2): frame-space coordinates straight onto
    // the wire, no mapFrameToRawTouch()/rotation-poll dependency at all -
    // the daemon does frame->panel scaling and rotation itself.
    if (m_rawInputDaemonAvailable && m_rawInputDaemonSession && m_rawInputDaemonSession->isRunning()) {
        switch (action) {
        case AMOTION_EVENT_ACTION_DOWN:
            m_rawInputDaemonSession->touchDown(slot, slot + 1, framePos, frameSize);
            break;
        case AMOTION_EVENT_ACTION_MOVE:
            ensureTouchMoveThrottle();
            m_pendingTouchMoves[slot] = { true, true, 0, 0, framePos, frameSize };
            break;
        case AMOTION_EVENT_ACTION_UP:
            m_pendingTouchMoves.remove(slot);
            m_rawInputDaemonSession->touchUp(slot);
            break;
        default:
            break;
        }
        return;
    }

    // Fallback path (plan §2.2): unchanged raw-panel sendevent behaviour via
    // AdbSendEventSession, pre-computing panel-space coordinates here since
    // that's what this path has always required.
    if (!m_realTouchSession || !m_realTouchSession->isRunning()) {
        qWarning() << "Controller::sendRealTouch: sendevent session not running, dropping touch event";
        return;
    }

    const QPoint raw = mapFrameToRawTouch(framePos, frameSize);
    const int rawX = raw.x();
    const int rawY = raw.y();

    switch (action) {
    case AMOTION_EVENT_ACTION_DOWN:
        m_realTouchSession->touchDown(slot, slot + 1, rawX, rawY);
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        ensureTouchMoveThrottle();
        m_pendingTouchMoves[slot] = { true, false, rawX, rawY, QPoint(), QSize() };
        break;
    case AMOTION_EVENT_ACTION_UP:
        m_pendingTouchMoves.remove(slot); // drop any move still queued for a slot that's now lifted
        m_realTouchSession->touchUp(slot);
        break;
    default:
        break;
    }
}

void Controller::sendRealScroll(QPoint framePos, const QSize &frameSize, float hScroll, float vScroll)
{
    if (m_cameraMode) {
        return;
    }
    if (qFuzzyIsNull(hScroll) && qFuzzyIsNull(vScroll)) {
        return;
    }

    ensureRealTouchSession();
    if (frameSize.width() <= 0 || frameSize.height() <= 0) {
        return;
    }

    // Converted into a short synthetic swipe at the cursor position through
    // the same real touchscreen channel used for taps/drags, rather than a
    // framework scroll command - this is what makes it feel like a real
    // finger flick instead of a synthesized "scroll" event. Wheel-up
    // (positive vScroll, Qt's convention) scrolls content up, which on a
    // real touchscreen is a swipe moving upward (decreasing Y) - the same
    // relationship mouse-to-touchpad emulation drivers use.
    constexpr double kPixelsPerNotch = 90.0;
    constexpr int kSteps = 4;

    QPointF endFramePos = QPointF(framePos) - QPointF(hScroll * kPixelsPerNotch, vScroll * kPixelsPerNotch);
    // Keep the synthetic swipe's endpoint inside the mirrored frame so it
    // doesn't map to an out-of-bounds raw coordinate.
    endFramePos.setX(qBound(0.0, endFramePos.x(), static_cast<double>(frameSize.width())));
    endFramePos.setY(qBound(0.0, endFramePos.y(), static_cast<double>(frameSize.height())));

    const int slot = kMouseTouchSlot;

    if (m_rawInputDaemonAvailable && m_rawInputDaemonSession && m_rawInputDaemonSession->isRunning()) {
        // Frame-space throughout - no mapFrameToRawTouch() needed on this path.
        m_rawInputDaemonSession->touchDown(slot, slot + 1, framePos, frameSize);
        for (int i = 1; i <= kSteps; ++i) {
            const double t = static_cast<double>(i) / kSteps;
            const QPointF stepPos = QPointF(framePos) + (endFramePos - QPointF(framePos)) * t;
            m_rawInputDaemonSession->touchMove(slot, stepPos.toPoint(), frameSize);
        }
        m_rawInputDaemonSession->touchUp(slot);
        return;
    }

    if (!m_realTouchSession || !m_realTouchSession->isRunning()) {
        qWarning() << "Controller::sendRealScroll: sendevent session not running, dropping scroll event";
        return;
    }

    const QPoint startRaw = mapFrameToRawTouch(framePos, frameSize);
    const QPoint endRaw = mapFrameToRawTouch(endFramePos.toPoint(), frameSize);

    m_realTouchSession->touchDown(slot, slot + 1, startRaw.x(), startRaw.y());
    for (int i = 1; i <= kSteps; ++i) {
        const double t = static_cast<double>(i) / kSteps;
        const int ix = static_cast<int>(qRound(startRaw.x() + (endRaw.x() - startRaw.x()) * t));
        const int iy = static_cast<int>(qRound(startRaw.y() + (endRaw.y() - startRaw.y()) * t));
        m_realTouchSession->touchMove(slot, ix, iy);
    }
    m_realTouchSession->touchUp(slot);
}

void Controller::mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (m_inputConvert) {
        m_inputConvert->mouseEvent(from, frameSize, showSize);
    }
}

void Controller::wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (m_inputConvert) {
        m_inputConvert->wheelEvent(from, frameSize, showSize);
    }
}

void Controller::keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (m_inputConvert) {
        m_inputConvert->keyEvent(from, frameSize, showSize);
    }
}

bool Controller::event(QEvent *event)
{
    if (event && static_cast<ControlMsg::Type>(event->type()) == ControlMsg::Control) {
        ControlMsg *controlMsg = dynamic_cast<ControlMsg *>(event);
        if (controlMsg) {
            sendControl(controlMsg->serializeData());
        }
        return true;
    }
    return QObject::event(event);
}

bool Controller::sendControl(const QByteArray &buffer)
{
    if (buffer.isEmpty()) {
        return false;
    }
    qint32 len = 0;
    if (m_sendData) {
        len = static_cast<qint32>(m_sendData(buffer));
    }
    return len == buffer.length() ? true : false;
}

void Controller::postKeyCodeClick(AndroidKeycode keycode)
{
    sendRealKeyEvent(static_cast<int>(keycode));
}

void Controller::sendRealKeyEvent(int androidKeycode)
{
    ensureRealTouchSession();
    if (!m_realTouchSession || !m_realTouchSession->isRunning()) {
        qWarning() << "Controller::sendRealKeyEvent: sendevent session not running, dropping key event";
        return;
    }
    m_realTouchSession->pressKeyEvent(androidKeycode);
}
