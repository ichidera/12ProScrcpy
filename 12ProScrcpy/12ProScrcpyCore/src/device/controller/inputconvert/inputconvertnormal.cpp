#include <cmath>
#include <QDebug>

#include "inputconvertnormal.h"
#include "controller.h"

InputConvertNormal::InputConvertNormal(Controller *controller) : InputConvertBase(controller) {}

InputConvertNormal::~InputConvertNormal() {}

void InputConvertNormal::mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (!from) {
        return;
    }

    // action
    AndroidMotioneventAction action;
    switch (from->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
        // Qt's own OS-level double-click detection turns every *second*
        // rapid click into a QEvent::MouseButtonDblClick instead of another
        // MouseButtonPress (Press -> Release -> DblClick -> Release). With
        // no case for it here, that DOWN was silently dropped (fell to
        // `default: return`), while its matching Release still went out as
        // an UP with no paired DOWN - net effect: every other fast mouse
        // click did nothing on-device, while finger taps on the real
        // touchscreen (no such merging) always registered. InputConvertGame
        // already treats DblClick as a DOWN trigger (see
        // processMouseClick()); do the same here.
        action = AMOTION_EVENT_ACTION_DOWN;
        break;
    case QEvent::MouseButtonRelease:
        action = AMOTION_EVENT_ACTION_UP;
        break;
    case QEvent::MouseMove:
        // only support left button drag
        if (!(from->buttons() & Qt::LeftButton)) {
            return;
        }
        action = AMOTION_EVENT_ACTION_MOVE;
        break;
    default:
        return;
    }

    // pos
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QPointF pos = from->localPos();
#else
    QPointF pos = from->position();
#endif
    // convert pos
    pos.setX(pos.x() * frameSize.width() / showSize.width());
    pos.setY(pos.y() * frameSize.height() / showSize.height());

    // Inject the touch directly via `adb shell sendevent` (real kernel input
    // node), instead of the old control-socket CMT_INJECT_TOUCH message.
    if (m_controller) {
        m_controller->sendRealTouch(Controller::kMouseTouchSlot, action, pos.toPoint(), frameSize);
    }
}

void InputConvertNormal::wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (!from || from->angleDelta().isNull()) {
        return;
    }

    // delta
    float hScroll = from->angleDelta().x() / 64.0f;
    float vScroll = from->angleDelta().y() / 64.0f;

    // pos
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    QPointF pos = from->position();
#else
    QPointF pos = from->posF();
#endif
    // convert pos
    pos.setX(pos.x() * frameSize.width() / showSize.width());
    pos.setY(pos.y() * frameSize.height() / showSize.height());

    // Inject as a real touch swipe via `adb shell sendevent` instead of the
    // old control-socket CMT_INJECT_SCROLL message - see Controller::sendRealScroll.
    if (m_controller) {
        m_controller->sendRealScroll(pos.toPoint(), frameSize, hScroll, vScroll);
    }
}

void InputConvertNormal::keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize)
{
    Q_UNUSED(frameSize)
    Q_UNUSED(showSize)
    if (!from) {
        return;
    }

    // action
    AndroidKeyeventAction action;
    switch (from->type()) {
    case QEvent::KeyPress:
        action = AKEY_EVENT_ACTION_DOWN;
        break;
    case QEvent::KeyRelease:
        action = AKEY_EVENT_ACTION_UP;
        break;
    default:
        return;
    }

    // key code
    AndroidKeycode keyCode = convertKeyCode(from->key(), from->modifiers());
    if (AKEYCODE_UNKNOWN == keyCode) {
        return;
    }

    // `input keyevent` (root-elevated, same su session as touch) fires an
    // atomic press+release and takes a bare keycode with no metastate flag -
    // there's no way to convey Shift/Ctrl/Alt modifiers through it the way
    // the old ControlMsg socket path's convertMetastate() could. Known
    // regression versus the socket path: Shift+letter, Ctrl+C, etc. will
    // not carry the modifier through this fallback.
    //
    // Fire only on physical key-down; Qt's own auto-repeat (isAutoRepeat())
    // re-fires KeyPress while a key is held, so a held key still produces
    // repeated presses server-side without us needing to track hold state
    // ourselves the way m_repeat used to.
    if (action != AKEY_EVENT_ACTION_DOWN) {
        return;
    }
    if (m_controller) {
        m_controller->sendRealKeyEvent(static_cast<int>(keyCode));
    }
}

