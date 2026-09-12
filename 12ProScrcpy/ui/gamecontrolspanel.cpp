#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include "gamecontrolmarker.h"
#include "gamecontrolseditor.h"
#include "gamecontrolspanel.h"
#include "gameinterfacemonitor.h"
#include "config.h"
#include "iconhelper.h"
#include "toggleswitch.h"
#include "videoform.h"
#include "../12ProScrcpyCore/include/QtScrcpyCore.h"

namespace
{
// FontAwesome 4 glyphs, same icon font IconHelper already registers -
// f11c = keyboard-o, f11b = gamepad (the latter is what the toolbar's own
// "open controls" button already uses).
constexpr ushort kIconKeyboard = 0xf11c;
constexpr ushort kIconGamepad = 0xf11b;
constexpr ushort kIconHelp = 0xf059;
} // namespace

GameControlsPanel::GameControlsPanel(const QString &serial, QWidget *parent) : QWidget(parent), m_serial(serial)
{
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose, false);

    m_ifaceMonitor = new GameInterfaceMonitor(serial, this);
    connect(m_ifaceMonitor, &GameInterfaceMonitor::interfaceChanged, this, &GameControlsPanel::onInterfaceChanged);
    m_interfaceId = KeyMapProfileStore::unknownInterfaceId();

    loadPersistedSettings();
    buildUi();

    m_masterToggle->setChecked(m_masterEnabled);
    m_onScreenToggle->setChecked(m_onScreenVisible);
    m_opacitySlider->setValue(m_opacityPercent);
    m_opacityValueLabel->setText(QStringLiteral("%1%").arg(m_opacityPercent));

    reloadSchemeList();

    // Start identifying the foreground app immediately, not only once this
    // panel is first opened, so the Scheme list is already correct by the
    // time someone opens it.
    m_ifaceMonitor->start();
}

GameControlsPanel::~GameControlsPanel()
{
    clearOnScreenMarkers();
}

void GameControlsPanel::setVideoForm(VideoForm *videoForm)
{
    m_videoForm = videoForm;
    if (!m_videoForm) {
        return;
    }
    connect(m_videoForm, &VideoForm::windowActiveChanged, this, &GameControlsPanel::onWindowActiveChanged);
    if (QWidget *surface = m_videoForm->gameControlsSurface()) {
        surface->installEventFilter(this);
    }
    rebuildOnScreenMarkers();
}

