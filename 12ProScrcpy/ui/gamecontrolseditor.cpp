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
#include "gamecontrolmarker.h"
#include "gamecontrolseditor.h"
#include "videoform.h"
#include "../QtScrcpyCore/include/QtScrcpyCore.h"

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
            m_pressPos = event->pos();
        }
        QToolButton::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if ((event->buttons() & Qt::LeftButton) && (event->pos() - m_pressPos).manhattanLength() > 12) {
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

GameControlsEditor::GameControlsEditor(const QString &serial, QWidget *parent) : QWidget(parent), m_serial(serial)
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

void GameControlsEditor::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    relayoutMarkers();
}

bool GameControlsEditor::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Resize && m_videoForm && watched == m_videoForm->gameControlsSurface()) {
        relayoutMarkers();
    }
    return QWidget::eventFilter(watched, event);
}

void GameControlsEditor::reloadProfileList(const QString &selectName)
{
    const QStringList profiles = KeyMapProfileStore::listProfiles();
    m_profileCombo->blockSignals(true);
    m_profileCombo->clear();
    m_profileCombo->addItem(tr("(unsaved scheme)"));
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
        return;
    }

    const QString name = m_profileCombo->itemText(index);
    QString switchKey;
    QVector<ControlNode> nodes;
    QString error;
    if (!KeyMapProfileStore::loadProfile(name, switchKey, nodes, &error)) {
        QMessageBox::warning(this, tr("Controls editor"), tr("Could not load profile: %1").arg(error));
        return;
    }

    m_currentProfileName = name;
    m_switchKeyCapture->setBoundKeyString(switchKey);
    for (const ControlNode &node : nodes) {
        addMarkerForNode(node);
    }
    relayoutMarkers();
}

void GameControlsEditor::onNewProfile()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New control scheme"), tr("Name:"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    if (KeyMapProfileStore::profileExists(name)) {
        QMessageBox::warning(this, tr("Controls editor"), tr("A profile named \"%1\" already exists.").arg(name));
        return;
    }

    clearMarkers();
    m_currentProfileName = name;
    m_switchKeyCapture->setBoundKeyString(KeyMapProfileStore::defaultSwitchKeyString());

    QVector<ControlNode> empty;
    QString error;
    if (!KeyMapProfileStore::saveProfile(name, m_switchKeyCapture->boundKeyString(), empty, &error)) {
        QMessageBox::warning(this, tr("Controls editor"), tr("Could not create profile: %1").arg(error));
        return;
    }
    reloadProfileList(name);
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
    if (!KeyMapProfileStore::saveProfile(name, m_switchKeyCapture->boundKeyString(), nodes, &error)) {
        QMessageBox::warning(this, tr("Controls editor"), tr("Could not save profile: %1").arg(error));
        return;
    }

    m_currentProfileName = name;
    reloadProfileList(name);
    applyLive();
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
    KeyMapProfileStore::deleteProfile(m_currentProfileName);
    m_currentProfileName.clear();
    clearMarkers();
    reloadProfileList();
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
    connect(marker, &GameControlMarker::editRequested, this, &GameControlsEditor::editMarker);
    connect(marker, &GameControlMarker::removeRequested, this, &GameControlsEditor::removeMarker);
    connect(marker, &GameControlMarker::moved, this, [this](GameControlMarker *) { m_dirty = true; });
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
    m_dirty = true;
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
    m_dirty = true;
}

void GameControlsEditor::removeMarker(GameControlMarker *marker)
{
    if (!marker) {
        return;
    }
    m_markers.removeAll(QPointer<GameControlMarker>(marker));
    marker->deleteLater();
    m_dirty = true;
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
    device->updateScript(KeyMapProfileStore::toJson(m_switchKeyCapture->boundKeyString(), nodes));
    m_dirty = false;
}
