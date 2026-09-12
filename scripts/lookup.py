import sqlite3

def lookup_app(package_id):
    conn = sqlite3.connect('playstore.db')
    cur = conn.cursor()
    cur.execute('SELECT "App Name", "App Id" FROM apps WHERE "App Id" = ?', (package_id,))
    result = cur.fetchone()
    conn.close()
    return result[0] if result else None

if __name__ == '__main__':
    import sys
    pkg = sys.argv[1]
    print(lookup_app(pkg))