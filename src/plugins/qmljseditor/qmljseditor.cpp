// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "qmljseditor.h"

#include "qmljsautocompleter.h"
#include "qmljscompletionassist.h"
#include "qmljseditorconstants.h"
#include "qmljseditordocument.h"
#include "qmljseditorplugin.h"
#include "qmljseditorsettings.h"
#include "qmljseditortr.h"
#include "qmljsfindreferences.h"
#include "qmljshighlighter.h"
#include "qmljshoverhandler.h"
#include "qmljsquickfixassist.h"
#include "qmllsclientsettings.h"
#include "qmloutlinemodel.h"
#include "quicktoolbar.h"

#include <qmljs/qmljsbind.h>
#include <qmljs/qmljsevaluate.h>
#include <qmljs/qmljsmodelmanagerinterface.h>
#include <qmljs/qmljsutils.h>

#include <qmljstools/qmljsindenter.h>
#include <qmljstools/qmljstoolsconstants.h>

#include <projectexplorer/project.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/projectmanager.h>
#include <projectexplorer/projectnodes.h>
#include <projectexplorer/projecttree.h>

#include <coreplugin/actionmanager/actioncontainer.h>
#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/actionmanager/command.h>
#include <coreplugin/coreconstants.h>
#include <coreplugin/coreplugintr.h>
#include <coreplugin/designmode.h>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/icore.h>
#include <coreplugin/modemanager.h>

#include <extensionsystem/pluginmanager.h>

#include <texteditor/textdocument.h>
#include <texteditor/fontsettings.h>
#include <texteditor/tabsettings.h>
#include <texteditor/texteditorconstants.h>
#include <texteditor/texteditorsettings.h>
#include <texteditor/syntaxhighlighter.h>
#include <texteditor/refactoroverlay.h>
#include <texteditor/codeassist/genericproposal.h>
#include <texteditor/codeassist/genericproposalmodel.h>
#include <texteditor/colorpreviewhoverhandler.h>
#include <texteditor/snippets/snippetprovider.h>
#include <texteditor/textmark.h>

#include <utils/algorithm.h>
#include <utils/aspects.h>
#include <utils/changeset.h>
#include <utils/delegates.h>
#include <utils/mimeconstants.h>
#include <utils/qtcassert.h>
#include <utils/uncommentselection.h>

#include <languageclient/languageclientmanager.h>
#include <languageclient/locatorfilter.h>
#include <languageclient/languageclientsymbolsupport.h>

#include <QComboBox>
#include <QCoreApplication>
#include <QHeaderView>
#include <QMenu>
#include <QMetaMethod>
#include <QPointer>
#include <QScopedPointer>
#include <QTimer>
#include <QTreeView>
#include <QDebug>

enum {
    UPDATE_USES_DEFAULT_INTERVAL = 150,
    UPDATE_OUTLINE_INTERVAL = 500 // msecs after new semantic info has been arrived / cursor has moved
};

const char QML_JS_EDITOR_PLUGIN[] = "QmlJSEditorPlugin";
const char QT_QUICK_TOOLBAR_MARKER_ID[] = "QtQuickToolbarMarkerId";

using namespace Core;
using namespace QmlJS;
using namespace QmlJS::AST;
using namespace QmlJSEditor::Internal;
using namespace QmlJSTools;
using namespace TextEditor;
using namespace Utils;

