#include <QComboBox>
#include <QDrag>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "controlinspectordialog.h"
#include "editmodeoverlay.h"
#include "gamecontrolmarker.h"
#include "gamecontrolseditor.h"
#include "videoform.h"
#include "../12ProScrcpyCore/include/QtScrcpyCore.h"

namespace
{
// A palette entry the user drags onto the mirrored screen to place a new
// control. Left-click-and-drag starts a QDrag carrying the ControlActionKind.
class PaletteButton : public QToolButton
{
public:
    PaletteButton(ControlActionKind kind, QWidget *parent) : QToolButton(parent), m_kind(kind)
    {
        setText(KeyMapProfileStore::actionLabel(kind));
        setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        setMinimumSize(76, 56);
        setCursor(Qt::OpenHandCursor);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_pressPos = event->position().toPoint();
        }
        QToolButton::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if ((event->buttons() & Qt::LeftButton) && (event->position().toPoint() - m_pressPos).manhattanLength() > 12) {
            auto *drag = new QDrag(this);
            auto *mime = new QMimeData();
            mime->setData(kGameControlMimeType, QByteArray::number(static_cast<int>(m_kind)));
            drag->setMimeData(mime);
            drag->exec(Qt::CopyAction);
            return;
        }
        QToolButton::mouseMoveEvent(event);
    }

private:
    ControlActionKind m_kind;
    QPoint m_pressPos;
};
} // namespace

GameControlsEditor::GameControlsEditor(const QString &serial, QWidget *parent)
    : QWidget(parent), m_serial(serial), m_interfaceId(KeyMapProfileStore::unknownInterfaceId())
{
    buildUi();
    reloadProfileList();
}

