#include "library_window.h"

#include "yacreader_global.h"

#include <QHBoxLayout>
#include <QSplitter>
#include <QLabel>
#include <QDir>
#include <QHeaderView>
#include <QProcess>
#include <QtCore>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QFileIconProvider>
#include <QSettings>
#include <QHeaderView>

#include <algorithm>
#include <iterator>
#include <typeinfo>
#include <thread>
#include <future>

#include "folder_item.h"
#include "data_base_management.h"
#include "no_libraries_widget.h"
#include "import_widget.h"

#include "yacreader_search_line_edit.h"
#include "comic_db.h"
#include "library_creator.h"
#include "package_manager.h"
#include "xml_info_library_scanner.h"
#include "create_library_dialog.h"
#include "rename_library_dialog.h"
#include "properties_dialog.h"
#include "export_library_dialog.h"
#include "import_library_dialog.h"
#include "export_comics_info_dialog.h"
#include "import_comics_info_dialog.h"
#include "add_library_dialog.h"
#include "options_dialog.h"
#include "help_about_dialog.h"
#include "server_config_dialog.h"
#include "comic_model.h"
#include "yacreader_tool_bar_stretch.h"

#include "yacreader_titled_toolbar.h"
#include "yacreader_main_toolbar.h"

#include "yacreader_sidebar.h"

#include "comics_remover.h"
#include "yacreader_library_list_widget.h"
#include "yacreader_folders_view.h"

#include "comic_vine_dialog.h"
#include "api_key_dialog.h"
// #include "yacreader_social_dialog.h"

#include "comics_view.h"

#include "edit_shortcuts_dialog.h"
#include "shortcuts_manager.h"

#include "comic_files_manager.h"

#include "reading_list_model.h"
#include "yacreader_reading_lists_view.h"
#include "add_label_dialog.h"

#include "yacreader_history_controller.h"
#include "db_helper.h"

#include "reading_list_item.h"
#include "opengl_checker.h"

#include "yacreader_content_views_manager.h"
#include "folder_content_view.h"

#include "trayicon_controller.h"

#include "whats_new_controller.h"

#include "library_comic_opener.h"

#include "recent_visibility_coordinator.h"

#include "QsLog.h"

#include "yacreader_http_server.h"
extern YACReaderHttpServer *httpServer;

#ifdef Q_OS_WIN
#include <shellapi.h>
#endif

#include <KDSignalThrottler.h>

namespace {
template<class Remover>
void moveAndConnectRemoverToThread(Remover *remover, QThread *thread)
{
    Q_ASSERT(remover);
    Q_ASSERT(thread);
    remover->moveToThread(thread);
    QObject::connect(thread, &QThread::started, remover, &Remover::process);
    QObject::connect(remover, &Remover::finished, remover, &QObject::deleteLater);
    QObject::connect(remover, &Remover::finished, thread, &QThread::quit);
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
}
}

using namespace YACReader;

LibraryWindow::LibraryWindow()
    : QMainWindow(), fullscreen(false), previousFilter(""), fetching(false), status(LibraryWindow::Normal), removeError(false)
{
    createSettings();

    setupUI();

    loadLibraries();

    if (libraries.isEmpty()) {
        showNoLibrariesWidget();
    } else {
        showRootWidget();
        selectedLibrary->setCurrentIndex(0);
    }

    afterLaunchTasks();
}

void LibraryWindow::afterLaunchTasks()
{
    if (!libraries.isEmpty()) {
        WhatsNewController whatsNewController;
        whatsNewController.showWhatsNewIfNeeded(this);
    }
}

bool LibraryWindow::eventFilter(QObject *object, QEvent *event)
{
    if (this->isActiveWindow()) {
        if (event->type() == QEvent::MouseButtonRelease) {
            auto mouseEvent = static_cast<QMouseEvent *>(event);

            if (mouseEvent->button() == Qt::ForwardButton) {
                forwardAction->trigger();
                event->accept();
                return true;
            }

            if (mouseEvent->button() == Qt::BackButton) {
                backAction->trigger();
                event->accept();
                return true;
            }
        }
    }

    if (this->foldersView->hasFocus() && event->type() == QEvent::Shortcut) {
        auto shortcutEvent = static_cast<QShortcutEvent *>(event);
        auto keySequence = shortcutEvent->key();

        if (keySequence.count() > 1) {
            return QMainWindow::eventFilter(object, event);
        }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        auto keyCombination = keySequence[0];

        if (keyCombination.keyboardModifiers() != Qt::NoModifier) {
            return QMainWindow::eventFilter(object, event);
        }
#endif

        auto string = keySequence.toString();

        if (string.size() > 1) {
            return QMainWindow::eventFilter(object, event);
        }

        event->ignore();

        foldersView->keyboardSearch(keySequence.toString());
        return true;
    }

    return QMainWindow::eventFilter(object, event);
}

void LibraryWindow::createSettings()
{
    settings = new QSettings(YACReader::getSettingsPath() + "/YACReaderLibrary.ini", QSettings::IniFormat); // TODO unificar la creación del fichero de config con el servidor
    settings->beginGroup("libraryConfig");
}

void LibraryWindow::setupOpenglSetting()
{
#ifndef NO_OPENGL
    // FLOW-----------------------------------------------------------------------
    //---------------------------------------------------------------------------

    OpenGLChecker openGLChecker;
    bool openGLAvailable = openGLChecker.hasCompatibleOpenGLVersion();

    if (openGLAvailable && !settings->contains(USE_OPEN_GL))
        settings->setValue(USE_OPEN_GL, 2);
    else if (!openGLAvailable)
        settings->setValue(USE_OPEN_GL, 0);
#endif
}

void LibraryWindow::setupUI()
{
    setupOpenglSetting();

    setUnifiedTitleAndToolBarOnMac(true);

    libraryCreator = new LibraryCreator(settings);
    packageManager = new PackageManager();
    xmlInfoLibraryScanner = new XMLInfoLibraryScanner();

    historyController = new YACReaderHistoryController(this);

    createActions();
    doModels();

    doDialogs();
    doLayout();
    createToolBars();
    createMenus();

    setupCoordinators();

    navigationController = new YACReaderNavigationController(this, contentViewsManager);

    createConnections();

    setWindowTitle(tr("YACReader Library"));

    setMinimumSize(800, 480);

    // restore
    if (settings->contains(MAIN_WINDOW_GEOMETRY))
        restoreGeometry(settings->value(MAIN_WINDOW_GEOMETRY).toByteArray());
    else
        // if(settings->value(USE_OPEN_GL).toBool() == false)
        showMaximized();

    trayIconController = new TrayIconController(settings, this);
}

void LibraryWindow::doLayout()
{
    // LAYOUT ELEMENTS------------------------------------------------------------
    auto sHorizontal = new QSplitter(Qt::Horizontal); // spliter principal
#ifdef Y_MAC_UI
    sHorizontal->setStyleSheet("QSplitter::handle{image:none;background-color:#B8B8B8;} QSplitter::handle:vertical {height:1px;}");
#else
    sHorizontal->setStyleSheet("QSplitter::handle:vertical {height:4px;}");
#endif

    // TOOLBARS-------------------------------------------------------------------
    //---------------------------------------------------------------------------
    editInfoToolBar = new QToolBar();
    editInfoToolBar->setStyleSheet("QToolBar {border: none;}");

#ifdef Y_MAC_UI
    libraryToolBar = new YACReaderMacOSXToolbar(this);
#else
    libraryToolBar = new YACReaderMainToolBar(this);
#endif

    // FOLDERS FILTER-------------------------------------------------------------
    //---------------------------------------------------------------------------
#ifndef Y_MAC_UI
    // in MacOSX the searchEdit is created using the toolbar wrapper
    searchEdit = new YACReaderSearchLineEdit();
#endif

    // SIDEBAR--------------------------------------------------------------------
    //---------------------------------------------------------------------------
    sideBar = new YACReaderSideBar;

    foldersView = sideBar->foldersView;
    listsView = sideBar->readingListsView;
    selectedLibrary = sideBar->selectedLibrary;

    YACReaderTitledToolBar *librariesTitle = sideBar->librariesTitle;
    YACReaderTitledToolBar *foldersTitle = sideBar->foldersTitle;
    YACReaderTitledToolBar *readingListsTitle = sideBar->readingListsTitle;

    librariesTitle->addAction(createLibraryAction);
    librariesTitle->addAction(openLibraryAction);
    librariesTitle->addSpacing(3);

    foldersTitle->addAction(addFolderAction);
    foldersTitle->addAction(deleteFolderAction);
    foldersTitle->addSepartor();
    foldersTitle->addAction(setRootIndexAction);
    foldersTitle->addAction(expandAllNodesAction);
    foldersTitle->addAction(colapseAllNodesAction);

    readingListsTitle->addAction(addReadingListAction);
    // readingListsTitle->addSepartor();
    readingListsTitle->addAction(addLabelAction);
    // readingListsTitle->addSepartor();
    readingListsTitle->addAction(renameListAction);
    readingListsTitle->addAction(deleteReadingListAction);
    readingListsTitle->addSpacing(3);

    // FINAL LAYOUT-------------------------------------------------------------

    contentViewsManager = new YACReaderContentViewsManager(settings, this);

    sHorizontal->addWidget(sideBar);
#ifndef Y_MAC_UI
    QVBoxLayout *rightLayout = new QVBoxLayout;
    rightLayout->addWidget(libraryToolBar);
    rightLayout->addWidget(contentViewsManager->containerWidget());

    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    QWidget *rightWidget = new QWidget();
    rightWidget->setLayout(rightLayout);

    sHorizontal->addWidget(rightWidget);
#else
    sHorizontal->addWidget(contentViewsManager->containerWidget());
#endif

    sHorizontal->setStretchFactor(0, 0);
    sHorizontal->setStretchFactor(1, 1);
    mainWidget = new QStackedWidget(this);
    mainWidget->addWidget(sHorizontal);
    setCentralWidget(mainWidget);
    // FINAL LAYOUT-------------------------------------------------------------

    // OTHER----------------------------------------------------------------------
    //---------------------------------------------------------------------------
    noLibrariesWidget = new NoLibrariesWidget();
    mainWidget->addWidget(noLibrariesWidget);

    importWidget = new ImportWidget();
    mainWidget->addWidget(importWidget);

    connect(noLibrariesWidget, &NoLibrariesWidget::createNewLibrary, this, &LibraryWindow::createLibrary);
    connect(noLibrariesWidget, &NoLibrariesWidget::addExistingLibrary, this, &LibraryWindow::showAddLibrary);

    // collapsible disabled in macosx (only temporaly)
#ifdef Y_MAC_UI
    sHorizontal->setCollapsible(0, false);
#endif
}

void LibraryWindow::doDialogs()
{
    createLibraryDialog = new CreateLibraryDialog(this);
    renameLibraryDialog = new RenameLibraryDialog(this);
    propertiesDialog = new PropertiesDialog(this);
    comicVineDialog = new ComicVineDialog(this);
    exportLibraryDialog = new ExportLibraryDialog(this);
    importLibraryDialog = new ImportLibraryDialog(this);
    exportComicsInfoDialog = new ExportComicsInfoDialog(this);
    importComicsInfoDialog = new ImportComicsInfoDialog(this);
    addLibraryDialog = new AddLibraryDialog(this);
    optionsDialog = new OptionsDialog(this);
    optionsDialog->restoreOptions(settings);

    editShortcutsDialog = new EditShortcutsDialog(this);
    setUpShortcutsManagement();

#ifdef SERVER_RELEASE
    serverConfigDialog = new ServerConfigDialog(this);
#endif

    had = new HelpAboutDialog(this); // TODO load data.
    QString sufix = QLocale::system().name();
    if (QFile(":/files/about_" + sufix + ".html").exists())
        had->loadAboutInformation(":/files/about_" + sufix + ".html");
    else
        had->loadAboutInformation(":/files/about.html");

    if (QFile(":/files/helpYACReaderLibrary_" + sufix + ".html").exists())
        had->loadHelp(":/files/helpYACReaderLibrary_" + sufix + ".html");
    else
        had->loadHelp(":/files/helpYACReaderLibrary.html");
}

void LibraryWindow::setUpShortcutsManagement()
{

    QList<QAction *> allActions;
    QList<QAction *> tmpList;

    editShortcutsDialog->addActionsGroup("Comics", QIcon(":/images/shortcuts_group_comics.svg"),
                                         tmpList = QList<QAction *>()
                                                 << openComicAction
                                                 << saveCoversToAction
                                                 << setAsReadAction
                                                 << setAsNonReadAction
                                                 << setMangaAction
                                                 << setNormalAction
                                                 << openContainingFolderComicAction
                                                 << resetComicRatingAction
                                                 << selectAllComicsAction
                                                 << editSelectedComicsAction
                                                 << asignOrderAction
                                                 << deleteMetadataAction
                                                 << deleteComicsAction
                                                 << getInfoAction);

    allActions << tmpList;

    editShortcutsDialog->addActionsGroup("Folders", QIcon(":/images/shortcuts_group_folders.svg"),
                                         tmpList = QList<QAction *>()
                                                 << addFolderAction
                                                 << deleteFolderAction
                                                 << setRootIndexAction
                                                 << expandAllNodesAction
                                                 << colapseAllNodesAction
                                                 << openContainingFolderAction
                                                 << setFolderAsNotCompletedAction
                                                 << setFolderAsCompletedAction
                                                 << setFolderAsReadAction
                                                 << setFolderAsUnreadAction
                                                 << setFolderAsMangaAction
                                                 << setFolderAsNormalAction
                                                 << updateCurrentFolderAction
                                                 << rescanXMLFromCurrentFolderAction);
    allActions << tmpList;

    editShortcutsDialog->addActionsGroup("Lists", QIcon(":/images/shortcuts_group_folders.svg"), // TODO change icon
                                         tmpList = QList<QAction *>()
                                                 << addReadingListAction
                                                 << deleteReadingListAction
                                                 << addLabelAction
                                                 << renameListAction);
    allActions << tmpList;

    editShortcutsDialog->addActionsGroup("General", QIcon(":/images/shortcuts_group_general.svg"),
                                         tmpList = QList<QAction *>()
                                                 << backAction
                                                 << forwardAction
                                                 << focusSearchLineAction
                                                 << focusComicsViewAction
                                                 << helpAboutAction
                                                 << optionsAction
                                                 << serverConfigAction
                                                 << showEditShortcutsAction
                                                 << quitAction);

    allActions << tmpList;

    editShortcutsDialog->addActionsGroup("Libraries", QIcon(":/images/shortcuts_group_libraries.svg"),
                                         tmpList = QList<QAction *>()
                                                 << createLibraryAction
                                                 << openLibraryAction
                                                 << exportComicsInfoAction
                                                 << importComicsInfoAction
                                                 << exportLibraryAction
                                                 << importLibraryAction
                                                 << updateLibraryAction
                                                 << renameLibraryAction
                                                 << removeLibraryAction
                                                 << rescanLibraryForXMLInfoAction);

    allActions << tmpList;

    editShortcutsDialog->addActionsGroup("Visualization", QIcon(":/images/shortcuts_group_visualization.svg"),
                                         tmpList = QList<QAction *>()
                                                 << showHideMarksAction
                                                 << toogleShowRecentIndicatorAction
#ifndef Q_OS_MACOS
                                                 << toggleFullScreenAction // Think about what to do in macos if the default theme is used
#endif
                                                 << toggleComicsViewAction);

    allActions << tmpList;

    ShortcutsManager::getShortcutsManager().registerActions(allActions);
}

void LibraryWindow::doModels()
{
    // folders
    foldersModel = new FolderModel(this);
    foldersModelProxy = new FolderModelProxy(this);
    folderQueryResultProcessor.reset(new FolderQueryResultProcessor(foldersModel));
    // foldersModelProxy->setSourceModel(foldersModel);
    // comics
    comicsModel = new ComicModel(this);
    // lists
    listsModel = new ReadingListModel(this);
    listsModelProxy = new ReadingListModelProxy(this);
}