namespace QmlJSEditor {

enum QmllsFunctionality {
    DefaultFunctionality,
    ExperimentalFunctionality,
};

static LanguageClient::Client *getQmllsClient(
    ProjectExplorer::Project *project, const FilePath &fileName, QmllsFunctionality functionality)
{
    if (functionality == ExperimentalFunctionality
        && qmllsSettings()->useQmllsWithBuiltinCodemodelOnProject(project, fileName))
        return nullptr;

    auto client = LanguageClient::LanguageClientManager::clientForFilePath(fileName);
    return client;
}

//
// QmlJSEditorWidget
//

QmlJSEditorWidget::QmlJSEditorWidget()
{
    m_findReferences = new FindReferences(this);
    setLanguageSettingsId(QmlJSTools::Constants::QML_JS_SETTINGS_ID);

    connect(this, &QmlJSEditorWidget::toolbarOutlineChanged,
            this, &QmlJSEditorWidget::updateOutline);
}

void QmlJSEditorWidget::finalizeInitialization()
{
    m_qmlJsEditorDocument = static_cast<QmlJSEditorDocument *>(textDocument());

    m_updateUsesTimer.setInterval(UPDATE_USES_DEFAULT_INTERVAL);
    m_updateUsesTimer.setSingleShot(true);
    connect(&m_updateUsesTimer, &QTimer::timeout, this, &QmlJSEditorWidget::updateUses);
    connect(this, &PlainTextEdit::cursorPositionChanged,
            &m_updateUsesTimer, QOverload<>::of(&QTimer::start));

    m_updateOutlineIndexTimer.setInterval(UPDATE_OUTLINE_INTERVAL);
    m_updateOutlineIndexTimer.setSingleShot(true);
    connect(&m_updateOutlineIndexTimer, &QTimer::timeout,
            this, &QmlJSEditorWidget::updateOutlineIndexNow);

    m_modelManager = ModelManagerInterface::instance();
    m_contextPane = QuickToolBar::instance();

    m_modelManager->activateScan();

    m_contextPaneTimer.setInterval(UPDATE_OUTLINE_INTERVAL);
    m_contextPaneTimer.setSingleShot(true);
    connect(&m_contextPaneTimer, &QTimer::timeout, this, &QmlJSEditorWidget::updateContextPane);
    if (m_contextPane) {
        connect(this, &QmlJSEditorWidget::cursorPositionChanged,
                &m_contextPaneTimer, QOverload<>::of(&QTimer::start));
        connect(m_contextPane, &QuickToolBar::closed, this, &QmlJSEditorWidget::showTextMarker);
    }

    connect(this->document(), &QTextDocument::modificationChanged,
            this, &QmlJSEditorWidget::updateModificationChange);

    connect(m_qmlJsEditorDocument, &QmlJSEditorDocument::updateCodeWarnings,
            this, &QmlJSEditorWidget::updateCodeWarnings);

    connect(m_qmlJsEditorDocument, &QmlJSEditorDocument::semanticInfoUpdated,
            this, &QmlJSEditorWidget::semanticInfoUpdated);

    setRequestMarkEnabled(true);
    createToolBar();
}

void QmlJSEditorWidget::restoreState(const QByteArray &state)
{
    using namespace Utils::Constants;
    QStringList qmlTypes = {QML_MIMETYPE, QBS_MIMETYPE, QMLTYPES_MIMETYPE, QMLUI_MIMETYPE};

    if (settings().foldAuxData() && qmlTypes.contains(textDocument()->mimeType())) {
        int version = 0;
        QDataStream stream(state);
        stream >> version;
        if (version < 1)
            foldAuxiliaryData();
    }

    TextEditorWidget::restoreState(state);
}

QModelIndex QmlJSEditorWidget::outlineModelIndex()
{
    if (!m_outlineModelIndex.isValid()) {
        m_outlineModelIndex = indexForPosition(position());
    }
    return m_outlineModelIndex;
}

static void appendExtraSelectionsForMessages(
        QList<QTextEdit::ExtraSelection> *selections,
        const QList<DiagnosticMessage> &messages,
        const QTextDocument *document)
{
    for (const DiagnosticMessage &d : messages) {
        const int line = d.loc.startLine;
        const int column = qMax(1U, d.loc.startColumn);

        QTextEdit::ExtraSelection sel;
        QTextCursor c(document->findBlockByNumber(line - 1));
        sel.cursor = c;

        sel.cursor.setPosition(c.position() + column - 1);

        if (d.loc.length == 0) {
            if (sel.cursor.atBlockEnd())
                sel.cursor.movePosition(QTextCursor::StartOfWord, QTextCursor::KeepAnchor);
            else
                sel.cursor.movePosition(QTextCursor::EndOfWord, QTextCursor::KeepAnchor);
        } else {
            sel.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, d.loc.length);
        }

        const auto fontSettings = TextEditor::TextEditorSettings::fontSettings();

        if (d.isWarning())
            sel.format = fontSettings.toTextCharFormat(TextEditor::C_WARNING);
        else
            sel.format = fontSettings.toTextCharFormat(TextEditor::C_ERROR);

        sel.format.setToolTip(d.message);

        selections->append(sel);
    }
}

void QmlJSEditorWidget::updateCodeWarnings(Document::Ptr doc)
{
    if (doc->ast()) {
        setExtraSelections(CodeWarningsSelection, QList<QTextEdit::ExtraSelection>());
    } else if (doc->language().isFullySupportedLanguage()) {
        // show parsing errors
        QList<QTextEdit::ExtraSelection> selections;
        appendExtraSelectionsForMessages(&selections, doc->diagnosticMessages(), document());
        setExtraSelections(CodeWarningsSelection, selections);
    } else {
        setExtraSelections(CodeWarningsSelection, QList<QTextEdit::ExtraSelection>());
    }
}

void QmlJSEditorWidget::foldAuxiliaryData()
{
    QTextDocument *doc = document();
    auto documentLayout = qobject_cast<TextDocumentLayout*>(doc->documentLayout());
    QTC_ASSERT(documentLayout, return);
    QTextBlock block = doc->lastBlock();

    while (block.isValid() && block.isVisible()) {
        if (TextBlockUserData::canFold(block) && block.next().isVisible()) {
            const QString trimmedText = block.text().trimmed();
            if (trimmedText.startsWith("/*##^##")) {
                TextBlockUserData::doFoldOrUnfold(block, false);
                documentLayout->requestUpdate();
                documentLayout->emitDocumentSizeChanged();
                break;
            }
        }
        block = block.previous();
    }
}

void QmlJSEditorWidget::updateModificationChange(bool changed)
{
    if (!changed && m_modelManager)
        m_modelManager->fileChangedOnDisk(textDocument()->filePath());
}

bool QmlJSEditorWidget::isOutlineCursorChangesBlocked()
{
    return hasFocus();
}

void QmlJSEditorWidget::jumpToOutlineElement(int /*index*/)
{
    if (!m_outlineCombo)
        return;
    QModelIndex index = m_outlineCombo->view()->currentIndex();
    SourceLocation location = m_qmlJsEditorDocument->outlineModel()->sourceLocation(index);

    if (!location.isValid())
        return;

    EditorManager::cutForwardNavigationHistory();
    EditorManager::addCurrentPositionToNavigationHistory();

    QTextCursor cursor = textCursor();
    cursor.setPosition(location.offset);
    setTextCursor(cursor);

    setFocus();
}

void QmlJSEditorWidget::updateOutlineIndexNow()
{
    if (!m_outlineCombo)
        return;
    if (!m_qmlJsEditorDocument->outlineModel()->document())
        return;

    if (m_qmlJsEditorDocument->outlineModel()->document()->editorRevision() != document()->revision()) {
        m_updateOutlineIndexTimer.start();
        return;
    }

    m_outlineModelIndex = QModelIndex(); // invalidate
    QModelIndex comboIndex = outlineModelIndex();
    emit outlineModelIndexChanged(m_outlineModelIndex);

    if (comboIndex.isValid()) {
        QSignalBlocker blocker(m_outlineCombo);

        // There is no direct way to select a non-root item
        m_outlineCombo->setRootModelIndex(comboIndex.parent());
        m_outlineCombo->setCurrentIndex(comboIndex.row());
        m_outlineCombo->setRootModelIndex(QModelIndex());
    }
}

