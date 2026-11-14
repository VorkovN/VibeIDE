#include "SyntaxHighlighting.h"

#include <QEvent>
#include <QFileInfo>
#include <QHash>
#include <QPlainTextEdit>
#include <QTextBlock>

#include <array>

namespace Syntax {
enum class Language { Plain, Cpp, CMake, Make, Ninja, Meson, Python, Json, JavaScript, Jsx,
                      Markup, Css, Scss, Yaml, Toml, Shell };
}

namespace {
using Syntax::Language;
enum class Token { Keyword, Literal, Number, String, Comment, Name, Call, Directive, Count };
enum class Markup { Text, Tag, Script, Style, JsxText, JsxExpression };

Language languageFor(const QString &path)
{
    const QFileInfo file(path);
    const auto name = file.fileName().toLower();
    if (name == "cmakelists.txt")
        return Language::CMake;
    if (name == "makefile" || name == "gnumakefile")
        return Language::Make;
    if (name == ".qmake.conf")
        return Language::Make;
    if (name == "meson.build" || name == "meson.options" || name == "meson_options.txt")
        return Language::Meson;
    if (name == "build" || name == "build.bazel" || name == "workspace" || name == "workspace.bazel"
        || name == "module.bazel")
        return Language::Python;
    if (name == "uv.lock" || name == "poetry.lock" || name == "pdm.lock" || name == "cargo.lock")
        return Language::Toml;
    if (name == "pipfile")
        return Language::Toml;
    if (name == "pipfile.lock")
        return Language::Json;
    if (name == ".bashrc" || name == ".zshrc" || name == ".bash_profile")
        return Language::Shell;

    static const QHash<QString, Language> languages{
        {"c", Language::Cpp}, {"h", Language::Cpp}, {"cc", Language::Cpp}, {"hh", Language::Cpp},
        {"cpp", Language::Cpp}, {"hpp", Language::Cpp}, {"cxx", Language::Cpp}, {"hxx", Language::Cpp},
        {"ipp", Language::Cpp}, {"inl", Language::Cpp}, {"tpp", Language::Cpp}, {"ixx", Language::Cpp},
        {"cppm", Language::Cpp}, {"c++", Language::Cpp}, {"cu", Language::Cpp}, {"cuh", Language::Cpp},
        {"m", Language::Cpp}, {"mm", Language::Cpp},
        {"cmake", Language::CMake}, {"mk", Language::Make}, {"mak", Language::Make},
        {"ninja", Language::Ninja}, {"meson", Language::Meson}, {"bzl", Language::Python},
        {"pro", Language::Make}, {"pri", Language::Make}, {"prf", Language::Make},
        {"py", Language::Python}, {"pyw", Language::Python}, {"pyi", Language::Python},
        {"pyx", Language::Python}, {"pxd", Language::Python}, {"spec", Language::Python},
        {"json", Language::Json}, {"jsonc", Language::Json}, {"json5", Language::Json},
        {"js", Language::JavaScript}, {"mjs", Language::JavaScript}, {"cjs", Language::JavaScript},
        {"ts", Language::JavaScript}, {"mts", Language::JavaScript}, {"cts", Language::JavaScript},
        {"jsx", Language::Jsx}, {"tsx", Language::Jsx},
        {"html", Language::Markup}, {"htm", Language::Markup}, {"xml", Language::Markup},
        {"svg", Language::Markup}, {"vue", Language::Markup}, {"svelte", Language::Markup},
        {"astro", Language::Markup},
        {"css", Language::Css}, {"scss", Language::Scss}, {"sass", Language::Scss}, {"less", Language::Scss},
        {"yaml", Language::Yaml}, {"yml", Language::Yaml}, {"toml", Language::Toml},
        {"sh", Language::Shell}, {"bash", Language::Shell}, {"zsh", Language::Shell},
    };
    return languages.value(file.suffix().toLower(), Language::Plain);
}

auto words(std::initializer_list<std::pair<QStringView, Token>> lists)
{
    QHash<QStringView, Token> result;
    for (const auto &[list, kind] : lists) {
        for (qsizetype start = 0; start < list.size();) {
            const auto next = list.indexOf(u' ', start);
            const auto end = next < 0 ? list.size() : next;
            result.insert(list.sliced(start, end - start), kind);
            start = end + 1;
        }
    }
    return result;
}

const QHash<QStringView, Token> &keywords(Language language)
{
    using enum Token;
    static const auto cpp = words({
        {u"alignas alignof asm auto bool break case catch char char8_t char16_t char32_t class co_await co_return co_yield "
         "concept const consteval constexpr constinit const_cast continue decltype default delete do double dynamic_cast "
         "else enum explicit export extern final float for friend goto if inline int long module import mutable namespace "
         "new noexcept operator override private protected public register reinterpret_cast requires restrict return short "
         "signed sizeof static static_assert static_cast struct switch template this thread_local throw try typedef typeid "
         "typename union unsigned using virtual void volatile wchar_t while and and_eq bitand bitor compl not not_eq or or_eq "
         "xor xor_eq _Alignas _Alignof _Atomic _Bool _Complex _Generic _Noreturn _Static_assert _Thread_local", Keyword},
        {u"true false nullptr NULL", Literal},
    });
    static const auto python = words({
        {u"and as assert async await break case class continue def del elif else except finally for from global if import "
         "in is lambda match nonlocal not or pass raise return try type while with yield", Keyword},
        {u"False None True NotImplemented Ellipsis", Literal},
        {u"bool bytes dict float int list object set str tuple frozenset complex range", Name},
    });
    static const auto javascript = words({
        {u"abstract any as asserts async await bigint boolean break case catch class const constructor continue "
         "debugger declare default delete do else enum export extends finally for from function get if implements "
         "import in infer instanceof interface is keyof let module namespace never new number object of override "
         "package private protected public readonly require return satisfies set static string super switch symbol "
         "this throw try type typeof unique unknown using var void while with yield", Keyword},
        {u"true false null undefined NaN Infinity", Literal},
    });
    static const auto cmake = words({
        {u"if elseif else endif foreach endforeach while endwhile continue break return function endfunction macro "
         "endmacro block endblock include find_package project cmake_minimum_required add_executable add_library "
         "target_link_libraries target_include_directories target_compile_definitions target_compile_features "
         "target_compile_options add_subdirectory set unset option install message list string file", Keyword},
        {u"ON OFF TRUE FALSE YES NO", Literal},
        {u"PUBLIC PRIVATE INTERFACE REQUIRED QUIET CONFIG MODULE CACHE PARENT_SCOPE", Directive},
    });
    static const auto make = words({
        {u"define endef ifdef ifndef ifeq ifneq else endif include sinclude export unexport override "
         "private vpath undefine", Keyword},
    });
    static const auto ninja = words({
        {u"rule build default pool include subninja", Keyword},
        {u"command depfile deps description restat generator rspfile rspfile_content depth", Directive},
    });
    static const auto meson = words({
        {u"if elif else endif foreach endforeach break continue in not and or", Keyword},
        {u"true false", Literal},
    });
    static const auto json = words({{u"true false null Infinity NaN", Literal}});
    static const auto yaml = words({{u"true True TRUE false False FALSE null Null NULL yes Yes YES no No NO "
                                     "on On ON off Off OFF", Literal}});
    static const auto toml = words({{u"true false inf nan", Literal}});
    static const auto shell = words({
        {u"if then elif else fi for while until do done case esac in select function return local export "
         "readonly declare typeset unset source break continue eval exec shift set trap", Keyword},
        {u"true false", Literal},
    });
    static const QHash<QStringView, Token> empty;
    switch (language) {
    case Language::Cpp: return cpp;
    case Language::CMake: return cmake;
    case Language::Make: return make;
    case Language::Ninja: return ninja;
    case Language::Meson: return meson;
    case Language::Python: return python;
    case Language::Json: return json;
    case Language::JavaScript:
    case Language::Jsx: return javascript;
    case Language::Yaml: return yaml;
    case Language::Toml: return toml;
    case Language::Shell: return shell;
    default: return empty;
    }
}

using Formats = std::array<QTextCharFormat, static_cast<size_t>(Token::Count)>;

Formats formatsFor(bool dark)
{
    Formats formats;
    const auto color = [&](Token token, const char *darkColor, const char *lightColor) {
        formats[static_cast<size_t>(token)].setForeground(QColor(dark ? darkColor : lightColor));
    };
    color(Token::Keyword,   "#f6b982", "#94420e");
    color(Token::Literal,   "#f6b982", "#94420e");
    color(Token::Number,    "#a6caff", "#2458a6");
    color(Token::String,    "#a9d494", "#397033");
    color(Token::Comment,   "#a6b2c8", "#616c82");
    color(Token::Name,      "#ebbbf9", "#82429b");
    color(Token::Call,      "#9edff3", "#156479");
    color(Token::Directive, "#f6d88c", "#8a560f");
    return formats;
}

struct BlockState final : QTextBlockUserData {
    Markup markup = Markup::Text;
    Markup afterTag = Markup::Text;
    bool needsTagName = false;
    bool closingTag = false;
    int jsxDepth = 0;
    Markup afterExpression = Markup::Text;
    int braces = 0;
    Token span = Token::String;
    QString end;
    bool escaped = false;

