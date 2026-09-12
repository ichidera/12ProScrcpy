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
class QToolButton;
class QLabel;
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

    // Scopes every profile operation below to one "interface" (Android
    // package id) - see GameInterfaceMonitor. Switching interfaces reloads
    // the profile list and clears any markers on screen. displayName is
    // used only for the window title (e.g. "Controls editor — Genshin Impact").
    void setInterface(const QString &interfaceId, const QString &displayName);

    // Selects an existing profile by name (e.g. the one currently active in
    // GameControlsPanel) so opening the full editor lands on the same
    // scheme instead of always starting from "(unsaved scheme)".
    void selectProfile(const QString &name);

    // Called by VideoForm::dropEvent() when a palette drag lands on the
    // mirrored screen. normPos is in [0,1] video-relative coordinates.
    void handleControlDropped(ControlActionKind kind, QPointF normPos);

signals:
    // Fired after a save/new/delete actually changes what's on disk for
    // this interface, so GameControlsPanel can refresh its Scheme picker
    // (and reload live data if the affected profile is the active one).
    void profilesChanged(const QString &interfaceId);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onProfileChanged(int index);
    void onNewProfile();
    void onRenameProfile();
    void onDuplicateProfile();
    void onSaveProfile();
    void onDeleteProfile();
    void onCancelEdits();

private:
    void buildUi();
    QWidget *buildControlSchemeSection();
    QWidget *buildPaletteSection();
    QToolButton *makeGlyphButton(QChar glyph, const QString &tooltip, bool enabled, QWidget *parent);
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
    QString m_interfaceId;   // Android package id this editor's profiles belong to
    QString m_interfaceLabel; // friendly name, title bar only
    QPointer<VideoForm> m_videoForm;
    QPointer<EditModeOverlay> m_overlay;

    QComboBox *m_profileCombo = nullptr;
    QLabel *m_titleLabel = nullptr;
    KeyCaptureButton *m_switchKeyCapture = nullptr;     // activates the scheme (default `)
    KeyCaptureButton *m_cursorLockKeyCapture = nullptr; // locks+hides the cursor (default F1)

    QVector<QPointer<GameControlMarker>> m_markers;
    QString m_currentProfileName;
    bool m_dirty = false;
};

#endif // GAMECONTROLSEDITOR_H