AndroidMotioneventButtons InputConvertNormal::convertMouseButtons(Qt::MouseButtons buttonState)
{
    quint32 buttons = 0;
    if (buttonState & Qt::LeftButton) {
        buttons |= AMOTION_EVENT_BUTTON_PRIMARY;
    }
    if (buttonState & Qt::RightButton) {
        buttons |= AMOTION_EVENT_BUTTON_SECONDARY;
    }
#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
    if (buttonState & Qt::MiddleButton) {
#else
    if (buttonState & Qt::MidButton) {
#endif    
        buttons |= AMOTION_EVENT_BUTTON_TERTIARY;
    }
    if (buttonState & Qt::XButton1) {
        buttons |= AMOTION_EVENT_BUTTON_BACK;
    }
    if (buttonState & Qt::XButton2) {
        buttons |= AMOTION_EVENT_BUTTON_FORWARD;
    }
    return static_cast<AndroidMotioneventButtons>(buttons);
}

AndroidKeycode InputConvertNormal::convertKeyCode(int key, Qt::KeyboardModifiers modifiers)
{
    AndroidKeycode keyCode = AKEYCODE_UNKNOWN;
    // functional keys
    switch (key) {
    case Qt::Key_Return:
        keyCode = AKEYCODE_ENTER;
        break;
    case Qt::Key_Enter:
        keyCode = AKEYCODE_NUMPAD_ENTER;
        break;
    case Qt::Key_Escape:
        keyCode = AKEYCODE_ESCAPE;
        break;
    case Qt::Key_Backspace:
        keyCode = AKEYCODE_DEL;
        break;
    case Qt::Key_Delete:
        keyCode = AKEYCODE_FORWARD_DEL;
        break;
    case Qt::Key_Tab:
        keyCode = AKEYCODE_TAB;
        break;
    case Qt::Key_Home:
        keyCode = AKEYCODE_MOVE_HOME;
        break;
    case Qt::Key_End:
        keyCode = AKEYCODE_MOVE_END;
        break;
    case Qt::Key_PageUp:
        keyCode = AKEYCODE_PAGE_UP;
        break;
    case Qt::Key_PageDown:
        keyCode = AKEYCODE_PAGE_DOWN;
        break;
    case Qt::Key_Left:
        keyCode = AKEYCODE_DPAD_LEFT;
        break;
    case Qt::Key_Right:
        keyCode = AKEYCODE_DPAD_RIGHT;
        break;
    case Qt::Key_Up:
        keyCode = AKEYCODE_DPAD_UP;
        break;
    case Qt::Key_Down:
        keyCode = AKEYCODE_DPAD_DOWN;
        break;
    }
    if (AKEYCODE_UNKNOWN != keyCode) {
        return keyCode;
    }

    // if ALT and META are pressed, dont handle letters and space
    if (modifiers & (Qt::AltModifier | Qt::MetaModifier)) {
        return keyCode;
    }

    // character keys
    switch (key) {
    case Qt::Key_A:
        keyCode = AKEYCODE_A;
        break;
    case Qt::Key_B:
        keyCode = AKEYCODE_B;
        break;
    case Qt::Key_C:
        keyCode = AKEYCODE_C;
        break;
    case Qt::Key_D:
        keyCode = AKEYCODE_D;
        break;
    case Qt::Key_E:
        keyCode = AKEYCODE_E;
        break;
    case Qt::Key_F:
        keyCode = AKEYCODE_F;
        break;
    case Qt::Key_G:
        keyCode = AKEYCODE_G;
        break;
    case Qt::Key_H:
        keyCode = AKEYCODE_H;
        break;
    case Qt::Key_I:
        keyCode = AKEYCODE_I;
        break;
    case Qt::Key_J:
        keyCode = AKEYCODE_J;
        break;
    case Qt::Key_K:
        keyCode = AKEYCODE_K;
        break;
    case Qt::Key_L:
        keyCode = AKEYCODE_L;
        break;
    case Qt::Key_M:
        keyCode = AKEYCODE_M;
        break;
    case Qt::Key_N:
        keyCode = AKEYCODE_N;
        break;
    case Qt::Key_O:
        keyCode = AKEYCODE_O;
        break;
    case Qt::Key_P:
        keyCode = AKEYCODE_P;
        break;
    case Qt::Key_Q:
        keyCode = AKEYCODE_Q;
        break;
    case Qt::Key_R:
        keyCode = AKEYCODE_R;
        break;
    case Qt::Key_S:
        keyCode = AKEYCODE_S;
        break;
    case Qt::Key_T:
        keyCode = AKEYCODE_T;
        break;
    case Qt::Key_U:
        keyCode = AKEYCODE_U;
        break;
    case Qt::Key_V:
        keyCode = AKEYCODE_V;
        break;
    case Qt::Key_W:
        keyCode = AKEYCODE_W;
        break;
    case Qt::Key_X:
        keyCode = AKEYCODE_X;
        break;
    case Qt::Key_Y:
        keyCode = AKEYCODE_Y;
        break;
    case Qt::Key_Z:
        keyCode = AKEYCODE_Z;
        break;
    case Qt::Key_0:
        keyCode = AKEYCODE_0;
        break;
    case Qt::Key_1:
    case Qt::Key_Exclam: // !
        keyCode = AKEYCODE_1;
        break;
    case Qt::Key_2:
        keyCode = AKEYCODE_2;
        break;
    case Qt::Key_3:
        keyCode = AKEYCODE_3;
        break;
    case Qt::Key_4:
    case Qt::Key_Dollar: //$
        keyCode = AKEYCODE_4;
        break;
    case Qt::Key_5:
    case Qt::Key_Percent: // %
        keyCode = AKEYCODE_5;
        break;
    case Qt::Key_6:
    case Qt::Key_AsciiCircum: //^
        keyCode = AKEYCODE_6;
        break;
    case Qt::Key_7:
    case Qt::Key_Ampersand: //&
        keyCode = AKEYCODE_7;
        break;
    case Qt::Key_8:
        keyCode = AKEYCODE_8;
        break;
    case Qt::Key_9:
        keyCode = AKEYCODE_9;
        break;
    case Qt::Key_Space:
        keyCode = AKEYCODE_SPACE;
        break;
    case Qt::Key_Comma: //,
    case Qt::Key_Less:  //<
        keyCode = AKEYCODE_COMMA;
        break;
    case Qt::Key_Period:  //.
    case Qt::Key_Greater: //>
        keyCode = AKEYCODE_PERIOD;
        break;
    case Qt::Key_Minus:      //-
    case Qt::Key_Underscore: //_
        keyCode = AKEYCODE_MINUS;
        break;
    case Qt::Key_Equal: //=
        keyCode = AKEYCODE_EQUALS;
        break;
    case Qt::Key_BracketLeft: //[
    case Qt::Key_BraceLeft:   //{
        keyCode = AKEYCODE_LEFT_BRACKET;
        break;
    case Qt::Key_BracketRight: //]
    case Qt::Key_BraceRight:   //}
        keyCode = AKEYCODE_RIGHT_BRACKET;
        break;
    case Qt::Key_Backslash: // \ ????
    case Qt::Key_Bar:       //|
        keyCode = AKEYCODE_BACKSLASH;
        break;
    case Qt::Key_Semicolon: //;
    case Qt::Key_Colon:     //:
        keyCode = AKEYCODE_SEMICOLON;
        break;
    case Qt::Key_Apostrophe: //'
    case Qt::Key_QuoteDbl:   //"
        keyCode = AKEYCODE_APOSTROPHE;
        break;
    case Qt::Key_Slash:    // /
    case Qt::Key_Question: //?
        keyCode = AKEYCODE_SLASH;
        break;
    case Qt::Key_At: //@
        keyCode = AKEYCODE_AT;
        break;
    case Qt::Key_Plus: //+
        keyCode = AKEYCODE_PLUS;
        break;
    case Qt::Key_QuoteLeft:  //`
    case Qt::Key_AsciiTilde: //~
        keyCode = AKEYCODE_GRAVE;
        break;
    case Qt::Key_NumberSign: //#
        keyCode = AKEYCODE_POUND;
        break;
    case Qt::Key_ParenLeft: //(
        keyCode = AKEYCODE_NUMPAD_LEFT_PAREN;
        break;
    case Qt::Key_ParenRight: //)
        keyCode = AKEYCODE_NUMPAD_RIGHT_PAREN;
        break;
    case Qt::Key_Asterisk: //*
        keyCode = AKEYCODE_STAR;
        break;
    }
    return keyCode;
}

AndroidMetastate InputConvertNormal::convertMetastate(Qt::KeyboardModifiers modifiers)
{
    int metastate = AMETA_NONE;

    if (modifiers & Qt::ShiftModifier) {
        metastate |= AMETA_SHIFT_ON;
    }
    if (modifiers & Qt::ControlModifier) {
        metastate |= AMETA_CTRL_ON;
    }
    if (modifiers & Qt::AltModifier) {
        metastate |= AMETA_ALT_ON;
    }
    if (modifiers & Qt::MetaModifier) {
        metastate |= AMETA_META_ON;
    }
    /*
    if (mod & KMOD_NUM) {
        metastate |= AMETA_NUM_LOCK_ON;
    }
    if (mod & KMOD_CAPS) {
        metastate |= AMETA_CAPS_LOCK_ON;
    }
    if (mod & KMOD_MODE) { // Alt Gr
        // no mapping?
    }
    */
    return static_cast<AndroidMetastate>(metastate);
}