bool GameControlsPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (m_videoForm && watched == m_videoForm->gameControlsSurface()) {
        if (event->type() == QEvent::Resize) {
            relayoutOnScreenMarkers();
        } else if (event->type() == QEvent::MouseButtonPress && isVisible()) {
            // Tapping the mirrored screen while this compact panel is open
            // dismisses it, matching expected transient-popup behavior -
            // opening is solely a product of clicking the toolbar's
            // control icon, and any tap elsewhere closes it again rather
            // than leaving it parked open indefinitely. Doesn't consume
            // the event, so the tap still reaches the normal touch-
            // forwarding path underneath.
            hide();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void GameControlsPanel::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *card = new QWidget(this);
    card->setObjectName("gcpCard");
    card->setAttribute(Qt::WA_StyledBackground, true);
    outer->addWidget(card);

    setStyleSheet(
        "QWidget#gcpCard { background:#16181d; border:1px solid #262b33; border-radius:10px; }"
        "QLabel#gcpTitle { color:#f2f3f5; font-weight:600; font-size:13px; }"
        "QLabel#gcpRowLabel { color:#d7dbe0; font-size:12px; }"
        "QLabel#gcpRowSub { color:#7d8590; font-size:10px; }"
        "QLabel#gcpValue { color:#9aa3af; font-size:11px; }"
        "QToolButton#gcpHelp { color:#7d8590; border:none; background:transparent; font-size:11px; }"
        "QToolButton[class=\"gcpModeBtn\"] { background:#20242c; border:1px solid #2f3541; border-radius:5px; padding:4px; "
        "color:#9aa3af; }"
        "QToolButton[class=\"gcpModeBtn\"]:checked { background:#1c3a2a; border-color:#22c55e; color:#22c55e; }"
        "QComboBox { background:#1d2127; border:1px solid #2f3541; border-radius:6px; padding:5px 8px; color:#e6e8eb; }"
        "QDoubleSpinBox { background:#1d2127; border:1px solid #2f3541; border-radius:6px; padding:3px 6px; color:#e6e8eb; }"
        "QPushButton#gcpOpenEditor { background:#22c55e; color:#0b1210; border:none; border-radius:8px; padding:9px; font-weight:600; }"
        "QPushButton#gcpOpenEditor:hover { background:#2ed474; }");

    auto *root = new QVBoxLayout(card);
    root->setContentsMargins(14, 12, 14, 14);
    root->setSpacing(12);

    // --- header: accent bar + title + help + master toggle ---
    auto *header = new QHBoxLayout;
    header->setSpacing(8);
    auto *accent = new QFrame(card);
    accent->setFixedSize(3, 15);
    accent->setStyleSheet("background:#22c55e; border-radius:1px;");
    header->addWidget(accent);
    auto *title = new QLabel(tr("Game controls"), card);
    title->setObjectName("gcpTitle");
    header->addWidget(title);
    auto *help = new QToolButton(card);
    help->setObjectName("gcpHelp");
    IconHelper::Instance()->SetIcon(help, QChar(kIconHelp), 10);
    help->setToolTip(tr("When on, this game's control scheme is sent to your phone while this window is focused - you don't need to "
                         "press ` to switch into it. ` still works as a manual override either way."));
    header->addWidget(help);
    header->addStretch(1);
    m_masterToggle = new ToggleSwitch(card);
    m_masterToggle->setToolTip(tr("Enable game controls for this app"));
    connect(m_masterToggle, &QAbstractButton::toggled, this, &GameControlsPanel::onMasterToggled);
    header->addWidget(m_masterToggle);
    root->addLayout(header);

    // --- on-screen controls ---
    auto *onScreenRow = new QHBoxLayout;
    auto *onScreenText = new QVBoxLayout;
    onScreenText->setSpacing(1);
    auto *onScreenLabel = new QLabel(tr("On-screen controls"), card);
    onScreenLabel->setObjectName("gcpRowLabel");
    auto *onScreenSub = new QLabel(tr("Show control hints over your game"), card);
    onScreenSub->setObjectName("gcpRowSub");
    onScreenText->addWidget(onScreenLabel);
    onScreenText->addWidget(onScreenSub);
    onScreenRow->addLayout(onScreenText);
    onScreenRow->addStretch(1);
    m_onScreenToggle = new ToggleSwitch(card);
    connect(m_onScreenToggle, &QAbstractButton::toggled, this, &GameControlsPanel::onOnScreenToggled);
    onScreenRow->addWidget(m_onScreenToggle);
    root->addLayout(onScreenRow);

    // --- opacity ---
    auto *opacityHeader = new QHBoxLayout;
    auto *opacityLabel = new QLabel(tr("Opacity"), card);
    opacityLabel->setObjectName("gcpRowLabel");
    opacityHeader->addWidget(opacityLabel);
    opacityHeader->addStretch(1);
    m_opacityValueLabel = new QLabel(card);
    m_opacityValueLabel->setObjectName("gcpValue");
    opacityHeader->addWidget(m_opacityValueLabel);
    root->addLayout(opacityHeader);

    m_opacitySlider = new QSlider(Qt::Horizontal, card);
    m_opacitySlider->setRange(0, 100);
    m_opacitySlider->setStyleSheet("QSlider::groove:horizontal { height:4px; background:#2b303a; border-radius:2px; }"
                                    "QSlider::sub-page:horizontal { background:#22c55e; border-radius:2px; }"
                                    "QSlider::add-page:horizontal { background:#2b303a; border-radius:2px; }"
                                    "QSlider::handle:horizontal { width:13px; height:13px; margin:-5px 0; background:#e6e8eb; "
                                    "border-radius:6px; }");
    connect(m_opacitySlider, &QSlider::valueChanged, this, &GameControlsPanel::onOpacityChanged);
    root->addWidget(m_opacitySlider);

    // --- controls for: keyboard / gamepad ---
    auto *modeRow = new QHBoxLayout;
    auto *modeLabel = new QLabel(tr("Controls for"), card);
    modeLabel->setObjectName("gcpRowLabel");
    modeRow->addWidget(modeLabel);
    modeRow->addStretch(1);
    m_keyboardBtn = new QToolButton(card);
    m_keyboardBtn->setProperty("class", "gcpModeBtn");
    m_keyboardBtn->setCheckable(true);
    m_keyboardBtn->setChecked(true);
    // The only mode this release supports - not user-toggleable off, since
    // there's nowhere else for input to go yet (gamepad is a placeholder).
    m_keyboardBtn->setEnabled(false);
    IconHelper::Instance()->SetIcon(m_keyboardBtn, QChar(kIconKeyboard), 12);
    m_keyboardBtn->setToolTip(tr("Keyboard & mouse"));
    modeRow->addWidget(m_keyboardBtn);
    m_gamepadBtn = new QToolButton(card);
    m_gamepadBtn->setProperty("class", "gcpModeBtn");
    m_gamepadBtn->setCheckable(true);
    m_gamepadBtn->setEnabled(false);
    IconHelper::Instance()->SetIcon(m_gamepadBtn, QChar(kIconGamepad), 12);
    m_gamepadBtn->setToolTip(tr("Gamepad - coming in a later release"));
    modeRow->addWidget(m_gamepadBtn);
    root->addLayout(modeRow);

    // --- scheme (hidden until this interface has at least one saved profile) ---
    m_schemeRow = new QWidget(card);
    auto *schemeLayout = new QVBoxLayout(m_schemeRow);
    schemeLayout->setContentsMargins(0, 0, 0, 0);
    schemeLayout->setSpacing(6);
    auto *schemeLabel = new QLabel(tr("Scheme"), m_schemeRow);
    schemeLabel->setObjectName("gcpRowLabel");
    schemeLayout->addWidget(schemeLabel);
    m_schemeCombo = new QComboBox(m_schemeRow);
    connect(m_schemeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &GameControlsPanel::onSchemeChanged);
    schemeLayout->addWidget(m_schemeCombo);
    root->addWidget(m_schemeRow);

    // --- mouse sensitivity (hidden unless the active scheme has a look node) ---
    m_sensitivityRow = new QWidget(card);
    auto *sensLayout = new QVBoxLayout(m_sensitivityRow);
    sensLayout->setContentsMargins(0, 0, 0, 0);
    sensLayout->setSpacing(6);
    auto *sensLabel = new QLabel(tr("Mouse sensitivity"), m_sensitivityRow);
    sensLabel->setObjectName("gcpRowLabel");
    sensLayout->addWidget(sensLabel);
    auto *sensFields = new QHBoxLayout;
    sensFields->addWidget(new QLabel(tr("X"), m_sensitivityRow));
    m_sensitivityX = new QDoubleSpinBox(m_sensitivityRow);
    m_sensitivityX->setRange(1.0, 50.0);
    m_sensitivityX->setSingleStep(0.5);
    m_sensitivityX->setDecimals(2);
    connect(m_sensitivityX, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &GameControlsPanel::onSensitivityXChanged);
    sensFields->addWidget(m_sensitivityX);
    sensFields->addSpacing(10);
    sensFields->addWidget(new QLabel(tr("Y"), m_sensitivityRow));
    m_sensitivityY = new QDoubleSpinBox(m_sensitivityRow);
    m_sensitivityY->setRange(1.0, 50.0);
    m_sensitivityY->setSingleStep(0.5);
    m_sensitivityY->setDecimals(2);
    connect(m_sensitivityY, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &GameControlsPanel::onSensitivityYChanged);
    sensFields->addWidget(m_sensitivityY);
    sensFields->addStretch(1);
    sensLayout->addLayout(sensFields);
    root->addWidget(m_sensitivityRow);

    // --- open full editor ---
    m_openEditorBtn = new QPushButton(card);
    m_openEditorBtn->setObjectName("gcpOpenEditor");
    m_openEditorBtn->setCursor(Qt::PointingHandCursor);
    // Plain unicode glyph (not the icon font) since this button mixes an
    // icon with regular text and QPushButton can't mix fonts in one label.
    m_openEditorBtn->setText(QStringLiteral("\u2630  ") + tr("Open controls editor"));
    connect(m_openEditorBtn, &QPushButton::clicked, this, &GameControlsPanel::onOpenEditorClicked);
    root->addWidget(m_openEditorBtn);

    setFixedWidth(240);
}

void GameControlsPanel::onInterfaceChanged(const QString &packageId, const QString &displayName)
{
    m_interfaceId = packageId;
    m_interfaceDisplayName = displayName;
    if (m_editor) {
        m_editor->setInterface(m_interfaceId, m_interfaceDisplayName);
    }
    reloadSchemeList();
}

void GameControlsPanel::onMasterToggled(bool enabled)
{
    m_masterEnabled = enabled;
    persistSettings();

    for (QWidget *w : { static_cast<QWidget *>(m_onScreenToggle), static_cast<QWidget *>(m_opacitySlider), m_schemeRow,
                         m_sensitivityRow }) {
        w->setEnabled(enabled);
    }

    applyLiveScript();
    refreshOnScreenVisibility();
}

void GameControlsPanel::onOnScreenToggled(bool enabled)
{
    m_onScreenVisible = enabled;
    persistSettings();
    refreshOnScreenVisibility();
}

void GameControlsPanel::onOpacityChanged(int value)
{
    m_opacityPercent = value;
    m_opacityValueLabel->setText(QStringLiteral("%1%").arg(value));
    for (const auto &marker : m_onScreenMarkers) {
        if (marker) {
            marker->setDisplayOpacityPercent(value);
        }
    }

    // QSlider::valueChanged fires continuously while dragging (dozens of
    // times per second) - persistSettings() forces a synchronous QSettings
    // disk sync every call, so calling it directly here was the actual
    // source of the lag: every drag tick blocked the UI thread on a file
    // write. Debounce it instead - only persist once the slider has been
    // quiet for a short moment, not on every single intermediate value.
    if (!m_opacityPersistDebounce) {
        m_opacityPersistDebounce = new QTimer(this);
        m_opacityPersistDebounce->setSingleShot(true);
        connect(m_opacityPersistDebounce, &QTimer::timeout, this, &GameControlsPanel::persistSettings);
    }
    m_opacityPersistDebounce->start(250);
}

void GameControlsPanel::onSchemeChanged(int index)
{
    const QString name = index >= 0 ? m_schemeCombo->itemText(index) : QString();
    m_currentProfileName = name;
    loadActiveScheme();
    applyLiveScript();
    rebuildOnScreenMarkers();
    updateSensitivityRowVisibility();
}

void GameControlsPanel::onSensitivityXChanged(double value)
{
    int idx = -1;
    if (!hasLookNode(&idx)) {
        return;
    }
    m_nodes[idx].lookSpeedX = static_cast<float>(value);
    applyLiveScript();
    QString error;
    KeyMapProfileStore::saveProfile(m_interfaceId, m_currentProfileName, m_switchKey, m_cursorLockKey, m_nodes, &error);
}

void GameControlsPanel::onSensitivityYChanged(double value)
{
    int idx = -1;
    if (!hasLookNode(&idx)) {
        return;
    }
    m_nodes[idx].lookSpeedY = static_cast<float>(value);
    applyLiveScript();
    QString error;
    KeyMapProfileStore::saveProfile(m_interfaceId, m_currentProfileName, m_switchKey, m_cursorLockKey, m_nodes, &error);
}

void GameControlsPanel::onOpenEditorClicked()
{
    if (!m_editor) {
        m_editor = new GameControlsEditor(m_serial, nullptr);
        m_editor->setAttribute(Qt::WA_DeleteOnClose, false);
        m_editor->setVideoForm(m_videoForm);
        connect(m_editor, &GameControlsEditor::profilesChanged, this, &GameControlsPanel::onEditorProfilesChanged);
    }

    if (m_editor->isVisible()) {
        m_editor->hide();
        return;
    }

    m_editor->setInterface(m_interfaceId, m_interfaceDisplayName);
    if (!m_currentProfileName.isEmpty()) {
        m_editor->selectProfile(m_currentProfileName);
    }
    m_editor->move(pos().x() - m_editor->width() - 12, pos().y());
    m_editor->show();
    m_editor->raise();
    m_editor->activateWindow();
}

void GameControlsPanel::onEditorProfilesChanged(const QString &interfaceId)
{
    if (interfaceId != m_interfaceId) {
        return;
    }
    reloadSchemeList(m_currentProfileName);
}

void GameControlsPanel::onWindowActiveChanged(bool active)
{
    m_windowActive = active;
    engageForFocus(active);
}

void GameControlsPanel::reloadSchemeList(const QString &selectName)
{
    const QStringList profiles = KeyMapProfileStore::listProfiles(m_interfaceId);

    m_schemeCombo->blockSignals(true);
    m_schemeCombo->clear();
    m_schemeCombo->addItems(profiles);
    m_schemeCombo->blockSignals(false);

    updateSchemeRowVisibility();

    if (profiles.isEmpty()) {
        m_currentProfileName.clear();
        loadActiveScheme();
        applyLiveScript();
        clearOnScreenMarkers();
        updateSensitivityRowVisibility();
        return;
    }

    QString target = selectName;
    if (target.isEmpty() || !profiles.contains(target)) {
        target = profiles.contains(m_currentProfileName) ? m_currentProfileName : profiles.first();
    }
    int idx = m_schemeCombo->findText(target);
    if (idx < 0) {
        idx = 0;
    }
    if (m_schemeCombo->currentIndex() == idx) {
        // setCurrentIndex() below won't emit currentIndexChanged (same
        // index) - drive the reload explicitly instead.
        onSchemeChanged(idx);
    } else {
        m_schemeCombo->setCurrentIndex(idx);
    }
}

void GameControlsPanel::loadActiveScheme()
{
    if (m_currentProfileName.isEmpty()) {
        m_nodes.clear();
        m_switchKey = KeyMapProfileStore::defaultSwitchKeyString();
        m_cursorLockKey = KeyMapProfileStore::defaultCursorLockKeyString();
        return;
    }
    QString error;
    if (!KeyMapProfileStore::loadProfile(m_interfaceId, m_currentProfileName, m_switchKey, m_cursorLockKey, m_nodes, &error)) {
        m_nodes.clear();
        m_switchKey = KeyMapProfileStore::defaultSwitchKeyString();
        m_cursorLockKey = KeyMapProfileStore::defaultCursorLockKeyString();
    }
}

void GameControlsPanel::applyLiveScript()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    if (!m_masterEnabled || m_currentProfileName.isEmpty()) {
        device->updateScript(QString());
        return;
    }
    device->updateScript(KeyMapProfileStore::toJson(m_switchKey, m_cursorLockKey, m_nodes));
    // updateScript() rebuilds the engine's InputConvertGame from scratch, so
    // any forced-on state from before this call is gone - reapply it.
    engageForFocus(m_windowActive);
}

