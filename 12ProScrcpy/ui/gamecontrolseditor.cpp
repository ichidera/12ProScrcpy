#include <QComboBox>
#include <QDrag>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "controlinspectordialog.h"
#include "editmodeoverlay.h"
#include "gamecontrolmarker.h"
#include "gamecontrolseditor.h"
#include "iconhelper.h"
#include "videoform.h"
#include "../12ProScrcpyCore/include/QtScrcpyCore.h"

namespace
{
// FontAwesome 4 glyphs (same bundled icon font IconHelper registers).
// Approximations picked for recognizability, not a 1:1 asset match - swap
// freely if real artwork shows up later.
constexpr ushort kIconHelp = 0xf059;
constexpr ushort kIconClose = 0xf00d;
constexpr ushort kIconCloud = 0xf0c2;
constexpr ushort kIconImport = 0xf019; // download
constexpr ushort kIconExport = 0xf093; // upload
constexpr ushort kIconFolder = 0xf07b;
constexpr ushort kIconRename = 0xf044; // pencil
constexpr ushort kIconDuplicate = 0xf0c5; // copy
constexpr ushort kIconDelete = 0xf1f8; // trash

constexpr ushort kIconTapSpot = 0xf192;
constexpr ushort kIconRepeatedTap = 0xf01e;
constexpr ushort kIconDPad = 0xf11b;
constexpr ushort kIconAimPanShoot = 0xf05b;
constexpr ushort kIconFreeLook = 0xf06e;
constexpr ushort kIconSwipe = 0xf25a;
constexpr ushort kIconScript = 0xf121;
constexpr ushort kIconZoom = 0xf00e;
constexpr ushort kIconTilt = 0xf074;
constexpr ushort kIconMobaDPad = 0xf047;
constexpr ushort kIconMobaSkillPad = 0xf10c;
constexpr ushort kIconRotate = 0xf021;
constexpr ushort kIconEdgeScroll = 0xf0b2;
constexpr ushort kIconScroll = 0xf07d;

// Renders one icon-font glyph as a real QIcon/QPixmap. Needed for
// PaletteButton, which shows an icon *and* a normal-font text label below it
// on the same QToolButton - QAbstractButton only has one font/text pair, so
// IconHelper::SetIcon() (which repoints both to the icon font) would garble
// the label. Buttons that show *only* a glyph (no separate text) can still
// use IconHelper::SetIcon() directly - see makeGlyphButton().
QIcon glyphIcon(QChar glyph, int pointSize, const QColor &color)
{
    const QFont font = IconHelper::Instance()->font(pointSize);
    const QFontMetrics fm(font);
    const QRect bounds = fm.boundingRect(glyph);
    const int side = qMax(qMax(bounds.width(), bounds.height()) + 6, pointSize + 6);

    QPixmap pixmap(side, side);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(pixmap.rect(), Qt::AlignCenter, glyph);
    return QIcon(pixmap);
}

// A palette entry the user drags onto the mirrored screen to place a new
// control. Left-click-and-drag starts a QDrag carrying the ControlActionKind.
// Entries not yet implemented on the engine side (comingSoon = true) render
// faded and refuse the drag entirely - they exist purely to show what's on
// the roadmap, matching the reference "Coming soon" section.
class PaletteButton : public QToolButton
{
public:
    PaletteButton(ControlActionKind kind, QChar glyph, bool comingSoon, QWidget *parent)
        : QToolButton(parent), m_kind(kind), m_comingSoon(comingSoon)
    {
        const QColor iconColor = comingSoon ? QColor("#4b5563") : QColor("#d7dbe0");
        setIcon(glyphIcon(glyph, 18, iconColor));
        setIconSize(QSize(22, 22));
        setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        setText(KeyMapProfileStore::actionLabel(kind));
        setMinimumSize(74, 60);
        setCursor(comingSoon ? Qt::ArrowCursor : Qt::OpenHandCursor);
        setEnabled(!comingSoon);
        setToolTip(comingSoon ? QObject::tr("Coming soon") : KeyMapProfileStore::actionLabel(kind));
        setStyleSheet(comingSoon ? "QToolButton { background:#1a1d23; border:1px solid #242832; border-radius:8px; color:#4b5563; }"
                                  : "QToolButton { background:#1d2127; border:1px solid #2f3541; border-radius:8px; color:#d7dbe0; }"
                                    "QToolButton:hover { border-color:#22c55e; }");
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (!m_comingSoon && event->button() == Qt::LeftButton) {
            m_pressPos = event->position().toPoint();
        }
        QToolButton::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_comingSoon && (event->buttons() & Qt::LeftButton) && (event->position().toPoint() - m_pressPos).manhattanLength() > 12) {
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
    bool m_comingSoon;
    QPoint m_pressPos;
};
} // namespace

GameControlsEditor::GameControlsEditor(const QString &serial, QWidget *parent)
    : QWidget(parent), m_serial(serial), m_interfaceId(KeyMapProfileStore::unknownInterfaceId())
{
    // Deliberately NOT a floating top-level window: this is always embedded
    // as a page inside GameControlsPanel's stacked widget, so it opens "in
    // place of" the quick-settings view instead of popping up as a second
    // box elsewhere on screen.
    buildUi();
    reloadProfileList();
}

QToolButton *GameControlsEditor::makeGlyphButton(QChar glyph, const QString &tooltip, bool enabled, QWidget *parent)
{
    auto *btn = new QToolButton(parent);
    IconHelper::Instance()->SetIcon(btn, glyph, 12);
    btn->setToolTip(tooltip);
    btn->setEnabled(enabled);
    btn->setCursor(enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
    btn->setAutoRaise(true);
    btn->setStyleSheet(enabled ? "QToolButton { color:#c8cdd4; border:none; background:transparent; padding:3px; }"
                                  "QToolButton:hover { color:#22c55e; }"
                                : "QToolButton { color:#454b56; border:none; background:transparent; padding:3px; }");
    return btn;
}

void GameControlsEditor::buildUi()
{
    setWindowTitle(tr("Controls editor"));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *card = new QWidget(this);
    card->setObjectName("ceCard");
    card->setAttribute(Qt::WA_StyledBackground, true);
    outer->addWidget(card);

    setStyleSheet(
        "QWidget#ceCard { background:#16181d; border:1px solid #262b33; border-radius:10px; }"
        "QLabel#ceTitle { color:#f2f3f5; font-weight:600; font-size:13px; }"
        "QLabel#ceSectionLabel { color:#d7dbe0; font-size:12px; font-weight:600; }"
        "QLabel#ceHint { color:#7d8590; font-size:10px; }"
        "QLabel#ceComingSoonLabel { color:#5b6270; font-size:10px; font-weight:600; }"
        "QComboBox { background:#1d2127; border:1px solid #2f3541; border-radius:6px; padding:5px 8px; color:#e6e8eb; }"
        "QPushButton#ceCreateProfile { color:#22c55e; border:none; background:transparent; text-align:left; font-size:11px; }"
        "QPushButton#ceCreateProfile:hover { color:#2ed474; text-decoration: underline; }"
        "QPushButton#ceReset { background:#3a1d22; color:#f87171; border:none; border-radius:8px; padding:8px; font-weight:600; }"
        "QPushButton#ceReset:hover { background:#4a2328; }"
        "QPushButton#ceSave { background:#22c55e; color:#0b1210; border:none; border-radius:8px; padding:8px; font-weight:600; }"
        "QPushButton#ceSave:hover { background:#2ed474; }"
        "QPushButton#ceSave:disabled { background:#1c3a2a; color:#4b6357; }"
        "QLabel#ceKeyRowLabel { color:#9aa3af; font-size:11px; }");

    auto *root = new QVBoxLayout(card);
    root->setContentsMargins(14, 12, 14, 14);
    root->setSpacing(12);

    // --- header: accent bar + title + help + close ---
    auto *header = new QHBoxLayout;
    header->setSpacing(8);
    auto *accent = new QFrame(card);
    accent->setFixedSize(3, 15);
    accent->setStyleSheet("background:#22c55e; border-radius:1px;");
    header->addWidget(accent);
    m_titleLabel = new QLabel(tr("Controls editor"), card);
    m_titleLabel->setObjectName("ceTitle");
    header->addWidget(m_titleLabel);
    auto *help = makeGlyphButton(QChar(kIconHelp), tr("Drag any action below onto your game screen to place it, then bind a key by "
                                                        "clicking the marker. \"%1\" toggles the whole scheme on/off, \"%2\" "
                                                        "locks/hides the cursor for aiming.")
                                                         .arg(KeyMapProfileStore::defaultSwitchKeyString(),
                                                              KeyMapProfileStore::defaultCursorLockKeyString()),
                                  true, card);
    header->addWidget(help);
    header->addStretch(1);
    auto *closeBtn = makeGlyphButton(QChar(kIconClose), tr("Back to Game controls"), true, card);
    connect(closeBtn, &QToolButton::clicked, this, &GameControlsEditor::closeRequested);
    header->addWidget(closeBtn);
    root->addLayout(header);

    root->addWidget(buildControlSchemeSection());

    // --- touch-mode toggle / cursor lock keys ---
    auto *switchRow = new QWidget(card);
    auto *switchLayout = new QHBoxLayout(switchRow);
    switchLayout->setContentsMargins(0, 0, 0, 0);
    auto *switchLabel = new QLabel(tr("Touch-mode toggle"), switchRow);
    switchLabel->setObjectName("ceKeyRowLabel");
    switchLayout->addWidget(switchLabel);
    switchLayout->addStretch(1);
    m_switchKeyCapture = new KeyCaptureButton(switchRow);
    m_switchKeyCapture->setBoundKeyString(KeyMapProfileStore::defaultSwitchKeyString());
    switchLayout->addWidget(m_switchKeyCapture);
    root->addWidget(switchRow);
    connect(m_switchKeyCapture, &KeyCaptureButton::keyChanged, this, [this](const QString &) { setDirty(true); });

    // Deliberately a *separate* key from the touch-mode toggle above: this
    // one only grabs+hides the cursor (BlueStacks' "enter/exit shooting
    // mode"), so `` ` `` can stay a plain focus switch while a dedicated key
    // (F1 by default) is the one that locks the mouse for aiming.
    auto *lockRow = new QWidget(card);
    auto *lockLayout = new QHBoxLayout(lockRow);
    lockLayout->setContentsMargins(0, 0, 0, 0);
    auto *lockLabel = new QLabel(tr("Lock/hide cursor"), lockRow);
    lockLabel->setObjectName("ceKeyRowLabel");
    lockLayout->addWidget(lockLabel);
    lockLayout->addStretch(1);
    m_cursorLockKeyCapture = new KeyCaptureButton(lockRow);
    m_cursorLockKeyCapture->setBoundKeyString(KeyMapProfileStore::defaultCursorLockKeyString());
    lockLayout->addWidget(m_cursorLockKeyCapture);
    root->addWidget(lockRow);
    connect(m_cursorLockKeyCapture, &KeyCaptureButton::keyChanged, this, [this](const QString &) { setDirty(true); });

    root->addWidget(buildPaletteSection());

    // --- bottom bar: reset / save ---
    auto *actionsLabel = new QLabel(tr("Current configuration actions"), card);
    actionsLabel->setObjectName("ceHint");
    actionsLabel->setAlignment(Qt::AlignCenter);
    root->addWidget(actionsLabel);

    auto *actionsRow = new QHBoxLayout;
    auto *resetBtn = new QPushButton(tr("Reset"), card);
    resetBtn->setObjectName("ceReset");
    resetBtn->setCursor(Qt::PointingHandCursor);
    auto *saveBtn = new QPushButton(tr("Save"), card);
    saveBtn->setObjectName("ceSave");
    saveBtn->setCursor(Qt::PointingHandCursor);
    actionsRow->addWidget(resetBtn);
    actionsRow->addWidget(saveBtn);
    root->addLayout(actionsRow);
    connect(resetBtn, &QPushButton::clicked, this, &GameControlsEditor::onCancelEdits);
    connect(saveBtn, &QPushButton::clicked, this, &GameControlsEditor::onSaveProfile);
}

QWidget *GameControlsEditor::buildControlSchemeSection()
{
    auto *section = new QWidget(this);
    auto *layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto *labelRow = new QHBoxLayout;
    auto *label = new QLabel(tr("Control scheme"), section);
    label->setObjectName("ceSectionLabel");
    labelRow->addWidget(label);
    labelRow->addStretch(1);
    // Cloud sync / import / export / browse-profiles: none of these are
    // implemented yet, shown (faded, inert) so the layout already has
    // somewhere for them to land later instead of shifting everything when
    // they do.
    labelRow->addWidget(makeGlyphButton(QChar(kIconCloud), tr("Cloud sync — coming soon"), false, section));
    labelRow->addWidget(makeGlyphButton(QChar(kIconImport), tr("Import — coming soon"), false, section));
    labelRow->addWidget(makeGlyphButton(QChar(kIconExport), tr("Export — coming soon"), false, section));
    labelRow->addWidget(makeGlyphButton(QChar(kIconFolder), tr("Browse profiles — coming soon"), false, section));
    layout->addLayout(labelRow);

    auto *comboRow = new QHBoxLayout;
    comboRow->setSpacing(4);
    m_profileCombo = new QComboBox(section);
    m_profileCombo->setEditable(false);
    connect(m_profileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &GameControlsEditor::onProfileChanged);
    comboRow->addWidget(m_profileCombo, 1);
    auto *renameBtn = makeGlyphButton(QChar(kIconRename), tr("Rename"), true, section);
    auto *duplicateBtn = makeGlyphButton(QChar(kIconDuplicate), tr("Duplicate"), true, section);
    auto *deleteBtn = makeGlyphButton(QChar(kIconDelete), tr("Delete"), true, section);
    comboRow->addWidget(renameBtn);
    comboRow->addWidget(duplicateBtn);
    comboRow->addWidget(deleteBtn);
    connect(renameBtn, &QToolButton::clicked, this, &GameControlsEditor::onRenameProfile);
    connect(duplicateBtn, &QToolButton::clicked, this, &GameControlsEditor::onDuplicateProfile);
    connect(deleteBtn, &QToolButton::clicked, this, &GameControlsEditor::onDeleteProfile);
    layout->addLayout(comboRow);

    auto *createBtn = new QPushButton(tr("+  Create new profile"), section);
    createBtn->setObjectName("ceCreateProfile");
    createBtn->setCursor(Qt::PointingHandCursor);
    connect(createBtn, &QPushButton::clicked, this, &GameControlsEditor::onNewProfile);
    layout->addWidget(createBtn);

    return section;
}

QWidget *GameControlsEditor::buildPaletteSection()
{
    auto *section = new QWidget(this);
    auto *layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto *addLabel = new QLabel(tr("Add controls"), section);
    addLabel->setObjectName("ceSectionLabel");
    layout->addWidget(addLabel);
    auto *hint = new QLabel(
        tr("Drag and drop an action onto your game screen to assign a key to it. Click \"?\" above to learn how the editor works."),
        section);
    hint->setObjectName("ceHint");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    // Implemented actions first, full brightness, draggable.
    auto *grid = new QGridLayout;
    grid->setSpacing(8);
    struct Entry
    {
        ControlActionKind kind;
        QChar glyph;
    };
    const Entry implemented[] = { { ControlActionKind::TapSpot, QChar(kIconTapSpot) },
                                   { ControlActionKind::RepeatedTap, QChar(kIconRepeatedTap) },
                                   { ControlActionKind::DPad, QChar(kIconDPad) },
                                   { ControlActionKind::AimPanShoot, QChar(kIconAimPanShoot) },
                                   { ControlActionKind::FreeLook, QChar(kIconFreeLook) },
                                   { ControlActionKind::DragSwipe, QChar(kIconSwipe) } };
    for (int i = 0; i < 6; ++i) {
        grid->addWidget(new PaletteButton(implemented[i].kind, implemented[i].glyph, false, section), i / 3, i % 3);
    }
    layout->addLayout(grid);

    auto *comingSoonLabel = new QLabel(tr("COMING SOON"), section);
    comingSoonLabel->setObjectName("ceComingSoonLabel");
    layout->addWidget(comingSoonLabel);

    // Not-yet-implemented actions: faded, non-draggable, tooltip explains
    // why. Kept in the same grid layout/shape as the implemented ones so
    // the whole palette reads as one coherent list rather than two
    // differently-styled panels.
    auto *comingSoonGrid = new QGridLayout;
    comingSoonGrid->setSpacing(8);
    struct ComingSoonEntry
    {
        ControlActionKind placeholderKind; // not actually usable - comingSoon=true blocks the drag regardless
        QChar glyph;
        QString label;
    };
    const ComingSoonEntry comingSoon[] = {
        { ControlActionKind::TapSpot, QChar(kIconScript), tr("Script") },
        { ControlActionKind::TapSpot, QChar(kIconZoom), tr("Zoom") },
        { ControlActionKind::TapSpot, QChar(kIconTilt), tr("Tilt") },
        { ControlActionKind::TapSpot, QChar(kIconMobaDPad), tr("MOBA D-Pad") },
        { ControlActionKind::TapSpot, QChar(kIconMobaSkillPad), tr("MOBA Skill pad") },
        { ControlActionKind::TapSpot, QChar(kIconRotate), tr("Rotate") },
        { ControlActionKind::TapSpot, QChar(kIconEdgeScroll), tr("Edge scroll") },
        { ControlActionKind::TapSpot, QChar(kIconScroll), tr("Scroll") },
    };
    for (int i = 0; i < 8; ++i) {
        auto *btn = new PaletteButton(comingSoon[i].placeholderKind, comingSoon[i].glyph, true, section);
        btn->setText(comingSoon[i].label);
        comingSoonGrid->addWidget(btn, i / 3, i % 3);
    }
    layout->addLayout(comingSoonGrid);

    return section;
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
    const QString title = displayName.isEmpty() ? tr("Controls editor") : tr("Controls editor — %1").arg(displayName);
    if (m_interfaceId == normalizedId) {
        m_interfaceLabel = displayName;
        setWindowTitle(title);
        if (m_titleLabel) {
            m_titleLabel->setText(title);
        }
        return;
    }
    m_interfaceId = normalizedId;
    m_interfaceLabel = displayName;
    setWindowTitle(title);
    if (m_titleLabel) {
        m_titleLabel->setText(title);
    }
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
    // Save/Reset now live on this panel's own header/footer buttons, not on
    // the overlay - it only still needs to relay palette drops.
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

void GameControlsEditor::onRenameProfile()
{
    if (m_currentProfileName.isEmpty()) {
        return;
    }
    bool ok = false;
    const QString newName =
        QInputDialog::getText(this, tr("Rename control scheme"), tr("Name:"), QLineEdit::Normal, m_currentProfileName, &ok).trimmed();
    if (!ok || newName.isEmpty() || newName == m_currentProfileName) {
        return;
    }
    if (KeyMapProfileStore::profileExists(m_interfaceId, newName)) {
        QMessageBox::warning(this, tr("Controls editor"), tr("A profile named \"%1\" already exists.").arg(newName));
        return;
    }

    QVector<ControlNode> nodes;
    for (const QPointer<GameControlMarker> &marker : m_markers) {
        if (marker) {
            nodes.push_back(marker->node());
        }
    }
    QString error;
    if (!KeyMapProfileStore::saveProfile(m_interfaceId, newName, m_switchKeyCapture->boundKeyString(),
                                          m_cursorLockKeyCapture->boundKeyString(), nodes, &error)) {
        QMessageBox::warning(this, tr("Controls editor"), tr("Could not rename profile: %1").arg(error));
        return;
    }
    KeyMapProfileStore::deleteProfile(m_interfaceId, m_currentProfileName);
    m_currentProfileName = newName;
    reloadProfileList(newName);
    emit profilesChanged(m_interfaceId);
}

void GameControlsEditor::onDuplicateProfile()
{
    if (m_currentProfileName.isEmpty()) {
        return;
    }
    bool ok = false;
    const QString newName = QInputDialog::getText(this, tr("Duplicate control scheme"), tr("Name:"), QLineEdit::Normal,
                                                    tr("%1 copy").arg(m_currentProfileName), &ok)
                                 .trimmed();
    if (!ok || newName.isEmpty()) {
        return;
    }
    if (KeyMapProfileStore::profileExists(m_interfaceId, newName)) {
        QMessageBox::warning(this, tr("Controls editor"), tr("A profile named \"%1\" already exists.").arg(newName));
        return;
    }

    QVector<ControlNode> nodes;
    for (const QPointer<GameControlMarker> &marker : m_markers) {
        if (marker) {
            nodes.push_back(marker->node());
        }
    }
    QString error;
    if (!KeyMapProfileStore::saveProfile(m_interfaceId, newName, m_switchKeyCapture->boundKeyString(),
                                          m_cursorLockKeyCapture->boundKeyString(), nodes, &error)) {
        QMessageBox::warning(this, tr("Controls editor"), tr("Could not duplicate profile: %1").arg(error));
        return;
    }
    // Original profile is untouched on disk under its old name; just switch
    // over to editing the new copy.
    m_currentProfileName = newName;
    reloadProfileList(newName);
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

QStringList GameControlsEditor::collectOtherKeys(const GameControlMarker *exclude) const
{
    QStringList keys;
    auto addIfBound = [&keys](const QString &k) {
        if (!k.isEmpty()) {
            keys << k;
        }
    };
    for (const QPointer<GameControlMarker> &marker : m_markers) {
        if (!marker || marker.data() == exclude) {
            continue;
        }
        const ControlNode &n = marker->node();
        addIfBound(n.key);
        addIfBound(n.upKey);
        addIfBound(n.downKey);
        addIfBound(n.leftKey);
        addIfBound(n.rightKey);
        addIfBound(n.smallEyesKey);
        addIfBound(n.suspendKey);
    }
    return keys;
}

QString GameControlsEditor::validateNodeKeys(const ControlNode &node, const QStringList &otherKeys) const
{
    struct Slot
    {
        QString value;
        QString label;
    };
    QVector<Slot> slots;
    auto add = [&slots](const QString &v, const QString &label) {
        if (!v.isEmpty()) {
            slots.push_back({ v, label });
        }
    };
    switch (node.action) {
    case ControlActionKind::TapSpot:
    case ControlActionKind::RepeatedTap:
    case ControlActionKind::DragSwipe:
        add(node.key, tr("Key / mouse button"));
        break;
    case ControlActionKind::DPad:
        add(node.upKey, tr("Up key"));
        add(node.downKey, tr("Down key"));
        add(node.leftKey, tr("Left key"));
        add(node.rightKey, tr("Right key"));
        break;
    case ControlActionKind::FreeLook:
    case ControlActionKind::AimPanShoot:
        add(node.key, tr("Shoot button"));
        add(node.smallEyesKey, tr("Precision-aim toggle"));
        add(node.suspendKey, tr("Suspend shoot-mode"));
        break;
    }

    // A node can't use the same key for two of its own slots (e.g. DPad's
    // up/down, or AimPanShoot's shoot button and its own suspend key) -
    // each would be ambiguous about which one you meant.
    for (int i = 0; i < slots.size(); ++i) {
        for (int j = i + 1; j < slots.size(); ++j) {
            if (slots[i].value == slots[j].value) {
                return tr("%1 and %2 can't share the same key (%3).").arg(slots[i].label, slots[j].label, slots[i].value);
            }
        }
    }

    // Neither the touch-mode toggle nor the shoot-mode (cursor lock) key
    // can double as a control binding - both are already spoken for.
    const QString switchKey = m_switchKeyCapture ? m_switchKeyCapture->boundKeyString() : QString();
    const QString cursorLockKey = m_cursorLockKeyCapture ? m_cursorLockKeyCapture->boundKeyString() : QString();
    for (const Slot &slot : slots) {
        if (!switchKey.isEmpty() && slot.value == switchKey) {
            return tr("%1 can't use %2 - that's the touch-mode toggle key. Pick a different key, or change the toggle key first.")
                .arg(slot.label, slot.value);
        }
        if (!cursorLockKey.isEmpty() && slot.value == cursorLockKey) {
            return tr("%1 can't use %2 - that's the shoot-mode (lock/hide cursor) key. Pick a different key, or change that key first.")
                .arg(slot.label, slot.value);
        }
    }

    // Keyboard keys are free to repeat across different controls (BlueStacks/
    // MEmu/LDPlayer all allow this too - e.g. binding both a movement key
    // and an ability to the same letter is normal since they're never
    // ambiguous in practice). Mouse buttons are different: a single
    // physical click can only mean one thing, so each button - Left, Right,
    // Middle, whichever - is capped at one binding for the whole scheme.
    // This still allows e.g. Left = fire and Right = aim at the same time
    // (each button individually unique), matching how LDPlayer's Call of
    // Duty: Mobile preset binds them.
    QHash<QString, int> mouseButtonUseCount;
    auto tallyIfMouse = [&mouseButtonUseCount](const QString &v) {
        bool isMouse = false;
        if (KeyMapProfileStore::stringToKey(v, nullptr, &isMouse) && isMouse) {
            ++mouseButtonUseCount[v];
        }
    };
    for (const QString &k : otherKeys) {
        tallyIfMouse(k);
    }
    for (const Slot &slot : slots) {
        tallyIfMouse(slot.value);
    }
    for (auto it = mouseButtonUseCount.constBegin(); it != mouseButtonUseCount.constEnd(); ++it) {
        if (it.value() > 1) {
            return tr("%1 is already bound to another control in this scheme. Each mouse button can only be bound to one control "
                      "at a time.")
                .arg(it.key());
        }
    }

    return QString();
}

bool GameControlsEditor::captureValidNode(ControlNode node, const GameControlMarker *exclude, ControlNode &outNode)
{
    const QStringList otherKeys = collectOtherKeys(exclude);
    while (true) {
        ControlInspectorDialog dialog(node, this);
        if (dialog.exec() != QDialog::Accepted) {
            return false;
        }
        ControlNode candidate = dialog.result();
        const QString error = validateNodeKeys(candidate, otherKeys);
        if (error.isEmpty()) {
            outNode = candidate;
            return true;
        }
        QMessageBox::warning(this, tr("Invalid key binding"), error);
        node = candidate; // keep every other field they set, only the key needs fixing
    }
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

    ControlNode result;
    if (!captureValidNode(node, nullptr, result)) {
        return;
    }
    addMarkerForNode(result);
    setDirty(true);
}

void GameControlsEditor::editMarker(GameControlMarker *marker)
{
    if (!marker) {
        return;
    }
    ControlNode result;
    if (!captureValidNode(marker->node(), marker, result)) {
        return;
    }
    marker->setNode(result);
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
