// Copyright (C) 2024 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0+ OR GPL-3.0 WITH Qt-GPL-exception-1.0
#include "messagemodel.h"

#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/editormanager/editormanager.h>
#include <qmldesignerplugin.h>
#include <texteditor/texteditorview.h>

#include <QDesktopServices>
#include <QWindow>

MessageModel::MessageModel(QObject *parent)
    : QAbstractListModel(parent)
{
    setupTaskHub();
}

int MessageModel::errorCount() const
{
    int count = 0;
    for (const auto &task : m_tasks) {
        if (task.isError())
            count++;
    }
    return count;
}

int MessageModel::warningCount() const
{
    int count = 0;
    for (const auto &task : m_tasks) {
        if (task.isWarning())
            count++;
    }
    return count;
}

void MessageModel::resetModel()
{
    auto *hub = &ProjectExplorer::taskHub();
    hub->clearTasks();

    beginResetModel();
    m_tasks.clear();
    endResetModel();
    emit modelChanged();
}

void MessageModel::jumpToCode(const QVariant &index)
{
    bool ok = false;
    if (int idx = index.toInt(&ok); ok) {
        if (idx >= 0 && std::cmp_less(idx, m_tasks.size())) {
            ProjectExplorer::Task task = m_tasks.at(static_cast<size_t>(idx));
            const int column = task.column() ? task.column() - 1 : 0;

            if (Core::EditorManager::openEditor(task.file(),
                                                Utils::Id(),
                                                Core::EditorManager::DoNotMakeVisible)) {

                auto &viewManager = QmlDesigner::QmlDesignerPlugin::instance()->viewManager();
                if (auto *editorView = viewManager.textEditorView()) {
                    if (TextEditor::BaseTextEditor *editor = editorView->textEditor()) {
                        editor->gotoLine(task.line(), column);
                        editor->widget()->setFocus();
                        editor->editorWidget()->updateFoldingHighlight(QTextCursor());
                    }
                }
            }
        }
    }
}

void MessageModel::openLink(const QVariant &url)
{
    QDesktopServices::openUrl(QUrl::fromUserInput(url.toString()));
}

int MessageModel::rowCount(const QModelIndex &) const
{
    return static_cast<int>(m_tasks.size());
}

QHash<int, QByteArray> MessageModel::roleNames() const
{
    QHash<int, QByteArray> out;
    out[MessageRole] = "message";
    out[FileNameRole] = "location";
    out[TypeRole] = "type";
    return out;
}

QVariant MessageModel::data(const QModelIndex &index, int role) const
{
    if (index.isValid() && index.row() < rowCount()) {
        size_t row = static_cast<size_t>(index.row());
        if (role == MessageRole) {
            return m_tasks.at(row).description();
        } else if (role == FileNameRole) {
            Utils::FilePath path = m_tasks.at(row).file();
            return path.fileName();
        } else if (role == TypeRole) {
            ProjectExplorer::Task::TaskType type = m_tasks.at(row).type();
            if (type == ProjectExplorer::Task::TaskType::Error)
                return "Error";
            else if (type == ProjectExplorer::Task::TaskType::Warning)
                return "Warning";
            else
                return "Unknown";
        } else {
            qWarning() << Q_FUNC_INFO << "invalid role";
        }
    } else {
        qWarning() << Q_FUNC_INFO << "invalid index";
    }
    return QVariant();
}

void MessageModel::setupTaskHub()
{
    auto *hub = &ProjectExplorer::taskHub();
    connect(hub, &ProjectExplorer::TaskHub::categoryAdded, this, &MessageModel::addCategory);
    connect(hub, &ProjectExplorer::TaskHub::taskAdded, this, &MessageModel::addTask);
    connect(hub, &ProjectExplorer::TaskHub::taskRemoved, this, &MessageModel::removeTask);
    connect(hub, &ProjectExplorer::TaskHub::tasksCleared, this, &MessageModel::clearTasks);
}

void MessageModel::addCategory(const ProjectExplorer::TaskCategory &category)
{
    m_categories[category.id] = category;
}

void MessageModel::addTask(const ProjectExplorer::Task &task)
{
    int at = static_cast<int>(m_tasks.size());
    beginInsertRows(QModelIndex(), at, at);
    m_tasks.push_back(task);
    endInsertRows();
    emit modelChanged();
}

void MessageModel::removeTask(const ProjectExplorer::Task &task)
{
    for (int i = 0; std::cmp_less(i, m_tasks.size()); i++) {
        if (m_tasks[static_cast<size_t>(i)] == task) {
            beginRemoveRows(QModelIndex(), i, i);
            m_tasks.erase(m_tasks.begin() + i);
            endRemoveRows();
            emit modelChanged();
            return;
        }
    }
}

void MessageModel::clearTasks(const Utils::Id &categoryId)
{
    beginResetModel();
    std::erase_if(m_tasks, [categoryId](const ProjectExplorer::Task &task) {
        return task.category() == categoryId;
    });
    endResetModel();
    emit modelChanged();
}
