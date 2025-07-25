// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "gitclient.h"

#include "branchadddialog.h"
#include "commitdata.h"
#include "gitconstants.h"
#include "giteditor.h"
#include "gitplugin.h"
#include "gittr.h"
#include "gitutils.h"
#include "mergetool.h"
#include "temporarypatchfile.h"

#include <coreplugin/coreconstants.h>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/generatedfile.h>
#include <coreplugin/icore.h>
#include <coreplugin/idocument.h>
#include <coreplugin/iversioncontrol.h>
#include <coreplugin/vcsmanager.h>

#include <diffeditor/diffeditorconstants.h>

#include <solutions/tasking/conditional.h>

#include <texteditor/fontsettings.h>
#include <texteditor/texteditorsettings.h>

#include <utils/ansiescapecodehandler.h>
#include <utils/async.h>
#include <utils/algorithm.h>
#include <utils/checkablemessagebox.h>
#include <utils/commandline.h>
#include <utils/environment.h>
#include <utils/fileutils.h>
#include <utils/hostosinfo.h>
#include <utils/mimeutils.h>
#include <utils/qtcprocess.h>
#include <utils/qtcassert.h>
#include <utils/shutdownguard.h>
#include <utils/stringutils.h>
#include <utils/temporaryfile.h>
#include <utils/theme/theme.h>

#include <vcsbase/commonvcssettings.h>
#include <vcsbase/submitfilemodel.h>
#include <vcsbase/vcsbasediffeditorcontroller.h>
#include <vcsbase/vcsbaseeditor.h>
#include <vcsbase/vcsbaseeditorconfig.h>
#include <vcsbase/vcsbaseplugin.h>
#include <vcsbase/vcsbasesubmiteditor.h>
#include <vcsbase/vcscommand.h>
#include <vcsbase/vcsoutputwindow.h>

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>

const char HEAD[] = "HEAD";
const char CHERRY_PICK_HEAD[] = "CHERRY_PICK_HEAD";
const char BRANCHES_PREFIX[] = "Branches: ";
const char stashNamePrefix[] = "stash@{";
const char noColorOption[] = "--no-color";
const char colorOption[] = "--color=always";
const char patchOption[] = "--patch";
const char graphOption[] = "--graph";
const char decorateOption[] = "--decorate";
const char allBranchesOption[] = "--all";

using namespace Core;
using namespace DiffEditor;
using namespace Tasking;
using namespace Utils;
using namespace VcsBase;

namespace Git::Internal {

static QString branchesDisplay(const QString &prefix, QStringList *branches, bool *first)
{
    const int limit = 12;
    const int count = branches->count();
    int more = 0;
    QString output;
    if (*first)
        *first = false;
    else
        output += QString(sizeof(BRANCHES_PREFIX) - 1 /* the \0 */, ' '); // Align
    output += prefix + ": ";
    // If there are more than 'limit' branches, list limit/2 (first limit/4 and last limit/4)
    if (count > limit) {
        const int leave = limit / 2;
        more = count - leave;
        branches->erase(branches->begin() + leave / 2 + 1, branches->begin() + count - leave / 2);
        (*branches)[leave / 2] = "...";
    }
    output += branches->join(", ");
    //: Displayed after the untranslated message "Branches: branch1, branch2 'and %n more'"
    //  in git show.
    if (more > 0)
        output += ' ' + Tr::tr("and %n more", nullptr, more);
    return output;
}

///////////////////////////////

static void stage(DiffEditorController *diffController, const QString &patch, bool revert)
{
    if (patch.isEmpty())
        return;
    TemporaryPatchFile patchFile(patch);
    const FilePath baseDir = diffController->workingDirectory();
    QStringList args = {"--cached"};
    if (revert)
        args << "--reverse";
    QString errorMessage;
    if (gitClient().synchronousApplyPatch(baseDir, patchFile.filePath().toUrlishString(),
                                                     &errorMessage, args)) {
        if (errorMessage.isEmpty()) {
            if (revert)
                VcsOutputWindow::appendSilently(baseDir, Tr::tr("Chunk successfully unstaged"));
            else
                VcsOutputWindow::appendSilently(baseDir, Tr::tr("Chunk successfully staged"));
        } else {
            VcsOutputWindow::appendError(baseDir, errorMessage);
        }
        diffController->requestReload();
    } else {
        VcsOutputWindow::appendError(baseDir, errorMessage);
    }
}

///////////////////////////////

static FilePaths submoduleDataToAbsolutePath(const SubmoduleDataMap &submodules,
                                                   const FilePath &rootDir)
{
    return Utils::transform<FilePaths>(submodules, [&rootDir](const SubmoduleData &data)
                                             { return rootDir.pathAppended(data.dir); });
}

class GitBaseDiffEditorController : public VcsBaseDiffEditorController
{
    Q_OBJECT

protected:
    explicit GitBaseDiffEditorController(IDocument *document);

    QStringList addConfigurationArguments(const QStringList &args) const;

private:
    void addExtraActions(QMenu *menu, int fileIndex, int chunkIndex,
                         const ChunkSelection &selection) final
    {
        menu->addSeparator();

        auto stageChunk = [this, fileIndex, chunkIndex](DiffEditorController::PatchOptions options,
                                                        const DiffEditor::ChunkSelection &selection) {
            options |= DiffEditorController::AddPrefix;
            const QString patch = makePatch(fileIndex, chunkIndex, selection, options);
            stage(this, patch, options & Revert);
        };

        QAction *stageChunkAction = menu->addAction(Tr::tr("Stage Chunk"));
        connect(stageChunkAction, &QAction::triggered, this, [stageChunk] {
            stageChunk(DiffEditorController::NoOption, {});
        });
        QAction *stageLinesAction = menu->addAction(Tr::tr("Stage Selection (%n Lines)", "",
                                                           selection.selectedRowsCount()));
        connect(stageLinesAction, &QAction::triggered,  this, [stageChunk, selection] {
            stageChunk(DiffEditorController::NoOption, selection);
        });
        QAction *unstageChunkAction = menu->addAction(Tr::tr("Unstage Chunk"));
        connect(unstageChunkAction, &QAction::triggered, this, [stageChunk] {
            stageChunk(DiffEditorController::Revert, {});
        });
        QAction *unstageLinesAction = menu->addAction(Tr::tr("Unstage Selection (%n Lines)", "",
                                                             selection.selectedRowsCount()));
        connect(unstageLinesAction, &QAction::triggered, this, [stageChunk, selection] {
            stageChunk(DiffEditorController::Revert, selection);
        });

        if (selection.isNull()) {
            stageLinesAction->setVisible(false);
            unstageLinesAction->setVisible(false);
        }
        if (!chunkExists(fileIndex, chunkIndex)) {
            stageChunkAction->setEnabled(false);
            stageLinesAction->setEnabled(false);
            unstageChunkAction->setEnabled(false);
            unstageLinesAction->setEnabled(false);
        }
    }
};

class GitDiffEditorController : public GitBaseDiffEditorController
{
public:
    explicit GitDiffEditorController(IDocument *document,
                                     const QString &leftCommit,
                                     const QString &rightCommit,
                                     const QStringList &extraArgs);
private:
    QStringList diffArgs(const QString &leftCommit, const QString &rightCommit,
                         const QStringList &extraArgs) const
    {
        QStringList res = {"diff"};
        if (!leftCommit.isEmpty())
            res << leftCommit;

        // This is workaround for lack of support for merge commits and resolving conflicts,
        // we compare the current state of working tree to the HEAD of current branch
        // instead of showing unsupported combined diff format.
        auto fixRightCommit = [this](const QString &commit) {
            if (!commit.isEmpty())
                return commit;
            if (gitClient().checkCommandInProgress(workingDirectory()) == GitClient::NoCommand)
                return QString();
            return QString(HEAD);
        };
        const QString fixedRightCommit = fixRightCommit(rightCommit);
        if (!fixedRightCommit.isEmpty())
            res << fixedRightCommit;

        res << extraArgs;
        return res;
    }
};

GitDiffEditorController::GitDiffEditorController(IDocument *document,
                                                 const QString &leftCommit,
                                                 const QString &rightCommit,
                                                 const QStringList &extraArgs)
    : GitBaseDiffEditorController(document)
{
    const Storage<QString> diffInputStorage;

    const auto onDiffSetup = [this, leftCommit, rightCommit, extraArgs](Process &process) {
        process.setEncoding(VcsBaseEditor::getEncoding(workingDirectory(), {}));
        setupCommand(process, {addConfigurationArguments(diffArgs(leftCommit, rightCommit, extraArgs))});
        VcsOutputWindow::appendCommand(process.workingDirectory(), process.commandLine());
    };
    const auto onDiffDone = [diffInputStorage](const Process &process) {
        *diffInputStorage = process.cleanedStdOut();
    };

    const Group root {
        diffInputStorage,
        ProcessTask(onDiffSetup, onDiffDone, CallDone::OnSuccess),
        postProcessTask(diffInputStorage)
    };
    setReloadRecipe(root);
}

GitBaseDiffEditorController::GitBaseDiffEditorController(IDocument *document)
    : VcsBaseDiffEditorController(document)
{
    setDisplayName("Git Diff");
}

///////////////////////////////

QStringList GitBaseDiffEditorController::addConfigurationArguments(const QStringList &args) const
{
    QTC_ASSERT(!args.isEmpty(), return args);

    QStringList realArgs = {
        "-c",
        "diff.color=false",
        args.at(0),
        "-m", // show diff against parents instead of merge commits
        "-M", "-C", // Detect renames and copies
        "--first-parent" // show only first parent
    };
    if (ignoreWhitespace())
        realArgs << "--ignore-space-change";
    realArgs << "--unified=" + QString::number(contextLineCount())
             << "--src-prefix=a/" << "--dst-prefix=b/" << args.mid(1);

    return realArgs;
}

class FileListDiffController : public GitBaseDiffEditorController
{
public:
    FileListDiffController(IDocument *document, const QStringList &stagedFiles,
                           const QStringList &unstagedFiles);
};

FileListDiffController::FileListDiffController(IDocument *document, const QStringList &stagedFiles,
                                               const QStringList &unstagedFiles)
        : GitBaseDiffEditorController(document)
{
    struct DiffStorage {
        QString m_stagedOutput;
        QString m_unstagedOutput;
    };

    const Storage<DiffStorage> storage;
    const Storage<QString> diffInputStorage;

    const auto onStagedSetup = [this, stagedFiles](Process &process) {
        if (stagedFiles.isEmpty())
            return SetupResult::StopWithError;
        process.setEncoding(VcsBaseEditor::getEncoding(workingDirectory(), stagedFiles));
        setupCommand(process, addConfigurationArguments(
                              QStringList({"diff", "--cached", "--"}) + stagedFiles));
        VcsOutputWindow::appendCommand(process.workingDirectory(), process.commandLine());
        return SetupResult::Continue;
    };
    const auto onStagedDone = [storage](const Process &process) {
        storage->m_stagedOutput = process.cleanedStdOut();
    };

    const auto onUnstagedSetup = [this, unstagedFiles](Process &process) {
        if (unstagedFiles.isEmpty())
            return SetupResult::StopWithError;
        process.setEncoding(VcsBaseEditor::getEncoding(workingDirectory(), unstagedFiles));
        setupCommand(process, addConfigurationArguments(
                              QStringList({"diff", "--"}) + unstagedFiles));
        VcsOutputWindow::appendCommand(process.workingDirectory(), process.commandLine());
        return SetupResult::Continue;
    };
    const auto onUnstagedDone = [storage](const Process &process) {
        storage->m_unstagedOutput = process.cleanedStdOut();
    };

    const auto onDone = [storage, diffInputStorage] {
        *diffInputStorage = storage->m_stagedOutput + storage->m_unstagedOutput;
    };

    const Group root {
        storage,
        diffInputStorage,
        Group {
            parallel,
            continueOnSuccess,
            ProcessTask(onStagedSetup, onStagedDone, CallDone::OnSuccess),
            ProcessTask(onUnstagedSetup, onUnstagedDone, CallDone::OnSuccess),
            onGroupDone(onDone, CallDone::OnSuccess)
        },
        postProcessTask(diffInputStorage)
    };
    setReloadRecipe(root);
}

class ShowController : public GitBaseDiffEditorController
{
public:
    ShowController(IDocument *document, const QString &id);
};

ShowController::ShowController(IDocument *document, const QString &id)
    : GitBaseDiffEditorController(document)
{
    setDisplayName("Git Show");
    setAnsiEnabled(true);
    static const QString busyMessage = Tr::tr("<resolving>");
    const QColor color = QColor::fromString(GitClient::styleColorName(TextEditor::C_LOG_DECORATION));
    const QString decorateColor = AnsiEscapeCodeHandler::ansiFromColor(color);
    const QString noColor = AnsiEscapeCodeHandler::noColor();

    struct ReloadStorage {
        bool m_postProcessDescription = false;
        QString m_commit;

        QString m_header;
        QString m_body;
        QString m_branches;
        QString m_precedes;
        QStringList m_follows;
    };

    const Storage<ReloadStorage> storage;

    const auto updateDescription = [this, decorateColor, noColor](const ReloadStorage &storage) {
        QString desc = storage.m_header;
        if (!storage.m_branches.isEmpty())
            desc.append(BRANCHES_PREFIX + storage.m_branches + '\n');
        if (!storage.m_precedes.isEmpty())
            desc.append("Precedes: " + decorateColor + storage.m_precedes + noColor + '\n');
        QStringList follows;
        for (const QString &str : storage.m_follows) {
            if (!str.isEmpty())
                follows.append(str);
        }
        if (!follows.isEmpty())
            desc.append("Follows: " + decorateColor + follows.join(", ") + noColor + '\n');
        desc.append('\n' + storage.m_body);
        setDescription(desc);
    };

    const auto onDescriptionSetup = [this, id](Process &process) {
        process.setEncoding(gitClient().encoding(GitClient::EncodingCommit, workingDirectory()));
        const ColorNames colors = GitClient::colorNames();

        const QString showFormat = QStringLiteral(
                                    "--pretty=format:"
                                    "commit %C(%1)%H%Creset %C(%2)%d%Creset%n"
                                    "Author: %C(%3)%aN <%aE>%Creset, %C(%4)%ad (%ar)%Creset%n"
                                    "Committer: %C(%3)%cN <%cE>%Creset, %C(%4)%cd (%cr)%Creset%n"
                                    "%n%C(%5)%s%Creset%n%n%b"
                                    ).arg(colors.hash, colors.decoration, colors.author,
                                          colors.date, colors.subject);
        setupCommand(process, {"show", "-s", colorOption, showFormat, id});
        VcsOutputWindow::appendCommand(process.workingDirectory(), process.commandLine());
        setDescription(Tr::tr("Waiting for data..."));
    };
    const auto onDescriptionDone = [this, storage, updateDescription](const Process &process) {
        ReloadStorage *data = storage.activeStorage();
        const QString output = process.cleanedStdOut();
        data->m_postProcessDescription = output.startsWith("commit ");
        if (!data->m_postProcessDescription) {
            setDescription(output);
            return;
        }
        const int lastHeaderLine = output.indexOf("\n\n") + 1;
        const int commitPos = output.indexOf('m', 8) + 1;
        data->m_commit = output.mid(commitPos, 12);
        data->m_header = output.left(lastHeaderLine);
        data->m_body = output.mid(lastHeaderLine + 1);
        updateDescription(*data);
    };

    const auto desciptionDetailsSetup = [storage] {
        if (!storage->m_postProcessDescription)
            return SetupResult::StopWithSuccess;
        return SetupResult::Continue;
    };

    const auto onBranchesSetup = [this, storage](Process &process) {
        storage->m_branches = busyMessage;
        const QString branchesFormat = QStringLiteral(
                                           "--format="
                                           "%(if:equals=refs/remotes)%(refname:rstrip=-2)%(then)"
                                             "%(refname:lstrip=1)"
                                           "%(else)"
                                             "%(refname:lstrip=2)"
                                           "%(end)"
            );
        setupCommand(process, {"branch", noColorOption, "-a", branchesFormat,
                               "--contains", storage->m_commit});
        VcsOutputWindow::appendCommand(process.workingDirectory(), process.commandLine());
    };
    const auto onBranchesDone = [storage, updateDescription, decorateColor, noColor](
                                    const Process &process, DoneWith result) {
        ReloadStorage *data = storage.activeStorage();
        data->m_branches.clear();
        if (result == DoneWith::Success) {
            const QString remotePrefix = "remotes/";
            const QString localPrefix = "<Local>";
            const int prefixLength = remotePrefix.length();
            QStringList branches;
            QString previousRemote = localPrefix;
            bool first = true;
            const QStringList branchList = process.cleanedStdOut().split('\n');
            for (const QString &branch : branchList) {
                if (branch.isEmpty())
                    continue;
                if (branch.startsWith(remotePrefix)) {
                    const int nextSlash = branch.indexOf('/', prefixLength);
                    if (nextSlash < 0)
                        continue;
                    const QString remote = branch.mid(prefixLength, nextSlash - prefixLength);
                    if (remote != previousRemote) {
                        data->m_branches += decorateColor + branchesDisplay(previousRemote, &branches, &first)
                                            + noColor + '\n';
                        branches.clear();
                        previousRemote = remote;
                    }
                    branches << branch.mid(nextSlash + 1);
                } else {
                    branches << branch;
                }
            }
            if (branches.isEmpty()) {
                if (previousRemote == localPrefix)
                    data->m_branches += decorateColor + Tr::tr("<None>") + noColor;
            } else {
                data->m_branches += decorateColor + branchesDisplay(previousRemote, &branches, &first)
                                    + noColor;
            }
            data->m_branches = data->m_branches.trimmed();
        }
        updateDescription(*data);
    };

    const auto onPrecedesSetup = [this, storage](Process &process) {
        storage->m_precedes = busyMessage;
        setupCommand(process, {"describe", "--contains", storage->m_commit});
    };
    const auto onPrecedesDone = [storage, updateDescription](const Process &process, DoneWith result) {
        ReloadStorage *data = storage.activeStorage();
        data->m_precedes.clear();
        if (result == DoneWith::Success) {
            data->m_precedes = process.cleanedStdOut().trimmed();
            const int tilde = data->m_precedes.indexOf('~');
            if (tilde != -1)
                data->m_precedes.truncate(tilde);
            if (data->m_precedes.endsWith("^0"))
                data->m_precedes.chop(2);
        }
        updateDescription(*data);
    };

    const auto onFollowsSetup = [this, storage, updateDescription](TaskTree &taskTree) {
        ReloadStorage *data = storage.activeStorage();
        QStringList parents;
        QString errorMessage;
        // TODO: it's trivial now to call below asynchonously, too
        gitClient().synchronousParentRevisions(workingDirectory(), data->m_commit,
                                               &parents, &errorMessage);
        data->m_follows = {busyMessage};
        data->m_follows.resize(parents.size());

        const LoopList iterator(parents);
        const auto onFollowSetup = [this, iterator](Process &process) {
            setupCommand(process, {"describe", "--tags", "--abbrev=0", *iterator});
        };
        const auto onFollowDone = [data, updateDescription, iterator](const Process &process) {
            data->m_follows[iterator.iteration()] = process.cleanedStdOut().trimmed();
            updateDescription(*data);
        };

        const auto onDone = [data, updateDescription] {
            data->m_follows.clear();
            updateDescription(*data);
        };

        const Group recipe = For (iterator) >> Do {
            parallel,
            continueOnSuccess,
            ProcessTask(onFollowSetup, onFollowDone, CallDone::OnSuccess),
            onGroupDone(onDone, CallDone::OnError)
        };
        taskTree.setRecipe(recipe);
    };

    const auto onDiffSetup = [this, id](Process &process) {
        setupCommand(process, addConfigurationArguments(
                                  {"show", "--format=format:", // omit header, already generated
                                   noColorOption, decorateOption, id}));
        VcsOutputWindow::appendCommand(process.workingDirectory(), process.commandLine());
    };
    const Storage<QString> diffInputStorage;
    const auto onDiffDone = [diffInputStorage](const Process &process) {
        *diffInputStorage = process.cleanedStdOut();
    };

    const Group root {
        storage,
        parallel,
        continueOnError,
        onGroupSetup([this] { setStartupFile(VcsBase::source(this->document()).toUrlishString()); }),
        Group {
            finishAllAndSuccess,
            ProcessTask(onDescriptionSetup, onDescriptionDone, CallDone::OnSuccess),
            Group {
                parallel,
                finishAllAndSuccess,
                onGroupSetup(desciptionDetailsSetup),
                ProcessTask(onBranchesSetup, onBranchesDone),
                ProcessTask(onPrecedesSetup, onPrecedesDone),
                TaskTreeTask(onFollowsSetup)
            }
        },
        Group {
            diffInputStorage,
            ProcessTask(onDiffSetup, onDiffDone, CallDone::OnSuccess),
            postProcessTask(diffInputStorage)
        }
    };
    setReloadRecipe(root);
}

///////////////////////////////

class GitBlameConfig : public VcsBaseEditorConfig
{
public:
    explicit GitBlameConfig(QToolBar *toolBar)
        : VcsBaseEditorConfig(toolBar)
    {
        mapSetting(addToggleButton(QString(), Tr::tr("Omit Date"),
                                   Tr::tr("Hide the date of a change from the output.")),
                   &settings().omitAnnotationDate);
        mapSetting(addToggleButton("-w", Tr::tr("Ignore Whitespace"),
                                   Tr::tr("Ignore whitespace only changes.")),
                   &settings().ignoreSpaceChangesInBlame);

        const QList<ChoiceItem> logChoices = {
            ChoiceItem(Tr::tr("No Move Detection"), ""),
            ChoiceItem(Tr::tr("Detect Moves Within File"), "-M"),
            ChoiceItem(Tr::tr("Detect Moves Between Files"), "-M -C"),
            ChoiceItem(Tr::tr("Detect Moves and Copies Between Files"), "-M -C -C")
        };
        mapSetting(addChoices(Tr::tr("Move detection"), {}, logChoices),
                   &settings().blameMoveDetection);

        addReloadButton();
    }
};

class GitBaseConfig : public VcsBaseEditorConfig
{
public:
    GitBaseConfig(GitEditorWidget *editor)
        : VcsBaseEditorConfig(editor->toolBar())
    {
        QAction *patienceAction = addToggleButton("--patience", Tr::tr("Patience"),
            Tr::tr("Use the patience algorithm for calculating the differences."));
        mapSetting(patienceAction, &settings().diffPatience);
        QAction *ignoreWSAction = addToggleButton("--ignore-space-change", Tr::tr("Ignore Whitespace"),
                                           Tr::tr("Ignore whitespace only changes."));
        mapSetting(ignoreWSAction, &settings().ignoreSpaceChangesInDiff);

        QToolBar *toolBar = editor->toolBar();
        QAction *diffButton = addToggleButton(patchOption, Tr::tr("Diff"),
                                              Tr::tr("Show difference."));
        mapSetting(diffButton, &settings().logDiff);
        connect(diffButton, &QAction::toggled, patienceAction, &QAction::setVisible);
        connect(diffButton, &QAction::toggled, ignoreWSAction, &QAction::setVisible);
        patienceAction->setVisible(diffButton->isChecked());
        ignoreWSAction->setVisible(diffButton->isChecked());
        auto filterAction = new QAction(Tr::tr("Filter"), toolBar);
        filterAction->setToolTip(Tr::tr("Filter commits by message or content."));
        filterAction->setCheckable(true);
        connect(filterAction, &QAction::toggled, editor, &GitEditorWidget::toggleFilters);
        toolBar->addAction(filterAction);
    }
};

class GitLogConfig : public GitBaseConfig
{
public:
    GitLogConfig(bool fileRelated, GitEditorWidget *editor)
        : GitBaseConfig(editor)
    {
        QAction *allBranchesButton = addToggleButton(
            QStringList{allBranchesOption},
            Tr::tr("All", "All branches"),
            Tr::tr("Show log for all local branches."));
        mapSetting(allBranchesButton, &settings().allBranches);
        QAction *firstParentButton =
                addToggleButton({"-m", "--first-parent"},
                                Tr::tr("First Parent"),
                                Tr::tr("Follow only the first parent on merge commits."));
        mapSetting(firstParentButton, &settings().firstParent);
        QAction *graphButton = addToggleButton(graphArguments(), Tr::tr("Graph"),
                                               Tr::tr("Show textual graph log."));
        mapSetting(graphButton, &settings().graphLog);

        QAction *colorButton = addToggleButton(QStringList{colorOption},
                                        Tr::tr("Color"), Tr::tr("Use colors in log."));
        mapSetting(colorButton, &settings().colorLog);

        if (fileRelated) {
            QAction *followButton = addToggleButton(
                        "--follow", Tr::tr("Follow"),
                        Tr::tr("Show log also for previous names of the file."));
            mapSetting(followButton, &settings().followRenames);
        }

        addReloadButton();
    }