void LibraryWindow::setupCoordinators()
{
    recentVisibilityCoordinator = new RecentVisibilityCoordinator(settings, foldersModel, contentViewsManager->folderContentView, comicsModel);

    auto canStartUpdateProvider = [this]() {
        return comicVineDialog->isVisible() == false &&
                propertiesDialog->isVisible() == false;
    };
    librariesUpdateCoordinator = new LibrariesUpdateCoordinator(settings, libraries, canStartUpdateProvider, this);

    connect(librariesUpdateCoordinator, &LibrariesUpdateCoordinator::updateStarted, sideBar->librariesTitle, &YACReaderTitledToolBar::showBusyIndicator);
    connect(librariesUpdateCoordinator, &LibrariesUpdateCoordinator::updateEnded, sideBar->librariesTitle, &YACReaderTitledToolBar::hideBusyIndicator);

    connect(librariesUpdateCoordinator, &LibrariesUpdateCoordinator::updateStarted, this, [=]() {
        disableAllActions();
    });
    connect(librariesUpdateCoordinator, &LibrariesUpdateCoordinator::updateEnded, this, &LibraryWindow::reloadCurrentLibrary);

    librariesUpdateCoordinator->init();

    connect(sideBar->librariesTitle, &YACReaderTitledToolBar::cancelOperationRequested, librariesUpdateCoordinator, &LibrariesUpdateCoordinator::cancel);
}

void LibraryWindow::createActions()
{
    backAction = new QAction(this);
    QIcon icoBackButton;
    icoBackButton.addFile(addExtensionToIconPath(":/images/main_toolbar/back"), QSize(), QIcon::Normal);
    // icoBackButton.addPixmap(QPixmap(":/images/main_toolbar/back_disabled.png"), QIcon::Disabled);
    backAction->setData(BACK_ACTION_YL);
    backAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(BACK_ACTION_YL));
    backAction->setIcon(icoBackButton);
    backAction->setDisabled(true);

    forwardAction = new QAction(this);
    QIcon icoFordwardButton;
    icoFordwardButton.addFile(addExtensionToIconPath(":/images/main_toolbar/forward"), QSize(), QIcon::Normal);
    // icoFordwardButton.addPixmap(QPixmap(":/images/main_toolbar/forward_disabled.png"), QIcon::Disabled);
    forwardAction->setData(FORWARD_ACTION_YL);
    forwardAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(FORWARD_ACTION_YL));
    forwardAction->setIcon(icoFordwardButton);
    forwardAction->setDisabled(true);

    createLibraryAction = new QAction(this);
    createLibraryAction->setToolTip(tr("Create a new library"));
    createLibraryAction->setData(CREATE_LIBRARY_ACTION_YL);
    createLibraryAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(CREATE_LIBRARY_ACTION_YL));
    createLibraryAction->setIcon(QIcon(addExtensionToIconPath(":/images/sidebar/newLibraryIcon")));

    openLibraryAction = new QAction(this);
    openLibraryAction->setToolTip(tr("Open an existing library"));
    openLibraryAction->setData(OPEN_LIBRARY_ACTION_YL);
    openLibraryAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(OPEN_LIBRARY_ACTION_YL));
    openLibraryAction->setIcon(QIcon(addExtensionToIconPath(":/images/sidebar/openLibraryIcon")));

    exportComicsInfoAction = new QAction(tr("Export comics info"), this);
    exportComicsInfoAction->setToolTip(tr("Export comics info"));
    exportComicsInfoAction->setData(EXPORT_COMICS_INFO_ACTION_YL);
    exportComicsInfoAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(EXPORT_COMICS_INFO_ACTION_YL));
    exportComicsInfoAction->setIcon(QIcon(":/images/menus_icons/exportComicsInfoIcon.svg"));

    importComicsInfoAction = new QAction(tr("Import comics info"), this);
    importComicsInfoAction->setToolTip(tr("Import comics info"));
    importComicsInfoAction->setData(IMPORT_COMICS_INFO_ACTION_YL);
    importComicsInfoAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(IMPORT_COMICS_INFO_ACTION_YL));
    importComicsInfoAction->setIcon(QIcon(":/images/menus_icons/importComicsInfoIcon.svg"));

    exportLibraryAction = new QAction(tr("Pack covers"), this);
    exportLibraryAction->setToolTip(tr("Pack the covers of the selected library"));
    exportLibraryAction->setData(EXPORT_LIBRARY_ACTION_YL);
    exportLibraryAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(EXPORT_LIBRARY_ACTION_YL));
    exportLibraryAction->setIcon(QIcon(":/images/menus_icons/exportLibraryIcon.svg"));

    importLibraryAction = new QAction(tr("Unpack covers"), this);
    importLibraryAction->setToolTip(tr("Unpack a catalog"));
    importLibraryAction->setData(IMPORT_LIBRARY_ACTION_YL);
    importLibraryAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(IMPORT_LIBRARY_ACTION_YL));
    importLibraryAction->setIcon(QIcon(":/images/menus_icons/importLibraryIcon.svg"));

    updateLibraryAction = new QAction(tr("Update library"), this);
    updateLibraryAction->setToolTip(tr("Update current library"));
    updateLibraryAction->setData(UPDATE_LIBRARY_ACTION_YL);
    updateLibraryAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(UPDATE_LIBRARY_ACTION_YL));
    updateLibraryAction->setIcon(QIcon(":/images/menus_icons/updateLibraryIcon.svg"));

    renameLibraryAction = new QAction(tr("Rename library"), this);
    renameLibraryAction->setToolTip(tr("Rename current library"));
    renameLibraryAction->setData(RENAME_LIBRARY_ACTION_YL);
    renameLibraryAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(RENAME_LIBRARY_ACTION_YL));
    renameLibraryAction->setIcon(QIcon(":/images/menus_icons/editIcon.svg"));

    removeLibraryAction = new QAction(tr("Remove library"), this);
    removeLibraryAction->setToolTip(tr("Remove current library from your collection"));
    removeLibraryAction->setData(REMOVE_LIBRARY_ACTION_YL);
    removeLibraryAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(REMOVE_LIBRARY_ACTION_YL));
    removeLibraryAction->setIcon(QIcon(":/images/menus_icons/removeLibraryIcon.svg"));

    rescanLibraryForXMLInfoAction = new QAction(tr("Rescan library for XML info"), this);
    rescanLibraryForXMLInfoAction->setToolTip(tr("Tries to find XML info embedded in comic files. You only need to do this if the library was created with 9.8.2 or earlier versions or if you are using third party software to embed XML info in the files."));
    rescanLibraryForXMLInfoAction->setData(RESCAN_LIBRARY_XML_INFO_ACTION_YL);
    rescanLibraryForXMLInfoAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(RESCAN_LIBRARY_XML_INFO_ACTION_YL));

    openComicAction = new QAction(tr("Open current comic"), this);
    openComicAction->setToolTip(tr("Open current comic on YACReader"));
    openComicAction->setData(OPEN_COMIC_ACTION_YL);
    openComicAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(OPEN_COMIC_ACTION_YL));
    openComicAction->setIcon(QIcon(":/images/comics_view_toolbar/openInYACReader.svg"));

    saveCoversToAction = new QAction(tr("Save selected covers to..."), this);
    saveCoversToAction->setToolTip(tr("Save covers of the selected comics as JPG files"));
    saveCoversToAction->setData(SAVE_COVERS_TO_ACTION_YL);
    saveCoversToAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SAVE_COVERS_TO_ACTION_YL));

    setAsReadAction = new QAction(tr("Set as read"), this);
    setAsReadAction->setToolTip(tr("Set comic as read"));
    setAsReadAction->setData(SET_AS_READ_ACTION_YL);
    setAsReadAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SET_AS_READ_ACTION_YL));
    setAsReadAction->setIcon(QIcon(":/images/comics_view_toolbar/setReadButton.svg"));

    setAsNonReadAction = new QAction(tr("Set as unread"), this);
    setAsNonReadAction->setToolTip(tr("Set comic as unread"));
    setAsNonReadAction->setData(SET_AS_NON_READ_ACTION_YL);
    setAsNonReadAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SET_AS_NON_READ_ACTION_YL));
    setAsNonReadAction->setIcon(QIcon(":/images/comics_view_toolbar/setUnread.svg"));

    setMangaAction = new QAction(tr("manga"), this);
    setMangaAction->setToolTip(tr("Set issue as manga"));
    setMangaAction->setData(SET_AS_MANGA_ACTION_YL);
    setMangaAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SET_AS_MANGA_ACTION_YL));
    setMangaAction->setIcon(QIcon(":/images/comics_view_toolbar/setManga.svg"));

    setNormalAction = new QAction(tr("comic"), this);
    setNormalAction->setToolTip(tr("Set issue as normal"));
    setNormalAction->setData(SET_AS_NORMAL_ACTION_YL);
    setNormalAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SET_AS_NORMAL_ACTION_YL));
    setNormalAction->setIcon(QIcon(":/images/comics_view_toolbar/setNormal.svg"));

    setWesternMangaAction = new QAction(tr("western manga"), this);
    setWesternMangaAction->setToolTip(tr("Set issue as western manga"));
    setWesternMangaAction->setData(SET_AS_WESTERN_MANGA_ACTION_YL);
    setWesternMangaAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SET_AS_WESTERN_MANGA_ACTION_YL));
    // setWesternMangaAction->setIcon(QIcon(":/images/comics_view_toolbar/setWesternManga.svg"));

    setWebComicAction = new QAction(tr("web comic"), this);
    setWebComicAction->setToolTip(tr("Set issue as web comic"));
    setWebComicAction->setData(SET_AS_WEB_COMIC_ACTION_YL);
    setWebComicAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SET_AS_WEB_COMIC_ACTION_YL));
    // setWebComicAction->setIcon(QIcon(":/images/comics_view_toolbar/setWebComic.svg"));

    setYonkomaAction = new QAction(tr("yonkoma"), this);
    setYonkomaAction->setToolTip(tr("Set issue as yonkoma"));
    setYonkomaAction->setData(SET_AS_YONKOMA_ACTION_YL);
    setYonkomaAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SET_AS_YONKOMA_ACTION_YL));

    showHideMarksAction = new QAction(tr("Show/Hide marks"), this);
    showHideMarksAction->setToolTip(tr("Show or hide read marks"));
    showHideMarksAction->setData(SHOW_HIDE_MARKS_ACTION_YL);
    showHideMarksAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SHOW_HIDE_MARKS_ACTION_YL));
    showHideMarksAction->setCheckable(true);
    showHideMarksAction->setIcon(QIcon(":/images/comics_view_toolbar/showMarks.svg"));
    showHideMarksAction->setChecked(true);

    toogleShowRecentIndicatorAction = new QAction(tr("Show/Hide recent indicator"), this);
    toogleShowRecentIndicatorAction->setToolTip(tr("Show or hide recent indicator"));
    toogleShowRecentIndicatorAction->setData(SHOW_HIDE_RECENT_INDICATOR_ACTION_YL);
    toogleShowRecentIndicatorAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SHOW_HIDE_RECENT_INDICATOR_ACTION_YL));
    toogleShowRecentIndicatorAction->setCheckable(true);
    toogleShowRecentIndicatorAction->setIcon(QIcon(":/images/comics_view_toolbar/showRecentIndicator.svg"));
    toogleShowRecentIndicatorAction->setChecked(settings->value(DISPLAY_RECENTLY_INDICATOR, true).toBool());

#ifndef Q_OS_MACOS
    toggleFullScreenAction = new QAction(tr("Fullscreen mode on/off"), this);
    toggleFullScreenAction->setToolTip(tr("Fullscreen mode on/off"));
    toggleFullScreenAction->setData(TOGGLE_FULL_SCREEN_ACTION_YL);
    toggleFullScreenAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(TOGGLE_FULL_SCREEN_ACTION_YL));
    QIcon icoFullscreenButton;
    icoFullscreenButton.addFile(addExtensionToIconPath(":/images/main_toolbar/fullscreen"), QSize(), QIcon::Normal);
    toggleFullScreenAction->setIcon(icoFullscreenButton);