void GameControlsEditor::buildUi()
{
    setWindowTitle(tr("Controls editor"));
    setMinimumWidth(240);

    auto *layout = new QVBoxLayout(this);

    auto *header = new QLabel(tr("Control scheme"), this);
    header->setStyleSheet("font-weight: bold;");
    layout->addWidget(header);

    m_profileCombo = new QComboBox(this);
    m_profileCombo->setEditable(false);
    layout->addWidget(m_profileCombo);
    connect(m_profileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &GameControlsEditor::onProfileChanged);

    auto *profileBtnRow = new QWidget(this);
    auto *profileBtnLayout = new QHBoxLayout(profileBtnRow);
    profileBtnLayout->setContentsMargins(0, 0, 0, 0);
    auto *newBtn = new QPushButton(tr("New"), profileBtnRow);
    auto *saveBtn = new QPushButton(tr("Save"), profileBtnRow);
    auto *deleteBtn = new QPushButton(tr("Delete"), profileBtnRow);
    profileBtnLayout->addWidget(newBtn);
    profileBtnLayout->addWidget(saveBtn);
    profileBtnLayout->addWidget(deleteBtn);
    layout->addWidget(profileBtnRow);
    connect(newBtn, &QPushButton::clicked, this, &GameControlsEditor::onNewProfile);
    connect(saveBtn, &QPushButton::clicked, this, &GameControlsEditor::onSaveProfile);
    connect(deleteBtn, &QPushButton::clicked, this, &GameControlsEditor::onDeleteProfile);

    auto *switchRow = new QWidget(this);
    auto *switchLayout = new QHBoxLayout(switchRow);
    switchLayout->setContentsMargins(0, 0, 0, 0);
    switchLayout->addWidget(new QLabel(tr("Touch-mode toggle:"), switchRow));
    m_switchKeyCapture = new KeyCaptureButton(switchRow);
    m_switchKeyCapture->setBoundKeyString(KeyMapProfileStore::defaultSwitchKeyString());
    switchLayout->addWidget(m_switchKeyCapture);
    layout->addWidget(switchRow);
    connect(m_switchKeyCapture, &KeyCaptureButton::keyChanged, this, [this](const QString &) { setDirty(true); });

    // Deliberately a *separate* key from the touch-mode toggle above: this
    // one only grabs+hides the cursor (BlueStacks' "enter/exit shooting
    // mode"), so `` ` `` can stay a plain focus switch while a dedicated key
    // (F1 by default) is the one that locks the mouse for aiming.
    auto *lockRow = new QWidget(this);
    auto *lockLayout = new QHBoxLayout(lockRow);
    lockLayout->setContentsMargins(0, 0, 0, 0);
    lockLayout->addWidget(new QLabel(tr("Lock/hide cursor:"), lockRow));
    m_cursorLockKeyCapture = new KeyCaptureButton(lockRow);
    m_cursorLockKeyCapture->setBoundKeyString(KeyMapProfileStore::defaultCursorLockKeyString());
    lockLayout->addWidget(m_cursorLockKeyCapture);
    layout->addWidget(lockRow);
    connect(m_cursorLockKeyCapture, &KeyCaptureButton::keyChanged, this, [this](const QString &) { setDirty(true); });

    auto *addLabel = new QLabel(tr("Add controls"), this);
    addLabel->setStyleSheet("font-weight: bold; margin-top: 8px;");
    layout->addWidget(addLabel);
    auto *hint = new QLabel(tr("Drag an action onto your game screen to place it."), this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: gray; font-size: 11px;");
    layout->addWidget(hint);

    auto *palette = new QWidget(this);
    auto *grid = new QGridLayout(palette);
    const ControlActionKind kinds[] = { ControlActionKind::TapSpot,   ControlActionKind::RepeatedTap, ControlActionKind::DPad,
                                         ControlActionKind::DragSwipe, ControlActionKind::FreeLook,    ControlActionKind::AimPanShoot };
    for (int i = 0; i < 6; ++i) {
        grid->addWidget(new PaletteButton(kinds[i], palette), i / 2, i % 2);
    }
    layout->addWidget(palette);

    layout->addStretch();
}

void GameControlsEditor::setVideoForm(VideoForm *videoForm)
{
    m_videoForm = videoForm;
    if (m_videoForm) {
        m_videoForm->setGameControlsEditor(this);
        if (QWidget *surface = m_videoForm->gameControlsSurface()) {
            surface->installEventFilter(this);
        }
    }
}

void GameControlsEditor::setInterface(const QString &interfaceId, const QString &displayName)
{
    const QString normalizedId = interfaceId.isEmpty() ? KeyMapProfileStore::unknownInterfaceId() : interfaceId;
    if (m_interfaceId == normalizedId) {
        m_interfaceLabel = displayName;
        setWindowTitle(m_interfaceLabel.isEmpty() ? tr("Controls editor") : tr("Controls editor — %1").arg(m_interfaceLabel));
        return;
    }
    m_interfaceId = normalizedId;
    m_interfaceLabel = displayName;
    setWindowTitle(m_interfaceLabel.isEmpty() ? tr("Controls editor") : tr("Controls editor — %1").arg(m_interfaceLabel));
    clearMarkers();
    m_currentProfileName.clear();
    reloadProfileList();
}

void GameControlsEditor::selectProfile(const QString &name)
{
    if (name.isEmpty()) {
        return;
    }
    const int idx = m_profileCombo->findText(name);
    if (idx >= 0) {
        m_profileCombo->setCurrentIndex(idx);
    }
}

void GameControlsEditor::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    ensureOverlay();
    relayoutMarkers();
}

void GameControlsEditor::hideEvent(QHideEvent *event)
{
    // Tear the click-shield down so the mirrored screen is fully live again
    // as soon as the editor panel isn't visible.
    if (m_overlay) {
        m_overlay->deleteLater();
    }
    QWidget::hideEvent(event);
}

void GameControlsEditor::ensureOverlay()
{
    if (m_overlay || !m_videoForm) {
        return;
    }
    QWidget *surface = m_videoForm->gameControlsSurface();
    if (!surface) {
        return;
    }
    m_overlay = new EditModeOverlay(surface);
    m_overlay->setGeometry(surface->rect());
    m_overlay->show();
    m_overlay->lower(); // stay under GameControlMarker children, above raw video
    m_overlay->setDirty(m_dirty);
    connect(m_overlay, &EditModeOverlay::saveRequested, this, &GameControlsEditor::onSaveProfile);
    connect(m_overlay, &EditModeOverlay::cancelRequested, this, &GameControlsEditor::onCancelEdits);
    connect(m_overlay, &EditModeOverlay::controlDropped, this, &GameControlsEditor::handleControlDropped);
}

void GameControlsEditor::setDirty(bool dirty)
{
    m_dirty = dirty;
    if (m_overlay) {
        m_overlay->setDirty(dirty);
    }
}

bool GameControlsEditor::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Resize && m_videoForm && watched == m_videoForm->gameControlsSurface()) {
        relayoutMarkers();
        if (m_overlay) {
            m_overlay->setGeometry(m_videoForm->gameControlsSurface()->rect());
        }
    }
    return QWidget::eventFilter(watched, event);
}

