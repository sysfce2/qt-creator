// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "taskwindow.h"

#include "taskhandlers.h"
#include "parseissuesdialog.h"
#include "projectexplorericons.h"
#include "projectexplorertr.h"
#include "task.h"
#include "taskhub.h"
#include "taskmodel.h"

#include <coreplugin/actionmanager/command.h>
#include <coreplugin/coreconstants.h>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/find/itemviewfind.h>
#include <coreplugin/icontext.h>
#include <coreplugin/icore.h>
#include <coreplugin/session.h>

#include <utils/algorithm.h>
#include <utils/execmenu.h>
#include <utils/fileinprojectfinder.h>
#include <utils/hostosinfo.h>
#include <utils/itemviews.h>
#include <utils/outputformatter.h>
#include <utils/qtcassert.h>
#include <utils/stylehelper.h>
#include <utils/theme/theme.h>
#include <utils/tooltip/tooltip.h>
#include <utils/utilsicons.h>

#include <QAbstractTextDocumentLayout>
#include <QDesktopServices>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>

using namespace Core;
using namespace Utils;

const char SESSION_FILTER_CATEGORIES[] = "TaskWindow.Categories";
const char SESSION_FILTER_WARNINGS[] = "TaskWindow.IncludeWarnings";

namespace ProjectExplorer {

namespace Internal {

class TaskView : public TreeView
{
public:
    TaskView();
    void resizeColumns();

private:
    void resizeEvent(QResizeEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    bool event(QEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

    QString anchorAt(const QPoint &pos);
    void showToolTip(const Task &task, const QPoint &pos);

    QString m_hoverAnchor;
    QString m_clickAnchor;
};

class TaskDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QTextDocument &doc() { return m_doc; }
private:
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    bool needsSpecialHandling(const QModelIndex &index) const;

    mutable QTextDocument m_doc;
};

/////
// TaskWindow
/////

class TaskWindowPrivate
{
public:
    Internal::TaskModel *m_model;
    Internal::TaskFilterModel *m_filter;
    TaskView m_treeView;
    const Core::Context m_taskWindowContext{Core::Context(Core::Constants::C_PROBLEM_PANE)};
    QToolButton *m_filterWarningsButton;
    QToolButton *m_categoriesButton;
    QToolButton *m_externalButton = nullptr;
    QMenu *m_categoriesMenu;
    int m_visibleIssuesCount = 0;
};

static QToolButton *createFilterButton(const QIcon &icon, const QString &toolTip,
                                       QObject *receiver, std::function<void(bool)> lambda)
{
    auto button = new QToolButton;
    button->setIcon(icon);
    button->setToolTip(toolTip);
    button->setCheckable(true);
    button->setChecked(true);
    button->setEnabled(true);
    QObject::connect(button, &QToolButton::toggled, receiver, lambda);
    return button;
}

TaskWindow::TaskWindow() : d(std::make_unique<TaskWindowPrivate>())
{
    setId("Issues");
    setDisplayName(Tr::tr("Issues"));
    setPriorityInStatusBar(100);

    d->m_model = new Internal::TaskModel(this);
    d->m_filter = new Internal::TaskFilterModel(d->m_model);
    d->m_filter->setAutoAcceptChildRows(true);

    d->m_treeView.setSearchRole(TaskModel::Description);
    d->m_treeView.setHeaderHidden(true);
    d->m_treeView.setExpandsOnDoubleClick(false);
    d->m_treeView.setAlternatingRowColors(true);
    d->m_treeView.setTextElideMode(Qt::ElideMiddle);
    d->m_treeView.setItemDelegate(new TaskDelegate(this));
    d->m_treeView.setModel(d->m_filter);
    d->m_treeView.setFrameStyle(QFrame::NoFrame);
    d->m_treeView.setWindowTitle(displayName());
    d->m_treeView.setSelectionMode(QAbstractItemView::ExtendedSelection);
    d->m_treeView.setWindowIcon(Icons::WINDOW.icon());
    d->m_treeView.setContextMenuPolicy(Qt::ActionsContextMenu);
    d->m_treeView.setAttribute(Qt::WA_MacShowFocusRect, false);
    d->m_treeView.resizeColumns();

    Core::IContext::attach(&d->m_treeView, d->m_taskWindowContext);

    connect(d->m_treeView.selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex &index) { d->m_treeView.scrollTo(index); });
    connect(&d->m_treeView, &QAbstractItemView::activated,
            this, &TaskWindow::triggerDefaultHandler);
    connect(d->m_treeView.selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this] {
        delayedInitialization();
        updateTaskHandlerActionsState();
    });

