// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "qmakeproject.h"

#include "qmakebuildconfiguration.h"
#include "qmakenodes.h"
#include "qmakenodetreebuilder.h"
#include "qmakeparser.h"
#include "qmakeprojectimporter.h"
#include "qmakeprojectmanagerconstants.h"
#include "qmakeprojectmanagertr.h"
#include "qmakestep.h"

#include <coreplugin/documentmanager.h>
#include <coreplugin/icontext.h>
#include <coreplugin/icore.h>
#include <coreplugin/progressmanager/progressmanager.h>

#include <cppeditor/cppmodelmanager.h>
#include <cppeditor/generatedcodemodelsupport.h>

#include <projectexplorer/buildinfo.h>
#include <projectexplorer/buildmanager.h>
#include <projectexplorer/buildsteplist.h>
#include <projectexplorer/buildtargetinfo.h>
#include <projectexplorer/deploymentdata.h>
#include <projectexplorer/devicesupport/devicekitaspects.h>
#include <projectexplorer/devicesupport/idevice.h>
#include <projectexplorer/extracompiler.h>
#include <projectexplorer/headerpath.h>
#include <projectexplorer/kitmanager.h>
#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/projectupdater.h>
#include <projectexplorer/rawprojectpart.h>
#include <projectexplorer/runconfiguration.h>
#include <projectexplorer/sysrootkitaspect.h>
#include <projectexplorer/target.h>
#include <projectexplorer/taskhub.h>
#include <projectexplorer/toolchain.h>
#include <projectexplorer/toolchainkitaspect.h>
#include <projectexplorer/toolchainmanager.h>

#include <proparser/qmakevfs.h>
#include <proparser/qmakeglobals.h>

#include <qtsupport/profilereader.h>
#include <qtsupport/qtcppkitinfo.h>
#include <qtsupport/qtkitaspect.h>
#include <qtsupport/qtversionmanager.h>

#include <utils/algorithm.h>
#include <utils/async.h>
#include <utils/mimeconstants.h>
#include <utils/qtcprocess.h>

#include <QDebug>
#include <QDir>
#include <QFileSystemWatcher>
#include <QFuture>
#include <QLoggingCategory>
#include <QTimer>

using namespace QmakeProjectManager::Internal;
using namespace ProjectExplorer;
using namespace QtSupport;
using namespace Utils;

namespace QmakeProjectManager {
namespace Internal {

static Q_LOGGING_CATEGORY(qmakeBuildSystemLog, "qtc.qmake.buildsystem", QtWarningMsg);

#define TRACE(msg)                                                   \
    if (qmakeBuildSystemLog().isDebugEnabled()) {                    \
        qCDebug(qmakeBuildSystemLog)                                 \
            << qPrintable(buildConfiguration()->displayName())       \
            << ", guards project: " << int(m_guard.guardsProject())  \
            << ", isParsing: " << int(isParsing())                   \
            << ", hasParsingData: " << int(hasParsingData())         \
            << ", " << __FUNCTION__                                  \
            << msg;                                                  \
    }

class QmakePriFileDocument : public Core::IDocument
{
public:
    QmakePriFileDocument(QmakePriFile *qmakePriFile, const FilePath &filePath) :
        IDocument(nullptr), m_priFile(qmakePriFile)
    {
        setId("Qmake.PriFile");
        setMimeType(Utils::Constants::PROFILE_MIMETYPE);
        setFilePath(filePath);
        Core::DocumentManager::addDocument(this);
    }

    ReloadBehavior reloadBehavior(ChangeTrigger state, ChangeType type) const override
    {
        Q_UNUSED(state)
        Q_UNUSED(type)
        return BehaviorSilent;
    }
    Result<> reload(ReloadFlag flag, ChangeType type) override
    {
        Q_UNUSED(flag)
        Q_UNUSED(type)
        if (m_priFile)
            m_priFile->scheduleUpdate();
        return ResultOk;
    }

    void setPriFile(QmakePriFile *priFile) { m_priFile = priFile; }

private:
    QmakePriFile *m_priFile;
};

/// Watches folders for QmakePriFile nodes
/// use one file system watcher to watch all folders
/// such minimizing system ressouce usage

class CentralizedFolderWatcher : public QObject
{
    Q_OBJECT
public:
    CentralizedFolderWatcher(QmakeBuildSystem *BuildSystem);

    void watchFolders(const QList<QString> &folders, QmakePriFile *file);
    void unwatchFolders(const QList<QString> &folders, QmakePriFile *file);

private:
    void folderChanged(const QString &folder);
    void onTimer();
    void delayedFolderChanged(const QString &folder);

    QmakeBuildSystem *m_buildSystem;
    QSet<QString> recursiveDirs(const QString &folder);
    QFileSystemWatcher m_watcher;
    QMultiMap<QString, QmakePriFile *> m_map;