void QmlJSEditorWidget::updateContextPane()
{
    const SemanticInfo info = m_qmlJsEditorDocument->semanticInfo();
    if (m_contextPane && document() && info.isValid()
            && document()->revision() == info.document->editorRevision())
    {
        Node *oldNode = info.declaringMemberNoProperties(m_oldCursorPosition);
        Node *newNode = info.declaringMemberNoProperties(position());
        if (oldNode != newNode && m_oldCursorPosition != -1)
            m_contextPane->apply(this, info.document, nullptr, newNode, false);

        if (m_contextPane->isAvailable(this, info.document, newNode) &&
            !m_contextPane->widget()->isVisible()) {
            RefactorMarkers markers;
            if (UiObjectMember *m = newNode->uiObjectMemberCast()) {
                const int start = qualifiedTypeNameId(m)->identifierToken.begin();
                for (UiQualifiedId *q = qualifiedTypeNameId(m); q; q = q->next) {
                    if (! q->next) {
                        const int end = q->identifierToken.end();
                        if (position() >= start && position() <= end) {
                            RefactorMarker marker;
                            QTextCursor tc(document());
                            tc.setPosition(end);
                            marker.cursor = tc;
                            marker.tooltip = Tr::tr("Show Qt Quick ToolBar");
                            marker.type = QT_QUICK_TOOLBAR_MARKER_ID;
                            marker.callback = [this](TextEditorWidget *) {
                                showContextPane();
                            };
                            markers.append(marker);
                        }
                    }
                }
            }
            setRefactorMarkers(markers, QT_QUICK_TOOLBAR_MARKER_ID);
        } else if (oldNode != newNode) {
            clearRefactorMarkers(QT_QUICK_TOOLBAR_MARKER_ID);
        }
        m_oldCursorPosition = position();

        setSelectedElements();
    }
}

void QmlJSEditorWidget::showTextMarker()
{
    m_oldCursorPosition = -1;
    updateContextPane();
}

void QmlJSEditorWidget::updateUses()
{
    if (m_qmlJsEditorDocument->isSemanticInfoOutdated()) // will be updated when info is updated
        return;

    QList<QTextEdit::ExtraSelection> selections;

    // code model may present the locations not in a document order
    const QList<SourceLocation> locations = Utils::sorted(
                m_qmlJsEditorDocument->semanticInfo().idLocations.value(wordUnderCursor()),
                [](const SourceLocation &lhs, const SourceLocation &rhs) {
        return lhs.begin() < rhs.begin();
    });
    for (const SourceLocation &loc : locations) {
        if (! loc.isValid())
            continue;

        QTextEdit::ExtraSelection sel;
        sel.format = textDocument()->fontSettings().toTextCharFormat(C_OCCURRENCES);
        sel.cursor = textCursor();
        sel.cursor.setPosition(loc.begin());
        sel.cursor.setPosition(loc.end(), QTextCursor::KeepAnchor);
        selections.append(sel);
    }

    setExtraSelections(CodeSemanticsSelection, selections);
}

class SelectedElement: protected Visitor
{
    unsigned m_cursorPositionStart = 0;
    unsigned m_cursorPositionEnd = 0;
    QList<UiObjectMember *> m_selectedMembers;

public:
    QList<UiObjectMember *> operator()(const Document::Ptr &doc, unsigned startPosition, unsigned endPosition)
    {
        m_cursorPositionStart = startPosition;
        m_cursorPositionEnd = endPosition;
        m_selectedMembers.clear();
        Node::accept(doc->qmlProgram(), this);
        return m_selectedMembers;
    }

protected:

    bool isSelectable(UiObjectMember *member) const
    {
        UiQualifiedId *id = qualifiedTypeNameId(member);
        if (id) {
            QStringView name = id->name;
            if (!name.isEmpty() && name.at(0).isUpper())
                return true;
        }

        return false;
    }

    inline bool isIdBinding(UiObjectMember *member) const
    {
        if (auto script = cast<const UiScriptBinding *>(member)) {
            if (! script->qualifiedId)
                return false;
            else if (script->qualifiedId->name.isEmpty())
                return false;
            else if (script->qualifiedId->next)
                return false;

            QStringView propertyName = script->qualifiedId->name;

            if (propertyName == QLatin1String("id"))
                return true;
        }

        return false;
    }

    inline bool containsCursor(unsigned begin, unsigned end)
    {
        return m_cursorPositionStart >= begin && m_cursorPositionEnd <= end;
    }

    inline bool intersectsCursor(unsigned begin, unsigned end)
    {
        return (m_cursorPositionEnd >= begin && m_cursorPositionStart <= end);
    }

    inline bool isRangeSelected() const
    {
        return (m_cursorPositionStart != m_cursorPositionEnd);
    }

    void postVisit(Node *ast) override
    {
        if (!isRangeSelected() && !m_selectedMembers.isEmpty())
            return; // nothing to do, we already have the results.

        if (UiObjectMember *member = ast->uiObjectMemberCast()) {
            unsigned begin = member->firstSourceLocation().begin();
            unsigned end = member->lastSourceLocation().end();

            if ((isRangeSelected() && intersectsCursor(begin, end))
            || (!isRangeSelected() && containsCursor(begin, end)))
            {
                if (initializerOfObject(member) && isSelectable(member)) {
                    m_selectedMembers << member;
                    // move start towards end; this facilitates multiselection so that root is usually ignored.
                    m_cursorPositionStart = qMin(end, m_cursorPositionEnd);
                }
            }
        }
    }

