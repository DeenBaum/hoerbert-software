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

#include "mainwindow.h"

#include <QProcess>

#include <QApplication>
#include <QLayout>
#include <QCheckBox>

#include <QMenuBar>
#include <QFileDialog>

#include <QPushButton>
#include <QMessageBox>
#include <QDate>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QDesktopServices>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkAccessManager>
#include <QDesktopWidget>
#include <QSettings>
#include <QDebug>
#include <QLocale>


extern QString SYNC_PATH;
extern QString HOERBERT_TEMP_PATH;
#if defined (Q_OS_WIN)
extern QString ZIP_PATH;
#endif

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_isWritingToDisk = false;
    m_migrationPath = QString("");

    QDesktopWidget dw;
    setGeometry((dw.width() - 800) / 2, (dw.height() - 494) / 2, 800, 494);
    setWindowTitle("hörbert");
    setWindowIcon(QIcon(":/images/hoerbert.ico"));
    setObjectName("MainWindow");
    setStyleSheet("#MainWindow {background: url(:/images/pappelholz.jpg)}");

    m_centralWidget = new QWidget(this);
    setCentralWidget(m_centralWidget);

    m_layout = new QVBoxLayout(m_centralWidget);
    m_layout->setContentsMargins(25, 15, 25, 25);

    m_capBar = new CapacityBar(m_centralWidget);

    m_infoLayout = new QHBoxLayout();
    m_infoLayout->setContentsMargins(0, 0, 0, 0);
    m_infoLayout->addWidget(new QLabel(), 1);
    m_infoLayout->addWidget(m_capBar, 1);
    m_infoLayout->addWidget(new QLabel(m_centralWidget), 1);

    m_stackWidget = new QStackedWidget(m_centralWidget);

    m_cardPage = new CardPage(this);

    m_playlistPage = new PlaylistPage(this);

    m_shadow = new QGraphicsDropShadowEffect(this);
    m_shadow->setBlurRadius(10);
    m_shadow->setOffset(5, 5);
    m_shadow->setColor(QColor(0, 0, 0));

    m_playlistPage->setGraphicsEffect(m_shadow);

    m_aboutDlg = new AboutDialog(this);

    m_featuresDlg = new AdvancedFeaturesDialog(this);
    m_featuresDlg->setModal(true);

    m_stackWidget->addWidget(m_cardPage);
    m_stackWidget->addWidget(m_playlistPage);

    m_layout->addLayout(m_infoLayout);
    m_layout->addWidget(m_stackWidget);

    m_dbgDlg = new DebugDialog(this);

    createActions();

    connect(m_aboutDlg, &AboutDialog::checkForUpdateRequested, this, &MainWindow::checkForUpdates);

    connect(m_featuresDlg, &AdvancedFeaturesDialog::buttonSettingsChanged, this, [this]() {
        m_cardPage->updateButtons();
    });

    connect( m_playlistPage->getPlaylistView(), &PlaylistView::currentPlaylistIsUntouched,  m_moveToPlaylistMenu, &QMenu::setEnabled );

    connect(m_playlistPage, &PlaylistPage::cancelClicked, this, [this]() {
        m_cardPage->enableButtons(true);
        showHideEditMenuEntries(false);

        m_stackWidget->setCurrentIndex(0);
        m_cardPage->update();
        m_capBar->resetEstimation();

        this->repaint();        // make sure the GUI is repainted. If not, it just looks ugly.
        qApp->processEvents();
    });

    connect(m_playlistPage, &PlaylistPage::errorOccurred, this, [this] (const QString &errorString) {
        m_dbgDlg->appendLog(errorString);
    });

    connect(m_cardPage, &CardPage::driveCapacityUpdated, m_capBar, &CapacityBar::setParams);

    connect(m_cardPage, &CardPage::driveCapacityUpdated, this, [&](quint64 used, quint64 total) {
        m_playlistPage->setDriveSpaceDetails(used, total, m_capBar->estimatedSeconds());
    });

    connect(m_cardPage, &CardPage::driveSelected, this, [this] (const QString &driveName) {
        bool remind_backup = false;
        if (driveName.compare(m_selectedDriveLabel) != 0)
            remind_backup = true;

        m_selectedDriveLabel = driveName;

        if (driveName.isEmpty() || m_cardPage->isDiagnosticsModeEnabled())
        {
            m_printAction->setEnabled(false);
            m_backupAction->setEnabled(false);
            m_restoreAction->setEnabled(false);
            m_formatAction->setEnabled(false);
            m_selectManually->setEnabled(true);

            if( driveName.isEmpty() )
            {
                m_toggleDiagnosticsModeAction->setEnabled(false);
                m_toggleDiagnosticsModeAction->setChecked(false);
            }

            if( m_cardPage->isDiagnosticsModeEnabled() && !driveName.isEmpty() )
            {
                m_advancedFeaturesAction->setEnabled(false);
                m_toggleDiagnosticsModeAction->setEnabled(true);
                m_toggleDiagnosticsModeAction->setChecked(true);
            }

        }
        else {
            // enable menu actions
            m_printAction->setEnabled(true);
            m_backupAction->setEnabled(true);
            m_restoreAction->setEnabled(true);
            m_formatAction->setEnabled(true);
            m_advancedFeaturesAction->setEnabled(true);
            m_toggleDiagnosticsModeAction->setEnabled(true);
            m_selectManually->setEnabled(false);

            if (remind_backup)
                this->remindBackup();

            if (!m_migrationPath.isEmpty())
            {
                this->migrate(m_migrationPath);
                m_migrationPath = QString("");
            }
        }
    });

    connect(m_cardPage, &CardPage::playlistChanged, this, [this] (quint8 dir_num, const QString &dir_path, const AudioList &result) {
        m_stackWidget->setCurrentIndex(1);
        m_playlistPage->setListData(dir_path, dir_num, result);
        m_playlistPage->setBackgroundColor(m_cardPage->getPlaylistColor(dir_num));
        m_shadow->setColor(m_cardPage->getPlaylistColor(dir_num));

        showHideEditMenuEntries( true );
/*
#if defined(Q_OS_MAC)
        m_subMenuBegin->setEnabled(true);
        m_subMenuEnd->setEnabled(true);
        m_subMenuBegin->menuAction()->setVisible(true);
        m_subMenuEnd->menuAction()->setVisible(true);
        m_moveToPlaylistMenu->menuAction()->setVisible(true);
#endif
        m_moveToPlaylistMenu->setEnabled(true);
*/
    });

    connect(m_cardPage, &CardPage::driveSelected, this, [this](const QString &dirPath) {
        if( dirPath.isEmpty() ){
            this->m_migrationPath = QString("");
        }
    });

    connect(m_cardPage, &CardPage::migrationNeeded, this, [this](const QString &dirPath) {
        this->m_migrationPath = dirPath;
    });

    connect(m_cardPage, &CardPage::toggleDiagnosticsMode, this, &MainWindow::switchDiagnosticsMode);

    connect(m_cardPage, &CardPage::plausibilityFixNeeded, this, &MainWindow::makePlausible);

    connect(m_cardPage, &CardPage::enableEditMenuItems, this, &MainWindow::showHideEditMenuEntries);

    connect(m_playlistPage, &PlaylistPage::commitChanges, this, &MainWindow::processCommit, Qt::QueuedConnection);

    connect(m_playlistPage, &PlaylistPage::durationChanged, m_capBar, &CapacityBar::addSpaceInSeconds);

    //m_cardPage->selectDrive();

    m_backupManager = nullptr;

    m_dbgDlg = new DebugDialog(this);
}

MainWindow::~MainWindow()
{
    QSettings settings;
    settings.beginGroup("Global");
    bool regenerateHoerbertXml = settings.value("regenerateHoerbertXml").toBool();
    settings.endGroup();

    if( m_cardPage->isHoerbertXMLDirty() && regenerateHoerbertXml ){
        m_cardPage->recreateXml();
    }

    if (!deleteAllFilesInDirecotry(HOERBERT_TEMP_PATH))
    {
        perror("Failed to delete old files in temp folder");
    }
}

void MainWindow::makePlausible(std::list <int> fixList)
{
    m_plausibilityCheckMutex.lock();    // We need to carefully unlock this lock whenever we return from this method anywhere.
    auto clean_selected = QMessageBox::question(this, tr("Plausibility check"), tr("The files on the memory card need some cleanup. Should this app do some clean up?"), QMessageBox::Yes|QMessageBox::No, QMessageBox::No );
    if (clean_selected == QMessageBox::No)
    {
        m_plausibilityCheckMutex.unlock();
        return;
    }

    auto backup_selected = QMessageBox::question(this, tr("Backup"), tr("It is recommended to backup memory card before cleaning up. Do you want to backup now?"), QMessageBox::Yes|QMessageBox::No, QMessageBox::No );
    if (backup_selected == QMessageBox::Yes)
        backupCard();

    auto dir = tailPath(m_cardPage->currentDrivePath());
    auto sub_dir = QString();

    for (auto i : fixList)
    {
        sub_dir = dir + QString::number(i);

        QDir dir(sub_dir);
        if (!dir.exists()) {
            qDebug() << "Sub-directory does not exist - " << i;
            m_plausibilityCheckMutex.unlock();
            return;
        }
        dir.setFilter(QDir::Files | QDir::NoSymLinks);
        dir.setNameFilters(QStringList() << "*" + DEFAULT_DESTINATION_FORMAT);
        dir.setSorting(QDir::Name);

        QFileInfoList list = dir.entryInfoList();

        // clean up empty files(file size is 0KB)
        for (const auto &item : list) {
            if (item.size() == 0) {
                if (QFile::remove(item.absoluteFilePath())) {
                    qDebug() << "Deleted empty file:" << item.absoluteFilePath();
                } else {
                    qDebug() << "Failed deleting empty file:" << item.absoluteFilePath();
                }
            }
        }

        list = dir.entryInfoList();

        std::sort(list.begin(), list.end(), sortByNumber);

        auto index = 0;
        for (const auto &item : list) {
            if (item.fileName().toLower().remove(DEFAULT_DESTINATION_FORMAT.toLower()).toInt() != index) {
                qDebug() << "Index" << index << "is missing in" << sub_dir;
                moveFile(item.absoluteFilePath(), tailPath(item.absolutePath()) + QString::number(index) + DEFAULT_DESTINATION_FORMAT);
            }
            index++;
        }
    }

    m_cardPage->update();
    m_plausibilityCheckMutex.unlock();
}