    QSet<QString> m_recursiveWatchedFolders;
    QTimer m_compressTimer;
    QSet<QString> m_changedFolders;
};

} // namespace Internal

/*!
  \class QmakeProject

  QmakeProject manages information about an individual qmake project file (.pro).
  */

QmakeProject::QmakeProject(const FilePath &fileName) :
    Project(Utils::Constants::PROFILE_MIMETYPE, fileName)
{
    setType(Constants::QMAKEPROJECT_ID);
    setProjectLanguages(Core::Context(ProjectExplorer::Constants::CXX_LANGUAGE_ID));
    setDisplayName(fileName.completeBaseName());
    setCanBuildProducts();
    setHasMakeInstallEquivalent(true);
    setBuildSystemCreator<QmakeBuildSystem>("qmake");
}

QmakeProject::~QmakeProject()
{
    delete m_projectImporter;
    m_projectImporter = nullptr;

    // Make sure root node (and associated readers) are shut hown before proceeding
    setRootProjectNode(nullptr);
}

Project::RestoreResult QmakeProject::fromMap(const Store &map, QString *errorMessage)
{
    RestoreResult result = Project::fromMap(map, errorMessage);
    if (result != RestoreResult::Ok)
        return result;

    // Prune targets without buildconfigurations:
    // This can happen esp. when updating from a old version of Qt Creator
    const QList<Target *> ts = targets();
    for (Target *t : ts) {
        if (t->buildConfigurations().isEmpty()) {
            qWarning() << "Removing" << t->id().name() << "since it has no buildconfigurations!";
            removeTarget(t);
        }
    }

    return RestoreResult::Ok;
}

DeploymentKnowledge QmakeProject::deploymentKnowledge() const
{
    return DeploymentKnowledge::Approximative; // E.g. QTCREATORBUG-21855
}

//
// QmakeBuildSystem
//

QmakeBuildSystem::QmakeBuildSystem(BuildConfiguration *bc)
    : BuildSystem(bc)
    , m_qmakeVfs(new QMakeVfs)
    , m_cppCodeModelUpdater(ProjectUpdaterFactory::createCppProjectUpdater())
{
    m_rootProFile = std::make_unique<QmakeProFile>(this, projectFilePath());

    connect(BuildManager::instance(), &BuildManager::buildQueueFinished,
            this, &QmakeBuildSystem::buildFinished);

    connect(bc->project(),
            &Project::activeBuildConfigurationChanged,
            this,
            [this](BuildConfiguration *bc) {
                if (bc == buildConfiguration())
                    scheduleUpdateAllNowOrLater();
                // FIXME: This is too eager in the presence of not handling updates
                // when the build configuration is not active, see startAsyncTimer
                // below.
                //        else
                //            m_cancelEvaluate = true;
            });

    connect(bc->project(), &Project::activeTargetChanged,
            this, &QmakeBuildSystem::activeTargetWasChanged);

    connect(bc->project(), &Project::projectFileIsDirty,
            this, &QmakeBuildSystem::scheduleUpdateAllLater);

    connect(bc, &BuildConfiguration::buildDirectoryChanged,
            this, &QmakeBuildSystem::scheduleUpdateAllNowOrLater);
    connect(bc, &BuildConfiguration::environmentChanged,
            this, &QmakeBuildSystem::scheduleUpdateAllNowOrLater);

    connect(ToolchainManager::instance(), &ToolchainManager::toolchainUpdated,
            this, [this](Toolchain *tc) {
        if (ToolchainKitAspect::cxxToolchain(kit()) == tc)
            scheduleUpdateAllNowOrLater();
    });

    connect(QtVersionManager::instance(), &QtVersionManager::qtVersionsChanged,
            this, [this](const QList<int> &,const QList<int> &, const QList<int> &changed) {
        if (changed.contains(QtKitAspect::qtVersionId(kit())))
            scheduleUpdateAllNowOrLater();
    });
}

QmakeBuildSystem::~QmakeBuildSystem()
{
    // Trigger any pending parsingFinished signals before destroying any other build system part:
    m_guard = {};
    delete m_cppCodeModelUpdater;
    m_cppCodeModelUpdater = nullptr;
    m_asyncUpdateState = ShuttingDown;

    // Make sure root node (and associated readers) are shut hown before proceeding
    m_rootProFile.reset();
    if (m_qmakeGlobalsRefCnt > 0) {
        m_qmakeGlobalsRefCnt = 0;
        deregisterFromCacheManager();
    }

    m_cancelEvaluate = true;
    QTC_CHECK(m_qmakeGlobalsRefCnt == 0);
    delete m_qmakeVfs;
    m_qmakeVfs = nullptr;

    if (m_asyncUpdateFutureInterface) {
        m_asyncUpdateFutureInterface->reportCanceled();
        m_asyncUpdateFutureInterface->reportFinished();
        m_asyncUpdateFutureInterface.reset();
    }
}

void QmakeBuildSystem::updateCodeModels()
{
    if (!buildConfiguration()->isActive())
        return;

    updateCppCodeModel();
    updateQmlCodeModel();
}

void QmakeBuildSystem::updateDocuments()
{
    QSet<FilePath> projectDocuments;
    project()->rootProjectNode()->forEachProjectNode([&projectDocuments](const ProjectNode *n) {
        projectDocuments.insert(n->filePath());

    });
    const auto priFileForPath = [p = project()](const FilePath &fp) -> QmakePriFile * {
        const Node * const n = p->nodeForFilePath(fp, [](const Node *n) {
            return dynamic_cast<const QmakePriFileNode *>(n); });
        QTC_ASSERT(n, return nullptr);
        return static_cast<const QmakePriFileNode *>(n)->priFile();
    };
    const auto docGenerator = [&](const FilePath &fp)
            -> std::unique_ptr<Core::IDocument> {
        QmakePriFile * const priFile = priFileForPath(fp);
        QTC_ASSERT(priFile, return std::make_unique<Core::IDocument>());
        return std::make_unique<QmakePriFileDocument>(priFile, fp);
    };
    const auto docUpdater = [&](Core::IDocument *doc) {
        QmakePriFile * const priFile = priFileForPath(doc->filePath());
        QTC_ASSERT(priFile, return);
        static_cast<QmakePriFileDocument *>(doc)->setPriFile(priFile);
    };
    project()->setExtraProjectFiles(projectDocuments, docGenerator, docUpdater);
}

void QmakeBuildSystem::updateCppCodeModel()
{
    m_toolChainWarnings.clear();

    QtSupport::CppKitInfo kitInfo(kit());
    QTC_ASSERT(kitInfo.isValid(), return);

    QList<ProjectExplorer::ExtraCompiler *> generators;
    RawProjectParts rpps;
    for (const QmakeProFile *pro : rootProFile()->allProFiles()) {
        warnOnToolChainMismatch(pro);
        RawProjectPart rpp;
        rpp.setDisplayName(pro->displayName());
        rpp.setProjectFileLocation(pro->filePath());
        rpp.setBuildSystemTarget(pro->filePath().toUrlishString());
        switch (pro->projectType()) {
        case ProjectType::ApplicationTemplate:
            rpp.setBuildTargetType(BuildTargetType::Executable);
            break;
        case ProjectType::SharedLibraryTemplate:
        case ProjectType::StaticLibraryTemplate:
            rpp.setBuildTargetType(BuildTargetType::Library);
            break;
        default:
            rpp.setBuildTargetType(BuildTargetType::Unknown);
            break;
        }
        const FilePath includeFileBaseDir = pro->sourceDir();

        QStringList cxxArgs = pro->variableValue(Variable::CppFlags);
        QStringList cArgs = pro->variableValue(Variable::CFlags);

        // For broken mkspecs, see QTCREATORBUG-28201.
        const auto getExtraFlagsFromCompilerVar = [pro](Variable var) {
            const QStringList value = pro->variableValue(var);
            const int firstOptIndex = Utils::indexOf(value, [](const QString &arg) {
                return arg.startsWith('-');
            });
            if (firstOptIndex <= 0)
                return QStringList();
            return value.mid(firstOptIndex);
        };

        const QStringList extraCxxArgs = getExtraFlagsFromCompilerVar(Variable::QmakeCxx);
        cxxArgs << extraCxxArgs;
        const QStringList extraCArgs = getExtraFlagsFromCompilerVar(Variable::QmakeCc);
        cArgs << extraCArgs;
        addTargetFlagForIos(cArgs, cxxArgs, this, [pro] {
            return pro->variableValue(Variable::IosDeploymentTarget).join(QString());
        });

        rpp.setFlagsForCxx({kitInfo.cxxToolchain, cxxArgs, includeFileBaseDir});
        rpp.setFlagsForC({kitInfo.cToolchain, cArgs, includeFileBaseDir});
        rpp.setMacros(Macro::toMacros(pro->cxxDefines()));
        rpp.setPreCompiledHeaders(pro->filePathsValue(Variable::PrecompiledHeader));
        rpp.setSelectedForBuilding(pro->includedInExactParse());

        // Qt Version
        if (pro->variableValue(Variable::Config).contains(QLatin1String("qt")))
            rpp.setQtVersion(kitInfo.projectPartQtVersion);
        else
            rpp.setQtVersion(QtMajorVersion::None);

        // Header paths
        HeaderPaths headerPaths;
        const FilePaths includes = pro->filePathsValue(Variable::IncludePath);
        for (const FilePath &inc : includes) {
            const HeaderPath headerPath = HeaderPath::makeUser(inc);
            if (!headerPaths.contains(headerPath))
                headerPaths += headerPath;
        }

        if (kitInfo.qtVersion && !kitInfo.qtVersion->frameworkPath().isEmpty())
            headerPaths += HeaderPath::makeFramework(kitInfo.qtVersion->frameworkPath());
        rpp.setHeaderPaths(headerPaths);

        // Files and generators
        const FilePaths cumulativeSourceFiles = pro->filePathsValue(Variable::CumulativeSource);
        FilePaths fileList = pro->filePathsValue(Variable::ExactSource);
        fileList += cumulativeSourceFiles;
        const QList<ExtraCompiler *> proGenerators = pro->extraCompilers();
        for (ExtraCompiler *ec : proGenerators) {
            ec->forEachTarget([&](const FilePath &generatedFile) { fileList += generatedFile; });
        }
        generators.append(proGenerators);
        fileList.prepend(CppEditor::CppModelManager::configurationFileName());
        rpp.setFiles(fileList, [cumulativeSourceFiles](const FilePath &filePath) {
            // Keep this lambda thread-safe!
            return !cumulativeSourceFiles.contains(filePath);
        });

        rpps.append(rpp);
    }

    m_cppCodeModelUpdater->update({project(), kitInfo, activeParseEnvironment(), rpps}, generators);
}

void QmakeBuildSystem::updateQmlCodeModelInfo(QmlCodeModelInfo &projectInfo)
{
    const QList<QmakeProFile *> proFiles = rootProFile()->allProFiles();
    const QString device = rootProFile()->deviceRoot();

    bool hasQmlLib = false;
    for (QmakeProFile *file : proFiles) {
        for (const QString &path : file->variableValue(Variable::QmlImportPath))
            projectInfo.qmlImportPaths.append(FilePath::fromString(path));
        const QStringList &exactResources = file->variableValue(Variable::ExactResource);
        const QStringList &cumulativeResources = file->variableValue(Variable::CumulativeResource);
        QString errorMessage;
        for (const QString &rc : exactResources) {
            FilePath rcPath = FilePath::fromString(rc);
            projectInfo.activeResourceFiles.append(rcPath);
            projectInfo.allResourceFiles.append(rcPath);
            QString contents;
            int id = m_qmakeVfs->idForFileName(device + rc, QMakeVfs::VfsExact);
            if (m_qmakeVfs->readFile(id, &contents, &errorMessage) == QMakeVfs::ReadOk)
                projectInfo.resourceFileContents[rcPath] = contents;
        }
        for (const QString &rc : cumulativeResources) {
            FilePath rcPath = FilePath::fromString(rc);
            projectInfo.allResourceFiles.append(rcPath);
            QString contents;
            int id = m_qmakeVfs->idForFileName(device + rc, QMakeVfs::VfsCumulative);
            if (m_qmakeVfs->readFile(id, &contents, &errorMessage) == QMakeVfs::ReadOk)
                projectInfo.resourceFileContents[rcPath] = contents;
        }
        if (!hasQmlLib) {
            QStringList qtLibs = file->variableValue(Variable::Qt);
            hasQmlLib = qtLibs.contains(QLatin1String("declarative")) ||
                    qtLibs.contains(QLatin1String("qml")) ||
                    qtLibs.contains(QLatin1String("quick"));
        }
    }

    // If the project directory has a pro/pri file that includes a qml or quick or declarative
    // library then chances of the project being a QML project is quite high.
    // This assumption fails when there are no QDeclarativeEngine/QDeclarativeView (QtQuick 1)
    // or QQmlEngine/QQuickView (QtQuick 2) instances.
    project()->setProjectLanguage(ProjectExplorer::Constants::QMLJS_LANGUAGE_ID, hasQmlLib);

    projectInfo.activeResourceFiles = Utils::filteredUnique(projectInfo.activeResourceFiles);
    projectInfo.allResourceFiles = Utils::filteredUnique(projectInfo.allResourceFiles);
}

void QmakeBuildSystem::scheduleAsyncUpdateFile(QmakeProFile *file, QmakeProFile::AsyncUpdateDelay delay)
{
    if (m_asyncUpdateState == ShuttingDown)
        return;

    if (m_cancelEvaluate) {
        // A cancel is in progress
        // That implies that a full update is going to happen afterwards
        // So we don't need to do anything
        return;
    }

    file->setParseInProgressRecursive(true);

    if (m_asyncUpdateState == AsyncFullUpdatePending) {
        // Just postpone
        startAsyncTimer(delay);
    } else if (m_asyncUpdateState == AsyncPartialUpdatePending
               || m_asyncUpdateState == Base) {
        // Add the node
        m_asyncUpdateState = AsyncPartialUpdatePending;

        bool add = true;
        auto it = m_partialEvaluate.begin();
        while (it != m_partialEvaluate.end()) {
            if (*it == file) {
                add = false;
                break;
            } else if (file->isParent(*it)) { // We already have the parent in the list, nothing to do
                it = m_partialEvaluate.erase(it);
            } else if ((*it)->isParent(file)) { // The node is the parent of a child already in the list
                add = false;
                break;
            } else {
                ++it;
            }
        }

        if (add)
            m_partialEvaluate.append(file);

        // Cancel running code model update
        m_cppCodeModelUpdater->cancel();

        startAsyncTimer(delay);
    } else if (m_asyncUpdateState == AsyncUpdateInProgress) {
        // A update is in progress
        // And this slot only gets called if a file changed on disc
        // So we'll play it safe and schedule a complete evaluate
        // This might trigger if due to version control a few files
        // change a partial update gets in progress and then another
        // batch of changes come in, which triggers a full update
        // even if that's not really needed
        scheduleUpdateAll(delay);
    }
}

void QmakeBuildSystem::scheduleUpdateAllNowOrLater()
{
    if (m_firstParseNeeded)
        scheduleUpdateAll(QmakeProFile::ParseNow);
    else
        scheduleUpdateAll(QmakeProFile::ParseLater);
}

ExtraCompiler *QmakeBuildSystem::findExtraCompiler(const ExtraCompilerFilter &filter) const
{
    return m_rootProFile->findExtraCompiler(filter);
}

QmakeBuildConfiguration *QmakeBuildSystem::qmakeBuildConfiguration() const
{
    return static_cast<QmakeBuildConfiguration *>(BuildSystem::buildConfiguration());
}

void QmakeBuildSystem::scheduleUpdateAll(QmakeProFile::AsyncUpdateDelay delay)
{
    if (m_asyncUpdateState == ShuttingDown) {
        TRACE("suppressed: we are shutting down");
        return;
    }

    if (m_cancelEvaluate) { // we are in progress of canceling
                            // and will start the evaluation after that
        TRACE("suppressed: was previously canceled");
        return;
    }

    if (!buildConfiguration()->isActive()) {
        TRACE("firstParseNeeded: " << int(m_firstParseNeeded)
              << ", suppressed: buildconfig not active");
        return;
    }

    TRACE("firstParseNeeded: " << int(m_firstParseNeeded) << ", delay: " << delay);

    rootProFile()->setParseInProgressRecursive(true);

    if (m_asyncUpdateState == AsyncUpdateInProgress) {
        m_cancelEvaluate = true;
        m_asyncUpdateState = AsyncFullUpdatePending;
        return;
    }

    m_partialEvaluate.clear();
    m_asyncUpdateState = AsyncFullUpdatePending;

    // Cancel running code model update
    m_cppCodeModelUpdater->cancel();
    startAsyncTimer(delay);
}

void QmakeBuildSystem::startAsyncTimer(QmakeProFile::AsyncUpdateDelay delay)
{
    if (!buildConfiguration()->isActive()) {
        TRACE("skipped, not active")
        return;
    }

    TRACE("delay: " << delay);
    switch (delay) {
    case QmakeProFile::ParseNow: requestParse(); break;
    case QmakeProFile::ParseLater: requestDelayedParse(); break;
    }
}

void QmakeBuildSystem::incrementPendingEvaluateFutures()
{
    if (m_pendingEvaluateFuturesCount == 0) {
        // The guard actually might already guard the project if this
        // here is the re-start of a previously aborted parse due to e.g.
        // changing build directories while parsing.
        if (!m_guard.guardsProject())
            m_guard = guardParsingRun();
    }
    ++m_pendingEvaluateFuturesCount;
    TRACE("pending inc to: " << m_pendingEvaluateFuturesCount);
    m_asyncUpdateFutureInterface->setProgressRange(m_asyncUpdateFutureInterface->progressMinimum(),
                                                   m_asyncUpdateFutureInterface->progressMaximum()
                                                       + 1);
}

void QmakeBuildSystem::decrementPendingEvaluateFutures()
{
    --m_pendingEvaluateFuturesCount;
    TRACE("pending dec to: " << m_pendingEvaluateFuturesCount);

    if (!rootProFile()) {
        TRACE("closing project");
        return; // We are closing the project!
    }

    m_asyncUpdateFutureInterface->setProgressValue(m_asyncUpdateFutureInterface->progressValue()
                                                   + 1);
    if (m_pendingEvaluateFuturesCount == 0) {
        // We are done!
        setRootProjectNode(QmakeNodeTreeBuilder::buildTree(this));

        if (!m_rootProFile->validParse())
            m_asyncUpdateFutureInterface->reportCanceled();

        m_asyncUpdateFutureInterface->reportFinished();
        m_asyncUpdateFutureInterface.reset();
        m_cancelEvaluate = false;

        // TODO clear the profile cache ?
        if (m_asyncUpdateState == AsyncFullUpdatePending || m_asyncUpdateState == AsyncPartialUpdatePending) {
            // Already parsing!
            rootProFile()->setParseInProgressRecursive(true);
            startAsyncTimer(QmakeProFile::ParseLater);
        } else  if (m_asyncUpdateState != ShuttingDown){
            // After being done, we need to call:

            m_asyncUpdateState = Base;
            updateBuildSystemData();
            updateCodeModels();
            updateDocuments();
            buildConfiguration()->updateDefaultDeployConfigurations();
            m_guard.markAsSuccess(); // Qmake always returns (some) data, even when it failed:-)
            TRACE("success" << int(m_guard.isSuccess()));
            m_guard = {}; // This triggers emitParsingFinished by destroying the previous guard.

            m_firstParseNeeded = false;
            TRACE("first parse succeeded");

            emitBuildSystemUpdated();
        }
    }
}

bool QmakeBuildSystem::wasEvaluateCanceled()
{
    return m_cancelEvaluate;
}

void QmakeBuildSystem::asyncUpdate()
{
    TaskHub::clearTasks(ProjectExplorer::Constants::TASK_CATEGORY_BUILDSYSTEM);
    TRACE("");

    if (m_invalidateQmakeVfsContents) {
        m_invalidateQmakeVfsContents = false;
        m_qmakeVfs->invalidateContents();
    } else {
        m_qmakeVfs->invalidateCache();
    }

    m_asyncUpdateFutureInterface.reset(new QFutureInterface<void>);
    m_asyncUpdateFutureInterface->setProgressRange(0, 0);
    Core::ProgressManager::addTask(m_asyncUpdateFutureInterface->future(),
                                   Tr::tr("Reading Project \"%1\"").arg(project()->displayName()),
                                   Constants::PROFILE_EVALUATE);

    m_asyncUpdateFutureInterface->reportStarted();
    const auto watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::canceled, this, [this] {
        if (m_qmakeGlobals)
            m_qmakeGlobals->killProcesses();
    });
    connect(watcher, &QFutureWatcher<void>::finished, watcher, &QObject::deleteLater);
    watcher->setFuture(m_asyncUpdateFutureInterface->future());