#endif
    helpAboutAction = new QAction(this);
    helpAboutAction->setToolTip(tr("Help, About YACReader"));
    helpAboutAction->setData(HELP_ABOUT_ACTION_YL);
    helpAboutAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(HELP_ABOUT_ACTION_YL));
    QIcon icoHelpButton;
    icoHelpButton.addFile(addExtensionToIconPath(":/images/main_toolbar/help"), QSize(), QIcon::Normal);
    helpAboutAction->setIcon(icoHelpButton);

    addFolderAction = new QAction(tr("Add new folder"), this);
    addFolderAction->setData(ADD_FOLDER_ACTION_YL);
    addFolderAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(ADD_FOLDER_ACTION_YL));
    addFolderAction->setToolTip(tr("Add new folder to the current library"));
    addFolderAction->setIcon(QIcon(addExtensionToIconPath(":/images/sidebar/addNew_sidebar")));

    deleteFolderAction = new QAction(tr("Delete folder"), this);
    deleteFolderAction->setData(REMOVE_FOLDER_ACTION_YL);
    deleteFolderAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(REMOVE_FOLDER_ACTION_YL));
    deleteFolderAction->setToolTip(tr("Delete current folder from disk"));
    deleteFolderAction->setIcon(QIcon(addExtensionToIconPath(":/images/sidebar/delete_sidebar")));

    setRootIndexAction = new QAction(this);
    setRootIndexAction->setData(SET_ROOT_INDEX_ACTION_YL);
    setRootIndexAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SET_ROOT_INDEX_ACTION_YL));
    setRootIndexAction->setToolTip(tr("Select root node"));
    setRootIndexAction->setIcon(QIcon(addExtensionToIconPath(":/images/sidebar/setRoot")));

    expandAllNodesAction = new QAction(this);
    expandAllNodesAction->setToolTip(tr("Expand all nodes"));
    expandAllNodesAction->setData(EXPAND_ALL_NODES_ACTION_YL);
    expandAllNodesAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(EXPAND_ALL_NODES_ACTION_YL));
    expandAllNodesAction->setIcon(QIcon(addExtensionToIconPath(":/images/sidebar/expand")));

    colapseAllNodesAction = new QAction(this);
    colapseAllNodesAction->setToolTip(tr("Collapse all nodes"));
    colapseAllNodesAction->setData(COLAPSE_ALL_NODES_ACTION_YL);
    colapseAllNodesAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(COLAPSE_ALL_NODES_ACTION_YL));
    colapseAllNodesAction->setIcon(QIcon(addExtensionToIconPath(":/images/sidebar/colapse")));

    optionsAction = new QAction(this);
    optionsAction->setToolTip(tr("Show options dialog"));
    optionsAction->setData(OPTIONS_ACTION_YL);
    optionsAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(OPTIONS_ACTION_YL));
    QIcon icoSettingsButton;
    icoSettingsButton.addFile(addExtensionToIconPath(":/images/main_toolbar/settings"), QSize(), QIcon::Normal);
    optionsAction->setIcon(icoSettingsButton);

    serverConfigAction = new QAction(this);
    serverConfigAction->setToolTip(tr("Show comics server options dialog"));
    serverConfigAction->setData(SERVER_CONFIG_ACTION_YL);
    serverConfigAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SERVER_CONFIG_ACTION_YL));
    QIcon icoServerButton;
    icoServerButton.addFile(addExtensionToIconPath(":/images/main_toolbar/server"), QSize(), QIcon::Normal);
    serverConfigAction->setIcon(icoServerButton);

    toggleComicsViewAction = new QAction(tr("Change between comics views"), this);
    toggleComicsViewAction->setToolTip(tr("Change between comics views"));
    QIcon icoViewsButton;

    if (!settings->contains(COMICS_VIEW_STATUS) || settings->value(COMICS_VIEW_STATUS) == Flow)
        icoViewsButton.addFile(addExtensionToIconPath(":/images/main_toolbar/grid"), QSize(), QIcon::Normal);
    else if (settings->value(COMICS_VIEW_STATUS) == Grid)
        icoViewsButton.addFile(addExtensionToIconPath(":/images/main_toolbar/info"), QSize(), QIcon::Normal);
    else
        icoViewsButton.addFile(addExtensionToIconPath(":/images/main_toolbar/flow"), QSize(), QIcon::Normal);

    toggleComicsViewAction->setData(TOGGLE_COMICS_VIEW_ACTION_YL);
    toggleComicsViewAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(TOGGLE_COMICS_VIEW_ACTION_YL));
    toggleComicsViewAction->setIcon(icoViewsButton);
    // socialAction = new QAction(this);

    //----

    openContainingFolderAction = new QAction(this);
    openContainingFolderAction->setText(tr("Open folder..."));
    openContainingFolderAction->setData(OPEN_CONTAINING_FOLDER_ACTION_YL);
    openContainingFolderAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(OPEN_CONTAINING_FOLDER_ACTION_YL));
    openContainingFolderAction->setIcon(QIcon(":/images/menus_icons/open_containing_folder.svg"));

    setFolderAsNotCompletedAction = new QAction(this);
    setFolderAsNotCompletedAction->setText(tr("Set as uncompleted"));
    setFolderAsNotCompletedAction->setData(SET_FOLDER_AS_NOT_COMPLETED_ACTION_YL);
    setFolderAsNotCompletedAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SET_FOLDER_AS_NOT_COMPLETED_ACTION_YL));

    setFolderAsCompletedAction = new QAction(this);
    setFolderAsCompletedAction->setText(tr("Set as completed"));
    setFolderAsCompletedAction->setData(SET_FOLDER_AS_COMPLETED_ACTION_YL);
    setFolderAsCompletedAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SET_FOLDER_AS_COMPLETED_ACTION_YL));

    setFolderAsReadAction = new QAction(this);
    setFolderAsReadAction->setText(tr("Set as read"));
    setFolderAsReadAction->setData(SET_FOLDER_AS_READ_ACTION_YL);
    setFolderAsReadAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SET_FOLDER_AS_READ_ACTION_YL));

    setFolderAsUnreadAction = new QAction(this);
    setFolderAsUnreadAction->setText(tr("Set as unread"));
    setFolderAsUnreadAction->setData(SET_FOLDER_AS_UNREAD_ACTION_YL);
    setFolderAsUnreadAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SET_FOLDER_AS_UNREAD_ACTION_YL));

    setFolderAsMangaAction = new QAction(this);
    setFolderAsMangaAction->setText(tr("manga"));
    setFolderAsMangaAction->setData(SET_FOLDER_AS_MANGA_ACTION_YL);
    setFolderAsMangaAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SET_FOLDER_AS_MANGA_ACTION_YL));

    setFolderAsNormalAction = new QAction(this);
    setFolderAsNormalAction->setText(tr("comic"));
    setFolderAsNormalAction->setData(SET_FOLDER_AS_NORMAL_ACTION_YL);
    setFolderAsNormalAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SET_FOLDER_AS_NORMAL_ACTION_YL));

    setFolderAsWesternMangaAction = new QAction(this);
    setFolderAsWesternMangaAction->setText(tr("western manga (left to right)"));
    setFolderAsWesternMangaAction->setData(SET_FOLDER_AS_WESTERN_MANGA_ACTION_YL);
    setFolderAsWesternMangaAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SET_FOLDER_AS_WESTERN_MANGA_ACTION_YL));

    setFolderAsWebComicAction = new QAction(this);
    setFolderAsWebComicAction->setText(tr("web comic"));
    setFolderAsWebComicAction->setData(SET_FOLDER_AS_WEB_COMIC_ACTION_YL);
    setFolderAsWebComicAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SET_FOLDER_AS_WEB_COMIC_ACTION_YL));

    setFolderAsYonkomaAction = new QAction(this);
    setFolderAsYonkomaAction->setText(tr("yonkoma"));
    setFolderAsYonkomaAction->setData(SET_FOLDER_AS_YONKOMA_ACTION_YL);
    setFolderAsYonkomaAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SET_FOLDER_AS_YONKOMA_ACTION_YL));

    //----

    openContainingFolderComicAction = new QAction(this);
    openContainingFolderComicAction->setText(tr("Open containing folder..."));
    openContainingFolderComicAction->setData(OPEN_CONTAINING_FOLDER_COMIC_ACTION_YL);
    openContainingFolderComicAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(OPEN_CONTAINING_FOLDER_COMIC_ACTION_YL));
    openContainingFolderComicAction->setIcon(QIcon(":/images/menus_icons/open_containing_folder.svg"));

    resetComicRatingAction = new QAction(this);
    resetComicRatingAction->setText(tr("Reset comic rating"));
    resetComicRatingAction->setData(RESET_COMIC_RATING_ACTION_YL);
    resetComicRatingAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(RESET_COMIC_RATING_ACTION_YL));

    // Edit comics actions------------------------------------------------------
    selectAllComicsAction = new QAction(this);
    selectAllComicsAction->setText(tr("Select all comics"));
    selectAllComicsAction->setData(SELECT_ALL_COMICS_ACTION_YL);
    selectAllComicsAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SELECT_ALL_COMICS_ACTION_YL));
    selectAllComicsAction->setIcon(QIcon(":/images/comics_view_toolbar/selectAll.svg"));

    editSelectedComicsAction = new QAction(this);
    editSelectedComicsAction->setText(tr("Edit"));
    editSelectedComicsAction->setData(EDIT_SELECTED_COMICS_ACTION_YL);
    editSelectedComicsAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(EDIT_SELECTED_COMICS_ACTION_YL));
    editSelectedComicsAction->setIcon(QIcon(":/images/comics_view_toolbar/editComic.svg"));

    asignOrderAction = new QAction(this);
    asignOrderAction->setText(tr("Assign current order to comics"));
    asignOrderAction->setData(ASIGN_ORDER_ACTION_YL);
    asignOrderAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(ASIGN_ORDER_ACTION_YL));
    asignOrderAction->setIcon(QIcon(":/images/comics_view_toolbar/asignNumber.svg"));

    forceCoverExtractedAction = new QAction(this);
    forceCoverExtractedAction->setText(tr("Update cover"));
    forceCoverExtractedAction->setData(FORCE_COVER_EXTRACTED_ACTION_YL);
    forceCoverExtractedAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(FORCE_COVER_EXTRACTED_ACTION_YL));
    forceCoverExtractedAction->setIcon(QIcon(":/images/importCover.png"));

    deleteComicsAction = new QAction(this);
    deleteComicsAction->setText(tr("Delete selected comics"));
    deleteComicsAction->setData(DELETE_COMICS_ACTION_YL);
    deleteComicsAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(DELETE_COMICS_ACTION_YL));
    deleteComicsAction->setIcon(QIcon(":/images/comics_view_toolbar/trash.svg"));

    deleteMetadataAction = new QAction(this);
    deleteMetadataAction->setText(tr("Delete metadata from selected comics"));
    deleteMetadataAction->setData(DELETE_METADATA_FROM_COMICS_ACTION_YL);
    deleteMetadataAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(DELETE_METADATA_FROM_COMICS_ACTION_YL));

    getInfoAction = new QAction(this);
    getInfoAction->setData(GET_INFO_ACTION_YL);
    getInfoAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(GET_INFO_ACTION_YL));
    getInfoAction->setText(tr("Download tags from Comic Vine"));
    getInfoAction->setIcon(QIcon(":/images/comics_view_toolbar/getInfo.svg"));
    //-------------------------------------------------------------------------

    focusSearchLineAction = new QAction(tr("Focus search line"), this);
    focusSearchLineAction->setData(FOCUS_SEARCH_LINE_ACTION_YL);
    focusSearchLineAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(FOCUS_SEARCH_LINE_ACTION_YL));
    focusSearchLineAction->setIcon(QIcon(":/images/iconSearch.png"));
    addAction(focusSearchLineAction);

    focusComicsViewAction = new QAction(tr("Focus comics view"), this);
    focusComicsViewAction->setData(FOCUS_COMICS_VIEW_ACTION_YL);
    focusComicsViewAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(FOCUS_COMICS_VIEW_ACTION_YL));
    addAction(focusComicsViewAction);

    showEditShortcutsAction = new QAction(tr("Edit shortcuts"), this);
    showEditShortcutsAction->setData(SHOW_EDIT_SHORTCUTS_ACTION_YL);
    showEditShortcutsAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SHOW_EDIT_SHORTCUTS_ACTION_YL));
    showEditShortcutsAction->setShortcutContext(Qt::ApplicationShortcut);
    addAction(showEditShortcutsAction);

    quitAction = new QAction(tr("&Quit"), this);
    quitAction->setIcon(QIcon(":/images/viewer_toolbar/close.svg"));
    quitAction->setData(QUIT_ACTION_YL);
    quitAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(QUIT_ACTION_YL));
    // TODO: is `quitAction->setMenuRole(QAction::QuitRole);` useful on macOS?
    addAction(quitAction);

    updateFolderAction = new QAction(tr("Update folder"), this);
    updateFolderAction->setIcon(QIcon(":/images/menus_icons/update_current_folder.svg"));

    updateCurrentFolderAction = new QAction(tr("Update current folder"), this);
    updateCurrentFolderAction->setData(UPDATE_CURRENT_FOLDER_ACTION_YL);
    updateCurrentFolderAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(UPDATE_CURRENT_FOLDER_ACTION_YL));
    updateCurrentFolderAction->setIcon(QIcon(":/images/menus_icons/update_current_folder.svg"));

    rescanXMLFromCurrentFolderAction = new QAction(tr("Scan legacy XML metadata"), this);
    rescanXMLFromCurrentFolderAction->setData(SCAN_XML_FROM_CURRENT_FOLDER_ACTION_YL);
    rescanXMLFromCurrentFolderAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(SCAN_XML_FROM_CURRENT_FOLDER_ACTION_YL));

    addReadingListAction = new QAction(tr("Add new reading list"), this);
    addReadingListAction->setData(ADD_READING_LIST_ACTION_YL);
    addReadingListAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(ADD_READING_LIST_ACTION_YL));
    addReadingListAction->setToolTip(tr("Add a new reading list to the current library"));
    addReadingListAction->setIcon(QIcon(addExtensionToIconPath(":/images/sidebar/addNew_sidebar")));

    deleteReadingListAction = new QAction(tr("Remove reading list"), this);
    deleteReadingListAction->setData(REMOVE_READING_LIST_ACTION_YL);
    deleteReadingListAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(REMOVE_READING_LIST_ACTION_YL));
    deleteReadingListAction->setToolTip(tr("Remove current reading list from the library"));
    deleteReadingListAction->setIcon(QIcon(addExtensionToIconPath(":/images/sidebar/delete_sidebar")));

    addLabelAction = new QAction(tr("Add new label"), this);
    addLabelAction->setData(ADD_LABEL_ACTION_YL);
    addLabelAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(ADD_LABEL_ACTION_YL));
    addLabelAction->setToolTip(tr("Add a new label to this library"));
    addLabelAction->setIcon(QIcon(addExtensionToIconPath(":/images/sidebar/addLabelIcon")));

    renameListAction = new QAction(tr("Rename selected list"), this);
    renameListAction->setData(RENAME_LIST_ACTION_YL);
    renameListAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(RENAME_LIST_ACTION_YL));
    renameListAction->setToolTip(tr("Rename any selected labels or lists"));
    renameListAction->setIcon(QIcon(addExtensionToIconPath(":/images/sidebar/renameListIcon")));

    //--
    addToMenuAction = new QAction(tr("Add to..."), this);

    addToFavoritesAction = new QAction(tr("Favorites"), this);
    addToFavoritesAction->setData(ADD_TO_FAVORITES_ACTION_YL);
    addToFavoritesAction->setShortcut(ShortcutsManager::getShortcutsManager().getShortcut(ADD_TO_FAVORITES_ACTION_YL));
    addToFavoritesAction->setToolTip(tr("Add selected comics to favorites list"));
    addToFavoritesAction->setIcon(QIcon(":/images/lists/default_1.svg"));

    // actions not asigned to any widget
    this->addAction(saveCoversToAction);
    this->addAction(openContainingFolderAction);
    this->addAction(updateCurrentFolderAction);
    this->addAction(resetComicRatingAction);
    this->addAction(setFolderAsCompletedAction);
    this->addAction(setFolderAsNotCompletedAction);
    this->addAction(setFolderAsReadAction);
    this->addAction(setFolderAsUnreadAction);
    this->addAction(setFolderAsMangaAction);
    this->addAction(setFolderAsNormalAction);
    this->addAction(setFolderAsWesternMangaAction);
    this->addAction(setFolderAsWebComicAction);
    this->addAction(setFolderAsYonkomaAction);
    this->addAction(deleteMetadataAction);
    this->addAction(rescanXMLFromCurrentFolderAction);
#ifndef Q_OS_MACOS
    this->addAction(toggleFullScreenAction);
#endif

    // disable actions
    disableAllActions();
}
void LibraryWindow::disableComicsActions(bool disabled)
{
    if (!disabled && librariesUpdateCoordinator->isRunning()) {
        disableComicsActions(true);
        return;
    }

    // if there aren't comics, no fullscreen option will be available
#ifndef Q_OS_MACOS
    toggleFullScreenAction->setDisabled(disabled);
#endif
    // edit toolbar
    openComicAction->setDisabled(disabled);
    editSelectedComicsAction->setDisabled(disabled);
    selectAllComicsAction->setDisabled(disabled);
    asignOrderAction->setDisabled(disabled);
    setAsReadAction->setDisabled(disabled);
    setAsNonReadAction->setDisabled(disabled);
    setNormalAction->setDisabled(disabled);
    setMangaAction->setDisabled(disabled);
    setWebComicAction->setDisabled(disabled);
    setWesternMangaAction->setDisabled(disabled);
    setYonkomaAction->setDisabled(disabled);
    // setAllAsReadAction->setDisabled(disabled);
    // setAllAsNonReadAction->setDisabled(disabled);
    showHideMarksAction->setDisabled(disabled);
    deleteMetadataAction->setDisabled(disabled);
    deleteComicsAction->setDisabled(disabled);
    // context menu
    openContainingFolderComicAction->setDisabled(disabled);
    resetComicRatingAction->setDisabled(disabled);

    getInfoAction->setDisabled(disabled);

    updateCurrentFolderAction->setDisabled(disabled);
}
void LibraryWindow::disableLibrariesActions(bool disabled)
{
    updateLibraryAction->setDisabled(disabled);
    renameLibraryAction->setDisabled(disabled);
    removeLibraryAction->setDisabled(disabled);
    exportComicsInfoAction->setDisabled(disabled);
    importComicsInfoAction->setDisabled(disabled);
    exportLibraryAction->setDisabled(disabled);
    rescanLibraryForXMLInfoAction->setDisabled(disabled);
    // importLibraryAction->setDisabled(disabled);
}

void LibraryWindow::disableNoUpdatedLibrariesActions(bool disabled)
{
    updateLibraryAction->setDisabled(disabled);
    exportComicsInfoAction->setDisabled(disabled);
    importComicsInfoAction->setDisabled(disabled);
    exportLibraryAction->setDisabled(disabled);
    rescanLibraryForXMLInfoAction->setDisabled(disabled);
}

void LibraryWindow::disableFoldersActions(bool disabled)
{
    setRootIndexAction->setDisabled(disabled);
    expandAllNodesAction->setDisabled(disabled);
    colapseAllNodesAction->setDisabled(disabled);

    openContainingFolderAction->setDisabled(disabled);

    updateFolderAction->setDisabled(disabled);
    rescanXMLFromCurrentFolderAction->setDisabled(disabled);
}

void LibraryWindow::disableAllActions()
{
    disableComicsActions(true);
    disableLibrariesActions(true);
    disableFoldersActions(true);
}

void LibraryWindow::createToolBars()
{

#ifdef Y_MAC_UI
    // libraryToolBar->setIconSize(QSize(16,16)); //TODO make icon size dynamic

    libraryToolBar->addAction(backAction);
    libraryToolBar->addAction(forwardAction);

    libraryToolBar->addSpace(10);

#ifdef SERVER_RELEASE
    libraryToolBar->addAction(serverConfigAction);
#endif
    libraryToolBar->addAction(optionsAction);
    libraryToolBar->addAction(helpAboutAction);

    libraryToolBar->addSpace(10);

    libraryToolBar->addAction(toggleComicsViewAction);

    libraryToolBar->addStretch();

    // Native toolbar search edit
    // libraryToolBar->addWidget(searchEdit);
    searchEdit = libraryToolBar->addSearchEdit();
    // connect(libraryToolBar,SIGNAL(searchTextChanged(YACReader::SearchModifiers,QString)),this,SLOT(setSearchFilter(YACReader::SearchModifiers, QString)));

    // libraryToolBar->setMovable(false);

    libraryToolBar->attachToWindow(this);

#else
    libraryToolBar->backButton->setDefaultAction(backAction);
    libraryToolBar->forwardButton->setDefaultAction(forwardAction);
    libraryToolBar->settingsButton->setDefaultAction(optionsAction);
    libraryToolBar->serverButton->setDefaultAction(serverConfigAction);
    libraryToolBar->helpButton->setDefaultAction(helpAboutAction);
    libraryToolBar->toggleComicsViewButton->setDefaultAction(toggleComicsViewAction);
#ifndef Q_OS_MACOS
    libraryToolBar->fullscreenButton->setDefaultAction(toggleFullScreenAction);
#endif
    libraryToolBar->setSearchWidget(searchEdit);
#endif

    editInfoToolBar->setIconSize(QSize(18, 18));
    editInfoToolBar->addAction(openComicAction);
    editInfoToolBar->addSeparator();
    editInfoToolBar->addAction(editSelectedComicsAction);
    editInfoToolBar->addAction(getInfoAction);
    editInfoToolBar->addAction(asignOrderAction);

    editInfoToolBar->addSeparator();

    editInfoToolBar->addAction(selectAllComicsAction);

    editInfoToolBar->addSeparator();

    editInfoToolBar->addAction(setAsReadAction);
    editInfoToolBar->addAction(setAsNonReadAction);

    editInfoToolBar->addAction(showHideMarksAction);

    editInfoToolBar->addSeparator();

    auto setTypeToolButton = new QToolButton();
    setTypeToolButton->addAction(setNormalAction);
    setTypeToolButton->addAction(setMangaAction);
    setTypeToolButton->addAction(setWesternMangaAction);
    setTypeToolButton->addAction(setWebComicAction);
    setTypeToolButton->addAction(setYonkomaAction);
    setTypeToolButton->setPopupMode(QToolButton::InstantPopup);
    setTypeToolButton->setDefaultAction(setNormalAction);
    editInfoToolBar->addWidget(setTypeToolButton);

    editInfoToolBar->addSeparator();

    editInfoToolBar->addAction(deleteComicsAction);

    auto toolBarStretch = new YACReaderToolBarStretch(this);
    editInfoToolBar->addWidget(toolBarStretch);

    editInfoToolBar->addAction(toogleShowRecentIndicatorAction);

    contentViewsManager->comicsView->setToolBar(editInfoToolBar);
}

