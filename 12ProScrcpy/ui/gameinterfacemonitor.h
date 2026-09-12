#ifndef GAMEINTERFACEMONITOR_H
#define GAMEINTERFACEMONITOR_H

#include <QObject>
#include <QPointer>
#include <QString>

class QTimer;

// Identifies which Android app ("interface") is currently in the foreground
// on the mirrored device, so control schemes can be loaded/saved per app
// instead of one flat, hand-named list shared by every game.
//
// This is a straight C++ port of scripts/realtime_lookup_demo.py:
//   - poll `adb shell dumpsys activity activities` on an interval
//   - pull the package id out of the topResumedActivity line
//   - resolve it to a human-readable name via the same playstore.db lookup
//     scripts/build_db.py builds and scripts/lookup.py queries, falling
//     back to the raw package id when there's no match (or no db at all) -
//     same fallback behaviour as the script's lookup_app().
class GameInterfaceMonitor : public QObject
{
    Q_OBJECT
public:
    explicit GameInterfaceMonitor(QString serial, QObject *parent = nullptr);
    ~GameInterfaceMonitor() override;

    void start();
    void stop();
    bool isRunning() const { return m_timer != nullptr; }

    // Whatever's currently in the foreground, or empty if unknown (monitor
    // not started, no device, adb unavailable...).
    QString currentPackageId() const { return m_currentPackageId; }
    QString currentDisplayName() const { return m_currentDisplayName; }

    // Looks up a human-readable app name for a package id using the same
    // table/query as scripts/lookup.py. Returns the package id itself if
    // there's no local playstore.db or no match, so callers never need a
    // separate null/empty check.
    static QString displayNameForPackage(const QString &packageId);

signals:
    // Fires only when the foreground package actually changes (mirrors the
    // demo script's "if pkg and pkg != last_pkg" guard), not on every poll.
    void interfaceChanged(const QString &packageId, const QString &displayName);

private slots:
    void poll();

private:
    QString queryForegroundPackage() const;

private:
    QString m_serial;
    QPointer<QTimer> m_timer;
    QString m_currentPackageId;
    QString m_currentDisplayName;
    bool m_polling = false; // guards against overlapping adb calls if one poll runs long
};

#endif // GAMEINTERFACEMONITOR_H