    const Kit *const k = kit();
    QtSupport::QtVersion *const qtVersion = QtSupport::QtKitAspect::qtVersion(k);
    if (!qtVersion || !qtVersion->isValid()) {
        const QString errorMessage
            = k ? Tr::tr("Cannot parse project \"%1\": The currently selected kit \"%2\" does not "
                         "have a valid Qt.")
                      .arg(project()->displayName(), k->displayName())
                : Tr::tr("Cannot parse project \"%1\": No kit selected.").arg(project()->displayName());
        proFileParseError(errorMessage, project()->projectFilePath());
        m_asyncUpdateFutureInterface->reportCanceled();
        m_asyncUpdateFutureInterface->reportFinished();
        m_asyncUpdateFutureInterface.reset();
        return;
    }

    // Make sure we ignore requests for re-evaluation for files whose QmakePriFile objects
    // will get deleted during the parse.
    const auto docUpdater = [](Core::IDocument *doc) {
        static_cast<QmakePriFileDocument *>(doc)->setPriFile(nullptr);
    };
    if (m_asyncUpdateState != AsyncFullUpdatePending) {
        QSet<FilePath> projectFilePaths;
        for (QmakeProFile * const file : std::as_const(m_partialEvaluate)) {
            QList<QmakePriFile *> priFiles = file->children();
            for (int i = 0; i < priFiles.count(); ++i) {
                const QmakePriFile * const priFile = priFiles.at(i);
                projectFilePaths << priFile->filePath();
                priFiles << priFile->children();
            }
        }
        project()->updateExtraProjectFiles(projectFilePaths, docUpdater);
    }