void MainWindow::processCommit(const QMap<ENTRY_LIST_TYPE, AudioList> &list, const quint8 dir_index)
{
    m_cardPage->enableButtons(true);

    this->repaint();        // make sure the GUI is repainted. If not, it just looks ugly.
    qApp->processEvents();

    if (list.count() == 0) {
        m_cardPage->update();
        m_stackWidget->setCurrentIndex(0);
        return;
    }

    QString dir_path = QString("%1/%2").arg(m_playlistPage->directoryPath()).arg(dir_index);

    // ensure the directory exists
    if (!QDir(dir_path).exists())
    {
        if (!QDir(m_playlistPage->directoryPath()).mkdir(QString::number(dir_index)))
        {
            qDebug() << "Failed to create a sub-directory!" << dir_path;
            return;
        }

        qDebug() << "Created sub-directory" << dir_path;
    }

    m_cardPage->setCardManageButtonsEnabled(false);

    // disable button while processing
    m_cardPage->setButtonEnabled(dir_index, false);
    m_stackWidget->setCurrentIndex(0);

    int *no_silence_counter = new int(0);

    HoerbertProcessor *processor = new HoerbertProcessor(dir_path, dir_index);
    processor->setEntryList(list);

    connect(processor, &HoerbertProcessor::processUpdated, m_cardPage, [=] (int percentage) {
       m_cardPage->setPercent(processor->directoryNumber(), percentage);
    }, Qt::UniqueConnection);

    connect(processor, &HoerbertProcessor::taskCompleted, m_cardPage, [=] (int failCounter, int totalEntryCount) {
        Q_UNUSED(failCounter)
        Q_UNUSED(totalEntryCount);
    }, Qt::UniqueConnection);

    connect(processor, &HoerbertProcessor::noSilenceDetected, this, [=] () {
       (*no_silence_counter)++;
    }, Qt::UniqueConnection);

    connect(processor, &HoerbertProcessor::failed, this, [=] (const QString &errorString) {
        this->processorErrorOccurred("On Commit\n" + errorString);
    }, Qt::UniqueConnection);

    connect(processor, &QThread::finished, this, [=] () {
       // enable the button back
        m_cardPage->setButtonEnabled(dir_index, true);
        m_cardPage->setPercent(dir_index, 0);
        m_cardPage->update();
        m_capBar->resetEstimation();
        m_cardPage->updateUsedSpace();
        m_playlistPage->clearDirectoryEstimation(dir_index);
        processor->quit();
        processor->deleteLater();

        this->sync();

        if (*no_silence_counter > 0)
        {
            QMessageBox::information(this, tr("Split Information"), tr("%1 file(s) were not split, because there was no silent part to split at.\nYou may want to split the file(s) in fixed 3-minute chunks").arg(*no_silence_counter));
        }

        if (!m_cardPage->isProcessing())
        {
            m_printAction->setEnabled(true);
            m_backupAction->setEnabled(true);
            m_restoreAction->setEnabled(true);
            m_formatAction->setEnabled(true);

            m_cardPage->setCardManageButtonsEnabled(true);
        }

    }, Qt::UniqueConnection);

    processor->start();
    m_isWritingToDisk = true;
}

void MainWindow::processorErrorOccurred(const QString &errorString)
{
    m_dbgDlg->appendLog(errorString);
}

void MainWindow::taskCompleted(int failCount, int totalCount)
{
    Q_UNUSED(failCount)
    Q_UNUSED(totalCount)
}

void MainWindow::migrate(const QString &dirPath)
{
// migration is now mandatory. We create hoerbert.xml anyways before ejecting the card, so this should be no problem.
//    auto selected = QMessageBox::question(this, tr("Migration is necessary"), tr("This memory card's files need to be updated to a new format.\nDo you want to update the memory card's files for using it with this new app in the future?") );
//
//    if (selected == QMessageBox::No)
//        return;

    XmlMetadataReader xml_reader(dirPath);
    AudioList metadata_list = xml_reader.getEntryList();
    for (const AudioEntry & entry : metadata_list) {
        qDebug() << entry.id << entry.metadata.comment;
    }

    if (metadata_list.count() > 0)
    {
        bool *is_completed = new bool(true);

        HoerbertProcessor *processor = new HoerbertProcessor(dirPath, -1);

        m_progress = new QProgressDialog(this);
        m_progress->setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::CustomizeWindowHint);
        m_progress->setModal(true);
        m_progress->setWindowTitle(tr("Migration"));
        m_progress->setLabelText(tr("Migrating files to the new hoerbert.app. Please wait."));
        m_progress->setFixedWidth(600);
        m_progress->setRange(0, 100);

        QPushButton *abort_button = new QPushButton(m_progress);
        abort_button->setText(tr("Abort"));

        // setting cancel button connects the button to the dialog to hide the dialog on button click
        m_progress->setCancelButton(abort_button);
        // need to disconnect the connections since clicking on the button immediately hides the dialog
        m_progress->disconnect(abort_button);

        // then define custom connections
        connect(abort_button, &QPushButton::clicked, this, [=] () {
            m_progress->setLabelText(tr("Aborting..."));
            m_progress->show();
            QCoreApplication::processEvents();
            abort_button->setDisabled(true);
           processor->abort();
           (*is_completed) = false;
        });

        m_progress->show();

        processor->addEntryList(ENTRY_LIST_TYPE::METADATA_CHANGED_ENTRIES, metadata_list);

        connect(processor, &HoerbertProcessor::processUpdated, this, [this] (int percent) {
           m_progress->setValue(percent);
           m_progress->setLabelText(tr("Migrating files to the new hoerbert.app. Please wait."));
           QCoreApplication::processEvents();
        });

        connect(processor, &HoerbertProcessor::failed, this, [this](const QString &errorString) {
            this->processorErrorOccurred("On migration\n" + errorString);
        });

        connect(processor, &QThread::finished, this, [=] () {
            processor->quit();
            processor->deleteLater();

            if (*is_completed)
                moveFile(dirPath + HOERBERT_XML, dirPath + HOERBERT_XML_BACKUP);

            this->sync();
            m_progress->close();
            m_progress->deleteLater();
        });

        QEventLoop loop;
        connect(processor, &QThread::finished, &loop, &QEventLoop::quit);

        processor->start();

        loop.exec();

        m_isWritingToDisk = true;

        m_cardPage->recreateXml();
        m_cardPage->setHoerbertXMLDirty( false );
    }


}

void MainWindow::addTitle()
{
    m_playlistPage->add();
}

void MainWindow::removeTitle()
{
    m_playlistPage->remove();
}

void MainWindow::moveToAnotherPlaylist(quint8 toDir, bool toBeginning)
{
    m_playlistPage->moveSelectedEntriesTo(toDir, toBeginning);
    m_isWritingToDisk = true;
    sync();
}

