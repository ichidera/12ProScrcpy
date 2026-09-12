#include <utility>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>

#include "config.h"
#include "gameinterfacemonitor.h"

namespace
{
// Same cadence as realtime_lookup_demo.py's `time.sleep(1)`.
constexpr int kPollIntervalMs = 1000;
constexpr int kAdbTimeoutMs = 3000;
} // namespace

GameInterfaceMonitor::GameInterfaceMonitor(QString serial, QObject *parent) : QObject(parent), m_serial(std::move(serial)) {}

GameInterfaceMonitor::~GameInterfaceMonitor()
{
    stop();
}

void GameInterfaceMonitor::start()
{
    if (m_timer) {
        return;
    }
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &GameInterfaceMonitor::poll);
    m_timer->start(kPollIntervalMs);
    poll(); // resolve the current foreground app right away instead of waiting a full interval
}

void GameInterfaceMonitor::stop()
{
    if (!m_timer) {
        return;
    }
    m_timer->stop();
    m_timer->deleteLater();
    m_timer = nullptr;
}

void GameInterfaceMonitor::poll()
{
    if (m_polling) {
        return; // previous adb call is still in flight - don't pile up requests
    }
    m_polling = true;
    const QString pkg = queryForegroundPackage();
    m_polling = false;

    // "if pkg and pkg != last_pkg:" from the python script - only act on
    // an actual foreground-app change, not on every poll tick.
    if (pkg.isEmpty() || pkg == m_currentPackageId) {
        return;
    }
    m_currentPackageId = pkg;
    m_currentDisplayName = displayNameForPackage(pkg);
    emit interfaceChanged(m_currentPackageId, m_currentDisplayName);
}

QString GameInterfaceMonitor::queryForegroundPackage() const
{
    if (m_serial.isEmpty()) {
        return QString();
    }

    QString adbPath = Config::getInstance().getAdbPath();
    if (adbPath.isEmpty()) {
        adbPath = QStringLiteral("adb");
    }

    // subprocess.run(["adb", "shell", "dumpsys activity activities"], ...)
    QProcess proc;
    proc.start(adbPath, { QStringLiteral("-s"), m_serial, QStringLiteral("shell"), QStringLiteral("dumpsys"), QStringLiteral("activity"),
                           QStringLiteral("activities") });
    if (!proc.waitForFinished(kAdbTimeoutMs)) {
        proc.kill();
        proc.waitForFinished(500);
        return QString();
    }
    const QString output = QString::fromUtf8(proc.readAllStandardOutput());

    // for line in result.stdout.splitlines():
    //     if "topResumedActivity" in line:
    //         return line.split("u0 ")[1].split("/")[0]
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (!line.contains(QLatin1String("topResumedActivity"))) {
            continue;
        }
        const QStringList afterU0 = line.split(QStringLiteral("u0 "));
        if (afterU0.size() < 2) {
            continue;
        }
        const QString pkg = afterU0.at(1).split(QLatin1Char('/')).first().trimmed();
        if (!pkg.isEmpty()) {
            return pkg;
        }
    }
    return QString();
}

QString GameInterfaceMonitor::displayNameForPackage(const QString &packageId)
{
    if (packageId.isEmpty()) {
        return packageId;
    }

    // scripts/build_db.py builds playstore.db next to the scripts, from a
    // Play Store CSV dump; scripts/lookup.py and realtime_lookup_demo.py
    // both read it via sqlite3.connect('playstore.db') from the current
    // working directory. Here we check a few sensible on-disk locations
    // instead of depending on process cwd.
    static QString s_dbPath;
    static bool s_dbPathResolved = false;
    if (!s_dbPathResolved) {
        s_dbPathResolved = true;
        const QStringList candidates = { QCoreApplication::applicationDirPath() + "/playstore.db",
                                          QCoreApplication::applicationDirPath() + "/scripts/playstore.db",
                                          QDir::currentPath() + "/playstore.db" };
        for (const QString &candidate : candidates) {
            if (QFileInfo::exists(candidate)) {
                s_dbPath = candidate;
                break;
            }
        }
    }
    if (s_dbPath.isEmpty()) {
        return packageId; // no db bundled - fall back to the raw package name, same as lookup_app()
    }

    // Dedicated connection name so this never collides with any other
    // QSqlDatabase connection the app opens elsewhere.
    const QString connectionName = QStringLiteral("gameInterfaceMonitorPlaystoreDb");
    QSqlDatabase db = QSqlDatabase::contains(connectionName) ? QSqlDatabase::database(connectionName)
                                                              : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    if (!db.isOpen()) {
        db.setDatabaseName(s_dbPath);
        if (!db.open()) {
            return packageId;
        }
    }

    // cur.execute('SELECT "App Name" FROM apps WHERE "App Id" = ?', (package_id,))
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT \"App Name\" FROM apps WHERE \"App Id\" = ?"));
    query.addBindValue(packageId);
    if (query.exec() && query.next()) {
        const QString name = query.value(0).toString();
        if (!name.isEmpty()) {
            return name;
        }
    }
    return packageId; // fallback to raw package name, same as lookup_app()
}
