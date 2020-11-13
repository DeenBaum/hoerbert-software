/***************************************************************************
 * hörbert Software
 * Copyright (C) 2019 WINZKI GmbH & Co. KG
 *
 * Authors of the original version: Igor Yalovenko, Rainer Brang
 * Dec. 2019
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 ****************************************************************************/

#include "devicemanager.h"

#include <QStorageInfo>
#include <QDebug>
#include <QProcess>
#include <QProgressDialog>
#include <QProgressBar>
#include <QInputDialog>
#include <QLineEdit>
#include <QApplication>

#ifdef _WIN32
#include <tchar.h>
#include <windows.h>
#include <direct.h>
#include <shlobj.h>
#include "helper.h"
#endif

#include "pleasewaitdialog.h"

struct VolumeInfo{

    VolumeInfo(const QString &n, const QStorageInfo &info):
    name(n),
    storageInfo(info){

    }
    QString name;
    QStorageInfo storageInfo;
};

DeviceManager::DeviceManager()
{
    m_currentDrive = "";
    m_currentDriveName = "";
    // Get the list of available drives right from the start.
    getDeviceList();
}

void DeviceManager::refresh( const QString &driveName )
{
    std::map<QString, VolumeInfo_ptr>::const_iterator ret;
    if( driveName != NULL ){
        ret = _deviceName2Root.find(driveName);
    } else {
        ret = _deviceName2Root.find(m_currentDriveName);
    }
    ret->second->storageInfo.refresh();
}

bool DeviceManager::isRemovable(const QString &volumeRoot)
{
#ifdef _WIN32
    auto root = reinterpret_cast<LPCWSTR>(volumeRoot.utf16());
    UINT type = GetDriveType(root);
    return (type == DRIVE_REMOVABLE);
#elif defined (Q_OS_MACOS)
    QString cmd = "diskutil info " + volumeRoot;
    QString output = executeCommand(cmd);

    return output.contains(QRegExp("Removable Media:.*Removable", Qt::CaseInsensitive));
#elif defined (Q_OS_LINUX)
    QString cmd = "udevadm info --query=property --export --name=" + volumeRoot;
    QString output = executeCommand(cmd);

    return output.contains(QRegExp("ID_USB_DRIVER='usb-storage'", Qt::CaseSensitive));
#endif
}

bool DeviceManager::isEjectable(const QString &volumeRoot)
{
#ifdef _WIN32
    auto root = reinterpret_cast<LPCWSTR>(volumeRoot.utf16());
    UINT type = GetDriveType(root);
    return (type != DRIVE_FIXED);
#elif defined (Q_OS_MACOS)
    QString cmd = "diskutil info " + volumeRoot;
    QString output = executeCommand(cmd);

    return output.contains(QRegExp("Ejectable", Qt::CaseInsensitive));
#elif defined (Q_OS_LINUX)
    QString cmd = "udevadm info --query=property --export --name=" + volumeRoot;
    QString output = executeCommand(cmd);

    return output.contains(QRegExp("ID_USB_DRIVER='usb-storage'", Qt::CaseSensitive));
#endif
}

ListString DeviceManager::getDeviceList()
{
    ListString  driveList;
    driveList.clear();

    QList<QStorageInfo> volumeList= QStorageInfo::mountedVolumes();

    for(const auto& v: volumeList)
    {
        auto root = getRoot(v);

        if(v.isValid() && v.isReady() && isRemovable(root))
        {
            QString diskName = QObject::tr("USB Drive");
            if (v.name() != "")
            {
                diskName = v.displayName();
            }

            QString volume = diskName + " (" + root + ")";
            driveList.push_back(volume);

			VolumeInfo_ptr vol=std::make_shared<VolumeInfo>(root,v);
            _deviceName2Root.emplace(volume,vol);
      }
  }

  return driveList;
}

QString DeviceManager::getRoot(const QStorageInfo &info)
{
#ifdef _WIN32
    return info.rootPath();
#else
    return info.device();
#endif
}

QString DeviceManager::getDrivePath(const QString &driveName) const
{
    QString path = QString("");
    if (m_currentDriveName.isEmpty())
        return path;

    QString theDriveName = m_currentDriveName;
    if( driveName!=nullptr ){
        theDriveName = driveName;
    }
    auto drive = _deviceName2Root.find(theDriveName);

    path = drive->second->storageInfo.rootPath();
    return path;
}

