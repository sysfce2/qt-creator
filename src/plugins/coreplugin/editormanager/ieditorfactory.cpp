// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "ieditorfactory.h"
#include "ieditorfactory_p.h"
#include "editormanager.h"
#include "../coreconstants.h"

#include <utils/algorithm.h>
#include <utils/mimeconstants.h>
#include <utils/mimeutils.h>
#include <utils/qtcassert.h>

using namespace Utils;

namespace Core {

/* Find the one best matching the mimetype passed in.
 * Recurse over the parent classes of the mimetype to find them. */
static void mimeTypeFactoryLookup(const Utils::MimeType &mimeType,
                                  const QList<IEditorFactory *> &allFactories,
                                  QList<IEditorFactory *> *list)
{
    QSet<IEditorFactory *> matches;
    Utils::visitMimeParents(mimeType, [&](const Utils::MimeType &mt) -> bool {
        // check for matching factories
        for (IEditorFactory *factory : allFactories) {
            if (!matches.contains(factory)) {
                const QStringList mimeTypes = factory->mimeTypes();
                for (const QString &mimeName : mimeTypes) {
                    if (mt.matchesName(mimeName)) {
                        list->append(factory);
                        matches.insert(factory);
                    }
                }
            }
        }
        return true; // continue
    });
    // Always offer the plain text editor as a fallback for the case that the mime type
    // is not detected correctly.
    if (auto plainTextEditorFactory = Utils::findOrDefault(
            allFactories,
            Utils::equal(&IEditorFactory::id, Utils::Id(Constants::K_DEFAULT_TEXT_EDITOR_ID)))) {
        if (!matches.contains(plainTextEditorFactory))
            list->append(plainTextEditorFactory);
    }
}

/*!
    \class Core::IEditorFactory
    \inheaderfile coreplugin/editormanager/ieditorfactory.h
    \inmodule QtCreator

    \brief The IEditorFactory class creates suitable editors for documents
    according to their MIME type.

    When a user wants to edit or create a document, the EditorManager
    scans all IEditorFactory instances for suitable editors and selects one
    to create an editor.

    Implementations should set the properties of the IEditorFactory subclass in
    their constructor with IEditorFactory::setId(), IEditorFactory::setDisplayName(),
    IEditorFactory::setMimeTypes(), and IEditorFactory::setEditorCreator().

    IEditorFactory instances automatically register themselves in \QC in their
    constructor.

    There are two varieties of editors: internal and external. Internal editors
    open within the main editing area of \QC. An IEditorFactory instance defines
    an internal editor by using the setEditorCreator() function. External
    editors are external applications and are defined by using the
    setEditorStarter() function. The user can access them from the
    \uicontrol{Open With} dialog.

    \sa Core::IEditor
    \sa Core::IDocument
    \sa Core::EditorManager
*/

/*!
    \fn void Core::IEditorFactory::addMimeType(const QString &mimeType)

    Adds \a mimeType to the list of MIME types supported by this editor type.

    \sa mimeTypes()
    \sa setMimeTypes()
*/

/*!
    \fn QString Core::IEditorFactory::displayName() const

    Returns a user-visible description of the editor type.

    \sa setDisplayName()
*/

/*!
    \fn Utils::Id Core::IEditorFactory::id() const

    Returns the ID of the editors' document type.

    \sa setId()
*/

/*!
    \fn QString Core::IEditorFactory::mimeTypes() const

    Returns the list of supported MIME types of this editor type.

    \sa addMimeType()
    \sa setMimeTypes()
*/

/*!
    \fn void Core::IEditorFactory::setDisplayName(const QString &displayName)

    Sets the \a displayName of the editor type. This is for example shown in
    the \uicontrol {Open With} menu and the MIME type preferences.

    \sa displayName()
*/

/*!
    \fn void Core::IEditorFactory::setId(Utils::Id id)

    Sets the \a id of the editors' document type. This must be the same as the
    IDocument::id() of the documents returned by created editors.

    \sa id()
*/

/*!
    \fn void Core::IEditorFactory::setMimeTypes(const QStringList &mimeTypes)

    Sets the MIME types supported by the editor type to \a mimeTypes.

    \sa addMimeType()
    \sa mimeTypes()
*/

static QList<IEditorFactory *> g_editorFactories;
static QHash<QString, IEditorFactory *> g_userPreferredEditorTypes;

/*!
    Creates an IEditorFactory.

    Registers the IEditorFactory in \QC.
*/
IEditorFactory::IEditorFactory()
{
    g_editorFactories.append(this);
}

/*!
    \internal
*/
IEditorFactory::~IEditorFactory()
{
    g_editorFactories.removeOne(this);
}

/*!
    Returns all registered internal and external editors.
*/
const EditorFactories IEditorFactory::allEditorFactories()
{
    return g_editorFactories;
}

IEditorFactory *IEditorFactory::editorFactoryForId(const Utils::Id &id)
{
    return Utils::findOrDefault(allEditorFactories(), Utils::equal(&IEditorFactory::id, id));
}

/*!
    Returns all available internal and external editors for the \a mimeType in the
    default order: editor types are ordered by MIME type hierarchy, internal editors
    first.
*/
const EditorFactories IEditorFactory::defaultEditorFactories(const MimeType &mimeType)
{
    EditorFactories result;
    const EditorFactories allTypes = IEditorFactory::allEditorFactories();
    const EditorFactories allEditorFactories
        = Utils::filtered(allTypes, &IEditorFactory::isInternalEditor);
    const EditorFactories allExternalEditors
        = Utils::filtered(allTypes, &IEditorFactory::isExternalEditor);
    mimeTypeFactoryLookup(mimeType, allEditorFactories, &result);
    mimeTypeFactoryLookup(mimeType, allExternalEditors, &result);
    return result;
}

// FIXME: Consolidate with preferredEditorFactories()
const EditorFactories IEditorFactory::preferredEditorTypes(const FilePath &filePath)
{
    // default factories by mime type
    const Utils::MimeType mimeType = Utils::mimeTypeForFile(filePath,
                                                            MimeMatchMode::MatchDefaultAndRemote);
    EditorFactories factories = defaultEditorFactories(mimeType);
    // user preferred factory to front
    IEditorFactory *userPreferred = Internal::userPreferredEditorTypes().value(mimeType.name());
    if (userPreferred) {
        factories.removeAll(userPreferred);
        factories.prepend(userPreferred);
    }
    // make binary editor first internal editor for text files > 48 MB
    if (filePath.fileSize() > EditorManager::maxTextFileSize() && mimeType.inherits("text/plain")) {
        const MimeType binary = mimeTypeForName(Utils::Constants::OCTET_STREAM_MIMETYPE);
        const EditorFactories binaryEditors = defaultEditorFactories(binary);
        if (!binaryEditors.isEmpty()) {
            IEditorFactory *binaryEditor = binaryEditors.first();
            factories.removeAll(binaryEditor);
            int insertionIndex = 0;
            while (factories.size() > insertionIndex
                   && !factories.at(insertionIndex)->isInternalEditor()) {
                ++insertionIndex;
            }
            factories.insert(insertionIndex, binaryEditor);
        }
    }
    return factories;
}

/*!
    Returns the available editor factories for \a filePath in order of
    preference. That is the default order for the document's MIME type but with
    a user overridden default editor first, and the binary editor as the very
    first item if a text document is too large to be opened as a text file.
*/
const EditorFactories IEditorFactory::preferredEditorFactories(const FilePath &filePath)
{
    const auto defaultEditorFactories = [](const MimeType &mimeType) {
        QList<IEditorFactory *> editorFactories;
        for (IEditorFactory *type : IEditorFactory::defaultEditorFactories(mimeType)) {
            if (type->isInternalEditor())
                editorFactories.append(type);
        }
        return editorFactories;
    };

    // default factories by mime type
    const Utils::MimeType mimeType = Utils::mimeTypeForFile(filePath);
    EditorFactories factories = defaultEditorFactories(mimeType);
    const auto factories_moveToFront = [&factories](IEditorFactory *f) {
        factories.removeAll(f);
        factories.prepend(f);
    };
    // user preferred factory to front
    IEditorFactory *userPreferred = Internal::userPreferredEditorTypes().value(mimeType.name());
    if (userPreferred && userPreferred->isInternalEditor())
        factories_moveToFront(userPreferred);
    // open text files > 48 MB in binary editor
    if (filePath.fileSize() > EditorManager::maxTextFileSize()
            && mimeType.inherits("text/plain")) {
        const MimeType binary = mimeTypeForName(Utils::Constants::OCTET_STREAM_MIMETYPE);
        const EditorFactories binaryEditors = defaultEditorFactories(binary);
        if (!binaryEditors.isEmpty())
            factories_moveToFront(binaryEditors.first());
    }
    return factories;
}

/**
    Returns true if this factory creates internal editors.
*/
bool IEditorFactory::isInternalEditor() const
{
    return bool(m_creator);
}

/**
    Returns true if this factory creates external editors.
*/
bool IEditorFactory::isExternalEditor() const
{
    return bool(m_starter);
}

/*!
    Creates an internal editor.

    Uses the function set with setEditorCreator() to create the editor.

    \sa setEditorCreator()
*/
IEditor *IEditorFactory::createEditor() const
{
    QTC_ASSERT(m_creator, return nullptr);
    return m_creator();
}

/*!
    Opens the file at \a filePath in an external editor.

    Returns \c Utils::ResultOk on success or \c Utils::ResultError on failure.
*/
Result<> IEditorFactory::startEditor(const FilePath &filePath)
{
    QTC_ASSERT(m_starter, return ResultError(ResultAssert));
    return m_starter(filePath);
}

/*!
    Sets the function that is used to create an editor instance in
    createEditor() to \a creator.

    This is mutually exclusive with the use of setEditorStarter().

    \sa createEditor()
*/
void IEditorFactory::setEditorCreator(const std::function<IEditor *()> &creator)
{
    QTC_CHECK(!m_starter);
    // The check below triggers within the TextEditorFactory sub-hierarchy
    // as the base TextEditorFactory already sets as simple creator.
    //QTC_CHECK(!m_creator);
    m_creator = creator;
}

/*!
    \fn void Core::IEditorFactory::setEditorStarter(const std::function<Utils::Result<>(const Utils::FilePath &)> &starter)

    Sets the function that is used to open a file for a given \c FilePath to
    \a starter.

    The function should return \c true on success, or return \c false and set the
    \c QString to an error message at failure.

    This is mutually exclusive with the use of setEditorCreator().
*/
void IEditorFactory::setEditorStarter(const std::function<Result<>(const FilePath &)> &starter)
{
    QTC_CHECK(!m_starter);
    QTC_CHECK(!m_creator);
    m_starter = starter;
}

/*!
    \internal
*/
QHash<QString, IEditorFactory *> Internal::userPreferredEditorTypes()
{
    return g_userPreferredEditorTypes;
}

/*!
    \internal
*/
void Internal::setUserPreferredEditorTypes(const QHash<QString, IEditorFactory *> &factories)
{
    g_userPreferredEditorTypes = factories;
}


} // Core
