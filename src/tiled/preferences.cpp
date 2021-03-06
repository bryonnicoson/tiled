/*
 * preferences.cpp
 * Copyright 2009-2011, Thorbjørn Lindeijer <thorbjorn@lindeijer.nl>
 * Copyright 2016, Mamed Ibrahimov <ibramlab@gmail.com>
 *
 * This file is part of Tiled.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "preferences.h"

#include "documentmanager.h"
#include "languagemanager.h"
#include "mapdocument.h"
#include "pluginmanager.h"
#include "savefile.h"
#include "tilesetmanager.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QVariantMap>

using namespace Tiled;

SessionOption<bool> Preferences::automappingWhileDrawing { "automapping.whileDrawing", false };
SessionOption<QStringList> Preferences::loadedWorlds { "loadedWorlds" };

Preferences *Preferences::mInstance;

Preferences *Preferences::instance()
{
    if (!mInstance)
        mInstance = new Preferences;
    return mInstance;
}

void Preferences::deleteInstance()
{
    delete mInstance;
    mInstance = nullptr;
}

Preferences::Preferences()
    : stampsDirectory("stampsFolder", dataLocation() + QLatin1String("/stamps"))
    , mSession(restoreSessionOnStartup() ? lastSession() : Session::defaultFileName())
{
    // Make sure the data directory exists
    const QDir dataDir { dataLocation() };
    if (!dataDir.exists())
        dataDir.mkpath(QStringLiteral("."));

    connect(&mWatcher, &FileSystemWatcher::fileChanged,
            this, &Preferences::objectTypesFileChangedOnDisk);

    SaveFile::setSafeSavingEnabled(safeSavingEnabled());

    // Backwards compatibility check since 'FusionStyle' was removed from the
    // preferences dialog.
    if (applicationStyle() == FusionStyle)
        setApplicationStyle(TiledStyle);

    // Retrieve defined object types
    ObjectTypesSerializer objectTypesSerializer;
    ObjectTypes objectTypes;
    bool success = objectTypesSerializer.readObjectTypes(objectTypesFile(), objectTypes);

    // For backwards compatibilty, read in object types from settings
    if (!success) {
        const auto names = get<QStringList>("ObjectTypes/Names");
        const auto colors = get<QStringList>("ObjectTypes/Colors");

        if (!names.isEmpty()) {
            const int count = qMin(names.size(), colors.size());
            for (int i = 0; i < count; ++i)
                objectTypes.append(ObjectType(names.at(i), QColor(colors.at(i))));
        }
    } else {
        remove(QLatin1String("ObjectTypes"));

        mWatcher.addPath(objectTypesFile());
    }

    Object::setObjectTypes(objectTypes);

    mSaveSessionTimer.setInterval(1000);
    mSaveSessionTimer.setSingleShot(true);
    connect(&mSaveSessionTimer, &QTimer::timeout, this, [this] { saveSessionNow(); });

    // Migrate some preferences to the session for compatibility
    migrateToSession<bool>("Automapping/WhileDrawing", "automapping.whileDrawing");

    migrateToSession<QStringList>("LoadedWorlds", "loadedWorlds");
    migrateToSession<QString>("Storage/StampsDirectory", "stampsFolder");

    migrateToSession<int>("Map/Orientation", "map.orientation");
    migrateToSession<int>("Storage/LayerDataFormat", "map.layerDataFormat");
    migrateToSession<int>("Storage/MapRenderOrder", "map.renderOrder");
    migrateToSession<bool>("Map/FixedSize", "map.fixedSize");
    migrateToSession<int>("Map/Width", "map.width");
    migrateToSession<int>("Map/Height", "map.height");
    migrateToSession<int>("Map/TileWidth", "map.tileWidth");
    migrateToSession<int>("Map/TileHeight", "map.tileHeight");

    migrateToSession<int>("Tileset/Type", "tileset.type");
    migrateToSession<bool>("Tileset/EmbedInMap", "tileset.embedInMap");
    migrateToSession<bool>("Tileset/UseTransparentColor", "tileset.useTransparentColor");
    migrateToSession<QColor>("Tileset/TransparentColor", "tileset.transparentColor");
    migrateToSession<QSize>("Tileset/TileSize", "tileset.tileSize");
    migrateToSession<int>("Tileset/Spacing", "tileset.spacing");
    migrateToSession<int>("Tileset/Margin", "tileset.margin");

    migrateToSession<QString>("AddPropertyDialog/PropertyType", "property.type");

    migrateToSession<QStringList>("Console/History", "console.history");

    migrateToSession<bool>("SaveAsImage/VisibleLayersOnly", "exportAsImage.visibleLayersOnly");
    migrateToSession<bool>("SaveAsImage/CurrentScale", "exportAsImage.useCurrentScale");
    migrateToSession<bool>("SaveAsImage/DrawGrid", "exportAsImage.drawTileGrid");
    migrateToSession<bool>("SaveAsImage/IncludeBackgroundColor", "exportAsImage.includeBackgroundColor");

    migrateToSession<bool>("ResizeMap/RemoveObjects", "resizeMap.removeObjects");

    migrateToSession<int>("Animation/FrameDuration", "frame.defaultDuration");

    migrateToSession<QString>("lastUsedExportFilter", "map.lastUsedExportFilter");
    migrateToSession<QString>("lastUsedMapFormat", "map.lastUsedFormat");
    migrateToSession<QString>("lastUsedOpenFilter", "file.lastUsedOpenFilter");
    migrateToSession<QString>("lastUsedTilesetExportFilter", "tileset.lastUsedExportFilter");
    migrateToSession<QString>("lastUsedTilesetFilter", "tileset.lastUsedFilter");
    migrateToSession<QString>("lastUsedTilesetFormat", "tileset.lastUsedFormat");

    // Migrate some preferences that need manual handling
    if (mSession.fileName() == Session::defaultFileName()) {
        if (contains(QLatin1String("recentFiles"))) {
            mSession.recentFiles = get<QStringList>("recentFiles/fileNames");
            mSession.openFiles = get<QStringList>("recentFiles/lastOpenFiles");
            mSession.activeFile = get<QString>("recentFiles/lastActive");
        }

        if (contains(QLatin1String("MapEditor/MapStates"))) {
            const auto mapStates = get<QVariantMap>("MapEditor/MapStates");

            for (auto it = mapStates.begin(); it != mapStates.end(); ++it) {
                const QString &fileName = it.key();
                auto mapState = it.value().toMap();

                const QPointF viewCenter = mapState.value(QLatin1String("viewCenter")).toPointF();

                mapState.insert(QLatin1String("viewCenter"), toSettingsValue(viewCenter));

                mSession.setFileState(fileName, mapState);
            }
        }

        if (mSession.save()) {
            remove(QLatin1String("recentFiles"));
            remove(QLatin1String("MapEditor/MapStates"));
        }
    }

    TilesetManager *tilesetManager = TilesetManager::instance();
    tilesetManager->setReloadTilesetsOnChange(reloadTilesetsOnChange());
    tilesetManager->setAnimateTiles(showTileAnimations());

    // Read the lists of enabled and disabled plugins
    const auto disabledPlugins = get<QStringList>("Plugins/Disabled");
    const auto enabledPlugins = get<QStringList>("Plugins/Enabled");

    PluginManager *pluginManager = PluginManager::instance();
    for (const QString &fileName : disabledPlugins)
        pluginManager->setPluginState(fileName, PluginDisabled);
    for (const QString &fileName : enabledPlugins)
        pluginManager->setPluginState(fileName, PluginEnabled);

    // Keeping track of some usage information
    if (contains(QLatin1String("Install/PatreonDialogTime"))) {
        setValue(QLatin1String("Install/DonationDialogTime"), value(QLatin1String("Install/PatreonDialogTime")));
        remove(QLatin1String("Install/PatreonDialogTime"));
    }

    if (!firstRun().isValid())
        setValue(QLatin1String("Install/FirstRun"), QDate::currentDate().toString(Qt::ISODate));

    if (!contains(QLatin1String("Install/DonationDialogTime"))) {
        QDate donationDialogTime = firstRun().addMonths(1);
        const QDate today(QDate::currentDate());
        if (donationDialogTime.daysTo(today) >= 0)
            donationDialogTime = today.addDays(2);
        setValue(QLatin1String("Install/DonationDialogTime"), donationDialogTime.toString(Qt::ISODate));
    }
    setValue(QLatin1String("Install/RunCount"), runCount() + 1);
}

Preferences::~Preferences()
{
}

bool Preferences::showGrid() const
{
    return get("Interface/ShowGrid", true);
}

bool Preferences::showTileObjectOutlines() const
{
    return get("Interface/ShowTileObjectOutlines", false);
}

bool Preferences::showTileAnimations() const
{
    return get("Interface/ShowTileAnimations", true);
}

bool Preferences::showTileCollisionShapes() const
{
    return get("Interface/ShowTileCollisionShapes", false);
}

bool Preferences::showObjectReferences() const
{
    return get("Interface/ShowObjectReferences", true);
}

bool Preferences::snapToGrid() const
{
    return get("Interface/SnapToGrid", false);
}

bool Preferences::snapToFineGrid() const
{
    return get("Interface/SnapToFineGrid", false);
}

bool Preferences::snapToPixels() const
{
    return get("Interface/SnapToPixels", false);
}

QColor Preferences::gridColor() const
{
    return get<QColor>("Interface/GridColor", Qt::black);
}

int Preferences::gridFine() const
{
    return get<int>("Interface/GridFine", 4);
}

qreal Preferences::objectLineWidth() const
{
    return get<qreal>("Interface/ObjectLineWidth", 2.0);
}

bool Preferences::highlightCurrentLayer() const
{
    return get("Interface/HighlightCurrentLayer", false);
}

bool Preferences::highlightHoveredObject() const
{
    return get("Interface/HighlightHoveredObject", true);
}

bool Preferences::showTilesetGrid() const
{
    return get("Interface/ShowTilesetGrid", true);
}

Preferences::ObjectLabelVisiblity Preferences::objectLabelVisibility() const
{
    return static_cast<ObjectLabelVisiblity>(get<int>("Interface/ObjectLabelVisibility", AllObjectLabels));
}

void Preferences::setObjectLabelVisibility(ObjectLabelVisiblity visibility)
{
    setValue(QLatin1String("Interface/ObjectLabelVisibility"), visibility);
    emit objectLabelVisibilityChanged(visibility);
}

bool Preferences::labelForHoveredObject() const
{
    return get("Interface/LabelForHoveredObject", false);
}

void Preferences::setLabelForHoveredObject(bool enabled)
{
    setValue(QLatin1String("Interface/LabelForHoveredObject"), enabled);
    emit labelForHoveredObjectChanged(enabled);
}

Preferences::ApplicationStyle Preferences::applicationStyle() const
{
#if defined(Q_OS_MAC)
    return static_cast<ApplicationStyle>(get<int>("Interface/ApplicationStyle", SystemDefaultStyle));
#else
    return static_cast<ApplicationStyle>(get<int>("Interface/ApplicationStyle", TiledStyle));
#endif
}

void Preferences::setApplicationStyle(ApplicationStyle style)
{
    setValue(QLatin1String("Interface/ApplicationStyle"), style);
    emit applicationStyleChanged(style);
}

QColor Preferences::baseColor() const
{
    return get<QColor>("Interface/BaseColor", Qt::lightGray);
}

void Preferences::setBaseColor(const QColor &color)
{
    setValue(QLatin1String("Interface/BaseColor"), color.name());
    emit baseColorChanged(color);
}

QColor Preferences::selectionColor() const
{
    return get<QColor>("Interface/SelectionColor", QColor(48, 140, 198));
}

void Preferences::setSelectionColor(const QColor &color)
{
    setValue(QLatin1String("Interface/SelectionColor"), color.name());
    emit selectionColorChanged(color);
}

Map::LayerDataFormat Preferences::layerDataFormat() const
{
    return static_cast<Map::LayerDataFormat>(get<int>("Storage/LayerDataFormat", Map::CSV));
}

void Preferences::setShowGrid(bool showGrid)
{
    setValue(QLatin1String("Interface/ShowGrid"), showGrid);
    emit showGridChanged(showGrid);
}

void Preferences::setShowTileObjectOutlines(bool enabled)
{
    setValue(QLatin1String("Interface/ShowTileObjectOutlines"), enabled);
    emit showTileObjectOutlinesChanged(enabled);
}

void Preferences::setShowTileAnimations(bool enabled)
{
    setValue(QLatin1String("Interface/ShowTileAnimations"), enabled);
    TilesetManager::instance()->setAnimateTiles(enabled);
    emit showTileAnimationsChanged(enabled);
}

void Preferences::setShowTileCollisionShapes(bool enabled)
{
    setValue(QLatin1String("Interface/ShowTileCollisionShapes"), enabled);
    emit showTileCollisionShapesChanged(enabled);
}

void Preferences::setShowObjectReferences(bool enabled)
{
    setValue(QLatin1String("Interface/ShowObjectReferences"), enabled);
    emit showObjectReferencesChanged(enabled);
}

void Preferences::setSnapToGrid(bool snapToGrid)
{
    setValue(QLatin1String("Interface/SnapToGrid"), snapToGrid);
    emit snapToGridChanged(snapToGrid);
}

void Preferences::setSnapToFineGrid(bool snapToFineGrid)
{
    setValue(QLatin1String("Interface/SnapToFineGrid"), snapToFineGrid);
    emit snapToFineGridChanged(snapToFineGrid);
}

void Preferences::setSnapToPixels(bool snapToPixels)
{
    setValue(QLatin1String("Interface/SnapToPixels"), snapToPixels);
    emit snapToPixelsChanged(snapToPixels);
}

void Preferences::setGridColor(QColor gridColor)
{
    setValue(QLatin1String("Interface/GridColor"), gridColor.name());
    emit gridColorChanged(gridColor);
}

void Preferences::setGridFine(int gridFine)
{
    setValue(QLatin1String("Interface/GridFine"), gridFine);
    emit gridFineChanged(gridFine);
}

void Preferences::setObjectLineWidth(qreal lineWidth)
{
    setValue(QLatin1String("Interface/ObjectLineWidth"), lineWidth);
    emit objectLineWidthChanged(lineWidth);
}

void Preferences::setHighlightCurrentLayer(bool highlight)
{
    setValue(QLatin1String("Interface/HighlightCurrentLayer"), highlight);
    emit highlightCurrentLayerChanged(highlight);
}

void Preferences::setHighlightHoveredObject(bool highlight)
{
    setValue(QLatin1String("Interface/HighlightHoveredObject"), highlight);
    emit highlightHoveredObjectChanged(highlight);
}

void Preferences::setShowTilesetGrid(bool showTilesetGrid)
{
    setValue(QLatin1String("Interface/ShowTilesetGrid"), showTilesetGrid);
    emit showTilesetGridChanged(showTilesetGrid);
}

void Preferences::setLayerDataFormat(Map::LayerDataFormat layerDataFormat)
{
    setValue(QLatin1String("Storage/LayerDataFormat"), layerDataFormat);
}

Map::RenderOrder Preferences::mapRenderOrder() const
{
    return static_cast<Map::RenderOrder>(get<int>("Storage/MapRenderOrder", Map::RightDown));
}

void Preferences::setMapRenderOrder(Map::RenderOrder mapRenderOrder)
{
    setValue(QLatin1String("Storage/MapRenderOrder"), mapRenderOrder);
}

bool Preferences::safeSavingEnabled() const
{
    return get("SafeSavingEnabled", true);
}

void Preferences::setSafeSavingEnabled(bool enabled)
{
    setValue(QLatin1String("Storage/SafeSavingEnabled"), enabled);
    SaveFile::setSafeSavingEnabled(enabled);
}

bool Preferences::exportOnSave() const
{
    return get("Storage/ExportOnSave", false);
}

void Preferences::setExportOnSave(bool enabled)
{
    setValue(QLatin1String("Storage/ExportOnSave"), enabled);
}

Preferences::ExportOptions Preferences::exportOptions() const
{
    ExportOptions options;

    if (get("Export/EmbedTilesets", false))
        options |= EmbedTilesets;
    if (get("Export/DetachTemplateInstances", false))
        options |= DetachTemplateInstances;
    if (get("Export/ResolveObjectTypesAndProperties", false))
        options |= ResolveObjectTypesAndProperties;
    if (get("Export/Minimized", false))
        options |= ExportMinimized;

    return options;
}

void Preferences::setExportOption(Preferences::ExportOption option, bool value)
{
    switch (option) {
    case EmbedTilesets:
        setValue(QLatin1String("Export/EmbedTilesets"), value);
        break;
    case DetachTemplateInstances:
        setValue(QLatin1String("Export/DetachTemplateInstances"), value);
        break;
    case ResolveObjectTypesAndProperties:
        setValue(QLatin1String("Export/ResolveObjectTypesAndProperties"), value);
        break;
    case ExportMinimized:
        setValue(QLatin1String("Export/Minimized"), value);
        break;
    }
}

bool Preferences::exportOption(Preferences::ExportOption option) const
{
    switch (option) {
    case EmbedTilesets:
        return get("Export/EmbedTilesets", false);
    case DetachTemplateInstances:
        return get("Export/DetachTemplateInstances", false);
    case ResolveObjectTypesAndProperties:
        return get("Export/ResolveObjectTypesAndProperties", false);
    case ExportMinimized:
        return get("Export/Minimized", false);
    }
    return false;
}

QString Preferences::language() const
{
    return get<QString>("Interface/Language");
}

void Preferences::setLanguage(const QString &language)
{
    setValue(QLatin1String("Interface/Language"), language);
    LanguageManager::instance()->installTranslators();
    emit languageChanged();
}

bool Preferences::reloadTilesetsOnChange() const
{
    return get("Storage/ReloadTilesets", true);
}

void Preferences::setReloadTilesetsOnChanged(bool reloadOnChanged)
{
    setValue(QLatin1String("Storage/ReloadTilesets"), reloadOnChanged);
    TilesetManager::instance()->setReloadTilesetsOnChange(reloadOnChanged);
}

bool Preferences::useOpenGL() const
{
    return get("Interface/OpenGL", false);
}

void Preferences::setUseOpenGL(bool useOpenGL)
{
    setValue(QLatin1String("Interface/OpenGL"), useOpenGL);
    emit useOpenGLChanged(useOpenGL);
}

void Preferences::setObjectTypes(const ObjectTypes &objectTypes)
{
    Object::setObjectTypes(objectTypes);
    emit objectTypesChanged();
}

static QString lastPathKey(Preferences::FileType fileType)
{
    QString key = QLatin1String("LastPaths/");

    switch (fileType) {
    case Preferences::ExportedFile:
        key.append(QLatin1String("ExportedFile"));
        break;
    case Preferences::ExternalTileset:
        key.append(QLatin1String("ExternalTileset"));
        break;
    case Preferences::ImageFile:
        key.append(QLatin1String("Images"));
        break;
    case Preferences::ObjectTemplateFile:
        key.append(QLatin1String("ObjectTemplates"));
        break;
    case Preferences::ObjectTypesFile:
        key.append(QLatin1String("ObjectTypes"));
        break;
    case Preferences::ProjectFile:
        key.append(QLatin1String("Project"));
        break;
    case Preferences::WorldFile:
        key.append(QLatin1String("WorldFile"));
        break;
    }

    return key;
}

/**
 * Returns the last location of a file chooser for the given file type. As long
 * as it was set using setLastPath().
 *
 * When no last path for this file type exists yet, the path of the currently
 * selected map is returned.
 *
 * When no map is open, the user's 'Documents' folder is returned.
 */