    if (m_asyncUpdateState == AsyncFullUpdatePending) {
        project()->updateExtraProjectFiles(docUpdater);
        rootProFile()->asyncUpdate();
    } else {
        for (QmakeProFile *file : std::as_const(m_partialEvaluate))
            file->asyncUpdate();
    }

    m_partialEvaluate.clear();
    m_asyncUpdateState = AsyncUpdateInProgress;
}

void QmakeBuildSystem::buildFinished(bool success)
{
    if (success)
        m_invalidateQmakeVfsContents = true;
}

Tasks QmakeProject::projectIssues(const Kit *k) const
{
    if (const Tasks result = Project::projectIssues(k); !result.isEmpty())
        return result;

    const QtSupport::QtVersion * const qtFromKit = QtSupport::QtKitAspect::qtVersion(k);
    QTC_ASSERT(qtFromKit, return {}); // Checked by "static" issues generator in base class.

    // A project can be considered part of more than one Qt version, for instance if it is an
    // example shipped via the installer.
    // Report a problem if and only if the project is considered to be part of *only* a Qt
    // that is not the one from the current kit.
    const QtVersions qtsContainingThisProject
            = QtVersionManager::versions([filePath = projectFilePath()](const QtVersion *qt) {
        return qt->isValid() && qt->isQtSubProject(filePath);
    });
    if (!qtsContainingThisProject.isEmpty()
            && !qtsContainingThisProject.contains(const_cast<QtVersion *>(qtFromKit))) {
        return {CompileTask(
            Task::Warning,
            Tr::tr("Project is part of Qt sources that do not match the Qt defined in the kit."))};
    }

    return {};
}

// Find the folder that contains a file with a certain name (recurse down)
static FolderNode *folderOf(FolderNode *in, const FilePath &filePath)
{
    if (in->findChildFileNode([&filePath](FileNode *fn) { return fn->filePath() == filePath; }))
        return in;

    return in->findChildFolderNode([&filePath](FolderNode *folder) {
        return folderOf(folder, filePath);
    });
}

// Find the QmakeProFileNode that contains a certain file.
// First recurse down to folder, then find the pro-file.
static FileNode *fileNodeOf(FolderNode *in, const FilePath &filePath)
{
    for (FolderNode *folder = folderOf(in, filePath); folder; folder = folder->parentFolderNode()) {
        if (auto proFile = dynamic_cast<QmakeProFileNode *>(folder)) {
            return proFile->findChildFileNode([&filePath](FileNode *fn) {
                return fn->filePath() == filePath;
            });
        }
    }
    return nullptr;
}