void GameControlsPanel::engageForFocus(bool windowActive)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    if (!m_masterEnabled || m_currentProfileName.isEmpty()) {
        device->setCustomKeymapForced(false);
        return;
    }
    device->setCustomKeymapForced(windowActive);
}

void GameControlsPanel::updateSensitivityRowVisibility()
{
    int idx = -1;
    const bool hasLook = hasLookNode(&idx);
    m_sensitivityRow->setVisible(hasLook);
    if (!hasLook) {
        return;
    }
    m_sensitivityX->blockSignals(true);
    m_sensitivityY->blockSignals(true);
    m_sensitivityX->setValue(m_nodes[idx].lookSpeedX);
    m_sensitivityY->setValue(m_nodes[idx].lookSpeedY);
    m_sensitivityX->blockSignals(false);
    m_sensitivityY->blockSignals(false);
}

void GameControlsPanel::updateSchemeRowVisibility()
{
    // Matches the Figma "Collapsed" state: an interface with nothing saved
    // yet just shows the generic toggles and the button to go create one.
    m_schemeRow->setVisible(m_schemeCombo->count() > 0);
}

void GameControlsPanel::rebuildOnScreenMarkers()
{
    clearOnScreenMarkers();
    if (!m_videoForm) {
        return;
    }
    QWidget *surface = m_videoForm->gameControlsSurface();
    if (!surface) {
        return;
    }
    for (const ControlNode &node : m_nodes) {
        auto *marker = new GameControlMarker(node, surface);
        marker->setInteractive(false);
        marker->setDisplayOpacityPercent(m_opacityPercent);
        m_onScreenMarkers.append(marker);
    }
    relayoutOnScreenMarkers();
    refreshOnScreenVisibility();
}

