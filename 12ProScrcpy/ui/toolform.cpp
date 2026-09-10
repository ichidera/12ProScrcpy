#include <QDebug>
#include <QApplication>
#include <QCoreApplication>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMessageBox>
#include <QHideEvent>
#include <QMouseEvent>
#include <QShowEvent>

#include "iconhelper.h"
#include "toolform.h"
#include "ui_toolform.h"
#include "videoform.h"
#include "../groupcontroller/groupcontroller.h"

ToolForm::ToolForm(QWidget *adsorbWidget, AdsorbPositions adsorbPos) : MagneticWidget(adsorbWidget, adsorbPos), ui(new Ui::ToolForm)
{
    ui->setupUi(this);
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    //setWindowFlags(windowFlags() & ~Qt::WindowMinMaxButtonsHint);

    updateGroupControl();

    initStyle();
}

ToolForm::~ToolForm()
{
    delete ui;
}

void ToolForm::setSerial(const QString &serial)
{
    m_serial = serial;
    updateCameraMode();
}

bool ToolForm::isHost()
{
    return m_isHost;
}

void ToolForm::updateCameraMode()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    const bool camera = device && device->isCameraMode();

    ui->groupControlBtn->setVisible(!camera);
    ui->expandNotifyBtn->setVisible(!camera);
    ui->expandSettingsBtn->setVisible(!camera);
    ui->rotateBtn->setVisible(!camera);
    ui->touchBtn->setVisible(!camera);
    ui->openScreenBtn->setVisible(!camera);
    ui->closeScreenBtn->setVisible(!camera);
    ui->powerBtn->setVisible(!camera);
    ui->volumeUpBtn->setVisible(!camera);
    ui->volumeDownBtn->setVisible(!camera);
    ui->appSwitchBtn->setVisible(!camera);
    ui->menuBtn->setVisible(!camera);
    ui->homeBtn->setVisible(!camera);
    ui->returnBtn->setVisible(!camera);
    ui->clipboardBtn->setVisible(!camera);
    ui->cameraTorchBtn->setVisible(camera);
    ui->cameraZoomOutBtn->setVisible(camera);
    ui->cameraZoomInBtn->setVisible(camera);
}

void ToolForm::initStyle()
{
    IconHelper::Instance()->SetIcon(ui->fullScreenBtn, QChar(0xf0b2), 15);
    IconHelper::Instance()->SetIcon(ui->menuBtn, QChar(0xf096), 15);
    IconHelper::Instance()->SetIcon(ui->homeBtn, QChar(0xf1db), 15);
    //IconHelper::Instance()->SetIcon(ui->returnBtn, QChar(0xf104), 15);
    IconHelper::Instance()->SetIcon(ui->returnBtn, QChar(0xf053), 15);
    IconHelper::Instance()->SetIcon(ui->appSwitchBtn, QChar(0xf24d), 15);
    IconHelper::Instance()->SetIcon(ui->volumeUpBtn, QChar(0xf028), 15);
    IconHelper::Instance()->SetIcon(ui->volumeDownBtn, QChar(0xf027), 15);
    IconHelper::Instance()->SetIcon(ui->openScreenBtn, QChar(0xf06e), 15);
    IconHelper::Instance()->SetIcon(ui->closeScreenBtn, QChar(0xf070), 15);
    IconHelper::Instance()->SetIcon(ui->powerBtn, QChar(0xf011), 15);
    IconHelper::Instance()->SetIcon(ui->expandNotifyBtn, QChar(0xf103), 15);
    IconHelper::Instance()->SetIcon(ui->expandSettingsBtn, QChar(0xf013), 15);
    IconHelper::Instance()->SetIcon(ui->rotateBtn, QChar(0xf021), 15);
    IconHelper::Instance()->SetIcon(ui->screenShotBtn, QChar(0xf0c4), 15);
    // cursor-lock button: lock icon (0xf023 = fa-lock), tooltip shows current key
    IconHelper::Instance()->SetIcon(ui->cursorLockBtn, QChar(0xf023), 15);
    {
        VideoForm *vf = qobject_cast<VideoForm*>(parent());
        if (vf) {
            ui->cursorLockBtn->setToolTip(
                QString("cursor lock (%1) — Ctrl+click to change")
                .arg(QKeySequence(vf->cursorLockKey()).toString()));
        }
    }
    IconHelper::Instance()->SetIcon(ui->touchBtn, QChar(0xf111), 15);
    IconHelper::Instance()->SetIcon(ui->groupControlBtn, QChar(0xf0c0), 15);
    IconHelper::Instance()->SetIcon(ui->clipboardBtn, QChar(0xf0c5), 15);
    IconHelper::Instance()->SetIcon(ui->cameraTorchBtn, QChar(0xf0eb), 15);
    IconHelper::Instance()->SetIcon(ui->cameraZoomOutBtn, QChar(0xf010), 15);
    IconHelper::Instance()->SetIcon(ui->cameraZoomInBtn, QChar(0xf00e), 15);
}

void ToolForm::updateGroupControl()
{
    if (m_isHost) {
        ui->groupControlBtn->setStyleSheet("color: red");
    } else {
        ui->groupControlBtn->setStyleSheet("color: green");
    }

    GroupController::instance().updateDeviceState(m_serial);
}

void ToolForm::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        m_dragPosition = event->globalPos() - frameGeometry().topLeft();
#else
        m_dragPosition = event->globalPosition().toPoint() - frameGeometry().topLeft();
#endif
        event->accept();
    }
}

void ToolForm::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event)
}

void ToolForm::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton) {
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        move(event->globalPos() - m_dragPosition);
#else
        move(event->globalPosition().toPoint() - m_dragPosition);
#endif
        event->accept();
    }
}