FilePath QmakeBuildSystem::buildDir(const FilePath &proFilePath) const
{
    const FilePath buildConfigBuildDir = buildConfiguration()->buildDirectory();
    const FilePath buildDir = buildConfigBuildDir.isEmpty()
                                 ? projectDirectory()
                                 : buildConfigBuildDir;

    // The remote version below is actually generic, but I don't dare to touch
    // the convoluted existing local version for now.
    // For starters, compute a 'new' version to check what it would look like,
    // but don't use it.
    if (proFilePath.isLocal()) {
        // This branch should not exist.
        const QDir srcDirRoot = QDir(projectDirectory().toUrlishString());
        const QString relativeDir = srcDirRoot.relativeFilePath(proFilePath.parentDir().toUrlishString());
        // FIXME: Convoluted. Try to migrate to newRes once we feel confident enough.
        const FilePath oldResult = buildDir.withNewPath(
                    QDir::cleanPath(QDir(buildDir.path()).absoluteFilePath(relativeDir)));
        const FilePath newResult = buildDir.resolvePath(relativeDir);
        QTC_ASSERT(oldResult == newResult,
                   qDebug() << "New build dir construction failed. Not equal:"
                            << oldResult.toUrlishString() << newResult.toUrlishString());
        return oldResult;
    }

    const QString relativeDir = proFilePath.parentDir().relativePathFromDir(projectDirectory());
    return buildDir.resolvePath(relativeDir).canonicalPath();
}

void QmakeBuildSystem::proFileParseError(const QString &errorMessage, const FilePath &filePath)
{
    TaskHub::addTask<QmakeTask>(Task::Error, errorMessage, filePath);
}

QtSupport::ProFileReader *QmakeBuildSystem::createProFileReader(const QmakeProFile *qmakeProFile)
{
    if (!m_qmakeGlobals) {
        m_qmakeGlobals = std::make_unique<QMakeGlobals>();
        m_qmakeGlobalsRefCnt = 0;

        QStringList qmakeArgs;

        Kit *k = kit();
        QmakeBuildConfiguration *bc = qmakeBuildConfiguration();

        Environment env = bc->environment();
        if (QMakeStep *qs = bc->qmakeStep())
            qmakeArgs = qs->parserArguments();
        else
            qmakeArgs = bc->configCommandLineArguments();

        QtSupport::QtVersion *qtVersion = QtSupport::QtKitAspect::qtVersion(k);
        m_qmakeSysroot = SysRootKitAspect::sysRoot(k);

        if (qtVersion && qtVersion->isValid()) {
            m_qmakeGlobals->qmake_abslocation =
                QDir::cleanPath(qtVersion->qmakeFilePath().path());
            qtVersion->applyProperties(m_qmakeGlobals.get());
        }

        QString rootProFileName = buildDir(rootProFile()->filePath()).path();
        m_qmakeGlobals->setDirectories(rootProFile()->sourceDir().path(),
                                       rootProFileName,
                                       deviceRoot());

        env.forEachEntry([&](const QString &key, const QString &value, bool enabled) {
            if (enabled)
                m_qmakeGlobals->environment.insert(key, env.expandVariables(value));
        });

        m_qmakeGlobals->setCommandLineArguments(rootProFileName, qmakeArgs);
        m_qmakeGlobals->runSystemFunction = bc->runQmakeSystemFunctions();

        QtSupport::ProFileCacheManager::instance()->incRefCount();

        // On ios, qmake is called recursively, and the second call with a different
        // spec.
        // macx-ios-clang just creates supporting makefiles, and to avoid being
        // slow does not evaluate everything, and contains misleading information
        // (that is never used).
        // macx-xcode correctly evaluates the variables and generates the xcodeproject
        // that is actually used to build the application.
        //
        // It is important to override the spec file only for the creator evaluator,
        // and not the qmake buildstep used to build the app (as we use the makefiles).
        const char IOSQT[] = "Qt4ProjectManager.QtVersion.Ios"; // from Ios::Constants
        if (qtVersion && qtVersion->type() == QLatin1String(IOSQT))
            m_qmakeGlobals->xqmakespec = QLatin1String("macx-xcode");
    }
    ++m_qmakeGlobalsRefCnt;

    auto reader = new QtSupport::ProFileReader(m_qmakeGlobals.get(), m_qmakeVfs);

    // Core parts of the ProParser hard-assert on non-local items
    reader->setOutputDir(buildDir(qmakeProFile->filePath()).path());

    return reader;
}

QMakeGlobals *QmakeBuildSystem::qmakeGlobals() const
{
    return m_qmakeGlobals.get();
}

QMakeVfs *QmakeBuildSystem::qmakeVfs() const
{
    return m_qmakeVfs;
}

const FilePath &QmakeBuildSystem::qmakeSysroot() const
{
    return m_qmakeSysroot;
}

void QmakeBuildSystem::destroyProFileReader(QtSupport::ProFileReader *reader)
{
    // The ProFileReader destructor is super expensive (but thread-safe).
    const auto deleteFuture = Utils::asyncRun(ProjectExplorerPlugin::sharedThreadPool(),
                                              [reader] { delete reader; });
    Utils::onFinished(deleteFuture, this, [this](const QFuture<void> &) {
        if (!--m_qmakeGlobalsRefCnt) {
            deregisterFromCacheManager();
            m_qmakeGlobals.reset();
        }
    });
}

void QmakeBuildSystem::deregisterFromCacheManager()
{
    QString dir = projectFilePath().path();
    if (!dir.endsWith(QLatin1Char('/')))
        dir += QLatin1Char('/');
    QtSupport::ProFileCacheManager::instance()->discardFiles(deviceRoot(), dir, qmakeVfs());
    QtSupport::ProFileCacheManager::instance()->decRefCount();
}

void QmakeBuildSystem::activeTargetWasChanged(Target *t)
{
    // We are only interested in our own target.
    if (t != target())
        return;

    m_invalidateQmakeVfsContents = true;
    scheduleUpdateAllNowOrLater();
}

static void notifyChangedHelper(const FilePath &fileName, QmakeProFile *file)
{
    if (file->filePath() == fileName) {
        QtSupport::ProFileCacheManager::instance()->discardFile(
            file->deviceRoot(), fileName.path(), file->buildSystem()->qmakeVfs());
        file->scheduleUpdate(QmakeProFile::ParseNow);
    }

    for (QmakePriFile *fn : file->children()) {
        if (auto pro = dynamic_cast<QmakeProFile *>(fn))
            notifyChangedHelper(fileName, pro);
    }
}

void QmakeBuildSystem::notifyChanged(const FilePath &name)
{
    FilePaths files = project()->files([&name](const Node *n) {
        return Project::SourceFiles(n) && n->filePath() == name;
    });

    if (files.isEmpty())
        return;

    notifyChangedHelper(name, m_rootProFile.get());
}

void QmakeBuildSystem::watchFolders(const QStringList &l, QmakePriFile *file)
{
    if (l.isEmpty())
        return;
    if (!m_centralizedFolderWatcher)
        m_centralizedFolderWatcher = new Internal::CentralizedFolderWatcher(this);
    m_centralizedFolderWatcher->watchFolders(l, file);
}

void QmakeBuildSystem::unwatchFolders(const QStringList &l, QmakePriFile *file)
{
    if (m_centralizedFolderWatcher && !l.isEmpty())
        m_centralizedFolderWatcher->unwatchFolders(l, file);
}

/////////////
/// Centralized Folder Watcher
////////////

// All the folder have a trailing slash!
CentralizedFolderWatcher::CentralizedFolderWatcher(QmakeBuildSystem *parent)
    : QObject(parent), m_buildSystem(parent)
{
    m_compressTimer.setSingleShot(true);
    m_compressTimer.setInterval(200);
    connect(&m_compressTimer, &QTimer::timeout, this, &CentralizedFolderWatcher::onTimer);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &CentralizedFolderWatcher::folderChanged);
}