    void throwRecursionDepthError() override
    {
        qWarning("Warning: Hit maximum recursion depth visiting AST in SelectedElement");
    }
};

void QmlJSEditorWidget::setSelectedElements()
{
    static const QMetaMethod selectedChangedSignal =
            QMetaMethod::fromSignal(&QmlJSEditorWidget::selectedElementsChanged);
    if (!isSignalConnected(selectedChangedSignal))
        return;

    QTextCursor tc = textCursor();
    QString wordAtCursor;
    QList<UiObjectMember *> offsets;

    unsigned startPos;
    unsigned endPos;

    if (tc.hasSelection()) {
        startPos = tc.selectionStart();
        endPos = tc.selectionEnd();
    } else {
        tc.movePosition(QTextCursor::StartOfWord);
        tc.movePosition(QTextCursor::EndOfWord, QTextCursor::KeepAnchor);

        startPos = textCursor().position();
        endPos = textCursor().position();
    }

    if (m_qmlJsEditorDocument->semanticInfo().isValid()) {
        SelectedElement selectedMembers;
        const QList<UiObjectMember *> members
            = selectedMembers(m_qmlJsEditorDocument->semanticInfo().document, startPos, endPos);
        if (!members.isEmpty()) {
            for (UiObjectMember *m : members) {
                offsets << m;
            }
        }
    }
    wordAtCursor = tc.selectedText();

    emit selectedElementsChanged(offsets, wordAtCursor);
}

void QmlJSEditorWidget::applyFontSettings()
{
    TextEditorWidget::applyFontSettings();
    if (!m_qmlJsEditorDocument->isSemanticInfoOutdated())
        updateUses();
}


QString QmlJSEditorWidget::wordUnderCursor() const
{
    QTextCursor tc = textCursor();
    const QChar ch = document()->characterAt(tc.position() - 1);
    // make sure that we're not at the start of the next word.
    if (ch.isLetterOrNumber() || ch == QLatin1Char('_'))
        tc.movePosition(QTextCursor::Left);
    tc.movePosition(QTextCursor::StartOfWord);
    tc.movePosition(QTextCursor::EndOfWord, QTextCursor::KeepAnchor);
    const QString word = tc.selectedText();
    return word;
}

void QmlJSEditorWidget::createToolBar()
{
    m_outlineCombo = new QComboBox;
    m_outlineCombo->setMinimumContentsLength(22);
    m_outlineCombo->setModel(m_qmlJsEditorDocument->outlineModel());

    auto treeView = new QTreeView;

    auto itemDelegate = new Utils::AnnotatedItemDelegate(this);
    itemDelegate->setDelimiter(QLatin1String(" "));
    itemDelegate->setAnnotationRole(Internal::QmlOutlineModel::AnnotationRole);
    treeView->setItemDelegateForColumn(0, itemDelegate);

    treeView->header()->hide();
    treeView->setItemsExpandable(false);
    treeView->setRootIsDecorated(false);
    m_outlineCombo->setView(treeView);
    treeView->expandAll();

    //m_outlineCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);

    // Make the combo box prefer to expand
    QSizePolicy policy = m_outlineCombo->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Expanding);
    m_outlineCombo->setSizePolicy(policy);

    connect(m_outlineCombo, &QComboBox::activated,
            this, &QmlJSEditorWidget::jumpToOutlineElement);
    connect(m_qmlJsEditorDocument->outlineModel(), &Internal::QmlOutlineModel::updated,
            static_cast<QTreeView *>(m_outlineCombo->view()), &QTreeView::expandAll);

    connect(this, &QmlJSEditorWidget::cursorPositionChanged,
            &m_updateOutlineIndexTimer, QOverload<>::of(&QTimer::start));

    setToolbarOutline(m_outlineCombo);
}

void QmlJSEditorWidget::updateOutline(QWidget *newOutline)
{
    if (!newOutline) {
        createToolBar();
    } else if (newOutline != m_outlineCombo){
        m_outlineCombo = nullptr;
    }
}

class CodeModelInspector : public MemberProcessor
{
public:
    explicit CodeModelInspector(const CppComponentValue *processingValue, QTextStream *stream) :
        m_processingValue(processingValue),
        m_stream(stream),
        m_indent(QLatin1String("    "))
    {
    }

    bool processProperty(const QString &name, const Value *value,
                                 const PropertyInfo &propertyInfo) override
    {
        QString type;
        if (const CppComponentValue *cpp = value->asCppComponentValue())
            type = cpp->metaObject()->className();
        else
            type = m_processingValue->propertyType(name);

        if (propertyInfo.isList())
            type = QStringLiteral("list<%1>").arg(type);

        *m_stream << m_indent;
        if (!propertyInfo.isWriteable())
            *m_stream << "readonly ";
        *m_stream << "property " << type << " " << name << '\n';

        return true;
    }
    bool processSignal(const QString &name, const Value *value) override
    {
        *m_stream << m_indent << "signal " << name << stringifyFunctionParameters(value) << '\n';
        return true;
    }
    bool processSlot(const QString &name, const Value *value) override
    {
        *m_stream << m_indent << "function " << name << stringifyFunctionParameters(value) << '\n';
        return true;
    }
    bool processGeneratedSlot(const QString &name, const Value *value) override
    {
        *m_stream << m_indent << "/*generated*/ function " << name
                  << stringifyFunctionParameters(value) << '\n';
        return true;
    }

private:
    QString stringifyFunctionParameters(const Value *value) const
    {
        QStringList params;
        const QmlJS::MetaFunction *metaFunction = value->asMetaFunction();
        if (metaFunction) {
            QStringList paramNames = metaFunction->fakeMetaMethod().parameterNames();
            QStringList paramTypes = metaFunction->fakeMetaMethod().parameterTypes();
            for (int i = 0; i < paramTypes.size(); ++i) {
                QString typeAndNamePair = paramTypes.at(i);
                if (paramNames.size() > i) {
                    QString paramName = paramNames.at(i);
                    if (!paramName.isEmpty())
                        typeAndNamePair += QLatin1Char(' ') + paramName;
                }
                params.append(typeAndNamePair);
            }
        }
        return QLatin1Char('(') + params.join(QLatin1String(", ")) + QLatin1Char(')');
    }

private:
    const CppComponentValue *m_processingValue;
    QTextStream *m_stream;
    const QString m_indent;
};

