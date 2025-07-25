// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "builtineditordocumentparser.h"
#include "cppcompletionassistprocessor.h"
#include "cppcompletionassistprovider.h"
#include "cppmodelmanager.h"
#include "cppworkingcopy.h"

#include <cplusplus/Icons.h>
#include <cplusplus/Symbol.h>
#include <cplusplus/TypeOfExpression.h>

#include <texteditor/texteditor.h>
#include <texteditor/codeassist/genericproposalmodel.h>
#include <texteditor/codeassist/assistinterface.h>
#include <texteditor/codeassist/iassistprocessor.h>
#include <texteditor/snippets/snippetassistcollector.h>


#include <QStringList>
#include <QVariant>

namespace CPlusPlus {
class LookupItem;
class ClassOrNamespace;
class Function;
class LookupContext;
} // namespace CPlusPlus

namespace CppEditor::Internal {

class CppCompletionAssistInterface;

class CppAssistProposalModel : public TextEditor::GenericProposalModel
{
public:
    CppAssistProposalModel()
        : TextEditor::GenericProposalModel()
        , m_typeOfExpression(new CPlusPlus::TypeOfExpression)
    {
        m_typeOfExpression->setExpandTemplates(true);
    }

    bool isSortable(const QString &prefix) const override;
    TextEditor::AssistProposalItemInterface *proposalItem(int index) const override;

    unsigned m_completionOperator = CPlusPlus::T_EOF_SYMBOL;
    bool m_replaceDotForArrow = false;
    QSharedPointer<CPlusPlus::TypeOfExpression> m_typeOfExpression;
};

using CppAssistProposalModelPtr = QSharedPointer<CppAssistProposalModel>;

class InternalCompletionAssistProvider : public CppCompletionAssistProvider
{
    Q_OBJECT

public:
    TextEditor::IAssistProcessor *createProcessor(const TextEditor::AssistInterface *) const override;

    std::unique_ptr<TextEditor::AssistInterface> createAssistInterface(
        const Utils::FilePath &filePath,
        const TextEditor::TextEditorWidget *textEditorWidget,
        const CPlusPlus::LanguageFeatures &languageFeatures,
        TextEditor::AssistReason reason) const override;
};

class InternalCppCompletionAssistProcessor : public CppCompletionAssistProcessor
{
public:
    InternalCppCompletionAssistProcessor();
    ~InternalCppCompletionAssistProcessor() override;

    TextEditor::IAssistProposal *performAsync() override;

private:
    TextEditor::IAssistProposal *createContentProposal();
    TextEditor::IAssistProposal *createHintProposal(QList<CPlusPlus::Function *> symbols) const;
    bool accepts();

    int startOfOperator(int positionInDocument, unsigned *kind, bool wantFunctionCall) const;
    int findStartOfName(int pos = -1) const;
    int startCompletionHelper();
    bool tryObjCCompletion();
    bool objcKeywordsWanted() const;
    int startCompletionInternal(const Utils::FilePath &filePath,
                                int line, int positionInBlock,
                                const QString &expression,
                                int endOfExpression);

    void completeObjCMsgSend(CPlusPlus::ClassOrNamespace *binding, bool staticClassAccess);
    bool completeInclude(const QTextCursor &cursor);
    void completeInclude(const Utils::FilePath &realPath, const QStringList &suffixes);
    void completePreprocessor();
    bool completeConstructorOrFunction(const QList<CPlusPlus::LookupItem> &results,
                                       int endOfExpression,
                                       bool toolTipOnly);
    bool completeMember(const QList<CPlusPlus::LookupItem> &results);
    bool completeScope(const QList<CPlusPlus::LookupItem> &results);
    void completeNamespace(CPlusPlus::ClassOrNamespace *binding);
    void completeClass(CPlusPlus::ClassOrNamespace *b, bool staticLookup = true);
    void addClassMembersToCompletion(CPlusPlus::Scope *scope, bool staticLookup);
    enum CompleteQtMethodMode {
        CompleteQt4Signals,
        CompleteQt4Slots,
        CompleteQt5Signals,
        CompleteQt5Slots,
    };
    bool completeQtMethod(const QList<CPlusPlus::LookupItem> &results, CompleteQtMethodMode type);
    bool completeQtMethodClassName(const QList<CPlusPlus::LookupItem> &results,
                                   CPlusPlus::Scope *cursorScope);
    bool globalCompletion(CPlusPlus::Scope *scope);