QString DeviceManager::getDriveName(const QString &drivePath)
{
    QList<QStorageInfo> volumeList= QStorageInfo::mountedVolumes();

    for(const auto& v: volumeList)
    {
        auto root = getRoot(v);

        if(v.isValid() && v.isReady() && isRemovable(root) && v.rootPath().compare(drivePath) == 0)
        {
            QString diskName = QObject::tr("USB Drive");
            if (v.name() != "")
            {
                diskName = v.displayName();
            }

            qDebug() << "Drive Name: " << diskName + " (" + root + ")";
            return diskName + " (" + root + ")";
        }
    }

    return QString();
}

#if defined (Q_OS_MACOS) || defined (Q_OS_LINUX)
/**
 * @brief Determine if the given device is valid(removable and storage).
 * @param device(root.device())
 * @return true if the given drive is valid
 * @note ensure you update device list by calling getDeviceList before calling this method.
 */
bool DeviceManager::isValidDevice(const QString &device)
{
    for (const auto& drive : _deviceName2Root)
    {
        auto device_str = drive.second->storageInfo.device();
        if (QString(device_str).section("/",QString(device_str).count("/")).compare(device) == 0)
            return true;
    }
    return false;
}
#endif

RetCode DeviceManager::formatDrive(QWidget* parentWidget, const QString &driveName, const QString &newLabel, const QString &passwd )
{
#ifdef Q_OS_LINUX
    Q_UNUSED( passwd )
#endif

    auto new_drive_label = newLabel;
    if (new_drive_label.isEmpty()) {
        new_drive_label = DEFAULT_DRIVE_LABEL;
    }

    auto ret = _deviceName2Root.find(driveName);
    if (ret == _deviceName2Root.end())
    {
        return DEVICE_NOT_FOUND;
    }

    QProcess formatCmdProcess;
    QString root = ret->second->name;

    if ( !isRemovable( root ) ){
        return FAILURE;
    }

#ifndef Q_OS_LINUX
    QString cmd = getFormatCommand(root, new_drive_label);

    if ( !cmd.isEmpty() ){

        pleaseWait = new PleaseWaitDialog();
        pleaseWait->setParent( parentWidget );
        pleaseWait->setWindowFlags(Qt::Window | Qt::Dialog | Qt::WindowTitleHint | Qt::CustomizeWindowHint);
        pleaseWait->setWindowTitle(tr("Formatting memory card..."));
        pleaseWait->setWindowModality(Qt::ApplicationModal);
        pleaseWait->show();

        formatProcess = new QProcess();

        connect( formatProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), [=](int exitCode, QProcess::ExitStatus exitStatus){
            Q_UNUSED( exitCode )
            Q_UNUSED( exitStatus )

            QString output = formatProcess->readAllStandardOutput();
            if (output.contains("Finished erase", Qt::CaseInsensitive) || output.contains("Format complete", Qt::CaseInsensitive))
            {
                pleaseWait->setResultString( tr("The memory card has been formatted successfully"));
            }
            else
            {
                pleaseWait->setResultString( tr("Formatting the memory card failed.") );
            }
        });

        formatProcess->start(cmd);

        return SUCCESS;
    }
#else
    int ret_code = -1;
    QString output = "";
    std::tie(ret_code, output) = executeCommandWithSudo(QString("umount %1").arg(root), passwd);
    qDebug() << "umount:\n" << output;
    if (ret_code == PASSWORD_INCORRECT) {
        return PASSWORD_INCORRECT;
    }

    if (ret_code == FAILURE || !output.isEmpty()) {
        executeCommand("sudo -k");
        return FAILURE;
    }

    ret_code = -1;
    output = "";

    std::tie(ret_code, output) = executeCommandWithSudo(QString("mkfs.vfat -n %1 -I %2").arg(new_drive_label).arg(root), passwd, true);

    qDebug() << "mkfs.vfat:\n" << output;
    if (ret_code == PASSWORD_INCORRECT) {
        return PASSWORD_INCORRECT;
    }

    if (ret_code == FAILURE || output.contains("mkfs.vfat:")) {
        executeCommand("sudo -k");
        return FAILURE;
    }

    QString user_name = qgetenv("USER");
    QDir dir(QString("/media/%1").arg(user_name));
    if (dir.exists())
    {
        ret_code = -1;
        output = "";

        QApplication::setOverrideCursor(Qt::WaitCursor);    // hint to background action
        qApp->processEvents();

        std::tie(ret_code, output) = executeCommandWithSudo(QString("mkdir \"/media/%1/%2\"").arg(user_name).arg(new_drive_label), passwd);

        QApplication::restoreOverrideCursor();
        qApp->processEvents();

        qDebug() << "mkdir:\n" << output;
        if (ret_code == PASSWORD_INCORRECT) {
            return PASSWORD_INCORRECT;
        }

        if (!output.isEmpty())
        {
            qDebug() << "Failed creating mount point";
        }

        ret_code = -1;
        output = "";

        QApplication::setOverrideCursor(Qt::WaitCursor);    // hint to background action
        qApp->processEvents();

        std::tie(ret_code, output) = executeCommandWithSudo(QString("sudo -S mount %1 \"/media/%2/%3\" -o uid=%2 -o gid=%2").arg(root).arg(user_name).arg(new_drive_label), passwd);

        QApplication::restoreOverrideCursor();
        qApp->processEvents();

        qDebug() << "mount:\n" << output;

        if (ret_code == PASSWORD_INCORRECT) {
            return PASSWORD_INCORRECT;
        }

        if (output.contains("mount:"))
        {
            qDebug() << "Failed mounting formatted drive";
        }
    }

    // sudo should forget password, otherwise the commands will work even with incorrect password within 15 minutes
    executeCommand("sudo -k");

    return SUCCESS;
