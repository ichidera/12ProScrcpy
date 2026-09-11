#include <QApplication>
#include <QClipboard>
#include <QDebug>
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
    ensureRotationPolling();
}

void Controller::ensureRotationPolling()
{
    if (m_cameraMode || m_serial.isEmpty() || m_rotationPollTimer) {
        return;
    }

    // Polled rather than queried per-touch: `dumpsys window` is too slow
    // (tens to hundreds of ms) to call on every ACTION_DOWN without adding
    // visible touch latency, and physical device rotation changes far less
    // often than that. A ~1s-stale cached value is an acceptable trade-off;
    // sendRealTouch() falls back to the ROTATION_90 formula if no poll has
    // completed yet.
    m_rotationPollTimer = new QTimer(this);
    connect(m_rotationPollTimer, &QTimer::timeout, this, &Controller::pollDeviceRotation);
    m_rotationPollTimer->start(1000);
    pollDeviceRotation(); // seed immediately instead of waiting for the first tick
}

void Controller::pollDeviceRotation()
{
    if (m_cameraMode || m_serial.isEmpty()) {
        return;
    }

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
        proc->deleteLater();
    });
    proc->start(AdbProcessImpl::getAdbPath(), {"-s", m_serial, "shell", "dumpsys", "window"});
}

void Controller::sendRealTouch(int slot, AndroidMotioneventAction action, QPoint framePos, const QSize &frameSize)
{
    if (m_cameraMode) {
        // no on-screen touch target in camera-only mode
        return;
    }

    ensureRealTouchSession();
    if (!m_realTouchSession || !m_realTouchSession->isRunning()) {
        qWarning() << "Controller::sendRealTouch: sendevent session not running, dropping touch event";
        return;
    }

    const AdbSendEventSession::TouchProfile &profile = m_realTouchSession->touchProfile();
    int rawX = 0;
    int rawY = 0;
    if (frameSize.width() > 0 && frameSize.height() > 0) {
        const bool frameIsLandscape = frameSize.width() > frameSize.height();
        // Touch panel's native coordinate space is portrait. Android has two
        // landscape rotations (ROTATION_90 and ROTATION_270) that are
        // mirror-image chiralities of each other and need different
        // pre-rotation formulas before scaling to native panel coords, or
        // Android's real rotation-correction double-applies (or wrongly
        // applies) the transform - but both give a width>height frame, so
        // frameSize alone can't tell them apart. m_deviceRotation (polled
        // from `dumpsys window`'s mCurrentRotation) disambiguates; if no
        // poll has landed yet, default to the ROTATION_90 formula below.
        if (frameIsLandscape && m_deviceRotation == DeviceRotation::Rotation270) {
            // ROTATION_270 ("seascape"): axes must be swapped (and one
            // flipped) before scaling to native panel coords. This is the
            // original, well-established landscape fix.
            double rx = framePos.y() * static_cast<double>(profile.xMax) / frameSize.height();
            double ry = profile.yMax - (framePos.x() * static_cast<double>(profile.yMax) / frameSize.width());
            rawX = qBound(0, static_cast<int>(qRound(rx)), profile.xMax);
            rawY = qBound(0, static_cast<int>(qRound(ry)), profile.yMax);
        } else if (frameIsLandscape) {
            // ROTATION_90 ("landscape"): no axis swap needed here, just a
            // flip of both axes before scaling to native panel coords.
            // Empirically derived and verified against on-device tap tests
            // (small, slightly noisy 3-point calibration sample).
            double rx = profile.xMax - (framePos.x() * static_cast<double>(profile.xMax) / frameSize.width());
            double ry = profile.yMax - (framePos.y() * static_cast<double>(profile.yMax) / frameSize.height());
            rawX = qBound(0, static_cast<int>(qRound(rx)), profile.xMax);
            rawY = qBound(0, static_cast<int>(qRound(ry)), profile.yMax);
        } else {
            rawX = qBound(0, static_cast<int>(qRound(framePos.x() * static_cast<double>(profile.xMax) / frameSize.width())), profile.xMax);
            rawY = qBound(0, static_cast<int>(qRound(framePos.y() * static_cast<double>(profile.yMax) / frameSize.height())), profile.yMax);
        }
    }

    switch (action) {
    case AMOTION_EVENT_ACTION_DOWN:
        m_realTouchSession->touchDown(slot, slot + 1, rawX, rawY);
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        m_realTouchSession->touchMove(slot, rawX, rawY);
        break;
    case AMOTION_EVENT_ACTION_UP:
        m_realTouchSession->touchUp(slot);
        break;
    default:
        break;
    }
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
    ControlMsg *controlEventDown = new ControlMsg(ControlMsg::CMT_INJECT_KEYCODE);
    if (!controlEventDown) {
        return;
    }
    controlEventDown->setInjectKeycodeMsgData(AKEY_EVENT_ACTION_DOWN, keycode, 0, AMETA_NONE);
    postControlMsg(controlEventDown);

    ControlMsg *controlEventUp = new ControlMsg(ControlMsg::CMT_INJECT_KEYCODE);
    if (!controlEventUp) {
        return;
    }
    controlEventUp->setInjectKeycodeMsgData(AKEY_EVENT_ACTION_UP, keycode, 0, AMETA_NONE);
    postControlMsg(controlEventUp);
}
