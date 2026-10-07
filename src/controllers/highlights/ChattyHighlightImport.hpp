#pragma once

#include <QString>

#include <vector>

namespace chatterino {

inline constexpr qsizetype CHATTY_HIGHLIGHT_IMPORT_MAX_SOURCE_CHARACTERS =
    4 * 1024 * 1024;

struct ChattyHighlightImportIssue {
    qsizetype line{};
    QString reason;
    QString source;
};

struct ChattyHighlightImportGroup {
    std::vector<QString> channels;
    std::vector<QString> regexes;
};

struct ChattyHighlightImportResult {
    std::vector<ChattyHighlightImportGroup> groups;
    std::vector<ChattyHighlightImportIssue> issues;
    qsizetype exactRules{};
    qsizetype adjustedRules{};
    qsizetype duplicateRules{};
    qsizetype invalidRules{};
    qsizetype unsupportedRules{};

    qsizetype importedRuleCount() const;
    bool hasSkippedRules() const;
};

ChattyHighlightImportResult importChattyHighlightRules(const QString &text);

bool looksLikeChattyHighlightRules(const QString &text);
bool looksLikeRegularExpressionList(const QString &text);

}
