#include "adbsendeventsession.h"
#include "adbprocessimpl.h"  // for AdbProcessImpl::getAdbPath()

#include <QDebug>

AdbSendEventSession::AdbSendEventSession(QObject *parent)
    : QObject(parent)
{
    connect(&m_shell, &QProcess::readyReadStandardOutput, this, [this]() {
        QString out = QString::fromUtf8(m_shell.readAllStandardOutput()).trimmed();
        if (!out.isEmpty()) {
            emit sessionOutput(out);
            qInfo() << "AdbSendEventSession::out:" << out;
        }
    });

    connect(&m_shell, &QProcess::readyReadStandardError, this, [this]() {
        QString err = QString::fromUtf8(m_shell.readAllStandardError()).trimmed();
        if (!err.isEmpty()) {
            emit sessionOutput(err);
            qWarning() << "AdbSendEventSession::err:" << err;
        }
    });

    connect(&m_shell, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        m_started = false;
        emit sessionError(QString("adb shell process error: %1").arg(static_cast<int>(error)));
    });

    connect(&m_shell, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) {
        m_started = false;
        emit sessionFinished(exitCode);
    });
}

AdbSendEventSession::~AdbSendEventSession()
{
    stop();
}

bool AdbSendEventSession::start(const QString &serial, const TouchProfile &profile)
{
    if (m_started) {
        return true;
    }

    m_profile = profile;

    QStringList args;
    if (!serial.isEmpty()) {
        args << "-s" << serial;
    }
    args << "shell";

    m_shell.start(AdbProcessImpl::getAdbPath(), args);
    if (!m_shell.waitForStarted(3000)) {
        emit sessionError("failed to start adb shell process");
        return false;
    }

    m_started = true;

    // Escalate to root inside the interactive shell.
    writeLine("su");

    // SELinux permissive is required for sendevent to actually take effect
    // on the target driver nodes — root alone is not sufficient.
    setSelinuxPermissive();

    emit sessionStarted();
    return true;
}

void AdbSendEventSession::stop()
{
    if (!m_started) {
        return;
    }
    writeLine("exit"); // leave su
    writeLine("exit"); // leave shell
    m_shell.closeWriteChannel();
    if (!m_shell.waitForFinished(1000)) {
        m_shell.kill();
    }
    m_started = false;
}

bool AdbSendEventSession::isRunning() const
{
    return m_started && m_shell.state() == QProcess::Running;
}

void AdbSendEventSession::writeLine(const QString &line)
{
    if (!isRunning() && m_shell.state() != QProcess::Starting) {
        emit sessionError(QString("attempted write while session not running: %1").arg(line));
        return;
    }
    m_shell.write((line + "\n").toUtf8());
}

void AdbSendEventSession::setSelinuxPermissive()
{
    writeLine("setenforce 0");
}

void AdbSendEventSession::sendRaw(const QString &devicePath, int type, int code, int value)
{
    writeLine(QString("sendevent %1 %2 %3 %4").arg(devicePath).arg(type).arg(code).arg(value));
}

void AdbSendEventSession::syn(const QString &devicePath)
{
    sendRaw(devicePath, 0, 0, 0);
}

void AdbSendEventSession::touchDown(int slot, int trackId, int x, int y)
{
    const QString &dev = m_profile.devicePath;
    sendRaw(dev, 3, 47, slot);       // ABS_MT_SLOT
    sendRaw(dev, 3, 57, trackId);    // ABS_MT_TRACKING_ID
    if (m_profile.requiresBtnTouch) {
        sendRaw(dev, 1, 330, 1);     // BTN_TOUCH down
    }
    sendRaw(dev, 3, 53, x);          // ABS_MT_POSITION_X
    sendRaw(dev, 3, 54, y);          // ABS_MT_POSITION_Y
    syn(dev);
}

void AdbSendEventSession::touchMove(int slot, int x, int y)
{
    const QString &dev = m_profile.devicePath;
    sendRaw(dev, 3, 47, slot);
    sendRaw(dev, 3, 53, x);
    sendRaw(dev, 3, 54, y);
    syn(dev);
}

void AdbSendEventSession::touchUp(int slot)
{
    const QString &dev = m_profile.devicePath;
    sendRaw(dev, 3, 47, slot);
    sendRaw(dev, 3, 57, -1);         // ABS_MT_TRACKING_ID -1 = lift
    if (m_profile.requiresBtnTouch) {
        sendRaw(dev, 1, 330, 0);     // BTN_TOUCH up
    }
    syn(dev);
}

void AdbSendEventSession::tap(int x, int y, int holdMs)
{
    touchDown(0, 1, x, y);
    writeLine(QString("sleep %1").arg(holdMs / 1000.0, 0, 'f', 3));
    touchUp(0);
}

void AdbSendEventSession::swipe(int x1, int y1, int x2, int y2, int steps, int stepDelayMs)
{
    touchDown(0, 1, x1, y1);
    writeLine("sleep 0.02");

    for (int i = 1; i <= steps; ++i) {
        double t = static_cast<double>(i) / steps;
        int ix = static_cast<int>(x1 + (x2 - x1) * t);
        int iy = static_cast<int>(y1 + (y2 - y1) * t);
        touchMove(0, ix, iy);
        writeLine(QString("sleep %1").arg(stepDelayMs / 1000.0, 0, 'f', 3));
    }

    writeLine("sleep 0.02");
    touchUp(0);
}

void AdbSendEventSession::pressHardwareKey(const QString &devicePath, int linuxKeyCode, int holdMs)
{
    sendRaw(devicePath, 1, linuxKeyCode, 1); // EV_KEY down
    syn(devicePath);
    if (holdMs > 0) {
        writeLine(QString("sleep %1").arg(holdMs / 1000.0, 0, 'f', 3));
    }
    sendRaw(devicePath, 1, linuxKeyCode, 0); // EV_KEY up
    syn(devicePath);
}

void AdbSendEventSession::pressHome()
{
    pressHardwareKey(m_keyProfile.homeBackMenuDevicePath, LINUX_KEY_HOME);
}

void AdbSendEventSession::pressBack()
{
    pressHardwareKey(m_keyProfile.homeBackMenuDevicePath, LINUX_KEY_BACK);
}

void AdbSendEventSession::pressMenu()
{
    pressHardwareKey(m_keyProfile.homeBackMenuDevicePath, LINUX_KEY_MENU);
}

void AdbSendEventSession::pressPower()
{
    pressHardwareKey(m_keyProfile.powerDevicePath, LINUX_KEY_POWER);
}

void AdbSendEventSession::pressVolumeUp()
{
    pressHardwareKey(m_keyProfile.volumeUpDevicePath, LINUX_KEY_VOLUMEUP);
}

void AdbSendEventSession::pressVolumeDown()
{
    pressHardwareKey(m_keyProfile.volumeDownDevicePath, LINUX_KEY_VOLUMEDOWN);
}

void AdbSendEventSession::pressKeyEvent(int androidKeycode)
{
    // framework-level fallback (root-elevated `input keyevent`) for keys
    // without a confirmed hardware node on this device — AppSwitch, Copy, Cut
    writeLine(QString("input keyevent %1").arg(androidKeycode));
}