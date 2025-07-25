// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "qmakevfs.h"

#include "ioutils.h"
using namespace QMakeInternal;

#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <utils/filepath.h>

#define fL1S(s) QString::fromLatin1(s)

QT_BEGIN_NAMESPACE

QMakeVfs::QMakeVfs()
#ifndef PROEVALUATOR_FULL
    : m_magicMissing(fL1S("missing"))
    , m_magicExisting(fL1S("existing"))
#endif
{
    ref();
}

QMakeVfs::~QMakeVfs()
{
    deref();
}

void QMakeVfs::ref()
{
#ifdef PROEVALUATOR_THREAD_SAFE
    QMutexLocker locker(&s_mutex);
#endif
    ++s_refCount;
}

void QMakeVfs::deref()
{
#ifdef PROEVALUATOR_THREAD_SAFE
    QMutexLocker locker(&s_mutex);
#endif
    if (!--s_refCount) {
        s_fileIdCounter = 0;
        s_fileIdMap.clear();
        s_idFileMap.clear();
    }
}

#ifdef PROPARSER_THREAD_SAFE
QMutex QMakeVfs::s_mutex;
#endif
int QMakeVfs::s_refCount;
QAtomicInt QMakeVfs::s_fileIdCounter;
QHash<QString, int> QMakeVfs::s_fileIdMap;
QHash<int, QString> QMakeVfs::s_idFileMap;

int QMakeVfs::idForFileName(const QString &fn, VfsFlags flags)
{
#ifdef PROEVALUATOR_DUAL_VFS
    {
# ifdef PROPARSER_THREAD_SAFE
        QMutexLocker locker(&m_vmutex);
# endif
        int idx = (flags & VfsCumulative) ? 1 : 0;
        if (flags & VfsCreate) {
            int &id = m_virtualFileIdMap[idx][fn];
            if (!id) {
                id = ++s_fileIdCounter;
                m_virtualIdFileMap[id] = fn;
            }
            return id;
        }
        int id = m_virtualFileIdMap[idx].value(fn);
        if (id || (flags & VfsCreatedOnly))
            return id;
    }
#endif
#ifdef PROPARSER_THREAD_SAFE
    QMutexLocker locker(&s_mutex);
#endif
    if (!(flags & VfsAccessedOnly)) {
        int &id = s_fileIdMap[fn];
        if (!id) {
            id = ++s_fileIdCounter;
            s_idFileMap[id] = fn;
        }
        return id;
    }
    return s_fileIdMap.value(fn);
}

QString QMakeVfs::fileNameForId(int id)
{
#ifdef PROEVALUATOR_DUAL_VFS
    {
# ifdef PROPARSER_THREAD_SAFE
        QMutexLocker locker(&m_vmutex);
# endif
        const QString &fn = m_virtualIdFileMap.value(id);
        if (!fn.isEmpty())
            return fn;
    }
#endif
#ifdef PROPARSER_THREAD_SAFE
    QMutexLocker locker(&s_mutex);
#endif
    return s_idFileMap.value(id);
}