void MainWindow::printTableOfContent(const QString &outputPath, bool showOnBrowser)
{
    // generate html file and show it on default browser

    setCursor(Qt::WaitCursor);
    QString drive_path = m_cardPage->currentDrivePath();
    QString sub_dir = QString();

    if (drive_path.isEmpty())
    {
        qDebug() << "No drive is selected.";
        return;
    }

    QFileInfoList file_info_list;

    for (int i = 0; i < 9; i++)
    {
        sub_dir = tailPath(drive_path + QString::number(i));

        QDir dir(sub_dir);

        dir.setFilter(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
        dir.setNameFilters(QStringList() << "*" + DEFAULT_DESTINATION_FORMAT);
        dir.setSorting(QDir::Name);

        QFileInfoList list = dir.entryInfoList();
        if (list.isEmpty())
        {
            continue;
        }
        std::sort(list.begin(), list.end(), sortByNumber);

        file_info_list.append(list);
    }

    if (file_info_list.count() <= 0)
    {
        qDebug() << "No audio file detected on the drive.";
        QMessageBox::information(this, tr("Print table of contents"), tr("No audio file detected on the drive."));
        setCursor(Qt::ArrowCursor);
        return;
    }

    m_progress = new QProgressDialog(this);

    AudioInfoThread *thread = new AudioInfoThread(file_info_list);
    connect(thread, &AudioInfoThread::processUpdated, this, [this] (int percent) {
        m_progress->setValue(percent);
        m_progress->setLabelText(tr("Generating table..."));
        QCoreApplication::processEvents();
    });
    connect(thread, &AudioInfoThread::taskCompleted, this, [this, outputPath, showOnBrowser] (const AudioList &result) {
        printHtml(result, outputPath, showOnBrowser);
        this->setCursor(Qt::ArrowCursor);
        m_progress->deleteLater();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    m_progress->setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::CustomizeWindowHint);
    m_progress->setWindowTitle(tr("Printing table of contents"));
    m_progress->setModal(true);
    m_progress->setLabelText(tr("Generating table..."));
    m_progress->setRange(0, 100);
    m_progress->setFixedWidth(360);

    QPushButton *abort_button = new QPushButton(m_progress);
    abort_button->setText(tr("Abort"));

    // setting cancel button connects the button to the dialog to hide the dialog on button click
    m_progress->setCancelButton(abort_button);
    // need to disconnect the connections since clicking on the button immediately hides the dialog
    m_progress->disconnect(abort_button);

    // then define custom connections
    connect(abort_button, &QPushButton::clicked, this, [this, abort_button, thread] () {
        m_progress->setLabelText(tr("Aborting..."));
        m_progress->show();
        QCoreApplication::processEvents();
        abort_button->setDisabled(true);

        thread->quit();
        thread->wait();
        thread->deleteLater();
    });

    m_progress->show();

    QEventLoop loop;
    connect(thread, &QThread::finished, &loop, &QEventLoop::quit);

    thread->start();

    loop.exec();
}

void MainWindow::printHtml(const AudioList &list, const QString &outputPath, bool showOnBrowser)
{
    if (list.count() <= 0)
    {
        qDebug() << "Failed retrieving audio information.";
        return;
    }

    // %1: constant string "hoerbert table of contents", %2: drive name
    QString head = QString("<head>"
                   "<META http-equiv=Content-Type content='text/html; charset=utf-8'>"
                   "<title>%1 [%2]</title>"
                   "</head>").arg(tr("hoerbert table of contents")).arg(m_cardPage->currentDriveName());

    // %1: constant string "hoerbert table of contents", %2: drive name, %3: contents
    QString body = QString("<body style='font-family:Arial,sans;font-size:0.8em;'>"
                   "<h1 class='main' style='text-align:center;font-size:1.2em;'>%1 [%2]</h1>"
                   "<div class='date' style='text-align:center'>%3</div>"
                   "%4"
                   "</body>");

    // %1: items
    QString block_start = "<div class='button' style='margin-top:10px;padding:5px;border:10px solid %1'>";
    QString block_end = "</div>";

    // %1: order number(filename + 1), %2: metadata of audio
    QString tableStart = "<table class='item' cellspacing='5'>";
    QString tableEnd = "</table>";
    QString item = "<tr><td class='track' style='font-weight:bold;'>"
                   "%1"
                   "</td>"
                   "<td class='title' style=''>"
                   "%2"
                   "</td>"
                   "<td class='artist' style=''>"
                   "%3"
                   "</td>"
                   "<td class='file' style=''>"
                   "%4"
                   "</td></tr>";


    QString contents = QString("");
    int prev_dir_num = -1;
    int id = 1;
    for (const auto& entry : list)
    {
        int new_dir_num = entry.path.section("/", -2).section("/", 0, 0).toInt();
        if (new_dir_num != prev_dir_num) {
            if (prev_dir_num != -1) {
                contents += tableEnd;
                contents += block_end;
                id = 1;
            }

            if (new_dir_num - prev_dir_num > 1)
            {
                for (int i = prev_dir_num + 1; i < new_dir_num; i++)
                {
                    contents += block_start.arg(COLOR_LIST.at(i));
                    contents += "<table>";
                    for (int l=0; l < 3; l++) {
                        contents += "<tr>";
                        for (int j=0; j < 3; j++) {
                            contents += "<td>";
                            contents += QString("<span style='height: 10px; width: 10px; opacity: %3;background-color: %1; "
                                                "border-radius: 50%; border: 2px solid %2;display: inline-block'>"
                                                "</span>")
                                    .arg(COLOR_LIST.at(l * 3 + j))
                                    .arg(COLOR_LIST.at(l * 3 + j))
                                    .arg(i == (l * 3 + j) ? "1" : "0.1");
                            contents += "</td>";
                        }
                        contents += "</tr>";
                    }
                    contents += "</table>";
                    contents += block_end;
                }
            }

            contents += block_start.arg(COLOR_LIST.at(new_dir_num));
            contents += "<table>";
            for (int i=0; i < 3; i++) {
                contents += "<tr>";
                for (int j=0; j < 3; j++) {
                    contents += "<td>";
                    contents += QString("<span style='height: 10px; width: 10px; opacity: %3;background-color: %1; "
                                        "border-radius: 50%; border: 2px solid %2;display: inline-block'>"
                                        "</span>")
                            .arg(COLOR_LIST.at(i * 3 + j))
                            .arg(COLOR_LIST.at(i * 3 + j))
                            .arg(new_dir_num == (i * 3 + j) ? "1" : "0.1");
                    contents += "</td>";
                }
                contents += "</tr>";
            }
            contents += "</table>";
            prev_dir_num = new_dir_num;

            contents += tableStart;
        }

        contents += item.arg(id).arg(entry.metadata.title).arg(entry.metadata.album).arg(entry.metadata.comment);
        id++;
    }

    contents += block_end;

    if (prev_dir_num != 8)
    {
        for (int i = prev_dir_num + 1; i < 9; i++)
        {
            contents += block_start.arg(COLOR_LIST.at(i));
            contents += "<table>";
            for (int l=0; l < 3; l++) {
                contents += "<tr>";
                for (int j=0; j < 3; j++) {
                    contents += "<td>";
                    contents += QString("<span style='height: 10px; width: 10px; opacity: %3;background-color: %1; "
                                        "border-radius: 50%; border: 2px solid %2;display: inline-block'>"
                                        "</span>")
                            .arg(COLOR_LIST.at(l * 3 + j))
                            .arg(COLOR_LIST.at(l * 3 + j))
                            .arg(i == (l * 3 + j) ? "1" : "0.1");
                    contents += "</td>";
                }
                contents += "</tr>";
            }
            contents += "</table>";
            contents += block_end;
        }
    }

    QString html = "<html>" + head + body.arg(tr("hörbert table of contents"))
            .arg(m_cardPage->currentDriveName())
            .arg(tr("Date: %1").arg(QDate::currentDate().toString("dd.MM.yyyy")))
            .arg(contents) + "</html>";

    QString output_file;
    if (outputPath.isEmpty()) {
        QString app_data_path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

        QDir dir(app_data_path);
        if (!dir.exists()) {
            dir.mkpath(app_data_path);
        }

        output_file = tailPath(app_data_path) + CONTENT_HTML;
    } else {
        output_file = tailPath(outputPath) + CONTENT_HTML;
    }

    QFile file(output_file);
    if (file.exists())
    {
        file.remove();
    }

    if (file.open(QIODevice::WriteOnly))
    {
        file.write(html.toUtf8());
        file.close();
    }
    else {
        qDebug() << "Failed writing contents file";
        return;
    }

    if (showOnBrowser) {
        if (outputPath.isEmpty() && !QDesktopServices::openUrl(QUrl::fromLocalFile(output_file)))
        {
            qDebug() << "Failed showing table of contents in browser.";
            return;
        }
    }
}

void MainWindow::sync()
{
    qDebug() << "About to sync disk writing...";
    if (!m_isWritingToDisk)
        return;

    QProcess process;
    QStringList arguments;

#ifdef WIN32
    arguments.append("/accepteula");
    arguments.append("-r");
    arguments.append(m_cardPage->currentDrivePath().left(1));
#endif
    QApplication::setOverrideCursor(Qt::WaitCursor);    // hint to background action
    qApp->processEvents();

    process.start(SYNC_PATH, arguments);

    if (!process.waitForStarted())
    {
        m_dbgDlg->appendLog("- Sync failed. Failed to start.");
        process.close();
        m_isWritingToDisk = false;

        QApplication::restoreOverrideCursor();
        qApp->processEvents();

        return;
    }

    if (!process.waitForFinished(60000))
    {
        m_dbgDlg->appendLog("- Sync failed. Failed to complete.");
        process.close();
        m_isWritingToDisk = false;

        QApplication::restoreOverrideCursor();
        qApp->processEvents();

        return;
    }
    process.close();

    QApplication::restoreOverrideCursor();
    qApp->processEvents();

    qDebug() << "Sync is successful!";

    m_isWritingToDisk = false;
}

void MainWindow::collectInformationForSupport()
{

    QString output_file_path = QFileDialog::getExistingDirectory(this, tr("Select destination directory"), QString());
    if (output_file_path.isEmpty())
    {
        return;
    }

    QString collect_path = tailPath(HOERBERT_TEMP_PATH) + COLLECT_FILE_NAME;
    QString destination_path = tailPath(output_file_path) + COLLECT_FILE_NAME;
    bool doOverwrite = false;

    QDir tmp_dir(collect_path);
    if (tmp_dir.exists())
    {
        // remove the temp dir and its contents to work on a clean slate
        tmp_dir.removeRecursively();
    }

    QString tmpDirString = tmp_dir.absolutePath();
    if (!tmp_dir.mkpath(tmpDirString))
    {
        QMessageBox::information(this, tr("Collecting support information"), tr("Failed to create the temporary folder [%1] for collecting information.").arg( tmpDirString ) );
        return;
    }

    QString destinationZipFilename = destination_path+".zip";

    if (QFile::exists(destinationZipFilename))
    {
        auto overwriteAnswer = QMessageBox::question(this, tr("Collecting support information"), tr("The file [%1] already exists in that destination. Overwrite the file?").arg( destinationZipFilename ), QMessageBox::Yes|QMessageBox::Cancel, QMessageBox::Cancel );
        if (overwriteAnswer == QMessageBox::Yes)
        {
            doOverwrite = true;
        }
        else
        {
            return;
        }
    }

    qDebug() << "tmp dir: " << tmp_dir.absolutePath();

    QApplication::setOverrideCursor(Qt::WaitCursor);    // hint to background action
    qApp->processEvents();

    QStringList info_list;
    info_list << "[Operating System]" << QSysInfo::productType() << QSysInfo::productVersion() << QLocale::languageToString(QLocale::system().language()) << "";

    qint64 free_space_of_installation_drive = 0;
    for (auto info : QStorageInfo::mountedVolumes())
    {
        if (info.rootPath() == "/")
        {
            free_space_of_installation_drive = info.bytesFree();
            break;
        }
    }

    info_list << "[Free space of installed drive]" << QString("%1 bytes free").arg(free_space_of_installation_drive) << "";

    if( m_cardPage->currentDriveName().isEmpty() )
    {
        info_list << "[Memory card]" << QString("not selected");
    }
    else
    {
        auto volume_size = m_cardPage->getCurrentVolumeSize();
        auto available_size = m_cardPage->getCurrentAvailableSpace();
        info_list << "[Memory card]" << QString("%1 bytes, %2 bytes used, %3 bytes free.").arg(volume_size).arg(volume_size - available_size).arg(available_size);
        info_list << QString("Filesystem(%1)").arg(m_cardPage->getCardFileSystemType().replace("msdos", "FAT32")) << "";

        QString dir_path = m_cardPage->currentDrivePath();
        info_list <<  "[Path]" << dir_path << "";

        info_list << "[Content]";

        QDir dir(dir_path);
        if (dir.exists())
        {
            dir.setFilter(QDir::Dirs | QDir::Files | QDir::Hidden | QDir::NoSymLinks);
            dir.setSorting(QDir::Name);

            QFileInfoList list = dir.entryInfoList();
            for (auto fileInfo : list) {
                auto path = fileInfo.absoluteFilePath();
                if (fileInfo.fileName().startsWith("."))
                    continue;

                if (path.startsWith(dir_path)) { // simply wipe out wrong entries which is in upper layer
                    if (fileInfo.isDir())
                    {
                        QStringList sub_info_list;
                        info_list << QString("|+%1").arg(fileInfo.fileName());
                        recursivelyGetDirectoryContent(&sub_info_list, fileInfo.absoluteFilePath(), 1);
                        info_list << sub_info_list << "";
                    } else {
                        info_list << QString("|%1 [%2]").arg(fileInfo.fileName()).arg(fileInfo.size());
                    }
                }
            }
        }

        info_list << "";
        info_list << "[notes]";

        QString current_card_path = m_cardPage->currentDrivePath();
        QDir card_dir(current_card_path);
        if (card_dir.exists(HOERBERT_XML))
        {
            if (!QFile::copy(tailPath(current_card_path) + HOERBERT_XML, tailPath(collect_path) + HOERBERT_XML))
            {
                info_list << QString("Failed to copy [%1] from").arg(HOERBERT_XML) << current_card_path << "to" << collect_path;
            }
        }
        else
        {
            info_list << "No hoerbert.xml was found in the card root directory. That's ok as long as this card will never be used with app versions 1.x";
        }

        if (card_dir.exists(HOERBERT_XML_BACKUP))
        {
            if (!QFile::copy(tailPath(current_card_path) + HOERBERT_XML_BACKUP, tailPath(collect_path) + HOERBERT_XML_BACKUP))
            {
                info_list << QString("Failed to copy [%1] from").arg(HOERBERT_XML_BACKUP) << current_card_path << "to" << collect_path;
            }
        }
        else
        {
            info_list << "No hoerbert.bak was found in the card root directory. That's ok as long as this card will never be used with app versions 1.x";
        }

        if (card_dir.exists("info.xml"))
        {
            if (!QFile::copy(tailPath(current_card_path) + "info.xml", tailPath(collect_path) + "info.xml"))
            {
                info_list << "Failed to copy info.xml from" << current_card_path << "to" << collect_path;
            }
        }
        else
        {
            info_list << "No info.xml was found in the card root directory. That means that this card has never been backed up using hoerbert.app 2.x";
        }

        printTableOfContent(collect_path);
    }

    info_list << "";
    info_list << "[settings]";

    QSettings settings;
    settings.beginGroup("Global");

    QStringList keys = settings.allKeys();
    QStringListIterator it(keys);
    while ( it.hasNext() )
    {
        QString currentKey = it.next();
        QString newString = currentKey + "=" + settings.value(currentKey).toString();
        info_list << newString;
    }
    settings.endGroup();


    // write collected information to our file
    QFile file(tailPath(collect_path) + "dir.txt");
    if (file.open(QFile::WriteOnly)) {
        QTextStream stream(&file);
        stream << info_list.join("\n");
        file.close();
    }

    QProcess process;
    QStringList arguments;
    bool processSuccess = true;

    if( doOverwrite ){
        QFile::remove( destinationZipFilename );
    }

    process.setWorkingDirectory(HOERBERT_TEMP_PATH);
    QFile::remove( collect_path + ".zip" );         // remove our temp file if it should exist.

#if defined (Q_OS_MAC) || defined (Q_OS_LINUX)
    arguments << "-r" << collect_path + ".zip" << COLLECT_FILE_NAME+"/";
    qDebug() << "zip" << arguments.join(" ");

    process.start("/usr/bin/zip", arguments);
    if (!process.waitForFinished())
    {
        QMessageBox::information(this, tr("Collecting support information"), tr("Failed to create the zip file for support information") );
        processSuccess = false;
    }

#else

    arguments << "-tzip" << "a" << collect_path + ".zip" << tailPath(HOERBERT_TEMP_PATH) + COLLECT_FILE_NAME;
    qDebug() << "7za.exe" << arguments.join(" ");

    process.start(ZIP_PATH, arguments);

    if (!process.waitForStarted())
    {
        processSuccess = false;
        m_dbgDlg->appendLog("- Compressing zip failed. Failed to start.");
        m_isWritingToDisk = false;
    }

    if ( processSuccess && !process.waitForFinished(60000))
    {
        processSuccess = false;
        m_dbgDlg->appendLog("- Compressing zip failed. Failed to complete.");
        m_isWritingToDisk = false;
    }

#endif

    process.close();

    if ( processSuccess && !QFile::copy( collect_path + ".zip", destinationZipFilename))
    {
        perror("File copy");
        QMessageBox::information(this, tr("Collecting support information"), tr("Failed to copy the zip file from [%1] to: [%2]").arg( collect_path+".zip" ).arg( destinationZipFilename ) );
    }

    if (tmp_dir.exists())
    {
        tmp_dir.removeRecursively();
    }

    QApplication::restoreOverrideCursor();
    qApp->processEvents();
}

void MainWindow::backupCard()
{
    QString sourcePath = m_cardPage->currentDrivePath();

    if (sourcePath.isEmpty())
        return;

    QString destPath = QFileDialog::getExistingDirectory(this, tr("Please select the destination directory for this backup"), QString());
    if (destPath.isEmpty())
        return;

    QString backup_dir = tr("hoerbert_Backup_") + m_cardPage->currentDriveName().left(m_cardPage->currentDriveName().lastIndexOf(" ("))+ QString("_") + QDateTime::currentDateTime().toString("yyyyMMddHHmmss");

    QDir dir(destPath);
    if (!dir.mkdir(backup_dir))
    {
        m_dbgDlg->appendLog(tr("Cannot create subdirectory for backup"));
        return;
    }

    destPath = tailPath(destPath) + backup_dir;

    QApplication::setOverrideCursor(Qt::WaitCursor);    // hint to background action
    qApp->processEvents();

    printTableOfContent(destPath);

    m_backupManager = new BackupManager(sourcePath, destPath);
    m_backupManager->addStamp("app_version", VER_PRODUCTVERSION_STR);
    m_backupManager->addStamp("last_write_date", QDateTime::currentDateTime().toString( "yyyyMMddHHmmss" ));
    m_backupManager->addStamp("drive_name", m_cardPage->currentDriveName());
    m_backupManager->addStamp("by_whom", tr("hörbert Software"));

    m_progress = new QProgressDialog(this);
    m_progress->setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::CustomizeWindowHint);
    m_progress->setWindowTitle(tr("Backup"));
    m_progress->setModal(true);
    m_progress->setLabelText(tr("Copying files..."));
    m_progress->setRange(0, 100);
    m_progress->setFixedWidth(360);

    QPushButton *abort_button = new QPushButton(m_progress);
    abort_button->setText(tr("Abort"));

    // setting cancel button connects the button to the dialog to hide the dialog on button click
    m_progress->setCancelButton(abort_button);
    // need to disconnect the connections since clicking on the button immediately hides the dialog
    m_progress->disconnect(abort_button);

    // then define custom connections
    connect(abort_button, &QPushButton::clicked, this, [this, abort_button] () {
        m_progress->setLabelText(tr("Aborting..."));
        m_progress->show();
        QCoreApplication::processEvents();
        abort_button->setDisabled(true);
       m_backupManager->abort();
    });

    m_progress->show();

    connect(m_backupManager, &BackupManager::processUpdated, this, [this] (int percent) {
       m_progress->setValue(percent);
       m_progress->setLabelText(tr("Copying files..."));
       QCoreApplication::processEvents();
    });

    connect(m_backupManager, &BackupManager::failed, this, [this](const QString &errorString) {
        this->processorErrorOccurred("On backup\n" + errorString);
    });

    m_isWritingToDisk = true;

    m_progress->setCursor(Qt::WaitCursor);

    m_backupManager->process();

    abort_button->deleteLater();
    m_backupManager->deleteLater();

    sync();
    m_progress->close();
    m_progress->deleteLater();

    QApplication::restoreOverrideCursor();
    qApp->processEvents();
}

void MainWindow::restoreBackupQuestion()
{
    QString sourcePath = QFileDialog::getExistingDirectory(this, tr("Please select the backup directory you want to restore"), QString());
    if (sourcePath.isEmpty())
        return;

    sourcePath = tailPath(sourcePath);

    m_backupRestoreDialog = new BackupRestoreDialog();
    m_backupRestoreDialog->setSourcePath(sourcePath);
    m_backupRestoreDialog->setParent( this );
    m_backupRestoreDialog->setWindowFlags(Qt::Window | Qt::Dialog | Qt::WindowTitleHint | Qt::CustomizeWindowHint);
    m_backupRestoreDialog->setWindowModality(Qt::ApplicationModal);
    m_backupRestoreDialog->setModal(true);
    m_backupRestoreDialog->open();

    connect(m_backupRestoreDialog, &BackupRestoreDialog::mergeClicked, this, &MainWindow::doRestoreBackup );
    connect(m_backupRestoreDialog, &BackupRestoreDialog::overwriteClicked, this, &MainWindow::doRestoreBackup );
}

void MainWindow::doRestoreBackup(const QString &sourcePath, bool doMerge)
{

    bool restore_only = !doMerge;

    QApplication::setOverrideCursor(Qt::WaitCursor);    // hint to background action
    qApp->processEvents();

    QString destPath = m_cardPage->currentDrivePath();

    m_backupManager = new BackupManager(sourcePath, destPath, false, restore_only);

    m_progress = new QProgressDialog(this);
    m_progress->setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::CustomizeWindowHint);
    m_progress->setWindowTitle(tr("Restore"));
    m_progress->setModal(true);
    m_progress->setLabelText(tr("Restoring files..."));
    m_progress->setRange(0, 100);
    m_progress->setFixedWidth(360);

    QPushButton *abort_button = new QPushButton(m_progress);
    abort_button->setText(tr("Abort"));

    // setting cancel button connects the button to the dialog to hide the dialog on button click
    m_progress->setCancelButton(abort_button);
    // need to disconnect the connections since clicking on the button immediately hides the dialog
    m_progress->disconnect(abort_button);

    // then define custom connections
    connect(abort_button, &QPushButton::clicked, this, [this, abort_button] () {
        m_progress->setLabelText(tr("Aborting..."));
        m_progress->show();
        QCoreApplication::processEvents();
        abort_button->setDisabled(true);
        m_backupManager->abort();
    });

    m_progress->show();

    connect(m_backupManager, &BackupManager::processUpdated, this, [this] (int percent) {
        m_progress->setValue(percent);
        m_progress->setLabelText(tr("Restoring files..."));
        QCoreApplication::processEvents();
    });

    connect(m_backupManager, &BackupManager::failed, this, [this](const QString &errorString){
        this->processorErrorOccurred("On restoring backup\n" + errorString);
    });

    m_isWritingToDisk = true;

    m_backupManager->process();

    m_backupManager->deleteLater();
    abort_button->deleteLater();

    sync();
    m_progress->close();
    m_progress->deleteLater();
    m_cardPage->update();

    QApplication::restoreOverrideCursor();
    qApp->processEvents();
}