    void addKeywordCompletionItem(const QString &text);
    void addCompletionItem(const QString &text,
                           const QIcon &icon = QIcon(),
                           int order = 0,
                           const QVariant &data = QVariant());
    void addCompletionItem(CPlusPlus::Symbol *symbol,
                           int order = 0);
    void addKeywords();
    void addMacros(const Utils::FilePath &filePath, const CPlusPlus::Snapshot &snapshot);
    void addMacros_helper(const CPlusPlus::Snapshot &snapshot,
                          const Utils::FilePath &filePath,
                          QSet<Utils::FilePath> *processed,
                          QSet<QString> *definedMacros);

    enum {
        CompleteQt5SignalOrSlotClassNameTrigger = CPlusPlus::T_LAST_TOKEN + 1,
        CompleteQt5SignalTrigger,
        CompleteQt5SlotTrigger
    };

    QScopedPointer<const CppCompletionAssistInterface> m_interface;
    const CppCompletionAssistInterface *cppInterface() const;
    CppAssistProposalModelPtr m_model;
};

class CppCompletionAssistInterface : public TextEditor::AssistInterface
{
public:
    CppCompletionAssistInterface(const Utils::FilePath &filePath,
                                 const TextEditor::TextEditorWidget *textEditorWidget,
                                 BuiltinEditorDocumentParser::Ptr parser,
                                 const CPlusPlus::LanguageFeatures &languageFeatures,
                                 TextEditor::AssistReason reason,
                                 const WorkingCopy &workingCopy)
        : TextEditor::AssistInterface(textEditorWidget->textCursor(), filePath, reason)
        , m_parser(parser)
        , m_gotCppSpecifics(false)
        , m_workingCopy(workingCopy)
        , m_languageFeatures(languageFeatures)
    {}

    CppCompletionAssistInterface(const Utils::FilePath &filePath,
                                 const TextEditor::TextEditorWidget *textEditorWidget,
                                 TextEditor::AssistReason reason,
                                 const CPlusPlus::Snapshot &snapshot,
                                 const ProjectExplorer::HeaderPaths &headerPaths,
                                 const CPlusPlus::LanguageFeatures &features)
        : TextEditor::AssistInterface(textEditorWidget->textCursor(), filePath, reason)
        , m_gotCppSpecifics(true)
        , m_snapshot(snapshot)
        , m_headerPaths(headerPaths)
        , m_languageFeatures(features)
    {}

    const CPlusPlus::Snapshot &snapshot() const { getCppSpecifics(); return m_snapshot; }
    const ProjectExplorer::HeaderPaths &headerPaths() const
    { getCppSpecifics(); return m_headerPaths; }
    CPlusPlus::LanguageFeatures languageFeatures() const
    { getCppSpecifics(); return m_languageFeatures; }
    bool isBaseObject() const override { return false; }

private:
    void getCppSpecifics() const;

    BuiltinEditorDocumentParser::Ptr m_parser;
    mutable bool m_gotCppSpecifics;
    WorkingCopy m_workingCopy;
    mutable CPlusPlus::Snapshot m_snapshot;
    mutable ProjectExplorer::HeaderPaths m_headerPaths;
    mutable CPlusPlus::LanguageFeatures m_languageFeatures;
};

} // CppEditor::Internal

Q_DECLARE_METATYPE(CPlusPlus::Symbol *)