#endif
    return FAILURE;
}


std::pair<int, QString> DeviceManager::executeCommandWithSudo(const QString &cmd, const QString &passwd, bool showPleaseWaitDialog, QWidget* parentWidget)
{
    int ret_code = -1;
    QString cmd_ = cmd;
    if (!cmd_.startsWith("sudo -S"))
    {
        cmd_ = "sudo -S " + cmd_;
    }

    qDebug() << cmd_;

    if( showPleaseWaitDialog ){
        pleaseWait = new PleaseWaitDialog();
        pleaseWait->setParent( parentWidget );
        pleaseWait->setWindowFlags(Qt::Window | Qt::Dialog | Qt::WindowTitleHint | Qt::CustomizeWindowHint);
        pleaseWait->setWindowTitle(tr("Formatting memory card..."));
        pleaseWait->setWindowModality(Qt::ApplicationModal);
        pleaseWait->show();
    }

    formatProcess = new QProcess();
    formatProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect( formatProcess, &QProcess::readyReadStandardOutput, [=] () {
        QString output = formatProcess->readAllStandardOutput();
        if (output.contains("Sorry, try again")) {      //@TODO This is debatable... does it work with other system languages at all?
            if( showPleaseWaitDialog ){
                pleaseWait->setResultString( tr("Password is incorrect. Please try again.") );
            }
            formatProcess->close();
        }
        qDebug() << output;
    });


    connect( formatProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), [=](int exitCode, QProcess::ExitStatus exitStatus){
        Q_UNUSED( exitCode )
        Q_UNUSED( exitStatus )

        QString output = formatProcess->readAllStandardOutput();
        if( showPleaseWaitDialog ){
            if (output.contains("Finished erase", Qt::CaseInsensitive) || output.contains("Format complete", Qt::CaseInsensitive))
            {
                pleaseWait->setResultString( tr("The memory card has been formatted successfully"));
            }
            else
            {
                pleaseWait->setResultString( tr("Formatting the memory card failed.") );
            }
        }
    });


    formatProcess->start(cmd_);
    if (!formatProcess->waitForStarted())
    {
        qDebug() << "Process failed to start!";
        ret_code = FAILURE;
        return std::pair<int, QString>(ret_code, formatProcess->readAll());
    }

    formatProcess->write(QString("%1\n").arg(passwd).toUtf8().constData());

    return std::pair<int, QString>(SUCCESS, "");    // in case we're only starting formatting, there's nothing better we can return (if the process START was successful.

/*
    case MOUNT_FAILURE:
        QMessageBox::information(this, tr("Format"), tr("Remounting the memory card failed. You may have to mount it manually."));
        deselectDrive();
        updateDriveList();
        break;
*/


}

