#pragma once

#include <QHash>
#include <QList>
#include <QStringList>
#include <QStringView>

#include <algorithm>

namespace DiffWordHighlights {

struct Span {
    int start;
    int length;
};

struct Hunk {
    QList<QList<Span>> before;
    QList<QList<Span>> after;
};

namespace Detail {

constexpr qsizetype maxTokens = 8192;
constexpr qsizetype maxMatchCells = 256 * 1024;

struct Token {
    int symbol;
    int line;
    int start;
    int end;
};

struct CodePoint {
    char32_t value;
    int width;
};

inline CodePoint codePoint(QStringView text, int position)
{
    const auto first = text.at(position);
    if (first.isHighSurrogate() && position + 1 < text.size()) {
        const auto second = text.at(position + 1);
        if (second.isLowSurrogate())
            return {QChar::surrogateToUcs4(first, second), 2};
    }
    return {first.unicode(), 1};
}

inline bool wordCharacter(char32_t value)
{
    return value == U'_' || QChar::isLetterOrNumber(value) || QChar::isMark(value);
}

inline bool tokenize(const QStringList &lines, int start, int count, QHash<QStringView, int> &symbols,
                     QList<Token> &tokens)
{
    for (int line = 0; line < count; ++line) {
        const QStringView text = lines.at(start + line);
        int position = 0;
        while (position < text.size()) {
            const int begin = position;
            const auto character = codePoint(text, position);
            position += character.width;
            if (QChar::isSpace(character.value))
                continue;
            if (tokens.size() == maxTokens)
                return false;
            if (wordCharacter(character.value)) {
                while (position < text.size()) {
                    const auto next = codePoint(text, position);
                    if (!wordCharacter(next.value))
                        break;
                    position += next.width;
                }
            }
            const auto word = text.sliced(begin, position - begin);
            auto symbol = symbols.find(word);
            if (symbol == symbols.end())
                symbol = symbols.insert(word, int(symbols.size()));
            tokens.append({symbol.value(), line, begin, position});
        }
    }
    return true;
}

inline void appendSpans(QList<QList<Span>> &lines, const QList<Token> &tokens, qsizetype first, qsizetype end)
{
    int previousLine = -1;
    for (qsizetype index = first; index < end; ++index) {
        const auto &token = tokens.at(index);
        auto &spans = lines[token.line];
        if (token.line == previousLine)
            spans.last().length = token.end - spans.last().start;
        else
            spans.append({token.start, token.end - token.start});
        previousLine = token.line;
    }
}

} // namespace Detail

inline Hunk compare(const QStringList &before, int oldStart, int oldCount,
                    const QStringList &after, int newStart, int newCount)
{
    using namespace Detail;
    Hunk result;
    result.before.resize(oldCount);
    result.after.resize(newCount);
    if (oldCount == 0 || newCount == 0)
        return result;

    QHash<QStringView, int> symbols;
    QList<Token> oldTokens;
    QList<Token> newTokens;
    if (!tokenize(before, oldStart, oldCount, symbols, oldTokens) ||
        !tokenize(after, newStart, newCount, symbols, newTokens))
        return result;

    // Match across line boundaries so moving words onto another line keeps their soft background.
    qsizetype first = 0;
    qsizetype oldEnd = oldTokens.size();
    qsizetype newEnd = newTokens.size();
    while (first < oldEnd && first < newEnd && oldTokens.at(first).symbol == newTokens.at(first).symbol)
        ++first;
    while (first < oldEnd && first < newEnd && oldTokens.at(oldEnd - 1).symbol == newTokens.at(newEnd - 1).symbol) {
        --oldEnd;
        --newEnd;
    }

    if (oldEnd == first || newEnd == first) {
        appendSpans(result.before, oldTokens, first, oldEnd);
        appendSpans(result.after, newTokens, first, newEnd);
        return result;
    }

    const qsizetype rows = oldEnd - first + 1;
    const qsizetype columns = newEnd - first + 1;
    if (rows > maxMatchCells / columns)
        return result;

    // The table stores the length of a longest common subsequence for each remaining suffix.
    QList<quint16> lengths(rows * columns, 0);
    auto *table = lengths.data();
    for (qsizetype row = rows - 2; row >= 0; --row) {
        for (qsizetype column = columns - 2; column >= 0; --column) {
            const auto index = row * columns + column;
            const bool match = oldTokens.at(first + row).symbol == newTokens.at(first + column).symbol;
            table[index] = match ? quint16(1 + table[index + columns + 1])
                                 : std::max(table[index + columns], table[index + 1]);
        }
    }

    qsizetype oldUnmatched = first;
    qsizetype newUnmatched = first;
    qsizetype row = 0;
    qsizetype column = 0;
    while (row < rows - 1 && column < columns - 1) {
        const auto index = row * columns + column;
        if (oldTokens.at(first + row).symbol == newTokens.at(first + column).symbol) {
            appendSpans(result.before, oldTokens, oldUnmatched, first + row);
            appendSpans(result.after, newTokens, newUnmatched, first + column);
            oldUnmatched = first + ++row;
            newUnmatched = first + ++column;
        } else if (table[index + columns] >= table[index + 1]) {
            ++row;
        } else {
            ++column;
        }
    }
    appendSpans(result.before, oldTokens, oldUnmatched, oldEnd);
    appendSpans(result.after, newTokens, newUnmatched, newEnd);
    return result;
}

} // namespace DiffWordHighlights