QSet<QString> CentralizedFolderWatcher::recursiveDirs(const QString &folder)
{
    QSet<QString> result;
    QDir dir(folder);
    const QStringList list = dir.entryList(QDir::Dirs | QDir::NoSymLinks | QDir::NoDotAndDotDot);
    for (const QString &f : list) {
        const QString a = folder + f + QLatin1Char('/');
        result.insert(a);
        result += recursiveDirs(a);
    }
    return result;
}

void CentralizedFolderWatcher::watchFolders(const QList<QString> &folders, QmakePriFile *file)
{
    m_watcher.addPaths(folders);

    const QChar slash = QLatin1Char('/');
    for (const QString &f : folders) {
        QString folder = f;
        if (!folder.endsWith(slash))
            folder.append(slash);
        m_map.insert(folder, file);

        // Support for recursive watching
        // we add the recursive directories we find
        QSet<QString> tmp = recursiveDirs(folder);
        if (!tmp.isEmpty())
            m_watcher.addPaths(Utils::toList(tmp));
        m_recursiveWatchedFolders += tmp;
    }
}

void CentralizedFolderWatcher::unwatchFolders(const QList<QString> &folders, QmakePriFile *file)
{
    const QChar slash = QLatin1Char('/');
    for (const QString &f : folders) {
        QString folder = f;
        if (!folder.endsWith(slash))
            folder.append(slash);
        m_map.remove(folder, file);
        if (!m_map.contains(folder))
            m_watcher.removePath(folder);

        // Figure out which recursive directories we can remove
        // this might not scale. I'm pretty sure it doesn't
        // A scaling implementation would need to save more information
        // where a given directory watcher actual comes from...

        QStringList toRemove;
        for (const QString &rwf : std::as_const(m_recursiveWatchedFolders)) {
            if (rwf.startsWith(folder)) {
                // So the rwf is a subdirectory of a folder we aren't watching
                // but maybe someone else wants us to watch
                bool needToWatch = false;
                auto end = m_map.constEnd();
                for (auto it = m_map.constBegin(); it != end; ++it) {
                    if (rwf.startsWith(it.key())) {
                        needToWatch = true;
                        break;
                    }
                }
                if (!needToWatch) {
                    m_watcher.removePath(rwf);
                    toRemove << rwf;
                }
            }
        }

        for (const QString &tr : std::as_const(toRemove))
            m_recursiveWatchedFolders.remove(tr);
    }
}

void CentralizedFolderWatcher::folderChanged(const QString &folder)
{
    m_changedFolders.insert(folder);
    m_compressTimer.start();
}

void CentralizedFolderWatcher::onTimer()
{
    for (const QString &folder : std::as_const(m_changedFolders))
        delayedFolderChanged(folder);
    m_changedFolders.clear();
}

void CentralizedFolderWatcher::delayedFolderChanged(const QString &folder)
{
    // Figure out whom to inform
    QString dir = folder;
    const QChar slash = QLatin1Char('/');
    bool newOrRemovedFiles = false;
    while (true) {
        if (!dir.endsWith(slash))
            dir.append(slash);
        const QList<QmakePriFile *> files = m_map.values(dir);
        if (!files.isEmpty()) {
            // Collect all the files
            QSet<FilePath> newFiles;
            newFiles += QmakePriFile::recursiveEnumerate(folder);
            for (QmakePriFile *file : files)
                newOrRemovedFiles = newOrRemovedFiles || file->folderChanged(folder, newFiles);
        }

        // Chop off last part, and break if there's nothing to chop off
        //
        if (dir.length() < 2)
            break;

        // We start before the last slash
        const int index = dir.lastIndexOf(slash, dir.length() - 2);
        if (index == -1)
            break;
        dir.truncate(index + 1);
    }

    QString folderWithSlash = folder;
    if (!folder.endsWith(slash))
        folderWithSlash.append(slash);

    // If a subdirectory was added, watch it too
    QSet<QString> tmp = recursiveDirs(folderWithSlash);
    if (!tmp.isEmpty()) {
        QSet<QString> alreadyAdded = Utils::toSet(m_watcher.directories());
        tmp.subtract(alreadyAdded);
        if (!tmp.isEmpty())
            m_watcher.addPaths(Utils::toList(tmp));
        m_recursiveWatchedFolders += tmp;
    }

    if (newOrRemovedFiles)
        m_buildSystem->updateCodeModels();
}

void QmakeProject::configureAsExampleProject(Kit *kit)
{
    QList<BuildInfo> infoList;
    const QList<Kit *> kits(kit != nullptr ? QList<Kit *>({kit}) : KitManager::kits());
    for (Kit *k : kits) {
        if (QtSupport::QtKitAspect::qtVersion(k) != nullptr) {
            if (auto factory = BuildConfigurationFactory::find(k, projectFilePath()))
                infoList << factory->allAvailableSetups(k, projectFilePath());
        }
    }
    setup(infoList);
}

void QmakeBuildSystem::updateBuildSystemData()
{
    const QmakeProFile *const file = rootProFile();
    if (!file || file->parseInProgress())
        return;

    DeploymentData deploymentData;
    collectData(file, deploymentData);
    setDeploymentData(deploymentData);

    QList<BuildTargetInfo> appTargetList;

    project()->rootProjectNode()->forEachProjectNode([this, &appTargetList](const ProjectNode *pn) {
        auto node = dynamic_cast<const QmakeProFileNode *>(pn);
        if (!node || !node->includedInExactParse())
            return;

        if (node->projectType() != ProjectType::ApplicationTemplate
                && node->projectType() != ProjectType::ScriptTemplate)
            return;

        TargetInformation ti = node->targetInformation();
        if (!ti.valid)
            return;

        const QStringList &config = node->variableValue(Variable::Config);

        BuildTargetInfo bti;
        bti.targetFilePath = executableFor(node->proFile());
        bti.projectFilePath = node->filePath();
        bti.displayName = node->proFile()->singleVariableValue(Variable::QmakeProjectName);
        if (bti.displayName.isEmpty())
            bti.displayName = bti.projectFilePath.completeBaseName();
        const FilePath relativePathInProject
                = bti.projectFilePath.relativeChildPath(projectDirectory());
        if (!relativePathInProject.isEmpty()) {
            bti.displayNameUniquifier = QString::fromLatin1(" (%1)")
                    .arg(relativePathInProject.toUserOutput());
        }
        bti.buildKey = bti.projectFilePath.toUrlishString();
        bti.isQtcRunnable = config.contains("qtc_runnable");

        if (config.contains("console") && !config.contains("testcase")) {
            const QStringList qt = node->variableValue(Variable::Qt);
            bti.usesTerminal = !qt.contains("testlib") && !qt.contains("qmltest");
        }

        FilePaths libraryPaths;

        // The user could be linking to a library found via a -L/some/dir switch
        // to find those libraries while actually running we explicitly prepend those
        // dirs to the library search path
        const QStringList libDirectories = node->variableValue(Variable::LibDirectories);
        if (!libDirectories.isEmpty()) {
            QmakeProFile *proFile = node->proFile();
            QTC_ASSERT(proFile, return);
            const QString proDirectory = buildDir(proFile->filePath()).toUrlishString();
            for (QString dir : libDirectories) {
                // Fix up relative entries like "LIBS+=-L.."
                const QFileInfo fi(dir);
                if (!fi.isAbsolute())
                    dir = QDir::cleanPath(proDirectory + '/' + dir);
                libraryPaths.append(FilePath::fromUserInput(dir));
            }
        }
        QtSupport::QtVersion *qtVersion = QtSupport::QtKitAspect::qtVersion(kit());
        if (qtVersion)
            libraryPaths.append(qtVersion->librarySearchPath());

        bti.runEnvModifierHash = qHash(libraryPaths);
        bti.runEnvModifier = [libraryPaths](Environment &env, bool useLibrarySearchPath) {
            if (useLibrarySearchPath)
                env.prependOrSetLibrarySearchPaths(libraryPaths);
        };

        appTargetList.append(bti);
    });

    setApplicationTargets(appTargetList);
}