static const CppComponentValue *findCppComponentToInspect(const SemanticInfo &semanticInfo,
                                                          const unsigned cursorPosition)
{
    AST::Node *node = semanticInfo.astNodeAt(cursorPosition);
    if (!node)
        return nullptr;

    const ScopeChain scopeChain = semanticInfo.scopeChain(semanticInfo.rangePath(cursorPosition));
    Evaluate evaluator(&scopeChain);
    const Value *value = evaluator.reference(node);
    if (!value)
        return nullptr;

    return value->asCppComponentValue();
}

static QString inspectCppComponent(const CppComponentValue *cppValue)
{
    QString result;
    QTextStream bufWriter(&result);

    // for QtObject
    QString superClassName = cppValue->metaObject()->superclassName();
    if (superClassName.isEmpty())
        superClassName = cppValue->metaObject()->className();

    bufWriter << "import QtQuick " << cppValue->importVersion().toString() << '\n'
              << "// " << cppValue->metaObject()->className()
              << " imported as " << cppValue->moduleName()  << " "
              << cppValue->importVersion().toString() << '\n'
              << '\n'
              << superClassName << " {" << '\n';

    CodeModelInspector insp(cppValue, &bufWriter);
    cppValue->processMembers(&insp);

    bufWriter << '\n';
    const int enumeratorCount = cppValue->metaObject()->enumeratorCount();
    for (int index = cppValue->metaObject()->enumeratorOffset(); index < enumeratorCount; ++index) {
        LanguageUtils::FakeMetaEnum enumerator = cppValue->metaObject()->enumerator(index);
        bufWriter << "    enum " << enumerator.name() << " {" << '\n';
        const QStringList keys = enumerator.keys();
        const int keysCount = keys.size();
        for (int i = 0; i < keysCount; ++i) {
            bufWriter << "        " << keys.at(i);
            if (i != keysCount - 1)
                bufWriter << ',';
            bufWriter << '\n';
        }
        bufWriter << "    }" << '\n';
    }

    bufWriter << "}" << '\n';
    return result;
}

void QmlJSEditorWidget::inspectElementUnderCursor() const
{
    const QTextCursor cursor = textCursor();

    const unsigned cursorPosition = cursor.position();
    const SemanticInfo semanticInfo = m_qmlJsEditorDocument->semanticInfo();
    if (!semanticInfo.isValid())
        return;

    const CppComponentValue *cppValue = findCppComponentToInspect(semanticInfo, cursorPosition);
    if (!cppValue) {
        QString title = Tr::tr("Code Model Not Available");
        const QString documentId = QML_JS_EDITOR_PLUGIN + QStringLiteral(".NothingToShow");
        EditorManager::openEditorWithContents(Core::Constants::K_DEFAULT_TEXT_EDITOR_ID, &title,
                                              Tr::tr("Code model not available.").toUtf8(), documentId,
                                              EditorManager::IgnoreNavigationHistory);
        return;
    }

    QString title = Tr::tr("Code Model of %1").arg(cppValue->metaObject()->className());
    const QString documentId = QML_JS_EDITOR_PLUGIN + QStringLiteral(".Class.")
                               + cppValue->metaObject()->className();
    IEditor *outputEditor = EditorManager::openEditorWithContents(
                Core::Constants::K_DEFAULT_TEXT_EDITOR_ID, &title, QByteArray(),
                documentId, EditorManager::IgnoreNavigationHistory);

    if (!outputEditor)
        return;

    auto widget = qobject_cast<TextEditor::TextEditorWidget *>(outputEditor->widget());
    if (!widget)
        return;

    widget->setReadOnly(true);
    widget->textDocument()->setTemporary(true);
    widget->textDocument()->resetSyntaxHighlighter([] { return new QmlJSHighlighter(); });

    const QString buf = inspectCppComponent(cppValue);
    widget->textDocument()->setPlainText(buf);
}