    d->m_treeView.setContextMenuPolicy(Qt::ActionsContextMenu);

    d->m_filterWarningsButton = createFilterButton(
                Utils::Icons::WARNING_TOOLBAR.icon(),
                Tr::tr("Show Warnings"), this, [this](bool show) { setShowWarnings(show); });

    d->m_externalButton = new QToolButton;
    d->m_externalButton->setIcon(Utils::Icons::OPENFILE_TOOLBAR.icon());
    d->m_externalButton->setToolTip(Tr::tr("Create Issues From External Build Output..."));
    connect(d->m_externalButton, &QToolButton::clicked, this, &executeParseIssuesDialog);

    d->m_categoriesButton = new QToolButton;
    d->m_categoriesButton->setIcon(Utils::Icons::FILTER.icon());
    d->m_categoriesButton->setToolTip(Tr::tr("Filter by categories"));
    d->m_categoriesButton->setProperty(StyleHelper::C_NO_ARROW, true);
    d->m_categoriesButton->setPopupMode(QToolButton::InstantPopup);

    d->m_categoriesMenu = new QMenu(d->m_categoriesButton);
    connect(d->m_categoriesMenu, &QMenu::aboutToShow, this, &TaskWindow::updateCategoriesMenu);
    Utils::addToolTipsToMenu(d->m_categoriesMenu);

    d->m_categoriesButton->setMenu(d->m_categoriesMenu);

    setupFilterUi("IssuesPane.Filter", "ProjectExplorer::Internal::TaskWindow");
    setFilteringEnabled(true);

    TaskHub *hub = &taskHub();
    connect(hub, &TaskHub::categoryAdded, this, &TaskWindow::addCategory);
    connect(hub, &TaskHub::taskAdded, this, &TaskWindow::addTask);
    connect(hub, &TaskHub::taskRemoved, this, &TaskWindow::removeTask);
    connect(hub, &TaskHub::taskLineNumberUpdated, this, &TaskWindow::updatedTaskLineNumber);
    connect(hub, &TaskHub::taskFilePathUpdated, this, &TaskWindow::updatedTaskFilePath);
    connect(hub, &TaskHub::tasksCleared, this, &TaskWindow::clearTasks);
    connect(hub, &TaskHub::categoryVisibilityChanged, this, &TaskWindow::setCategoryVisibility);
    connect(hub, &TaskHub::popupRequested, this, &TaskWindow::popup, Qt::QueuedConnection);
    connect(hub, &TaskHub::showTask, this, &TaskWindow::showTask);
    connect(hub, &TaskHub::openTask, this, &TaskWindow::openTask);

    connect(d->m_filter, &TaskFilterModel::rowsAboutToBeRemoved, this,
            [this](const QModelIndex &, int first, int last) {
        d->m_visibleIssuesCount -= d->m_filter->issuesCount(first, last);
        emit setBadgeNumber(d->m_visibleIssuesCount);
    });
    connect(d->m_filter, &TaskFilterModel::rowsInserted, this,
            [this](const QModelIndex &, int first, int last) {
        d->m_visibleIssuesCount += d->m_filter->issuesCount(first, last);
        emit setBadgeNumber(d->m_visibleIssuesCount);
    });
    connect(d->m_filter, &TaskFilterModel::modelReset, this, [this] {
        d->m_visibleIssuesCount = d->m_filter->issuesCount(0, d->m_filter->rowCount());
        emit setBadgeNumber(d->m_visibleIssuesCount);
    });