    bool empty() const { return markup == Markup::Text && end.isEmpty(); }
    bool operator==(const BlockState &other) const
    {
        if (markup != other.markup || end != other.end || jsxDepth != other.jsxDepth)
            return false;
        if (markup == Markup::Tag && (afterTag != other.afterTag || needsTagName != other.needsTagName
                                      || closingTag != other.closingTag))
            return false;
        if (markup == Markup::JsxExpression && (braces != other.braces || afterExpression != other.afterExpression))
            return false;
        return end.isEmpty() || (span == other.span && escaped == other.escaped);
    }
};

bool isOneOf(Language language, std::initializer_list<Language> list)
{
    for (const auto candidate : list) {
        if (language == candidate)
            return true;
    }
    return false;
}

bool startsWord(QChar ch)
{
    return ch == u'_' || ch.isLetter();
}

bool wordPart(QChar ch)
{
    return ch == u'_' || ch.isLetterOrNumber() || ch.isMark();
}

bool hexDigit(QChar ch)
{
    return ch.isDigit() || (ch >= u'a' && ch <= u'f') || (ch >= u'A' && ch <= u'F');
}

template<typename Paint>
class Lexer
{
public:
    Lexer(QStringView text, Language language, const BlockState *previous, Paint paint)
        : text_(text), language_(language), paint_(paint)
    {
        if (previous)
            state = *previous;
    }