void QmlJSEditorWidget::findLinkAt(const QTextCursor &cursor,
                                   const Utils::LinkHandler &processLinkCallback,
                                   bool resolveTarget,
                                   bool /*inNextSplit*/)
{
    if (auto client = getQmllsClient(ProjectExplorer::ProjectManager::startupProject(),
                                     textDocument()->filePath(),
                                     DefaultFunctionality)) {
        client->findLinkAt(textDocument(),
                           cursor,
                           processLinkCallback,
                           resolveTarget,
                           LanguageClient::LinkTarget::SymbolDef);
        return;
    }

    const SemanticInfo semanticInfo = m_qmlJsEditorDocument->semanticInfo();
    if (! semanticInfo.isValid())
        return processLinkCallback(Utils::Link());

    const unsigned cursorPosition = cursor.position();

    AST::Node *node = semanticInfo.astNodeAt(cursorPosition);
    QTC_ASSERT(node, return;);

    if (auto importAst = cast<const AST::UiImport *>(node)) {
        // if it's a file import, link to the file
        const QList<ImportInfo> imports = semanticInfo.document->bind()->imports();
        for (const ImportInfo &import : imports) {
            if (import.ast() == importAst && import.type() == ImportType::File) {
                Utils::Link link(
                    m_modelManager->fileToSource(FilePath::fromString(import.path())));
                link.linkTextStart = importAst->firstSourceLocation().begin();
                link.linkTextEnd = importAst->lastSourceLocation().end();
                processLinkCallback(Utils::Link());
                return;
            }
        }
        processLinkCallback(Utils::Link());
        return;
    }

    const ProjectExplorer::Project * const project = ProjectExplorer::ProjectTree::currentProject();
    ProjectExplorer::ProjectNode* projectRootNode = nullptr;
    if (project) {
        projectRootNode = project->rootProjectNode();
    }

    // string literals that could refer to a file link to them
    if (auto literal = cast<const StringLiteral *>(node)) {
        const QString &text = literal->value.toString();
        if (text.startsWith("qrc:/")) {
            if (projectRootNode) {
                const ProjectExplorer::Node * const nodeForPath = projectRootNode->findNode(
                            [qrcPath = text.mid(text.indexOf(':') + 1)](ProjectExplorer::Node *n) {
                    if (!n->asFileNode())
                        return false;
                    const auto qrcNode = dynamic_cast<ProjectExplorer::ResourceFileNode *>(n);
                    return qrcNode && qrcNode->qrcPath() == qrcPath;
                });
                if (nodeForPath) {
                    Link link(nodeForPath->filePath());
                    link.linkTextStart = literal->firstSourceLocation().begin();
                    link.linkTextEnd = literal->lastSourceLocation().end();
                    processLinkCallback(link);
                    return;
                }
            }
        }

        if (text.startsWith("https:/") || text.startsWith("http:/")) {
            Link link = Link::fromString(text);
            link.linkTextStart = literal->literalToken.begin();
            link.linkTextEnd = literal->literalToken.end();
            processLinkCallback(link);
            return;
        }

        Utils::Link link;
        link.linkTextStart = literal->literalToken.begin();
        link.linkTextEnd = literal->literalToken.end();
        Utils::FilePath targetFilePath = Utils::FilePath::fromUserInput(text);
        if (semanticInfo.snapshot.document(targetFilePath)) {
            link.targetFilePath = targetFilePath;
            processLinkCallback(link);
            return;
        }
        const Utils::FilePath relative = semanticInfo.document->path().pathAppended(text);
        if (relative.exists()) {
            link.targetFilePath = m_modelManager->fileToSource(relative);
            processLinkCallback(link);
            return;
        }
    }

    const ScopeChain scopeChain = semanticInfo.scopeChain(semanticInfo.rangePath(cursorPosition));
    Evaluate evaluator(&scopeChain);
    const Value *value = evaluator.reference(node);

    Utils::FilePath fileName;
    int line = 0, column = 0;

    if (! (value && value->getSourceLocation(&fileName, &line, &column)))
        return processLinkCallback(Utils::Link());

    Utils::Link link;
    link.targetFilePath = m_modelManager->fileToSource(fileName);
    link.target.line = line;
    link.target.column = column - 1; // adjust the column

    auto processPotentialCppLink = [&]() -> bool {
        if (!value->asCppComponentValue() || !projectRootNode) {
            processLinkCallback(link);
            return true;
        }

        const ProjectExplorer::Node * const nodeForPath = projectRootNode->findNode(
            [&fileName](ProjectExplorer::Node *n) {
                const auto fileNode = n->asFileNode();
                if (!fileNode)
                    return false;
                Utils::FilePath filePath = n->filePath();
                return filePath.endsWith(fileName.toUserOutput());
            });
        if (nodeForPath) {
            link.targetFilePath = nodeForPath->filePath();
            processLinkCallback(link);
            return true;
        }

        // else we will process an empty link below to avoid an error dialog
        return false;
    };

    if (auto q = AST::cast<const AST::UiQualifiedId *>(node)) {
        for (const AST::UiQualifiedId *tail = q; tail; tail = tail->next) {
            if (tail->next || !(cursorPosition <= tail->identifierToken.end())) {
                continue;
            }

            link.linkTextStart = tail->identifierToken.begin();
            link.linkTextEnd = tail->identifierToken.end();

            if (processPotentialCppLink()) {
                return;
            }
        }
    } else if (auto id = AST::cast<const AST::IdentifierExpression *>(node)) {
        link.linkTextStart = id->firstSourceLocation().begin();
        link.linkTextEnd = id->lastSourceLocation().end();

        if (processPotentialCppLink()) {
            return;
        }

    } else if (auto mem = AST::cast<const AST::FieldMemberExpression *>(node)) {
        link.linkTextStart = mem->lastSourceLocation().begin();
        link.linkTextEnd = mem->lastSourceLocation().end();
        processLinkCallback(link);
        return;
    }

    processLinkCallback(Utils::Link());
}

void QmlJSEditorWidget::findUsages()
{
    const Utils::FilePath fileName = textDocument()->filePath();

    if (auto client = getQmllsClient(ProjectExplorer::ProjectManager::startupProject(),
                                     fileName, DefaultFunctionality)) {
        client->symbolSupport().findUsages(textDocument(), textCursor());
    } else {
        const int offset = textCursor().position();
        m_findReferences->findUsages(fileName, offset);
    }
}