void LibraryWindow::createMenus()
{
    foldersView->addAction(addFolderAction);
    foldersView->addAction(deleteFolderAction);
    YACReader::addSperator(foldersView);

    foldersView->addAction(openContainingFolderAction);
    foldersView->addAction(updateFolderAction);
    YACReader::addSperator(foldersView);

    foldersView->addAction(setFolderAsNotCompletedAction);
    foldersView->addAction(setFolderAsCompletedAction);
    YACReader::addSperator(foldersView);

    foldersView->addAction(setFolderAsReadAction);
    foldersView->addAction(setFolderAsUnreadAction);
    YACReader::addSperator(foldersView);

    foldersView->addAction(setFolderAsNormalAction);
    foldersView->addAction(setFolderAsMangaAction);
    foldersView->addAction(setFolderAsWesternMangaAction);
    foldersView->addAction(setFolderAsWebComicAction);
    foldersView->addAction(setFolderAsYonkomaAction);

    selectedLibrary->addAction(updateLibraryAction);
    selectedLibrary->addAction(renameLibraryAction);
    selectedLibrary->addAction(removeLibraryAction);
    YACReader::addSperator(selectedLibrary);

    selectedLibrary->addAction(rescanLibraryForXMLInfoAction);
    YACReader::addSperator(selectedLibrary);

    selectedLibrary->addAction(exportComicsInfoAction);
    selectedLibrary->addAction(importComicsInfoAction);
    YACReader::addSperator(selectedLibrary);

    selectedLibrary->addAction(exportLibraryAction);
    selectedLibrary->addAction(importLibraryAction);

// MacOSX app menus
#ifdef Q_OS_MACOS
    QMenuBar *menu = this->menuBar();
    // about / preferences
    // TODO

    // library
    QMenu *libraryMenu = new QMenu(tr("Library"));

    libraryMenu->addAction(updateLibraryAction);
    libraryMenu->addAction(renameLibraryAction);
    libraryMenu->addAction(removeLibraryAction);
    libraryMenu->addSeparator();

    libraryMenu->addAction(rescanLibraryForXMLInfoAction);
    libraryMenu->addSeparator();

    libraryMenu->addAction(exportComicsInfoAction);
    libraryMenu->addAction(importComicsInfoAction);

    libraryMenu->addSeparator();

    libraryMenu->addAction(exportLibraryAction);
    libraryMenu->addAction(importLibraryAction);

    // folder
    QMenu *folderMenu = new QMenu(tr("Folder"));
    folderMenu->addAction(openContainingFolderAction);
    folderMenu->addAction(updateFolderAction);
    folderMenu->addSeparator();
    folderMenu->addAction(rescanXMLFromCurrentFolderAction);
    folderMenu->addSeparator();
    folderMenu->addAction(setFolderAsNotCompletedAction);
    folderMenu->addAction(setFolderAsCompletedAction);
    folderMenu->addSeparator();
    folderMenu->addAction(setFolderAsReadAction);
    folderMenu->addAction(setFolderAsUnreadAction);
    folderMenu->addSeparator();
    foldersView->addAction(setFolderAsNormalAction);
    foldersView->addAction(setFolderAsMangaAction);
    foldersView->addAction(setFolderAsWesternMangaAction);
    foldersView->addAction(setFolderAsWebComicAction);
    foldersView->addAction(setFolderAsYonkomaAction);

    // comic
    QMenu *comicMenu = new QMenu(tr("Comic"));
    comicMenu->addAction(openContainingFolderComicAction);
    comicMenu->addSeparator();
    comicMenu->addAction(resetComicRatingAction);

    menu->addMenu(libraryMenu);
    menu->addMenu(folderMenu);
    menu->addMenu(comicMenu);
#endif
}

void LibraryWindow::createConnections()
{
    // history navigation
    connect(backAction, &QAction::triggered, historyController, &YACReaderHistoryController::backward);
    connect(forwardAction, &QAction::triggered, historyController, &YACReaderHistoryController::forward);
    //--
    connect(historyController, &YACReaderHistoryController::enabledBackward, backAction, &QAction::setEnabled);
    connect(historyController, &YACReaderHistoryController::enabledForward, forwardAction, &QAction::setEnabled);
    // connect(foldersView, SIGNAL(clicked(QModelIndex)), historyController, SLOT(updateHistory(QModelIndex)));

    // libraryCreator connections
    connect(createLibraryDialog, &CreateLibraryDialog::createLibrary, this, QOverload<QString, QString, QString>::of(&LibraryWindow::create));
    connect(createLibraryDialog, &CreateLibraryDialog::libraryExists, this, &LibraryWindow::libraryAlreadyExists);
    connect(importComicsInfoDialog, &QDialog::finished, this, &LibraryWindow::reloadCurrentLibrary);

    // connect(libraryCreator,SIGNAL(coverExtracted(QString)),createLibraryDialog,SLOT(showCurrentFile(QString)));
    // connect(libraryCreator,SIGNAL(coverExtracted(QString)),updateLibraryDialog,SLOT(showCurrentFile(QString)));
    connect(libraryCreator, &LibraryCreator::finished, this, &LibraryWindow::showRootWidget);
    connect(libraryCreator, &LibraryCreator::updated, this, &LibraryWindow::reloadCurrentLibrary);
    connect(libraryCreator, &LibraryCreator::created, this, &LibraryWindow::openLastCreated);
    // connect(libraryCreator,SIGNAL(updatedCurrentFolder()), this, SLOT(showRootWidget()));
    connect(libraryCreator, &LibraryCreator::updatedCurrentFolder, this, &LibraryWindow::reloadAfterCopyMove);
    connect(libraryCreator, &LibraryCreator::comicAdded, importWidget, &ImportWidget::newComic);
    // libraryCreator errors
    connect(libraryCreator, &LibraryCreator::failedCreatingDB, this, &LibraryWindow::manageCreatingError);
    // connect(libraryCreator, SIGNAL(failedUpdatingDB(QString)), this, SLOT(manageUpdatingError(QString))); // TODO: implement failedUpdatingDB

    connect(xmlInfoLibraryScanner, &QThread::finished, this, &LibraryWindow::showRootWidget);
    connect(xmlInfoLibraryScanner, &QThread::finished, this, &LibraryWindow::reloadCurrentFolderComicsContent);
    connect(xmlInfoLibraryScanner, &XMLInfoLibraryScanner::comicScanned, importWidget, &ImportWidget::newComic);

    // new import widget
    connect(importWidget, &ImportWidget::stop, this, &LibraryWindow::stopLibraryCreator);
    connect(importWidget, &ImportWidget::stop, this, &LibraryWindow::stopXMLScanning);

    // packageManager connections
    connect(exportLibraryDialog, &ExportLibraryDialog::exportPath, this, &LibraryWindow::exportLibrary);
    connect(exportLibraryDialog, &QDialog::rejected, packageManager, &PackageManager::cancel);
    connect(packageManager, &PackageManager::exported, exportLibraryDialog, &ExportLibraryDialog::close);
    connect(importLibraryDialog, &ImportLibraryDialog::unpackCLC, this, &LibraryWindow::importLibrary);
    connect(importLibraryDialog, &QDialog::rejected, packageManager, &PackageManager::cancel);
    connect(importLibraryDialog, &QDialog::rejected, this, &LibraryWindow::deleteCurrentLibrary);
    connect(importLibraryDialog, &ImportLibraryDialog::libraryExists, this, &LibraryWindow::libraryAlreadyExists);
    connect(packageManager, &PackageManager::imported, importLibraryDialog, &QWidget::hide);
    connect(packageManager, &PackageManager::imported, this, &LibraryWindow::openLastCreated);

    // create and update dialogs
    connect(createLibraryDialog, &CreateLibraryDialog::cancelCreate, this, &LibraryWindow::cancelCreating);

    // open existing library from dialog.
    connect(addLibraryDialog, &AddLibraryDialog::addLibrary, this, &LibraryWindow::openLibrary);

    // load library when selected library changes
    connect(selectedLibrary, &YACReaderLibraryListWidget::currentIndexChanged, this, &LibraryWindow::loadLibrary);

    // rename library dialog
    connect(renameLibraryDialog, &RenameLibraryDialog::renameLibrary, this, &LibraryWindow::rename);

    // navigations between view modes (tree,list and flow)
    // TODO connect(foldersView, SIGNAL(pressed(QModelIndex)), this, SLOT(updateFoldersViewConextMenu(QModelIndex)));
    // connect(foldersView, SIGNAL(clicked(QModelIndex)), this, SLOT(loadCovers(QModelIndex)));

    // drops in folders view
    connect(foldersView, QOverload<QList<QPair<QString, QString>>, QModelIndex>::of(&YACReaderFoldersView::copyComicsToFolder),
            this, &LibraryWindow::copyAndImportComicsToFolder);
    connect(foldersView, QOverload<QList<QPair<QString, QString>>, QModelIndex>::of(&YACReaderFoldersView::moveComicsToFolder),
            this, &LibraryWindow::moveAndImportComicsToFolder);
    connect(foldersView, &QWidget::customContextMenuRequested, this, &LibraryWindow::showFoldersContextMenu);

    // actions
    connect(createLibraryAction, &QAction::triggered, this, &LibraryWindow::createLibrary);
    connect(exportLibraryAction, &QAction::triggered, exportLibraryDialog, &ExportLibraryDialog::open);
    connect(importLibraryAction, &QAction::triggered, this, &LibraryWindow::importLibraryPackage);

    connect(openLibraryAction, &QAction::triggered, this, &LibraryWindow::showAddLibrary);
    connect(setAsReadAction, &QAction::triggered, this, &LibraryWindow::setCurrentComicReaded);
    connect(setAsNonReadAction, &QAction::triggered, this, &LibraryWindow::setCurrentComicUnreaded);

    connect(setNormalAction, &QAction::triggered, this, [=]() {
        setSelectedComicsType(FileType::Comic);
    });
    connect(setMangaAction, &QAction::triggered, this, [=]() {
        setSelectedComicsType(FileType::Manga);
    });
    connect(setWesternMangaAction, &QAction::triggered, this, [=]() {
        setSelectedComicsType(FileType::WesternManga);
    });
    connect(setWebComicAction, &QAction::triggered, this, [=]() {
        setSelectedComicsType(FileType::WebComic);
    });
    connect(setYonkomaAction, &QAction::triggered, this, [=]() {
        setSelectedComicsType(FileType::Yonkoma);
    });

    // comicsInfoManagement
    connect(exportComicsInfoAction, &QAction::triggered, this, &LibraryWindow::showExportComicsInfo);
    connect(importComicsInfoAction, &QAction::triggered, this, &LibraryWindow::showImportComicsInfo);

    // properties & config
    connect(propertiesDialog, &QDialog::accepted, contentViewsManager, &YACReaderContentViewsManager::updateCurrentContentView);
    connect(propertiesDialog, &PropertiesDialog::coverChangedSignal, this, [=](const ComicDB &comic) {
        comicsModel->notifyCoverChange(comic);
    });

    // comic vine
    connect(comicVineDialog, &QDialog::accepted, contentViewsManager, &YACReaderContentViewsManager::updateCurrentContentView, Qt::QueuedConnection);

    connect(updateLibraryAction, &QAction::triggered, this, &LibraryWindow::updateLibrary);
    connect(renameLibraryAction, &QAction::triggered, this, &LibraryWindow::renameLibrary);
    // connect(deleteLibraryAction,SIGNAL(triggered()),this,SLOT(deleteLibrary()));
    connect(removeLibraryAction, &QAction::triggered, this, &LibraryWindow::removeLibrary);
    connect(rescanLibraryForXMLInfoAction, &QAction::triggered, this, &LibraryWindow::rescanLibraryForXMLInfo);
    connect(openComicAction, &QAction::triggered, this, QOverload<>::of(&LibraryWindow::openComic));
    connect(helpAboutAction, &QAction::triggered, had, &QWidget::show);
    connect(addFolderAction, &QAction::triggered, this, &LibraryWindow::addFolderToCurrentIndex);
    connect(deleteFolderAction, &QAction::triggered, this, &LibraryWindow::deleteSelectedFolder);
    connect(setRootIndexAction, &QAction::triggered, this, &LibraryWindow::setRootIndex);
    connect(expandAllNodesAction, &QAction::triggered, foldersView, &QTreeView::expandAll);
    connect(colapseAllNodesAction, &QAction::triggered, foldersView, &QTreeView::collapseAll);
#ifndef Q_OS_MACOS
    connect(toggleFullScreenAction, &QAction::triggered, this, &LibraryWindow::toggleFullScreen);
#endif
    connect(toggleComicsViewAction, &QAction::triggered, contentViewsManager, &YACReaderContentViewsManager::toggleComicsView);
    connect(optionsAction, &QAction::triggered, optionsDialog, &QWidget::show);
#ifdef SERVER_RELEASE
    connect(serverConfigAction, &QAction::triggered, serverConfigDialog, &QWidget::show);
#endif
    connect(optionsDialog, &YACReaderOptionsDialog::optionsChanged, this, &LibraryWindow::reloadOptions);
    connect(optionsDialog, &YACReaderOptionsDialog::editShortcuts, editShortcutsDialog, &QWidget::show);

    auto searchDebouncer = new KDToolBox::KDSignalDebouncer(this);
    searchDebouncer->setTimeout(400);

// Search filter
#ifdef Y_MAC_UI
    connect(searchEdit, &YACReaderMacOSXSearchLineEdit::filterChanged, searchDebouncer, &KDToolBox::KDSignalThrottler::throttle);
    connect(searchDebouncer, &KDToolBox::KDSignalThrottler::triggered, this, [=] {
        setSearchFilter(searchEdit->text());
    });
#else
    connect(searchEdit, &YACReaderSearchLineEdit::filterChanged, searchDebouncer, &KDToolBox::KDSignalThrottler::throttle);
    connect(searchDebouncer, &KDToolBox::KDSignalThrottler::triggered, this, [=] {
        setSearchFilter(searchEdit->text());
    });
#endif
    connect(&comicQueryResultProcessor, &ComicQueryResultProcessor::newData, this, &LibraryWindow::setComicSearchFilterData);
    qRegisterMetaType<FolderItem *>("FolderItem *");
    qRegisterMetaType<QMap<unsigned long long int, FolderItem *> *>("QMap<unsigned long long int, FolderItem *> *");
    connect(folderQueryResultProcessor.get(), &FolderQueryResultProcessor::newData, this, &LibraryWindow::setFolderSearchFilterData);

    // ContextMenus
    connect(openContainingFolderComicAction, &QAction::triggered, this, &LibraryWindow::openContainingFolderComic);
    connect(setFolderAsNotCompletedAction, &QAction::triggered, this, &LibraryWindow::setFolderAsNotCompleted);
    connect(setFolderAsCompletedAction, &QAction::triggered, this, &LibraryWindow::setFolderAsCompleted);
    connect(setFolderAsReadAction, &QAction::triggered, this, &LibraryWindow::setFolderAsRead);
    connect(setFolderAsUnreadAction, &QAction::triggered, this, &LibraryWindow::setFolderAsUnread);
    connect(openContainingFolderAction, &QAction::triggered, this, &LibraryWindow::openContainingFolder);

    connect(setFolderAsMangaAction, &QAction::triggered, this, [=]() {
        setFolderType(FileType::Manga);
    });
    connect(setFolderAsNormalAction, &QAction::triggered, this, [=]() {
        setFolderType(FileType::Comic);
    });
    connect(setFolderAsWesternMangaAction, &QAction::triggered, this, [=]() {
        setFolderType(FileType::WesternManga);
    });
    connect(setFolderAsWebComicAction, &QAction::triggered, this, [=]() {
        setFolderType(FileType::WebComic);
    });
    connect(setFolderAsYonkomaAction, &QAction::triggered, this, [=]() {
        setFolderType(FileType::Yonkoma);
    });

    connect(resetComicRatingAction, &QAction::triggered, this, &LibraryWindow::resetComicRating);

    // Comicts edition
    connect(editSelectedComicsAction, &QAction::triggered, this, &LibraryWindow::showProperties);
    connect(asignOrderAction, &QAction::triggered, this, &LibraryWindow::asignNumbers);

    connect(deleteMetadataAction, &QAction::triggered, this, &LibraryWindow::deleteMetadataFromSelectedComics);

    connect(deleteComicsAction, &QAction::triggered, this, &LibraryWindow::deleteComics);

    connect(getInfoAction, &QAction::triggered, this, &LibraryWindow::showComicVineScraper);

    connect(focusSearchLineAction, &QAction::triggered, searchEdit, [this] { searchEdit->setFocus(Qt::ShortcutFocusReason); });
    connect(focusComicsViewAction, &QAction::triggered, contentViewsManager, &YACReaderContentViewsManager::focusComicsViewViaShortcut);

    connect(showEditShortcutsAction, &QAction::triggered, editShortcutsDialog, &QWidget::show);

    connect(quitAction, &QAction::triggered, this, &LibraryWindow::closeApp);

    // update folders (partial updates)
    connect(updateCurrentFolderAction, &QAction::triggered, this, &LibraryWindow::updateCurrentFolder);
    connect(updateFolderAction, &QAction::triggered, this, &LibraryWindow::updateCurrentFolder);

    connect(rescanXMLFromCurrentFolderAction, &QAction::triggered, this, &LibraryWindow::rescanCurrentFolderForXMLInfo);

    // lists
    connect(addReadingListAction, &QAction::triggered, this, &LibraryWindow::addNewReadingList);
    connect(deleteReadingListAction, &QAction::triggered, this, &LibraryWindow::deleteSelectedReadingList);
    connect(addLabelAction, &QAction::triggered, this, &LibraryWindow::showAddNewLabelDialog);
    connect(renameListAction, &QAction::triggered, this, &LibraryWindow::showRenameCurrentList);

    connect(listsModel, &ReadingListModel::addComicsToFavorites, comicsModel, QOverload<const QList<qulonglong> &>::of(&ComicModel::addComicsToFavorites));
    connect(listsModel, &ReadingListModel::addComicsToLabel, comicsModel, QOverload<const QList<qulonglong> &, qulonglong>::of(&ComicModel::addComicsToLabel));
    connect(listsModel, &ReadingListModel::addComicsToReadingList, comicsModel, QOverload<const QList<qulonglong> &, qulonglong>::of(&ComicModel::addComicsToReadingList));
    //--

    connect(addToFavoritesAction, &QAction::triggered, this, &LibraryWindow::addSelectedComicsToFavorites);

    // save covers
    connect(saveCoversToAction, &QAction::triggered, this, &LibraryWindow::saveSelectedCoversTo);

    // upgrade library
    connect(this, &LibraryWindow::libraryUpgraded, this, &LibraryWindow::loadLibrary, Qt::QueuedConnection);
    connect(this, &LibraryWindow::errorUpgradingLibrary, this, &LibraryWindow::showErrorUpgradingLibrary, Qt::QueuedConnection);

    connect(toogleShowRecentIndicatorAction, &QAction::toggled, recentVisibilityCoordinator, &RecentVisibilityCoordinator::toggleVisibility);
}

