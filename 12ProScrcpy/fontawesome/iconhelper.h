#ifndef ICONHELPER_H
#define ICONHELPER_H

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QLabel>
#include <QMutex>
#include <QObject>
#include <QPushButton>

class IconHelper : public QObject
{
private:
    explicit IconHelper(QObject *parent = 0);
    QFont iconFont;
    static IconHelper *_instance;

public:
    static IconHelper *Instance()
    {
        static QMutex mutex;
        if (!_instance) {
            QMutexLocker locker(&mutex);
            if (!_instance) {
                _instance = new IconHelper;
            }
        }
        return _instance;
    }

    void SetIcon(QLabel *lab, QChar c, int size = 10);
    void SetIcon(QPushButton *btn, QChar c, int size = 10);
    // Covers QToolButton (and anything else QAbstractButton-derived) without
    // needing a dedicated overload per button class.
    void SetIcon(QAbstractButton *btn, QChar c, int size = 10);

    // For cases that need the glyph as an actual QIcon/QPixmap (e.g. a
    // button that mixes an icon-font glyph with normal-font text, which
    // can't share a single QAbstractButton::setText()/setFont() pair).
    QFont font(int pointSize) const
    {
        QFont f = iconFont;
        f.setPointSize(pointSize);
        return f;
    }
};

#endif // ICONHELPER_H