QString Preferences::lastPath(FileType fileType) const
{
    QString path = value(lastPathKey(fileType)).toString();

    if (path.isEmpty()) {
        DocumentManager *documentManager = DocumentManager::instance();
        Document *document = documentManager->currentDocument();
        if (document)
            path = QFileInfo(document->fileName()).path();
    }

    if (path.isEmpty()) {
        path = QStandardPaths::writableLocation(
                    QStandardPaths::DocumentsLocation);
    }

    return path;
}

/**
 * \see lastPath()
 */
void Preferences::setLastPath(FileType fileType, const QString &path)
{
    if (path.isEmpty())
        return;

    setValue(lastPathKey(fileType), path);
}

QDate Preferences::firstRun() const
{
    return get<QDate>("Install/FirstRun");
}

int Preferences::runCount() const
{
    return get<int>("Install/RunCount", 0);
}

bool Preferences::isPatron() const
{
    return get("Install/IsPatron", false);
}

void Preferences::setPatron(bool isPatron)
{
    setValue(QLatin1String("Install/IsPatron"), isPatron);
    emit isPatronChanged();
}

bool Preferences::shouldShowDonationDialog() const
{
    if (isPatron())
        return false;
    if (runCount() < 7)
        return false;

    const QDate dialogTime = donationDialogTime();
    if (!dialogTime.isValid())
        return false;

    return dialogTime.daysTo(QDate::currentDate()) >= 0;
}