void LibraryWindow::showErrorUpgradingLibrary(const QString &path)
{
    QMessageBox::critical(this, tr("Upgrade failed"), tr("There were errors during library upgrade in: ") + path + "/library.ydb");
}

void LibraryWindow::loadLibrary(const QString &name)
{
    if (!libraries.isEmpty()) // si hay bibliotecas...
    {
        historyController->clear();

        showRootWidget();
        QString path = libraries.getPath(name) + "/.yacreaderlibrary";
        QDir d; // TODO change this by static methods (utils class?? with delTree for example)
        QString dbVersion;
        if (d.exists(path) && d.exists(path + "/library.ydb") && (dbVersion = DataBaseManagement::checkValidDB(path + "/library.ydb")) != "") // si existe en disco la biblioteca seleccionada, y es válida..
        {
            int comparation = DataBaseManagement::compareVersions(dbVersion, DB_VERSION);

            if (comparation < 0) {
                int ret = QMessageBox::question(this, tr("Update needed"), tr("This library was created with a previous version of YACReaderLibrary. It needs to be updated. Update now?"), QMessageBox::Yes, QMessageBox::No);
                if (ret == QMessageBox::Yes) {
                    importWidget->setUpgradeLook();
                    showImportingWidget();

                    upgradeLibraryFuture = std::async(std::launch::async, [this, name, path] {
                        bool updated = DataBaseManagement::updateToCurrentVersion(path);

                        if (!updated)
                            emit errorUpgradingLibrary(path);

                        emit libraryUpgraded(name);
                    });

                    return;
                } else {
                    contentViewsManager->comicsView->setModel(NULL);
                    foldersView->setModel(NULL);
                    listsView->setModel(NULL);
                    disableAllActions(); // TODO comprobar que se deben deshabilitar
                    // será possible renombrar y borrar estas bibliotecas
                    renameLibraryAction->setEnabled(true);
                    removeLibraryAction->setEnabled(true);
                }
            }

            if (comparation == 0) // en caso de que la versión se igual que la actual
            {
                foldersModel->setupModelData(path);
                foldersModelProxy->setSourceModel(foldersModel);
                foldersView->setModel(foldersModelProxy);
                foldersView->setCurrentIndex(QModelIndex()); // why is this necesary?? by default it seems that returns an arbitrary index.

                listsModel->setupReadingListsData(path);
                listsModelProxy->setSourceModel(listsModel);
                listsView->setModel(listsModelProxy);

                if (foldersModel->rowCount(QModelIndex()) > 0)
                    disableFoldersActions(false);
                else
                    disableFoldersActions(true);

                d.setCurrent(libraries.getPath(name));
                d.setFilter(QDir::AllDirs | QDir::Files | QDir::Hidden | QDir::NoSymLinks | QDir::NoDotAndDotDot);
                if (d.count() <= 1) // read only library
                {
                    disableLibrariesActions(false);
                    updateLibraryAction->setDisabled(true);
                    openContainingFolderAction->setDisabled(true);
                    rescanLibraryForXMLInfoAction->setDisabled(true);

                    disableComicsActions(true);
#ifndef Q_OS_MACOS
                    toggleFullScreenAction->setEnabled(true);
#endif

                    importedCovers = true;
                } else // librería normal abierta
                {
                    disableLibrariesActions(false);
                    importedCovers = false;
                }

                setRootIndex();

                searchEdit->clear();
            } else if (comparation > 0) {
                int ret = QMessageBox::question(this, tr("Download new version"), tr("This library was created with a newer version of YACReaderLibrary. Download the new version now?"), QMessageBox::Yes, QMessageBox::No);
                if (ret == QMessageBox::Yes)
                    QDesktopServices::openUrl(QUrl("http://www.yacreader.com"));

                contentViewsManager->comicsView->setModel(NULL);
                foldersView->setModel(NULL);
                listsView->setModel(NULL);
                disableAllActions(); // TODO comprobar que se deben deshabilitar
                // será possible renombrar y borrar estas bibliotecas
                renameLibraryAction->setEnabled(true);
                removeLibraryAction->setEnabled(true);
            }
        } else {
            contentViewsManager->comicsView->setModel(NULL);
            foldersView->setModel(NULL);
            listsView->setModel(NULL);
            disableAllActions(); // TODO comprobar que se deben deshabilitar

            // si la librería no existe en disco, se ofrece al usuario la posibiliad de eliminarla
            if (!d.exists(path)) {
                QString currentLibrary = selectedLibrary->currentText() + " -> " + libraries.getPath(name);
                if (QMessageBox::question(this, tr("Library not available"), tr("Library '%1' is no longer available. Do you want to remove it?").arg(currentLibrary), QMessageBox::Yes, QMessageBox::No) == QMessageBox::Yes) {
                    deleteCurrentLibrary();
                }
                // será possible renombrar y borrar estas bibliotecas
                renameLibraryAction->setEnabled(true);
                removeLibraryAction->setEnabled(true);

            } else // si existe el path, puede ser que la librería sea alguna versión pre-5.0 ó que esté corrupta o que no haya drivers sql
            {

                if (d.exists(path + "/library.ydb")) {
                    QSqlDatabase db = DataBaseManagement::loadDatabase(path);
                    manageOpeningLibraryError(db.lastError().databaseText() + "-" + db.lastError().driverText());
                    // será possible renombrar y borrar estas bibliotecas
                    renameLibraryAction->setEnabled(true);
                    removeLibraryAction->setEnabled(true);
                } else {
                    QString currentLibrary = selectedLibrary->currentText();
                    QString path = libraries.getPath(selectedLibrary->currentText());
                    if (QMessageBox::question(this, tr("Old library"), tr("Library '%1' has been created with an older version of YACReaderLibrary. It must be created again. Do you want to create the library now?").arg(currentLibrary), QMessageBox::Yes, QMessageBox::No) == QMessageBox::Yes) {
                        QDir d(path + "/.yacreaderlibrary");
                        d.removeRecursively();
                        // d.rmdir(path+"/.yacreaderlibrary");
                        createLibraryDialog->setDataAndStart(currentLibrary, path);
                        // create(path,path+"/.yacreaderlibrary",currentLibrary);
                    }
                    // será possible renombrar y borrar estas bibliotecas
                    renameLibraryAction->setEnabled(true);
                    removeLibraryAction->setEnabled(true);
                }
            }
        }
    } else // en caso de que no exista ninguna biblioteca se desactivan los botones pertinentes
    {
        disableAllActions();
        showNoLibrariesWidget();
    }
}

void LibraryWindow::loadCoversFromCurrentModel()
{
    contentViewsManager->comicsView->setModel(comicsModel);
}

void LibraryWindow::copyAndImportComicsToCurrentFolder(const QList<QPair<QString, QString>> &comics)
{
    QLOG_DEBUG() << "-copyAndImportComicsToCurrentFolder-";
    if (comics.size() > 0) {
        QString destFolderPath = currentFolderPath();

        QModelIndex folderDestination = getCurrentFolderIndex();

        QProgressDialog *progressDialog = newProgressDialog(tr("Copying comics..."), comics.size());

        auto comicFilesManager = new ComicFilesManager();
        comicFilesManager->copyComicsTo(comics, destFolderPath, folderDestination);

        processComicFiles(comicFilesManager, progressDialog);
    }
}

void LibraryWindow::moveAndImportComicsToCurrentFolder(const QList<QPair<QString, QString>> &comics)
{
    QLOG_DEBUG() << "-moveAndImportComicsToCurrentFolder-";
    if (comics.size() > 0) {
        QString destFolderPath = currentFolderPath();

        QModelIndex folderDestination = getCurrentFolderIndex();

        QProgressDialog *progressDialog = newProgressDialog(tr("Moving comics..."), comics.size());

        auto comicFilesManager = new ComicFilesManager();
        comicFilesManager->moveComicsTo(comics, destFolderPath, folderDestination);

        processComicFiles(comicFilesManager, progressDialog);
    }
}

void LibraryWindow::copyAndImportComicsToFolder(const QList<QPair<QString, QString>> &comics, const QModelIndex &miFolder)
{
    QLOG_DEBUG() << "-copyAndImportComicsToFolder-";
    if (comics.size() > 0) {
        QModelIndex folderDestination = foldersModelProxy->mapToSource(miFolder);

        QString destFolderPath = QDir::cleanPath(currentPath() + foldersModel->getFolderPath(folderDestination));

        QLOG_DEBUG() << "Coping to " << destFolderPath;

        QProgressDialog *progressDialog = newProgressDialog(tr("Copying comics..."), comics.size());

        auto comicFilesManager = new ComicFilesManager();
        comicFilesManager->copyComicsTo(comics, destFolderPath, folderDestination);

        processComicFiles(comicFilesManager, progressDialog);
    }
}

void LibraryWindow::moveAndImportComicsToFolder(const QList<QPair<QString, QString>> &comics, const QModelIndex &miFolder)
{
    QLOG_DEBUG() << "-moveAndImportComicsToFolder-";
    if (comics.size() > 0) {
        QModelIndex folderDestination = foldersModelProxy->mapToSource(miFolder);

        QString destFolderPath = QDir::cleanPath(currentPath() + foldersModel->getFolderPath(folderDestination));

        QLOG_DEBUG() << "Moving to " << destFolderPath;

        QProgressDialog *progressDialog = newProgressDialog(tr("Moving comics..."), comics.size());

        auto comicFilesManager = new ComicFilesManager();
        comicFilesManager->moveComicsTo(comics, destFolderPath, folderDestination);

        processComicFiles(comicFilesManager, progressDialog);
    }
}