void QmakeBuildSystem::collectData(const QmakeProFile *file, DeploymentData &deploymentData)
{
    if (!file->isSubProjectDeployable(file->filePath()))
        return;

    const InstallsList &installsList = file->installsList();
    for (const InstallsItem &item : installsList.items) {
        if (!item.active)
            continue;
        for (const auto &localFile : item.files) {
            deploymentData.addFile(FilePath::fromString(localFile.fileName), item.path,
               item.executable ? DeployableFile::TypeExecutable : DeployableFile::TypeNormal);
        }
    }

    switch (file->projectType()) {
    case ProjectType::ApplicationTemplate:
        if (!installsList.targetPath.isEmpty())
            collectApplicationData(file, deploymentData);
        break;
    case ProjectType::SharedLibraryTemplate:
    case ProjectType::StaticLibraryTemplate:
        collectLibraryData(file, deploymentData);
        break;
    case ProjectType::SubDirsTemplate:
        for (const QmakePriFile *const subPriFile : file->subPriFilesExact()) {
            auto subProFile = dynamic_cast<const QmakeProFile *>(subPriFile);
            if (subProFile)
                collectData(subProFile, deploymentData);
        }
        break;
    default:
        break;
    }
}

void QmakeBuildSystem::collectApplicationData(const QmakeProFile *file, DeploymentData &deploymentData)
{
    const FilePath executable = executableFor(file);
    if (!executable.isEmpty())
        deploymentData.addFile(executable, file->installsList().targetPath,
                               DeployableFile::TypeExecutable);
}

FilePaths QmakeBuildSystem::allLibraryTargetFiles(const QmakeProFile *file) const
{
    const Toolchain *const toolchain = ToolchainKitAspect::cxxToolchain(kit());
    if (!toolchain)
        return {};

    FilePaths libs;
    TargetInformation ti = file->targetInformation();
    QString targetFileName = ti.target;
    const QStringList config = file->variableValue(Variable::Config);
    const bool isStatic = config.contains(QLatin1String("static"));
    const bool isPlugin = config.contains(QLatin1String("plugin"));
    const bool nameIsVersioned = !isPlugin && !config.contains("unversioned_libname");
    switch (toolchain->targetAbi().os()) {
    case Abi::WindowsOS: {
        QString targetVersionExt = file->singleVariableValue(Variable::TargetVersionExt);
        if (targetVersionExt.isEmpty()) {
            const QString version = file->singleVariableValue(Variable::Version);
            if (!version.isEmpty()) {
                targetVersionExt = version.left(version.indexOf(QLatin1Char('.')));
                if (targetVersionExt == QLatin1String("0"))
                    targetVersionExt.clear();
            }
        }
        targetFileName += targetVersionExt + QLatin1Char('.');
        targetFileName += QLatin1String(isStatic ? "lib" : "dll");
        libs << FilePath::fromString(targetFileName);
        break;
    }
    case Abi::DarwinOS: {
        FilePath destDir = ti.destDir;
        if (config.contains(QLatin1String("lib_bundle"))) {
            destDir = destDir.pathAppended(ti.target + ".framework");
        } else {
            if (!(isPlugin && config.contains(QLatin1String("no_plugin_name_prefix"))))
                targetFileName.prepend(QLatin1String("lib"));

            if (nameIsVersioned) {
                targetFileName += QLatin1Char('.');
                const QString version = file->singleVariableValue(Variable::Version);
                QString majorVersion = version.left(version.indexOf(QLatin1Char('.')));
                if (majorVersion.isEmpty())
                    majorVersion = QLatin1String("1");
                targetFileName += majorVersion;
            }
            targetFileName += QLatin1Char('.');
            targetFileName += file->singleVariableValue(isStatic ? Variable::StaticLibExtension
                                                                 : Variable::ShLibExtension);
        }
        libs << destDir / targetFileName;
        break;
    }
    case Abi::LinuxOS:
    case Abi::BsdOS:
    case Abi::QnxOS:
    case Abi::UnixOS:
        if (!(isPlugin && config.contains(QLatin1String("no_plugin_name_prefix"))))
            targetFileName.prepend(QLatin1String("lib"));

        targetFileName += QLatin1Char('.');
        if (isStatic) {
            libs << ti.destDir.pathAppended(targetFileName + QLatin1Char('a'));
        } else {
            targetFileName += QLatin1String("so");
            libs << ti.destDir / targetFileName;
            if (nameIsVersioned) {
                QString version = file->singleVariableValue(Variable::Version);
                if (version.isEmpty())
                    version = QLatin1String("1.0.0");
                QStringList versionComponents = version.split('.');
                while (versionComponents.size() < 3)
                    versionComponents << QLatin1String("0");
                targetFileName += QLatin1Char('.');
                while (!versionComponents.isEmpty()) {
                    const QString versionString = versionComponents.join(QLatin1Char('.'));
                    libs << ti.destDir.pathAppended(targetFileName + versionString);
                    versionComponents.removeLast();
                }
            }
        }
        break;
    default:
        break;
    }

    return libs;
}

void QmakeBuildSystem::collectLibraryData(const QmakeProFile *file, DeploymentData &deploymentData)
{
    const QString targetPath = file->installsList().targetPath;
    if (!targetPath.isEmpty()) {
        const FilePaths libs = allLibraryTargetFiles(file);
        for (const FilePath &lib : libs)
            deploymentData.addFile(lib, targetPath);
    }
}

static FilePath getFullPathOf(const QmakeProFile *pro, Variable variable,
                              const BuildConfiguration *bc)
{
    // Take last non-flag value, to cover e.g. '@echo $< && $$QMAKE_CC' or 'ccache gcc'
    const QStringList values = Utils::filtered(pro->variableValue(variable),
                                               [](const QString &value) {
        return !value.startsWith('-');
    });
    if (values.isEmpty())
        return {};
    const QString exe = values.last();
    QTC_ASSERT(bc, return FilePath::fromUserInput(exe));
    QFileInfo fi(exe);
    if (fi.isAbsolute())
        return FilePath::fromUserInput(exe);

    return bc->environment().searchInPath(exe);
}

void QmakeBuildSystem::testToolChain(Toolchain *tc, const FilePath &path) const
{
    if (!tc || path.isEmpty())
        return;

    const FilePath expected = tc->compilerCommand();
    if (tc->matchesCompilerCommand(expected))
        return;
    const QPair<FilePath, FilePath> pair{expected, path};
    if (m_toolChainWarnings.contains(pair))
        return;
    // Suppress warnings on Apple machines where compilers in /usr/bin point into Xcode.
    // This will suppress some valid warnings, but avoids annoying Apple users with
    // spurious warnings all the time!
    if (pair.first.path().startsWith("/usr/bin/")
            && pair.second.path().contains("/Contents/Developer/Toolchains/")) {
        return;
    }
    TaskHub::addTask<BuildSystemTask>(
        Task::Warning,
        Tr::tr(
            "\"%1\" is used by qmake, but \"%2\" is configured in the kit.\n"
            "Please update your kit (%3) or choose a mkspec for qmake that matches "
            "your target environment better.")
            .arg(path.toUserOutput())
            .arg(expected.toUserOutput())
            .arg(kit()->displayName()));
    m_toolChainWarnings.insert(pair);
}