QDate Preferences::donationDialogTime() const
{
    return get<QDate>("Install/DonationDialogTime");
}

void Preferences::setDonationDialogReminder(const QDate &date)
{
    if (date.isValid())
        setPatron(false);
    setValue(QLatin1String("Install/DonationDialogTime"), date.toString(Qt::ISODate));
}

QString Preferences::fileDialogStartLocation() const
{
    if (!mSession.activeFile.isEmpty())
        return QFileInfo(mSession.activeFile).path();

    if (!mSession.recentFiles.isEmpty())
        return QFileInfo(mSession.recentFiles.first()).path();

    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
}

/**
 * Adds the given file to the recent files list.
 */
void Preferences::addRecentFile(const QString &fileName)
{
    mSession.addRecentFile(fileName);
    saveSession();
    emit recentFilesChanged();
}

QStringList Preferences::recentProjects() const
{
    return get<QStringList>("Project/RecentProjects");
}

void Preferences::addRecentProject(const QString &fileName)
{
    setLastPath(ProjectFile, fileName);

    auto files = get<QStringList>("Project/RecentProjects");
    addToRecentFileList(fileName, files);
    setValue(QLatin1String("Project/RecentProjects"), files);
    emit recentProjectsChanged();
}

QString Preferences::lastSession() const
{
    const auto session = get<QString>("Project/LastSession");
    return session.isEmpty() ? Session::defaultFileName() : session;
}

