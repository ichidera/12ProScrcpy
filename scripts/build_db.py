import pandas as pd
import sqlite3

# One-time conversion — this takes a few minutes, do it ONCE
df = pd.read_csv(r'.\data\Google-Playstore.csv', usecols=['App Id', 'App Name', 'Category', 'Installs'], low_memory=False)

conn = sqlite3.connect('playstore.db')
df.to_sql('apps', conn, if_exists='replace', index=False)
conn.execute('CREATE INDEX idx_appid ON apps("App Id")')
conn.commit()
conn.close()
print("Done — playstore.db created")