void LibraryWindow::processComicFiles(ComicFilesManager *comicFilesManager, QProgressDialog *progressDialog)
{
    connect(comicFilesManager, &ComicFilesManager::progress, progressDialog, &QProgressDialog::setValue);

    QThread *thread = NULL;

    thread = new QThread();

    comicFilesManager->moveToThread(thread);

    connect(progressDialog, &QProgressDialog::canceled, comicFilesManager, &ComicFilesManager::cancel, Qt::DirectConnection);

    connect(thread, &QThread::started, comicFilesManager, &ComicFilesManager::process);
    connect(comicFilesManager, &ComicFilesManager::success, this, &LibraryWindow::updateCopyMoveFolderDestination);
    connect(comicFilesManager, &ComicFilesManager::finished, thread, &QThread::quit);
    connect(comicFilesManager, &ComicFilesManager::finished, comicFilesManager, &QObject::deleteLater);
    connect(comicFilesManager, &ComicFilesManager::finished, progressDialog, &QWidget::close);
    connect(comicFilesManager, &ComicFilesManager::finished, progressDialog, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    if (thread != NULL)
        thread->start();
}

void LibraryWindow::updateCopyMoveFolderDestination(const QModelIndex &mi)
{
    updateFolder(mi);
}

void LibraryWindow::updateCurrentFolder()
{
    updateFolder(getCurrentFolderIndex());
}

void LibraryWindow::updateFolder(const QModelIndex &miFolder)
{
    QLOG_DEBUG() << "UPDATE FOLDER!!!!";

    importWidget->setUpdateLook();
    showImportingWidget();

    QString currentLibrary = selectedLibrary->currentText();
    QString path = libraries.getPath(currentLibrary);
    _lastAdded = currentLibrary;
    libraryCreator->updateFolder(QDir::cleanPath(path), QDir::cleanPath(path + "/.yacreaderlibrary"), QDir::cleanPath(currentPath() + foldersModel->getFolderPath(miFolder)), miFolder);
    libraryCreator->start();
}

QProgressDialog *LibraryWindow::newProgressDialog(const QString &label, int maxValue)
{
    QProgressDialog *progressDialog = new QProgressDialog(label, "Cancel", 0, maxValue, this);
    progressDialog->setWindowModality(Qt::WindowModal);
    progressDialog->setMinimumWidth(350);
    progressDialog->show();
    return progressDialog;
}

void LibraryWindow::reloadCurrentFolderComicsContent()
{
    navigationController->loadFolderInfo(getCurrentFolderIndex());

    enableNeededActions();
}

void LibraryWindow::reloadAfterCopyMove(const QModelIndex &mi)
{
    if (getCurrentFolderIndex() == mi) {
        auto item = static_cast<FolderItem *>(mi.internalPointer());

        if (item == nullptr) {
            foldersModel->reload();
        } else {
            foldersModel->reload(mi);
        }

        contentViewsManager->updateCurrentContentView();
    }

    enableNeededActions();
}

QModelIndex LibraryWindow::getCurrentFolderIndex()
{
    if (foldersView->selectionModel()->selectedRows().length() > 0)
        return foldersModelProxy->mapToSource(foldersView->currentIndex());
    else
        return QModelIndex();
}

void LibraryWindow::enableNeededActions()
{
    if (foldersModel->rowCount(QModelIndex()) > 0)
        disableFoldersActions(false);

    if (comicsModel->rowCount() > 0)
        disableComicsActions(false);

    disableLibrariesActions(false);
}

void LibraryWindow::addFolderToCurrentIndex()
{
    exitSearchMode(); // Creating a folder in search mode is broken => exit it.

    QModelIndex currentIndex = getCurrentFolderIndex();

    bool ok;
    QString newFolderName = QInputDialog::getText(this, tr("Add new folder"),
                                                  tr("Folder name:"), QLineEdit::Normal,
                                                  "", &ok);

    // chars not supported in a folder's name: / \ : * ? " < > |
    QRegularExpression invalidChars("\\/\\:\\*\\?\\\"\\<\\>\\|\\\\"); // TODO this regexp is not properly written
    bool isValid = !newFolderName.contains(invalidChars);

    if (ok && !newFolderName.isEmpty() && isValid) {
        QString parentPath = QDir::cleanPath(currentPath() + foldersModel->getFolderPath(currentIndex));
        QDir parentDir(parentPath);
        QDir newFolder(parentPath + "/" + newFolderName);
        if (parentDir.mkdir(newFolderName) || newFolder.exists()) {
            QModelIndex newIndex = foldersModel->addFolderAtParent(newFolderName, currentIndex);
            foldersView->setCurrentIndex(foldersModelProxy->mapFromSource(newIndex));
            navigationController->loadFolderInfo(newIndex);
            historyController->updateHistory(YACReaderLibrarySourceContainer(newIndex, YACReaderLibrarySourceContainer::Folder));
            // a new folder is always an empty folder
            contentViewsManager->showFolderContentView();
        }
    }
}

void LibraryWindow::deleteSelectedFolder()
{
    QModelIndex currentIndex = getCurrentFolderIndex();
    QString relativePath = foldersModel->getFolderPath(currentIndex);
    QString folderPath = QDir::cleanPath(currentPath() + relativePath);

    if (!currentIndex.isValid())
        QMessageBox::information(this, tr("No folder selected"), tr("Please, select a folder first"));
    else {
        QString libraryPath = QDir::cleanPath(currentPath());
        if ((libraryPath == folderPath) || relativePath.isEmpty() || relativePath == "/")
            QMessageBox::critical(this, tr("Error in path"), tr("There was an error accessing the folder's path"));
        else {
            int ret = QMessageBox::question(this, tr("Delete folder"), tr("The selected folder and all its contents will be deleted from your disk. Are you sure?") + "\n\nFolder : " + folderPath, QMessageBox::Yes, QMessageBox::No);

            if (ret == QMessageBox::Yes) {
                // no folders multiselection by now
                QModelIndexList indexList;
                indexList << currentIndex;

                QList<QString> paths;
                paths << folderPath;

                auto remover = new FoldersRemover(indexList, paths);
                const auto thread = new QThread(this);
                moveAndConnectRemoverToThread(remover, thread);

                connect(remover, &FoldersRemover::remove, foldersModel, &FolderModel::deleteFolder);
                connect(remover, &FoldersRemover::removeError, this, &LibraryWindow::errorDeletingFolder);
                connect(remover, &FoldersRemover::finished, navigationController, &YACReaderNavigationController::reselectCurrentFolder);

                thread->start();
            }
        }
    }
}

void LibraryWindow::errorDeletingFolder()
{
    QMessageBox::critical(this, tr("Unable to delete"), tr("There was an issue trying to delete the selected folders. Please, check for write permissions and be sure that any applications are using these folders or any of the contained files."));
}

void LibraryWindow::addNewReadingList()
{
    QModelIndexList selectedLists = listsView->selectionModel()->selectedIndexes();
    QModelIndex sourceMI;
    if (!selectedLists.isEmpty())
        sourceMI = listsModelProxy->mapToSource(selectedLists.at(0));

    if (selectedLists.isEmpty() || !listsModel->isReadingSubList(sourceMI)) {
        bool ok;
        QString newListName = QInputDialog::getText(this, tr("Add new reading lists"),
                                                    tr("List name:"), QLineEdit::Normal,
                                                    "", &ok);
        if (ok) {
            if (selectedLists.isEmpty() || !listsModel->isReadingList(sourceMI))
                listsModel->addReadingList(newListName); // top level
            else {
                listsModel->addReadingListAt(newListName, sourceMI); // sublist
            }
        }
    }
}

void LibraryWindow::deleteSelectedReadingList()
{
    QModelIndexList selectedLists = listsView->selectionModel()->selectedIndexes();
    if (!selectedLists.isEmpty()) {
        QModelIndex mi = listsModelProxy->mapToSource(selectedLists.at(0));
        if (listsModel->isEditable(mi)) {
            int ret = QMessageBox::question(this, tr("Delete list/label"), tr("The selected item will be deleted, your comics or folders will NOT be deleted from your disk. Are you sure?"), QMessageBox::Yes, QMessageBox::No);
            if (ret == QMessageBox::Yes) {
                listsModel->deleteItem(mi);
                navigationController->reselectCurrentList();
            }
        }
    }
}

void LibraryWindow::showAddNewLabelDialog()
{
    auto dialog = new AddLabelDialog();
    int ret = dialog->exec();

    if (ret == QDialog::Accepted) {
        YACReader::LabelColors color = dialog->selectedColor();
        QString name = dialog->name();

        listsModel->addNewLabel(name, color);
    }
}

// TODO implement editors in treeview
void LibraryWindow::showRenameCurrentList()
{
    QModelIndexList selectedLists = listsView->selectionModel()->selectedIndexes();
    if (!selectedLists.isEmpty()) {
        QModelIndex mi = listsModelProxy->mapToSource(selectedLists.at(0));
        if (listsModel->isEditable(mi)) {
            bool ok;
            QString newListName = QInputDialog::getText(this, tr("Rename list name"),
                                                        tr("List name:"), QLineEdit::Normal,
                                                        listsModel->name(mi), &ok);

            if (ok)
                listsModel->rename(mi, newListName);
        }
    }
}

void LibraryWindow::addSelectedComicsToFavorites()
{
    QModelIndexList indexList = getSelectedComics();
    comicsModel->addComicsToFavorites(indexList);
}

void LibraryWindow::showComicsViewContextMenu(const QPoint &point)
{
    QMenu menu;

    menu.addAction(openComicAction);
    menu.addAction(saveCoversToAction);
    menu.addSeparator();
    menu.addAction(openContainingFolderComicAction);
    menu.addAction(updateCurrentFolderAction);
    menu.addSeparator();
    menu.addAction(resetComicRatingAction);
    menu.addSeparator();
    menu.addAction(editSelectedComicsAction);
    menu.addAction(getInfoAction);
    menu.addAction(asignOrderAction);
    menu.addSeparator();
    menu.addAction(selectAllComicsAction);
    menu.addSeparator();
    menu.addAction(setAsReadAction);
    menu.addAction(setAsNonReadAction);
    menu.addSeparator();
    auto typeMenu = new QMenu(tr("Set type"));
    menu.addMenu(typeMenu);
    typeMenu->addAction(setNormalAction);
    typeMenu->addAction(setMangaAction);
    typeMenu->addAction(setWesternMangaAction);
    typeMenu->addAction(setWebComicAction);
    typeMenu->addAction(setYonkomaAction);
    menu.addSeparator();
    menu.addAction(deleteMetadataAction);
    menu.addSeparator();
    menu.addAction(deleteComicsAction);
    menu.addSeparator();
    menu.addAction(addToMenuAction);
    QMenu subMenu;
    setupAddToSubmenu(subMenu);

#ifndef Q_OS_MACOS
    menu.addSeparator();
    menu.addAction(toggleFullScreenAction);
#endif

    menu.exec(contentViewsManager->comicsView->mapToGlobal(point));
}

void LibraryWindow::showComicsItemContextMenu(const QPoint &point)
{
    QMenu menu;

    menu.addAction(openComicAction);
    menu.addAction(saveCoversToAction);
    menu.addSeparator();
    menu.addAction(openContainingFolderComicAction);
    menu.addAction(updateCurrentFolderAction);
    menu.addSeparator();
    menu.addAction(resetComicRatingAction);
    menu.addSeparator();
    menu.addAction(editSelectedComicsAction);
    menu.addAction(getInfoAction);
    menu.addAction(asignOrderAction);
    menu.addSeparator();
    menu.addAction(setAsReadAction);
    menu.addAction(setAsNonReadAction);
    menu.addSeparator();
    auto typeMenu = new QMenu(tr("Set type"));
    menu.addMenu(typeMenu);
    typeMenu->addAction(setNormalAction);
    typeMenu->addAction(setMangaAction);
    typeMenu->addAction(setWesternMangaAction);
    typeMenu->addAction(setWebComicAction);
    typeMenu->addAction(setYonkomaAction);
    menu.addSeparator();
    menu.addAction(deleteMetadataAction);
    menu.addSeparator();
    menu.addAction(deleteComicsAction);
    menu.addSeparator();
    menu.addAction(addToMenuAction);
    QMenu subMenu;
    setupAddToSubmenu(subMenu);

    menu.exec(contentViewsManager->comicsView->mapToGlobal(point));
}

void LibraryWindow::showGridFoldersContextMenu(QPoint point, Folder folder)
{
    QMenu menu;

    auto openContainingFolderAction = new QAction();
    openContainingFolderAction->setText(tr("Open folder..."));
    openContainingFolderAction->setIcon(QIcon(":/images/menus_icons/open_containing_folder.svg"));

    auto updateFolderAction = new QAction(tr("Update folder"), this);
    updateFolderAction->setIcon(QIcon(":/images/menus_icons/update_current_folder.svg"));

    auto rescanLibraryForXMLInfoAction = new QAction(tr("Rescan library for XML info"), this);

    auto setFolderAsNotCompletedAction = new QAction();
    setFolderAsNotCompletedAction->setText(tr("Set as uncompleted"));

    auto setFolderAsCompletedAction = new QAction();
    setFolderAsCompletedAction->setText(tr("Set as completed"));

    auto setFolderAsReadAction = new QAction();
    setFolderAsReadAction->setText(tr("Set as read"));

    auto setFolderAsUnreadAction = new QAction();
    setFolderAsUnreadAction->setText(tr("Set as unread"));

    auto setFolderAsMangaAction = new QAction();
    setFolderAsMangaAction->setText(tr("manga"));

    auto setFolderAsNormalAction = new QAction();
    setFolderAsNormalAction->setText(tr("comic"));

    auto setFolderAsWesternMangaAction = new QAction();
    setFolderAsWesternMangaAction->setText(tr("manga (or left to right)"));

    auto setFolderAsWebComicAction = new QAction();
    setFolderAsWebComicAction->setText(tr("web comic"));

    auto setFolderAs4KomaAction = new QAction();
    setFolderAs4KomaAction->setText(tr("4koma (or top to botom"));

    menu.addAction(openContainingFolderAction);
    menu.addAction(updateFolderAction);
    menu.addSeparator();
    menu.addAction(rescanLibraryForXMLInfoAction);
    menu.addSeparator();
    if (folder.completed)
        menu.addAction(setFolderAsNotCompletedAction);
    else
        menu.addAction(setFolderAsCompletedAction);
    menu.addSeparator();
    if (folder.finished)
        menu.addAction(setFolderAsUnreadAction);
    else
        menu.addAction(setFolderAsReadAction);
    menu.addSeparator();

    auto typeMenu = new QMenu(tr("Set type"));
    menu.addMenu(typeMenu);
    typeMenu->addAction(setFolderAsNormalAction);
    typeMenu->addAction(setFolderAsMangaAction);
    typeMenu->addAction(setFolderAsWesternMangaAction);
    typeMenu->addAction(setFolderAsWebComicAction);
    typeMenu->addAction(setFolderAs4KomaAction);

    auto subfolderModel = contentViewsManager->folderContentView->currentFolderModel();

    connect(openContainingFolderAction, &QAction::triggered, this, [=]() {
        QDesktopServices::openUrl(QUrl("file:///" + QDir::cleanPath(currentPath() + "/" + folder.path), QUrl::TolerantMode));
    });
    connect(updateFolderAction, &QAction::triggered, this, [=]() {
        updateFolder(foldersModel->getIndexFromFolder(folder));
    });
    connect(rescanLibraryForXMLInfoAction, &QAction::triggered, this, [=]() {
        rescanFolderForXMLInfo(foldersModel->getIndexFromFolder(folder));
    });
    connect(setFolderAsNotCompletedAction, &QAction::triggered, this, [=]() {
        foldersModel->updateFolderCompletedStatus(QModelIndexList() << foldersModel->getIndexFromFolder(folder), false);
        subfolderModel->updateFolderCompletedStatus(QModelIndexList() << subfolderModel->getIndexFromFolder(folder), false);
    });
    connect(setFolderAsCompletedAction, &QAction::triggered, this, [=]() {
        foldersModel->updateFolderCompletedStatus(QModelIndexList() << foldersModel->getIndexFromFolder(folder), true);
        subfolderModel->updateFolderCompletedStatus(QModelIndexList() << subfolderModel->getIndexFromFolder(folder), true);
    });
    connect(setFolderAsReadAction, &QAction::triggered, this, [=]() {
        foldersModel->updateFolderFinishedStatus(QModelIndexList() << foldersModel->getIndexFromFolder(folder), true);
        subfolderModel->updateFolderFinishedStatus(QModelIndexList() << subfolderModel->getIndexFromFolder(folder), true);
    });
    connect(setFolderAsUnreadAction, &QAction::triggered, this, [=]() {
        foldersModel->updateFolderFinishedStatus(QModelIndexList() << foldersModel->getIndexFromFolder(folder), false);
        subfolderModel->updateFolderFinishedStatus(QModelIndexList() << subfolderModel->getIndexFromFolder(folder), false);
    });
    connect(setFolderAsMangaAction, &QAction::triggered, this, [=]() {
        foldersModel->updateFolderType(QModelIndexList() << foldersModel->getIndexFromFolder(folder), FileType::Manga);
        subfolderModel->updateFolderType(QModelIndexList() << foldersModel->getIndexFromFolder(folder), FileType::Manga);
    });
    connect(setFolderAsNormalAction, &QAction::triggered, this, [=]() {
        foldersModel->updateFolderType(QModelIndexList() << foldersModel->getIndexFromFolder(folder), FileType::Comic);
        subfolderModel->updateFolderType(QModelIndexList() << foldersModel->getIndexFromFolder(folder), FileType::Comic);
    });
    connect(setFolderAsWesternMangaAction, &QAction::triggered, this, [=]() {
        foldersModel->updateFolderType(QModelIndexList() << foldersModel->getIndexFromFolder(folder), FileType::WesternManga);
        subfolderModel->updateFolderType(QModelIndexList() << foldersModel->getIndexFromFolder(folder), FileType::WesternManga);
    });
    connect(setFolderAsWebComicAction, &QAction::triggered, this, [=]() {
        foldersModel->updateFolderType(QModelIndexList() << foldersModel->getIndexFromFolder(folder), FileType::WebComic);
        subfolderModel->updateFolderType(QModelIndexList() << foldersModel->getIndexFromFolder(folder), FileType::WebComic);
    });
    connect(setFolderAs4KomaAction, &QAction::triggered, this, [=]() {
        foldersModel->updateFolderType(QModelIndexList() << foldersModel->getIndexFromFolder(folder), FileType::Yonkoma);
        subfolderModel->updateFolderType(QModelIndexList() << foldersModel->getIndexFromFolder(folder), FileType::Yonkoma);
    });

    menu.exec(contentViewsManager->folderContentView->mapToGlobal(point));
}

void LibraryWindow::showContinueReadingContextMenu(QPoint point, ComicDB comic)
{
    QMenu menu;

    auto setAsUnReadAction = new QAction();
    setAsUnReadAction->setText(tr("Set as unread"));
    setAsUnReadAction->setIcon(QIcon(":/images/comics_view_toolbar/setUnread.svg"));

    menu.addAction(setAsUnReadAction);

    connect(setAsUnReadAction, &QAction::triggered, this, [=]() {
        auto libraryId = libraries.getId(selectedLibrary->currentText());
        auto info = comic.info;
        info.setRead(false);
        info.currentPage = 1;
        info.hasBeenOpened = false;
        info.lastTimeOpened = QVariant();
        DBHelper::update(libraryId, info);

        contentViewsManager->folderContentView->reloadContinueReadingModel();
    });

    menu.exec(contentViewsManager->folderContentView->mapToGlobal(point));
}

void LibraryWindow::setupAddToSubmenu(QMenu &menu)
{
    menu.addAction(addToFavoritesAction);
    addToMenuAction->setMenu(&menu);

    const QList<LabelItem *> labels = listsModel->getLabels();
    if (labels.count() > 0)
        menu.addSeparator();
    foreach (LabelItem *label, labels) {
        auto action = new QAction(this);
        action->setIcon(label->getIcon());
        action->setText(label->name());

        action->setData(label->getId());

        menu.addAction(action);

        connect(action, &QAction::triggered, this, &LibraryWindow::onAddComicsToLabel);
    }
}

void LibraryWindow::onAddComicsToLabel()
{
    auto action = static_cast<QAction *>(sender());

    qulonglong labelId = action->data().toULongLong();

    QModelIndexList comics = getSelectedComics();

    comicsModel->addComicsToLabel(comics, labelId);
}

void LibraryWindow::setToolbarTitle(const QModelIndex &modelIndex)
{
#ifndef Y_MAC_UI
    if (!modelIndex.isValid())
        libraryToolBar->setCurrentFolderName(selectedLibrary->currentText());
    else
        libraryToolBar->setCurrentFolderName(modelIndex.data().toString());
#endif
}

void LibraryWindow::saveSelectedCoversTo()
{
    QFileDialog saveDialog;
    QString folderPath = saveDialog.getExistingDirectory(this, tr("Save covers"), QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
    if (!folderPath.isEmpty()) {
        QModelIndexList comics = getSelectedComics();
        foreach (QModelIndex comic, comics) {
            QString origin = comic.data(ComicModel::CoverPathRole).toString().remove("file:///").remove("file:");
            QString destination = QDir(folderPath).filePath(comic.data(ComicModel::FileNameRole).toString() + ".jpg");

            QLOG_DEBUG() << "From : " << origin;
            QLOG_DEBUG() << "To : " << destination;

            QFile::copy(origin, destination);
        }
    }
}

void LibraryWindow::checkMaxNumLibraries()
{
    int numLibraries = libraries.getNames().length();
    if (numLibraries >= MAX_LIBRARIES_WARNING_NUM) {
        QMessageBox::warning(this, tr("You are adding too many libraries."), tr("You are adding too many libraries.\n\nYou probably only need one library in your top level comics folder, you can browse any subfolders using the folders section in the left sidebar.\n\nYACReaderLibrary will not stop you from creating more libraries but you should keep the number of libraries low."));
    }
}

void LibraryWindow::selectSubfolder(const QModelIndex &mi, int child)
{
    QModelIndex dest = foldersModel->index(child, 0, mi);
    foldersView->setCurrentIndex(dest);
    navigationController->selectedFolder(dest);
}

// this methods is only using after deleting comics
// TODO broken window :)
void LibraryWindow::checkEmptyFolder()
{
    if (comicsModel->rowCount() > 0 && !importedCovers) {
        disableComicsActions(false);
    } else {
        disableComicsActions(true);
#ifndef Q_OS_MACOS
        if (comicsModel->rowCount() > 0)
            toggleFullScreenAction->setEnabled(true);
#endif
        if (comicsModel->rowCount() == 0)
            navigationController->reselectCurrentFolder();
    }
}

void LibraryWindow::openComic()
{
    if (!importedCovers) {

        auto comic = comicsModel->getComic(contentViewsManager->comicsView->currentIndex());
        auto mode = comicsModel->getMode();

        openComic(comic, mode);
    }
}

void LibraryWindow::openComic(const ComicDB &comic, const ComicModel::Mode mode)
{
    auto libraryId = libraries.getId(selectedLibrary->currentText());

    OpenComicSource::Source source;

    if (mode == ComicModel::ReadingList) {
        source = OpenComicSource::Source::ReadingList;
    } else if (mode == ComicModel::Reading) {
        // TODO check where the comic was opened from the last time it was read
        source = OpenComicSource::Source::Folder;
    } else {
        source = OpenComicSource::Source::Folder;
    }

    auto yacreaderFound = YACReader::openComic(comic, libraryId, currentPath(), OpenComicSource { source, comicsModel->getSourceId() });

    if (!yacreaderFound) {
#ifdef Q_OS_WIN
        QMessageBox::critical(this, tr("YACReader not found"), tr("YACReader not found. YACReader should be installed in the same folder as YACReaderLibrary."));
#else
        QMessageBox::critical(this, tr("YACReader not found"), tr("YACReader not found. There might be a problem with your YACReader installation."));
#endif
    }
}

void LibraryWindow::setCurrentComicsStatusReaded(YACReaderComicReadStatus readStatus)
{
    comicsModel->setComicsRead(getSelectedComics(), readStatus);
    contentViewsManager->updateCurrentComicView();
}

void LibraryWindow::setCurrentComicReaded()
{
    this->setCurrentComicsStatusReaded(YACReader::Read);
}

void LibraryWindow::setCurrentComicUnreaded()
{
    this->setCurrentComicsStatusReaded(YACReader::Unread);
}

void LibraryWindow::setSelectedComicsType(FileType type)
{
    comicsModel->setComicsType(getSelectedComics(), type);
}

void LibraryWindow::createLibrary()
{
    checkMaxNumLibraries();
    createLibraryDialog->open(libraries);
}

void LibraryWindow::create(QString source, QString dest, QString name)
{
    QLOG_INFO() << QString("About to create a library from '%1' to '%2' with name '%3'").arg(source, dest, name);
    libraryCreator->createLibrary(source, dest);
    libraryCreator->start();
    _lastAdded = name;
    _sourceLastAdded = source;

    importWidget->setImportLook();
    showImportingWidget();
}

void LibraryWindow::reloadCurrentLibrary()
{
    foldersModel->reload();
    contentViewsManager->updateCurrentContentView();

    enableNeededActions();
}

void LibraryWindow::openLastCreated()
{

    selectedLibrary->disconnect();

    selectedLibrary->setCurrentIndex(selectedLibrary->findText(_lastAdded));
    libraries.addLibrary(_lastAdded, _sourceLastAdded);
    selectedLibrary->addItem(_lastAdded, _sourceLastAdded);
    selectedLibrary->setCurrentIndex(selectedLibrary->findText(_lastAdded));
    libraries.save();

    connect(selectedLibrary, &YACReaderLibraryListWidget::currentIndexChanged, this, &LibraryWindow::loadLibrary);

    loadLibrary(_lastAdded);
}

void LibraryWindow::showAddLibrary()
{
    checkMaxNumLibraries();
    addLibraryDialog->open();
}

void LibraryWindow::openLibrary(QString path, QString name)
{
    if (!libraries.contains(name)) {
        // TODO: fix bug, /a/b/c/.yacreaderlibrary/d/e
        path.remove("/.yacreaderlibrary");
        QDir d; // TODO change this by static methods (utils class?? with delTree for example)
        if (d.exists(path + "/.yacreaderlibrary")) {
            _lastAdded = name;
            _sourceLastAdded = path;
            openLastCreated();
            addLibraryDialog->close();
        } else
            QMessageBox::warning(this, tr("Library not found"), tr("The selected folder doesn't contain any library."));
    } else {
        libraryAlreadyExists(name);
    }
}

void LibraryWindow::loadLibraries()
{
    libraries.load();
    foreach (QString name, libraries.getNames())
        selectedLibrary->addItem(name, libraries.getPath(name));
}

void LibraryWindow::saveLibraries()
{
    libraries.save();
}

void LibraryWindow::updateLibrary()
{
    importWidget->setUpdateLook();
    showImportingWidget();

    QString currentLibrary = selectedLibrary->currentText();
    QString path = libraries.getPath(currentLibrary);
    _lastAdded = currentLibrary;
    libraryCreator->updateLibrary(path, path + "/.yacreaderlibrary");
    libraryCreator->start();
}

void LibraryWindow::deleteCurrentLibrary()
{
    QString path = libraries.getPath(selectedLibrary->currentText());
    libraries.remove(selectedLibrary->currentText());
    selectedLibrary->removeItem(selectedLibrary->currentIndex());
    // selectedLibrary->setCurrentIndex(0);
    path = path + "/.yacreaderlibrary";

    QDir d(path);
    d.removeRecursively();
    if (libraries.isEmpty()) // no more libraries available.
    {
        contentViewsManager->comicsView->setModel(NULL);
        foldersView->setModel(NULL);
        listsView->setModel(NULL);

        disableAllActions();
        showNoLibrariesWidget();
    }
    libraries.save();
}

void LibraryWindow::removeLibrary()
{
    QString currentLibrary = selectedLibrary->currentText();
    QMessageBox *messageBox = new QMessageBox(tr("Are you sure?"), tr("Do you want remove ") + currentLibrary + tr(" library?"), QMessageBox::Question, QMessageBox::Yes, QMessageBox::YesToAll, QMessageBox::No);
    messageBox->button(QMessageBox::YesToAll)->setText(tr("Remove and delete metadata"));
    messageBox->setParent(this);
    messageBox->setWindowModality(Qt::WindowModal);
    int ret = messageBox->exec();
    if (ret == QMessageBox::Yes) {
        libraries.remove(currentLibrary);
        selectedLibrary->removeItem(selectedLibrary->currentIndex());
        // selectedLibrary->setCurrentIndex(0);
        if (libraries.isEmpty()) // no more libraries available.
        {
            contentViewsManager->comicsView->setModel(NULL);
            foldersView->setModel(NULL);
            listsView->setModel(NULL);

            disableAllActions();
            showNoLibrariesWidget();
        }
        libraries.save();
    } else if (ret == QMessageBox::YesToAll) {
        deleteCurrentLibrary();
    }
}

void LibraryWindow::renameLibrary()
{
    renameLibraryDialog->open();
}

void LibraryWindow::rename(QString newName) // TODO replace
{
    QString currentLibrary = selectedLibrary->currentText();
    if (newName != currentLibrary) {
        if (!libraries.contains(newName)) {
            libraries.rename(currentLibrary, newName);
            // selectedLibrary->removeItem(selectedLibrary->currentIndex());
            // libraries.addLibrary(newName,path);
            selectedLibrary->renameCurrentLibrary(newName);
            libraries.save();
            renameLibraryDialog->close();
#ifndef Y_MAC_UI
            if (!foldersModelProxy->mapToSource(foldersView->currentIndex()).isValid())
                libraryToolBar->setCurrentFolderName(selectedLibrary->currentText());
#endif
        } else {
            libraryAlreadyExists(newName);
        }
    } else
        renameLibraryDialog->close();
    // selectedLibrary->setCurrentIndex(selectedLibrary->findText(newName));
}

void LibraryWindow::rescanLibraryForXMLInfo()
{
    importWidget->setXMLScanLook();
    showImportingWidget();

    QString currentLibrary = selectedLibrary->currentText();
    QString path = libraries.getPath(currentLibrary);
    _lastAdded = currentLibrary;

    xmlInfoLibraryScanner->scanLibrary(path, path + "/.yacreaderlibrary");
}

void LibraryWindow::rescanCurrentFolderForXMLInfo()
{
    rescanFolderForXMLInfo(getCurrentFolderIndex());
}

void LibraryWindow::rescanFolderForXMLInfo(QModelIndex modelIndex)
{
    importWidget->setXMLScanLook();
    showImportingWidget();

    QString currentLibrary = selectedLibrary->currentText();
    QString path = libraries.getPath(currentLibrary);
    _lastAdded = currentLibrary;

    xmlInfoLibraryScanner->scanFolder(path, path + "/.yacreaderlibrary", QDir::cleanPath(currentPath() + foldersModel->getFolderPath(modelIndex)), modelIndex);
}

void LibraryWindow::cancelCreating()
{
    stopLibraryCreator();
}

void LibraryWindow::stopLibraryCreator()
{
    libraryCreator->stop();
    libraryCreator->wait();
}

void LibraryWindow::stopXMLScanning()
{
    xmlInfoLibraryScanner->stop();
    xmlInfoLibraryScanner->wait();
}

void LibraryWindow::setRootIndex()
{
    if (!libraries.isEmpty()) {
        QString path = libraries.getPath(selectedLibrary->currentText()) + "/.yacreaderlibrary";
        QDir d; // TODO change this by static methods (utils class?? with delTree for example)
        if (d.exists(path)) {
            navigationController->selectedFolder(QModelIndex());
        } else {
            contentViewsManager->comicsView->setModel(NULL);
        }

        foldersView->selectionModel()->clear();
    }
}

void LibraryWindow::toggleFullScreen()
{
    fullscreen ? toNormal() : toFullScreen();
    fullscreen = !fullscreen;
}

#ifdef Q_OS_WIN // fullscreen mode in Windows for preventing this bug: QTBUG-41309 https://bugreports.qt.io/browse/QTBUG-41309
void LibraryWindow::toFullScreen()
{
    fromMaximized = this->isMaximized();

    sideBar->hide();
    libraryToolBar->hide();

    previousWindowFlags = windowFlags();
    previousPos = pos();
    previousSize = size();

    showNormal();
    setWindowFlags(previousWindowFlags | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);

    QRect r = windowHandle()->screen()->geometry();

    r.setHeight(r.height() + 1);

    setGeometry(r);
    show();

    contentViewsManager->toFullscreen();
}

void LibraryWindow::toNormal()
{
    sideBar->show();
    libraryToolBar->show();

    setWindowFlags(previousWindowFlags);
    move(previousPos);
    resize(previousSize);
    show();

    if (fromMaximized)
        showMaximized();

    contentViewsManager->toNormal();
}

#else

void LibraryWindow::toFullScreen()
{
    fromMaximized = this->isMaximized();

    sideBar->hide();
    libraryToolBar->hide();

    contentViewsManager->toFullscreen();

    showFullScreen();
}

void LibraryWindow::toNormal()
{
    sideBar->show();

    contentViewsManager->toNormal();

    if (fromMaximized)
        showMaximized();
    else
        showNormal();

#ifdef Y_MAC_UI
    auto timer = new QTimer();
    timer->setSingleShot(true);
    timer->start();
    connect(timer, &QTimer::timeout, libraryToolBar, &YACReaderMacOSXToolbar::show);
    connect(timer, &QTimer::timeout, timer, &QTimer::deleteLater);
#else
    libraryToolBar->show();
#endif
}

#endif

void LibraryWindow::setSearchFilter(QString filter)
{
    if (!filter.isEmpty()) {
        folderQueryResultProcessor->createModelData(filter);
        comicQueryResultProcessor.createModelData(filter, foldersModel->getDatabase());
    } else if (status == LibraryWindow::Searching) { // if no searching, then ignore this
        clearSearchFilter();
        navigationController->loadPreviousStatus();
    }
}

void LibraryWindow::setComicSearchFilterData(QList<ComicItem *> *data, const QString &databasePath)
{
    status = LibraryWindow::Searching;

    comicsModel->setModelData(data, databasePath);
    contentViewsManager->comicsView->enableFilterMode(true);
    contentViewsManager->comicsView->setModel(comicsModel); // TODO, columns are messed up after ResetModel some times, this shouldn't be necesary

    if (comicsModel->rowCount() == 0) {
        contentViewsManager->showNoSearchResultsView();
        disableComicsActions(true);
    } else {
        contentViewsManager->showComicsView();
        disableComicsActions(false);
    }
}

void LibraryWindow::setFolderSearchFilterData(QMap<unsigned long long, FolderItem *> *filteredItems, FolderItem *root)
{
    foldersModelProxy->setFilterData(filteredItems, root);
    foldersView->expandAll();
}

void LibraryWindow::clearSearchFilter()
{
    foldersModelProxy->clear();
    contentViewsManager->comicsView->enableFilterMode(false);
    foldersView->collapseAll();
    status = LibraryWindow::Normal;
}

void LibraryWindow::showProperties()
{
    QModelIndexList indexList = getSelectedComics();

    QList<ComicDB> comics = comicsModel->getComics(indexList);
    ComicDB c = comics[0];
    _comicIdEdited = c.id; // static_cast<TableItem*>(indexList[0].internalPointer())->data(4).toULongLong();

    propertiesDialog->databasePath = foldersModel->getDatabase();
    propertiesDialog->basePath = currentPath();

    if (indexList.length() > 1) { // edit common properties
        propertiesDialog->setComics(comics);
    } else {
        auto allComics = comicsModel->getAllComics();
        int index = allComics.indexOf(c);
        propertiesDialog->setComicsForSequentialEditing(index, comicsModel->getAllComics());
    }

    propertiesDialog->show();
}

void LibraryWindow::showComicVineScraper()
{
    QSettings s(YACReader::getSettingsPath() + "/YACReaderLibrary.ini", QSettings::IniFormat); // TODO unificar la creación del fichero de config con el servidor
    s.beginGroup("ComicVine");

    if (!s.contains(COMIC_VINE_API_KEY)) {
        ApiKeyDialog d;
        d.exec();
    }

    // check if the api key was inserted
    if (s.contains(COMIC_VINE_API_KEY)) {
        QModelIndexList indexList = getSelectedComics();

        QList<ComicDB> comics = comicsModel->getComics(indexList);
        ComicDB c = comics[0];
        _comicIdEdited = c.id; // static_cast<TableItem*>(indexList[0].internalPointer())->data(4).toULongLong();

        comicVineDialog->databasePath = foldersModel->getDatabase();
        comicVineDialog->basePath = currentPath();
        comicVineDialog->setComics(comics);

        comicVineDialog->show();
    }
}

void LibraryWindow::setRemoveError()
{
    removeError = true;
}

void LibraryWindow::checkRemoveError()
{
    if (removeError) {
        QMessageBox::critical(this, tr("Unable to delete"), tr("There was an issue trying to delete the selected comics. Please, check for write permissions in the selected files or containing folder."));
    }
    removeError = false;
}

void LibraryWindow::resetComicRating()
{
    QModelIndexList indexList = getSelectedComics();

    comicsModel->startTransaction();
    for (auto &index : indexList) {
        comicsModel->resetComicRating(index);
    }
    comicsModel->finishTransaction();
}

void LibraryWindow::checkSearchNumResults(int numResults)
{
    if (numResults == 0)
        contentViewsManager->showNoSearchResultsView();
    else
        contentViewsManager->showComicsView();
}

void LibraryWindow::asignNumbers()
{
    QModelIndexList indexList = getSelectedComics();

    int startingNumber = indexList[0].row() + 1;
    if (indexList.count() > 1) {
        bool ok;
        int n = QInputDialog::getInt(this, tr("Assign comics numbers"),
                                     tr("Assign numbers starting in:"), startingNumber, 0, 2147483647, 1, &ok);
        if (ok)
            startingNumber = n;
        else
            return;
    }
    qint64 edited = comicsModel->asignNumbers(indexList, startingNumber);

    // TODO add resorting without reloading
    navigationController->loadFolderInfo(foldersModelProxy->mapToSource(foldersView->currentIndex()));

    const QModelIndex &mi = comicsModel->getIndexFromId(edited);
    if (mi.isValid()) {
        contentViewsManager->comicsView->scrollTo(mi, QAbstractItemView::PositionAtCenter);
        contentViewsManager->comicsView->setCurrentIndex(mi);
    }
}

void LibraryWindow::openContainingFolderComic()
{
    QModelIndex modelIndex = contentViewsManager->comicsView->currentIndex();
    QFileInfo file(QDir::cleanPath(currentPath() + comicsModel->getComicPath(modelIndex)));
#if defined Q_OS_UNIX && !defined Q_OS_MACOS
    QString path = file.absolutePath();
    QDesktopServices::openUrl(QUrl("file:///" + path, QUrl::TolerantMode));
#endif

#ifdef Q_OS_MACOS
    QString filePath = file.absoluteFilePath();
    QStringList args;
    args << "-e";
    args << "tell application \"Finder\"";
    args << "-e";
    args << "activate";
    args << "-e";
    args << "select POSIX file \"" + filePath + "\"";
    args << "-e";
    args << "end tell";
    QProcess::startDetached("osascript", args);
#endif

#ifdef Q_OS_WIN
    QString filePath = file.absoluteFilePath();
    QString cmdArgs = QString("/select,\"") + QDir::toNativeSeparators(filePath) + QStringLiteral("\"");
    ShellExecuteW(0, L"open", L"explorer.exe", reinterpret_cast<LPCWSTR>(cmdArgs.utf16()), 0, SW_NORMAL);
#endif
}

void LibraryWindow::openContainingFolder()
{
    QModelIndex modelIndex = foldersModelProxy->mapToSource(foldersView->currentIndex());
    QString path;
    if (modelIndex.isValid())
        path = QDir::cleanPath(currentPath() + foldersModel->getFolderPath(modelIndex));
    else
        path = QDir::cleanPath(currentPath());
    QDesktopServices::openUrl(QUrl("file:///" + path, QUrl::TolerantMode));
}

void LibraryWindow::setFolderAsNotCompleted()
{
    // foldersModel->updateFolderCompletedStatus(foldersView->selectionModel()->selectedRows(),false);
    foldersModel->updateFolderCompletedStatus(QModelIndexList() << foldersModelProxy->mapToSource(foldersView->currentIndex()), false);
}

void LibraryWindow::setFolderAsCompleted()
{
    // foldersModel->updateFolderCompletedStatus(foldersView->selectionModel()->selectedRows(),true);
    foldersModel->updateFolderCompletedStatus(QModelIndexList() << foldersModelProxy->mapToSource(foldersView->currentIndex()), true);
}

void LibraryWindow::setFolderAsRead()
{
    // foldersModel->updateFolderFinishedStatus(foldersView->selectionModel()->selectedRows(),true);
    foldersModel->updateFolderFinishedStatus(QModelIndexList() << foldersModelProxy->mapToSource(foldersView->currentIndex()), true);
}

void LibraryWindow::setFolderAsUnread()
{
    // foldersModel->updateFolderFinishedStatus(foldersView->selectionModel()->selectedRows(),false);
    foldersModel->updateFolderFinishedStatus(QModelIndexList() << foldersModelProxy->mapToSource(foldersView->currentIndex()), false);
}

void LibraryWindow::setFolderType(FileType type)
{
    foldersModel->updateFolderType(QModelIndexList() << foldersModelProxy->mapToSource(foldersView->currentIndex()), type);
}

void LibraryWindow::exportLibrary(QString destPath)
{
    QString currentLibrary = selectedLibrary->currentText();
    QString path = libraries.getPath(currentLibrary) + "/.yacreaderlibrary";
    packageManager->createPackage(path, destPath + "/" + currentLibrary);
}

void LibraryWindow::importLibrary(QString clc, QString destPath, QString name)
{
    packageManager->extractPackage(clc, destPath + "/" + name);
    _lastAdded = name;
    _sourceLastAdded = destPath + "/" + name;
}

void LibraryWindow::reloadOptions()
{
    contentViewsManager->comicsView->updateConfig(settings);

    trayIconController->updateIconVisibility();

    recentVisibilityCoordinator->updateTimeRange();
}

QString LibraryWindow::currentPath()
{
    return libraries.getPath(selectedLibrary->currentText());
}

QString LibraryWindow::currentFolderPath()
{
    QString path;

    if (foldersView->selectionModel()->selectedRows().length() > 0)
        path = foldersModel->getFolderPath(foldersModelProxy->mapToSource(foldersView->currentIndex()));
    else
        path = foldersModel->getFolderPath(QModelIndex());

    QLOG_DEBUG() << "current folder path : " << QDir::cleanPath(currentPath() + path);

    return QDir::cleanPath(currentPath() + path);
}

void LibraryWindow::showExportComicsInfo()
{
    exportComicsInfoDialog->source = currentPath() + "/.yacreaderlibrary/library.ydb";
    exportComicsInfoDialog->open();
}

void LibraryWindow::showImportComicsInfo()
{
    importComicsInfoDialog->dest = currentPath() + "/.yacreaderlibrary/library.ydb";
    importComicsInfoDialog->open();
}

void LibraryWindow::closeEvent(QCloseEvent *event)
{
    if (!trayIconController->handleCloseToTrayIcon(event)) {
        event->accept();
        closeApp();
    }
}

void LibraryWindow::prepareToCloseApp()
{
    httpServer->stop();

    libraryCreator->stop();
    librariesUpdateCoordinator->stop();

    settings->setValue(MAIN_WINDOW_GEOMETRY, saveGeometry());

    contentViewsManager->comicsView->close();
    sideBar->close();

    QApplication::instance()->processEvents();
}

void LibraryWindow::closeApp()
{
    prepareToCloseApp();

    qApp->exit(0);
}

void LibraryWindow::showNoLibrariesWidget()
{
    disableAllActions();
    searchEdit->setDisabled(true);
    mainWidget->setCurrentIndex(1);
}

void LibraryWindow::showRootWidget()
{
#ifndef Y_MAC_UI
    libraryToolBar->setDisabled(false);
#endif
    searchEdit->setEnabled(true);
    mainWidget->setCurrentIndex(0);
}

void LibraryWindow::showImportingWidget()
{
    disableAllActions();
    importWidget->clear();
#ifndef Y_MAC_UI
    libraryToolBar->setDisabled(true);
#endif
    searchEdit->setDisabled(true);
    mainWidget->setCurrentIndex(2);
}

void LibraryWindow::manageCreatingError(const QString &error)
{
    QMessageBox::critical(this, tr("Error creating the library"), error);
}

void LibraryWindow::manageUpdatingError(const QString &error)
{
    QMessageBox::critical(this, tr("Error updating the library"), error);
}

void LibraryWindow::manageOpeningLibraryError(const QString &error)
{
    QMessageBox::critical(this, tr("Error opening the library"), error);
}

bool lessThanModelIndexRow(const QModelIndex &m1, const QModelIndex &m2)
{
    return m1.row() < m2.row();
}

QModelIndexList LibraryWindow::getSelectedComics()
{
    // se fuerza a que haya almenos una fila seleccionada TODO comprobar se se puede forzar a la tabla a que lo haga automáticamente
    // avoid selection.count()==0 forcing selection in comicsView
    QModelIndexList selection = contentViewsManager->comicsView->selectionModel()->selectedRows();
    QLOG_TRACE() << "selection count " << selection.length();
    std::sort(selection.begin(), selection.end(), lessThanModelIndexRow);

    if (selection.count() == 0) {
        contentViewsManager->comicsView->selectIndex(0);
        selection = contentViewsManager->comicsView->selectionModel()->selectedRows();
    }
    return selection;
}

void LibraryWindow::deleteMetadataFromSelectedComics()
{
    QModelIndexList indexList = getSelectedComics();
    QList<ComicDB> comics = comicsModel->getComics(indexList);

    for (auto &comic : comics) {
        comic.info.deleteMetadata();
    }

    DBHelper::updateComicsInfo(comics, foldersModel->getDatabase());

    comicsModel->reload();
}

void LibraryWindow::deleteComics()
{
    // TODO
    if (!listsView->selectionModel()->selectedRows().isEmpty()) {
        deleteComicsFromList();
    } else {
        deleteComicsFromDisk();
    }
}

void LibraryWindow::deleteComicsFromDisk()
{
    int ret = QMessageBox::question(this, tr("Delete comics"), tr("All the selected comics will be deleted from your disk. Are you sure?"), QMessageBox::Yes, QMessageBox::No);

    if (ret == QMessageBox::Yes) {

        QModelIndexList indexList = getSelectedComics();

        QList<ComicDB> comics = comicsModel->getComics(indexList);

        QList<QString> paths;
        QString libraryPath = currentPath();
        foreach (ComicDB comic, comics) {
            paths.append(libraryPath + comic.path);
            QLOG_TRACE() << comic.path;
            QLOG_TRACE() << comic.id;
            QLOG_TRACE() << comic.parentId;
        }

        auto remover = new ComicsRemover(indexList, paths, comics.at(0).parentId);
        const auto thread = new QThread(this);
        moveAndConnectRemoverToThread(remover, thread);

        comicsModel->startTransaction();

        connect(remover, &ComicsRemover::remove, comicsModel, &ComicModel::remove);
        connect(remover, &ComicsRemover::removeError, this, &LibraryWindow::setRemoveError);
        connect(remover, &ComicsRemover::finished, comicsModel, &ComicModel::finishTransaction);
        connect(remover, &ComicsRemover::removedItemsFromFolder, foldersModel, &FolderModel::updateFolderChildrenInfo);

        connect(remover, &ComicsRemover::finished, this, &LibraryWindow::checkEmptyFolder);
        connect(remover, &ComicsRemover::finished, this, &LibraryWindow::checkRemoveError);

        thread->start();
    }
}

void LibraryWindow::deleteComicsFromList()
{
    int ret = QMessageBox::question(this, tr("Remove comics"), tr("Comics will only be deleted from the current label/list. Are you sure?"), QMessageBox::Yes, QMessageBox::No);

    if (ret == QMessageBox::Yes) {
        QModelIndexList indexList = getSelectedComics();
        if (indexList.isEmpty())
            return;

        QModelIndex mi = listsModelProxy->mapToSource(listsView->currentIndex());

        ReadingListModel::TypeList typeList = (ReadingListModel::TypeList)mi.data(ReadingListModel::TypeListsRole).toInt();

        qulonglong id = mi.data(ReadingListModel::IDRole).toULongLong();
        switch (typeList) {
        case ReadingListModel::SpecialList:
            comicsModel->deleteComicsFromSpecialList(indexList, id);
            break;
        case ReadingListModel::Label:
            comicsModel->deleteComicsFromLabel(indexList, id);
            break;
        case ReadingListModel::ReadingList:
            comicsModel->deleteComicsFromReadingList(indexList, id);
            break;
        case ReadingListModel::Separator:
            break;
        }
    }
}

void LibraryWindow::showFoldersContextMenu(const QPoint &point)
{
    QModelIndex sourceMI = foldersModelProxy->mapToSource(foldersView->indexAt(point));

    bool isCompleted = sourceMI.data(FolderModel::CompletedRole).toBool();
    bool isRead = sourceMI.data(FolderModel::FinishedRole).toBool();

    QMenu menu;

    menu.addAction(openContainingFolderAction);
    menu.addAction(updateFolderAction);
    menu.addSeparator(); //-------------------------------
    menu.addAction(rescanXMLFromCurrentFolderAction);
    menu.addSeparator(); //-------------------------------
    if (isCompleted)
        menu.addAction(setFolderAsNotCompletedAction);
    else
        menu.addAction(setFolderAsCompletedAction);
    menu.addSeparator(); //-------------------------------
    if (isRead)
        menu.addAction(setFolderAsUnreadAction);
    else
        menu.addAction(setFolderAsReadAction);
    menu.addSeparator(); //-------------------------------
    auto typeMenu = new QMenu(tr("Set type"));
    menu.addMenu(typeMenu);
    typeMenu->addAction(setFolderAsNormalAction);
    typeMenu->addAction(setFolderAsMangaAction);
    typeMenu->addAction(setFolderAsWesternMangaAction);
    typeMenu->addAction(setFolderAsWebComicAction);
    typeMenu->addAction(setFolderAsYonkomaAction);

    menu.exec(foldersView->mapToGlobal(point));
}

/*
void LibraryWindow::showSocial()
{
        socialDialog->move(this->mapToGlobal(QPoint(width()-socialDialog->width()-10, centralWidget()->pos().y()+10)));

        QModelIndexList indexList = getSelectedComics();

        ComicDB comic = dmCV->getComic(indexList.at(0));

        socialDialog->setComic(comic,currentPath());
        socialDialog->setHidden(false);
}*/

void LibraryWindow::libraryAlreadyExists(const QString &name)
{
    QMessageBox::information(this, tr("Library name already exists"), tr("There is another library with the name '%1'.").arg(name));
}

void LibraryWindow::importLibraryPackage()
{
    importLibraryDialog->open(libraries);
}

void LibraryWindow::updateViewsOnClientSync()
{
    comicsModel->reload();
    contentViewsManager->updateCurrentComicView();
    contentViewsManager->updateContinueReadingView();
}

void LibraryWindow::updateViewsOnComicUpdateWithId(quint64 libraryId, quint64 comicId)
{
    if (libraryId == (quint64)libraries.getId(selectedLibrary->currentText())) {
        auto path = libraries.getPath(libraryId);
        if (path.isEmpty()) {
            return;
        }
        QString connectionName = "";
        {
            QSqlDatabase db = DataBaseManagement::loadDatabase(path + "/.yacreaderlibrary");
            bool found;
            auto comic = DBHelper::loadComic(comicId, db, found);
            if (found) {
                updateViewsOnComicUpdate(libraryId, comic);
            }

            qDebug() << db.lastError();
            connectionName = db.connectionName();
        }
        QSqlDatabase::removeDatabase(connectionName);
    }
}

void LibraryWindow::updateViewsOnComicUpdate(quint64 libraryId, const ComicDB &comic)
{
    if (libraryId == (quint64)libraries.getId(selectedLibrary->currentText())) {
        comicsModel->reload(comic);
        contentViewsManager->updateCurrentComicView();
        contentViewsManager->updateContinueReadingView();
    }
}

bool LibraryWindow::exitSearchMode()
{
    if (status != LibraryWindow::Searching)
        return false;
    searchEdit->clearText();
    clearSearchFilter();
    return true;
}