    BlockState state;

    void run()
    {
        while (position_ < text_.size()) {
            if (!state.end.isEmpty()) {
                finishSpan(position_);
            } else if (state.markup == Markup::Tag) {
                tag();
            } else if (state.markup == Markup::JsxText
                       || (state.markup == Markup::Text && language_ == Language::Markup)) {
                markupText();
            } else if ((state.markup == Markup::Script && at(u"</script", Qt::CaseInsensitive))
                       || (state.markup == Markup::Style && at(u"</style", Qt::CaseInsensitive))) {
                startTag();
            } else {
                const auto language = state.markup == Markup::Script ? Language::JavaScript
                    : state.markup == Markup::Style ? Language::Css : language_;
                code(language);
            }
        }
    }

private:
    QChar ch(qsizetype at) const { return at < text_.size() ? text_.at(at) : QChar{}; }

    bool at(QStringView prefix, Qt::CaseSensitivity sensitivity = Qt::CaseSensitive) const
    {
        return text_.sliced(position_).startsWith(prefix, sensitivity);
    }

    qsizetype skipSpace(qsizetype at) const
    {
        while (ch(at).isSpace())
            ++at;
        return at;
    }

    void mark(qsizetype start, Token token)
    {
        // A minified file can put hundreds of KiB in one QTextBlock. Its state still
        // advances to the end, but the layout only needs a bounded number of formats.
        constexpr qsizetype maxStyledColumns = 16 * 1024;
        if (start < maxStyledColumns && start < position_)
            paint_(int(start), int(qMin(position_, maxStyledColumns) - start), token);
    }

    void span(Token token, qsizetype openingLength, QStringView end, bool escaped = false)
    {
        const auto start = position_;
        position_ += openingLength;
        state.end = end.toString();
        state.span = token;
        state.escaped = escaped;
        finishSpan(start);
    }

