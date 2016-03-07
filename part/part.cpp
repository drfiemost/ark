/*
 * ark -- archiver for the KDE project
 *
 * Copyright (C) 2007 Henrique Pinto <henrique.pinto@kdemail.net>
 * Copyright (C) 2008-2009 Harald Hvaal <haraldhv@stud.ntnu.no>
 * Copyright (C) 2009-2012 Raphael Kubo da Costa <rakuco@FreeBSD.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include "part.h"
#include "ark_debug.h"
#include "archivemodel.h"
#include "archiveview.h"
#include "arkviewer.h"
#include "dnddbusinterfaceadaptor.h"
#include "infopanel.h"
#include "jobtracker.h"
#include "kerfuffle/archive_kerfuffle.h"
#include "kerfuffle/extractiondialog.h"
#include "kerfuffle/extractionsettingspage.h"
#include "kerfuffle/jobs.h"
#include "kerfuffle/settings.h"
#include "kerfuffle/previewsettingspage.h"

#include <KAboutData>
#include <KActionCollection>
#include <KConfigGroup>
#include <KGuiItem>
#include <KIO/Job>
#include <KJobWidgets>
#include <KIO/StatJob>
#include <KMessageBox>
#include <KPluginFactory>
#include <KRun>
#include <KSelectAction>
#include <KStandardGuiItem>
#include <KToggleAction>
#include <KLocalizedString>
#include <KXMLGUIFactory>

#include <QAction>
#include <QCursor>
#include <QHeaderView>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QScopedPointer>
#include <QPointer>
#include <QSplitter>
#include <QTimer>
#include <QtDBus/QtDBus>
#include <QFileDialog>
#include <QIcon>
#include <QInputDialog>
#include <QFileSystemWatcher>
#include <QGroupBox>
#include <QPlainTextEdit>

using namespace Kerfuffle;

K_PLUGIN_FACTORY(Factory, registerPlugin<Ark::Part>();)

namespace Ark
{

static quint32 s_instanceCounter = 1;

Part::Part(QWidget *parentWidget, QObject *parent, const QVariantList& args)
        : KParts::ReadWritePart(parent),
          m_splitter(Q_NULLPTR),
          m_busy(false),
          m_jobTracker(Q_NULLPTR)
{
    Q_UNUSED(args)
    setComponentData(*createAboutData(), false);

    new DndExtractAdaptor(this);

    const QString pathName = QStringLiteral("/DndExtract/%1").arg(s_instanceCounter++);
    if (!QDBusConnection::sessionBus().registerObject(pathName, this)) {
        qCCritical(ARK) << "Could not register a D-Bus object for drag'n'drop";
    }

    // m_vlayout is needed for later insertion of QMessageWidget
    QWidget *mainWidget = new QWidget;
    m_vlayout = new QVBoxLayout;
    m_model = new ArchiveModel(pathName, this);
    m_splitter = new QSplitter(Qt::Horizontal, parentWidget);
    m_view = new ArchiveView;
    m_infoPanel = new InfoPanel(m_model);

    // Add widgets for the comment field.
    m_commentView = new QPlainTextEdit();
    m_commentView->setReadOnly(true);
    m_commentView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_commentBox = new QGroupBox(i18n("Comment"));
    m_commentBox->hide();
    QVBoxLayout *vbox = new QVBoxLayout;
    vbox->addWidget(m_commentView);
    m_commentBox->setLayout(vbox);

    setWidget(mainWidget);
    mainWidget->setLayout(m_vlayout);

    // Configure the QVBoxLayout and add widgets
    m_vlayout->setContentsMargins(0,0,0,0);
    m_vlayout->addWidget(m_splitter);

    // Vertical QSplitter for the file view and comment field.
    m_commentSplitter = new QSplitter(Qt::Vertical, parentWidget);
    m_commentSplitter->setOpaqueResize(false);
    m_commentSplitter->addWidget(m_view);
    m_commentSplitter->addWidget(m_commentBox);
    m_commentSplitter->setCollapsible(0, false);

    // Horizontal QSplitter for the file view and infopanel.
    m_splitter->addWidget(m_commentSplitter);
    m_splitter->addWidget(m_infoPanel);

    // Read settings from config file and show/hide infoPanel.
    if (!ArkSettings::showInfoPanel()) {
        m_infoPanel->hide();
    } else {
        m_splitter->setSizes(ArkSettings::splitterSizes());
    }

    setupView();
    setupActions();

    connect(m_model, &ArchiveModel::loadingStarted,
            this, &Part::slotLoadingStarted);
    connect(m_model, &ArchiveModel::loadingFinished,
            this, &Part::slotLoadingFinished);
    connect(m_model, SIGNAL(droppedFiles(QStringList,QString)),
            this, SLOT(slotAddFiles(QStringList,QString)));
    connect(m_model, &ArchiveModel::error,
            this, &Part::slotError);

    connect(this, &Part::busy,
            this, &Part::setBusyGui);
    connect(this, &Part::ready,
            this, &Part::setReadyGui);
    connect(this, SIGNAL(completed()),
            this, SLOT(setFileNameFromArchive()));

    m_statusBarExtension = new KParts::StatusBarExtension(this);

    setXMLFile(QStringLiteral("ark_part.rc"));
}

Part::~Part()
{
    qDeleteAll(m_tmpOpenDirList);

    // Only save splitterSizes if infopanel is visible,
    // because we don't want to store zero size for infopanel.
    if (m_showInfoPanelAction->isChecked()) {
        ArkSettings::setSplitterSizes(m_splitter->sizes());
    }
    ArkSettings::setShowInfoPanel(m_showInfoPanelAction->isChecked());
    ArkSettings::self()->save();

    m_extractArchiveAction->menu()->deleteLater();
    m_extractFilesAction->menu()->deleteLater();
    m_toolbarExtractAction->menu()->deleteLater();
}

KAboutData *Part::createAboutData()
{
    return new KAboutData(QStringLiteral("ark"),
                          i18n("ArkPart"),
                          QStringLiteral("3.0"));
}

void Part::registerJob(KJob* job)
{
    if (!m_jobTracker) {
        m_jobTracker = new JobTracker(widget());
        m_statusBarExtension->addStatusBarItem(m_jobTracker->widget(0), 0, true);
        m_jobTracker->widget(job)->show();
    }
    m_jobTracker->registerJob(job);

    emit busy();
    connect(job, &KJob::result, this, &Part::ready);
}

// TODO: KIO::mostLocalHere is used here to resolve some KIO URLs to local
// paths (e.g. desktop:/), but more work is needed to support extraction
// to non-local destinations. See bugs #189322 and #204323.
void Part::extractSelectedFilesTo(const QString& localPath)
{
    if (!m_model) {
        return;
    }

    const QUrl url = QUrl::fromUserInput(localPath, QString());
    KIO::StatJob* statJob = nullptr;

    // Try to resolve the URL to a local path.
    if (!url.isLocalFile() && !url.scheme().isEmpty()) {
        statJob = KIO::mostLocalUrl(url);

        if (!statJob->exec() || statJob->error() != 0) {
            return;
        }
    }

    const QString destination = statJob ? statJob->statResult().stringValue(KIO::UDSEntry::UDS_LOCAL_PATH) : localPath;
    delete statJob;

    // The URL could not be resolved to a local path.
    if (!url.isLocalFile() && destination.isEmpty()) {
        qCWarning(ARK) << "Ark cannot extract to non-local destination:" << localPath;
        KMessageBox::sorry(widget(), xi18nc("@info", "Ark can only extract to local destinations."));
        return;
    }

    qCDebug(ARK) << "Extract to" << destination;

    Kerfuffle::ExtractionOptions options;
    options[QStringLiteral("PreservePaths")] = true;
    options[QStringLiteral("RemoveRootNode")] = true;
    options[QStringLiteral("DragAndDrop")] = true;

    // Create and start the ExtractJob.
    ExtractJob *job = m_model->extractFiles(filesAndRootNodesForIndexes(addChildren(m_view->selectionModel()->selectedRows())), destination, options);
    registerJob(job);
    connect(job, &KJob::result,
            this, &Part::slotExtractionDone);
    job->start();
}

void Part::setupView()
{
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);

    m_view->setModel(m_model);

    m_view->setSortingEnabled(true);

    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &Part::updateActions);
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &Part::selectionChanged);

    connect(m_view, &QTreeView::activated, this, &Part::slotActivated);

    connect(m_view, &QWidget::customContextMenuRequested, this, &Part::slotShowContextMenu);

    connect(m_model, &QAbstractItemModel::columnsInserted,
            this, &Part::adjustColumns);
}

void Part::slotActivated(QModelIndex)
{
    // The activated signal is emitted when items are selected with the mouse,
    // so do nothing if CTRL or SHIFT key is pressed.
    if (QGuiApplication::keyboardModifiers() != Qt::ShiftModifier &&
        QGuiApplication::keyboardModifiers() != Qt::ControlModifier) {
        ArkSettings::defaultOpenAction() == ArkSettings::EnumDefaultOpenAction::Preview ? slotOpenEntry(Preview) : slotOpenEntry(OpenFile);
    }
}

void Part::setupActions()
{
    // We use a QSignalMapper for the preview, open and openwith actions. This
    // way we can connect all three actions to the same slot slotOpenEntry and
    // pass the OpenFileMode as argument to the slot.
    m_signalMapper = new QSignalMapper;

    m_showInfoPanelAction = new KToggleAction(i18nc("@action:inmenu", "Show information panel"), this);
    actionCollection()->addAction(QStringLiteral( "show-infopanel" ), m_showInfoPanelAction);
    m_showInfoPanelAction->setChecked(ArkSettings::showInfoPanel());
    connect(m_showInfoPanelAction, &QAction::triggered,
            this, &Part::slotToggleInfoPanel);

    m_saveAsAction = actionCollection()->addAction(KStandardAction::SaveAs, QStringLiteral("ark_file_save_as"), this, SLOT(slotSaveAs()));

    m_openFileAction = actionCollection()->addAction(QStringLiteral("openfile"));
    m_openFileAction->setText(i18nc("open a file with external program", "&Open"));
    m_openFileAction->setIcon(QIcon::fromTheme(QStringLiteral("document-open")));
    m_openFileAction->setToolTip(i18nc("@info:tooltip", "Click to open the selected file with the associated application"));
    connect(m_openFileAction, SIGNAL(triggered(bool)), m_signalMapper, SLOT(map()));
    m_signalMapper->setMapping(m_openFileAction, OpenFile);

    m_openFileWithAction = actionCollection()->addAction(QStringLiteral("openfilewith"));
    m_openFileWithAction->setText(i18nc("open a file with external program", "Open &With..."));
    m_openFileWithAction->setIcon(QIcon::fromTheme(QStringLiteral("document-open")));
    m_openFileWithAction->setToolTip(i18nc("@info:tooltip", "Click to open the selected file with an external program"));
    connect(m_openFileWithAction, SIGNAL(triggered(bool)), m_signalMapper, SLOT(map()));
    m_signalMapper->setMapping(m_openFileWithAction, OpenFileWith);

    m_previewAction = actionCollection()->addAction(QStringLiteral("preview"));
    m_previewAction->setText(i18nc("to preview a file inside an archive", "Pre&view"));
    m_previewAction->setIcon(QIcon::fromTheme(QStringLiteral("document-preview-archive")));
    m_previewAction->setToolTip(i18nc("@info:tooltip", "Click to preview the selected file"));
    actionCollection()->setDefaultShortcut(m_previewAction, Qt::CTRL + Qt::Key_P);
    connect(m_previewAction, SIGNAL(triggered(bool)), m_signalMapper, SLOT(map()));
    m_signalMapper->setMapping(m_previewAction, Preview);

    m_extractArchiveAction = actionCollection()->addAction(QStringLiteral("extract"));
    m_extractArchiveAction->setText(i18nc("@action:inmenu", "E&xtract"));
    m_extractArchiveAction->setIcon(QIcon::fromTheme(QStringLiteral("archive-extract")));
    m_extractArchiveAction->setToolTip(i18n("Click to open an extraction dialog, where you can choose how to extract all the files in the archive"));
    connect(m_extractArchiveAction, &QAction::triggered,
            this, &Part::slotExtractArchive);

    m_extractFilesAction = actionCollection()->addAction(QStringLiteral("extract_files"));
    m_extractFilesAction->setText(i18nc("@action:inmenu", "E&xtract"));
    m_extractFilesAction->setIcon(QIcon::fromTheme(QStringLiteral("archive-extract")));
    m_extractFilesAction->setToolTip(i18n("Click to open an extraction dialog, where you can choose how to extract the selected files"));
    connect(m_extractFilesAction, &QAction::triggered,
            this, &Part::slotShowExtractionDialog);

    m_toolbarExtractAction = actionCollection()->addAction(QStringLiteral("toolbar_extract"));
    m_toolbarExtractAction->setText(i18nc("@action:intoolbar", "E&xtract"));
    m_toolbarExtractAction->setIcon(QIcon::fromTheme(QStringLiteral("archive-extract")));
    m_toolbarExtractAction->setToolTip(i18n("Click to open an extraction dialog, where you can choose to extract either all files or just the selected ones"));
    actionCollection()->setDefaultShortcut(m_toolbarExtractAction, Qt::CTRL + Qt::Key_E);
    connect(m_toolbarExtractAction, &QAction::triggered,
            this, &Part::slotShowExtractionDialog);

    m_addFilesAction = actionCollection()->addAction(QStringLiteral("add"));
    m_addFilesAction->setIcon(QIcon::fromTheme(QStringLiteral("archive-insert")));
    m_addFilesAction->setText(i18n("Add &File..."));
    m_addFilesAction->setToolTip(i18nc("@info:tooltip", "Click to add files to the archive"));
    connect(m_addFilesAction, SIGNAL(triggered(bool)),
            this, SLOT(slotAddFiles()));

    m_addDirAction = actionCollection()->addAction(QStringLiteral("add-dir"));
    m_addDirAction->setIcon(QIcon::fromTheme(QStringLiteral("archive-insert-directory")));
    m_addDirAction->setText(i18n("Add Fo&lder..."));
    m_addDirAction->setToolTip(i18nc("@info:tooltip", "Click to add a folder to the archive"));
    connect(m_addDirAction, &QAction::triggered,
            this, &Part::slotAddDir);

    m_deleteFilesAction = actionCollection()->addAction(QStringLiteral("delete"));
    m_deleteFilesAction->setIcon(QIcon::fromTheme(QStringLiteral("archive-remove")));
    m_deleteFilesAction->setText(i18n("De&lete"));
    actionCollection()->setDefaultShortcut(m_deleteFilesAction, Qt::Key_Delete);
    m_deleteFilesAction->setToolTip(i18nc("@info:tooltip", "Click to delete the selected files"));
    connect(m_deleteFilesAction, &QAction::triggered,
            this, &Part::slotDeleteFiles);

    connect(m_signalMapper, SIGNAL(mapped(int)), this, SLOT(slotOpenEntry(int)));

    updateActions();
}

void Part::updateActions()
{
    bool isWritable = m_model->archive() && !m_model->archive()->isReadOnly();
    bool isDirectory = m_model->entryForIndex(m_view->selectionModel()->currentIndex())[IsDirectory].toBool();
    int selectedEntriesCount = m_view->selectionModel()->selectedRows().count();

    // Figure out if entry size is larger than preview size limit.
    const int maxPreviewSize = ArkSettings::previewFileSizeLimit() * 1024 * 1024;
    const bool limit = ArkSettings::limitPreviewFileSize();
    const qlonglong size = m_model->entryForIndex(m_view->selectionModel()->currentIndex())[Size].toLongLong();
    bool isPreviewable = (!limit || (limit && size < maxPreviewSize));

    m_previewAction->setEnabled(!isBusy() &&
                                isPreviewable &&
                                !isDirectory &&
                                (selectedEntriesCount == 1));
    m_extractArchiveAction->setEnabled(!isBusy() &&
                                       (m_model->rowCount() > 0));
    m_extractFilesAction->setEnabled(!isBusy() &&
                                     (m_model->rowCount() > 0) &&
                                     (selectedEntriesCount > 0));
    m_toolbarExtractAction->setEnabled(!isBusy() &&
                                       (m_model->rowCount() > 0));
    m_saveAsAction->setEnabled(!isBusy() &&
                               m_model->rowCount() > 0);
    m_addFilesAction->setEnabled(!isBusy() &&
                                 isWritable);
    m_addDirAction->setEnabled(!isBusy() &&
                               isWritable);
    m_deleteFilesAction->setEnabled(!isBusy() &&
                                    isWritable &&
                                    (selectedEntriesCount > 0));
    m_openFileAction->setEnabled(!isBusy() &&
                                 isPreviewable &&
                                 !isDirectory &&
                                 (selectedEntriesCount == 1));
    m_openFileWithAction->setEnabled(!isBusy() &&
                                     isPreviewable &&
                                     !isDirectory &&
                                     (selectedEntriesCount == 1));

    // TODO: why do we even update these menus here?
    // It should be enough to update them when a new dir is appended to the history.
    updateQuickExtractMenu(m_extractArchiveAction);
    updateQuickExtractMenu(m_extractFilesAction);
    updateQuickExtractMenu(m_toolbarExtractAction);
}

void Part::updateQuickExtractMenu(QAction *extractAction)
{
    if (!extractAction) {
        return;
    }

    QMenu *menu = extractAction->menu();

    if (!menu) {
        menu = new QMenu();
        extractAction->setMenu(menu);
        connect(menu, &QMenu::triggered,
                this, &Part::slotQuickExtractFiles);

        // Remember to keep this action's properties as similar to
        // extractAction's as possible (except where it does not make
        // sense, such as the text or the shortcut).
        QAction *extractTo = menu->addAction(i18n("Extract To..."));
        extractTo->setIcon(extractAction->icon());
        extractTo->setToolTip(extractAction->toolTip());

        if (extractAction == m_extractArchiveAction) {
            connect(extractTo, &QAction::triggered,
                    this, &Part::slotExtractArchive);
        } else {
            connect(extractTo, &QAction::triggered,
                    this, &Part::slotShowExtractionDialog);
        }

        menu->addSeparator();

        QAction *header = menu->addAction(i18n("Quick Extract To..."));
        header->setEnabled(false);
        header->setIcon(QIcon::fromTheme(QStringLiteral("archive-extract")));
    }

    while (menu->actions().size() > 3) {
        menu->removeAction(menu->actions().last());
    }

    const KConfigGroup conf(KSharedConfig::openConfig(), "DirSelect Dialog");
    const QStringList dirHistory = conf.readPathEntry("History Items", QStringList());

    for (int i = 0; i < qMin(10, dirHistory.size()); ++i) {
        const QString dir = QUrl(dirHistory.value(i)).toString(QUrl::RemoveScheme | QUrl::NormalizePathSegments | QUrl::PreferLocalFile);
        QAction *newAction = menu->addAction(dir);
        newAction->setData(dir);
    }
}

void Part::slotQuickExtractFiles(QAction *triggeredAction)
{
    // #190507: triggeredAction->data.isNull() means it's the "Extract to..."
    //          action, and we do not want it to run here
    if (!triggeredAction->data().isNull()) {
        const QString userDestination = triggeredAction->data().toString();
        qCDebug(ARK) << "Extract to user dest" << userDestination;
        QString finalDestinationDirectory;
        const QString detectedSubfolder = detectSubfolder();
        qCDebug(ARK) << "Detected subfolder" << detectedSubfolder;

        if (!isSingleFolderArchive()) {
            finalDestinationDirectory = userDestination +
                                        QDir::separator() + detectedSubfolder;
            QDir(userDestination).mkdir(detectedSubfolder);
        } else {
            finalDestinationDirectory = userDestination;
        }

        qCDebug(ARK) << "Extract to final dest" << finalDestinationDirectory;

        Kerfuffle::ExtractionOptions options;
        options[QStringLiteral("PreservePaths")] = true;
        QList<QVariant> files = filesAndRootNodesForIndexes(m_view->selectionModel()->selectedRows());
        ExtractJob *job = m_model->extractFiles(files, finalDestinationDirectory, options);
        registerJob(job);

        connect(job, &KJob::result,
                this, &Part::slotExtractionDone);

        job->start();
    }
}

void Part::selectionChanged()
{
    m_infoPanel->setIndexes(m_view->selectionModel()->selectedRows());
}

bool Part::openFile()
{
    qCDebug(ARK) << "Attempting to open archive" << localFilePath();

    const QString localFile(localFilePath());
    const QFileInfo localFileInfo(localFile);
    const bool creatingNewArchive =
        arguments().metaData()[QStringLiteral("createNewArchive")] == QLatin1String("true");

    if (localFileInfo.isDir()) {
        KMessageBox::error(widget(), xi18nc("@info",
                                            "<filename>%1</filename> is a directory.",
                                            localFile));
        return false;
    }

    if (creatingNewArchive) {
        if (localFileInfo.exists()) {
            int overwrite =  KMessageBox::questionYesNo(widget(), xi18nc("@info", "The archive <filename>%1</filename> already exists. Would you like to open it instead?", localFile), i18nc("@title:window", "File Exists"), KGuiItem(i18n("Open File")), KStandardGuiItem::cancel());

            if (overwrite == KMessageBox::No) {
                return false;
            }
        }
    } else {
        if (!localFileInfo.exists()) {
            KMessageBox::sorry(widget(), xi18nc("@info", "The archive <filename>%1</filename> was not found.", localFile), i18nc("@title:window", "Error Opening Archive"));
            return false;
        }

        if (!localFileInfo.isReadable()) {
            displayMsgWidget(KMessageWidget::Warning, xi18nc("@info", "The archive <filename>%1</filename> could not be loaded, as it was not possible to read from it.", localFile));
            return false;
        }
    }

    QScopedPointer<Kerfuffle::Archive> archive(Kerfuffle::Archive::create(localFile, m_model));

    Q_ASSERT(archive);

    if (archive->error() == NoPlugin) {
        KMessageBox::sorry(widget(),
                           xi18nc("@info", "Ark was not able to open <filename>%1</filename>. No suitable plugin found."
                                  "<nl/><nl/>Ark does not seem to support this file type.",
                                  localFile.section(QLatin1Char('/'), -1)),
                           i18nc("@title:window", "Error Opening Archive"));
        return false;
    } else if (archive->error() == FailedPlugin) {
        KMessageBox::sorry(widget(),
                           xi18nc("@info", "Ark was not able to open <filename>%1</filename>. Failed to load a suitable plugin."
                                  "<nl/><nl/>Make sure any executables needed to handle the archive type are installed.",
                                  localFile.section(QLatin1Char('/'), -1)),
                           i18nc("@title:window", "Error Opening Archive"));
        return false;
    }

    Q_ASSERT(archive->isValid());

    // Plugin loaded successfully.
    KJob *job = m_model->setArchive(archive.take(), localFileInfo.exists());
    if (job) {
        registerJob(job);
        job->start();
    } else {
        updateActions();
    }

    m_infoPanel->setIndex(QModelIndex());

    if (arguments().metaData()[QStringLiteral("showExtractDialog")] == QLatin1String("true")) {
        QTimer::singleShot(0, this, &Part::slotShowExtractionDialog);
    }

    const QString password = arguments().metaData()[QStringLiteral("encryptionPassword")];
    if (!password.isEmpty()) {
        m_model->setPassword(password);

        if (arguments().metaData()[QStringLiteral("encryptHeader")] == QLatin1String("true")) {
            m_model->enableHeaderEncryption();
        }
    }

    return true;
}

bool Part::saveFile()
{
    return true;
}

bool Part::isBusy() const
{
    return m_busy;
}

KConfigSkeleton *Part::config() const
{
    return ArkSettings::self();
}

QList<Kerfuffle::SettingsPage*> Part::settingsPages(QWidget *parent) const
{
    QList<SettingsPage*> pages;
    pages.append(new ExtractionSettingsPage(parent, i18n("Extraction settings"), QStringLiteral("archive-extract")));
    pages.append(new PreviewSettingsPage(parent, i18n("Preview settings"), QStringLiteral("document-preview-archive")));

    return pages;
}

void Part::slotLoadingStarted()
{
}

void Part::slotLoadingFinished(KJob *job)
{
    if (job->error()) {
        if (arguments().metaData()[QStringLiteral("createNewArchive")] != QLatin1String("true")) {
            if (job->error() != KJob::KilledJobError) {
                KMessageBox::error(widget(),
                                   xi18nc("@info", "Loading the archive <filename>%1</filename> failed with the following error:<nl/><nl/><message>%2</message>",
                                          localFilePath(),
                                          job->errorText()),
                                   i18nc("@title:window", "Error Opening Archive"));
            }

            // The file failed to open, so reset the open archive, info panel and caption.
            m_model->setArchive(Q_NULLPTR);

            m_infoPanel->setPrettyFileName(QString());
            m_infoPanel->updateWithDefaults();

            emit setWindowCaption(QString());
        }
    }

    m_view->sortByColumn(0, Qt::AscendingOrder);

    // #303708: expand the first level only when there is just one root folder.
    // Typical use case: an archive with source files.
    if (m_view->model()->rowCount() == 1) {
        m_view->expandToDepth(0);
    }

    // After loading all files, resize the columns to fit all fields
    m_view->header()->resizeSections(QHeaderView::ResizeToContents);

    updateActions();

    if (!m_model->archive()) {
        return;
    }

    if (!m_model->archive()->comment().isEmpty()) {
        m_commentView->setPlainText(m_model->archive()->comment());
        m_commentBox->show();
        m_commentSplitter->setSizes(QList<int>() << m_view->height() * 0.6 << 1);
    } else {
        m_commentView->clear();
        m_commentBox->hide();
    }

    if (m_model->rowCount() == 0) {
        qCWarning(ARK) << "No entry listed by the plugin";
        displayMsgWidget(KMessageWidget::Warning, xi18nc("@info", "The archive is empty or Ark could not open its content."));
    } else if (m_model->rowCount() == 1) {
        QMimeType mime = QMimeDatabase().mimeTypeForName(Archive::determineMimeType(m_model->archive()->fileName()));
        if (mime.inherits(QStringLiteral("application/x-cd-image")) &&
            m_model->entryForIndex(m_model->index(0, 0))[FileName].toString() == QLatin1String("README.TXT")) {
            qCWarning(ARK) << "Detected ISO image with UDF filesystem";
            displayMsgWidget(KMessageWidget::Warning, xi18nc("@info", "Ark does not currently support ISO files with UDF filesystem."));
        }
    }
}

void Part::setReadyGui()
{
    QApplication::restoreOverrideCursor();
    m_busy = false;
    m_view->setEnabled(true);
    updateActions();
}

void Part::setBusyGui()
{
    QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
    m_busy = true;
    m_view->setEnabled(false);
    updateActions();
}

void Part::setFileNameFromArchive()
{
    const QString prettyName = url().fileName();

    m_infoPanel->setPrettyFileName(prettyName);
    m_infoPanel->updateWithDefaults();

    emit setWindowCaption(prettyName);
}

void Part::slotOpenEntry(int mode)
{
    qCDebug(ARK) << "Opening with mode" << mode;

    QModelIndex index = m_view->selectionModel()->currentIndex();
    const ArchiveEntry& entry =  m_model->entryForIndex(index);

    // Don't open directories.
    if (entry[IsDirectory].toBool()) {
        return;
    }

    // We don't support opening symlinks.
    if (entry[Link].toBool()) {
        displayMsgWidget(KMessageWidget::Information, i18n("Ark cannot open symlinks."));
        return;
    }

    // Extract the entry.
    if (!entry.isEmpty()) {
        Kerfuffle::ExtractionOptions options;
        options[QStringLiteral("PreservePaths")] = true;

        m_tmpOpenDirList.append(new QTemporaryDir);
        m_openFileMode = static_cast<OpenFileMode>(mode);
        ExtractJob *job = m_model->extractFile(entry[InternalID], m_tmpOpenDirList.last()->path(), options);

        registerJob(job);
        connect(job, &KJob::result,
                this, &Part::slotOpenExtractedEntry);
        job->start();
    }
}

void Part::slotOpenExtractedEntry(KJob *job)
{
    // FIXME: the error checking here isn't really working
    //        if there's an error or an overwrite dialog,
    //        the preview dialog will be launched anyway
    if (!job->error()) {
        const ArchiveEntry& entry =
            m_model->entryForIndex(m_view->selectionModel()->currentIndex());

        ExtractJob *extractJob = qobject_cast<ExtractJob*>(job);
        Q_ASSERT(extractJob);
        QString fullName = extractJob->destinationDirectory() + QLatin1Char('/') + entry[FileName].toString();

        // Make sure a maliciously crafted archive with parent folders named ".." do
        // not cause the previewed file path to be located outside the temporary
        // directory, resulting in a directory traversal issue.
        fullName.remove(QStringLiteral("../"));

        bool isWritable = m_model->archive() && !m_model->archive()->isReadOnly();

        // If archive is readonly set temporarily extracted file to readonly as
        // well so user will be notified if trying to modify and save the file.
        if (!isWritable) {
            QFile::setPermissions(fullName, QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
        }

        // TODO: get rid of m_openFileMode by extending ExtractJob with a
        // Preview/OpenJob. This would prevent race conditions if we ever stop
        // disabling the whole UI while extracting a file to preview it.
        if (m_openFileMode != Preview && isWritable) {
            m_fileWatcher = new QFileSystemWatcher;
            connect(m_fileWatcher, &QFileSystemWatcher::fileChanged, this, &Part::slotWatchedFileModified);
        }

        QMimeDatabase db;
        switch (m_openFileMode) {

        case Preview:
            ArkViewer::view(fullName, widget());
            break;
        case OpenFile:
            KRun::runUrl(QUrl::fromUserInput(fullName,
                                             QString(),
                                             QUrl::AssumeLocalFile),
                         db.mimeTypeForFile(fullName).name(),
                         widget());
            break;
        case OpenFileWith:
            QList<QUrl> list;
            list.append(QUrl::fromUserInput(fullName,
                                            QString(),
                                            QUrl::AssumeLocalFile));
            KRun::displayOpenWithDialog(list, widget(), true);

            break;
        }
        if (m_openFileMode != Preview && isWritable) {
            m_fileWatcher->addPath(fullName);
        }

    } else if (job->error() != KJob::KilledJobError) {
        KMessageBox::error(widget(), job->errorString());
    }
    setReadyGui();
}

void Part::slotWatchedFileModified(const QString& file)
{
    qCDebug(ARK) << "Watched file modified:" << file;

    // Find the relative path of the file within the archive.
    QString relPath = file;
    foreach (QTemporaryDir *tmpDir, m_tmpOpenDirList) {
        relPath.remove(tmpDir->path()); //Remove tmpDir.
    }
    relPath = relPath.mid(1); //Remove leading slash.
    if (relPath.contains(QLatin1Char('/'))) {
        relPath = relPath.section(QLatin1Char('/'), 0, -2); //Remove filename.
    } else {
        // File is in the root of the archive, no path.
        relPath = QString();
    }

    // Set up a string for display in KMessageBox.
    QString prettyFilename;
    if (relPath.isEmpty()) {
        prettyFilename = file.section(QLatin1Char('/'), -1);
    } else {
        prettyFilename = relPath + QLatin1Char('/') + file.section(QLatin1Char('/'), -1);
    }

    if (KMessageBox::questionYesNo(widget(),
                               xi18n("The file <filename>%1</filename> was modified. Do you want to update the archive?",
                                     prettyFilename),
                               i18nc("@title:window", "File Modified")) == KMessageBox::Yes) {
        QStringList list = QStringList() << file;

        qCDebug(ARK) << "Updating file" << file << "with path" << relPath;
        slotAddFiles(list, relPath);
    }
    // This is needed because some apps, such as Kate, delete and recreate
    // files when saving.
    m_fileWatcher->addPath(file);
}

void Part::slotError(const QString& errorMessage, const QString& details)
{
    if (details.isEmpty()) {
        KMessageBox::error(widget(), errorMessage);
    } else {
        KMessageBox::detailedError(widget(), errorMessage, details);
    }
}

bool Part::isSingleFolderArchive() const
{
    return m_model->archive()->isSingleFolderArchive();
}

QString Part::detectSubfolder() const
{
    if (!m_model) {
        return QString();
    }

    return m_model->archive()->subfolderName();
}

void Part::slotExtractArchive()
{
    if (m_view->selectionModel()->selectedRows().count() > 0) {
        m_view->selectionModel()->clear();
    }

    slotShowExtractionDialog();
}

void Part::slotShowExtractionDialog()
{
    if (!m_model) {
        return;
    }

    QPointer<Kerfuffle::ExtractionDialog> dialog(new Kerfuffle::ExtractionDialog);

    dialog.data()->setModal(true);

    if (m_view->selectionModel()->selectedRows().count() > 0) {
        dialog.data()->setShowSelectedFiles(true);
    }

    dialog.data()->setSingleFolderArchive(isSingleFolderArchive());
    dialog.data()->setSubfolder(detectSubfolder());

    dialog.data()->setCurrentUrl(QUrl::fromLocalFile(QFileInfo(m_model->archive()->fileName()).absolutePath()));

    dialog.data()->show();
    dialog.data()->restoreWindowSize();

    if (dialog.data()->exec()) {
        //this is done to update the quick extract menu
        updateActions();

        QVariantList files;

        // If the user has chosen to extract only selected entries, fetch these
        // from the QTreeView.
        if (!dialog.data()->extractAllFiles()) {
            files = filesAndRootNodesForIndexes(addChildren(m_view->selectionModel()->selectedRows()));
        }

        qCDebug(ARK) << "Selected " << files;

        Kerfuffle::ExtractionOptions options;

        if (dialog.data()->preservePaths()) {
            options[QStringLiteral("PreservePaths")] = true;
        }
        options[QStringLiteral("FollowExtractionDialogSettings")] = true;

        const QString destinationDirectory = dialog.data()->destinationDirectory().toDisplayString(QUrl::PreferLocalFile);
        ExtractJob *job = m_model->extractFiles(files, destinationDirectory, options);
        registerJob(job);

        connect(job, &KJob::result,
                this, &Part::slotExtractionDone);

        job->start();
    }

    delete dialog.data();
}

QModelIndexList Part::addChildren(const QModelIndexList &list) const
{
    Q_ASSERT(m_model);

    QModelIndexList ret = list;

    // Iterate over indexes in list and add all children.
    for (int i = 0; i < ret.size(); ++i) {
        QModelIndex index = ret.at(i);

        for (int j = 0; j < m_model->rowCount(index); ++j) {
            QModelIndex child = m_model->index(j, 0, index);
            if (!ret.contains(child)) {
                ret << child;
            }
        }
    }

    return ret;
}

QList<QVariant> Part::filesForIndexes(const QModelIndexList& list) const
{
    QVariantList ret;

    foreach(const QModelIndex& index, list) {
        const ArchiveEntry& entry = m_model->entryForIndex(index);
        ret << entry[InternalID].toString();
    }

    return ret;
}

QList<QVariant> Part::filesAndRootNodesForIndexes(const QModelIndexList& list) const
{
    QVariantList fileList;

    foreach (const QModelIndex& index, list) {

        // Find the topmost unselected parent. This is done by iterating up
        // through the directory hierarchy and see if each parent is included
        // in the selection OR if the parent is already part of list.
        // The latter is needed for unselected folders which are subfolders of
        // a selected parent folder.
        QModelIndex selectionRoot = index.parent();
        while (m_view->selectionModel()->isSelected(selectionRoot) ||
               list.contains(selectionRoot)) {
            selectionRoot = selectionRoot.parent();
        }

        // Fetch the root node for the unselected parent.
        const QString rootInternalID =
            m_model->entryForIndex(selectionRoot).value(InternalID).toString();


        // Append index with root node to fileList.
        QModelIndexList alist = QModelIndexList() << index;
        foreach (const QVariant &file, filesForIndexes(alist)) {
            QVariant v = QVariant::fromValue(fileRootNodePair(file.toString(), rootInternalID));
            if (!fileList.contains(v)) {
                fileList.append(v);
            }
        }
    }
    return fileList;
}

void Part::slotExtractionDone(KJob* job)
{
    if (job->error() && job->error() != KJob::KilledJobError) {
        KMessageBox::error(widget(), job->errorString());
    } else {
        ExtractJob *extractJob = qobject_cast<ExtractJob*>(job);
        Q_ASSERT(extractJob);

        const bool followExtractionDialogSettings =
            extractJob->extractionOptions().value(QStringLiteral("FollowExtractionDialogSettings"), false).toBool();
        if (!followExtractionDialogSettings) {
            return;
        }

        if (ArkSettings::openDestinationFolderAfterExtraction()) {
            qCDebug(ARK) << "Shall open" << extractJob->destinationDirectory();
            QUrl destinationDirectory = QUrl::fromLocalFile(extractJob->destinationDirectory()).adjusted(QUrl::NormalizePathSegments);
            qCDebug(ARK) << "Shall open URL" << destinationDirectory;

            KRun::runUrl(destinationDirectory, QStringLiteral("inode/directory"), widget());
        }

        if (ArkSettings::closeAfterExtraction()) {
           emit quit();
        }
    }
}

void Part::adjustColumns()
{
    m_view->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
}

void Part::slotAddFiles(const QStringList& filesToAdd, const QString& path)
{
    if (filesToAdd.isEmpty()) {
        return;
    }

    qCDebug(ARK) << "Adding " << filesToAdd << " to " << path;

    // Add a trailing slash to directories.
    QStringList cleanFilesToAdd(filesToAdd);
    for (int i = 0; i < cleanFilesToAdd.size(); ++i) {
        QString& file = cleanFilesToAdd[i];
        if (QFileInfo(file).isDir()) {
            if (!file.endsWith(QLatin1Char( '/' ))) {
                file += QLatin1Char( '/' );
            }
        }
    }

    // GlobalWorkDir is used by AddJob and should contain the part of the
    // absolute path of files to be added that should NOT be included in the
    // directory structure within the archive.
    // Example: We add file "/home/user/somedir/somefile.txt" and want the file
    // to have the relative path within the archive "somedir/somefile.txt".
    // GlobalWorkDir is then: "/home/user"
    QString globalWorkDir = cleanFilesToAdd.first();

    // path represents the path of the file within the archive. This needs to
    // be removed from globalWorkDir, otherwise the files will be added to the
    // root of the archive. In the example above, path would be "somedir/".
    if (!path.isEmpty()) {
        globalWorkDir.remove(path);
    }

    // Remove trailing slash (needed when adding dirs).
    if (globalWorkDir.right(1) == QLatin1String("/")) {
        globalWorkDir.chop(1);
    }

    // Now take the absolute path of the parent directory.
    globalWorkDir = QFileInfo(globalWorkDir).dir().absolutePath();

    qCDebug(ARK) << "Detected GlobalWorkDir to be " << globalWorkDir;
    CompressionOptions options;
    options[QStringLiteral("GlobalWorkDir")] = globalWorkDir;

    AddJob *job = m_model->addFiles(cleanFilesToAdd, options);
    if (!job) {
        return;
    }

    connect(job, &KJob::result,
            this, &Part::slotAddFilesDone);
    registerJob(job);
    job->start();
}

void Part::slotAddFiles()
{
    // #264819: passing widget() as the parent will not work as expected.
    //          KFileDialog will create a KFileWidget, which runs an internal
    //          event loop to stat the given directory. This, in turn, leads to
    //          events being delivered to widget(), which is a QSplitter, which
    //          in turn reimplements childEvent() and will end up calling
    //          QWidget::show() on the KFileDialog (thus showing it in a
    //          non-modal state).
    //          When KFileDialog::exec() is called, the widget is already shown
    //          and nothing happens.

    const QStringList filesToAdd = QFileDialog::getOpenFileNames(widget(), i18nc("@title:window", "Add Files"));

    slotAddFiles(filesToAdd);
}

void Part::slotAddDir()
{
    const QString dirToAdd = QFileDialog::getExistingDirectory(widget(), i18nc("@title:window", "Add Folder"));

    if (!dirToAdd.isEmpty()) {
        slotAddFiles(QStringList() << dirToAdd);
    }
}

void Part::slotAddFilesDone(KJob* job)
{
    if (job->error() && job->error() != KJob::KilledJobError) {
        KMessageBox::error(widget(), job->errorString());
    }
}

void Part::slotDeleteFilesDone(KJob* job)
{
    if (job->error() && job->error() != KJob::KilledJobError) {
        KMessageBox::error(widget(), job->errorString());
    }
}

void Part::slotDeleteFiles()
{
    const int reallyDelete =
        KMessageBox::questionYesNo(widget(),
                                   i18n("Deleting these files is not undoable. Are you sure you want to do this?"),
                                   i18nc("@title:window", "Delete files"),
                                   KStandardGuiItem::del(),
                                   KStandardGuiItem::cancel(),
                                   QString(),
                                   KMessageBox::Dangerous | KMessageBox::Notify);

    if (reallyDelete == KMessageBox::No) {
        return;
    }

    DeleteJob *job = m_model->deleteFiles(filesForIndexes(addChildren(m_view->selectionModel()->selectedRows())));
    connect(job, &KJob::result,
            this, &Part::slotDeleteFilesDone);
    registerJob(job);
    job->start();
}

void Part::slotToggleInfoPanel(bool visible)
{
    if (visible) {
        m_splitter->setSizes(ArkSettings::splitterSizes());
        m_infoPanel->show();
    } else {
        // We need to save the splitterSizes before hiding, otherwise
        // Ark won't remember resizing done by the user.
        ArkSettings::setSplitterSizes(m_splitter->sizes());
        m_infoPanel->hide();
    }
}

void Part::slotSaveAs()
{
    QUrl saveUrl = QFileDialog::getSaveFileUrl(widget(), i18nc("@title:window", "Save Archive As"), url());

    if ((saveUrl.isValid()) && (!saveUrl.isEmpty())) {
        auto statJob = KIO::stat(saveUrl, KIO::StatJob::DestinationSide, 0);
        KJobWidgets::setWindow(statJob, widget());
        if (statJob->exec()) {
            int overwrite = KMessageBox::warningContinueCancel(widget(),
                                                               xi18nc("@info", "An archive named <filename>%1</filename> already exists. Are you sure you want to overwrite it?", saveUrl.fileName()),
                                                               QString(),
                                                               KStandardGuiItem::overwrite());

            if (overwrite != KMessageBox::Continue) {
                return;
            }
        }

        QUrl srcUrl = QUrl::fromLocalFile(localFilePath());

        if (!QFile::exists(localFilePath())) {
            if (url().isLocalFile()) {
                KMessageBox::error(widget(),
                                   xi18nc("@info", "The archive <filename>%1</filename> cannot be copied to the specified location. The archive does not exist anymore.", localFilePath()));

                return;
            } else {
                srcUrl = url();
            }
        }

        KIO::Job *copyJob = KIO::file_copy(srcUrl, saveUrl, -1, KIO::Overwrite);

        KJobWidgets::setWindow(copyJob, widget());
        copyJob->exec();
        if (copyJob->error()) {
            KMessageBox::error(widget(),
                               xi18nc("@info", "The archive could not be saved as <filename>%1</filename>. Try saving it to another location.", saveUrl.path()));
        }
    }
}

void Part::slotShowContextMenu()
{
    if (!factory()) {
        return;
    }

    QMenu *popup = static_cast<QMenu *>(factory()->container(QStringLiteral("context_menu"), this));
    popup->popup(QCursor::pos());
}

void Part::displayMsgWidget(KMessageWidget::MessageType type, const QString& msg)
{
    KMessageWidget *msgWidget = new KMessageWidget();
    msgWidget->setText(msg);
    msgWidget->setMessageType(type);
    m_vlayout->insertWidget(0, msgWidget);
    msgWidget->animatedShow();
}

} // namespace Ark

#include "part.moc"