QString QmakeBuildSystem::deviceRoot() const
{
    IDeviceConstPtr device = BuildDeviceKitAspect::device(kit());
    QTC_ASSERT(device, return {});
    FilePath deviceRoot = device->rootPath();
    if (!deviceRoot.isLocal())
        return deviceRoot.toFSPathString();

    return {};
}

void QmakeBuildSystem::warnOnToolChainMismatch(const QmakeProFile *pro) const
{
    const BuildConfiguration *bc = buildConfiguration();
    testToolChain(ToolchainKitAspect::cToolchain(kit()), getFullPathOf(pro, Variable::QmakeCc, bc));
    testToolChain(ToolchainKitAspect::cxxToolchain(kit()),
                  getFullPathOf(pro, Variable::QmakeCxx, bc));
}

FilePath QmakeBuildSystem::executableFor(const QmakeProFile *file)
{
    const Toolchain *const tc = ToolchainKitAspect::cxxToolchain(kit());
    if (!tc)
        return {};

    TargetInformation ti = file->targetInformation();
    QString target;

    QTC_ASSERT(file, return {});

    if (tc->targetAbi().os() == Abi::DarwinOS
            && file->variableValue(Variable::Config).contains("app_bundle")) {
        target = ti.target + ".app/Contents/MacOS/" + ti.target;
    } else {
        const QString extension = file->singleVariableValue(Variable::TargetExt);
        if (extension.isEmpty())
            target = OsSpecificAspects::withExecutableSuffix(Abi::abiOsToOsType(tc->targetAbi().os()), ti.target);
        else
            target = ti.target + extension;
    }
    return ti.destDir / target;
}

ProjectImporter *QmakeProject::projectImporter() const
{
    if (!m_projectImporter)
        m_projectImporter = new QmakeProjectImporter(projectFilePath());
    return m_projectImporter;
}

QmakeBuildSystem::AsyncUpdateState QmakeBuildSystem::asyncUpdateState() const
{
    return m_asyncUpdateState;
}

QmakeProFile *QmakeBuildSystem::rootProFile() const
{
    return m_rootProFile.get();
}

void QmakeBuildSystem::triggerParsing()
{
    asyncUpdate();
}

FilePaths QmakeBuildSystem::filesGeneratedFrom(const FilePath &input) const
{
    if (!project()->rootProjectNode())
        return {};

    if (const FileNode *file = fileNodeOf(project()->rootProjectNode(), input)) {
        const QmakeProFileNode *pro = dynamic_cast<QmakeProFileNode *>(file->parentFolderNode());
        QTC_ASSERT(pro, return {});
        if (const QmakeProFile *proFile = pro->proFile())
            return proFile->generatedFiles(buildDir(pro->filePath()),
                                           file->filePath(), file->fileType());
    }
    return {};
}

QVariant QmakeBuildSystem::additionalData(Id id) const
{
    if (id == "QmlDesignerImportPath")
        return m_rootProFile->variableValue(Variable::QmlDesignerImportPath);
    return BuildSystem::additionalData(id);
}

static const Id xcodeGeneratorId() { return "QMAKE_GENERATOR_XCODE"; }
static const Id vsGeneratorId() { return "QMAKE_GENERATOR_VS"; }

QList<QPair<Id, QString>> QmakeBuildSystem::generators() const
{
    if (HostOsInfo::isMacHost())
        return {{xcodeGeneratorId(), Tr::tr("Xcode Project (via qmake)")}};
    if (HostOsInfo::isWindowsHost())
        return {{vsGeneratorId(), Tr::tr("Visual Studio Project (via qmake)")}};
    return {};
}

void QmakeBuildSystem::runGenerator(Utils::Id id)
{
    TaskHub::clearAndRemoveTask(m_generatorError);

    QTC_ASSERT(buildConfiguration(), return);
    const auto showError = [this](const QString &detail) {
        m_generatorError
            = OtherTask(Task::DisruptingError, Tr::tr("qmake generator failed.").append('\n').append(detail));
        TaskHub::addTask(m_generatorError);
    };
    const QtVersion * const qtVersion = QtKitAspect::qtVersion(kit());
    if (!qtVersion) {
        showError(Tr::tr("No Qt in kit"));
        return;
    }
    const FilePath qmake = qtVersion->qmakeFilePath();
    if (!qmake.isExecutableFile()) {
        showError(Tr::tr("No valid qmake executable"));
        return;
    }
    const QMakeStep * const step = buildConfiguration()->buildSteps()->firstOfType<QMakeStep>();
    if (!step) {
        showError(Tr::tr("No qmake step in active build configuration"));
        return;
    }
    FilePath outDir = buildConfiguration()->buildDirectory();
    CommandLine cmdLine(qmake, {"-r"});
    cmdLine.addArgs(step->allArguments(qtVersion), CommandLine::Raw);
    if (id == xcodeGeneratorId()) {
        QStringList args = cmdLine.splitArguments();
        for (auto it = args.begin(); it != args.end(); ++it) {
            if (*it == "-spec") {
                it = args.erase(it);
                if (it != args.end())
                    args.erase(it);
                break;
            }
        }
        args << "-spec" << "macx-xcode";
        cmdLine.setArguments({});
        cmdLine.addArgs(args);
        outDir = outDir / "qtcgen_xcode";
    } else if (id == vsGeneratorId()) {
        cmdLine.addArgs({"-tp", "vc"});
        outDir = outDir / "qtcgen_vs";
    } else {
        QTC_ASSERT(false, return);
    }
    if (!outDir.ensureWritableDir()) {
        showError(Tr::tr("Cannot create output directory \"%1\".").arg(outDir.toUserOutput()));
        return;
    }
    const auto proc = new Process(this);
    connect(proc, &Process::done, proc, &Process::deleteLater);
    connect(proc, &Process::readyReadStandardOutput, this, [proc] {
        Core::MessageManager::writeFlashing(proc->readAllStandardOutput());
    });
    connect(proc, &Process::readyReadStandardError, this, [proc] {
        Core::MessageManager::writeDisrupting(proc->readAllStandardError());
    });
    proc->setWorkingDirectory(outDir);
    proc->setEnvironment(buildConfiguration()->environment());
    proc->setCommand(cmdLine);
    Core::MessageManager::writeFlashing(
        Tr::tr("Running in \"%1\": %2.").arg(outDir.toUserOutput(), cmdLine.toUserOutput()));
    proc->start();
}

void QmakeBuildSystem::buildHelper(Action action, bool isFileBuild, QmakeProFileNode *profile,
                                   FileNode *buildableFile)
{
    auto bc = qmakeBuildConfiguration();

    if (!profile || !buildableFile)
        isFileBuild = false;

    if (profile) {
        if (profile != project()->rootProjectNode() || isFileBuild)
            bc->setSubNodeBuild(profile->proFileNode());
    }

    if (isFileBuild)
        bc->setFileNodeBuild(buildableFile);
    if (ProjectExplorerPlugin::saveModifiedFiles()) {
        if (action == BUILD)
            BuildManager::buildList(bc->buildSteps());
        else if (action == CLEAN)
            BuildManager::buildList(bc->cleanSteps());
        else if (action == REBUILD)
            BuildManager::buildLists({bc->cleanSteps(), bc->buildSteps()});
    }

    bc->setSubNodeBuild(nullptr);
    bc->setFileNodeBuild(nullptr);
}

} // QmakeProjectManager

#include "qmakeproject.moc"