void MainWindow::formatCard()
{
    m_cardPage->formatSelectedDrive();
}

void MainWindow::advancedFeatures()
{
    m_featuresDlg->open();
    m_featuresDlg->readSettings();
}

void MainWindow::selectDestinationManually()
{
    QString default_path = QString();
#if defined (Q_OS_MAC)
    default_path = "/Volumes";
#elif defined (Q_OS_LINUX)
    default_path = "/media";
#endif
    QString dest_path = QFileDialog::getExistingDirectory(this, tr("Select destination"), default_path);
    if (!dest_path.isEmpty()) {
        m_cardPage->selectDriveByPath( dest_path );
    }
}

void MainWindow::switchDiagnosticsMode()
{
    bool doEnable = !m_cardPage->isDiagnosticsModeEnabled();

    if (doEnable)
    {
        m_toggleDiagnosticsModeAction->setChecked(true);
        // if on playlist page, discard changes and quit the page to card page
        if (m_stackWidget->currentIndex() == 1)
        {
            m_playlistPage->discard();
            QApplication::processEvents();
        }

        auto selected = QMessageBox::question(this, tr("Backup reminder"), tr("We recommend to back up your memory card to your computer before switching to diagnostics mode.\n"
                                                                              "Create a backup now?"), QMessageBox::Yes|QMessageBox::No, QMessageBox::No );
        if (selected == QMessageBox::Yes)
            backupCard();
    }
    else
    {
        m_toggleDiagnosticsModeAction->setChecked(false);
    }

    auto drive_path = m_cardPage->currentDrivePath();

    m_progress = new QProgressDialog(this);
    m_progress->setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::CustomizeWindowHint);
    m_progress->setModal(true);
    m_progress->setLabelText(tr("Switching..."));
    m_progress->setFixedWidth(360);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->show();
    QApplication::processEvents();

    QApplication::setOverrideCursor(Qt::WaitCursor);    // hint to background action
    qApp->processEvents();

    m_isWritingToDisk = true;

    if (doEnable) // switch to diagnostics mode
    {
        m_progress->setWindowTitle(tr("Switching to diagnostics mode"));

        // create "originals" folder to back up original files
        QDir dir(drive_path);
        if (!dir.exists(DIAGMODE_ORIGINALS_DIR))
        {
            if (!dir.mkdir(DIAGMODE_ORIGINALS_DIR))
            {
                qDebug() << "Failed creating originals directory on memory card";
            }
        }

        // now move original hoerbert files to "originals" folder
        for (int i = 0; i < 9; i++)
        {
            if (dir.exists(QString::number(i)))
            {
                if (!dir.rename(drive_path + QString::number(i), tailPath(drive_path + DIAGMODE_ORIGINALS_DIR) + QString::number(i)))
                    qDebug() << QString("Failed moving directory (%1)").arg(i);
            }
            m_progress->setValue(m_progress->value() + 5);
            QApplication::processEvents();
        }

        if (dir.exists(HOERBERT_XML))
        {
            if (!moveFile(drive_path + HOERBERT_XML, tailPath(drive_path + DIAGMODE_ORIGINALS_DIR) + HOERBERT_XML))
                qDebug() << "Failed moving hoerbert xml file";
        }

        if (dir.exists(HOERBERT_XML_BACKUP))
        {
            if (!moveFile(drive_path + HOERBERT_XML_BACKUP, tailPath(drive_path + DIAGMODE_ORIGINALS_DIR) + HOERBERT_XML_BACKUP))
                qDebug() << "Failed moving hoerbert xml backup file";
        }

        if (dir.exists(BACKUP_INFO_FILE))
        {
            if (!moveFile(drive_path + BACKUP_INFO_FILE, tailPath(drive_path + DIAGMODE_ORIGINALS_DIR) + BACKUP_INFO_FILE))
                qDebug() << "Failed moving backup info file";
        }

        if (dir.exists(CARD_INFO_FILE))
        {
            if (!moveFile(drive_path + CARD_INFO_FILE, tailPath(drive_path + DIAGMODE_ORIGINALS_DIR) + CARD_INFO_FILE))
                qDebug() << "Failed moving card info file";
        }

        m_progress->setValue(m_progress->value() + 5);
        QApplication::processEvents();

#if defined (Q_OS_WIN)
        auto DIAG_FILES_PATH = DIAG_FILES_PATH_WIN;
#elif defined (Q_OS_MACOS)
        auto DIAG_FILES_PATH = DIAG_FILES_PATH_MAC;
#elif defined (Q_OS_LINUX)
        auto DIAG_FILES_PATH = DIAG_FILES_PATH_LINUX;
#endif

        QDir diag_dir(tailPath(QCoreApplication::applicationDirPath()) + DIAG_FILES_PATH);
        if (!diag_dir.exists())
        {
            qDebug() << "Failed locating diagnostics files";
        }
        else
        {
            for (int i = 0; i < 9; i++)
            {
                if (diag_dir.exists(QString::number(i)))
                {
                    auto src = tailPath(diag_dir.absolutePath()) + QString::number(i);
                    auto dest = drive_path + QString::number(i);
                    if (!copyRecursively(src, dest))
                    {
                        qDebug() << QString("Failed copying diagnotcis folder (%1)").arg(i);
                    }
                }
                m_progress->setValue(m_progress->value() + 5);
                QApplication::processEvents();
            }
        }

        // generate diagmode file to mark the card as to be diagnostics mode
        QFile file(drive_path + DIAGMODE_FILE);
        if (!file.exists() && !file.open(QIODevice::WriteOnly))
            qDebug() << "Failed creating diagnostics mode file.";

        m_cardPage->switchDiagnosticsMode(true);
        m_progress->setValue(100);
        QApplication::processEvents();
    }
    else // returning to normal mode
    {
        m_progress->setWindowTitle(tr("Returning to normal mode"));

        QDir card_dir(drive_path);

        for (int i = 0; i < 9; i++)
        {
            QDir sub_dir(tailPath(card_dir.absolutePath()) + QString::number(i));
            if (sub_dir.exists())
            {                
                QFile file0(sub_dir.absoluteFilePath("0.WAV"));
                if (file0.exists())
                {
                    if (!file0.remove())
                    {
                        qDebug() << "Failed deleting diagnostics file. 0.WAV";
                    }
                }

                QFile file1(sub_dir.absoluteFilePath("1.WAV"));
                if (file1.exists())
                {
                    if (!file1.remove())
                    {
                        qDebug() << "Failed deleting diagnostics file. 1.WAV";
                    }
                }
            }
        }

        QFile file(drive_path + DIAGMODE_FILE);
        if (file.exists() && !file.remove())
        {
            qDebug() << "Failed removing diagnostics mode file.";
        }

        QDir dir(drive_path + DIAGMODE_ORIGINALS_DIR);
        if (!dir.exists())
        {
            QMessageBox::critical(this, "hörbert", tr("Cannot find original directories!"));
            return;
        }

        QString original_path = dir.absolutePath();

        for (int i = 0; i < 9; i++)
        {
            if (dir.exists(QString::number(i)))
            {
                QDir sub_dir(dir.absoluteFilePath(QString::number(i)));

                sub_dir.setFilter(QDir::Files | QDir::NoSymLinks);
                sub_dir.setNameFilters(QStringList() << "*" + DEFAULT_DESTINATION_FORMAT);

                QFileInfoList file_list;
                file_list << sub_dir.entryInfoList();
                std::sort(file_list.begin(), file_list.end(), sortByNumber);

                for (const auto& file_info : file_list)
                {
                    moveFile(file_info.absoluteFilePath(), tailPath(card_dir.absoluteFilePath(QString::number(i))) + file_info.fileName());
                }
            }
            m_progress->setValue(10 * (i + 1));
            QApplication::processEvents();
        }

        if (dir.exists(HOERBERT_XML))
        {
            if (!moveFile(tailPath(original_path) + HOERBERT_XML, drive_path + HOERBERT_XML))
                qDebug() << "Failed moving hoerbert xml file";
        }

        if (dir.exists(HOERBERT_XML_BACKUP))
        {
            if (!moveFile(tailPath(original_path) + HOERBERT_XML_BACKUP, drive_path + HOERBERT_XML_BACKUP))
                qDebug() << "Failed moving hoerbert xml backup file";
        }

        if (dir.exists(BACKUP_INFO_FILE))
        {
            if (!moveFile(tailPath(original_path) + BACKUP_INFO_FILE, drive_path + BACKUP_INFO_FILE))
                qDebug() << "Failed moving backup info file";
        }

        bool criticalError = false;
        if (dir.exists(CARD_INFO_FILE))
        {
            if (!moveFile(tailPath(original_path) + CARD_INFO_FILE, drive_path + CARD_INFO_FILE)){
                criticalError = true;                   // there's nothing we can do but stay in diagnostics mode.
                qDebug() << "Failed moving card info file";
            }
        }

        if( !criticalError ){
            if (!dir.removeRecursively())
            {
                qDebug() << "Failed removing empty original directory";
            }

            m_cardPage->switchDiagnosticsMode(false);
        }

        m_progress->setValue(100);
        QApplication::processEvents();
    }

    sync();

    m_progress->close();
    m_progress->deleteLater();
    QApplication::processEvents();

    m_printAction->setEnabled(!doEnable);
    m_backupAction->setEnabled(!doEnable);
    m_restoreAction->setEnabled(!doEnable);
    m_formatAction->setEnabled(!doEnable);

    QApplication::restoreOverrideCursor();
    qApp->processEvents();
}