void ToolForm::showEvent(QShowEvent *event)
{
    Q_UNUSED(event)
    qDebug() << "show event";
}

void ToolForm::hideEvent(QHideEvent *event)
{
    Q_UNUSED(event)
    qDebug() << "hide event";
}

void ToolForm::on_fullScreenBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }

    dynamic_cast<VideoForm*>(parent())->switchFullScreen();
}

void ToolForm::on_returnBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->postGoBack();
}

void ToolForm::on_homeBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->postGoHome();
}

void ToolForm::on_menuBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->postGoMenu();
}

void ToolForm::on_appSwitchBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->postAppSwitch();
}

void ToolForm::on_powerBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->postPower();
}

void ToolForm::on_screenShotBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->screenshot();
}

void ToolForm::on_volumeUpBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->postVolumeUp();
}

void ToolForm::on_volumeDownBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->postVolumeDown();
}

void ToolForm::on_closeScreenBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->setDisplayPower(false);
}

void ToolForm::on_expandNotifyBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->expandNotificationPanel();
}

void ToolForm::on_expandSettingsBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (device) {
        device->expandSettingsPanel();
    }
}

void ToolForm::on_rotateBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (device) {
        device->rotateDevice();
    }
}

void ToolForm::on_touchBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }

    m_showTouch = !m_showTouch;
    device->showTouch(m_showTouch);
}

void ToolForm::on_cameraTorchBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device || !device->isCameraMode()) {
        return;
    }
    m_cameraTorch = !m_cameraTorch;
    device->setCameraTorch(m_cameraTorch);
    ui->cameraTorchBtn->setStyleSheet(m_cameraTorch ? "color: #f0c419" : "");
}

void ToolForm::on_cameraZoomOutBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (device && device->isCameraMode()) {
        device->cameraZoomOut();
    }
}

void ToolForm::on_cameraZoomInBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (device && device->isCameraMode()) {
        device->cameraZoomIn();
    }
}

void ToolForm::on_groupControlBtn_clicked()
{
    m_isHost = !m_isHost;
    updateGroupControl();
}

void ToolForm::on_openScreenBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->setDisplayPower(true);
}

void ToolForm::on_clipboardBtn_clicked()
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    device->requestDeviceClipboard();
}

void ToolForm::on_cursorLockBtn_clicked()
{
    // The button has two behaviours depending on modifier keys:
    //   - Plain click: toggle the current cursor-lock state (same as F1).
    //   - Ctrl+click: open a dialog to reassign the lock key.
    VideoForm *vf = qobject_cast<VideoForm*>(parent());
    if (!vf) {
        return;
    }

    if (QApplication::keyboardModifiers() & Qt::ControlModifier) {
        // ── Ctrl+click: let the user press any key to rebind ─────────────
        QMessageBox msgBox(this);
        msgBox.setWindowTitle("Change cursor-lock key");
        msgBox.setText(
            QString("Current key: <b>%1</b><br><br>"
                    "Choose a new key:\n")
            .arg(QKeySequence(vf->cursorLockKey()).toString()));
        QPushButton *f1Btn  = msgBox.addButton("F1 (default)",  QMessageBox::ActionRole);
        QPushButton *f2Btn  = msgBox.addButton("F2",            QMessageBox::ActionRole);
        QPushButton *f3Btn  = msgBox.addButton("F3",            QMessageBox::ActionRole);
        QPushButton *f4Btn  = msgBox.addButton("F4",            QMessageBox::ActionRole);
        QPushButton *tabBtn = msgBox.addButton("Tab",           QMessageBox::ActionRole);
        QPushButton *graveBtn = msgBox.addButton("`  (backtick)",  QMessageBox::ActionRole);
        msgBox.addButton(QMessageBox::Cancel);
        msgBox.exec();

        int newKey = -1;
        if      (msgBox.clickedButton() == f1Btn)    newKey = Qt::Key_F1;
        else if (msgBox.clickedButton() == f2Btn)    newKey = Qt::Key_F2;
        else if (msgBox.clickedButton() == f3Btn)    newKey = Qt::Key_F3;
        else if (msgBox.clickedButton() == f4Btn)    newKey = Qt::Key_F4;
        else if (msgBox.clickedButton() == tabBtn)   newKey = Qt::Key_Tab;
        else if (msgBox.clickedButton() == graveBtn) newKey = Qt::Key_QuoteLeft;

        if (newKey != -1) {
            vf->setCursorLockKey(newKey);
            // Update tooltip to reflect new key.
            ui->cursorLockBtn->setToolTip(
                QString("cursor lock (%1) — Ctrl+click to change")
                .arg(QKeySequence(newKey).toString()));
        }
    } else {
        // ── Plain click: fire the lock key shortcut programmatically ─────
        // Synthesise a shortcut activation the same way pressing the key would.
        // We call the shortcut's activated() signal indirectly via QKeyEvent
        // because QShortcut doesn't expose an activate() method.  Instead we
        // route through VideoForm which owns the shortcut.
        //
        // Simplest correct approach: emit a synthetic QKeyEvent to the video form.
        QKeyEvent press(QEvent::KeyPress, vf->cursorLockKey(), Qt::NoModifier);
        QCoreApplication::sendEvent(vf, &press);
        QKeyEvent release(QEvent::KeyRelease, vf->cursorLockKey(), Qt::NoModifier);
        QCoreApplication::sendEvent(vf, &release);

        // Reflect state in button checked appearance.
        m_cursorLockState = !m_cursorLockState;
        ui->cursorLockBtn->setChecked(m_cursorLockState);
    }
}