    SessionManager *session = SessionManager::instance();
    connect(session, &SessionManager::aboutToSaveSession, this, &TaskWindow::saveSettings);
    connect(session, &SessionManager::sessionLoaded, this, &TaskWindow::loadSettings);
}

TaskWindow::~TaskWindow()
{
    delete d->m_externalButton;
    delete d->m_filterWarningsButton;
    delete d->m_filter;
    delete d->m_model;
}

void TaskWindow::delayedInitialization()
{
    static bool alreadyDone = false;
    if (alreadyDone)
        return;

    alreadyDone = true;

    const auto registerTaskHandlerAction = [this](QAction *action) {
        action->setParent(this);
        action->setEnabled(false);
        d->m_treeView.addAction(action);
    };
    const auto getTasksForHandler = [this] {
        return d->m_filter->tasks(d->m_treeView.selectionModel()->selectedIndexes());
    };
    setupTaskHandlers(this, d->m_taskWindowContext, registerTaskHandlerAction, getTasksForHandler);
}

QList<QWidget*> TaskWindow::toolBarWidgets() const
{
    return {d->m_externalButton, d->m_filterWarningsButton, d->m_categoriesButton, filterWidget()};
}

QWidget *TaskWindow::outputWidget(QWidget *)
{
    return &d->m_treeView;
}

void TaskWindow::clearTasks(Id categoryId)
{
    d->m_model->clearTasks(categoryId);

    emit tasksChanged();
    navigateStateChanged();
}

void TaskWindow::setCategoryVisibility(Id categoryId, bool visible)
{
    if (!categoryId.isValid())
        return;

    QSet<Id> categories = d->m_filter->filteredCategories();

    if (visible)
        categories.remove(categoryId);
    else
        categories.insert(categoryId);

    d->m_filter->setFilteredCategories(categories);
}

void TaskWindow::saveSettings()
{
    const QStringList categories = Utils::toList(
        Utils::transform(d->m_filter->filteredCategories(), &Id::toString));
    SessionManager::setValue(SESSION_FILTER_CATEGORIES, categories);
    SessionManager::setValue(SESSION_FILTER_WARNINGS, d->m_filter->filterIncludesWarnings());
}

void TaskWindow::loadSettings()
{
    QVariant value = SessionManager::value(SESSION_FILTER_CATEGORIES);
    if (value.isValid()) {
        const QSet<Id> categories = Utils::toSet(
            Utils::transform(value.toStringList(), &Id::fromString));
        d->m_filter->setFilteredCategories(categories);
    }
    value = SessionManager::value(SESSION_FILTER_WARNINGS);
    if (value.isValid()) {
        bool includeWarnings = value.toBool();
        d->m_filter->setFilterIncludesWarnings(includeWarnings);
        d->m_filterWarningsButton->setChecked(d->m_filter->filterIncludesWarnings());
    }
}

void TaskWindow::visibilityChanged(bool visible)
{
    if (visible)
        delayedInitialization();
}

void TaskWindow::addCategory(const TaskCategory &category)
{
    d->m_model->addCategory(category);
    if (!category.visible) {
        QSet<Id> filters = d->m_filter->filteredCategories();
        filters.insert(category.id);
        d->m_filter->setFilteredCategories(filters);
    }
}

void TaskWindow::addTask(const Task &task)
{
    d->m_model->addTask(task);

    emit tasksChanged();
    navigateStateChanged();

    if (task.isFlashworthy()
         && task.isError()
         && d->m_filter->filterIncludesErrors()
         && !d->m_filter->filteredCategories().contains(task.category())) {
        flash();
    }
}

void TaskWindow::removeTask(const Task &task)
{
    d->m_model->removeTask(task.id());

    emit tasksChanged();
    navigateStateChanged();
}

void TaskWindow::updatedTaskFilePath(const Task &task, const FilePath &filePath)
{
    d->m_model->updateTaskFilePath(task, filePath);
    emit tasksChanged();
}

void TaskWindow::updatedTaskLineNumber(const Task &task, int line)
{
    d->m_model->updateTaskLineNumber(task, line);
    emit tasksChanged();
}

void TaskWindow::showTask(const Task &task)
{
    int sourceRow = d->m_model->rowForTask(task);
    QModelIndex sourceIdx = d->m_model->index(sourceRow, 0);
    QModelIndex filterIdx = d->m_filter->mapFromSource(sourceIdx);
    d->m_treeView.setCurrentIndex(filterIdx);
    popup(Core::IOutputPane::ModeSwitch);
}

void TaskWindow::openTask(const Task &task)
{
    int sourceRow = d->m_model->rowForTask(task);
    QModelIndex sourceIdx = d->m_model->index(sourceRow, 0);
    QModelIndex filterIdx = d->m_filter->mapFromSource(sourceIdx);
    triggerDefaultHandler(filterIdx);
}

void TaskWindow::triggerDefaultHandler(const QModelIndex &index)
{
    ITaskHandler * const defaultHandler = defaultTaskHandler();
    if (!index.isValid() || !defaultHandler)
        return;

    QModelIndex taskIndex = index;
    if (index.parent().isValid())
        taskIndex = index.parent();
    if (taskIndex.column() == 1)
        taskIndex = taskIndex.siblingAtColumn(0);
    Task task(d->m_filter->task(taskIndex));
    if (task.isNull())
        return;

    if (task.hasFile() && !task.file().isAbsolutePath()
            && !task.fileCandidates().empty()) {
        const FilePath userChoice = Utils::chooseFileFromList(task.fileCandidates());
        if (!userChoice.isEmpty()) {
            task.setFile(userChoice);
            updatedTaskFilePath(task, task.file());
        }
    }

    if (defaultHandler->canHandle(task)) {
        defaultHandler->handle(task);
    } else {
        if (!task.file().exists())
            d->m_model->setFileNotFound(taskIndex, true);
    }
}

void TaskWindow::setShowWarnings(bool show)
{
    d->m_filter->setFilterIncludesWarnings(show);
}

void TaskWindow::updateCategoriesMenu()
{
    d->m_categoriesMenu->clear();

    const QSet<Id> filteredCategories = d->m_filter->filteredCategories();
    const QList<TaskCategory> categories = Utils::sorted(d->m_model->categories(),
                                                         &TaskCategory::displayName);

    for (const TaskCategory &c : categories) {
        auto action = new QAction(d->m_categoriesMenu);
        action->setCheckable(true);
        action->setText(c.displayName);
        action->setToolTip(c.description);
        action->setChecked(!filteredCategories.contains(c.id));
        connect(action, &QAction::triggered, this, [this, action, id = c.id] {
            setCategoryVisibility(id, action->isChecked());
        });
        d->m_categoriesMenu->addAction(action);
    }
}

int TaskWindow::taskCount(Id category) const
{
    return d->m_model->taskCount(category);
}

int TaskWindow::errorTaskCount(Id category) const
{
    return d->m_model->errorTaskCount(category);
}

int TaskWindow::warningTaskCount(Id category) const
{
    return d->m_model->warningTaskCount(category);
}

void TaskWindow::clearContents()
{
    // clear all tasks in all displays
    // Yeah we are that special
    TaskHub::clearTasks();
}

bool TaskWindow::hasFocus() const
{
    return d->m_treeView.window()->focusWidget() == &d->m_treeView;
}

bool TaskWindow::canFocus() const
{
    return d->m_filter->rowCount();
}

void TaskWindow::setFocus()
{
    if (d->m_filter->rowCount()) {
        d->m_treeView.setFocus();
        if (!d->m_treeView.currentIndex().isValid())
            d->m_treeView.setCurrentIndex(d->m_filter->index(0,0, QModelIndex()));
        if (d->m_treeView.selectionModel()->selection().isEmpty()) {
            d->m_treeView.selectionModel()->setCurrentIndex(d->m_treeView.currentIndex(),
                                                            QItemSelectionModel::Select);
        }
    }
}

bool TaskWindow::canNext() const
{
    return d->m_filter->rowCount();
}

bool TaskWindow::canPrevious() const
{
    return d->m_filter->rowCount();
}

void TaskWindow::goToNext()
{
    if (canNext())
        goToNextOrPrev(1);
}

void TaskWindow::goToPrev()
{
    if (canPrevious())
        goToNextOrPrev(-1);
}

void TaskWindow::goToNextOrPrev(int offset)
{
    QModelIndex startIndex = d->m_treeView.currentIndex();
    QModelIndex currentIndex = startIndex;
    QModelIndex actualNeighbor;

    if (startIndex.isValid()) {
        do {
            int row = currentIndex.row() + offset;
            if (row == d->m_filter->rowCount())
                row = 0;
            else if (row < 0)
                row = d->m_filter->rowCount() - 1;
            currentIndex = d->m_filter->index(row, 0);
            if (!actualNeighbor.isValid())
                actualNeighbor = currentIndex;
            if (d->m_filter->hasFile(currentIndex))
                break;
        } while (startIndex != currentIndex);
    } else {
        currentIndex = d->m_filter->index(0, 0);
    }

    // We only consider elements with files, except if there are none at all, in which case
    // we don't skip anything.
    if (currentIndex == startIndex && actualNeighbor.isValid()
        && !d->m_filter->hasFile(currentIndex)) {
        currentIndex = actualNeighbor;
    }

    d->m_treeView.setCurrentIndex(currentIndex);
    triggerDefaultHandler(currentIndex);
}

void TaskWindow::updateFilter()
{
    d->m_filter->updateFilterProperties(filterText(), filterCaseSensitivity(), filterUsesRegexp(),
                                        filterIsInverted());
}

bool TaskWindow::canNavigate() const
{
    return true;
}

void TaskDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                         const QModelIndex &index) const
{
    if (!needsSpecialHandling(index)) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    QStyleOptionViewItem options = option;
    initStyleOption(&options, index);

    painter->save();
    m_doc.setHtml(options.text);
    m_doc.setTextWidth(options.rect.width());
    options.text = "";
    options.widget->style()->drawControl(QStyle::CE_ItemViewItem, &options, painter);
    painter->translate(options.rect.left(), options.rect.top());
    QRect clip(0, 0, options.rect.width(), options.rect.height());
    QAbstractTextDocumentLayout::PaintContext paintContext;
    paintContext.palette = options.palette;
    painter->setClipRect(clip);
    paintContext.clip = clip;
    if (qobject_cast<const QAbstractItemView *>(options.widget)
            ->selectionModel()->isSelected(index)) {
        QAbstractTextDocumentLayout::Selection selection;
        selection.cursor = QTextCursor(&m_doc);
        selection.cursor.select(QTextCursor::Document);
        selection.format.setBackground(options.palette.brush(QPalette::Highlight));
        selection.format.setForeground(options.palette.brush(QPalette::HighlightedText));
        paintContext.selections << selection;
    }
    m_doc.documentLayout()->draw(painter, paintContext);
    painter->restore();
}