void QmlJSEditorWidget::renameSymbolUnderCursor()
{
    const Utils::FilePath fileName = textDocument()->filePath();

    if (auto client = getQmllsClient(ProjectExplorer::ProjectManager::startupProject(),
                                     fileName, DefaultFunctionality)) {
        client->symbolSupport().renameSymbol(textDocument(), textCursor(), QString());
    } else {
        const int offset = textCursor().position();
        m_findReferences->renameUsages(fileName, offset);
    }
}

void QmlJSEditorWidget::showContextPane()
{
    const SemanticInfo info = m_qmlJsEditorDocument->semanticInfo();
    if (m_contextPane && info.isValid()) {
        Node *newNode = info.declaringMemberNoProperties(position());
        ScopeChain scopeChain = info.scopeChain(info.rangePath(position()));
        m_contextPane->apply(this, info.document,
                             &scopeChain,
                             newNode, false, true);
        m_oldCursorPosition = position();
        clearRefactorMarkers(QT_QUICK_TOOLBAR_MARKER_ID);
    }
}

void QmlJSEditorWidget::contextMenuEvent(QContextMenuEvent *e)
{
    QPointer<QMenu> menu(new QMenu(this));

    QMenu *refactoringMenu = new QMenu(Tr::tr("Refactoring"), menu);

    if (!m_qmlJsEditorDocument->isSemanticInfoOutdated()) {
        std::unique_ptr<AssistInterface> interface = createAssistInterface(QuickFix, ExplicitlyInvoked);
        if (interface) {
            QScopedPointer<IAssistProcessor> processor(
                Internal::quickFixAssistProvider()->createProcessor(interface.get()));
            QScopedPointer<IAssistProposal> proposal(processor->start(std::move(interface)));
            if (!proposal.isNull()) {
                GenericProposalModelPtr model = proposal->model().staticCast<GenericProposalModel>();
                for (int index = 0; index < model->size(); ++index) {
                    auto item = static_cast<const AssistProposalItem *>(model->proposalItem(index));
                    QuickFixOperation::Ptr op = item->data().value<QuickFixOperation::Ptr>();
                    QAction *action = refactoringMenu->addAction(op->description());
                    connect(action, &QAction::triggered, this, [op]() { op->perform(); });
                }
            }
        }
    }

    refactoringMenu->setEnabled(!refactoringMenu->isEmpty());

    if (ActionContainer *mcontext = ActionManager::actionContainer(Constants::M_CONTEXT)) {
        QMenu *contextMenu = mcontext->menu();
        const QList<QAction *> actions = contextMenu->actions();
        for (QAction *action : actions) {
            menu->addAction(action);
            if (action->objectName() == QLatin1String(Constants::M_REFACTORING_MENU_INSERTION_POINT))
                menu->addMenu(refactoringMenu);
            if (action->objectName() == QLatin1String(Constants::SHOW_QT_QUICK_HELPER)) {
                bool enabled = m_contextPane->isAvailable(
                            this, m_qmlJsEditorDocument->semanticInfo().document,
                            m_qmlJsEditorDocument->semanticInfo().declaringMemberNoProperties(position()));
                action->setEnabled(enabled);
            }
        }
    }

    appendStandardContextMenuActions(menu);

    menu->exec(e->globalPos());
    delete menu;
}

bool QmlJSEditorWidget::event(QEvent *e)
{
    switch (e->type()) {
    case QEvent::ShortcutOverride:
        if (static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape && m_contextPane) {
            if (hideContextPane()) {
                e->accept();
                return true;
            }
        }
        break;
    default:
        break;
    }

    return TextEditorWidget::event(e);
}


void QmlJSEditorWidget::wheelEvent(QWheelEvent *event)
{
    bool visible = false;
    if (m_contextPane && m_contextPane->widget()->isVisible())
        visible = true;

    TextEditorWidget::wheelEvent(event);

    if (visible)
        m_contextPane->apply(this, m_qmlJsEditorDocument->semanticInfo().document, nullptr,
                             m_qmlJsEditorDocument->semanticInfo().declaringMemberNoProperties(m_oldCursorPosition),
                             false, true);
}

void QmlJSEditorWidget::resizeEvent(QResizeEvent *event)
{
    TextEditorWidget::resizeEvent(event);
    hideContextPane();
}

 void QmlJSEditorWidget::scrollContentsBy(int dx, int dy)
 {
     TextEditorWidget::scrollContentsBy(dx, dy);
     hideContextPane();
 }

QmlJSEditorDocument *QmlJSEditorWidget::qmlJsEditorDocument() const
{
    return m_qmlJsEditorDocument;
}

void QmlJSEditorWidget::semanticInfoUpdated(const SemanticInfo &semanticInfo)
{
    if (isVisible()) {
         // trigger semantic highlighting and model update if necessary
        textDocument()->triggerPendingUpdates();
    }

    if (m_contextPane) {
        Node *newNode = semanticInfo.declaringMemberNoProperties(position());
        if (newNode) {
            m_contextPane->apply(this, semanticInfo.document, nullptr, newNode, true);
            m_contextPaneTimer.start(); //update text marker
        }
    }

    updateUses();
}

QModelIndex QmlJSEditorWidget::indexForPosition(unsigned cursorPosition, const QModelIndex &rootIndex) const
{
    QModelIndex lastIndex = rootIndex;

    Internal::QmlOutlineModel *model = m_qmlJsEditorDocument->outlineModel();
    const int rowCount = model->rowCount(rootIndex);
    for (int i = 0; i < rowCount; ++i) {
        QModelIndex childIndex = model->index(i, 0, rootIndex);
        SourceLocation location = model->sourceLocation(childIndex);

        if ((cursorPosition >= location.offset)
              && (cursorPosition <= location.offset + location.length)) {
            lastIndex = childIndex;
            break;
        }
    }

    if (lastIndex != rootIndex) {
        // recurse
        lastIndex = indexForPosition(cursorPosition, lastIndex);
    }
    return lastIndex;
}

