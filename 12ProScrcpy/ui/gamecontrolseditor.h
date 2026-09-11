#ifndef GAMECONTROLSEDITOR_H
#define GAMECONTROLSEDITOR_H

#include <QPointer>
#include <QVector>
#include <QWidget>

#include "keymapprofilestore.h"

// MIME type used to drag a palette action (see PaletteButton in the .cpp)
// from this panel onto VideoForm's mirrored screen.
constexpr const char *kGameControlMimeType = "application/x-qtscrcpy-control-action";

class VideoForm;
class GameControlMarker;
class EditModeOverlay;
class QComboBox;
class KeyCaptureButton;

// The "App Control" panel: replaces hand-writing a keymap .json - profiles
// are created, edited and saved entirely from this UI, then applied live via
// IDevice::updateScript() (no restart of the mirror session needed).
class GameControlsEditor : public QWidget
{
    Q_OBJECT
public:
    explicit GameControlsEditor(const QString &serial, QWidget *parent = nullptr);

    // Must be called once, before the panel is useful: gives the editor a
    // surface to host markers on and registers it so VideoForm can route
    // palette drops back here.
    void setVideoForm(VideoForm *videoForm);

    // Called by VideoForm::dropEvent() when a palette drag lands on the
    // mirrored screen. normPos is in [0,1] video-relative coordinates.
    void handleControlDropped(ControlActionKind kind, QPointF normPos);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onProfileChanged(int index);
    void onNewProfile();
    void onSaveProfile();
    void onDeleteProfile();
    void onCancelEdits();

private:
    void buildUi();
    void reloadProfileList(const QString &selectName = QString());
    void clearMarkers();
    void addMarkerForNode(const ControlNode &node);
    void relayoutMarkers();
    void applyLive();
    void editMarker(GameControlMarker *marker);
    void removeMarker(GameControlMarker *marker);
    bool hasLookNode(GameControlMarker *exclude = nullptr) const;
    void ensureOverlay();
    void setDirty(bool dirty);

private:
    QString m_serial;
    QPointer<VideoForm> m_videoForm;
    QPointer<EditModeOverlay> m_overlay;

    QComboBox *m_profileCombo = nullptr;
    KeyCaptureButton *m_switchKeyCapture = nullptr;     // activates the scheme (default `)
    KeyCaptureButton *m_cursorLockKeyCapture = nullptr; // locks+hides the cursor (default F1)

    QVector<QPointer<GameControlMarker>> m_markers;
    QString m_currentProfileName;
    bool m_dirty = false;
};

#endif // GAMECONTROLSEDITOR_H