QSize TaskDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (!needsSpecialHandling(index))
        return QStyledItemDelegate::sizeHint(option, index);

    QStyleOptionViewItem options = option;
    options.initFrom(options.widget);
    initStyleOption(&options, index);
    m_doc.setHtml(options.text);
    const auto view = qobject_cast<const QTreeView *>(options.widget);
    QTC_ASSERT(view, return {});
    m_doc.setTextWidth(view->width() * 0.85 - view->indentation());
    return QSize(m_doc.idealWidth(), m_doc.size().height());
}

bool TaskDelegate::needsSpecialHandling(const QModelIndex &index) const
{
    QModelIndex sourceIndex = index;
    if (const auto proxyModel = qobject_cast<const QAbstractProxyModel *>(index.model()))
        sourceIndex = proxyModel->mapToSource(index);
    return sourceIndex.internalId();
}

TaskView::TaskView()
{
    setMouseTracking(true);
    setVerticalScrollMode(ScrollPerPixel);
    setUniformRowHeights(false);
}

void TaskView::resizeColumns()
{
    setColumnWidth(0, width() * 0.85);
}

void TaskView::resizeEvent(QResizeEvent *e)
{
    TreeView::resizeEvent(e);
    resizeColumns();
}

void TaskView::mousePressEvent(QMouseEvent *e)
{
    m_clickAnchor = anchorAt(e->pos());
    if (m_clickAnchor.isEmpty())
        TreeView::mousePressEvent(e);
}

