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

    const TouchProfile &touchProfile() const { return m_profile; }

signals:
    void sessionStarted();
    void sessionError(const QString &message);
    void sessionOutput(const QString &output);
    void sessionFinished(int exitCode);

private:
    void writeLine(const QString &line);

    QProcess m_shell;
    TouchProfile m_profile;
    bool m_started = false;
};