void GameControlsEditor::reloadProfileList(const QString &selectName)
{
    const QStringList profiles = KeyMapProfileStore::listProfiles(m_interfaceId);
    m_profileCombo->blockSignals(true);
    m_profileCombo->clear();
    // Default entry shown for an interface with no saved scheme yet -
    // matches the generic "Game controls" label the compact panel shows in
    // that same situation, instead of a raw "(unsaved scheme)" placeholder.
    m_profileCombo->addItem(KeyMapProfileStore::defaultProfileDisplayName());
    m_profileCombo->addItems(profiles);
    m_profileCombo->blockSignals(false);

    if (!selectName.isEmpty()) {
        int idx = m_profileCombo->findText(selectName);
        if (idx >= 0) {
            m_profileCombo->setCurrentIndex(idx);
            return;
        }
    }
    m_profileCombo->setCurrentIndex(0);
}

void GameControlsEditor::onProfileChanged(int index)
{
    clearMarkers();
    if (index <= 0) {
        m_currentProfileName.clear();
        m_switchKeyCapture->setBoundKeyString(KeyMapProfileStore::defaultSwitchKeyString());
        m_cursorLockKeyCapture->setBoundKeyString(KeyMapProfileStore::defaultCursorLockKeyString());
        setDirty(false);
        return;
    }

    const QString name = m_profileCombo->itemText(index);
    QString switchKey;
    QString cursorLockKey;
    QVector<ControlNode> nodes;
    QString error;
    if (!KeyMapProfileStore::loadProfile(m_interfaceId, name, switchKey, cursorLockKey, nodes, &error)) {
        QMessageBox::warning(this, tr("Controls editor"), tr("Could not load profile: %1").arg(error));
        return;
    }

    m_currentProfileName = name;
    m_switchKeyCapture->setBoundKeyString(switchKey);
    m_cursorLockKeyCapture->setBoundKeyString(cursorLockKey);
    for (const ControlNode &node : nodes) {
        addMarkerForNode(node);
    }
    relayoutMarkers();
    setDirty(false);
}

void GameControlsEditor::onNewProfile()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New control scheme"), tr("Name:"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    if (KeyMapProfileStore::profileExists(m_interfaceId, name)) {
        QMessageBox::warning(this, tr("Controls editor"), tr("A profile named \"%1\" already exists.").arg(name));
        return;
    }

    clearMarkers();
    m_currentProfileName = name;
    m_switchKeyCapture->setBoundKeyString(KeyMapProfileStore::defaultSwitchKeyString());
    m_cursorLockKeyCapture->setBoundKeyString(KeyMapProfileStore::defaultCursorLockKeyString());

    QVector<ControlNode> empty;
    QString error;
    if (!KeyMapProfileStore::saveProfile(m_interfaceId, name, m_switchKeyCapture->boundKeyString(), m_cursorLockKeyCapture->boundKeyString(),
                                          empty, &error)) {
        QMessageBox::warning(this, tr("Controls editor"), tr("Could not create profile: %1").arg(error));
        return;
    }
    reloadProfileList(name);
    emit profilesChanged(m_interfaceId);
}