void MainWindow::about()
{
    m_aboutDlg->open();
}

void MainWindow::checkForUpdates()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);    // hint to background action
    qApp->processEvents();

    QNetworkRequest request = QNetworkRequest(QUrl("https://www.hoerbert.com/client/checkVersion2"));
    QNetworkAccessManager *manager = new QNetworkAccessManager();

    connect(manager, &QNetworkAccessManager::finished, this, [this] (QNetworkReply *reply) {
        if (reply->error()) {
            qDebug() << "Failed retrieving version code:" << reply->errorString();

            QApplication::setOverrideCursor(Qt::ArrowCursor);
            qApp->processEvents();
            return;
        }

        QString result = reply->readAll();
        QString version = result.section(":", 1);

        this->showVersion(version);

        QApplication::restoreOverrideCursor();
        qApp->processEvents();
    });

    manager->get(request);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (m_cardPage->isProcessing())
    {
        auto selected = QMessageBox::question(this, "hörbert", QString(tr("Current drive [%1] is being processed.")+"\n\n"+tr("Are you sure you want to close this app?")).arg(m_cardPage->currentDriveName()), QMessageBox::Yes|QMessageBox::No, QMessageBox::No );

        if (selected == QMessageBox::No)
            return;
    }
    QMainWindow::closeEvent(e);
}

