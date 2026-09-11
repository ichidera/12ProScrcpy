#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

class AdbSendEventSession : public QObject
{
    Q_OBJECT

public:
    struct TouchProfile
    {
        QString devicePath = "/dev/input/event6";
        int xMax = 14399;
        int yMax = 31999;
        bool requiresBtnTouch = true;
    };

    // a010f622 (Waipio-class hardware) — confirmed working per
    // docs/real-device-adb-sendevent.md §10-11
    struct HardwareKeyProfile
    {
        QString homeBackMenuDevicePath = "/dev/input/event1"; // uinput-goodix
        QString powerDevicePath        = "/dev/input/event2"; // pmic_pwrkey (primary, world-writable)
        QString volumeUpDevicePath     = "/dev/input/event0"; // gpio-keys
        QString volumeDownDevicePath   = "/dev/input/event3"; // pmic_resin
    };

    explicit AdbSendEventSession(QObject *parent = nullptr);
    virtual ~AdbSendEventSession();

    bool start(const QString &serial, const TouchProfile &profile = TouchProfile());
    void stop();
    bool isRunning() const;

    void setSelinuxPermissive();

    void sendRaw(const QString &devicePath, int type, int code, int value);
    void syn(const QString &devicePath);

    void touchDown(int slot, int trackId, int x, int y);
    void touchMove(int slot, int x, int y);
    void touchUp(int slot);

    void tap(int x, int y, int holdMs = 50);
    void swipe(int x1, int y1, int x2, int y2, int steps = 15, int stepDelayMs = 15);

    // Hardware-key injection via raw sendevent on this device's confirmed
    // kernel nodes (see HardwareKeyProfile above). Root-elevated (same su
    // session as touch) so it bypasses the same INJECT_EVENTS gate that
    // blocks the unprivileged scrcpy-server socket path.
    //
    // Linux input-event-codes.h values used by pressHardwareKey() / pressHome() etc.
    static constexpr int LINUX_KEY_HOME       = 102;
    static constexpr int LINUX_KEY_BACK       = 158;
    static constexpr int LINUX_KEY_MENU       = 139;
    static constexpr int LINUX_KEY_POWER      = 116;
    static constexpr int LINUX_KEY_VOLUMEUP   = 115;
    static constexpr int LINUX_KEY_VOLUMEDOWN = 114;

    void pressHardwareKey(const QString &devicePath, int linuxKeyCode, int holdMs = 30);
    void pressHome();
    void pressBack();
    void pressMenu();
    void pressPower();
    void pressVolumeUp();
    void pressVolumeDown();

    // Framework-level fallback (root-elevated `input keyevent`) for keys
    // with no confirmed hardware node on this device (AppSwitch, Copy, Cut).
    // Still bypasses INJECT_EVENTS since it runs inside the su'd shell.
    void pressKeyEvent(int androidKeycode);

    const TouchProfile &touchProfile() const { return m_profile; }
    const HardwareKeyProfile &hardwareKeyProfile() const { return m_keyProfile; }

signals:
    void sessionStarted();
    void sessionError(const QString &message);
    void sessionOutput(const QString &output);
    void sessionFinished(int exitCode);

private:
    void writeLine(const QString &line);

    QProcess m_shell;
    TouchProfile m_profile;
    HardwareKeyProfile m_keyProfile;
    bool m_started = false;
};