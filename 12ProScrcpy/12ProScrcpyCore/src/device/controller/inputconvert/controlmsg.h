#ifndef CONTROLMSG_H
#define CONTROLMSG_H

#include <QBuffer>
#include <QRect>
#include <QString>

#include "input.h"
#include "keycodes.h"
#include "qscrcpyevent.h"

#define CONTROL_MSG_MAX_SIZE (1 << 18) // 256k

#define CONTROL_MSG_INJECT_TEXT_MAX_LENGTH 300
#define CONTROL_MSG_START_APP_MAX_LENGTH 255
#define CONTROL_MSG_SCAN_FILE_PATH_MAX_LENGTH 256
// type: 1 byte; sequence: 8 bytes; paste flag: 1 byte; length: 4 bytes
#define CONTROL_MSG_CLIPBOARD_TEXT_MAX_LENGTH \
    (CONTROL_MSG_MAX_SIZE - 14)

// ControlMsg
class ControlMsg : public QScrcpyEvent
{
public:
    enum ControlMsgType
    {
        CMT_NULL = -1,
        CMT_INJECT_KEYCODE = 0,
        CMT_INJECT_TEXT,
        // CMT_INJECT_TOUCH: kept only to preserve the wire-protocol ordinal
        // for every value after it (scrcpy-server on the device expects
        // these numeric IDs). No client code constructs this message
        // anymore - all touch/click input now goes through
        // Controller::sendRealTouch() -> AdbSendEventSession, writing
        // directly to the device's /dev/input node. See
        // docs/real-device-adb-sendevent.md.
        CMT_INJECT_TOUCH,
        CMT_INJECT_SCROLL,
        CMT_BACK_OR_SCREEN_ON,
        CMT_EXPAND_NOTIFICATION_PANEL,
        CMT_EXPAND_SETTINGS_PANEL,
        CMT_COLLAPSE_PANELS,
        CMT_GET_CLIPBOARD,
        CMT_SET_CLIPBOARD,
        CMT_SET_DISPLAY_POWER,
        CMT_ROTATE_DEVICE,
        CMT_OPEN_HARD_KEYBOARD_SETTINGS = 15,
        CMT_START_APP,
        CMT_RESET_VIDEO,
        CMT_CAMERA_SET_TORCH = 18,
        CMT_CAMERA_ZOOM_IN = 19,
        CMT_CAMERA_ZOOM_OUT = 20,
        CMT_RESIZE_DISPLAY,
        CMT_SCAN_FILE,
    };

    enum GetClipboardCopyKey {
        GCCK_NONE,
        GCCK_COPY,
        GCCK_CUT,
    };

    ControlMsg(ControlMsgType controlMsgType);
    virtual ~ControlMsg();

    void setInjectKeycodeMsgData(AndroidKeyeventAction action, AndroidKeycode keycode, quint32 repeat, AndroidMetastate metastate);
    void setInjectTextMsgData(QString &text);
    void setInjectScrollMsgData(QRect position, float hScroll, float vScroll, AndroidMotioneventButtons buttons);
    void setGetClipboardMsgData(ControlMsg::GetClipboardCopyKey copyKey); 
    void setSetClipboardMsgData(QString &text, bool paste);
    void setDisplayPowerData(bool on);
    void setBackOrScreenOnData(bool down);
    void setCameraTorchData(bool on);
    void setStartAppData(const QString &name);
    void setScanFileData(const QString &path);
    void setResizeDisplayData(const QSize &size);

    ControlMsgType type() const { return m_data.type; }
    QByteArray serializeData();

private:
    void writePosition(QBuffer &buffer, const QRect &value);
    qint16 flostToI16fp(float f);

private:
    struct ControlMsgData
    {
        ControlMsgType type = CMT_NULL;
        union
        {
            struct
            {
                AndroidKeyeventAction action;
                AndroidKeycode keycode;
                quint32 repeat;
                AndroidMetastate metastate;
            } injectKeycode;
            struct
            {
                char *text = Q_NULLPTR;
            } injectText;
            struct
            {
                QRect position;
                float hScroll;
                float vScroll;
                AndroidMotioneventButtons buttons;
            } injectScroll;
            struct
            {
                AndroidKeyeventAction action; // action for the BACK key
                // screen may only be turned on on ACTION_DOWN
            } backOrScreenOn;
            struct
            {
                enum GetClipboardCopyKey copyKey;
            } getClipboard;
            struct
            {
                uint64_t sequence = 0;
                char *text = Q_NULLPTR;
                bool paste = true;
            } setClipboard;
            struct
            {
                bool on;
            } setDisplayPower;
            struct
            {
                bool on;
            } cameraTorch;
            struct
            {
                char *name = Q_NULLPTR;
            } startApp;
            struct
            {
                quint16 width;
                quint16 height;
            } resizeDisplay;
            struct
            {
                char *path = Q_NULLPTR;
            } scanFile;
        };

        ControlMsgData() {}
        ~ControlMsgData() {}
    };

    ControlMsgData m_data;
};

#endif // CONTROLMSG_H
