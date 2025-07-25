// Copyright (C) 2017 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "url.h"

#include "filepath.h"
#include "temporaryfile.h"

#include <QDebug>
#include <QHostAddress>
#include <QTcpServer>
#include <QUrl>

namespace Utils {

QUrl urlFromLocalHostAndFreePort()
{
    QUrl serverUrl;
    QTcpServer server;
    serverUrl.setScheme(urlTcpScheme());
    if (server.listen(QHostAddress::LocalHost) || server.listen(QHostAddress::LocalHostIPv6)) {
        serverUrl.setHost(server.serverAddress().toString());
        serverUrl.setPort(server.serverPort());
    }
    return serverUrl;
}

QUrl urlFromLocalSocket()
{
    QUrl serverUrl;
    serverUrl.setScheme(urlSocketScheme());
    TemporaryFile file("qtc-socket");
    QTC_ASSERT(file.filePath().isLocal(), qDebug() << "Only local paths supported");
    const QString fileName = file.filePath().path();
    // see "man unix" for unix socket file name size limitations
    if (fileName.size() > 104) {
        qWarning().nospace()
            << "Socket file name \"" << fileName
            << "\" is larger than 104 characters, which will not work on Darwin/macOS/Linux!";
    }
    if (file.open())
        serverUrl.setPath(fileName);
    return serverUrl;
}

QString urlSocketScheme()
{
    return QString("socket");
}

QString urlTcpScheme()
{
    return QString("tcp");
}

}