void MainWindow::showVersion(const QString &version)
{
    QDialog *dlg = new QDialog();
    dlg->setModal(true);
    dlg->setFixedSize(320, 198);

    QLabel *label = new QLabel(dlg);
    label->setText(tr("This version:")+" "+VER_PRODUCTVERSION_STR+"\n"+tr("Latest available version online: %1").arg(version));
    label->setAlignment(Qt::AlignCenter);

    QPushButton *download_button = new QPushButton(dlg);
    download_button->setText(tr("Download"));
    download_button->setDefault(false);

    connect(download_button, &QPushButton::clicked, this, [dlg] () {
        dlg->hide();
        dlg->deleteLater();
        QDesktopServices::openUrl(QUrl(tr("https://en.hoerbert.com/customer-service/downloads-and-questions-about-transferring-contents/")));
    });

    QPushButton *cancel_button = new QPushButton(dlg);
    cancel_button->setText(tr("Cancel"));
    cancel_button->setDefault(true);

    connect(cancel_button, &QPushButton::clicked, [dlg] () {
        dlg->hide();
        dlg->deleteLater();
    });

    QVBoxLayout *vlay = new QVBoxLayout(dlg);

    QHBoxLayout *button_layout = new QHBoxLayout;
    button_layout->addWidget(download_button);
    button_layout->addWidget(cancel_button);

    vlay->addWidget(label);
    vlay->addLayout(button_layout);

    dlg->show();
}

void MainWindow::remindBackup()
{
    QSettings settings;
    settings.beginGroup("Global");
    bool is_reminder_enabled = settings.value("showBackupReminder").toBool();
    settings.endGroup();

    if (!is_reminder_enabled)
        return;

    bool need_to_remind = false;
    QFile file(m_cardPage->currentDrivePath() + CARD_INFO_FILE);
    if (file.exists())
    {
        QDate last_modified_date = QFileInfo(file).lastModified().date();
        auto elapsed_days = last_modified_date.daysTo(QDate::currentDate());
        if (elapsed_days > 7)
            need_to_remind = true;
    }
    else
        need_to_remind = true;

    if (need_to_remind)
    {
        QDialog *dlg = new QDialog(this);
        dlg->setWindowTitle(tr("Backup reminder"));
        dlg->setFixedSize(640, 240);

        QLabel *text = new QLabel(dlg);
        text->setText( tr("Backup reminder: Don't forget to backup your memory card to your computer.")+"\n"+tr("This will keep you from losing files accidently. Do you want to create a backup now?") );

        QCheckBox *reminder_chk = new QCheckBox(dlg);
        reminder_chk->setText(tr("Never remind me again"));

        QPushButton *btn_yes = new QPushButton(dlg);
        btn_yes->setFixedWidth(100);
        btn_yes->setText(QString(tr("Yes")));

        QPushButton *btn_no = new QPushButton(dlg);
        btn_no->setFixedWidth(100);
        btn_no->setText(QString(tr("No")));

        QHBoxLayout *button_layout = new QHBoxLayout;
        button_layout->setSpacing(12);
        button_layout->setAlignment(Qt::AlignCenter);
        button_layout->addWidget(btn_yes);
        button_layout->addWidget(btn_no);

        QVBoxLayout *layout = new QVBoxLayout(dlg);
        layout->setContentsMargins(25, 25, 25, 25);
        layout->setAlignment(Qt::AlignHCenter);
        layout->setSpacing(30);

        layout->addWidget(text);
        layout->addWidget(reminder_chk, 0, Qt::AlignHCenter);
        layout->addLayout(button_layout);

        connect(reminder_chk, &QCheckBox::stateChanged, this, [] (int state) {
           if (state >= 1)
           {
               QSettings settings;
               settings.beginGroup("Global");
               settings.setValue("showBackupReminder", false);
               settings.endGroup();
           }
        });

        connect(btn_yes, &QPushButton::clicked, this, [this, dlg, text, reminder_chk, btn_yes, btn_no, layout, button_layout] () {
            dlg->accept();
            delete text;
            delete reminder_chk;
            delete btn_no;
            delete btn_yes;
            delete button_layout;
            delete layout;
            delete dlg;
            this->backupCard();
        });
        connect(btn_no, &QPushButton::clicked, this, [dlg, text, reminder_chk, btn_yes, btn_no, layout, button_layout] () {
            dlg->reject();
            delete text;
            delete reminder_chk;
            delete btn_no;
            delete btn_yes;
            delete button_layout;
            delete layout;
            delete dlg;
        });

        dlg->exec();
    }
}

