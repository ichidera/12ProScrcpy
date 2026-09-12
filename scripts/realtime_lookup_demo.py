import subprocess
import sqlite3
import time

def get_foreground_package():
    result = subprocess.run(
        ["adb", "shell", "dumpsys activity activities"],
        capture_output=True, text=True
    )
    for line in result.stdout.splitlines():
        if "topResumedActivity" in line:
            return line.split("u0 ")[1].split("/")[0]
    return None

def lookup_app(package_id, conn):
    cur = conn.cursor()
    cur.execute('SELECT "App Name" FROM apps WHERE "App Id" = ?', (package_id,))
    row = cur.fetchone()
    return row[0] if row else package_id  # fallback to raw package name

conn = sqlite3.connect('playstore.db')
last_pkg = None

while True:
    pkg = get_foreground_package()
    if pkg and pkg != last_pkg:
        print(lookup_app(pkg, conn))
        last_pkg = pkg
    time.sleep(1)