RetCode DeviceManager::ejectDrive(const QString &driveName)
{
    auto ret = _deviceName2Root.find(driveName);
    if (ret == _deviceName2Root.end())
    {
        return DEVICE_NOT_FOUND;
    }

    QString diskName=ret->second->name;

#ifdef _WIN32
    auto driveLetter = diskName.at(0);
  
    QApplication::setOverrideCursor(Qt::WaitCursor);    // hint to background action
    qApp->processEvents();

    if ( 0 != EjectDriveWin(driveLetter.unicode()) ){
        QApplication::restoreOverrideCursor();
        qApp->processEvents();
        return FAILURE;
    }

    QApplication::restoreOverrideCursor();
    qApp->processEvents();

#elif defined (Q_OS_MACOS)
    // we use the "unmount" command, since it does not ask for admin privileges.
    QApplication::setOverrideCursor(Qt::WaitCursor);    // hint to background action
    qApp->processEvents();

    QString output = executeCommand("diskutil unmount " + diskName);               // execute the umount command synchronously.

    QApplication::restoreOverrideCursor();
    qApp->processEvents();

    if ( !(output.contains("Volume", Qt::CaseInsensitive) && output.contains("unmounted", Qt::CaseInsensitive)) )       // The positive result of "unmount" looks like this: Volume NO NAME on disk2s1 unmounted
    {
        return FAILURE;
    }
#elif defined (Q_OS_LINUX)
    // TODO: udisksctl is only for preliminary use, need to replace it with dbus-send and gdbus in the future

    QApplication::setOverrideCursor(Qt::WaitCursor);    // hint to background action
    qApp->processEvents();

    QString output = executeCommand("udisksctl unmount -b " + diskName);

    QApplication::restoreOverrideCursor();
    qApp->processEvents();

    if (!output.contains("Unmounted "))
        return FAILURE;

    output = executeCommand("udisksctl power-off -b " + diskName);
    if (!output.isEmpty())
        return FAILURE;
#endif

    m_currentDrive = QString();
    m_currentDriveName = QString();
    return SUCCESS;
}

void DeviceManager::setCurrentDrive(const QString &driveName)
{
    if (driveName.isEmpty())
    {
        m_currentDrive = driveName;
        return;
    }
    auto ret = _deviceName2Root.find(driveName);
    if(ret == _deviceName2Root.end())
    {
        qDebug() << "Device not found!";
        return;
    }
    m_currentDrive = ret->second->name;
    m_currentDriveName = driveName;

    qDebug() << m_currentDrive << m_currentDriveName;
}

QString DeviceManager::selectedDrive() const
{
    return m_currentDrive;
}

QString DeviceManager::selectedDriveName() const
{
    return m_currentDriveName;
}

QString DeviceManager::getFormatCommand(const QString &root, const QString &driveLabel)
{
    QString cmd;
    QString newRoot = "";
#if defined (Q_OS_WIN)
    newRoot = root.at(0);
    cmd = "CMD /C format " + newRoot + ": /FS:FAT32 /Q /X /Y /V:" + driveLabel;
#elif defined (Q_OS_MACOS)
    newRoot = root.left(root.length() - 2);
    cmd = QString("diskutil eraseDisk FAT32 %1 MBRFormat %2").arg(driveLabel).arg(newRoot);
#endif
    return cmd;
}

QString DeviceManager::executeCommand(const QString &cmdString)
{
    QProcess cmdProgress;
    cmdProgress.start(cmdString);
    cmdProgress.waitForFinished();
    return cmdProgress.readAllStandardOutput();
}

qint64 DeviceManager::getVolumeSize(const QString &driveName)
{
    auto ret = _deviceName2Root.find(driveName);
    if (ret == _deviceName2Root.end())
    {
        return 0;
    }
    return ret->second->storageInfo.bytesTotal();
}

qint64 DeviceManager::getAvailableSize(const QString &driveName)
{
    auto ret = _deviceName2Root.find(driveName);
    if (ret == _deviceName2Root.end())
    {
        return 0;
    }
    return ret->second->storageInfo.bytesAvailable();
}

QString DeviceManager::getVolumeFileSystem(const QString &driveName)
{
    auto ret = _deviceName2Root.find(driveName);
    if (ret == _deviceName2Root.end())
    {
        return "";
    }

    return ret->second->storageInfo.fileSystemType();
}

bool DeviceManager::hasFat32FileSystem(const QString &driveName)
{
    auto ret = _deviceName2Root.find(driveName);
    if (ret == _deviceName2Root.end())
    {
        return false;
    }

    auto fileSystem=ret->second->storageInfo.fileSystemType();
    if(fileSystem == "msdos" || fileSystem == "FAT32" || fileSystem == "vfat")
    {
        return true;
    }
    return false;
}

bool DeviceManager::isWriteProtected(const QString &driveName)
{
    auto ret = _deviceName2Root.find(driveName);
    if (ret == _deviceName2Root.end())
    {
        return false;
    }
    return ret->second->storageInfo.isReadOnly();
}
