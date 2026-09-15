#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
// RAWINPUTDEVICE / RAWINPUT / WM_INPUT live in these headers.
// hidusage.h defines the HID usage page / usage constants.
#include <hidusage.h>

#include <QDebug>
#include "winmousetap.h"

WinMouseTap::WinMouseTap() {}
WinMouseTap::~WinMouseTap() { unregisterRawInput(); }

void WinMouseTap::initMouseEventTap() {}
void WinMouseTap::quitMouseEventTap() { unregisterRawInput(); }

void WinMouseTap::enableMouseEventTap(QRect rc, bool enabled)
{
    if (enabled && rc.isEmpty()) return;

    if (enabled) {
        RECT r;
        r.left   = (LONG)rc.left();
        r.right  = (LONG)rc.right();
        r.top    = (LONG)rc.top();
        r.bottom = (LONG)rc.bottom();
        ClipCursor(&r);
    } else {
        ClipCursor(nullptr);
        // When the grab is released, also drop Raw Input registration so
        // we don't receive WM_INPUT messages for unfocused mouse movement.
        unregisterRawInput();
    }
}

void WinMouseTap::registerRawInput(HWND hwnd)
{
    if (m_rawInputRegistered && m_registeredHwnd == hwnd) return;

    // Unregister any previous registration first (different hwnd, or
    // a stale registration from a previous session).
    if (m_rawInputRegistered) unregisterRawInput();

    RAWINPUTDEVICE rid;
    rid.usUsagePage = HID_USAGE_PAGE_GENERIC;   // 0x01 — generic desktop
    rid.usUsage     = HID_USAGE_GENERIC_MOUSE;  // 0x02 — mouse
    // RIDEV_INPUTSINK: keep receiving WM_INPUT even when the window is not
    // in the foreground (e.g. Alt+Tab momentarily loses focus but cursor is
    // still clipped).  Do NOT use RIDEV_NOLEGACY — we still need normal
    // WM_LBUTTONDOWN / WM_RBUTTONDOWN etc. for press/release handling.
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = hwnd;

    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        qWarning() << "WinMouseTap: RegisterRawInputDevices failed, error ="
                   << GetLastError();
        return;
    }

    m_rawInputRegistered = true;
    m_registeredHwnd     = hwnd;
    qDebug() << "WinMouseTap: Raw Input registered for hwnd" << hwnd;
}

void WinMouseTap::unregisterRawInput()
{
    if (!m_rawInputRegistered) return;

    RAWINPUTDEVICE rid;
    rid.usUsagePage = HID_USAGE_PAGE_GENERIC;
    rid.usUsage     = HID_USAGE_GENERIC_MOUSE;
    rid.dwFlags     = RIDEV_REMOVE;   // remove the registration
    rid.hwndTarget  = nullptr;        // must be null when removing

    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    m_rawInputRegistered = false;
    m_registeredHwnd     = nullptr;
    qDebug() << "WinMouseTap: Raw Input unregistered";
}