bool QmlJSEditorWidget::hideContextPane()
{
    bool b = (m_contextPane) && m_contextPane->widget()->isVisible();
    if (b)
        m_contextPane->apply(this, m_qmlJsEditorDocument->semanticInfo().document,
                             nullptr, nullptr, false);
    return b;
}

std::unique_ptr<AssistInterface> QmlJSEditorWidget::createAssistInterface(
    AssistKind assistKind,
    AssistReason reason) const
{
    if (assistKind == Completion) {
        return std::make_unique<QmlJSCompletionAssistInterface>(
            textCursor(), textDocument()->filePath(), reason, m_qmlJsEditorDocument->semanticInfo());
    } else if (assistKind == QuickFix) {
        return std::make_unique<Internal::QmlJSQuickFixAssistInterface>(
            const_cast<QmlJSEditorWidget *>(this), reason);
    }
    return TextEditorWidget::createAssistInterface(assistKind, reason);
}

QString QmlJSEditorWidget::foldReplacementText(const QTextBlock &block) const
{
    const int curlyIndex = block.text().indexOf(QLatin1Char('{'));

    if (curlyIndex != -1 && m_qmlJsEditorDocument->semanticInfo().isValid()) {
        const int pos = block.position() + curlyIndex;
        Node *node = m_qmlJsEditorDocument->semanticInfo().rangeAt(pos);

        const QString objectId = idOfObject(node);
        if (!objectId.isEmpty())
            return QLatin1String("id: ") + objectId + QLatin1String("...");
    }

    return TextEditorWidget::foldReplacementText(block);
}


//
// QmlJSEditor
//

QmlJSEditor::QmlJSEditor()
{
    addContext(ProjectExplorer::Constants::QMLJS_LANGUAGE_ID);
}

QmlJSEditorDocument *QmlJSEditor::qmlJSDocument() const
{
    return qobject_cast<QmlJSEditorDocument *>(document());
}

bool QmlJSEditor::isDesignModePreferred() const
{

    // stay in design mode if we are there
    const Id mode = ModeManager::currentModeId();
    return qmlJSDocument()->isDesignModePreferred() || mode == Core::Constants::MODE_DESIGN;
}

//
// QmlJSEditorFactory
//

QmlJSEditorFactory::QmlJSEditorFactory()
    : QmlJSEditorFactory(Constants::C_QMLJSEDITOR_ID)
{}

QmlJSEditorFactory::QmlJSEditorFactory(Utils::Id _id)
{
    setId(_id);
    setDisplayName(::Core::Tr::tr("QMLJS Editor"));

    using namespace Utils::Constants;
    addMimeType(QML_MIMETYPE);
    addMimeType(QMLPROJECT_MIMETYPE);
    addMimeType(QMLTYPES_MIMETYPE);
    addMimeType(JS_MIMETYPE);

    setDocumentCreator([this]() { return new QmlJSEditorDocument(id()); });
    setEditorWidgetCreator([]() { return new QmlJSEditorWidget; });
    setEditorCreator([]() { return new QmlJSEditor; });
    setAutoCompleterCreator([]() { return new AutoCompleter; });
    setCommentDefinition(Utils::CommentDefinition::CppStyle);
    setParenthesesMatchingEnabled(true);
    setCodeFoldingSupported(true);

    addHoverHandler(new QmlJSHoverHandler);
    addHoverHandler(new ColorPreviewHoverHandler);
    setCompletionAssistProvider(new QmlJSCompletionAssistProvider);

    setOptionalActionMask(OptionalActions::Format
                            | OptionalActions::UnCommentSelection
                            | OptionalActions::UnCollapseAll
                            | OptionalActions::FollowSymbolUnderCursor
                            | OptionalActions::RenameSymbol
                            | OptionalActions::FindUsage);
}

static void decorateEditor(TextEditorWidget *editor)
{
    editor->textDocument()->resetSyntaxHighlighter([] { return new QmlJSHighlighter(); });
    editor->textDocument()->setIndenter(createQmlJsIndenter(editor->textDocument()->document()));
    editor->setAutoCompleter(new AutoCompleter);
}

namespace Internal {

void inspectElement()
{
    if (auto widget = qobject_cast<QmlJSEditorWidget *>(EditorManager::currentEditor()->widget()))
        widget->inspectElementUnderCursor();
}

void showContextPane()
{
    if (auto editor = qobject_cast<QmlJSEditorWidget*>(EditorManager::currentEditor()->widget()))
        editor->showContextPane();
}

void setupQmlJSEditor()
{
    static QmlJSEditorFactory theQmlJSEditorFactory;

    TextEditor::SnippetProvider::registerGroup(Constants::QML_SNIPPETS_GROUP_ID,
                                               Tr::tr("QML", "SnippetProvider"),
                                               &decorateEditor);

}

} // namespace Internal

QdsSettings::QdsSettings()
{
    connect(&settings().qdsCommand, &FilePathAspect::changed, this, &QdsSettings::changed);
}

void QdsSettings::setQdsSettingVisible(bool visible)
{
    Internal::settings().qdsCommand.setVisible(visible);
}

FilePath QdsSettings::qdsCommand()
{
    const FilePath command = Internal::settings().qdsCommand.effectiveBinary();
    if (command.isEmpty())
        return Internal::settings().defaultQdsCommand();
    return command;
}

QdsSettings &qdsSettings()
{
    static QdsSettings settings;
    return settings;
}

// namespace Internal

} // namespace QmlJSEditor