    void finishSpan(qsizetype start)
    {
        if (state.escaped) {
            while (position_ < text_.size() && !at(state.end)) {
                if (ch(position_) == u'\\' && position_ + 1 < text_.size())
                    ++position_;
                ++position_;
            }
        } else {
            const auto end = text_.indexOf(state.end, position_);
            position_ = end < 0 ? text_.size() : end;
        }
        if (position_ < text_.size()) {
            position_ += state.end.size();
            state.end.clear();
        }
        mark(start, state.span);
    }

    void quoted(Language language, qsizetype prefix = 0)
    {
        const auto start = position_;
        position_ += prefix;
        const auto quote = ch(position_);
        if (isOneOf(language, {Language::Python, Language::Toml}) && ch(position_ + 1) == quote
            && ch(position_ + 2) == quote) {
            const auto end = text_.sliced(position_, 3);
            position_ += 3;
            state.span = Token::String;
            state.end = end.toString();
            state.escaped = language == Language::Python || quote == u'"';
            finishSpan(start);
            return;
        }
        if (quote == u'`') {
            const auto end = text_.sliced(position_, 1);
            ++position_;
            state.span = Token::String;
            state.end = end.toString();
            state.escaped = true;
            finishSpan(start);
            return;
        }
        ++position_;
        const bool escaped = quote != u'\'' || !isOneOf(language, {Language::Shell, Language::Toml});
        while (position_ < text_.size()) {
            const auto c = ch(position_++);
            if (c == quote)
                break;
            if (escaped && c == u'\\' && position_ < text_.size())
                ++position_;
        }
        const auto next = ch(skipSpace(position_));
        const bool key = (next == u':' && isOneOf(language, {Language::Json, Language::Yaml}))
            || (next == u'=' && language == Language::Toml);
        mark(start, key ? Token::Name : Token::String);
    }

    bool cmakeBracket(bool comment)
    {
        const auto bracket = position_ + (comment ? 1 : 0);
        if (ch(bracket) != u'[')
            return false;
        auto last = bracket + 1;
        while (ch(last) == u'=')
            ++last;
        if (ch(last) != u'[')
            return false;
        const QString end = u']' + text_.sliced(bracket + 1, last - bracket - 1).toString() + u']';
        span(comment ? Token::Comment : Token::String, last + 1 - position_, end);
        return true;
    }

    bool cppRawString(qsizetype start)
    {
        if (ch(position_) != u'"')
            return false;
        auto end = position_ + 1;
        while (end < position_ + 18 && ch(end) != u'(') {
            const auto c = ch(end);
            if (c.isNull() || c.isSpace() || c == u'\\' || c == u')')
                return false;
            ++end;
        }
        if (ch(end) != u'(')
            return false;
        const QString terminator = u')' + text_.sliced(position_ + 1, end - position_ - 1).toString() + u'"';
        position_ = start;
        span(Token::String, end + 1 - start, terminator);
        return true;
    }

    void startTag()
    {
        const auto start = position_;
        ++position_;
        state.closingTag = ch(position_) == u'/';
        if (state.closingTag || ch(position_) == u'!' || ch(position_) == u'?')
            ++position_;
        state.markup = Markup::Tag;
        state.afterTag = Markup::Text;
        state.needsTagName = true;
        mark(start, Token::Directive);
    }

    void enterJsxExpression()
    {
        ++position_;
        state.afterExpression = state.markup;
        state.markup = Markup::JsxExpression;
        state.braces = 1;
    }

