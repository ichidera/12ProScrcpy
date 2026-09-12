#ifndef GAMECONTROLSPANEL_H
#define GAMECONTROLSPANEL_H

#include <QPointer>
#include <QVector>
#include <QWidget>

#include "keymapprofilestore.h"

class VideoForm;
class GameControlsEditor;
class GameInterfaceMonitor;
class GameControlMarker;
class ToggleSwitch;
class QComboBox;
class QSlider;
class QLabel;
class QToolButton;
class QDoubleSpinBox;
class QPushButton;

// The compact "Game controls" quick-settings panel (Figma: Collapsed /
// Scheme / Full settings states). This is what actually opens when the
// control button in ToolForm is clicked now; its own "Open controls editor"
// button is what launches the full node-placement GameControlsEditor.
//
// Responsible for:
//  - the master "Game control" toggle: when on, the active scheme stays
//    engaged for as long as the mirrored window has focus, instead of only
//    while the switch key (` by default, unaffected either way) is pressed
//  - "On-screen controls": a persistent, non-interactive, opacity-adjustable
//    display of the active scheme's controls over the mirrored screen
//  - "Controls for": Keyboard (available) / Gamepad (placeholder, disabled -
//    "coming in a later release")
//  - "Scheme": which of possibly several saved profiles for the current
//    foreground app ("interface", see GameInterfaceMonitor) is active -
//    hidden entirely when that app has no saved scheme yet, which is also
//    when this panel reads as plain "Game controls" (its default label)
//  - "Mouse sensitivity": X/Y look-speed for the active scheme's Free
//    look / Aim-pan-shoot node, shown only when it has one
class GameControlsPanel : public QWidget
{
    Q_OBJECT
public:
    explicit GameControlsPanel(const QString &serial, QWidget *parent = nullptr);
    ~GameControlsPanel() override;

    void setVideoForm(VideoForm *videoForm);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onInterfaceChanged(const QString &packageId, const QString &displayName);
    void onMasterToggled(bool enabled);
    void onOnScreenToggled(bool enabled);
    void onOpacityChanged(int value);
    void onSchemeChanged(int index);
    void onSensitivityXChanged(double value);
    void onSensitivityYChanged(double value);
    void onOpenEditorClicked();
    void onEditorProfilesChanged(const QString &interfaceId);
    void onWindowActiveChanged(bool active);

private:
    void buildUi();
    void reloadSchemeList(const QString &selectName = QString());
    void loadActiveScheme(); // (re)loads m_currentProfileName from disk into m_nodes/m_switchKey/m_cursorLockKey
    void applyLiveScript();  // pushes current nodes to the device via updateScript(), then re-engages focus state
    void engageForFocus(bool windowActive);
    void updateSensitivityRowVisibility();
    void updateSchemeRowVisibility();
    void rebuildOnScreenMarkers();
    void clearOnScreenMarkers();
    void refreshOnScreenVisibility();
    void relayoutOnScreenMarkers();
    bool hasLookNode(int *outIndex = nullptr) const;
    void loadPersistedSettings();
    void persistSettings();

private:
    QString m_serial;
    QPointer<VideoForm> m_videoForm;
    GameInterfaceMonitor *m_ifaceMonitor = nullptr;
    QPointer<GameControlsEditor> m_editor;

    QString m_interfaceId;
    QString m_interfaceDisplayName;
    QString m_currentProfileName; // empty = no saved scheme yet for this interface
    QString m_switchKey;
    QString m_cursorLockKey;
    QVector<ControlNode> m_nodes;
    QVector<QPointer<GameControlMarker>> m_onScreenMarkers;

    bool m_masterEnabled = true;
    bool m_onScreenVisible = true;
    int m_opacityPercent = 72;
    bool m_windowActive = false;

    // UI
    ToggleSwitch *m_masterToggle = nullptr;
    ToggleSwitch *m_onScreenToggle = nullptr;
    QSlider *m_opacitySlider = nullptr;
    QLabel *m_opacityValueLabel = nullptr;
    QToolButton *m_keyboardBtn = nullptr;
    QToolButton *m_gamepadBtn = nullptr;
    QWidget *m_schemeRow = nullptr;
    QComboBox *m_schemeCombo = nullptr;
    QWidget *m_sensitivityRow = nullptr;
    QDoubleSpinBox *m_sensitivityX = nullptr;
    QDoubleSpinBox *m_sensitivityY = nullptr;
    QPushButton *m_openEditorBtn = nullptr;
};

#endif // GAMECONTROLSPANEL_H