void GameControlsEditor::onSaveProfile()
{
    QString name = m_currentProfileName;
    if (name.isEmpty()) {
        bool ok = false;
        name = QInputDialog::getText(this, tr("Save control scheme"), tr("Name:"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || name.isEmpty()) {
            return;
        }
    }

    QVector<ControlNode> nodes;
    for (const QPointer<GameControlMarker> &marker : m_markers) {
        if (marker) {
            nodes.push_back(marker->node());
        }
    }

    QString error;
    if (!KeyMapProfileStore::saveProfile(m_interfaceId, name, m_switchKeyCapture->boundKeyString(), m_cursorLockKeyCapture->boundKeyString(),
                                          nodes, &error)) {
        QMessageBox::warning(this, tr("Controls editor"), tr("Could not save profile: %1").arg(error));
        return;
    }

    m_currentProfileName = name;
    reloadProfileList(name);
    applyLive();
    setDirty(false);
    emit profilesChanged(m_interfaceId);
}

void GameControlsEditor::onCancelEdits()
{
    // BlueStacks-style "Reset": discard unsaved edits and reload whatever is
    // currently persisted on disk for this profile.
    if (m_dirty
        && QMessageBox::question(this, tr("Discard changes?"),
                                  tr("Revert \"%1\" to its last saved state? Unsaved changes will be lost.")
                                      .arg(m_currentProfileName.isEmpty() ? KeyMapProfileStore::defaultProfileDisplayName()
                                                                           : m_currentProfileName))
               != QMessageBox::Yes) {
        return;
    }
    clearMarkers();
    onProfileChanged(m_profileCombo->currentIndex());
    setDirty(false);
}

void GameControlsEditor::onDeleteProfile()
{
    if (m_currentProfileName.isEmpty()) {
        return;
    }
    if (QMessageBox::question(this, tr("Delete control scheme"), tr("Delete \"%1\"? This cannot be undone.").arg(m_currentProfileName))
        != QMessageBox::Yes) {
        return;
    }
    KeyMapProfileStore::deleteProfile(m_interfaceId, m_currentProfileName);
    m_currentProfileName.clear();
    clearMarkers();
    reloadProfileList();
    emit profilesChanged(m_interfaceId);
}

void GameControlsEditor::clearMarkers()
{
    for (const QPointer<GameControlMarker> &marker : m_markers) {
        if (marker) {
            marker->deleteLater();
        }
    }
    m_markers.clear();
}

void GameControlsEditor::addMarkerForNode(const ControlNode &node)
{
    if (!m_videoForm) {
        return;
    }
    QWidget *surface = m_videoForm->gameControlsSurface();
    if (!surface) {
        return;
    }
    auto *marker = new GameControlMarker(node, surface);
    marker->show();
    marker->raise(); // stay clickable/draggable above the EditModeOverlay click-shield
    connect(marker, &GameControlMarker::editRequested, this, &GameControlsEditor::editMarker);
    connect(marker, &GameControlMarker::removeRequested, this, &GameControlsEditor::removeMarker);
    connect(marker, &GameControlMarker::moved, this, [this](GameControlMarker *) { setDirty(true); });
    m_markers.push_back(marker);
    marker->relayout(surface->size());
}

void GameControlsEditor::relayoutMarkers()
{
    if (!m_videoForm) {
        return;
    }
    QWidget *surface = m_videoForm->gameControlsSurface();
    if (!surface) {
        return;
    }
    for (const QPointer<GameControlMarker> &marker : m_markers) {
        if (marker) {
            marker->relayout(surface->size());
        }
    }
}

bool GameControlsEditor::hasLookNode(GameControlMarker *exclude) const
{
    for (const QPointer<GameControlMarker> &marker : m_markers) {
        if (!marker || marker == exclude) {
            continue;
        }
        if (marker->node().action == ControlActionKind::FreeLook || marker->node().action == ControlActionKind::AimPanShoot) {
            return true;
        }
    }
    return false;
}

void GameControlsEditor::handleControlDropped(ControlActionKind kind, QPointF normPos)
{
    if ((kind == ControlActionKind::FreeLook || kind == ControlActionKind::AimPanShoot) && hasLookNode()) {
        QMessageBox::information(
            this, tr("Controls editor"),
            tr("Only one Free look / Aim, pan and shoot control is supported per scheme. Edit or remove the existing one first."));
        return;
    }

    ControlNode node;
    node.action = kind;
    node.pos = normPos;
    node.endPos = QPointF(qBound(0.0, normPos.x() + 0.15, 1.0), normPos.y());
    if (kind == ControlActionKind::AimPanShoot) {
        node.key = QStringLiteral("LeftButton");
    }

    ControlInspectorDialog dialog(node, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    addMarkerForNode(dialog.result());
    setDirty(true);
}

void GameControlsEditor::editMarker(GameControlMarker *marker)
{
    if (!marker) {
        return;
    }
    ControlInspectorDialog dialog(marker->node(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    marker->setNode(dialog.result());
    setDirty(true);
}

void GameControlsEditor::removeMarker(GameControlMarker *marker)
{
    if (!marker) {
        return;
    }
    m_markers.removeAll(QPointer<GameControlMarker>(marker));
    marker->deleteLater();
    setDirty(true);
}

void GameControlsEditor::applyLive()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    QVector<ControlNode> nodes;
    for (const QPointer<GameControlMarker> &marker : m_markers) {
        if (marker) {
            nodes.push_back(marker->node());
        }
    }
    device->updateScript(KeyMapProfileStore::toJson(m_switchKeyCapture->boundKeyString(), m_cursorLockKeyCapture->boundKeyString(), nodes));
}
