#ifndef KEYMAPPROFILESTORE_H
#define KEYMAPPROFILESTORE_H

#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

// One control placed on screen by the in-app editor. This is intentionally a
// flat POD-ish struct (not KeyMap::KeyMapNode) so the UI layer doesn't need
// to know about KeyMap's internal unions - toJson()/fromJson() below convert
// to/from exactly the JSON schema that KeyMap::loadKeyMap() parses.
enum class ControlActionKind
{
    TapSpot,
    RepeatedTap,
    DPad,
    DragSwipe,
    FreeLook,
    AimPanShoot
};

struct ControlNode
{
    ControlActionKind action = ControlActionKind::TapSpot;

    // Anchor position, normalized [0,1] over the mirrored screen. Meaning
    // depends on action:
    //   TapSpot / RepeatedTap / AimPanShoot: the tap/aim point
    //   DPad:                                the stick's resting center
    //   DragSwipe / FreeLook:                the touch-down / aim start point
    QPointF pos = QPointF(0.5, 0.5);

    QPointF endPos = QPointF(0.65, 0.5); // DragSwipe only: drag destination

    QString key; // bound "Key_Xxx" / mouse-button name (TapSpot, RepeatedTap,
                 // DragSwipe, AimPanShoot's "shoot" button)

    // DPad only
    QString upKey, downKey, leftKey, rightKey;
    double offset = 0.12; // normalized travel distance, shared by all 4 directions

    // RepeatedTap only
    int repeatCount = 3;
    int repeatIntervalMs = 120;

    // DragSwipe only
    quint32 dragStartDelayMs = 0;
    float dragSpeed = 1.0f;

    // FreeLook / AimPanShoot only (there can be at most one such node per profile)
    float lookSpeedX = 18.0f;
    float lookSpeedY = 8.0f;
    QString smallEyesKey; // optional toggle key, empty = disabled

    QString label; // shown on the marker/list; auto-filled if left empty
};

// Reads/writes profiles as .json files in the same keymap directory the
// engine (KeyMap::loadKeyMap) already reads from, and converts between that
// JSON and the ControlNode list above. This is the only place besides
// KeyMap itself that needs to know the on-disk schema.
class KeyMapProfileStore
{
public:
    static QString profileDirPath();
    static QStringList listProfiles(); // display names, no ".json" suffix

    // cursorLockKey: the key that grabs+hides the OS cursor (BlueStacks-style
    // "enter/exit shooting mode"), independent of switchKey which only
    // activates the scheme. Defaults to F1 when a profile predates this
    // field or leaves it blank.
    static bool loadProfile(const QString &name, QString &switchKey, QString &cursorLockKey, QVector<ControlNode> &nodes,
                             QString *error = nullptr);
    static bool saveProfile(const QString &name, const QString &switchKey, const QString &cursorLockKey,
                             const QVector<ControlNode> &nodes, QString *error = nullptr);
    static bool deleteProfile(const QString &name);
    static bool profileExists(const QString &name);

    static QString toJson(const QString &switchKey, const QString &cursorLockKey, const QVector<ControlNode> &nodes);
    static bool fromJson(const QString &json, QString &switchKey, QString &cursorLockKey, QVector<ControlNode> &nodes,
                          QString *error = nullptr);

    // Qt::Key_* <-> "Key_Xxx" / Qt::MouseButton <-> "LeftButton" etc, using
    // the same QMetaEnum lookups KeyMap::getItemKey() uses, so round-tripping
    // is guaranteed to match what the engine will accept.
    static QString keyToString(int qtKeyOrButton, bool isMouse);
    // Returns true and fills outValue/outIsMouse on success.
    static bool stringToKey(const QString &s, int *outValue, bool *outIsMouse);

    static QString defaultSwitchKeyString();
    static QString defaultCursorLockKeyString();
    static QString actionLabel(ControlActionKind kind);
};

#endif // KEYMAPPROFILESTORE_H