    QStringList graphArguments() const
    {
        const ColorNames colors = GitClient::colorNames();
        const QString formatArg = QStringLiteral(
                    "--pretty=format:"
                    "%C(%1)%h%Creset "
                    "%C(%2)%d%Creset "
                    "%C(%3)%aN%Creset "
                    "%C(%4)%s%Creset "
                    "%C(%5)%ci%Creset"
                    ).arg(colors.hash, colors.decoration, colors.author, colors.subject, colors.date);
        return {graphOption, "--oneline", "--topo-order", formatArg};
    }
};

class GitRefLogConfig : public GitBaseConfig
{
public:
    explicit GitRefLogConfig(GitEditorWidget *editor)
        : GitBaseConfig(editor)
    {
        QAction *showDateButton =
                addToggleButton("--date=iso",
                                Tr::tr("Show Date"),
                                Tr::tr("Show date instead of sequence."));
        mapSetting(showDateButton, &settings().refLogShowDate);

        addReloadButton();
    }
};

static void handleConflictResponse(const VcsBase::CommandResult &result,
                                   const FilePath &workingDirectory,
                                   const QString &abortCommand = {})
{
    const bool success = result.result() == ProcessResult::FinishedWithSuccess;
    const QString stdOutData = success ? QString() : result.cleanedStdOut();
    const QString stdErrData = success ? QString() : result.cleanedStdErr();
    static const QRegularExpression patchFailedRE("Patch failed at ([^\\n]*)");
    static const QRegularExpression conflictedFilesRE("Merge conflict in ([^\\n]*)");
    static const QRegularExpression couldNotApplyRE("[Cc]ould not (?:apply|revert) ([^\\n]*)");
    QString commit;
    QStringList files;

    const QRegularExpressionMatch outMatch = patchFailedRE.match(stdOutData);
    if (outMatch.hasMatch())
        commit = outMatch.captured(1);
    QRegularExpressionMatchIterator it = conflictedFilesRE.globalMatch(stdOutData);
    while (it.hasNext())
        files.append(it.next().captured(1));
    const QRegularExpressionMatch errMatch = couldNotApplyRE.match(stdErrData);
    if (errMatch.hasMatch())
        commit = errMatch.captured(1);

    if (commit.isEmpty() && files.isEmpty()) {
        if (gitClient().checkCommandInProgress(workingDirectory) == GitClient::NoCommand)
            gitClient().endStashScope(workingDirectory);
    } else {
        gitClient().handleMergeConflicts(workingDirectory, commit, files, abortCommand);
    }
}

class GitProgressParser
{
public:
    void operator()(QFutureInterface<void> &fi, const QString &inputText) const {
        const QRegularExpressionMatch match = m_progressExp.match(inputText);
        if (match.hasMatch()) {
            fi.setProgressRange(0, match.captured(2).toInt());
            fi.setProgressValue(match.captured(1).toInt());
        }
    }

private:
    const QRegularExpression m_progressExp{"\\((\\d+)/(\\d+)\\)"}; // e.g. Rebasing (7/42)
};

static inline QString msgRepositoryNotFound(const FilePath &dir)
{
    return Tr::tr("Cannot determine the repository for \"%1\".").arg(dir.toUserOutput());
}

static inline QString msgParseFilesFailed()
{
    return  Tr::tr("Cannot parse the file output.");
}

static QString msgCannotLaunch(const FilePath &binary)
{
    return Tr::tr("Cannot launch \"%1\".").arg(binary.toUserOutput());
}

static inline void msgCannotRun(const FilePath &workingDirectory, const QString &message,
                                QString *errorMessage)
{
    if (errorMessage)
        *errorMessage = message;
    else
        VcsOutputWindow::appendError(workingDirectory, message);
}

static inline void msgCannotRun(const QStringList &args, const FilePath &workingDirectory,
                                const QString &error, QString *errorMessage)
{
    const QString message = Tr::tr("Cannot run \"%1\" in \"%2\": %3")
            .arg("git " + args.join(' '),
                 workingDirectory.toUserOutput(),
                 error);

    msgCannotRun(workingDirectory, message, errorMessage);
}

// ---------------- GitClient

GitClient &gitClient()
{
    static GuardedObject<GitClient> client;
    return client;
}

GitClient::GitClient()
    : VcsBase::VcsBaseClientImpl(&Internal::settings())
{
    m_gitQtcEditor = QString::fromLatin1("\"%1\" -client -block -pid %2")
            .arg(QCoreApplication::applicationFilePath())
            .arg(QCoreApplication::applicationPid());

    if (VcsBase::Internal::commonSettings().vcsShowStatus())
        setupTimer();
    connect(&VcsBase::Internal::commonSettings().vcsShowStatus, &Utils::BaseAspect::changed,
            [this] {
        bool enable = VcsBase::Internal::commonSettings().vcsShowStatus();
        QTC_CHECK(enable == bool(!m_timer));
        if (enable) {
            setupTimer();
        } else {
            m_timer.reset();
            for (auto fp : std::as_const(m_modifInfos)) {
                m_modifInfos[fp.rootPath].modifiedFiles.clear();
                emitClearFileStatus(fp.rootPath);
            }
        }
    });
}

GitClient::~GitClient() = default;

GitSettings &GitClient::settings()
{
    return Internal::settings();
}

FilePath GitClient::findRepositoryForDirectory(const FilePath &directory) const
{
    return VcsManager::findRepositoryForFiles(directory, {".git", ".git/config"});
}

FilePath GitClient::findGitDirForRepository(const FilePath &repositoryDir) const
{
    static QHash<FilePath, FilePath> repoDirCache;
    FilePath &res = repoDirCache[repositoryDir];
    if (!res.isEmpty())
        return res;

    QString output;
    synchronousRevParseCmd(repositoryDir, "--git-dir", &output);

    res = repositoryDir.resolvePath(output);
    return res;
}

bool GitClient::managesFile(const FilePath &workingDirectory, const QString &fileName) const
{
    const CommandResult result = vcsSynchronousExec(workingDirectory,
                                 {"ls-files", "--error-unmatch", fileName}, RunFlags::NoOutput);
    return result.result() == ProcessResult::FinishedWithSuccess;
}

FilePaths GitClient::unmanagedFiles(const FilePaths &filePaths) const
{
    QMap<FilePath, QStringList> filesForDir;
    for (const FilePath &fp : filePaths) {
        filesForDir[fp.parentDir()] << fp.fileName();
    }
    FilePaths res;
    for (auto it = filesForDir.begin(), end = filesForDir.end(); it != end; ++it) {
        QStringList args({"ls-files", "-z"});
        const QDir wd(it.key().toUrlishString());
        args << transform(it.value(), [&wd](const QString &fp) { return wd.relativeFilePath(fp); });
        const CommandResult result = vcsSynchronousExec(it.key(), args, RunFlags::NoOutput);
        if (result.result() != ProcessResult::FinishedWithSuccess)
            return filePaths;
        const auto toAbs = [&wd](const QString &fp) { return wd.absoluteFilePath(fp); };
        const QStringList managedFilePaths =
                Utils::transform(result.cleanedStdOut().split(QChar('\0'), Qt::SkipEmptyParts), toAbs);
        const QStringList absPaths = Utils::transform(it.value(), toAbs);
        const QStringList filtered = Utils::filtered(absPaths, [&managedFilePaths](const QString &fp) {
            return !managedFilePaths.contains(fp);
        });
        res += FilePaths::fromStrings(filtered);
    }
    return res;
}

IVersionControl::FileState GitClient::modificationState(const Utils::FilePath &workingDirectory,
                                const Utils::FilePath &fileName) const
{
    const ModificationInfo &info = m_modifInfos[workingDirectory];
    int length = workingDirectory.toUrlishString().size();
    const QString fileNameFromRoot = fileName.absoluteFilePath().path().mid(length + 1);
    return info.modifiedFiles.value(fileNameFromRoot, IVersionControl::FileState::NoModification);
}

void GitClient::stopMonitoring(const Utils::FilePath &path)
{
    const FilePath directory = path;
    // Submodule management
    const QList<FilePath> subPaths = submoduleDataToAbsolutePath(submoduleList(directory), directory);
    for (const FilePath &subModule : subPaths)
        m_modifInfos.remove(subModule);
    m_modifInfos.remove(directory);
    if (m_modifInfos.isEmpty() && m_timer)
        m_timer->stop();
}

void GitClient::monitorDirectory(const Utils::FilePath &path)
{
    const FilePath directory = path;
    if (directory.isEmpty())
        return;
    m_modifInfos.insert(directory, {directory, {}});
    // Submodule management
    const QList<FilePath> subPaths = submoduleDataToAbsolutePath(submoduleList(directory), directory);
    for (const FilePath &subModule : subPaths)
        m_modifInfos.insert(subModule, {subModule, {}});

    if (!m_timer)
        return;

    updateModificationInfos();
}

void GitClient::updateModificationInfos()
{
    for (const ModificationInfo &infoTemp : std::as_const(m_modifInfos)) {
        const FilePath path = infoTemp.rootPath;
        m_statusUpdateQueue.append(path);
    }
    updateNextModificationInfo();
}

void GitClient::updateNextModificationInfo()
{
    using IVCF = IVersionControl::FileState;

    if (m_statusUpdateQueue.isEmpty()) {
        m_timer->start();
        return;
    }

    const FilePath path = m_statusUpdateQueue.dequeue();

    const auto command = [path, this](const CommandResult &result) {
        updateNextModificationInfo();

        if (!m_modifInfos.contains(path))
            return;

        ModificationInfo &info = m_modifInfos[path];

        const QStringList res = result.cleanedStdOut().split("\n", Qt::SkipEmptyParts);
        QHash<QString, IVCF> modifiedFiles;
        for (const QString &line : res) {
            if (line.size() <= 3)
                continue;

            static const QHash<QChar, IVCF> gitStates {
                                                      {'M', IVCF::ModifiedState},
                                                      {'A', IVCF::AddedState},
                                                      {'R', IVCF::RenamedState},
                                                      {'D', IVCF::DeletedState},
                                                      {'?', IVCF::UnmanagedState},
                                                      };

            const IVCF modification = std::max(gitStates.value(line.at(0), IVCF::NoModification),
                                               gitStates.value(line.at(1), IVCF::NoModification));

            if (modification != IVCF::NoModification)
                modifiedFiles.insert(line.mid(3).trimmed(), modification);
        }

        const QHash<QString, IVCF> oldfiles = info.modifiedFiles;
        info.modifiedFiles = modifiedFiles;

        QStringList newList = modifiedFiles.keys();
        QStringList list = oldfiles.keys();
        newList.sort();
        list.sort();
        QStringList statusChangedFiles;

        std::set_symmetric_difference(std::begin(list), std::end(list),
                                      std::begin(newList), std::end(newList),
                                      std::back_inserter(statusChangedFiles));

        emitFileStatusChanged(info.rootPath, statusChangedFiles);
    };
    enqueueCommand({path, {"status", "-s", "--porcelain", "--ignore-submodules"},
                    RunFlags::NoOutput, {}, {}, command});
}

TextEncoding GitClient::defaultCommitEncoding() const
{
    // Set default commit encoding to 'UTF-8', when it's not set,
    // to solve displaying error of commit log with non-latin characters.
    return TextEncoding::Utf8;
}

TextEncoding GitClient::encoding(GitClient::EncodingType encodingType, const FilePath &source) const
{
    auto encoding = [this](const FilePath &workingDirectory, const QString &configVar) {
        const QString codecName = readConfigValue(workingDirectory, configVar).trimmed();
        if (codecName.isEmpty())
            return defaultCommitEncoding();
        return TextEncoding(codecName.toUtf8());
    };

    switch (encodingType) {
    case EncodingSource:
        return source.isFile() ? VcsBaseEditor::getEncoding(source) : encoding(source, "gui.encoding");
    case EncodingLogOutput:
        return encoding(source, "i18n.logOutputEncoding");
    case EncodingCommit:
        return encoding(source, "i18n.commitEncoding");
    default:
        return {};
    }
}

void GitClient::requestReload(const QString &documentId, const FilePath &source,
                              const QString &title, const FilePath &workingDirectory,
                              std::function<GitBaseDiffEditorController *(IDocument *)> factory) const
{
    // Creating document might change the referenced source. Store a copy and use it.
    const FilePath sourceCopy = source;

    IDocument *document = DiffEditorController::findOrCreateDocument(documentId, title);
    QTC_ASSERT(document, return);
    GitBaseDiffEditorController *controller = factory(document);
    QTC_ASSERT(controller, return);
    controller->setVcsBinary(vcsBinary(workingDirectory));
    controller->setProcessEnvironment(processEnvironment(workingDirectory));
    controller->setWorkingDirectory(workingDirectory);

    using namespace std::placeholders;

    VcsBase::setSource(document, sourceCopy);
    EditorManager::activateEditorForDocument(document);
    controller->requestReload();
}

void GitClient::diffFiles(const FilePath &workingDirectory,
                          const QStringList &unstagedFileNames,
                          const QStringList &stagedFileNames) const
{
    const QString documentId = QLatin1String(Constants::GIT_PLUGIN)
            + QLatin1String(".DiffFiles.") + workingDirectory.toUrlishString();
    requestReload(documentId,
                  workingDirectory, Tr::tr("Git Diff Files"), workingDirectory,
                  [stagedFileNames, unstagedFileNames](IDocument *doc) {
                      return new FileListDiffController(doc, stagedFileNames, unstagedFileNames);
                  });
}

static QStringList diffModeArguments(GitClient::DiffMode diffMode, QStringList args = {})
{
    if (diffMode == GitClient::Staged)
        args.prepend("--cached");
    return args;
}

void GitClient::diffProject(const FilePath &workingDirectory, const QString &projectDirectory,
                            DiffMode diffMode) const
{
    const QString title = (diffMode == Staged)
        ? Tr::tr("Git Diff Staged Project Changes")
        : Tr::tr("Git Diff Project");
    const QString documentId = QLatin1String(Constants::GIT_PLUGIN)
            + QLatin1String(".DiffProject.") + workingDirectory.toUrlishString();
    const QStringList args = diffModeArguments(diffMode, {"--", projectDirectory});
    requestReload(documentId,
                  workingDirectory, title, workingDirectory,
                  [&args](IDocument *doc) {
                      return new GitDiffEditorController(doc, {}, {}, args);
                  });
}

void GitClient::diffRepository(const FilePath &workingDirectory,
                               const QString &leftCommit,
                               const QString &rightCommit,
                               DiffMode diffMode) const
{
    const QString title = (diffMode == Staged)
        ? Tr::tr("Git Diff Staged Repository Changes")
        : Tr::tr("Git Diff Repository");
    const QString documentId = QLatin1String(Constants::GIT_PLUGIN)
            + QLatin1String(".DiffRepository.") + workingDirectory.toUrlishString();
    const QStringList args = diffModeArguments(diffMode);
    requestReload(documentId, workingDirectory, title, workingDirectory,
                  [&leftCommit, &rightCommit, &args](IDocument *doc) {
        return new GitDiffEditorController(doc, leftCommit, rightCommit, args);
    });
}

void GitClient::diffFile(const FilePath &workingDirectory, const QString &fileName,
                         DiffMode diffMode) const
{
    const QString title = (diffMode == Staged)
        ? Tr::tr("Git Diff Staged \"%1\" Changes").arg(fileName)
        : Tr::tr("Git Diff \"%1\"").arg(fileName);
    const FilePath sourceFile = VcsBaseEditor::getSource(workingDirectory, fileName);
    const QString documentId = QLatin1String(Constants::GIT_PLUGIN)
            + QLatin1String(".DiffFile.") + sourceFile.toUrlishString();
    const QStringList args = diffModeArguments(diffMode, {"--", fileName});
    requestReload(documentId, sourceFile, title, workingDirectory,
                  [&args](IDocument *doc) {
        return new GitDiffEditorController(doc, {}, {}, args);
    });
}

void GitClient::diffBranch(const FilePath &workingDirectory, const QString &branchName) const
{
    const QString title = Tr::tr("Git Diff Branch \"%1\"").arg(branchName);
    const QString documentId = QLatin1String(Constants::GIT_PLUGIN)
            + QLatin1String(".DiffBranch.") + branchName;
    requestReload(documentId, workingDirectory, title, workingDirectory,
                  [branchName](IDocument *doc) {
        return new GitDiffEditorController(doc, branchName, {}, {});
    });
}

void GitClient::merge(const FilePath &workingDirectory, const QStringList &unmergedFileNames)
{
    auto mergeTool = new MergeTool(this);
    mergeTool->start(workingDirectory, unmergedFileNames);
}

void GitClient::status(const FilePath &workingDirectory)
{
    enqueueCommand({workingDirectory, {"status"}, RunFlags::ShowStdOut});
}

void GitClient::fullStatus(const FilePath &workingDirectory)
{
    enqueueCommand({workingDirectory, {"status", "-u"}, RunFlags::ShowStdOut});
}

static QStringList normalLogArguments()
{
    const ColorNames colors = GitClient::colorNames();
    const QString logArgs = QStringLiteral(
                "--pretty=format:"
                "commit %C(%1)%H%Creset %C(%2)%d%Creset%n"
                "Author: %C(%3)%aN <%aE>%Creset%n"
                "Date:   %C(%4)%cD %Creset%n%n"
                "%C(%5)%w(0,4,4)%s%Creset%n%n%b"
                ).arg(colors.hash, colors.decoration, colors.author, colors.date, colors.subject);
    return {logArgs};
}

void GitClient::log(const FilePath &workingDirectory, const QString &fileName,
                    bool enableAnnotationContextMenu, const QStringList &args)
{
    QString msgArg;
    if (!fileName.isEmpty())
        msgArg = fileName;
    else if (!args.isEmpty() && !args.first().startsWith('-'))
        msgArg = args.first();
    else
        msgArg = workingDirectory.toUrlishString();
    // Creating document might change the referenced workingDirectory. Store a copy and use it.
    const FilePath workingDir = workingDirectory;
    const QString title = Tr::tr("Git Log \"%1\"").arg(msgArg);
    const Id editorId = Git::Constants::GIT_LOG_EDITOR_ID;
    const FilePath sourceFile = VcsBaseEditor::getSource(workingDir, fileName);
    GitEditorWidget *editor = static_cast<GitEditorWidget *>(createVcsEditor(
        editorId, title, sourceFile, encoding(EncodingLogOutput, sourceFile), "logTitle", msgArg));
    VcsBaseEditorConfig *argWidget = editor->editorConfig();
    if (!argWidget) {
        argWidget = new GitLogConfig(!fileName.isEmpty(), editor);
        argWidget->setBaseArguments(args);
        connect(argWidget, &VcsBaseEditorConfig::commandExecutionRequested, this,
                [this, workingDir, fileName, enableAnnotationContextMenu, args] {
            log(workingDir, fileName, enableAnnotationContextMenu, args);
        });
        editor->setEditorConfig(argWidget);
    }
    editor->setFileLogAnnotateEnabled(enableAnnotationContextMenu);
    editor->setWorkingDirectory(workingDir);

    QStringList arguments = {"log", decorateOption};
    int logCount = settings().logCount();
    if (logCount > 0)
        arguments << "-n" << QString::number(logCount);

    arguments << argWidget->arguments();
    if (arguments.contains(patchOption)) {
        arguments.removeAll(colorOption);
        editor->setHighlightingEnabled(true);
    } else {
        editor->setHighlightingEnabled(false);
    }

    // remove "all branches" option when "log for line" is requested as they conflict
    if (Utils::anyOf(arguments, [](const QString &arg) { return arg.startsWith("-L "); }))
        arguments.removeAll(allBranchesOption);

    if (!arguments.contains(graphOption) && !arguments.contains(patchOption))
        arguments << normalLogArguments();

    const QString authorValue = editor->authorValue();
    if (!authorValue.isEmpty())
        arguments << "--author=" + ProcessArgs::quoteArg(authorValue);

    const QString grepValue = editor->grepValue();
    if (!grepValue.isEmpty())
        arguments << "--grep=" + ProcessArgs::quoteArg(grepValue);

    const QString pickaxeValue = editor->pickaxeValue();
    if (!pickaxeValue.isEmpty())
        arguments << "-S" << ProcessArgs::quoteArg(pickaxeValue);

    if (!editor->caseSensitive())
        arguments << "-i";

    if (!fileName.isEmpty())
        arguments << "--" << fileName;

    executeInEditor(workingDir, arguments, editor);
}

void GitClient::reflog(const FilePath &workingDirectory, const QString &ref)
{
    const QString title = Tr::tr("Git Reflog \"%1\"").arg(workingDirectory.toUserOutput());
    const Id editorId = Git::Constants::GIT_REFLOG_EDITOR_ID;
    // Creating document might change the referenced workingDirectory. Store a copy and use it.
    const FilePath workingDir = workingDirectory;
    GitEditorWidget *editor = static_cast<GitEditorWidget *>(
                createVcsEditor(editorId, title, workingDir, encoding(EncodingLogOutput),
                                "reflogRepository", workingDir.toUrlishString()));
    VcsBaseEditorConfig *argWidget = editor->editorConfig();
    if (!argWidget) {
        argWidget = new GitRefLogConfig(editor);
        if (!ref.isEmpty())
            argWidget->setBaseArguments({ref});
        connect(argWidget, &VcsBaseEditorConfig::commandExecutionRequested, this,
                [this, workingDir, ref] { reflog(workingDir, ref); });
        editor->setEditorConfig(argWidget);
    }
    editor->setWorkingDirectory(workingDir);

    QStringList arguments = {"reflog", noColorOption, decorateOption};
    arguments << argWidget->arguments();
    int logCount = settings().logCount();
    if (logCount > 0)
        arguments << "-n" << QString::number(logCount);

    executeInEditor(workingDir, arguments, editor);
}

// Do not show "0000" or "^32ae4"
static inline bool canShow(const QString &hash)
{
    return !hash.startsWith('^') && hash.count('0') != hash.size();
}

static inline QString msgCannotShow(const QString &hash)
{
    return Tr::tr("Cannot describe \"%1\".").arg(hash);
}

void GitClient::show(const FilePath &source, const QString &id, const QString &name)
{
    FilePath workingDirectory = source.isDir() ? source.absoluteFilePath() : source.absolutePath();

    if (!canShow(id)) {
        VcsOutputWindow::appendError(workingDirectory, msgCannotShow(id));
        return;
    }

    const QString title = Tr::tr("Git Show \"%1\"").arg(name.isEmpty() ? id : name);
    const FilePath repoDirectory = VcsManager::findTopLevelForDirectory(workingDirectory);
    if (!repoDirectory.isEmpty())
        workingDirectory = repoDirectory;
    const QString documentId = QLatin1String(Constants::GIT_PLUGIN)
            + QLatin1String(".Show.") + id;
    requestReload(documentId, source, title, workingDirectory,
                  [id](IDocument *doc) { return new ShowController(doc, id); });
}

void GitClient::archive(const FilePath &workingDirectory, QString commit)
{
    FilePath repoDirectory = VcsManager::findTopLevelForDirectory(workingDirectory);
    if (repoDirectory.isEmpty())
        repoDirectory = workingDirectory;
    const QString repoName = repoDirectory.fileName();

    QHash<QString, QString> filters;
    QString selectedFilter;
    auto appendFilter = [&filters, &selectedFilter](const QString &name, bool isSelected){
        const auto mimeType = Utils::mimeTypeForName(name);
        const auto filterString = mimeType.filterString();
        filters.insert(filterString, "." + mimeType.preferredSuffix());
        if (isSelected)
            selectedFilter = filterString;
    };

    bool windows = HostOsInfo::isWindowsHost();
    appendFilter("application/zip", windows);
    appendFilter("application/x-compressed-tar", !windows);

    QString output;
    if (synchronousRevParseCmd(repoDirectory, commit, &output))
        commit = output.trimmed();

    FilePath archiveName = FileUtils::getSaveFilePath(
                Tr::tr("Generate %1 archive").arg(repoName),
                repoDirectory.pathAppended(QString("../%1-%2").arg(repoName, commit.left(8))),
                filters.keys().join(";;"),
                &selectedFilter);
    if (archiveName.isEmpty())
        return;
    const QString extension = filters.value(selectedFilter);
    QFileInfo archive(archiveName.toUrlishString());
    if (extension != "." + archive.completeSuffix()) {
        archive = QFileInfo(archive.filePath() + extension);
    }

    if (archive.exists()) {
        if (QMessageBox::warning(ICore::dialogParent(), Tr::tr("Overwrite?"),
            Tr::tr("An item named \"%1\" already exists at this location. "
               "Do you want to overwrite it?").arg(QDir::toNativeSeparators(archive.absoluteFilePath())),
            QMessageBox::Yes | QMessageBox::No) == QMessageBox::No) {
            return;
        }
    }

    enqueueCommand({workingDirectory, {"archive", commit, "-o", archive.absoluteFilePath()},
                    RunFlags::ShowStdOut});
}

void GitClient::annotate(const Utils::FilePath &workingDir, const QString &file, int lineNumber,
                         const QString &revision, const QStringList &extraOptions, int firstLine)
{
    const Id editorId = Git::Constants::GIT_BLAME_EDITOR_ID;
    const QString id = VcsBaseEditor::getTitleId(workingDir, {file}, revision);
    const QString title = Tr::tr("Git Blame \"%1\"").arg(id);
    const FilePath sourceFile = VcsBaseEditor::getSource(workingDir, file);

    VcsBaseEditorWidget *editor = createVcsEditor(editorId, title, sourceFile,
            encoding(EncodingSource, sourceFile), "blameFileName", id);
    VcsBaseEditorConfig *argWidget = editor->editorConfig();
    if (!argWidget) {
        argWidget = new GitBlameConfig(editor->toolBar());
        argWidget->setBaseArguments(extraOptions);
        connect(argWidget, &VcsBaseEditorConfig::commandExecutionRequested, this,
                [this, workingDir, file, revision, extraOptions] {
            const int line = VcsBaseEditor::lineNumberOfCurrentEditor();
            annotate(workingDir, file, line, revision, extraOptions);
        });
        editor->setEditorConfig(argWidget);
    }

    editor->setWorkingDirectory(workingDir);
    QStringList arguments = {"blame", "--root"};
    arguments << argWidget->arguments();
    if (!revision.isEmpty())
        arguments << revision;
    arguments << "--" << file;
    editor->setDefaultLineNumber(lineNumber);
    if (firstLine > 0)
        editor->setFirstLineNumber(firstLine);
    executeInEditor(workingDir, arguments, editor);
}

void GitClient::checkout(const FilePath &workingDirectory, const QString &ref, StashMode stashMode,
                         const CommandHandler &handler)
{
    if (stashMode == StashMode::TryStash && !beginStashScope(workingDirectory, "Checkout"))
        return;

    const QStringList arguments = setupCheckoutArguments(workingDirectory, ref);
    const auto commandHandler = [this, stashMode, workingDirectory, handler](const CommandResult &result) {
        if (stashMode == StashMode::TryStash)
            endStashScope(workingDirectory);
        if (result.result() == ProcessResult::FinishedWithSuccess)
            updateSubmodulesIfNeeded(workingDirectory, true);
        if (handler)
            handler(result);
    };
    enqueueCommand({workingDirectory, arguments,
                    RunFlags::ShowStdOut | RunFlags::ExpectRepoChanges | RunFlags::ShowSuccessMessage,
                    {}, {}, commandHandler});
}

/* method used to setup arguments for checkout, in case user wants to create local branch */
QStringList GitClient::setupCheckoutArguments(const FilePath &workingDirectory,
                                              const QString &ref)
{
    QStringList arguments = {"checkout", ref};

    QStringList localBranches = synchronousRepositoryBranches(workingDirectory.toUrlishString());
    if (localBranches.contains(ref))
        return arguments;

    if (Utils::CheckableMessageBox::question(
            Tr::tr("Create Local Branch") /*title*/,
            Tr::tr("Would you like to create a local branch?") /*message*/,
            Key("Git.CreateLocalBranchOnCheckout"), /* decider */
            QMessageBox::Yes | QMessageBox::No /*buttons*/,
            QMessageBox::No /*default button*/,
            QMessageBox::No /*button to save*/)
        != QMessageBox::Yes) {
        return arguments;
    }

    if (synchronousCurrentLocalBranch(workingDirectory).isEmpty())
        localBranches.removeFirst();

    QString refSha;
    if (!synchronousRevParseCmd(workingDirectory, ref, &refSha))
        return arguments;

    QString output;
    const QStringList forEachRefArgs = {"refs/remotes/", "--format=%(objectname) %(refname:short)"};
    if (!synchronousForEachRefCmd(workingDirectory, forEachRefArgs, &output))
        return arguments;

    QString remoteBranch;
    const QString head("/HEAD");

    const QStringList refs = output.split('\n');
    for (const QString &singleRef : refs) {
        if (singleRef.startsWith(refSha)) {
            // branch name might be origin/foo/HEAD
            if (!singleRef.endsWith(head) || singleRef.count('/') > 1) {
                remoteBranch = singleRef.mid(refSha.length() + 1);
                if (remoteBranch == ref)
                    break;
            }
        }
    }

    QString target = remoteBranch;
    BranchTargetType targetType = BranchTargetType::Remote;
    if (remoteBranch.isEmpty()) {
        target = ref;
        targetType = BranchTargetType::Commit;
    }
    const QString suggestedName = suggestedLocalBranchName(
                workingDirectory, localBranches, target, targetType);
    BranchAddDialog branchAddDialog(localBranches, BranchAddDialog::Type::AddBranch, ICore::dialogParent());
    branchAddDialog.setBranchName(suggestedName);
    branchAddDialog.setTrackedBranchName(remoteBranch, true);

    if (branchAddDialog.exec() != QDialog::Accepted)
        return arguments;

    arguments.removeLast();
    arguments << "-b" << branchAddDialog.branchName();
    if (branchAddDialog.track())
        arguments << "--track" << remoteBranch;
    else
        arguments << "--no-track" << ref;

    return arguments;
}

void GitClient::reset(const FilePath &workingDirectory, const QString &argument, const QString &commit)
{
    QStringList arguments = {"reset", argument};
    if (!commit.isEmpty())
        arguments << commit;

    RunFlags flags = RunFlags::ShowStdOut | RunFlags::ShowSuccessMessage;
    if (argument == "--hard")
        flags |= RunFlags::ExpectRepoChanges;

    const auto isHard = [argument] { return argument == "--hard"; };

    const Storage<StatusResultData> statusResultStorage;

    const auto onStatusDone = [statusResultStorage] {
        if (statusResultStorage->result == StatusResult::Unchanged)
            return true;

        return QMessageBox::question(
                   Core::ICore::dialogParent(), Tr::tr("Reset"),
                   Tr::tr("All changes in working directory will be discarded. Are you sure?"),
                   QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
    };

    const Group recipe {
        If (isHard) >> Then {
            statusResultStorage,
            statusTask(workingDirectory, StatusMode(NoUntracked | NoSubmodules), statusResultStorage),
            onGroupDone(onStatusDone, CallDone::OnSuccess)
        },
        commandTask({workingDirectory, arguments, flags})
    };

    enqueueTask(recipe);
}

void GitClient::removeStaleRemoteBranches(const FilePath &workingDirectory, const QString &remote)
{
    const QStringList arguments = {"remote", "prune", remote};
    const auto commandHandler = [workingDirectory](const CommandResult &result) {
        if (result.result() == ProcessResult::FinishedWithSuccess)
            updateBranches(workingDirectory);
    };
    enqueueCommand({workingDirectory, arguments,
                    RunFlags::ShowStdOut | RunFlags::ShowSuccessMessage, {}, {}, commandHandler});
}

void GitClient::recoverDeletedFiles(const FilePath &workingDirectory)
{
    const auto commandHandler = [this, workingDirectory](const CommandResult &result) {
        if (result.result() == ProcessResult::FinishedWithSuccess) {
            const QString stdOut = result.cleanedStdOut().trimmed();
            if (stdOut.isEmpty()) {
                VcsOutputWindow::appendError(workingDirectory, Tr::tr("Nothing to recover"));
                return;
            }
            const QStringList files = stdOut.split('\n');
            synchronousCheckoutFiles(workingDirectory, files, QString(), nullptr, false);
            VcsOutputWindow::appendMessage(workingDirectory, Tr::tr("Files recovered"));
        }
    };
    enqueueCommand({workingDirectory, {"ls-files", "--deleted"}, RunFlags::SuppressCommandLogging,
                    {}, {}, commandHandler});
}

void GitClient::addFile(const FilePath &workingDirectory, const QString &fileName)
{
    enqueueCommand({workingDirectory, {"add", fileName}});
}

Result<QString> GitClient::synchronousLog(const FilePath &workingDirectory,
                                          const QStringList &arguments,
                                          RunFlags flags)
{
    QStringList allArguments = {"log", noColorOption};

    allArguments.append(arguments);

    const CommandResult result = vcsSynchronousExec(workingDirectory, allArguments, flags,
                        vcsTimeoutS(), encoding(EncodingLogOutput, workingDirectory));
    if (result.result() == ProcessResult::FinishedWithSuccess)
        return result.cleanedStdOut();

    QString errorMessage;
    msgCannotRun(workingDirectory, Tr::tr("Cannot obtain log of \"%1\": %2")
                 .arg(workingDirectory.toUserOutput(), result.cleanedStdErr()), &errorMessage);
    return ResultError(errorMessage);
}

bool GitClient::synchronousAdd(const FilePath &workingDirectory,
                               const QStringList &files,
                               const QStringList &extraOptions)
{
    QStringList args{"add"};
    args += extraOptions;
    args += "--";
    args += files;
    return vcsSynchronousExec(workingDirectory, args).result()
            == ProcessResult::FinishedWithSuccess;
}

bool GitClient::synchronousDelete(const FilePath &workingDirectory,
                                  bool force,
                                  const QStringList &files)
{
    QStringList arguments = {"rm"};
    if (force)
        arguments << "--force";
    arguments << "--";
    arguments.append(files);
    return vcsSynchronousExec(workingDirectory, arguments).result()
            == ProcessResult::FinishedWithSuccess;
}

bool GitClient::synchronousMove(
    const FilePath &workingDirectory, const FilePath &from, const FilePath &to)
{
    return vcsSynchronousExec(workingDirectory, {"mv", from.path(), to.path()}).result()
            == ProcessResult::FinishedWithSuccess;
}

bool GitClient::synchronousReset(const FilePath &workingDirectory,
                                 const QStringList &files,
                                 QString *errorMessage)
{
    QStringList arguments = {"reset"};
    if (files.isEmpty())
        arguments << "--hard";
    else
        arguments << HEAD << "--" << files;

    const CommandResult result = vcsSynchronousExec(workingDirectory, arguments);
    const QString stdOut = result.cleanedStdOut();
    VcsOutputWindow::appendSilently(workingDirectory, stdOut);
    // Note that git exits with 1 even if the operation is successful
    // Assume real failure if the output does not contain "foo.cpp modified"
    // or "Unstaged changes after reset" (git 1.7.0).
    if (result.result() != ProcessResult::FinishedWithSuccess
        && (!stdOut.contains("modified") && !stdOut.contains("Unstaged changes after reset"))) {
        if (files.isEmpty()) {
            msgCannotRun(arguments, workingDirectory, result.cleanedStdErr(), errorMessage);
        } else {
            msgCannotRun(workingDirectory,
                         Tr::tr("Cannot reset %n files in \"%1\": %2", nullptr, files.size())
                            .arg(workingDirectory.toUserOutput(), result.cleanedStdErr()),
                         errorMessage);
        }
        return false;
    }
    return true;
}

// Initialize repository
bool GitClient::synchronousInit(const FilePath &workingDirectory)
{
    const CommandResult result = vcsSynchronousExec(workingDirectory, QStringList{"init"});
    // '[Re]Initialized...'
    VcsOutputWindow::appendSilently(workingDirectory, result.cleanedStdOut());
    if (result.result() == ProcessResult::FinishedWithSuccess) {
        resetCachedVcsInfo(workingDirectory);
        return true;
    }
    return false;
}

bool GitClient::synchronousAddGitignore(const FilePath &workingDirectory)
{
    const FilePath gitIgnoreDestination = workingDirectory.pathAppended(".gitignore");

    auto intentToAddGitignore = [this, workingDirectory, gitIgnoreDestination] {
        return synchronousAdd(workingDirectory, {gitIgnoreDestination.fileName()}, {"--intent-to-add"});
    };

    if (gitIgnoreDestination.exists())
        return intentToAddGitignore();

    const FilePath gitIgnoreTemplate =
        Core::ICore::resourcePath().pathAppended("templates/wizards/projects/git.ignore");

    if (!QTC_GUARD(gitIgnoreTemplate.exists()))
        return false;

    Core::GeneratedFile gitIgnoreFile(gitIgnoreDestination);
    gitIgnoreFile.setBinaryContents(gitIgnoreTemplate.fileContents().value());
    if (const Result<> res = gitIgnoreFile.write(); !res) {
        VcsOutputWindow::appendError(workingDirectory, res.error());
        return false;
    }

    return intentToAddGitignore();
}

/* Checkout, supports:
 * git checkout -- <files>
 * git checkout revision -- <files>
 * git checkout revision -- . */
bool GitClient::synchronousCheckoutFiles(const FilePath &workingDirectory, QStringList files,
                                         QString revision, QString *errorMessage,
                                         bool revertStaging)
{
    if (revertStaging && revision.isEmpty())
        revision = HEAD;
    if (files.isEmpty())
        files = QStringList(".");
    QStringList arguments = {"checkout"};
    if (revertStaging)
        arguments << revision;
    arguments << "--" << files;
    const CommandResult result = vcsSynchronousExec(workingDirectory, arguments,
                                                    RunFlags::ExpectRepoChanges);
    if (result.result() == ProcessResult::FinishedWithSuccess)
        return true;

    const QString fileArg = files.join(", ");
    //: Meaning of the arguments: %1: revision, %2: files, %3: repository,
    //: %4: Error message
    msgCannotRun(workingDirectory, Tr::tr("Cannot checkout \"%1\" of %2 in \"%3\": %4")
                 .arg(revision, fileArg, workingDirectory.toUserOutput(), result.cleanedStdErr()),
                 errorMessage);
    return false;
}

static QString msgParentRevisionFailed(const FilePath &workingDirectory,
                                              const QString &revision,
                                              const QString &why)
{
    //: Failed to find parent revisions of a hash for "annotate previous"
    return Tr::tr("Cannot find parent revisions of \"%1\" in \"%2\": %3")
            .arg(revision, workingDirectory.toUserOutput(), why);
}

static QString msgInvalidRevision()
{
    return Tr::tr("Invalid revision");
}

// Split a line of "<commit> <parent1> ..." to obtain parents from "rev-list" or "log".
static inline bool splitCommitParents(const QString &line,
                                      QString *commit = nullptr,
                                      QStringList *parents = nullptr)
{
    if (commit)
        commit->clear();
    if (parents)
        parents->clear();
    QStringList tokens = line.trimmed().split(' ');
    if (tokens.size() < 2)
        return false;
    if (commit)
        *commit = tokens.front();
    tokens.pop_front();
    if (parents)
        *parents = tokens;
    return true;
}

bool GitClient::synchronousRevListCmd(const FilePath &workingDirectory, const QStringList &extraArguments,
                                      QString *output, QString *errorMessage) const
{
    const QStringList arguments = QStringList({"rev-list", noColorOption}) + extraArguments;
    const CommandResult result = vcsSynchronousExec(workingDirectory, arguments, RunFlags::NoOutput);
    if (result.result() != ProcessResult::FinishedWithSuccess) {
        msgCannotRun(arguments, workingDirectory, result.cleanedStdErr(), errorMessage);
        return false;
    }
    *output = result.cleanedStdOut();
    return true;
}

// Find out the immediate parent revisions of a revision of the repository.
// Might be several in case of merges.
bool GitClient::synchronousParentRevisions(const FilePath &workingDirectory,
                                           const QString &revision,
                                           QStringList *parents,
                                           QString *errorMessage) const
{
    if (parents && !isValidRevision(revision)) { // Not Committed Yet
        *parents = QStringList(HEAD);
        return true;
    }
    QString outputText;
    QString errorText;
    if (!synchronousRevListCmd(workingDirectory, {"--parents", "--max-count=1", revision},
                               &outputText, &errorText)) {
        *errorMessage = msgParentRevisionFailed(workingDirectory, revision, errorText);
        return false;
    }
    // Should result in one line of blank-delimited revisions, specifying current first
    // unless it is top.
    outputText.remove('\n');
    if (!splitCommitParents(outputText, nullptr, parents)) {
        *errorMessage = msgParentRevisionFailed(workingDirectory, revision, msgInvalidRevision());
        return false;
    }
    return true;
}

QString GitClient::synchronousShortDescription(const FilePath &workingDirectory, const QString &revision) const
{
    // HACK: The hopefully rare "_-_" will be replaced by quotes in the output,
    // leaving it in breaks command line quoting on Windows, see QTCREATORBUG-23208.
    const QString quoteReplacement = "_-_";

    // Short hash, author, subject
    const QString defaultShortLogFormat = "%h (%aN " + quoteReplacement + "%s";
    const int maxShortLogLength = 120;

    // Short hash, author, subject
    QString output = synchronousShortDescription(workingDirectory, revision, defaultShortLogFormat);
    output.replace(quoteReplacement, "\"");
    if (output != revision) {
        if (output.length() > maxShortLogLength) {
            output.truncate(maxShortLogLength);
            output.append("...");
        }
        output.append("\")");
    }
    return output;
}

QString GitClient::synchronousCurrentLocalBranch(const FilePath &workingDirectory) const
{
    QString branch;
    const CommandResult result = vcsSynchronousExec(workingDirectory, {"symbolic-ref", HEAD},
                                                    RunFlags::NoOutput);
    if (result.result() == ProcessResult::FinishedWithSuccess) {
        branch = result.cleanedStdOut().trimmed();
    } else {
        const FilePath gitDir = findGitDirForRepository(workingDirectory);
        const FilePath rebaseHead = gitDir / "rebase-merge/head-name";
        QFile head(rebaseHead.toFSPathString());
        if (head.open(QFile::ReadOnly))
            branch = QString::fromUtf8(head.readLine()).trimmed();
    }
    if (!branch.isEmpty()) {
        const QString refsHeadsPrefix = "refs/heads/";
        if (branch.startsWith(refsHeadsPrefix)) {
            branch.remove(0, refsHeadsPrefix.size());
            return branch;
        }
    }
    return {};
}

bool GitClient::synchronousHeadRefs(const FilePath &workingDirectory, QStringList *output,
                                    QString *errorMessage) const
{
    const QStringList arguments = {"show-ref", "--head", "--abbrev=10", "--dereference"};
    const CommandResult result = vcsSynchronousExec(workingDirectory, arguments, RunFlags::NoOutput);
    if (result.result() != ProcessResult::FinishedWithSuccess) {
        msgCannotRun(arguments, workingDirectory, result.cleanedStdErr(), errorMessage);
        return false;
    }

    const QString stdOut = result.cleanedStdOut();
    const QString headHash = stdOut.left(10);
    QString rest = stdOut.mid(15);

    const QStringList headHashLines = Utils::filtered(
                rest.split('\n'), [&headHash](const QString &s) { return s.startsWith(headHash); });
    *output = Utils::transform(headHashLines, [](const QString &s) { return s.mid(11); }); // hash + space

    return true;
}

// Retrieve topic (branch, tag or HEAD hash)
QString GitClient::synchronousTopic(const FilePath &workingDirectory) const
{
    // First try to find branch
    const QString branch = synchronousCurrentLocalBranch(workingDirectory);
    if (!branch.isEmpty())
        return branch;

    // Detached HEAD, try a tag or remote branch
    QStringList references;
    if (!synchronousHeadRefs(workingDirectory, &references))
        return {};

    const QString tagStart("refs/tags/");
    const QString remoteStart("refs/remotes/");
    const QString dereference("^{}");
    QString remoteBranch;

    for (const QString &ref : std::as_const(references)) {
        int derefInd = ref.indexOf(dereference);
        if (ref.startsWith(tagStart))
            return ref.mid(tagStart.size(), (derefInd == -1) ? -1 : derefInd - tagStart.size());
        if (ref.startsWith(remoteStart)) {
            remoteBranch = ref.mid(remoteStart.size(),
                                   (derefInd == -1) ? -1 : derefInd - remoteStart.size());
        }
    }
    if (!remoteBranch.isEmpty())
        return remoteBranch;

    // No tag or remote branch - try git describe
    const CommandResult result = vcsSynchronousExec(workingDirectory, QStringList{"describe"},
                                                    RunFlags::NoOutput);
    if (result.result() == ProcessResult::FinishedWithSuccess) {
        const QString stdOut = result.cleanedStdOut().trimmed();
        if (!stdOut.isEmpty())
            return stdOut;
    }
    return Tr::tr("Detached HEAD");
}

bool GitClient::synchronousRevParseCmd(const FilePath &workingDirectory, const QString &ref,
                                       QString *output, QString *errorMessage) const
{
    const QStringList arguments = {"rev-parse", ref};
    const CommandResult result = vcsSynchronousExec(workingDirectory, arguments, RunFlags::NoOutput);
    *output = result.cleanedStdOut().trimmed();
    if (result.result() == ProcessResult::FinishedWithSuccess)
        return true;
    msgCannotRun(arguments, workingDirectory, result.cleanedStdErr(), errorMessage);
    return false;
}

// Retrieve head revision
GroupItem GitClient::topRevision(const FilePath &workingDirectory,
    const std::function<void(const QString &, const QDateTime &)> &callback)
{
    const auto onProcessSetup = [this, workingDirectory](Process &process) {
        setupCommand(process, workingDirectory, {"show", "-s", "--pretty=format:%H:%ct", HEAD});
    };
    const auto onProcessDone = [callback](const Process &process) {
        const QStringList output = process.cleanedStdOut().trimmed().split(':');
        QDateTime dateTime;
        if (output.size() > 1) {
            bool ok = false;
            const qint64 timeT = output.at(1).toLongLong(&ok);
            if (ok)
                dateTime = QDateTime::fromSecsSinceEpoch(timeT);
        }
        callback(output.first(), dateTime);
    };

    return ProcessTask(onProcessSetup, onProcessDone, CallDone::OnSuccess);
}

bool GitClient::isRemoteCommit(const FilePath &workingDirectory, const QString &commit)
{
    const CommandResult result = vcsSynchronousExec(workingDirectory,
                                 {"branch", "-r", "--contains", commit}, RunFlags::NoOutput);
    return !result.rawStdOut().isEmpty();
}

// Format an entry in a one-liner for selection list using git log.
QString GitClient::synchronousShortDescription(const FilePath &workingDirectory, const QString &revision,
                                            const QString &format) const
{
    const QStringList arguments = {"log", noColorOption, ("--pretty=format:" + format),
                                   "--max-count=1", revision};
    const CommandResult result = vcsSynchronousExec(workingDirectory, arguments, RunFlags::NoOutput);
    if (result.result() != ProcessResult::FinishedWithSuccess) {
        VcsOutputWindow::appendSilently(workingDirectory,
            Tr::tr("Cannot describe revision \"%1\" in \"%2\": %3")
                .arg(revision, workingDirectory.toUserOutput(), result.cleanedStdErr()));
        return revision;
    }
    return stripLastNewline(result.cleanedStdOut());
}

// Create a default message to be used for describing stashes
static inline QString creatorStashMessage(const QString &keyword = QString())
{
    QString rc = QCoreApplication::applicationName() + ' ';
    if (!keyword.isEmpty())
        rc += keyword + ' ';
    rc += QDateTime::currentDateTime().toString(Qt::ISODate);
    return rc;
}

/* Do a stash and return the message as identifier. Note that stash names (stash{n})
 * shift as they are pushed, so, enforce the use of messages to identify them. Flags:
 * StashPromptDescription: Prompt the user for a description message.
 * StashImmediateRestore: Immediately re-apply this stash (used for snapshots), user keeps on working
 * StashIgnoreUnchanged: Be quiet about unchanged repositories (used for IVersionControl's snapshots). */

QString GitClient::synchronousStash(const FilePath &workingDirectory, const QString &messageKeyword,
                                    unsigned flags, bool *unchanged) const
{
    if (unchanged)
        *unchanged = false;
    QString message;
    bool success = false;
    // Check for changes and stash
    QString errorMessage;
    switch (gitStatus(workingDirectory, StatusMode(NoUntracked | NoSubmodules), nullptr, &errorMessage)) {
    case StatusResult::Changed: {
        message = creatorStashMessage(messageKeyword);
        do {
            if ((flags & StashPromptDescription)) {
                if (!inputText(ICore::dialogParent(),
                               Tr::tr("Stash Description"), Tr::tr("Description:"), &message))
                    break;
            }
            if (!executeSynchronousStash(workingDirectory, message))
                break;
            if ((flags & StashImmediateRestore)
                && !synchronousStashRestore(workingDirectory, "stash@{0}"))
                break;
            success = true;
        } while (false);
        break;
    }
    case StatusResult::Unchanged:
        if (unchanged)
            *unchanged = true;
        if (!(flags & StashIgnoreUnchanged))
            VcsOutputWindow::appendWarning(workingDirectory, msgNoChangedFiles());
        break;
    case StatusResult::Failed:
        VcsOutputWindow::appendError(workingDirectory, errorMessage);
        break;
    }
    if (!success)
        message.clear();
    return message;
}

bool GitClient::executeSynchronousStash(const FilePath &workingDirectory,
                                        const QString &message,
                                        bool unstagedOnly,
                                        QString *errorMessage) const
{
    QStringList arguments = {"stash", "push"};
    if (unstagedOnly)
        arguments << "--keep-index";
    if (!message.isEmpty())
        arguments << "-m" << message;
    const RunFlags flags = RunFlags::ShowStdOut
                         | RunFlags::ExpectRepoChanges
                         | RunFlags::ShowSuccessMessage;
    const CommandResult result = vcsSynchronousExec(workingDirectory, arguments, flags);
    if (result.result() == ProcessResult::FinishedWithSuccess)
        return true;
    msgCannotRun(arguments, workingDirectory, result.cleanedStdErr(), errorMessage);
    return false;
}

// Resolve a stash name from message
static QString stashNameFromMessage(const FilePath &workingDirectory, const QString &message)
{
    // All happy
    if (message.startsWith(stashNamePrefix))
        return message;
    // Retrieve list and find via message
    const QList<Stash> stashes = gitClient().synchronousStashList(workingDirectory);
    for (const Stash &stash : stashes) {
        if (stash.message == message)
            return stash.name;
    }
    //: Look-up of a stash via its descriptive message failed.
    msgCannotRun(workingDirectory, Tr::tr("Cannot resolve stash message \"%1\" in \"%2\".")
                 .arg(message, workingDirectory.toUserOutput()), nullptr);
    return {};
}

bool GitClient::synchronousBranchCmd(const FilePath &workingDirectory, QStringList branchArgs,
                                     QString *output, QString *errorMessage) const
{
    branchArgs.push_front("branch");
    const CommandResult result = vcsSynchronousExec(workingDirectory, branchArgs);
    *output = result.cleanedStdOut();
    if (result.result() == ProcessResult::FinishedWithSuccess)
        return true;
    msgCannotRun(branchArgs, workingDirectory, result.cleanedStdErr(), errorMessage);
    return false;
}

bool GitClient::synchronousTagCmd(const FilePath &workingDirectory, QStringList tagArgs,
                                  QString *output, QString *errorMessage) const
{
    tagArgs.push_front("tag");
    const CommandResult result = vcsSynchronousExec(workingDirectory, tagArgs);
    *output = result.cleanedStdOut();
    if (result.result() == ProcessResult::FinishedWithSuccess)
        return true;
    msgCannotRun(tagArgs, workingDirectory, result.cleanedStdErr(), errorMessage);
    return false;
}

bool GitClient::synchronousForEachRefCmd(const FilePath &workingDirectory, QStringList args,
                                      QString *output, QString *errorMessage) const
{
    args.push_front("for-each-ref");
    const CommandResult result = vcsSynchronousExec(workingDirectory, args, RunFlags::NoOutput);
    *output = result.cleanedStdOut();
    if (result.result() == ProcessResult::FinishedWithSuccess)
        return true;
    msgCannotRun(args, workingDirectory, result.cleanedStdErr(), errorMessage);
    return false;
}

bool GitClient::synchronousRemoteCmd(const FilePath &workingDirectory, QStringList remoteArgs,
                                     QString *output, QString *errorMessage, bool silent) const
{
    remoteArgs.push_front("remote");
    const CommandResult result = vcsSynchronousExec(workingDirectory, remoteArgs,
                                                    silent ? RunFlags::NoOutput : RunFlags::None);
    const QString stdErr = result.cleanedStdErr();
    *errorMessage = stdErr;
    *output = result.cleanedStdOut();

    if (result.result() == ProcessResult::FinishedWithSuccess)
        return true;
    msgCannotRun(remoteArgs, workingDirectory, stdErr, errorMessage);
    return false;
}

QMap<QString,QString> GitClient::synchronousRemotesList(const FilePath &workingDirectory,
                                                        QString *errorMessage) const
{
    QMap<QString,QString> result;

    QString output;
    QString error;
    if (!synchronousRemoteCmd(workingDirectory, {"-v"}, &output, &error, true)) {
        msgCannotRun(workingDirectory, error, errorMessage);
        return result;
    }

    const QStringList remotes = output.split("\n");
    for (const QString &remote : remotes) {
        if (!remote.endsWith(" (push)"))
            continue;

        const int tabIndex = remote.indexOf('\t');
        if (tabIndex == -1)
            continue;
        const QString url = remote.mid(tabIndex + 1, remote.length() - tabIndex - 8);
        result.insert(remote.left(tabIndex), url);
    }
    return result;
}

QStringList GitClient::synchronousSubmoduleStatus(const FilePath &workingDirectory,
                                                  QString *errorMessage) const
{
    // get submodule status
    const CommandResult result = vcsSynchronousExec(workingDirectory, {"submodule", "status"},
                                                    RunFlags::NoOutput);

    if (result.result() != ProcessResult::FinishedWithSuccess) {
        msgCannotRun(workingDirectory, Tr::tr("Cannot retrieve submodule status of \"%1\": %2")
                     .arg(workingDirectory.toUserOutput(), result.cleanedStdErr()), errorMessage);
        return {};
    }
    return splitLines(result.cleanedStdOut());
}

SubmoduleDataMap GitClient::submoduleList(const FilePath &workingDirectory) const
{
    SubmoduleDataMap result;
    FilePath gitmodulesFileName = workingDirectory.pathAppended(".gitmodules");
    if (!gitmodulesFileName.exists())
        return result;

    static QMap<FilePath, SubmoduleDataMap> cachedSubmoduleData;

    if (cachedSubmoduleData.contains(workingDirectory))
        return cachedSubmoduleData.value(workingDirectory);

    const QStringList allConfigs = readConfigValue(workingDirectory, "-l").split('\n');
    const QString submoduleLineStart = "submodule.";
    for (const QString &configLine : allConfigs) {
        if (!configLine.startsWith(submoduleLineStart))
            continue;

        const int nameStart = submoduleLineStart.size();
        const int nameEnd   = configLine.indexOf('.', nameStart);

        const QString submoduleName = configLine.mid(nameStart, nameEnd - nameStart);

        SubmoduleData submoduleData;
        if (result.contains(submoduleName))
            submoduleData = result[submoduleName];

        if (configLine.mid(nameEnd, 5) == ".url=")
            submoduleData.url = configLine.mid(nameEnd + 5);
        else if (configLine.mid(nameEnd, 8) == ".ignore=")
            submoduleData.ignore = configLine.mid(nameEnd + 8);
        else
            continue;

        result.insert(submoduleName, submoduleData);
    }

    // if config found submodules
    if (!result.isEmpty()) {
        QSettings gitmodulesFile(gitmodulesFileName.toUrlishString(), QSettings::IniFormat);

        const QList<QString> submodules = result.keys();
        for (const QString &submoduleName : submodules) {
            gitmodulesFile.beginGroup("submodule \"" + submoduleName + '"');
            const QString path = gitmodulesFile.value("path").toString();
            if (path.isEmpty()) { // invalid submodule entry in config
                result.remove(submoduleName);
            } else {
                SubmoduleData &submoduleRef = result[submoduleName];
                submoduleRef.dir = path;
                const QString ignore = gitmodulesFile.value("ignore").toString();
                if (!ignore.isEmpty() && submoduleRef.ignore.isEmpty())
                    submoduleRef.ignore = ignore;
            }
            gitmodulesFile.endGroup();
        }
    }
    cachedSubmoduleData.insert(workingDirectory, result);

    return result;
}

QByteArray GitClient::synchronousShow(const FilePath &workingDirectory, const QString &id,
                                      RunFlags flags) const
{
    if (!canShow(id)) {
        VcsOutputWindow::appendError(workingDirectory, msgCannotShow(id));
        return {};
    }
    const QStringList arguments = {"show", decorateOption, noColorOption, "--no-patch", id};
    const CommandResult result = vcsSynchronousExec(workingDirectory, arguments, flags);
    if (result.result() != ProcessResult::FinishedWithSuccess) {
        msgCannotRun(arguments, workingDirectory, result.cleanedStdErr(), nullptr);
        return {};
    }
    return result.rawStdOut();
}

// Retrieve list of files to be cleaned
bool GitClient::cleanList(const FilePath &workingDirectory, const QString &modulePath,
                          const QString &flag, QStringList *files, QString *errorMessage)
{
    const FilePath directory = workingDirectory.pathAppended(modulePath);
    const QStringList arguments = {"clean", "--dry-run", flag};

    const CommandResult result = vcsSynchronousExec(directory, arguments, RunFlags::ForceCLocale);
    if (result.result() != ProcessResult::FinishedWithSuccess) {
        msgCannotRun(arguments, directory, result.cleanedStdErr(), errorMessage);
        return false;
    }

    // Filter files that git would remove
    const QString relativeBase = modulePath.isEmpty() ? QString() : modulePath + '/';
    const QString prefix = "Would remove ";
    const QStringList removeLines = Utils::filtered(
                splitLines(result.cleanedStdOut()), [](const QString &s) {
        return s.startsWith("Would remove ");
    });
    *files = Utils::transform(removeLines, [&relativeBase, &prefix](const QString &s) -> QString {
        return relativeBase + s.mid(prefix.size());
    });
    return true;
}

bool GitClient::synchronousCleanList(const FilePath &workingDirectory, const QString &modulePath,
                                     QStringList *files, QStringList *ignoredFiles,
                                     QString *errorMessage)
{
    bool res = cleanList(workingDirectory, modulePath, "-df", files, errorMessage);
    res &= cleanList(workingDirectory, modulePath, "-dXf", ignoredFiles, errorMessage);

    const SubmoduleDataMap submodules = submoduleList(workingDirectory.pathAppended(modulePath));
    for (const SubmoduleData &submodule : submodules) {
        if (submodule.ignore != "all"
                && submodule.ignore != "dirty") {
            const QString submodulePath = modulePath.isEmpty() ? submodule.dir
                                                               : modulePath + '/' + submodule.dir;
            res &= synchronousCleanList(workingDirectory, submodulePath,
                                        files, ignoredFiles, errorMessage);
        }
    }
    return res;
}

bool GitClient::synchronousApplyPatch(const FilePath &workingDirectory,
                                      const QString &file, QString *errorMessage,
                                      const QStringList &extraArguments) const
{
    QStringList arguments = {"apply", "--whitespace=fix"};
    arguments << extraArguments << file;

    const CommandResult result = vcsSynchronousExec(workingDirectory, arguments);
    const QString stdErr = result.cleanedStdErr();
    if (result.result() == ProcessResult::FinishedWithSuccess) {
        if (!stdErr.isEmpty())
            *errorMessage = Tr::tr("There were warnings while applying \"%1\" to \"%2\":\n%3")
                .arg(file, workingDirectory.toUserOutput(), stdErr);
        return true;
    }

    *errorMessage = Tr::tr("Cannot apply patch \"%1\" to \"%2\": %3")
            .arg(QDir::toNativeSeparators(file), workingDirectory.toUserOutput(), stdErr);
    return false;
}

Environment GitClient::processEnvironment(const FilePath &appliedTo) const
{
    Environment environment;
    environment.prependOrSetPath(settings().path());
    if (HostOsInfo::isWindowsHost() && settings().winSetHomeEnvironment()) {
        QString homePath;
        if (qtcEnvironmentVariableIsEmpty("HOMESHARE")) {
            homePath = QDir::toNativeSeparators(QDir::homePath());
        } else {
            homePath = qtcEnvironmentVariable("HOMEDRIVE") + qtcEnvironmentVariable("HOMEPATH");
        }
        environment.set("HOME", homePath);
    }
    environment.set("GIT_EDITOR", m_disableEditor ? "true" : m_gitQtcEditor);
    environment.set("GIT_OPTIONAL_LOCKS", "0");
    return environment.appliedToEnvironment(appliedTo.deviceEnvironment());
}

bool GitClient::beginStashScope(const FilePath &workingDirectory, const QString &command,
                                StashFlag flag, PushAction pushAction)
{
    const FilePath repoDirectory = VcsManager::findTopLevelForDirectory(workingDirectory);
    QTC_ASSERT(!repoDirectory.isEmpty(), return false);
    StashInfo &stashInfo = m_stashInfo[repoDirectory];
    return stashInfo.init(repoDirectory, command, flag, pushAction);
}

void GitClient::endStashScope(const FilePath &workingDirectory)
{
    const FilePath repoDirectory = VcsManager::findTopLevelForDirectory(workingDirectory);
    if (!m_stashInfo.contains(repoDirectory))
        return;
    m_stashInfo[repoDirectory].end();
}

bool GitClient::isValidRevision(const QString &revision) const
{
    if (revision.length() < 1)
        return false;
    for (const auto i : revision)
        if (i != '0')
            return true;
    return false;
}

void GitClient::updateSubmodulesIfNeeded(const FilePath &workingDirectory, bool prompt)
{
    if (!m_updatedSubmodules.isEmpty() || submoduleList(workingDirectory).isEmpty())
        return;

    const QStringList submoduleStatus = synchronousSubmoduleStatus(workingDirectory);
    if (submoduleStatus.isEmpty())
        return;

    bool updateNeeded = false;
    for (const QString &status : submoduleStatus) {
        if (status.startsWith('+')) {
            updateNeeded = true;
            break;
        }
    }
    if (!updateNeeded)
        return;

    if (prompt && QMessageBox::question(ICore::dialogParent(), Tr::tr("Submodules Found"),
            Tr::tr("Would you like to update submodules?"),
            QMessageBox::Yes | QMessageBox::No) == QMessageBox::No) {
        return;
    }

    for (const QString &statusLine : submoduleStatus) {
        // stash only for lines starting with +
        // because only they would be updated
        if (!statusLine.startsWith('+'))
            continue;

        // get submodule name
        const int nameStart  = statusLine.indexOf(' ', 2) + 1;
        const int nameLength = statusLine.indexOf(' ', nameStart) - nameStart;
        const FilePath submoduleDir = workingDirectory.pathAppended(statusLine.mid(nameStart, nameLength));

        if (beginStashScope(submoduleDir, "SubmoduleUpdate")) {
            m_updatedSubmodules.append(submoduleDir);
        } else {
            finishSubmoduleUpdate();
            return;
        }
    }

    enqueueCommand({workingDirectory, {"submodule", "update"},
                    RunFlags::ShowStdOut | RunFlags::ExpectRepoChanges, {}, {},
                    [this](const CommandResult &) { finishSubmoduleUpdate(); }});
}

void GitClient::finishSubmoduleUpdate()
{
    for (const FilePath &submoduleDir : std::as_const(m_updatedSubmodules))
        endStashScope(submoduleDir);
    m_updatedSubmodules.clear();
}

StatusResult GitClient::gitStatus(const FilePath &workingDirectory, StatusMode mode,
                                  QString *output, QString *errorMessage) const
{
    // Run 'status'. Note that git returns exitcode 1 if there are no added files.
    QStringList arguments = {"status"};
    if (mode & NoUntracked)
        arguments << "--untracked-files=no";
    else
        arguments << "--untracked-files=all";
    if (mode & NoSubmodules)
        arguments << "--ignore-submodules=all";
    arguments << "--porcelain" << "-b";

    const CommandResult result = vcsSynchronousExec(workingDirectory, arguments, RunFlags::NoOutput);
    const QString stdOut = result.cleanedStdOut();

    if (output)
        *output = stdOut;

    const bool statusRc = result.result() == ProcessResult::FinishedWithSuccess;
    const bool branchKnown = !stdOut.startsWith("## HEAD (no branch)\n");
    // Is it something really fatal?
    if (!statusRc && !branchKnown) {
        if (errorMessage) {
            *errorMessage = Tr::tr("Cannot obtain status: %1").arg(result.cleanedStdErr());
        }
        return StatusResult::Failed;
    }
    // Unchanged (output text depending on whether -u was passed)
    const bool hasChanges = Utils::contains(stdOut.split('\n'), [](const QString &s) {
                                                return !s.isEmpty() && !s.startsWith('#');
                                            });
    return hasChanges ? StatusResult::Changed : StatusResult::Unchanged;
}

ExecutableItem GitClient::statusTask(const FilePath &workingDirectory, StatusMode mode,
                                     const Storage<StatusResultData> &resultStorage) const
{
    // Run 'status'. Note that git returns exitcode 1 if there are no added files.
    QStringList arguments = {"status"};
    if (mode & NoUntracked)
        arguments << "--untracked-files=no";
    else
        arguments << "--untracked-files=all";
    if (mode & NoSubmodules)
        arguments << "--ignore-submodules=all";
    arguments << "--porcelain" << "-b";

    const auto commandHandler = [resultStorage](const CommandResult &result) {
        StatusResultData &statusResult = *resultStorage;
        statusResult.output = result.cleanedStdOut();

        const bool statusRc = result.result() == ProcessResult::FinishedWithSuccess;
        const bool branchKnown = !statusResult.output.startsWith("## HEAD (no branch)\n");
        // Is it something really fatal?
        if (!statusRc && !branchKnown) {
            statusResult.result = StatusResult::Failed;
            statusResult.errorMessage = Tr::tr("Cannot obtain status: %1").arg(result.cleanedStdErr());
            return;
        }
        // Unchanged (output text depending on whether -u was passed)
        const bool hasChanges = Utils::contains(statusResult.output.split('\n'), [](const QString &s) {
            return !s.isEmpty() && !s.startsWith('#');
        });
        statusResult.result = hasChanges ? StatusResult::Changed : StatusResult::Unchanged;
    };

    return Group {
        resultStorage,
        commandTask({workingDirectory, arguments, RunFlags::NoOutput, {}, {}, commandHandler})
    };
}

QString GitClient::commandInProgressDescription(const FilePath &workingDirectory) const
{
    switch (checkCommandInProgress(workingDirectory)) {
    case NoCommand:
        break;
    case Rebase:
    case RebaseMerge:
        return Tr::tr("REBASING");
    case Revert:
        return Tr::tr("REVERTING");
    case CherryPick:
        return Tr::tr("CHERRY-PICKING");
    case Merge:
        return Tr::tr("MERGING");
    }
    return {};
}

GitClient::CommandInProgress GitClient::checkCommandInProgress(const FilePath &workingDirectory) const
{
    const FilePath gitDir = findGitDirForRepository(workingDirectory);
    if (gitDir.pathAppended("MERGE_HEAD").exists())
        return Merge;
    if (gitDir.pathAppended("rebase-apply").exists())
        return Rebase;
    if (gitDir.pathAppended("rebase-merge").exists())
        return RebaseMerge;
    if (gitDir.pathAppended("REVERT_HEAD").exists())
        return Revert;
    if (gitDir.pathAppended("CHERRY_PICK_HEAD").exists())
        return CherryPick;
    return NoCommand;
}

void GitClient::continueCommandIfNeeded(const FilePath &workingDirectory, bool allowContinue)
{
    if (isCommitEditorOpen())
        return;
    CommandInProgress command = checkCommandInProgress(workingDirectory);
    ContinueCommandMode continueMode;
    if (allowContinue)
        continueMode = command == RebaseMerge ? ContinueOnly : SkipIfNoChanges;
    else
        continueMode = SkipOnly;
    switch (command) {
    case Rebase:
    case RebaseMerge:
        continuePreviousGitCommand(workingDirectory, Tr::tr("Continue Rebase"),
                                   Tr::tr("Rebase is in progress. What do you want to do?"),
                                   Tr::tr("Continue"), "rebase", continueMode);
        break;
    case Merge:
        continuePreviousGitCommand(workingDirectory, Tr::tr("Continue Merge"),
                Tr::tr("You need to commit changes to finish merge.\nCommit now?"),
                Tr::tr("Commit"), "merge", continueMode);
        break;
    case Revert:
        continuePreviousGitCommand(workingDirectory, Tr::tr("Continue Revert"),
                Tr::tr("You need to commit changes to finish revert.\nCommit now?"),
                Tr::tr("Commit"), "revert", continueMode);
        break;
    case CherryPick:
        continuePreviousGitCommand(workingDirectory, Tr::tr("Continue Cherry-Picking"),
                Tr::tr("You need to commit changes to finish cherry-picking.\nCommit now?"),
                Tr::tr("Commit"), "cherry-pick", continueMode);
        break;
    default:
        break;
    }
}

void GitClient::continuePreviousGitCommand(const FilePath &workingDirectory,
                                           const QString &msgBoxTitle, QString msgBoxText,
                                           const QString &buttonName, const QString &gitCommand,
                                           ContinueCommandMode continueMode)
{
    bool isRebase = gitCommand == "rebase";
    bool hasChanges = false;
    switch (continueMode) {
    case ContinueOnly:
        hasChanges = true;
        break;
    case SkipIfNoChanges:
        hasChanges = gitStatus(workingDirectory, StatusMode(NoUntracked | NoSubmodules))
            == StatusResult::Changed;
        if (!hasChanges)
            msgBoxText.prepend(Tr::tr("No changes found.") + ' ');
        break;
    case SkipOnly:
        hasChanges = false;
        break;
    }

    QMessageBox msgBox(QMessageBox::Question, msgBoxTitle, msgBoxText,
                       QMessageBox::NoButton, ICore::dialogParent());
    if (hasChanges || isRebase)
        msgBox.addButton(hasChanges ? buttonName : Tr::tr("Skip"), QMessageBox::AcceptRole);
    msgBox.addButton(QMessageBox::Abort);
    msgBox.addButton(QMessageBox::Ignore);
    switch (msgBox.exec()) {
    case QMessageBox::Ignore:
        break;
    case QMessageBox::Abort:
        synchronousAbortCommand(workingDirectory, gitCommand);
        break;
    default: // Continue/Skip
        if (isRebase)
            rebase(workingDirectory, QLatin1String(hasChanges ? "--continue" : "--skip"));
        else
            startCommit();
    }
}

// Quietly retrieve branch list of remote repository URL
//
// The branch HEAD is pointing to is always returned first.
QStringList GitClient::synchronousRepositoryBranches(const QString &repositoryURL,
                                                     const FilePath &workingDirectory) const
{
    const CommandResult result = vcsSynchronousExec(workingDirectory,
                                 {"ls-remote", repositoryURL, HEAD, "refs/heads/*"},
                                 RunFlags::SuppressStdErr | RunFlags::SuppressFailMessage);
    QStringList branches;
    branches << Tr::tr("<Detached HEAD>");
    QString headHash;
    // split "82bfad2f51d34e98b18982211c82220b8db049b<tab>refs/heads/master"
    bool headFound = false;
    bool branchFound = false;
    const QStringList lines = result.cleanedStdOut().split('\n');
    for (const QString &line : lines) {
        if (line.endsWith("\tHEAD")) {
            QTC_CHECK(headHash.isNull());
            headHash = line.left(line.indexOf('\t'));
            continue;
        }

        const QString pattern = "\trefs/heads/";
        const int pos = line.lastIndexOf(pattern);
        if (pos != -1) {
            branchFound = true;
            const QString branchName = line.mid(pos + pattern.size());
            if (!headFound && line.startsWith(headHash)) {
                branches[0] = branchName;
                headFound = true;
            } else {
                branches.push_back(branchName);
            }
        }
    }
    if (!branchFound)
        branches.clear();
    return branches;
}

void GitClient::launchGitK(const FilePath &workingDirectory, const QString &fileName)
{
    tryLaunchingGitK(processEnvironment(workingDirectory), workingDirectory, fileName);
}

void GitClient::launchRepositoryBrowser(const FilePath &workingDirectory)
{
    const FilePath repBrowserBinary = settings().repositoryBrowserCmd();
    if (!repBrowserBinary.isEmpty())
        Process::startDetached({repBrowserBinary, {workingDirectory.toUrlishString()}}, workingDirectory);
}

static FilePath gitBinDir(const GitClient::GitKLaunchTrial trial, const FilePath &parentDir)
{
    if (trial == GitClient::Bin)
        return parentDir;
    if (trial == GitClient::ParentOfBin) {
        QTC_CHECK(parentDir.fileName() == "bin");
        FilePath foundBinDir = parentDir.parentDir();
        const QString binDirName = foundBinDir.fileName();
        if (binDirName == "usr" || binDirName.startsWith("mingw"))
            foundBinDir = foundBinDir.parentDir();
        return foundBinDir / "cmd";
    }
    if (trial == GitClient::SystemPath)
        return Environment::systemEnvironment().searchInPath("gitk").parentDir();
    QTC_CHECK(false);
    return {};
}

void GitClient::tryLaunchingGitK(const Environment &env,
                                 const FilePath &workingDirectory,
                                 const QString &fileName,
                                 GitClient::GitKLaunchTrial trial) const
{
    const FilePath gitBinDirectory = gitBinDir(trial, vcsBinary(workingDirectory).parentDir());
    FilePath binary = gitBinDirectory.pathAppended("gitk").withExecutableSuffix();
    QStringList arguments;
    if (HostOsInfo::isWindowsHost()) {
        // If git/bin is in path, use 'wish' shell to run. Otherwise (git/cmd), directly run gitk
        const FilePath wish = gitBinDirectory.pathAppended("wish").withExecutableSuffix();
        if (wish.withExecutableSuffix().exists()) {
            arguments << binary.toUrlishString();
            binary = wish;
        }
    }
    const QString gitkOpts = settings().gitkOptions();
    if (!gitkOpts.isEmpty())
        arguments.append(ProcessArgs::splitArgs(gitkOpts, HostOsInfo::hostOs()));
    if (!fileName.isEmpty())
        arguments << "--" << fileName;
    VcsOutputWindow::appendCommand(workingDirectory, {binary, arguments});

    // This should always use Process::startDetached (as not to kill
    // the child), but that does not have an environment parameter.
    if (!settings().path().isEmpty()) {
        auto process = new Process(const_cast<GitClient*>(this));
        process->setWorkingDirectory(workingDirectory);
        process->setEnvironment(env);
        process->setCommand({binary, arguments});
        connect(process, &Process::done, this,
                [this, process, env, workingDirectory, fileName, trial, gitBinDirectory] {
            if (process->result() == ProcessResult::StartFailed)
                handleGitKFailedToStart(env, workingDirectory, fileName, trial, gitBinDirectory);
            process->deleteLater();
        });
        process->start();
    } else {
        if (!Process::startDetached({binary, arguments}, workingDirectory))
            handleGitKFailedToStart(env, workingDirectory, fileName, trial, gitBinDirectory);
    }
}

void GitClient::handleGitKFailedToStart(const Environment &env,
                                        const FilePath &workingDirectory,
                                        const QString &fileName,
                                        const GitClient::GitKLaunchTrial oldTrial,
                                        const FilePath &oldGitBinDir) const
{
    QTC_ASSERT(oldTrial != None, return);
    VcsOutputWindow::appendSilently(workingDirectory, msgCannotLaunch(oldGitBinDir / "gitk"));

    GitKLaunchTrial nextTrial = None;

    if (oldTrial == Bin && vcsBinary(workingDirectory).parentDir().fileName() == "bin") {
        nextTrial = ParentOfBin;
    } else if (oldTrial != SystemPath
               && !Environment::systemEnvironment().searchInPath("gitk").isEmpty()) {
        nextTrial = SystemPath;
    }

    if (nextTrial == None) {
        VcsOutputWindow::appendError(workingDirectory, msgCannotLaunch("gitk"));
        return;
    }

    tryLaunchingGitK(env, workingDirectory, fileName, nextTrial);
}

bool GitClient::launchGitGui(const FilePath &workingDirectory)
{
    const QString cannotLaunchGitGui = msgCannotLaunch("git gui");
    FilePath gitBinary = vcsBinary(workingDirectory);
    if (gitBinary.isEmpty()) {
        VcsOutputWindow::appendError(workingDirectory, cannotLaunchGitGui);
        return false;
    }

    auto process = new Process(const_cast<GitClient *>(this));
    process->setWorkingDirectory(workingDirectory);
    process->setCommand({gitBinary, {"gui"}});
    connect(process, &Process::done, this, [process, cannotLaunchGitGui] {
        if (process->result() == ProcessResult::StartFailed) {
            const QString errorMessage = process->readAllStandardError();
            VcsOutputWindow::appendError(process->workingDirectory(), cannotLaunchGitGui);
            VcsOutputWindow::appendError(process->workingDirectory(), errorMessage);
        }
        process->deleteLater();
    });
    process->start();
    return true;
}

FilePath GitClient::gitBinDirectory() const
{
    const QString git = vcsBinary({}).toUrlishString();
    if (git.isEmpty())
        return {};

    // Is 'git\cmd' in the path (folder containing .bats)?
    QString path = QFileInfo(git).absolutePath();
    // Git for Windows has git and gitk redirect executables in {setup dir}/cmd
    // and the real binaries are in {setup dir}/bin. If cmd is configured in PATH
    // or in Git settings, return bin instead.
    if (HostOsInfo::isWindowsHost()) {
        if (path.endsWith("/cmd", Qt::CaseInsensitive))
            path.replace(path.size() - 3, 3, "bin");
        if (path.endsWith("/bin", Qt::CaseInsensitive)
                && !path.endsWith("/usr/bin", Qt::CaseInsensitive)) {
            // Legacy msysGit used Git/bin for additional tools.
            // Git for Windows uses Git/usr/bin. Prefer that if it exists.
            QString usrBinPath = path;
            usrBinPath.replace(usrBinPath.size() - 3, 3, "usr/bin");
            if (QFileInfo::exists(usrBinPath))
                path = usrBinPath;
        }
    }
    return FilePath::fromString(path);
}

bool GitClient::launchGitBash(const FilePath &workingDirectory)
{
    bool success = true;
    const FilePath git = vcsBinary(workingDirectory);

    if (git.isEmpty()) {
        success = false;
    } else {
        const FilePath gitBash = git.absolutePath().parentDir() / "git-bash.exe";
        success = Process::startDetached(CommandLine{gitBash}, workingDirectory);
    }

    if (!success)
        VcsOutputWindow::appendError(workingDirectory, msgCannotLaunch("git-bash"));

    return success;
}

FilePath GitClient::vcsBinary(const FilePath &forDirectory) const
{
    if (!forDirectory.isLocal()) {
        auto it = m_gitExecutableCache.find(forDirectory.withNewPath({}));
        if (it == m_gitExecutableCache.end()) {
            const FilePath gitBin = forDirectory.withNewPath("git").searchInPath();
            it = m_gitExecutableCache.insert(forDirectory.withNewPath({}),
                                             gitBin.isExecutableFile() ? gitBin : FilePath{});
        }

        return it.value();
    }
    return settings().gitExecutable().value_or(FilePath{});
}

// returns first line from log and removes it
static QByteArray shiftLogLine(QByteArray &logText)
{
    const int index = logText.indexOf('\n');
    const QByteArray res = logText.left(index);
    logText.remove(0, index + 1);
    return res;
}

Result<CommitData> GitClient::enrichCommitData(const FilePath &repoDirectory,
                                               const QString &commit,
                                               const CommitData &commitDataIn)
{
    // Get commit data as "hash<lf>author<lf>email<lf>message".
    const QStringList arguments = {"log", "--max-count=1", "--pretty=format:%h\n%aN\n%aE\n%B", commit};
    const CommandResult result = vcsSynchronousExec(repoDirectory, arguments, RunFlags::NoOutput);

    if (result.result() != ProcessResult::FinishedWithSuccess) {
        return ResultError(Tr::tr("Cannot retrieve last commit data of repository \"%1\".")
                           .arg(repoDirectory.toUserOutput()));
    }

    CommitData commitData = commitDataIn;
    const TextEncoding authorEncoding = HostOsInfo::isWindowsHost()
            ? TextEncoding::Utf8
            : TextEncoding(commitData.commitEncoding);
    QByteArray stdOut = result.rawStdOut();
    commitData.amendHash = QLatin1String(shiftLogLine(stdOut));
    commitData.panelData.author = authorEncoding.decode(shiftLogLine(stdOut));
    commitData.panelData.email = authorEncoding.decode(shiftLogLine(stdOut));
    commitData.commitTemplate = TextEncoding(commitData.commitEncoding).decode(stdOut);
    return commitData;
}

Author GitClient::parseAuthor(const QString &authorInfo)
{
    // The format is:
    // Joe Developer <joedev@example.com> unixtimestamp +HHMM
    int lt = authorInfo.lastIndexOf('<');
    int gt = authorInfo.lastIndexOf('>');
    if (gt == -1 || uint(lt) > uint(gt)) {
        // shouldn't happen!
        return {};
    }

    const Author result {authorInfo.left(lt - 1), authorInfo.mid(lt + 1, gt - lt - 1)};
    return result;
}

Author GitClient::getAuthor(const Utils::FilePath &workingDirectory)
{
    const QString authorInfo = readGitVar(workingDirectory, "GIT_AUTHOR_IDENT");
    return parseAuthor(authorInfo);
}

Result<CommitData> GitClient::getCommitData(CommitType commitType, const FilePath &workingDirectory)
{
    CommitData commitData(commitType);

    // Find repo
    const FilePath repoDirectory = VcsManager::findTopLevelForDirectory(workingDirectory);
    if (repoDirectory.isEmpty())
        return ResultError(msgRepositoryNotFound(workingDirectory));

    commitData.panelInfo.repository = repoDirectory;

    const FilePath gitDir = findGitDirForRepository(repoDirectory);
    if (gitDir.isEmpty()) {
        return ResultError(Tr::tr("The repository \"%1\" is not initialized.")
            .arg(repoDirectory.toUserOutput()));
    }

    // Run status. Note that it has exitcode 1 if there are no added files.
    QString errorMessage;
    QString output;
    if (commitData.commitType == FixupCommit) {
        const Result<QString> res = synchronousLog(repoDirectory, {HEAD, "--not", "--remotes", "-n1"},
                                                   RunFlags::SuppressCommandLogging);
        if (res)
            output = res.value();
        else
            errorMessage = res.error();
        if (output.isEmpty())
            return ResultError(msgNoCommits(false));
    } else {
        commitData.commentChar = commentChar(repoDirectory);
    }

    const StatusResult status = gitStatus(repoDirectory, ShowAll, &output, &errorMessage);
    switch (status) {
    case StatusResult::Changed:
        break;
    case StatusResult::Unchanged:
        if (commitData.commitType == AmendCommit) // amend might be run just for the commit message
            break;
        return ResultError(msgNoChangedFiles());
    case StatusResult::Failed:
        return ResultError(errorMessage);
    }

    //    Output looks like:
    //    ## branch_name
    //    MM filename
    //     A new_unstaged_file
    //    R  old -> new
    //     D deleted_file
    //    ?? untracked_file
    if (!commitData.parseFilesFromStatus(output))
        return ResultError(msgParseFilesFailed());

    if (status != StatusResult::Unchanged) {
        // Filter out untracked files that are not part of the project
        QStringList untrackedFiles = commitData.filterFiles(UntrackedFile);

        VcsBaseSubmitEditor::filterUntrackedFilesOfProject(repoDirectory, &untrackedFiles);
        QList<CommitData::StateFilePair> filteredFiles;
        QList<CommitData::StateFilePair>::const_iterator it = commitData.files.constBegin();
        for ( ; it != commitData.files.constEnd(); ++it) {
            if (it->first == UntrackedFile && !untrackedFiles.contains(it->second))
                continue;
            filteredFiles.append(*it);
        }
        commitData.files = filteredFiles;

        if (commitData.files.isEmpty() && commitData.commitType != AmendCommit)
            return ResultError(msgNoChangedFiles());
    }

    commitData.commitEncoding = encoding(EncodingCommit, workingDirectory);

    // Get the commit template or the last commit message
    switch (commitData.commitType) {
    case AmendCommit: {
        if (const Result<CommitData> res = enrichCommitData(repoDirectory, HEAD, commitData))
            commitData = res.value();
        else
            return ResultError(res.error());
        break;
    }
    case SimpleCommit: {
        bool authorFromCherryPick = false;
        // For cherry-picked commit, read author data from the commit (but template from MERGE_MSG)
        if (gitDir.pathAppended(CHERRY_PICK_HEAD).exists()) {
            if (const Result<CommitData> res = enrichCommitData(repoDirectory, CHERRY_PICK_HEAD, commitData)) {
                authorFromCherryPick = true;
                commitData = res.value();
            }
            commitData.amendHash.clear();
        }
        if (!authorFromCherryPick) {
            const Author author = getAuthor(workingDirectory);
            commitData.panelData.author = author.name;
            commitData.panelData.email = author.email;
        }
        // Commit: Get the commit template
        FilePath templateFile = gitDir / "MERGE_MSG";
        if (!templateFile.exists())
            templateFile = gitDir / "SQUASH_MSG";
        if (!templateFile.exists()) {
            templateFile = FilePath::fromUserInput(
                readConfigValue(workingDirectory, "commit.template"));
        }
        if (!templateFile.isEmpty()) {
            templateFile = repoDirectory.resolvePath(templateFile);
            const Result<QByteArray> res = templateFile.fileContents();
            if (!res)
                return ResultError(res.error());
            commitData.commitTemplate = QString::fromLocal8Bit(normalizeNewlines(*res));
        }
        break;
    }
    case FixupCommit:
        break;
    }

    commitData.enablePush = !synchronousRemotesList(repoDirectory).isEmpty();
    if (commitData.enablePush) {
        CommandInProgress commandInProgress = checkCommandInProgress(repoDirectory);
        if (commandInProgress == Rebase || commandInProgress == RebaseMerge)
            commitData.enablePush = false;
    }

    return commitData;
}

// Log message for commits/amended commits to go to output window
static inline QString msgCommitted(const QString &amendHash, int fileCount)
{
    if (amendHash.isEmpty())
        return Tr::tr("Committed %n files.", nullptr, fileCount);
    if (fileCount)
        return Tr::tr("Amended \"%1\" (%n files).", nullptr, fileCount).arg(amendHash);
    return Tr::tr("Amended \"%1\".").arg(amendHash);
}

bool GitClient::addAndCommit(const FilePath &repositoryDirectory,
                             const GitSubmitEditorPanelData &data,
                             CommitType commitType,
                             const QString &amendHash,
                             const FilePath &messageFile,
                             SubmitFileModel *model)
{
    const QString renameSeparator = " -> ";

    QStringList filesToAdd;
    QStringList filesToRemove;
    QStringList filesToReset;

    int commitCount = 0;

    for (int i = 0; i < model->rowCount(); ++i) {
        const FileStates state = static_cast<FileStates>(model->extraData(i).toInt());
        const QString file = model->file(i);
        const bool checked = model->checked(i);

        if (checked)
            ++commitCount;

        if (state == UntrackedFile && checked)
            filesToAdd.append(file);

        if ((state & StagedFile) && !checked) {
            if (state & (ModifiedFile | AddedFile | DeletedFile | TypeChangedFile)) {
                filesToReset.append(file);
            } else if (state & (RenamedFile | CopiedFile)) {
                const QString newFile = file.mid(file.indexOf(renameSeparator) + renameSeparator.size());
                filesToReset.append(newFile);
            }
        } else if (state & UnmergedFile && checked) {
            QTC_ASSERT(false, continue); // There should not be unmerged files when committing!
        }

        if ((state == ModifiedFile || state == TypeChangedFile) && checked) {
            filesToReset.removeAll(file);
            filesToAdd.append(file);
        } else if (state == AddedFile && checked) {
            filesToAdd.append(file);
        } else if (state == DeletedFile && checked) {
            filesToReset.removeAll(file);
            filesToRemove.append(file);
        } else if (state == RenamedFile && checked) {
            QTC_ASSERT(false, continue); // git mv directly stages.
        } else if (state == CopiedFile && checked) {
            QTC_ASSERT(false, continue); // only is noticed after adding a new file to the index
        } else if (state == UnmergedFile && checked) {
            QTC_ASSERT(false, continue); // There should not be unmerged files when committing!
        }
    }

    if (!filesToReset.isEmpty() && !synchronousReset(repositoryDirectory, filesToReset))
        return false;

    if (!filesToRemove.isEmpty() && !synchronousDelete(repositoryDirectory, true, filesToRemove))
        return false;

    if (!filesToAdd.isEmpty() && !synchronousAdd(repositoryDirectory, filesToAdd))
        return false;

    // Do the final commit
    QStringList arguments = {"commit"};
    if (commitType == FixupCommit) {
        arguments << "--fixup" << amendHash;
    } else {
        arguments << "-F" << messageFile.nativePath();
        if (commitType == AmendCommit)
            arguments << "--amend";
        const QString &authorString =  data.authorString();
        if (!authorString.isEmpty())
             arguments << "--author" << authorString;
        if (data.bypassHooks)
            arguments << "--no-verify";
        if (data.signOff)
            arguments << "--signoff";
    }

    const CommandResult result = vcsSynchronousExec(repositoryDirectory, arguments,
                                                    RunFlags::UseEventLoop);
    if (result.result() == ProcessResult::FinishedWithSuccess) {
        VcsOutputWindow::appendMessage(repositoryDirectory, msgCommitted(amendHash, commitCount));
        updateCurrentBranch();
        return true;
    }
    VcsOutputWindow::appendError(repositoryDirectory,
                                 Tr::tr("Cannot commit %n file(s).", nullptr, commitCount) + "\n");
    return false;
}

/**
 * Formats the patches given in \a patchRange as multiple singe file patches.
 *
 * The format for \a patchRange is {"-n", "hash"} where `n` specifies the
 * number of commits before `hash`.
 */
void GitClient::formatPatch(const Utils::FilePath &workingDirectory, const QStringList &patchRange)
{
    if (patchRange.isEmpty())
        return;

    const QStringList args = {"format-patch"};
    enqueueCommand({workingDirectory, args + patchRange, RunFlags::ShowSuccessMessage});
}

/* Revert: This function can be called with a file list (to revert single
 * files)  or a single directory (revert all). Qt Creator currently has only
 * 'revert single' in its VCS menus, but the code is prepared to deal with
 * reverting a directory pending a sophisticated selection dialog in the
 * VcsBase plugin. */
GitClient::RevertResult GitClient::revertI(QStringList files,
                                           bool *ptrToIsDirectory,
                                           QString *errorMessage,
                                           bool revertStaging,
                                           FilePath *repository)
{
    if (files.empty())
        return RevertCanceled;

    // Figure out the working directory
    const QFileInfo firstFile(files.front());
    const bool isDirectory = firstFile.isDir();
    if (ptrToIsDirectory)
        *ptrToIsDirectory = isDirectory;
    const FilePath workingDirectory =
        FilePath::fromString(isDirectory ? firstFile.absoluteFilePath() : firstFile.absolutePath());

    const FilePath repoDirectory = VcsManager::findTopLevelForDirectory(workingDirectory);
    *repository = repoDirectory;
    if (repoDirectory.isEmpty()) {
        *errorMessage = msgRepositoryNotFound(workingDirectory);
        return RevertFailed;
    }

    // Check for changes
    QString output;
    switch (gitStatus(repoDirectory, StatusMode(NoUntracked | NoSubmodules), &output, errorMessage)) {
    case StatusResult::Changed:
        break;
    case StatusResult::Unchanged:
        return RevertUnchanged;
    case StatusResult::Failed:
        return RevertFailed;
    }
    CommitData data;
    if (!data.parseFilesFromStatus(output)) {
        *errorMessage = msgParseFilesFailed();
        return RevertFailed;
    }

    // If we are looking at files, make them relative to the repository
    // directory to match them in the status output list.
    if (!isDirectory) {
        const QDir repoDir(repoDirectory.toUrlishString());
        const QStringList::iterator cend = files.end();
        for (QStringList::iterator it = files.begin(); it != cend; ++it)
            *it = repoDir.relativeFilePath(*it);
    }

    // From the status output, determine all modified [un]staged files.
    const QStringList allStagedFiles = data.filterFiles(StagedFile | ModifiedFile);
    const QStringList allUnstagedFiles = data.filterFiles(ModifiedFile);
    // Unless a directory was passed, filter all modified files for the
    // argument file list.
    QStringList stagedFiles = allStagedFiles;
    QStringList unstagedFiles = allUnstagedFiles;
    if (!isDirectory) {
        const QSet<QString> filesSet = Utils::toSet(files);
        stagedFiles = Utils::toList(Utils::toSet(allStagedFiles).intersect(filesSet));
        unstagedFiles = Utils::toList(Utils::toSet(allUnstagedFiles).intersect(filesSet));
    }
    if ((!revertStaging || stagedFiles.empty()) && unstagedFiles.empty())
        return RevertUnchanged;

    // Ask to revert (to do: Handle lists with a selection dialog)
    const QMessageBox::StandardButton answer
        = QMessageBox::question(ICore::dialogParent(),
                                Tr::tr("Revert"),
                                Tr::tr("The file has been changed. Do you want to revert it?"),
                                QMessageBox::Yes | QMessageBox::No,
                                QMessageBox::No);
    if (answer == QMessageBox::No)
        return RevertCanceled;

    // Unstage the staged files
    if (revertStaging && !stagedFiles.empty() && !synchronousReset(repoDirectory, stagedFiles, errorMessage))
        return RevertFailed;
    QStringList filesToRevert = unstagedFiles;
    if (revertStaging)
        filesToRevert += stagedFiles;
    // Finally revert!
    if (!synchronousCheckoutFiles(repoDirectory, filesToRevert, QString(), errorMessage, revertStaging))
        return RevertFailed;
    return RevertOk;
}

void GitClient::revertFiles(const QStringList &files, bool revertStaging)
{
    bool isDirectory;
    QString errorMessage;
    FilePath repository;
    switch (revertI(files, &isDirectory, &errorMessage, revertStaging, &repository)) {
    case RevertOk:
        emitFilesChanged(FilePaths::fromStrings(files));
        break;
    case RevertCanceled:
        break;
    case RevertUnchanged: {
        const QString msg = (isDirectory || files.size() > 1) ? msgNoChangedFiles() : Tr::tr("The file is not modified.");
        VcsOutputWindow::appendWarning(repository, msg);
    }
        break;
    case RevertFailed:
        VcsOutputWindow::appendError(repository, errorMessage);
        break;
    }
}

void GitClient::fetch(const FilePath &workingDirectory, const QString &remote)
{
    const QStringList arguments{"fetch", (remote.isEmpty() ? "--all" : remote)};
    const auto commandHandler = [workingDirectory](const CommandResult &result) {
        if (result.result() == ProcessResult::FinishedWithSuccess)
            updateBranches(workingDirectory);
    };
    enqueueCommand({workingDirectory, arguments,
                    RunFlags::ShowStdOut | RunFlags::ShowSuccessMessage, {}, {}, commandHandler});
}

bool GitClient::executeAndHandleConflicts(const FilePath &workingDirectory,
                                          const QStringList &arguments,
                                          const QString &abortCommand) const
{
    const RunFlags flags = RunFlags::ShowStdOut
                         | RunFlags::ExpectRepoChanges
                         | RunFlags::ShowSuccessMessage;
    const CommandResult result = vcsSynchronousExec(workingDirectory, arguments, flags);
    // Notify about changed files or abort the rebase.
    handleConflictResponse(result, workingDirectory, abortCommand);
    return result.result() == ProcessResult::FinishedWithSuccess;
}

void GitClient::pull(const FilePath &workingDirectory, bool rebase)
{
    QString abortCommand;
    QStringList arguments = {"pull"};
    if (rebase) {
        arguments << "--rebase";
        abortCommand = "rebase";
    } else {
        abortCommand = "merge";
    }

    const auto commandHandler = [this, workingDirectory](const CommandResult &result) {
        if (result.result() == ProcessResult::FinishedWithSuccess)
            updateSubmodulesIfNeeded(workingDirectory, true);
    };
    vcsExecAbortable(workingDirectory, arguments, rebase, abortCommand, commandHandler);
}

void GitClient::synchronousAbortCommand(const FilePath &workingDir, const QString &abortCommand)
{
    // Abort to clean if something goes wrong
    if (abortCommand.isEmpty()) {
        // no abort command - checkout index to clean working copy.
        synchronousCheckoutFiles(VcsManager::findTopLevelForDirectory(workingDir),
                                 {}, {}, nullptr, false);
        return;
    }

    const CommandResult result = vcsSynchronousExec(workingDir, {abortCommand, "--abort"},
                                 RunFlags::ExpectRepoChanges | RunFlags::ShowSuccessMessage);
    VcsOutputWindow::appendSilently(workingDir, result.cleanedStdOut());
}

QString GitClient::synchronousTrackingBranch(const FilePath &workingDirectory, const QString &branch)
{
    QString remote;
    QString localBranch = branch.isEmpty() ? synchronousCurrentLocalBranch(workingDirectory) : branch;
    if (localBranch.isEmpty())
        return {};
    localBranch.prepend("branch.");
    remote = readConfigValue(workingDirectory, localBranch + ".remote");
    if (remote.isEmpty())
        return {};
    const QString rBranch = readConfigValue(workingDirectory, localBranch + ".merge")
            .replace("refs/heads/", QString());
    if (rBranch.isEmpty())
        return {};
    return remote + '/' + rBranch;
}

bool GitClient::synchronousSetTrackingBranch(const FilePath &workingDirectory,
                                             const QString &branch, const QString &tracking)
{
    const CommandResult result = vcsSynchronousExec(workingDirectory,
                                 {"branch", "--set-upstream-to=" + tracking, branch});
    return result.result() == ProcessResult::FinishedWithSuccess;
}

void GitClient::handleMergeConflicts(const FilePath &workingDir, const QString &commit,
                                     const QStringList &files, const QString &abortCommand)
{
    QString message;
    if (!commit.isEmpty()) {
        message = Tr::tr("Conflicts detected with commit %1.").arg(commit);
    } else if (!files.isEmpty()) {
        QString fileList;
        QStringList partialFiles = files;
        while (partialFiles.count() > 20)
            partialFiles.removeLast();
        fileList = partialFiles.join('\n');
        if (partialFiles.count() != files.count())
            fileList += "\n...";
        message = Tr::tr("Conflicts detected with files:\n%1").arg(fileList);
    } else {
        message = Tr::tr("Conflicts detected.");
    }
    QMessageBox mergeOrAbort(QMessageBox::Question, Tr::tr("Conflicts Detected"), message,
                             QMessageBox::NoButton, ICore::dialogParent());
    QPushButton *mergeToolButton = mergeOrAbort.addButton(Tr::tr("Run &Merge Tool"),
                                                          QMessageBox::AcceptRole);
    const QString mergeTool = readConfigValue(workingDir, "merge.tool");
    if (mergeTool.isEmpty() || mergeTool.startsWith("vimdiff")) {
        mergeToolButton->setEnabled(false);
        mergeToolButton->setToolTip(Tr::tr("Only graphical merge tools are supported. "
                                       "Please configure merge.tool."));
    }
    mergeOrAbort.addButton(QMessageBox::Ignore);
    if (abortCommand == "rebase")
        mergeOrAbort.addButton(Tr::tr("&Skip"), QMessageBox::RejectRole);
    if (!abortCommand.isEmpty())
        mergeOrAbort.addButton(QMessageBox::Abort);
    switch (mergeOrAbort.exec()) {
    case QMessageBox::Abort:
        synchronousAbortCommand(workingDir, abortCommand);
        break;
    case QMessageBox::Ignore:
        break;
    default: // Merge or Skip
        if (mergeOrAbort.clickedButton() == mergeToolButton)
            merge(workingDir);
        else if (!abortCommand.isEmpty())
            executeAndHandleConflicts(workingDir, {abortCommand, "--skip"}, abortCommand);
    }
}

// Subversion: git svn
void GitClient::subversionFetch(const FilePath &workingDirectory)
{
    enqueueCommand({workingDirectory, {"svn", "fetch"},
                    RunFlags::ShowStdOut | RunFlags::ShowSuccessMessage});
}

void GitClient::subversionLog(const FilePath &workingDirectory)
{
    QStringList arguments = {"svn", "log"};
    int logCount = settings().logCount();
    if (logCount > 0)
         arguments << ("--limit=" + QString::number(logCount));

    // Create a command editor, no highlighting or interaction.
    const QString title = Tr::tr("Git SVN Log");
    const Id editorId = Git::Constants::GIT_SVN_LOG_EDITOR_ID;
    const FilePath sourceFile = VcsBaseEditor::getSource(workingDirectory, QStringList());
    VcsBaseEditorWidget *editor = createVcsEditor(editorId, title, sourceFile, encoding(EncodingDefault),
                                                  "svnLog", sourceFile.toUrlishString());
    editor->setWorkingDirectory(workingDirectory);
    executeInEditor(workingDirectory, arguments, editor);
}

void GitClient::subversionDeltaCommit(const FilePath &workingDirectory)
{
    enqueueCommand({workingDirectory, {"svn", "dcommit"},
                    RunFlags::ShowStdOut | RunFlags::ShowSuccessMessage});
}

enum class PushFailure { Unknown, NonFastForward, NoRemoteBranch };

static PushFailure handleError(const QString &text, QString *pushFallbackCommand)
{
    if (text.contains("non-fast-forward"))
        return PushFailure::NonFastForward;

    if (text.contains("has no upstream branch")) {
        const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            /* Extract the suggested command from the git output which
                 * should be similar to the following:
                 *
                 *     git push --set-upstream origin add_set_upstream_dialog
                 */
            const QString trimmedLine = line.trimmed();
            if (trimmedLine.startsWith("git push")) {
                *pushFallbackCommand = trimmedLine;
                break;
            }
        }
        return PushFailure::NoRemoteBranch;
    }
    return PushFailure::Unknown;
};

void GitClient::push(const FilePath &workingDirectory, const QStringList &pushArgs)
{
    const auto commandHandler = [this, workingDirectory, pushArgs](const CommandResult &result) {
        QString pushFallbackCommand;
        const PushFailure pushFailure = handleError(result.cleanedStdErr(),
                                                    &pushFallbackCommand);
        if (result.result() == ProcessResult::FinishedWithSuccess) {
            updateCurrentBranch();
            return;
        }
        if (pushFailure == PushFailure::Unknown)
            return;

        if (pushFailure == PushFailure::NonFastForward) {
            const QColor warnColor = Utils::creatorColor(Theme::TextColorError);
            if (QMessageBox::question(
                    Core::ICore::dialogParent(), Tr::tr("Force Push"),
                    Tr::tr("Push failed. Would you like to force-push <span style=\"color:#%1\">"
                           "(rewrites remote history)</span>?")
                        .arg(QString::number(warnColor.rgba(), 16)),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No) != QMessageBox::Yes) {
                return;
            }
            const auto commandHandler = [](const CommandResult &result) {
                if (result.result() == ProcessResult::FinishedWithSuccess)
                    updateCurrentBranch();
            };
            enqueueCommand({workingDirectory,
                            QStringList{"push", "--force-with-lease"} + pushArgs,
                            RunFlags::ShowStdOut | RunFlags::ShowSuccessMessage, {}, {},
                            commandHandler});
            return;
        }
        // NoRemoteBranch case
        if (QMessageBox::question(
                Core::ICore::dialogParent(), Tr::tr("No Upstream Branch"),
                Tr::tr("Push failed because the local branch \"%1\" "
                       "does not have an upstream branch on the remote.\n\n"
                       "Would you like to create the branch \"%1\" on the "
                       "remote and set it as upstream?")
                    .arg(synchronousCurrentLocalBranch(workingDirectory)),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        const QStringList fallbackCommandParts = pushFallbackCommand.split(' ', Qt::SkipEmptyParts);
        const auto commandHandler = [workingDirectory](const CommandResult &result) {
            if (result.result() == ProcessResult::FinishedWithSuccess)
                updateBranches(workingDirectory);
        };
        enqueueCommand({workingDirectory, fallbackCommandParts.mid(1),
                        RunFlags::ShowStdOut | RunFlags::ShowSuccessMessage, {}, {}, commandHandler});
    };
    enqueueCommand({workingDirectory, QStringList({"push"}) + pushArgs,
                    RunFlags::ShowStdOut | RunFlags::ShowSuccessMessage, {}, {}, commandHandler});
}

bool GitClient::synchronousMerge(const FilePath &workingDirectory, const QString &branch,
                                 bool allowFastForward)
{
    const QString command = "merge";
    QStringList arguments = {command};
    if (!allowFastForward)
        arguments << "--no-ff";
    arguments << branch;
    return executeAndHandleConflicts(workingDirectory, arguments, command);
}

bool GitClient::canRebase(const FilePath &workingDirectory) const
{
    const FilePath gitDir = findGitDirForRepository(workingDirectory);
    if (gitDir.pathAppended("rebase-apply").exists()
            || gitDir.pathAppended("rebase-merge").exists()) {
        VcsOutputWindow::appendError(workingDirectory,
                    Tr::tr("Rebase, merge or am is in progress. Finish "
                       "or abort it and then try again."));
        return false;
    }
    return true;
}

void GitClient::rebase(const FilePath &workingDirectory, const QString &argument)
{
    vcsExecAbortable(workingDirectory, {"rebase", argument}, true);
}

void GitClient::cherryPick(const FilePath &workingDirectory, const QString &argument)
{
    vcsExecAbortable(workingDirectory, {"cherry-pick", argument});
}

void GitClient::revert(const FilePath &workingDirectory, const QString &argument)
{
    vcsExecAbortable(workingDirectory, {"revert", argument});
}

// Executes a command asynchronously. Work tree is expected to be clean.
// Stashing is handled prior to this call.
void GitClient::vcsExecAbortable(const FilePath &workingDirectory, const QStringList &arguments,
                                 bool isRebase, const QString &abortCommand,
                                 const CommandHandler &handler)
{
    QTC_ASSERT(!arguments.isEmpty(), return);
    const QString abortString = abortCommand.isEmpty() ? arguments.at(0) : abortCommand;
    const ProgressParser progressParser = isRebase ? GitProgressParser() : ProgressParser();
    enqueueCommand({workingDirectory, arguments, RunFlags::ShowStdOut | RunFlags::ShowSuccessMessage,
                    progressParser, {},
                    [workingDirectory, abortString, handler](const CommandResult &result) {
                        handleConflictResponse(result, workingDirectory, abortString);
                        if (handler)
                            handler(result);
                    }});
}

bool GitClient::synchronousRevert(const FilePath &workingDirectory, const QString &commit)
{
    const QString command = "revert";
    // Do not stash if --continue or --abort is given as the commit
    if (!commit.startsWith('-') && !beginStashScope(workingDirectory, command))
        return false;
    return executeAndHandleConflicts(workingDirectory, {command, "--no-edit", commit}, command);
}

bool GitClient::synchronousCherryPick(const FilePath &workingDirectory, const QString &commit)
{
    const QString command = "cherry-pick";
    // "commit" might be --continue or --abort
    const bool isRealCommit = !commit.startsWith('-');
    if (isRealCommit && !beginStashScope(workingDirectory, command))
        return false;

    QStringList arguments = {command};
    if (isRealCommit && isRemoteCommit(workingDirectory, commit))
        arguments << "-x";
    arguments << commit;

    return executeAndHandleConflicts(workingDirectory, arguments, command);
}

void GitClient::interactiveRebase(const FilePath &workingDirectory, const QString &commit, bool fixup)
{
    QStringList arguments = {"rebase", "-i"};
    if (fixup)
        arguments << "--autosquash";
    arguments << commit + '^';
    if (fixup)
        m_disableEditor = true;
    vcsExecAbortable(workingDirectory, arguments, true);
    if (fixup)
        m_disableEditor = false;
}

QString GitClient::msgNoChangedFiles()
{
    return Tr::tr("There are no modified files.");
}

QString GitClient::msgNoCommits(bool includeRemote)
{
    return includeRemote ? Tr::tr("No commits were found") : Tr::tr("No local commits were found");
}

void GitClient::stashPop(const FilePath &workingDirectory, const QString &stash)
{
    QStringList arguments = {"stash", "pop"};
    if (!stash.isEmpty())
        arguments << stash;
    const auto commandHandler = [workingDirectory](const CommandResult &result) {
        handleConflictResponse(result, workingDirectory);
    };
    enqueueCommand({workingDirectory, arguments,
                    RunFlags::ShowStdOut | RunFlags::ExpectRepoChanges, {}, {}, commandHandler});
}

bool GitClient::synchronousStashRestore(const FilePath &workingDirectory,
                                        const QString &stash,
                                        bool pop,
                                        const QString &branch /* = QString()*/) const
{
    QStringList arguments = {"stash"};
    if (branch.isEmpty())
        arguments << QLatin1String(pop ? "pop" : "apply") << stash;
    else
        arguments << "branch" << branch << stash;
    return executeAndHandleConflicts(workingDirectory, arguments);
}

bool GitClient::synchronousStashRemove(const FilePath &workingDirectory, const QString &stash,
                                       QString *errorMessage) const
{
    QStringList arguments = {"stash"};
    if (stash.isEmpty())
        arguments << "clear";
    else
        arguments << "drop" << stash;

    const CommandResult result = vcsSynchronousExec(workingDirectory, arguments);
    if (result.result() == ProcessResult::FinishedWithSuccess) {
        const QString output = result.cleanedStdOut();
        if (!output.isEmpty())
            VcsOutputWindow::appendSilently(workingDirectory, output);
        return true;
    }
    msgCannotRun(arguments, workingDirectory, result.cleanedStdErr(), errorMessage);
    return false;
}

/* Parse a stash line in its 2 manifestations (with message/without message containing
 * <base_hash>+subject):
\code
stash@{1}: WIP on <branch>: <base_hash> <subject_base_hash>
stash@{2}: On <branch>: <message>
\endcode */

static std::optional<Stash> parseStashLine(const QString &l)
{
    const QChar colon = ':';
    const int branchPos = l.indexOf(colon);
    if (branchPos < 0)
        return {};
    const int messagePos = l.indexOf(colon, branchPos + 1);
    if (messagePos < 0)
        return {};
    // Branch spec
    const int onIndex = l.indexOf("on ", branchPos + 2, Qt::CaseInsensitive);
    if (onIndex == -1 || onIndex >= messagePos)
        return {};
    return Stash{l.left(branchPos),
                 l.mid(onIndex + 3, messagePos - onIndex - 3),
                 l.mid(messagePos + 2)}; // skip blank
}

QList<Stash> GitClient::synchronousStashList(const FilePath &workingDirectory) const
{
    const QStringList arguments = {"stash", "list", noColorOption};
    const CommandResult result = vcsSynchronousExec(workingDirectory, arguments,
                                                    RunFlags::ForceCLocale);
    if (result.result() != ProcessResult::FinishedWithSuccess) {
        msgCannotRun(arguments, workingDirectory, result.cleanedStdErr(), nullptr);
        return {};
    }
    const QStringList lines = splitLines(result.cleanedStdOut());
    QList<Stash> stashes;
    for (const QString &line : lines) {
        const auto stash = parseStashLine(line);
        if (stash)
            stashes.push_back(*stash);
    }
    return stashes;
}

// Read a single-line config value, return trimmed
QString GitClient::readConfigValue(const FilePath &workingDirectory, const QString &configVar) const
{
    return readOneLine(workingDirectory, {"config", configVar});
}

QChar GitClient::commentChar(const Utils::FilePath &workingDirectory)
{
    const QString commentChar = readConfigValue(workingDirectory, "core.commentChar");
    return commentChar.isEmpty() ? QChar(Constants::DEFAULT_COMMENT_CHAR) : commentChar.at(0);
}

void GitClient::setConfigValue(const FilePath &workingDirectory, const QString &configVar,
                               const QString &value) const
{
    readOneLine(workingDirectory, {"config", configVar, value});
}

QString GitClient::readGitVar(const FilePath &workingDirectory, const QString &configVar) const
{
    return readOneLine(workingDirectory, {"var", configVar});
}

static TextEncoding configFileEncoding()
{
    // Git for Windows always uses UTF-8 for configuration:
    // https://github.com/msysgit/msysgit/wiki/Git-for-Windows-Unicode-Support#convert-config-files
    static const TextEncoding encoding =
            HostOsInfo::isWindowsHost() ? TextEncoding::Utf8 : TextEncoding::encodingForLocale();
    return encoding;
}

QString GitClient::readOneLine(const FilePath &workingDirectory, const QStringList &arguments) const
{
    const CommandResult result = vcsSynchronousExec(workingDirectory, arguments,
                                                    RunFlags::NoOutput, vcsTimeoutS(),
                                                    configFileEncoding());
    if (result.result() == ProcessResult::FinishedWithSuccess)
        return result.cleanedStdOut().trimmed();
    return {};
}

void GitClient::readConfigAsync(const FilePath &workingDirectory, const QStringList &arguments,
                                const CommandHandler &handler)
{
    enqueueCommand({workingDirectory, arguments, RunFlags::NoOutput, {}, configFileEncoding(),
                    handler});
}

QString GitClient::styleColorName(TextEditor::TextStyle style)
{
    using namespace TextEditor;

    const ColorScheme &scheme = TextEditorSettings::fontSettings().colorScheme();
    QColor color = scheme.formatFor(style).foreground();
    if (!color.isValid())
        color = scheme.formatFor(C_TEXT).foreground();
    return color.name();
}

bool GitClient::StashInfo::init(const FilePath &workingDirectory, const QString &command,
                                StashFlag flag, PushAction pushAction)
{
    m_workingDir = workingDirectory;
    m_flags = flag;
    m_pushAction = pushAction;
    QString errorMessage;
    QString statusOutput;
    switch (gitClient().gitStatus(m_workingDir, StatusMode(NoUntracked | NoSubmodules),
                                &statusOutput, &errorMessage)) {
    case StatusResult::Changed:
        if (m_flags & NoPrompt)
            executeStash(command, &errorMessage);
        else
            stashPrompt(command, statusOutput, &errorMessage);
        break;
    case StatusResult::Unchanged:
        m_stashResult = StashUnchanged;
        break;
    case StatusResult::Failed:
        m_stashResult = StashFailed;
        break;
    }

    if (m_stashResult == StashFailed)
        VcsOutputWindow::appendError(m_workingDir, errorMessage);
    return !stashingFailed();
}

void GitClient::StashInfo::stashPrompt(const QString &command, const QString &statusOutput,
                                       QString *errorMessage)
{
    QMessageBox msgBox(QMessageBox::Question, Tr::tr("Uncommitted Changes Found"),
                       Tr::tr("What would you like to do with local changes in:") + "\n\n\""
                       + m_workingDir.toUserOutput() + '\"',
                       QMessageBox::NoButton, ICore::dialogParent());

    msgBox.setDetailedText(statusOutput);

    QPushButton *stashAndPopButton = msgBox.addButton(Tr::tr("Stash && &Pop"), QMessageBox::AcceptRole);
    stashAndPopButton->setToolTip(Tr::tr("Stash local changes and pop when %1 finishes.").arg(command));

    QPushButton *stashButton = msgBox.addButton(Tr::tr("&Stash"), QMessageBox::AcceptRole);
    stashButton->setToolTip(Tr::tr("Stash local changes and execute %1.").arg(command));

    QPushButton *discardButton = msgBox.addButton(Tr::tr("&Discard"), QMessageBox::AcceptRole);
    discardButton->setToolTip(Tr::tr("Discard (reset) local changes and execute %1.").arg(command));

    QPushButton *ignoreButton = nullptr;
    if (m_flags & AllowUnstashed) {
        ignoreButton = msgBox.addButton(QMessageBox::Ignore);
        ignoreButton->setToolTip(Tr::tr("Execute %1 with local changes in working directory.")
                                 .arg(command));
    }

    QPushButton *cancelButton = msgBox.addButton(QMessageBox::Cancel);
    cancelButton->setToolTip(Tr::tr("Cancel %1.").arg(command));

    QPushButton *diffButton = msgBox.addButton(Tr::tr("Di&ff && Cancel"), QMessageBox::RejectRole);
    diffButton->setToolTip(Tr::tr("Show a diff of the local changes and cancel %1.").arg(command));

    msgBox.exec();

    if (msgBox.clickedButton() == discardButton) {
        m_stashResult = gitClient().synchronousReset(m_workingDir, QStringList(), errorMessage) ?
                    StashUnchanged : StashFailed;
    } else if (msgBox.clickedButton() == ignoreButton) { // At your own risk, so.
        m_stashResult = NotStashed;
    } else if (msgBox.clickedButton() == cancelButton) {
        m_stashResult = StashCanceled;
    } else if (msgBox.clickedButton() == diffButton) {
        m_stashResult = StashCanceled;
        gitClient().diffUnstagedRepository(m_workingDir);
    } else if (msgBox.clickedButton() == stashButton) {
        const bool result = gitClient().executeSynchronousStash(
                    m_workingDir, creatorStashMessage(command), false, errorMessage);
        m_stashResult = result ? StashUnchanged : StashFailed;
    } else if (msgBox.clickedButton() == stashAndPopButton) {
        executeStash(command, errorMessage);
    }
}

void GitClient::StashInfo::executeStash(const QString &command, QString *errorMessage)
{
    m_message = creatorStashMessage(command);
    if (!gitClient().executeSynchronousStash(m_workingDir, m_message, false, errorMessage))
        m_stashResult = StashFailed;
    else
        m_stashResult = Stashed;
 }

bool GitClient::StashInfo::stashingFailed() const
{
    switch (m_stashResult) {
    case StashCanceled:
    case StashFailed:
        return true;
    case NotStashed:
        return !(m_flags & AllowUnstashed);
    default:
        return false;
    }
}

void GitClient::StashInfo::end()
{
    if (m_stashResult == Stashed) {
        const QString stashName = stashNameFromMessage(m_workingDir, m_message);
        if (!stashName.isEmpty())
            gitClient().stashPop(m_workingDir, stashName);
    }

    if (m_pushAction == NormalPush)
        gitClient().push(m_workingDir);
    else if (m_pushAction == PushToGerrit)
        gerritPush(m_workingDir);

    m_pushAction = NoPush;
    m_stashResult = NotStashed;
}

GitRemote::GitRemote(const QString &location) : Core::IVersionControl::RepoUrl(location)
{
    if (isValid && protocol == "file")
        isValid = QDir(path).exists() || QDir(path + ".git").exists();
}

QString GitClient::suggestedLocalBranchName(
        const FilePath &workingDirectory,
        const QStringList &localNames,
        const QString &target,
        BranchTargetType targetType)
{
    QString initialName;
    if (targetType == BranchTargetType::Remote) {
        initialName = target.mid(target.lastIndexOf('/') + 1);
    } else {
        const Result<QString> res =
                gitClient().synchronousLog(workingDirectory,
                                           {"-n", "1", "--format=%s", target},
                                           RunFlags::NoOutput);
        if (res)
            initialName = res.value().trimmed();
        else
            VcsOutputWindow::appendError(workingDirectory, res.error());
    }
    QString suggestedName = initialName;
    int i = 2;
    while (localNames.contains(suggestedName)) {
        suggestedName = initialName + QString::number(i);
        ++i;
    }

    return suggestedName;
}

void GitClient::addChangeActions(QMenu *menu, const FilePath &source, const QString &change)
{
    QTC_ASSERT(!change.isEmpty(), return);
    const FilePath &workingDir = fileWorkingDirectory(source);
    const bool isRange = change.contains("..");
    menu->addAction(Tr::tr("Cherr&y-Pick %1").arg(change), [workingDir, change] {
        gitClient().synchronousCherryPick(workingDir, change);
    });
    menu->addAction(Tr::tr("Re&vert %1").arg(change), [workingDir, change] {
        gitClient().synchronousRevert(workingDir, change);
    });
    if (!isRange) {
        menu->addAction(Tr::tr("C&heckout %1").arg(change), [workingDir, change] {
            gitClient().checkout(workingDir, change);
        });
        menu->addAction(Tr::tr("Create &Branch from %1...").arg(change), [workingDir, change] {
            const QStringList localBranches =
                gitClient().synchronousRepositoryBranches(workingDir.toFSPathString());
            BranchAddDialog dialog(localBranches, BranchAddDialog::Type::AddBranch,
                                   Core::ICore::dialogParent());
            dialog.setBranchName(suggestedLocalBranchName(workingDir, localBranches, change,
                                                          BranchTargetType::Commit));
            dialog.setCheckoutVisible(true);
            if (dialog.exec() != QDialog::Accepted)
                return;

            const QString newBranch = dialog.branchName();
            QString output;
            QString errorMessage;
            if (!gitClient().synchronousBranchCmd(workingDir,
                                                  {"--no-track", newBranch, change},
                                                  &output, &errorMessage)) {
                VcsOutputWindow::appendError(workingDir, errorMessage);
                return;
            }

            if (dialog.checkout())
                gitClient().checkout(workingDir, newBranch);
        });

        connect(menu->addAction(Tr::tr("&Interactive Rebase from %1...").arg(change)),
                &QAction::triggered, [workingDir, change] {
            startRebaseFromCommit(workingDir, change);
        });
    }
    QAction *logAction = menu->addAction(Tr::tr("&Log for %1").arg(change), [workingDir, change] {
        gitClient().log(workingDir, QString(), false, {change});
    });
    if (isRange) {
        menu->setDefaultAction(logAction);
    } else {
        const FilePath filePath = source;
        if (!filePath.isDir()) {
            menu->addAction(Tr::tr("Sh&ow file \"%1\" on revision %2").arg(filePath.fileName(), change),
                            [workingDir, change, source] {
                gitClient().openShowEditor(workingDir, change, source);
            });
        }
        menu->addAction(Tr::tr("Add &Tag for %1...").arg(change), [workingDir, change] {
            QString output;
            QString errorMessage;
            gitClient().synchronousTagCmd(workingDir, QStringList(), &output, &errorMessage);

            const QStringList tags = output.split('\n');
            BranchAddDialog dialog(tags, BranchAddDialog::Type::AddTag, Core::ICore::dialogParent());

            if (dialog.exec() == QDialog::Rejected)
                return;

            const QString tag = dialog.branchName();
            const QString annotation = dialog.annotation();
            const auto args = [annotation, tag, change] {
                if (annotation.isEmpty())
                    return QStringList{tag, change};
                return QStringList{"-a", "-m", annotation, tag, change};
            };
            gitClient().synchronousTagCmd(workingDir, args(), &output, &errorMessage);
            VcsOutputWindow::appendSilently(workingDir, output);
            if (!errorMessage.isEmpty())
                VcsOutputWindow::appendError(workingDir, errorMessage);
        });

        auto resetChange = [workingDir, change](const QByteArray &resetType) {
            gitClient().reset(workingDir, QLatin1String("--" + resetType), change);
        };
        auto resetMenu = new QMenu(Tr::tr("&Reset to Change %1").arg(change), menu);
        resetMenu->addAction(Tr::tr("&Hard"), std::bind(resetChange, "hard"));
        resetMenu->addAction(Tr::tr("&Mixed"), std::bind(resetChange, "mixed"));
        resetMenu->addAction(Tr::tr("&Soft"), std::bind(resetChange, "soft"));
        menu->addMenu(resetMenu);
    }

    menu->addAction((isRange ? Tr::tr("Di&ff %1") : Tr::tr("Di&ff Against %1")).arg(change),
                    [workingDir, change] {
        gitClient().diffRepository(workingDir, change, {});
    });
    if (!isRange) {
        if (!gitClient().m_diffCommit.isEmpty()) {
            menu->addAction(Tr::tr("Diff &Against Saved %1").arg(gitClient().m_diffCommit),
                            [workingDir, change] {
                gitClient().diffRepository(workingDir, gitClient().m_diffCommit, change);
                gitClient().m_diffCommit.clear();
            });
        }
        menu->addAction(Tr::tr("&Save for Diff"), [change] {
            gitClient().m_diffCommit = change;
        });
    }
}

FilePath GitClient::fileWorkingDirectory(const Utils::FilePath &file)
{
    Utils::FilePath path = file;
    if (!path.isEmpty() && !path.isDir())
        path = path.parentDir();
    while (!path.isEmpty() && !path.exists())
        path = path.parentDir();
    return path;
}

IEditor *GitClient::openShowEditor(const FilePath &workingDirectory, const QString &ref,
                                   const FilePath &path, ShowEditor showSetting)
{
    const FilePath topLevel = VcsManager::findTopLevelForDirectory(workingDirectory);
    const QString topLevelString = topLevel.toUrlishString();
    const QString relativePath = QDir(topLevelString).relativeFilePath(path.toUrlishString());
    const QByteArray content = synchronousShow(topLevel, ref + ":" + relativePath);
    if (showSetting == ShowEditor::OnlyIfDifferent) {
        if (content.isEmpty())
            return nullptr;
        QByteArray fileContent;
        if (TextFileFormat::readFileUtf8(path, {}, &fileContent)) {
            if (fileContent == content)
                return nullptr; // open the file for read/write
        }
    }

    const QString documentId = QLatin1String(Git::Constants::GIT_PLUGIN)
            + QLatin1String(".GitShow.") + topLevelString
            + QLatin1String(".") + relativePath;
    QString title = Tr::tr("Git Show %1:%2").arg(ref, relativePath);
    IEditor *editor = EditorManager::openEditorWithContents(Id(), &title, content, documentId,
                                                            EditorManager::DoNotSwitchToDesignMode);
    editor->document()->setTemporary(true);
    // FIXME: Check should that be relative
    VcsBase::setSource(editor->document(), path);
    return editor;
}

ColorNames GitClient::colorNames()
{
    ColorNames result;
    result.author = styleColorName(TextEditor::C_LOG_AUTHOR_NAME);
    result.date = styleColorName(TextEditor::C_LOG_COMMIT_DATE);
    result.hash = styleColorName(TextEditor::C_LOG_COMMIT_HASH);
    result.decoration = styleColorName(TextEditor::C_LOG_DECORATION);
    result.subject = styleColorName(TextEditor::C_LOG_COMMIT_SUBJECT);
    result.body = styleColorName(TextEditor::C_TEXT);
    return result;
}

void GitClient::setupTimer()
{
    QTC_ASSERT(!m_timer, return);
    m_timer.reset(new QTimer);
    connect(m_timer.get(), &QTimer::timeout, this, &GitClient::updateModificationInfos);
    using namespace std::chrono_literals;
    m_timer->setInterval(10s);
    m_timer->setSingleShot(true);
    m_timer->start();
}

} // Git::Internal

#include "gitclient.moc"