void Preferences::setLastSession(const QString &fileName)
{
    setValue(QLatin1String("Project/LastSession"), fileName);
}

bool Preferences::restoreSessionOnStartup() const
{
    return get("Startup/RestorePreviousSession", true);
}

void Preferences::switchSession(Session session)
{
    mSession = std::move(session);
    setLastSession(mSession.fileName());

    Session::notifySessionChanged();
    emit recentFilesChanged();
}

void Preferences::saveSession()
{
    if (!mSaveSessionTimer.isActive())
        mSaveSessionTimer.start();
}

void Preferences::saveSessionNow()
{
    emit aboutToSaveSession();
    mSaveSessionTimer.stop();
    mSession.save();
}

void Preferences::addToRecentFileList(const QString &fileName, QStringList& files)
{
    // Remember the file by its absolute file path (not the canonical one,
    // which avoids unexpected paths when symlinks are involved).
    const QString absoluteFilePath = QDir::cleanPath(QFileInfo(fileName).absoluteFilePath());
    if (absoluteFilePath.isEmpty())
        return;

    files.removeAll(absoluteFilePath);
    files.prepend(absoluteFilePath);
    while (files.size() > MaxRecentFiles)
        files.removeLast();
}

void Preferences::clearRecentFiles()
{
    mSession.recentFiles.clear();
    saveSession();
    emit recentFilesChanged();
}