void TaskView::mouseMoveEvent(QMouseEvent *e)
{
    const QString anchor = anchorAt(e->pos());
    if (m_clickAnchor != anchor)
        m_clickAnchor.clear();
    if (m_hoverAnchor != anchor) {
        m_hoverAnchor = anchor;
        if (!m_hoverAnchor.isEmpty())
            setCursor(Qt::PointingHandCursor);
        else
            unsetCursor();
    }
}

void TaskView::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_clickAnchor.isEmpty() || e->button() == Qt::RightButton) {
        TreeView::mouseReleaseEvent(e);
        return;
    }

    const QString anchor = anchorAt(e->pos());
    if (anchor == m_clickAnchor) {
        if (OutputLineParser::isLinkTarget(m_clickAnchor)) {
            EditorManager::openEditorAt(
                OutputLineParser::parseLinkTarget(m_clickAnchor),
                {},
                EditorManager::SwitchSplitIfAlreadyVisible);
        } else {
            QDesktopServices::openUrl(QUrl(m_clickAnchor));
        }
    }
    m_clickAnchor.clear();
}

void TaskView::keyReleaseEvent(QKeyEvent *e)
{
    TreeView::keyReleaseEvent(e);
    if (e->key() == Qt::Key_Space) {
        const Task task = static_cast<TaskFilterModel *>(model())->task(currentIndex());
        if (!task.isNull()) {
            const QPoint toolTipPos = mapToGlobal(visualRect(currentIndex()).topLeft());
            QMetaObject::invokeMethod(this, [this, task, toolTipPos] {
                    showToolTip(task, toolTipPos); }, Qt::QueuedConnection);
        }
    }
}

