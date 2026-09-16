#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaEnum>
#include <QObject>
#include <QStandardPaths>

#include "keymapprofilestore.h"

namespace
{
// Sanitizes a package id (or whatever else is passed) into something safe
// to use as a single directory name - package ids are already just dotted
// alphanumerics, so this only matters for the unknown-interface fallback
// and any oddball input.
QString sanitizedInterfaceDir(const QString &interfaceId)
{
    QString id = interfaceId.isEmpty() ? KeyMapProfileStore::unknownInterfaceId() : interfaceId;
    for (QChar &c : id) {
        if (!(c.isLetterOrNumber() || c == QLatin1Char('.') || c == QLatin1Char('_') || c == QLatin1Char('-'))) {
            c = QLatin1Char('_');
        }
    }
    return id;
}

QString interfaceDirPath(const QString &interfaceId)
{
    return KeyMapProfileStore::storageRoot() + "/" + sanitizedInterfaceDir(interfaceId);
}

QPointF jsonPos(const QJsonObject &node, const QString &name)
{
    QJsonObject pos = node.value(name).toObject();
    return QPointF(pos.value("x").toDouble(), pos.value("y").toDouble());
}

QJsonObject posJson(QPointF pos)
{
    QJsonObject obj;
    obj.insert("x", pos.x());
    obj.insert("y", pos.y());
    return obj;
}

QJsonObject clickJson(const QString &key, QPointF pos, bool switchMap = false)
{
    QJsonObject obj;
    obj.insert("type", "KMT_CLICK");
    obj.insert("key", key);
    obj.insert("pos", posJson(pos));
    obj.insert("switchMap", switchMap);
    obj.insert("androidKey", 0);
    return obj;
}
} // namespace

QString KeyMapProfileStore::storageRoot()
{
    static QString s_root;
    if (s_root.isEmpty()) {
        // Internal app storage, not a folder anyone is meant to browse by
        // hand - QTSCRCPY_KEYMAP_PATH / the old "<app dir>/keymap" folder
        // are both gone. Schemes are filed per-interface underneath this.
        s_root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/gamecontrols";
    }
    return s_root;
}

QString KeyMapProfileStore::unknownInterfaceId()
{
    return QStringLiteral("_unknown");
}

QString KeyMapProfileStore::defaultProfileDisplayName()
{
    return QObject::tr("Game controls");
}

QStringList KeyMapProfileStore::listProfiles(const QString &interfaceId)
{
    QStringList names;
    QDir dir(interfaceDirPath(interfaceId));
    if (!dir.exists()) {
        return names;
    }
    dir.setFilter(QDir::Files | QDir::NoSymLinks);
    dir.setNameFilters(QStringList() << "*.json");
    const QFileInfoList list = dir.entryInfoList();
    for (const QFileInfo &info : list) {
        names << info.completeBaseName();
    }
    return names;
}

bool KeyMapProfileStore::profileExists(const QString &interfaceId, const QString &name)
{
    return QFile::exists(interfaceDirPath(interfaceId) + "/" + name + ".json");
}

bool KeyMapProfileStore::deleteProfile(const QString &interfaceId, const QString &name)
{
    return QFile::remove(interfaceDirPath(interfaceId) + "/" + name + ".json");
}

bool KeyMapProfileStore::loadProfile(const QString &interfaceId, const QString &name, QString &switchKey, QString &cursorLockKey,
                                      QVector<ControlNode> &nodes, QString *error)
{
    QDir dir(interfaceDirPath(interfaceId));
    QFile file(dir.filePath(name + ".json"));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QObject::tr("could not open profile file");
        }
        return false;
    }
    QString json = QString::fromUtf8(file.readAll());
    file.close();
    return fromJson(json, switchKey, cursorLockKey, nodes, error);
}

