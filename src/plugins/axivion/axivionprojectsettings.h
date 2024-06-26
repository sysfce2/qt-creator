// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <utils/id.h>

#include <QObject>

namespace ProjectExplorer { class Project; }

namespace Axivion::Internal {

class AxivionProjectSettings : public QObject
{
public:
    explicit AxivionProjectSettings(ProjectExplorer::Project *project);

    void setDashboardProjectName(const QString &name) { m_dashboardProjectName = name; }
    QString dashboardProjectName() const { return m_dashboardProjectName; }
    void setDashboardId(const Utils::Id &dashboardId) { m_dashboardId = dashboardId; }
    Utils::Id dashboardId() const { return m_dashboardId; }

    static AxivionProjectSettings *projectSettings(ProjectExplorer::Project *project);
    static void destroyProjectSettings();
    static void setupProjectPanel();

private:
    void load();
    void save();

    ProjectExplorer::Project *m_project = nullptr;
    QString m_dashboardProjectName;
    // id of the dashboard in use for this project, or default from settings on initialization
    Utils::Id m_dashboardId;
};

} // Axivion::Internal
