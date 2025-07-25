// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "cmaketoolsettingsaccessor.h"

#include "cmakeprojectmanagertr.h"
#include "cmakespecificsettings.h"
#include "cmaketool.h"

#include <coreplugin/icore.h>

#include <utils/algorithm.h>
#include <utils/environment.h>

#include <QDebug>
#include <QGuiApplication>

using namespace Utils;
using namespace ProjectExplorer;

namespace CMakeProjectManager::Internal {

// --------------------------------------------------------------------
// CMakeToolSettingsUpgraders:
// --------------------------------------------------------------------

class CMakeToolSettingsUpgraderV0 : public VersionUpgrader
{
    // Necessary to make Version 1 supported.
public:
    CMakeToolSettingsUpgraderV0() : VersionUpgrader(0, "4.6") { }

    // NOOP
    Store upgrade(const Store &data) final { return data; }
};

// --------------------------------------------------------------------
// Helpers:
// --------------------------------------------------------------------

const char CMAKE_TOOL_COUNT_KEY[] = "CMakeTools.Count";
const char CMAKE_TOOL_DATA_KEY[] = "CMakeTools.";
const char CMAKE_TOOL_DEFAULT_KEY[] = "CMakeTools.Default";
const char CMAKE_TOOL_FILENAME[] = "cmaketools.xml";

static std::vector<std::unique_ptr<CMakeTool>> autoDetectCMakeTools()
{
    FilePaths extraDirs;

    if (HostOsInfo::isWindowsHost()) {
        for (const auto &envVar : QStringList{"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"}) {
            if (qtcEnvironmentVariableIsSet(envVar)) {
                const QString progFiles = qtcEnvironmentVariable(envVar);
                extraDirs.append(FilePath::fromUserInput(progFiles + "/CMake"));
                extraDirs.append(FilePath::fromUserInput(progFiles + "/CMake/bin"));
            }
        }
    }

    if (HostOsInfo::isMacHost()) {
        extraDirs.append("/Applications/CMake.app/Contents/bin");
        extraDirs.append("/usr/local/bin");    // homebrew intel
        extraDirs.append("/opt/homebrew/bin"); // homebrew arm
        extraDirs.append("/opt/local/bin");    // macports
    }

    const FilePaths suspects = FilePath("cmake").searchAllInPath(extraDirs);

    std::vector<std::unique_ptr<CMakeTool>> found;
    for (const FilePath &command : std::as_const(suspects)) {
        auto item = std::make_unique<CMakeTool>(DetectionSource::FromSystem, CMakeTool::createId());
        item->setFilePath(command);
        item->setDisplayName(Tr::tr("System CMake at %1").arg(command.toUserOutput()));

        found.emplace_back(std::move(item));
    }

    return found;
}


static std::vector<std::unique_ptr<CMakeTool>>
mergeTools(std::vector<std::unique_ptr<CMakeTool>> &sdkTools,
           std::vector<std::unique_ptr<CMakeTool>> &userTools,
           std::vector<std::unique_ptr<CMakeTool>> &autoDetectedTools)
{
    std::vector<std::unique_ptr<CMakeTool>> result = std::move(sdkTools);
    while (userTools.size() > 0) {
        std::unique_ptr<CMakeTool> userTool = std::move(userTools[0]);
        userTools.erase(std::begin(userTools));

        int userToolIndex = Utils::indexOf(result, [&userTool](const std::unique_ptr<CMakeTool> &tool) {
            // Id should be sufficient, but we have older "mis-registered" docker based items.
            // Make sure that these don't override better new values from the sdk by
            // also checking the actual executable.
            return userTool->id() == tool->id() && userTool->cmakeExecutable() == tool->cmakeExecutable();
        });
        if (userToolIndex >= 0) {
            // Replace the sdk tool with the user tool, so any user changes do not get lost
            result[userToolIndex] = std::move(userTool);
        } else {
            const bool wasOriginallyAutoDetected = userTool->detectionSource().isAutoDetected();
            const bool isAutoDetectedAgain = Utils::contains(
                autoDetectedTools,
                Utils::equal(&CMakeTool::cmakeExecutable, userTool->cmakeExecutable()));
            const bool hasAutoDetectionSource = !userTool->detectionSource().id.isEmpty();

            // Tools from the SDK are auto-detected, but don't have an auto-detection source set.
            // Tools auto-detected from a device by the user have an auto-detection source set.
            if (wasOriginallyAutoDetected && !isAutoDetectedAgain && !hasAutoDetectionSource) {
                qWarning() << QString::fromLatin1(
                                  "Previously SDK provided CMakeTool \"%1\" (%2) dropped.")
                                  .arg(userTool->cmakeExecutable().toUserOutput())
                                  .arg(userTool->id().toString());
                continue;
            }
            result.emplace_back(std::move(userTool));
        }
    }

    // add all the autodetected tools that are not known yet
    while (autoDetectedTools.size() > 0) {
        std::unique_ptr<CMakeTool> autoDetectedTool = std::move(autoDetectedTools[0]);
        autoDetectedTools.erase(std::begin(autoDetectedTools));

        if (!Utils::contains(result,
                             Utils::equal(&CMakeTool::cmakeExecutable, autoDetectedTool->cmakeExecutable())))
            result.emplace_back(std::move(autoDetectedTool));
    }

    return result;
}


// --------------------------------------------------------------------
// CMakeToolSettingsAccessor:
// --------------------------------------------------------------------

CMakeToolSettingsAccessor::CMakeToolSettingsAccessor()
{
    setDocType("QtCreatorCMakeTools");
    setApplicationDisplayName(QGuiApplication::applicationDisplayName());
    setBaseFilePath(Core::ICore::userResourcePath(CMAKE_TOOL_FILENAME));

    addVersionUpgrader(std::make_unique<CMakeToolSettingsUpgraderV0>());
}

CMakeToolSettingsAccessor::CMakeTools CMakeToolSettingsAccessor::restoreCMakeTools(QWidget *parent) const
{
    CMakeTools result;

    const FilePath sdkSettingsFile = Core::ICore::installerResourcePath(CMAKE_TOOL_FILENAME);

    CMakeTools sdkTools = cmakeTools(restoreSettings(sdkSettingsFile, parent), true);

    //read the tools from the user settings file
    CMakeTools userTools = cmakeTools(restoreSettings(parent), false);

    //autodetect tools
    std::vector<std::unique_ptr<CMakeTool>> autoDetectedTools = autoDetectCMakeTools();

    //filter out the tools that were stored in SDK
    std::vector<std::unique_ptr<CMakeTool>> toRegister = mergeTools(sdkTools.cmakeTools,
                                                                    userTools.cmakeTools,
                                                                    autoDetectedTools);

    // Store all tools
    for (auto it = std::begin(toRegister); it != std::end(toRegister); ++it)
        result.cmakeTools.emplace_back(std::move(*it));

    result.defaultToolId = userTools.defaultToolId.isValid() ? userTools.defaultToolId : sdkTools.defaultToolId;

    // Set default TC...
    return result;
}

void CMakeToolSettingsAccessor::saveCMakeTools(const QList<CMakeTool *> &cmakeTools,
                                               const Id &defaultId,
                                               QWidget *parent)
{
    Store data;
    data.insert(CMAKE_TOOL_DEFAULT_KEY, defaultId.toSetting());

    int count = 0;
    for (CMakeTool *item : cmakeTools) {
        if (item->detectionSource().isTemporary())
            continue;

        Utils::FilePath fi = item->cmakeExecutable();
        if (!fi.isLocal() || fi.isExecutableFile()) { // be graceful for device related stuff
            Store tmp = item->toMap();
            if (tmp.isEmpty())
                continue;
            data.insert(numberedKey(CMAKE_TOOL_DATA_KEY, count), variantFromStore(tmp));
            ++count;
        }
    }
    data.insert(CMAKE_TOOL_COUNT_KEY, count);

    saveSettings(data, parent);
}

CMakeToolSettingsAccessor::CMakeTools
CMakeToolSettingsAccessor::cmakeTools(const Store &data, bool fromSdk) const
{
    CMakeTools result;

    int count = data.value(CMAKE_TOOL_COUNT_KEY, 0).toInt();
    for (int i = 0; i < count; ++i) {
        const Key key = numberedKey(CMAKE_TOOL_DATA_KEY, i);
        if (!data.contains(key))
            continue;

        const Store dbMap = storeFromVariant(data.value(key));
        auto item = std::make_unique<CMakeTool>(dbMap, fromSdk);
        const FilePath cmakeExecutable = item->cmakeExecutable();
        if (item->detectionSource().isAutoDetected() && cmakeExecutable.isLocal()
            && !cmakeExecutable.isExecutableFile()) {
            qWarning() << QString("CMakeTool \"%1\" (%2) dropped since the command is not executable.")
                          .arg(cmakeExecutable.toUserOutput(), item->id().toString());
            continue;
        }

        result.cmakeTools.emplace_back(std::move(item));
    }

    result.defaultToolId = Id::fromSetting(data.value(CMAKE_TOOL_DEFAULT_KEY, Id().toSetting()));

    return result;
}

} // CMakeProjectManager::Internal