bool KeyMapProfileStore::saveProfile(const QString &interfaceId, const QString &name, const QString &switchKey,
                                      const QString &cursorLockKey, const QVector<ControlNode> &nodes, QString *error)
{
    QDir dir(interfaceDirPath(interfaceId));
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            if (error) {
                *error = QObject::tr("could not create control scheme storage for this app");
            }
            return false;
        }
    }
    QFile file(dir.filePath(name + ".json"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = QObject::tr("could not write profile file");
        }
        return false;
    }
    const QString json = toJson(switchKey, cursorLockKey, nodes);
    file.write(json.toUtf8());
    file.close();
    return true;
}

QString KeyMapProfileStore::defaultSwitchKeyString()
{
    return QStringLiteral("Key_QuoteLeft"); // backtick `, matches KeyMap's built-in default
}

QString KeyMapProfileStore::defaultCursorLockKeyString()
{
    return QStringLiteral("Key_F1"); // locks+hides the cursor; independent of the switch key above
}

QString KeyMapProfileStore::actionLabel(ControlActionKind kind)
{
    switch (kind) {
    case ControlActionKind::TapSpot:
        return QObject::tr("Tap spot");
    case ControlActionKind::RepeatedTap:
        return QObject::tr("Repeated tap");
    case ControlActionKind::DPad:
        return QObject::tr("D-Pad");
    case ControlActionKind::DragSwipe:
        return QObject::tr("Swipe");
    case ControlActionKind::FreeLook:
        return QObject::tr("Free look");
    case ControlActionKind::AimPanShoot:
        return QObject::tr("Aim, pan and shoot");
    }
    return QString();
}

QString KeyMapProfileStore::keyToString(int qtKeyOrButton, bool isMouse)
{
    if (isMouse) {
        QMetaEnum me = QMetaEnum::fromType<Qt::MouseButtons>();
        const char *k = me.valueToKey(qtKeyOrButton);
        return k ? QString::fromLatin1(k) : QString();
    }
    QMetaEnum me = QMetaEnum::fromType<Qt::Key>();
    const char *k = me.valueToKey(qtKeyOrButton);
    return k ? QString::fromLatin1(k) : QString();
}

bool KeyMapProfileStore::stringToKey(const QString &s, int *outValue, bool *outIsMouse)
{
    if (s.isEmpty()) {
        return false;
    }
    QMetaEnum meKey = QMetaEnum::fromType<Qt::Key>();
    QMetaEnum meMouse = QMetaEnum::fromType<Qt::MouseButtons>();
    bool ok = false;
    int key = meKey.keyToValue(s.toStdString().c_str(), &ok);
    if (ok) {
        if (outValue) {
            *outValue = key;
        }
        if (outIsMouse) {
            *outIsMouse = false;
        }
        return true;
    }
    int btn = meMouse.keyToValue(s.toStdString().c_str(), &ok);
    if (ok) {
        if (outValue) {
            *outValue = btn;
        }
        if (outIsMouse) {
            *outIsMouse = true;
        }
        return true;
    }
    return false;
}