void Preferences::clearRecentProjects()
{
    remove(QLatin1String("Project/RecentProjects"));
    emit recentProjectsChanged();
}

bool Preferences::checkForUpdates() const
{
    return get("Install/CheckForUpdates", true);
}

void Preferences::setCheckForUpdates(bool on)
{
    setValue(QLatin1String("Install/CheckForUpdates"), on);
    emit checkForUpdatesChanged(on);
}

bool Preferences::displayNews() const
{
    return get("Install/DisplayNews", true);
}

void Preferences::setDisplayNews(bool on)
{
    setValue(QLatin1String("Install/DisplayNews"), on);
    emit displayNewsChanged(on);
}

bool Preferences::wheelZoomsByDefault() const
{
    return get("Interface/WheelZoomsByDefault", false);
}

void Preferences::setRestoreSessionOnStartup(bool enabled)
{
    setValue(QLatin1String("Startup/RestorePreviousSession"), enabled);
}

void Preferences::setPluginEnabled(const QString &fileName, bool enabled)
{
    PluginManager *pluginManager = PluginManager::instance();
    pluginManager->setPluginState(fileName, enabled ? PluginEnabled : PluginDisabled);

    QStringList disabledPlugins;
    QStringList enabledPlugins;

    auto &states = pluginManager->pluginStates();

    for (auto it = states.begin(), it_end = states.end(); it != it_end; ++it) {
        const QString &fileName = it.key();
        PluginState state = it.value();
        switch (state) {
        case PluginEnabled:
            enabledPlugins.append(fileName);
            break;
        case PluginDisabled:
            disabledPlugins.append(fileName);
            break;
        case PluginDefault:
        case PluginStatic:
            break;
        }
    }

    setValue(QLatin1String("Plugins/Disabled"), disabledPlugins);
    setValue(QLatin1String("Plugins/Enabled"), enabledPlugins);
}