void MainWindow::createActions()
{
    m_editMenu = new QMenu( tr("&Edit"), this );
    menuBar()->addMenu( m_editMenu );

    m_addTitleAction = new QAction(tr("&Add title"), this);
    m_addTitleAction->setStatusTip(tr("Add title"));
    m_addTitleAction->setEnabled(false);
    connect(m_addTitleAction, &QAction::triggered, this, &MainWindow::addTitle);
    m_editMenu->addAction(m_addTitleAction);

    m_removeTitleAction = new QAction(tr("&Remove title"), this);
    m_removeTitleAction->setStatusTip(tr("Remove title"));
    m_removeTitleAction->setEnabled(false);
    connect(m_removeTitleAction, &QAction::triggered, this, &MainWindow::removeTitle);
    m_editMenu->addAction(m_removeTitleAction);

    m_moveToPlaylistMenu = new QMenu(tr("&Move to playlist..."), this);
    m_moveToPlaylistMenu->setEnabled(false);
    m_editMenu->addMenu( m_moveToPlaylistMenu );

    m_subMenuBegin = new QMenu(tr("Beginning of..."), this);
    m_moveToPlaylistMenu->addMenu(m_subMenuBegin);

    QSettings settings;
    settings.beginGroup("Global");
    bool darkMode = settings.value("darkMode").toBool();
    settings.endGroup();

    m_moveToB1 = new QAction("1", this);
    m_moveToB1->setStatusTip(tr("Move to beginning of playlist %1").arg(1));
    if( darkMode )
    {
        m_moveToB1->setIcon(QIcon(":/images/colorblind_hint_01_dark.png"));
    }
    else
    {
        m_moveToB1->setIcon(QIcon(":/images/colorblind_hint_01.png"));
    }
    connect(m_moveToB1, &QAction::triggered, [this] () {
       this->moveToAnotherPlaylist(0, true);
    });
    m_subMenuBegin->addAction(m_moveToB1);

    m_moveToB2 = new QAction("2", this);
    m_moveToB2->setStatusTip(tr("Move to beginning of playlist %1").arg(2));
    if( darkMode )
    {
        m_moveToB2->setIcon(QIcon(":/images/colorblind_hint_02_dark.png"));
    }
    else
    {
        m_moveToB2->setIcon(QIcon(":/images/colorblind_hint_02.png"));
    }
    connect(m_moveToB2, &QAction::triggered, [this] () {
       this->moveToAnotherPlaylist(1, true);
    });
    m_subMenuBegin->addAction(m_moveToB2);

    m_moveToB3 = new QAction("3", this);
    m_moveToB3->setStatusTip(tr("Move to beginning of playlist %1").arg(3));
    if( darkMode )
    {
        m_moveToB3->setIcon(QIcon(":/images/colorblind_hint_03_dark.png"));
    }
    else
    {
        m_moveToB3->setIcon(QIcon(":/images/colorblind_hint_03.png"));
    }
    connect(m_moveToB3, &QAction::triggered, [this] () {
       this->moveToAnotherPlaylist(2, true);
    });
    m_subMenuBegin->addAction(m_moveToB3);

    m_moveToB4 = new QAction("4", this);
    m_moveToB4->setStatusTip(tr("Move to beginning of playlist %1").arg(4));
    if( darkMode )
    {
        m_moveToB4->setIcon(QIcon(":/images/colorblind_hint_04_dark.png"));
    }
    else
    {
        m_moveToB4->setIcon(QIcon(":/images/colorblind_hint_04.png"));
    }
    connect(m_moveToB4, &QAction::triggered, [this] () {
       this->moveToAnotherPlaylist(3, true);
    });
    m_subMenuBegin->addAction(m_moveToB4);

    m_moveToB5 = new QAction("5", this);
    m_moveToB5->setStatusTip(tr("Move to beginning of playlist %1").arg(5));
    if( darkMode )
    {
        m_moveToB5->setIcon(QIcon(":/images/colorblind_hint_05_dark.png"));
    }
    else
    {
        m_moveToB5->setIcon(QIcon(":/images/colorblind_hint_05.png"));
    }
    connect(m_moveToB5, &QAction::triggered, [this] () {
       this->moveToAnotherPlaylist(4, true);
    });
    m_subMenuBegin->addAction(m_moveToB5);

    m_moveToB6 = new QAction("6", this);
    m_moveToB6->setStatusTip(tr("Move to beginning of playlist %1").arg(6));
    if( darkMode )
    {
        m_moveToB6->setIcon(QIcon(":/images/colorblind_hint_06_dark.png"));
    }
    else
    {
        m_moveToB6->setIcon(QIcon(":/images/colorblind_hint_06.png"));
    }
    connect(m_moveToB6, &QAction::triggered, [this] () {
       this->moveToAnotherPlaylist(5, true);
    });
    m_subMenuBegin->addAction(m_moveToB6);

    m_moveToB7 = new QAction("7", this);
    m_moveToB7->setStatusTip(tr("Move to beginning of playlist %1").arg(7));
    if( darkMode )
    {
        m_moveToB7->setIcon(QIcon(":/images/colorblind_hint_07_dark.png"));
    }
    else
    {
        m_moveToB7->setIcon(QIcon(":/images/colorblind_hint_07.png"));
    }
    connect(m_moveToB7, &QAction::triggered, [this] () {
       this->moveToAnotherPlaylist(6, true);
    });
    m_subMenuBegin->addAction(m_moveToB7);

    m_moveToB8 = new QAction("8", this);
    m_moveToB8->setStatusTip(tr("Move to beginning of playlist %1").arg(8));
    if( darkMode )
    {
        m_moveToB8->setIcon(QIcon(":/images/colorblind_hint_08_dark.png"));
    }
    else
    {
        m_moveToB8->setIcon(QIcon(":/images/colorblind_hint_08.png"));
    }
    connect(m_moveToB8, &QAction::triggered, [this] () {
       this->moveToAnotherPlaylist(7, true);
    });
    m_subMenuBegin->addAction(m_moveToB8);

    m_moveToB9 = new QAction("9", this);
    m_moveToB9->setStatusTip(tr("Move to beginning of playlist %1").arg(9));
    if( darkMode )
    {
        m_moveToB9->setIcon(QIcon(":/images/colorblind_hint_09_dark.png"));
    }
    else
    {
        m_moveToB9->setIcon(QIcon(":/images/colorblind_hint_09.png"));
    }
    connect(m_moveToB9, &QAction::triggered, [this] () {
       this->moveToAnotherPlaylist(8, true);
    });
    m_subMenuBegin->addAction(m_moveToB9);


    m_subMenuEnd = new QMenu(tr("End of..."), this);
    m_moveToPlaylistMenu->addMenu(m_subMenuEnd);

    m_moveToE1 = new QAction("1", this);
    m_moveToE1->setStatusTip(tr("Move to end of playlist %1").arg(1));
    if( darkMode )
    {
        m_moveToE1->setIcon(QIcon(":/images/colorblind_hint_01_dark.png"));
    }
    else
    {
        m_moveToE1->setIcon(QIcon(":/images/colorblind_hint_01.png"));
    }
    connect(m_moveToE1, &QAction::triggered, [this] () {
       this->moveToAnotherPlaylist(0, false);
    });
    m_subMenuEnd->addAction(m_moveToE1);

    m_moveToE2 = new QAction("2", this);
    m_moveToE2->setStatusTip(tr("Move to end of playlist %1").arg(2));
    if( darkMode )
    {
        m_moveToE2->setIcon(QIcon(":/images/colorblind_hint_02_dark.png"));
    }
    else
    {
        m_moveToE2->setIcon(QIcon(":/images/colorblind_hint_02.png"));
    }
    connect(m_moveToE2, &QAction::triggered, [this] () {
       this->moveToAnotherPlaylist(1, false);
    });
    m_subMenuEnd->addAction(m_moveToE2);

    m_moveToE3 = new QAction("3", this);
    m_moveToE3->setStatusTip(tr("Move to end of playlist %1").arg(3));
    if( darkMode )
    {
        m_moveToE3->setIcon(QIcon(":/images/colorblind_hint_03_dark.png"));
    }
    else
    {
        m_moveToE3->setIcon(QIcon(":/images/colorblind_hint_03.png"));
    }
    connect(m_moveToE3, &QAction::triggered, [this] () {
       this->moveToAnotherPlaylist(2, false);
    });
    m_subMenuEnd->addAction(m_moveToE3);

    m_moveToE4 = new QAction("4", this);
    m_moveToE4->setStatusTip(tr("Move to end of playlist %1").arg(4));
    if( darkMode )
    {
        m_moveToE4->setIcon(QIcon(":/images/colorblind_hint_04_dark.png"));
    }
    else
    {
        m_moveToE4->setIcon(QIcon(":/images/colorblind_hint_04.png"));
    }
    connect(m_moveToE4, &QAction::triggered, [this] () {
       this->moveToAnotherPlaylist(3, false);
    });
    m_subMenuEnd->addAction(m_moveToE4);

    m_moveToE5 = new QAction("5", this);
    m_moveToE5->setStatusTip(tr("Move to end of playlist %1").arg(5));
    if( darkMode )
    {
        m_moveToE5->setIcon(QIcon(":/images/colorblind_hint_05_dark.png"));
    }
    else
    {
        m_moveToE5->setIcon(QIcon(":/images/colorblind_hint_05.png"));
    }
    connect(m_moveToE5, &QAction::triggered, [this] () {
       this->moveToAnotherPlaylist(4, false);
    });
    m_subMenuEnd->addAction(m_moveToE5);

    m_moveToE6 = new QAction("6", this);
    m_moveToE6->setStatusTip(tr("Move to end of playlist %1").arg(6));
    if( darkMode )
    {
        m_moveToE6->setIcon(QIcon(":/images/colorblind_hint_06_dark.png"));
    }
    else
    {
        m_moveToE6->setIcon(QIcon(":/images/colorblind_hint_06.png"));
    }
    connect(m_moveToE6, &QAction::triggered, [this] () {
       this->moveToAnotherPlaylist(5, false);
    });
    m_subMenuEnd->addAction(m_moveToE6);

    m_moveToE7 = new QAction("7", this);
    m_moveToE7->setStatusTip(tr("Move to end of playlist %1").arg(7));
    if( darkMode )
    {
        m_moveToE7->setIcon(QIcon(":/images/colorblind_hint_07_dark.png"));
    }
    else
    {
        m_moveToE7->setIcon(QIcon(":/images/colorblind_hint_07.png"));
    }
    connect(m_moveToE7, &QAction::triggered, [this] () {
       this->moveToAnotherPlaylist(6, false);
    });
    m_subMenuEnd->addAction(m_moveToE7);

    m_moveToE8 = new QAction("8", this);
    m_moveToE8->setStatusTip(tr("Move to end of playlist %1").arg(8));
    if( darkMode )
    {
        m_moveToE8->setIcon(QIcon(":/images/colorblind_hint_08_dark.png"));
    }
    else
    {
        m_moveToE8->setIcon(QIcon(":/images/colorblind_hint_08.png"));
    }
    connect(m_moveToE8, &QAction::triggered, [this] () {
       this->moveToAnotherPlaylist(7, false);
    });
    m_subMenuEnd->addAction(m_moveToE8);

    m_moveToE9 = new QAction("9", this);
    m_moveToE9->setStatusTip(tr("Move to end of playlist %1").arg(9));
    if( darkMode )
    {
        m_moveToE9->setIcon(QIcon(":/images/colorblind_hint_09_dark.png"));
    }
    else
    {
        m_moveToE9->setIcon(QIcon(":/images/colorblind_hint_09.png"));
    }
    connect(m_moveToE9, &QAction::triggered, [this] () {
       this->moveToAnotherPlaylist(8, false);
    });
    m_subMenuEnd->addAction(m_moveToE9);

/*
#if defined(Q_OS_MAC)
    m_subMenuBegin->setEnabled(false);
    m_subMenuEnd->setEnabled(false);
    m_subMenuBegin->menuAction()->setVisible(false);
    m_subMenuEnd->menuAction()->setVisible(false);
    m_moveToPlaylistMenu->menuAction()->setVisible(false);
#endif
*/

    m_viewMenu = new QMenu(tr("View"), this);
    menuBar()->addMenu(m_viewMenu);

    m_darkModeAction = new QAction(tr("Dark mode"), this);
    m_darkModeAction->setStatusTip(tr("Switch on or off dark mode"));
    m_darkModeAction->setEnabled(true);
    m_darkModeAction->setCheckable(true);
    connect(m_darkModeAction, &QAction::triggered, this, [this] () {
        QSettings settings;
        settings.beginGroup("Global");
        settings.setValue("darkMode", m_darkModeAction->isChecked());
        settings.endGroup();

        QMessageBox::information(this, tr("Dark mode switch"), tr("Please restart this app to see the change."));
    });

    {
        QSettings settings;
        settings.beginGroup("Global");
        bool darkMode = settings.value("darkMode").toBool();
        m_darkModeAction->setChecked( darkMode );
        settings.endGroup();
    }
    m_viewMenu->addAction(m_darkModeAction);


    m_subMenuColumns = new QMenu(tr("Playlist columns..."), this);
    m_viewMenu->addMenu(m_subMenuColumns);

    m_showAlbumAction = new QAction(tr("Show artist column in playlists"), this);
    m_showAlbumAction->setStatusTip(tr("Show artist column in playlists"));
    m_showAlbumAction->setEnabled(true);
    m_showAlbumAction->setCheckable(true);
    connect(m_showAlbumAction, &QAction::triggered, this, [this] () {
        QSettings settings;
        settings.beginGroup("Global");
        settings.setValue("album", m_showAlbumAction->isChecked() );
        settings.endGroup();

        changeAlbumColumnVisibility( m_showAlbumAction->isChecked() );
    });

    {
        QSettings settings;
        settings.beginGroup("Global");
        bool enabled = settings.value("album").toBool();
        settings.endGroup();
        m_showAlbumAction->setChecked(enabled);
    }
    m_subMenuColumns->addAction(m_showAlbumAction);


    m_showPathAction = new QAction(tr("Show comment column in playlists"), this);
    m_showPathAction->setStatusTip(tr("Show comment column in playlists"));
    m_showPathAction->setEnabled(true);
    m_showPathAction->setCheckable(true);
    connect(m_showPathAction, &QAction::triggered, this, [this] () {
        QSettings settings;
        settings.beginGroup("Global");
        settings.setValue("comment", m_showPathAction->isChecked() );
        settings.endGroup();

        changeCommentColumnVisibility( m_showPathAction->isChecked() );
    });

    {
        QSettings settings;
        settings.beginGroup("Global");
        bool enabled = settings.value("comment").toBool();
        settings.endGroup();
        m_showPathAction->setChecked(enabled);
    }
    m_subMenuColumns->addAction(m_showPathAction);



    m_extrasMenu = new QMenu(tr("E&xtras"), this);
    menuBar()->addMenu(m_extrasMenu);

    m_printAction = new QAction(tr("&Print table of contents"), this);
    m_printAction->setStatusTip(tr("Print table of contents"));
    m_printAction->setEnabled(false);
    connect(m_printAction, &QAction::triggered, this, [this] () {
        printTableOfContent();
    });
    m_extrasMenu->addAction(m_printAction);

    m_backupAction = new QAction(tr("&Backup memory card"), this);
    m_backupAction->setStatusTip(tr("Backup memory card"));
    m_backupAction->setEnabled(false);
    connect(m_backupAction, &QAction::triggered, this, &MainWindow::backupCard);
    m_extrasMenu->addAction(m_backupAction);

    m_restoreAction = new QAction(tr("&Restore a backup"), this);
    m_restoreAction->setStatusTip(tr("Restore a backup"));
    m_restoreAction->setEnabled(false);
    connect(m_restoreAction, &QAction::triggered, this, &MainWindow::restoreBackupQuestion);
    m_extrasMenu->addAction(m_restoreAction);

    m_extrasMenu->addSeparator();

    m_formatAction = new QAction(tr("&Format memory card"), this);
    m_formatAction->setStatusTip(tr("Format memory card"));
    m_formatAction->setEnabled(true);
    connect(m_formatAction, &QAction::triggered, this, &MainWindow::formatCard);
    m_extrasMenu->addAction(m_formatAction);

    m_advancedFeaturesAction = new QAction(tr("&Advanced features"), this);
    m_advancedFeaturesAction->setStatusTip(tr("Advanced features"));
    connect(m_advancedFeaturesAction, &QAction::triggered, this, &MainWindow::advancedFeatures);
    m_extrasMenu->addAction(m_advancedFeaturesAction);

    m_extrasMenu->addSeparator();

    m_selectManually = new QAction(tr("Select destination &manually"), this);
    m_selectManually->setStatusTip(tr("Select mount point of memory card manually"));
    m_selectManually->setEnabled(true);
#ifndef Q_OS_LINUX
    m_selectManually->setVisible(false);
#endif
    connect(m_selectManually, &QAction::triggered, this, &MainWindow::selectDestinationManually);
    m_extrasMenu->addAction(m_selectManually);

    m_helpMenu = new QMenu(tr("&Help"), this);
    menuBar()->addMenu(m_helpMenu);

    aboutAction = new QAction(tr("&About"), this);
    aboutAction->setStatusTip(tr("About"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::about);
    m_helpMenu->addAction(aboutAction);

    findBooksAction = new QAction(tr("&Find music and audio books"), this);
    findBooksAction->setStatusTip(tr("Find music and audio books"));
    connect(findBooksAction, &QAction::triggered, this, [=](){
        QDesktopServices::openUrl(QUrl(tr("https://en.hoerbert.com/contents/")));
    });
    m_helpMenu->addAction(findBooksAction);

    checkUpdatesAction = new QAction(tr("&Check for updates"), this);
    checkUpdatesAction->setStatusTip(tr("Check for updates"));
    connect(checkUpdatesAction, &QAction::triggered, this, &MainWindow::checkForUpdates);
    m_helpMenu->addAction(checkUpdatesAction);

    m_helpMenu->addSeparator();

    m_visitServiceWebsiteAction = new QAction(tr("Visit service web site"), this);
    m_visitServiceWebsiteAction->setStatusTip(tr("Visit service web site"));
    connect(m_visitServiceWebsiteAction, &QAction::triggered, this, [=](){
        QDesktopServices::openUrl(QUrl(tr("https://en.hoerbert.com/service/hoerbert-application")));
    });
    m_helpMenu->addAction(m_visitServiceWebsiteAction);

    m_serviceToolsMenu = new QMenu(tr("Service tools"), this);
    m_helpMenu->addMenu(m_serviceToolsMenu);

    m_collectDataAction = new QAction(tr("Collect data for service"), this);
    m_collectDataAction->setStatusTip(tr("Collect data for service"));
    connect(m_collectDataAction, &QAction::triggered, this, &MainWindow::collectInformationForSupport);
    m_serviceToolsMenu->addAction(m_collectDataAction);

    m_toggleDiagnosticsModeAction = new QAction(tr("Diagnostics mode"), this);
    m_toggleDiagnosticsModeAction->setStatusTip(tr("Switch card to diagnostics mode and back"));
    m_toggleDiagnosticsModeAction->setCheckable(true);
    m_toggleDiagnosticsModeAction->setEnabled(false);
    m_toggleDiagnosticsModeAction->setChecked(false);
    connect(m_toggleDiagnosticsModeAction, &QAction::triggered, this, &MainWindow::switchDiagnosticsMode);
    m_serviceToolsMenu->addAction(m_toggleDiagnosticsModeAction);
}

bool MainWindow::copyRecursively(const QString &sourceFolder, const QString &destFolder)
{
    bool success = false;
    QDir sourceDir(sourceFolder);

    if(!sourceDir.exists())
        return false;

    QDir destDir(destFolder);
    if(!destDir.exists())
        destDir.mkdir(destFolder);

    QStringList files = sourceDir.entryList(QDir::Files);
    for(int i = 0; i< files.count(); i++) {
        QString srcName = tailPath(sourceFolder) + files[i];
        QString destName = tailPath(destFolder) + files[i];
        success = QFile::copy(srcName, destName);
        if(!success)
            return false;
    }

    files.clear();
    files = sourceDir.entryList(QDir::AllDirs | QDir::NoDotAndDotDot);
    for(int i = 0; i< files.count(); i++)
    {
        QString srcName = tailPath(sourceFolder) + files[i];
        QString destName = tailPath(destFolder) + files[i];
        success = copyRecursively(srcName, destName);
        if(!success)
            return false;
    }

    return true;
}

void MainWindow::showHideEditMenuEntries( bool showHide )
{
    if( showHide )
    {
        m_addTitleAction->setEnabled(true);
        m_removeTitleAction->setEnabled(true);

#if defined(Q_OS_MAC)
        m_subMenuBegin->setEnabled(true);
        m_subMenuEnd->setEnabled(true);

        m_subMenuBegin->menuAction()->setEnabled(true);
        m_subMenuEnd->menuAction()->setEnabled(true);
//        m_moveToPlaylistMenu->menuAction()->setVisible(true);
#endif
        m_moveToPlaylistMenu->menuAction()->setEnabled(true);
        m_moveToPlaylistMenu->menuAction()->setDisabled(false);
        m_moveToPlaylistMenu->setEnabled(true);
        m_moveToPlaylistMenu->setDisabled( false );

        m_printAction->setEnabled(false);
        m_backupAction->setEnabled(false);
        m_restoreAction->setEnabled(false);
        m_formatAction->setEnabled(false);
        m_advancedFeaturesAction->setEnabled(false);
        m_toggleDiagnosticsModeAction->setEnabled(false);

    }
    else
    {
        m_addTitleAction->setEnabled(false);
        m_removeTitleAction->setEnabled(false);

#if defined(Q_OS_MAC)
        m_subMenuBegin->setEnabled(false);
        m_subMenuEnd->setEnabled(false);
        m_subMenuBegin->menuAction()->setEnabled(false);
        m_subMenuEnd->menuAction()->setEnabled(false);
//        m_moveToPlaylistMenu->menuAction()->setVisible(false);
#endif
        m_moveToPlaylistMenu->menuAction()->setEnabled(false);
        m_moveToPlaylistMenu->menuAction()->setDisabled(true);
        m_moveToPlaylistMenu->setEnabled(false);
        m_moveToPlaylistMenu->setDisabled( true );

        m_printAction->setEnabled(true);
        m_backupAction->setEnabled(true);
        m_restoreAction->setEnabled(true);
        m_formatAction->setEnabled(true);
        m_advancedFeaturesAction->setEnabled(false);

        m_toggleDiagnosticsModeAction->setEnabled(true);

    }
}