    void tag()
    {
        const auto start = position_;
        const auto c = ch(position_);
        if (c == u'>' || at(u"/>") || at(u"?>")) {
            if (language_ == Language::Jsx) {
                if (c == u'>') {
                    if (state.closingTag)
                        state.jsxDepth = qMax(0, state.jsxDepth - 1);
                    else
                        ++state.jsxDepth;
                }
                state.afterTag = state.jsxDepth > 0 ? Markup::JsxText : Markup::Text;
            } else if (c != u'>') {
                state.afterTag = Markup::Text;
            }
            position_ += c == u'>' ? 1 : 2;
            state.markup = state.afterTag;
            mark(start, Token::Directive);
        } else if (c == u'"' || c == u'\'') {
            span(Token::String, 1, text_.sliced(position_, 1), language_ == Language::Jsx);
        } else if (c == u'{' && language_ == Language::Jsx) {
            enterJsxExpression();
        } else if (startsWord(c) || c == u'@' || c == u':') {
            do {
                ++position_;
            } while (wordPart(ch(position_)) || ch(position_) == u':' || ch(position_) == u'-'
                     || ch(position_) == u'.');
            const auto word = text_.sliced(start, position_ - start);
            if (state.needsTagName && !state.closingTag && language_ == Language::Markup) {
                if (word.compare(u"script", Qt::CaseInsensitive) == 0)
                    state.afterTag = Markup::Script;
                else if (word.compare(u"style", Qt::CaseInsensitive) == 0)
                    state.afterTag = Markup::Style;
            }
            mark(start, state.needsTagName ? Token::Keyword : Token::Name);
            state.needsTagName = false;
        } else {
            ++position_;
        }
    }

    void markupText()
    {
        const auto start = position_;
        if (at(u"<!--")) {
            span(Token::Comment, 4, u"-->");
        } else if (at(u"<![CDATA[")) {
            span(Token::String, 9, u"]]>");
        } else if (ch(position_) == u'<' && (startsWord(ch(position_ + 1)) || ch(position_ + 1) == u'/'
                                            || ch(position_ + 1) == u'!' || ch(position_ + 1) == u'?'
                                            || (language_ == Language::Jsx && ch(position_ + 1) == u'>'))) {
            startTag();
        } else if (language_ == Language::Jsx && ch(position_) == u'{') {
            enterJsxExpression();
        } else if (ch(position_) == u'&') {
            auto end = position_ + 1;
            while (end - start < 16 && (wordPart(ch(end)) || ch(end) == u'#'))
                ++end;
            position_ = ch(end) == u';' && end > start + 1 ? end + 1 : start + 1;
            if (position_ > start + 1)
                mark(start, Token::Literal);
        } else {
            ++position_;
        }
    }

    void number(Language language)
    {
        const auto start = position_;
        if (ch(position_) == u'-')
            ++position_;
        const bool hex = at(u"0x", Qt::CaseInsensitive);
        const bool binary = at(u"0b", Qt::CaseInsensitive);
        const bool octal = at(u"0o", Qt::CaseInsensitive);
        if (hex || binary || octal)
            position_ += 2;
        const auto digits = [&] {
            for (;;) {
                const auto c = ch(position_);
                const bool separator = ((c == u'_' && language != Language::Json)
                                      || (c == u'\'' && language == Language::Cpp))
                                      && ch(position_ + 1).isDigit();
                if (!(hex ? hexDigit(c) : c.isDigit()) && !separator)
                    break;
                ++position_;
            }
        };
        digits();
        if (ch(position_) == u'.' && ch(position_ + 1) != u'.') {
            ++position_;
            digits();
        }
        const auto exponent = ch(position_).toLower();
        if (exponent == (hex ? u'p' : u'e')) {
            auto end = position_ + 1;
            if (ch(end) == u'+' || ch(end) == u'-')
                ++end;
            if (ch(end).isDigit()) {
                position_ = end;
                digits();
            }
        }
        if (language == Language::Cpp) {
            while (wordPart(ch(position_)))
                ++position_;
        } else if ((language == Language::Python && ch(position_).toLower() == u'j')
                   || (isOneOf(language, {Language::JavaScript, Language::Jsx}) && ch(position_) == u'n')) {
            ++position_;
        }
        mark(start, Token::Number);
    }

    bool variable(Language language)
    {
        if (!isOneOf(language, {Language::CMake, Language::Make, Language::Ninja, Language::Shell,
                                Language::Scss}))
            return false;
        const auto start = position_;
        const auto next = ch(position_ + 1);
        if (next == u'{' || next == u'(' || (next == u'<' && language == Language::CMake)) {
            const auto close = next == u'{' ? u'}' : next == u'(' ? u')' : u'>';
            position_ += 2;
            while (position_ < text_.size() && !ch(position_).isSpace() && ch(position_) != close)
                ++position_;
            if (ch(position_) == close)
                ++position_;
        } else if (wordPart(next)) {
            do {
                ++position_;
            } while (wordPart(ch(position_)));
        } else if (!next.isNull() && QStringView(u"@*#?-$!+%^").contains(next)) {
            position_ += 2;
        } else {
            return false;
        }
        mark(start, Token::Name);
        return true;
    }