void Preferences::setWheelZoomsByDefault(bool mode)
{
    setValue(QLatin1String("Interface/WheelZoomsByDefault"), mode);
}

QString Preferences::dataLocation()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString Preferences::objectTypesFile() const
{
    QString file = get<QString>("Storage/ObjectTypesFile");
    if (file.isEmpty())
        return dataLocation() + QLatin1String("/objecttypes.xml");

    return file;
}

void Preferences::setObjectTypesFile(const QString &fileName)
{
    QString previousObjectTypesFile = objectTypesFile();
    if (previousObjectTypesFile == fileName)
        return;

    if (!previousObjectTypesFile.isEmpty())
        mWatcher.removePath(previousObjectTypesFile);

    setValue(QLatin1String("Storage/ObjectTypesFile"), fileName);
    mWatcher.addPath(fileName);
}

void Preferences::setObjectTypesFileLastSaved(const QDateTime &time)
{
    mObjectTypesFileLastSaved = time;
}

void Preferences::objectTypesFileChangedOnDisk()
{
    const QFileInfo fileInfo { objectTypesFile() };
    if (fileInfo.lastModified() == mObjectTypesFileLastSaved)
        return;

    ObjectTypesSerializer objectTypesSerializer;
    ObjectTypes objectTypes;

    if (objectTypesSerializer.readObjectTypes(fileInfo.filePath(), objectTypes))
        setObjectTypes(objectTypes);
}