QString KeyMapProfileStore::toJson(const QString &switchKey, const QString &cursorLockKey, const QVector<ControlNode> &nodes)
{
    QJsonObject root;
    root.insert("switchKey", switchKey.isEmpty() ? defaultSwitchKeyString() : switchKey);
    // Read by InputConvertGame::loadKeyMap() directly - not part of KeyMap's
    // own node schema, since it toggles cursor grab/hide rather than a touch.
    root.insert("cursorLockKey", cursorLockKey.isEmpty() ? defaultCursorLockKeyString() : cursorLockKey);

    // At most one FreeLook/AimPanShoot node is meaningful (KeyMap only has a
    // single mouseMoveMap slot) - the editor enforces this, but stay
    // defensive here too and just use the first one found.
    for (const ControlNode &node : nodes) {
        if (node.action != ControlActionKind::FreeLook && node.action != ControlActionKind::AimPanShoot) {
            continue;
        }
        QJsonObject mouseMoveMap;
        // Multiplier, not the old "speedRatio" divisor - see
        // KeyMap::loadKeyMap(), which still reads the legacy keys and
        // converts them. Deliberately not writing the legacy keys back out:
        // emitting both would leave two contradictory numbers in the file
        // for the same setting.
        mouseMoveMap.insert("sensitivityX", node.lookSpeedX);
        mouseMoveMap.insert("sensitivityY", node.lookSpeedY);
        mouseMoveMap.insert("startPos", posJson(node.pos));
        if (!node.smallEyesKey.isEmpty()) {
            QJsonObject smallEyes = clickJson(node.smallEyesKey, node.pos, false);
            mouseMoveMap.insert("smallEyes", smallEyes);
        }
        if (!node.suspendKey.isEmpty()) {
            // Plain string, not clickJson()-wrapped: like cursorLockKey,
            // this is a hold-modifier InputConvertGame reads directly, not
            // an actual touch/tap node for KeyMap to dispatch.
            mouseMoveMap.insert("suspendKey", node.suspendKey);
        }
        root.insert("mouseMoveMap", mouseMoveMap);

        // Read by InputConvertGame::loadKeyMap() directly, same idea as
        // cursorLockKey: tells the engine which single mouse button (if
        // any) is this scheme's "shoot" button, so that button alone is
        // gated behind shoot-mode being engaged (see "Left click mouse
        // only fires when shoot-mode is toggled" behavior in
        // InputConvertGame::processMouseClick()). Left empty when the
        // shoot action is bound to a keyboard key instead - nothing to
        // gate in that case.
        if (node.action == ControlActionKind::AimPanShoot) {
            const QString shootKey = node.key.isEmpty() ? QStringLiteral("LeftButton") : node.key;
            bool isMouse = false;
            if (stringToKey(shootKey, nullptr, &isMouse) && isMouse) {
                root.insert("shootButton", shootKey);
            }
        }
        break;
    }

    QJsonArray keyMapNodes;
    for (const ControlNode &node : nodes) {
        switch (node.action) {
        case ControlActionKind::TapSpot: {
            keyMapNodes.append(clickJson(node.key, node.pos, false));
            break;
        }
        case ControlActionKind::RepeatedTap: {
            QJsonObject obj;
            obj.insert("type", "KMT_CLICK_MULTI");
            obj.insert("key", node.key);
            QJsonArray clickNodes;
            const int count = qMax(1, node.repeatCount);
            for (int i = 0; i < count; ++i) {
                QJsonObject clickNode;
                clickNode.insert("delay", i * qMax(1, node.repeatIntervalMs));
                clickNode.insert("pos", posJson(node.pos));
                clickNodes.append(clickNode);
            }
            obj.insert("clickNodes", clickNodes);
            keyMapNodes.append(obj);
            break;
        }
        case ControlActionKind::DPad: {
            QJsonObject obj;
            obj.insert("type", "KMT_STEER_WHEEL");
            obj.insert("leftKey", node.leftKey);
            obj.insert("rightKey", node.rightKey);
            obj.insert("upKey", node.upKey);
            obj.insert("downKey", node.downKey);
            obj.insert("leftOffset", node.offset);
            obj.insert("rightOffset", node.offset);
            obj.insert("upOffset", node.offset);
            obj.insert("downOffset", node.offset);
            obj.insert("centerPos", posJson(node.pos));
            keyMapNodes.append(obj);
            break;
        }
        case ControlActionKind::DragSwipe: {
            QJsonObject obj;
            obj.insert("type", "KMT_DRAG");
            obj.insert("key", node.key);
            obj.insert("startPos", posJson(node.pos));
            obj.insert("endPos", posJson(node.endPos));
            obj.insert("startDelay", static_cast<int>(node.dragStartDelayMs));
            obj.insert("dragSpeed", node.dragSpeed);
            keyMapNodes.append(obj);
            break;
        }
        case ControlActionKind::AimPanShoot: {
            // The look/pan part became the root-level mouseMoveMap above;
            // the "shoot" part is its own plain tap node, always at its own
            // fireAnchorPos - the icon the user drops on the game's real
            // fire button. The engine drives it on its own multitouch slot,
            // so it fires there while pan keeps looking around from `pos`.
            const QString shootKey = node.key.isEmpty() ? QStringLiteral("LeftButton") : node.key;
            keyMapNodes.append(clickJson(shootKey, node.fireAnchorPos, false));
            break;
        }
        case ControlActionKind::FreeLook:
            // fully represented by the root-level mouseMoveMap above
            break;
        }
    }
    root.insert("keyMapNodes", keyMapNodes);

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

bool KeyMapProfileStore::fromJson(const QString &json, QString &switchKey, QString &cursorLockKey, QVector<ControlNode> &nodes,
                                   QString *error)
{
    nodes.clear();

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error) {
            *error = parseError.errorString();
        }
        return false;
    }

    QJsonObject root = doc.object();
    switchKey = root.value("switchKey").toString(defaultSwitchKeyString());
    // Absent in profiles saved before this field existed - fall back to F1
    // rather than leaving the lock permanently unbound.
    cursorLockKey = root.value("cursorLockKey").toString(defaultCursorLockKeyString());

    // Also read by InputConvertGame::loadKeyMap() at runtime (see there for
    // why it lives at root level rather than in a node) - reading it here
    // as well is what lets the AimPanShoot/shoot KMT_CLICK pairing below be
    // identified by *key* rather than by matching position, which is what
    // makes the separately-placed fire anchor round-trip correctly.
    const QString shootButtonStr = root.value("shootButton").toString();

    ControlNode lookNode;
    bool haveLookNode = false;
    if (root.contains("mouseMoveMap") && root.value("mouseMoveMap").isObject()) {
        QJsonObject mouseMoveMap = root.value("mouseMoveMap").toObject();
        lookNode.action = ControlActionKind::FreeLook;
        lookNode.pos = jsonPos(mouseMoveMap, "startPos");
        // Legacy divisor keys are converted to the multiplier scale (1/ratio),
        // mirroring KeyMap::loadKeyMap(), so an old profile opened in the
        // editor shows the value that actually reproduces its old feel
        // rather than a number on a scale that no longer exists. Current
        // "sensitivity*" keys are read afterwards so they win outright if a
        // file somehow carries both.
        auto legacyRatioToSensitivity = [](float ratio) -> float { return ratio < 0.001f ? 1.0f : 1.0f / ratio; };
        if (mouseMoveMap.contains("speedRatioX")) {
            lookNode.lookSpeedX = legacyRatioToSensitivity(static_cast<float>(mouseMoveMap.value("speedRatioX").toDouble(1.0)));
        } else if (mouseMoveMap.contains("speedRatio")) {
            lookNode.lookSpeedX = legacyRatioToSensitivity(static_cast<float>(mouseMoveMap.value("speedRatio").toDouble(1.0)));
        }
        if (mouseMoveMap.contains("speedRatioY")) {
            lookNode.lookSpeedY = legacyRatioToSensitivity(static_cast<float>(mouseMoveMap.value("speedRatioY").toDouble(1.0)));
        } else if (mouseMoveMap.contains("speedRatio")) {
            // The old general-ratio path divided Y by 2.25 ("phone screens
            // are often FHD+") before using it as a divisor.
            lookNode.lookSpeedY = legacyRatioToSensitivity(static_cast<float>(mouseMoveMap.value("speedRatio").toDouble(1.0)) / 2.25f);
        }
        if (mouseMoveMap.contains("sensitivityX")) {
            lookNode.lookSpeedX = static_cast<float>(mouseMoveMap.value("sensitivityX").toDouble(lookNode.lookSpeedX));
        }
        if (mouseMoveMap.contains("sensitivityY")) {
            lookNode.lookSpeedY = static_cast<float>(mouseMoveMap.value("sensitivityY").toDouble(lookNode.lookSpeedY));
        }
        if (mouseMoveMap.contains("smallEyes") && mouseMoveMap.value("smallEyes").isObject()) {
            lookNode.smallEyesKey = mouseMoveMap.value("smallEyes").toObject().value("key").toString();
        }
        lookNode.suspendKey = mouseMoveMap.value("suspendKey").toString();
        haveLookNode = true;
    }

    if (root.contains("keyMapNodes") && root.value("keyMapNodes").isArray()) {
        const QJsonArray arr = root.value("keyMapNodes").toArray();
        for (const QJsonValue &v : arr) {
            if (!v.isObject()) {
                continue;
            }
            QJsonObject obj = v.toObject();
            const QString type = obj.value("type").toString();

            if (type == "KMT_CLICK") {
                const QString key = obj.value("key").toString();
                const QPointF pos = jsonPos(obj, "pos");
                // A CLICK is recognized as AimPanShoot's shoot pairing (not
                // a separate tap spot) because its key matches the
                // root-level shootButton field, which is written
                // specifically so the pairing survives wherever the user
                // has dragged the fire icon to. Legacy profiles written
                // before the fire icon existed put that click right on the
                // look anchor instead and have no shootButton field, so
                // that position match is still honored as a fallback.
                const bool atLookAnchor = haveLookNode && (pos - lookNode.pos).manhattanLength() < 0.001;
                const bool isShootButton = haveLookNode && !shootButtonStr.isEmpty() && key == shootButtonStr;
                if (atLookAnchor || isShootButton) {
                    lookNode.action = ControlActionKind::AimPanShoot;
                    lookNode.key = key;
                    lookNode.fireAnchorPos = pos;
                    continue;
                }
                ControlNode node;
                node.action = ControlActionKind::TapSpot;
                node.key = key;
                node.pos = pos;
                nodes.push_back(node);
            } else if (type == "KMT_CLICK_MULTI") {
                ControlNode node;
                node.action = ControlActionKind::RepeatedTap;
                node.key = obj.value("key").toString();
                QJsonArray clickNodes = obj.value("clickNodes").toArray();
                node.repeatCount = qMax(1, clickNodes.size());
                if (clickNodes.size() > 0) {
                    node.pos = jsonPos(clickNodes.at(0).toObject(), "pos");
                }
                if (clickNodes.size() > 1) {
                    int d0 = clickNodes.at(0).toObject().value("delay").toInt();
                    int d1 = clickNodes.at(1).toObject().value("delay").toInt();
                    node.repeatIntervalMs = qMax(1, d1 - d0);
                }
                nodes.push_back(node);
            } else if (type == "KMT_STEER_WHEEL") {
                ControlNode node;
                node.action = ControlActionKind::DPad;
                node.leftKey = obj.value("leftKey").toString();
                node.rightKey = obj.value("rightKey").toString();
                node.upKey = obj.value("upKey").toString();
                node.downKey = obj.value("downKey").toString();
                node.offset = obj.value("leftOffset").toDouble(node.offset);
                node.pos = jsonPos(obj, "centerPos");
                nodes.push_back(node);
            } else if (type == "KMT_DRAG") {
                ControlNode node;
                node.action = ControlActionKind::DragSwipe;
                node.key = obj.value("key").toString();
                node.pos = jsonPos(obj, "startPos");
                node.endPos = jsonPos(obj, "endPos");
                node.dragStartDelayMs = static_cast<quint32>(obj.value("startDelay").toInt());
                node.dragSpeed = static_cast<float>(obj.value("dragSpeed").toDouble(1.0));
                nodes.push_back(node);
            }
            // KMT_CLICK_TWICE / KMT_ANDROID_KEY: not authored by this editor
            // (v1 scope); silently skipped if present in a hand-written file.
        }
    }

    if (haveLookNode) {
        nodes.push_back(lookNode);
    }

    return true;
}