    void word(Language language)
    {
        const auto start = position_++;
        const bool css = isOneOf(language, {Language::Css, Language::Scss});
        const bool hyphenated = css || isOneOf(language, {Language::Yaml, Language::Toml});
        while (wordPart(ch(position_)) || (hyphenated && ch(position_) == u'-'))
            ++position_;
        const auto word = text_.sliced(start, position_ - start);
        const auto c = ch(position_);
        if (language == Language::Cpp && (word == u"R" || word == u"u8R" || word == u"uR"
                                        || word == u"UR" || word == u"LR") && cppRawString(start))
            return;
        if (language == Language::Python && word.size() <= 2 && (c == u'"' || c == u'\'')) {
            const auto prefix = word.toString().toLower();
            if (prefix == "r" || prefix == "u" || prefix == "b" || prefix == "f" || prefix == "t"
                || prefix == "rf" || prefix == "fr" || prefix == "rb" || prefix == "br"
                || prefix == "rt" || prefix == "tr") {
                const auto length = position_ - start;
                position_ = start;
                quoted(language, length);
                return;
            }
        }

        const auto next = ch(skipSpace(position_));
        const auto &table = keywords(language);
        auto token = table.constFind(word);
        if (token == table.cend() && language == Language::CMake && word.size() <= 32) {
            const auto lower = word.toString().toLower();
            token = table.constFind(lower);
        }
        if (token != table.cend()) {
            mark(start, token.value());
        } else if ((next == u':' && (css || isOneOf(language, {Language::Json, Language::Yaml, Language::Make})))
                   || (next == u'=' && isOneOf(language, {Language::Toml, Language::Ninja, Language::Make}))) {
            mark(start, Token::Name);
        } else if (next == u'(' && !isOneOf(language, {Language::Json, Language::Yaml, Language::Toml})) {
            mark(start, Token::Call);
        }
    }

    bool jsxTag() const
    {
        const auto next = ch(position_ + 1);
        if (next == u'/')
            return startsWord(ch(position_ + 2)) || ch(position_ + 2) == u'>';
        if (next != u'>' && !startsWord(next))
            return false;

        const auto prefix = text_.first(position_).trimmed();
        if (!prefix.isEmpty() && !QStringView(u"(=[{,?:>&|").contains(prefix.back())
            && !prefix.endsWith(u"return") && !prefix.endsWith(u"yield"))
            return false;

        auto end = position_ + 2;
        while (wordPart(ch(end)))
            ++end;
        return next == u'>' || ch(end) != u',';
    }

