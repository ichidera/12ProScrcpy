#ifndef WINMOUSETAP_H
#define WINMOUSETAP_H

#include "mousetap.h"
#include <QRect>

class WinMouseTap : public MouseTap
{
public:
    WinMouseTap();
    virtual ~WinMouseTap();

    void initMouseEventTap() override;
    void quitMouseEventTap() override;
    void enableMouseEventTap(QRect rc, bool enabled) override;

    // Raw Input registration.
    // hwnd: the window that will receive WM_INPUT messages.
    // Must be called after enableMouseEventTap(rc, true) so we only
    // register while the cursor is actually grabbed/clipped.
    void registerRawInput(HWND hwnd);
    void unregisterRawInput();

    bool isRawInputRegistered() const { return m_rawInputRegistered; }

private:
    bool   m_rawInputRegistered = false;
    HWND   m_registeredHwnd     = nullptr;
};

#endif // WINMOUSETAP_H