bool QMakeVfs::writeFile(int id, QIODevice::OpenMode mode, VfsFlags flags,
                         const QString &contents, QString *errStr)
{
#ifndef PROEVALUATOR_FULL
# ifdef PROEVALUATOR_THREAD_SAFE
    QMutexLocker locker(&m_mutex);
# endif
    QString *cont = &m_files[id];
    Q_UNUSED(flags)
    if (mode & QIODevice::Append)
        *cont += contents;
    else
        *cont = contents;
    Q_UNUSED(errStr)
    return true;
#else
    QFileInfo qfi(fileNameForId(id));
    if (!QDir::current().mkpath(qfi.path())) {
        *errStr = fL1S("Cannot create parent directory");
        return false;
    }
    QByteArray bytes = contents.toLocal8Bit();
    QFile cfile(qfi.filePath());
    if (!(mode & QIODevice::Append) && cfile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (cfile.readAll() == bytes) {
            if (flags & VfsExecutable) {
                cfile.setPermissions(cfile.permissions()
                                     | QFile::ExeUser | QFile::ExeGroup | QFile::ExeOther);
            } else {
                cfile.setPermissions(cfile.permissions()
                                     & ~(QFile::ExeUser | QFile::ExeGroup | QFile::ExeOther));
            }
            return true;
        }
        cfile.close();
    }
    if (!cfile.open(mode | QIODevice::WriteOnly | QIODevice::Text)) {
        *errStr = cfile.errorString();
        return false;
    }
    cfile.write(bytes);
    cfile.close();
    if (cfile.error() != QFile::NoError) {
        *errStr = cfile.errorString();
        return false;
    }
    if (flags & VfsExecutable)
        cfile.setPermissions(cfile.permissions()
                             | QFile::ExeUser | QFile::ExeGroup | QFile::ExeOther);
    return true;
#endif
}

QMakeVfs::ReadResult QMakeVfs::readFile(int id, QString *contents, QString *errStr)
{
    using namespace Utils;
#ifndef PROEVALUATOR_FULL
# ifdef PROEVALUATOR_THREAD_SAFE
    QMutexLocker locker(&m_mutex);
# endif
    auto it = m_files.constFind(id);
    if (it != m_files.constEnd()) {
        if (it->constData() == m_magicMissing.constData()) {
            *errStr = fL1S("No such file or directory");
            return ReadNotFound;
        }
        if (it->constData() != m_magicExisting.constData()) {
            *contents = *it;
            return ReadOk;
        }
    }
#endif

    FilePath file = FilePath::fromString(fileNameForId(id));
        if (!file.exists()){
#ifndef PROEVALUATOR_FULL
                m_files[id] = m_magicMissing;
#endif
                *errStr = fL1S("No such file or directory");
                return ReadNotFound;
        }

        Result<QByteArray> readResult = file.fileContents();
        if (!readResult){
            *errStr = readResult.error();
            return ReadOtherError;
        }

        if (readResult.value().startsWith("\xef\xbb\xbf")) {
        // UTF-8 BOM will cause subtle errors
        *errStr = fL1S("Unexpected UTF-8 BOM");
        return ReadOtherError;
    }
    *contents = QString::fromLocal8Bit(readResult.value());
    return ReadOk;
}

bool QMakeVfs::exists(const QString &device, const QString &fn, VfsFlags flags)
{
#ifndef PROEVALUATOR_FULL
# ifdef PROEVALUATOR_THREAD_SAFE
    QMutexLocker locker(&m_mutex);
# endif
    int id = idForFileName(device + fn, flags);
    auto it = m_files.constFind(id);
    if (it != m_files.constEnd())
        return it->constData() != m_magicMissing.constData();
#else
    Q_UNUSED(flags)
#endif
    bool ex = IoUtils::fileType(device, fn) == IoUtils::FileIsRegular;
#ifndef PROEVALUATOR_FULL
    m_files[id] = ex ? m_magicExisting : m_magicMissing;
#endif
    return ex;
}

#ifndef PROEVALUATOR_FULL
// This should be called when the sources may have changed (e.g., VCS update).
void QMakeVfs::invalidateCache()
{
# ifdef PROEVALUATOR_THREAD_SAFE
    QMutexLocker locker(&m_mutex);
# endif
    auto it = m_files.begin(), eit = m_files.end();
    while (it != eit) {
        if (it->constData() == m_magicMissing.constData()
                ||it->constData() == m_magicExisting.constData())
            it = m_files.erase(it);
        else
            ++it;
    }
}

// This should be called when generated files may have changed (e.g., actual build).
void QMakeVfs::invalidateContents()
{
# ifdef PROEVALUATOR_THREAD_SAFE
    QMutexLocker locker(&m_mutex);
# endif
    m_files.clear();
}
#endif

QT_END_NAMESPACE