    void code(Language language)
    {
        const auto start = position_;
        const auto c = ch(position_);
        const auto next = ch(position_ + 1);
        const bool css = isOneOf(language, {Language::Css, Language::Scss});
        const bool slashComment = isOneOf(language, {Language::Cpp, Language::JavaScript, Language::Jsx,
                                                     Language::Scss, Language::Json});
        if (state.markup == Markup::JsxExpression && (c == u'{' || c == u'}')) {
            state.braces += c == u'{' ? 1 : -1;
            if (state.braces == 0)
                state.markup = state.afterExpression;
            ++position_;
        } else if ((slashComment || css) && at(u"/*")) {
            span(Token::Comment, 2, u"*/");
        } else if ((slashComment && at(u"//"))
                   || (c == u'#' && isOneOf(language, {Language::Python, Language::CMake, Language::Make, Language::Ninja,
                                                    Language::Meson, Language::Yaml, Language::Toml, Language::Shell})
                       && (language != Language::Shell || start == 0 || !wordPart(ch(start - 1))))) {
            if (language == Language::CMake && cmakeBracket(true))
                return;
            position_ = text_.size();
            mark(start, Token::Comment);
        } else if (language == Language::CMake && c == u'[' && cmakeBracket(false)) {
            return;
        } else if (c == u'#' && language == Language::Cpp && text_.first(start).trimmed().isEmpty()) {
            position_ = skipSpace(position_ + 1);
            while (wordPart(ch(position_)))
                ++position_;
            mark(start, Token::Directive);
            const auto operand = skipSpace(position_);
            if (ch(operand) == u'<') {
                const auto end = text_.indexOf(u'>', operand + 1);
                if (end >= 0) {
                    position_ = end + 1;
                    mark(operand, Token::String);
                }
            }
        } else if (c == u'"' || c == u'\''
                   || (c == u'`' && isOneOf(language, {Language::JavaScript, Language::Jsx, Language::Shell}))) {
            quoted(language);
        } else if (c == u'$' && variable(language)) {
            return;
        } else if (c == u'<' && language == Language::Jsx && state.markup == Markup::Text && jsxTag()) {
            startTag();
        } else if (c == u'@' && (css || isOneOf(language, {Language::Python, Language::JavaScript, Language::Jsx}))
                   && (startsWord(next) || next == u'-')) {
            do {
                ++position_;
            } while (wordPart(ch(position_)) || ch(position_) == u'-' || ch(position_) == u'.');
            mark(start, Token::Directive);
        } else if (css && c == u'#' && wordPart(next)) {
            do {
                ++position_;
            } while (wordPart(ch(position_)) || ch(position_) == u'-');
            mark(start, Token::Literal);
        } else if (language == Language::Toml && c == u'[' && text_.first(start).trimmed().isEmpty()
                   && text_.indexOf(u']', start + 1) >= 0) {
            position_ = text_.lastIndexOf(u']') + 1;
            mark(start, Token::Directive);
        } else if (c.isDigit() || ((c == u'.' || c == u'-') && next.isDigit())) {
            number(language);
        } else if (startsWord(c) || (css && c == u'-' && (startsWord(next) || next == u'-'))) {
            word(language);
        } else {
            ++position_;
        }
    }

    QStringView text_;
    Language language_;
    Paint paint_;
    qsizetype position_ = 0;
};
}

SyntaxHighlighting::SyntaxHighlighting(QPlainTextEdit *editor)
    : QSyntaxHighlighter(editor->document()), editor_(editor), language_(Language::Plain),
      dark_(editor->palette().color(QPalette::Base).lightness() < 128)
{
    editor->installEventFilter(this);
}

void SyntaxHighlighting::setFileName(const QString &path)
{
    const auto language = languageFor(path);
    if (language_ == language)
        return;
    language_ = language;
    rehighlight();
}

bool SyntaxHighlighting::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == editor_ && event->type() == QEvent::PaletteChange) {
        const bool dark = editor_->palette().color(QPalette::Base).lightness() < 128;
        if (dark_ != dark) {
            dark_ = dark;
            rehighlight();
        }
    }
    return QSyntaxHighlighter::eventFilter(watched, event);
}

void SyntaxHighlighting::highlightBlock(const QString &text)
{
    const auto *old = static_cast<const BlockState *>(currentBlockUserData());
    if (language_ == Language::Plain) {
        if (old)
            setCurrentBlockUserData(nullptr);
        setCurrentBlockState(0);
        return;
    }

    static const auto lightFormats = formatsFor(false);
    static const auto darkFormats = formatsFor(true);
    const auto &formats = dark_ ? darkFormats : lightFormats;
    const auto *previous = static_cast<const BlockState *>(currentBlock().previous().userData());
    Lexer lexer(text, language_, previous, [&](int start, int length, Token token) {
        setFormat(start, length, formats[static_cast<size_t>(token)]);
    });
    lexer.run();

    if (lexer.state.empty()) {
        if (old)
            setCurrentBlockUserData(nullptr);
        setCurrentBlockState(0);
    } else if (!old || *old != lexer.state) {
        // Qt revisits the next block when the integer changes. Toggle it even
        // when a construct stays open but its closing delimiter changes.
        setCurrentBlockUserData(new BlockState(lexer.state));
        setCurrentBlockState(currentBlockState() == 1 ? 2 : 1);
    }
}