bool TaskView::event(QEvent *e)
{
    if (e->type() != QEvent::ToolTip)
        return TreeView::event(e);

    const auto helpEvent = static_cast<QHelpEvent*>(e);
    const Task task = static_cast<TaskFilterModel *>(model())->task(indexAt(helpEvent->pos()));
    if (task.isNull())
        return TreeView::event(e);
    showToolTip(task, helpEvent->globalPos());
    e->accept();
    return true;
}

void TaskView::showToolTip(const Task &task, const QPoint &pos)
{
    if (!task.hasDetails()) {
        ToolTip::hideImmediately();
        return;
    }

    const auto layout = new QVBoxLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(new QLabel(task.formattedDescription({})));
    ToolTip::show(pos, layout);
}

QString TaskView::anchorAt(const QPoint &pos)
{
    const QModelIndex index = indexAt(pos);
    if (!index.isValid() || !index.internalId())
        return {};

    const QRect itemRect = visualRect(index);
    QTextDocument &doc = static_cast<TaskDelegate *>(itemDelegate())->doc();
    doc.setHtml(model()->data(index, Qt::DisplayRole).toString());
    const QAbstractTextDocumentLayout * const textLayout = doc.documentLayout();
    QTC_ASSERT(textLayout, return {});
    return textLayout->anchorAt(pos - itemRect.topLeft());
}

} // namespace Internal
} // namespace ProjectExplorer