void GameControlsPanel::clearOnScreenMarkers()
{
    for (const auto &marker : m_onScreenMarkers) {
        if (marker) {
            marker->deleteLater();
        }
    }
    m_onScreenMarkers.clear();
}

void GameControlsPanel::refreshOnScreenVisibility()
{
    const bool visible = m_masterEnabled && m_onScreenVisible && !m_nodes.isEmpty();
    for (const auto &marker : m_onScreenMarkers) {
        if (marker) {
            marker->setVisible(visible);
        }
    }
}

void GameControlsPanel::relayoutOnScreenMarkers()
{
    if (!m_videoForm) {
        return;
    }
    QWidget *surface = m_videoForm->gameControlsSurface();
    if (!surface) {
        return;
    }
    for (const auto &marker : m_onScreenMarkers) {
        if (marker) {
            marker->relayout(surface->size());
        }
    }
}

bool GameControlsPanel::hasLookNode(int *outIndex) const
{
    for (int i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i].action == ControlActionKind::FreeLook || m_nodes[i].action == ControlActionKind::AimPanShoot) {
            if (outIndex) {
                *outIndex = i;
            }
            return true;
        }
    }
    return false;
}

void GameControlsPanel::loadPersistedSettings()
{
    Config::getInstance().getGameControlsPanelSettings(m_serial, m_masterEnabled, m_onScreenVisible, m_opacityPercent);
}

void GameControlsPanel::persistSettings()
{
    Config::getInstance().setGameControlsPanelSettings(m_serial, m_masterEnabled, m_onScreenVisible, m_opacityPercent);
}
