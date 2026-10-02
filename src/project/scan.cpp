module mcppls.project.scan;

import std;
import mcppls.base.text;
import mcppls.base.path;
import mcppls.spec.database;
import mcxx.frontend;

namespace mcppls::project {

namespace {

enum class TokenKind { identifier, punctuation, string, header_name, other };

struct Token {
    TokenKind kind { TokenKind::other };
    std::string_view text;
    std::size_t offset { 0 };
    bool startsLine { false };     // first token on its logical line
};

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\f' || c == '\v' || c == '\r'; }

// Where a byte of `text` is, as an editor shows the text: a byte order mark at its start takes no
// column (fix plan F2), so a file read from disk and the same file open in an editor get the same ranges.
base::Position position_in(std::string_view text, std::size_t offset) {
    const std::size_t mark { base::byte_order_mark_size(text) };
    return base::position_at(text.substr(mark), offset < mark ? 0 : offset - mark);
}

// The text as MC++'s own lexer reads it (mcxx.frontend, token for token Clang's raw lexer: comments,
// literals of every kind, raw strings, line splices, a byte order mark): the tokens outside comments
// and preprocessor directives, each saying whether it starts its logical line. Offsets are the text's
// own; the directives' #if nesting is counted, not evaluated -- a declaration in any branch is
// reported, marked conditional.
class Lexer {
private:
    std::string_view text_;
    std::vector<mcxx::frontend::Token> tokens_;
    std::size_t at_ { 0 };
    int conditionalDepth_ { 0 };

public:
    explicit Lexer(std::string_view text) : text_ { text }, tokens_ { mcxx::frontend::lex(text) } {}

    int conditional_depth() const { return conditionalDepth_; }

    // The next token outside comments, literals' interiors and preprocessor directives.
    std::optional<Token> next(bool wantHeaderName) {
        using K = mcxx::frontend::Kind;
        while (at_ < tokens_.size()) {
            const auto& t = tokens_[at_];
            if (t.start_of_line && t.kind == K::hash) {
                // A directive, to the end of its logical line.
                std::size_t end { at_ + 1 };
                const std::string_view name { end < tokens_.size() && !tokens_[end].start_of_line && tokens_[end].kind == K::raw_identifier
                                                  ? text_.substr(tokens_[end].begin, tokens_[end].end - tokens_[end].begin)
                                                  : std::string_view {} };
                if (name == "if" || name == "ifdef" || name == "ifndef") ++conditionalDepth_;
                if (name == "endif" && conditionalDepth_ > 0) --conditionalDepth_;
                while (end < tokens_.size() && !tokens_[end].start_of_line) ++end;
                at_ = end;
                continue;
            }
            if (wantHeaderName && t.kind == K::less) {
                // <header>: to its `>` on the same line.
                std::size_t close { at_ + 1 };
                while (close < tokens_.size() && !tokens_[close].start_of_line && tokens_[close].kind != K::greater) ++close;
                if (close < tokens_.size() && !tokens_[close].start_of_line) {
                    at_ = close + 1;
                    return Token { TokenKind::header_name, text_.substr(t.begin, tokens_[close].end - t.begin), t.begin, t.start_of_line };
                }
            }
            ++at_;
            const std::string_view spelled { text_.substr(t.begin, t.end - t.begin) };
            TokenKind kind { TokenKind::punctuation };
            switch (t.kind) {
            case K::raw_identifier: kind = TokenKind::identifier; break;
            case K::string_literal:
            case K::wide_string_literal:
            case K::utf8_string_literal:
            case K::utf16_string_literal:
            case K::utf32_string_literal: kind = TokenKind::string; break;
            case K::numeric_constant:
            case K::char_constant:
            case K::wide_char_constant:
            case K::utf8_char_constant:
            case K::utf16_char_constant:
            case K::utf32_char_constant:
            case K::unknown: kind = TokenKind::other; break;
            default: break;
            }
            return Token { kind, spelled, t.begin, t.start_of_line };
        }
        return std::nullopt;
    }
};

struct NameParse {
    std::string module;
    std::string partition;
    std::size_t begin { 0 };
    std::size_t end { 0 };
    bool ok { false };
};

// module-name: identifier ( '.' identifier )* ; partition: ':' module-name
NameParse parse_name(Lexer& lexer, std::optional<Token>& token, bool allowLeadingPartition) {
    NameParse result;
    auto read_dotted = [&](std::string& out) {
        if (!token || token->kind != TokenKind::identifier) return false;
        out.append(token->text);
        result.end = token->offset + token->text.size();
        token = lexer.next(false);
        while (token && token->kind == TokenKind::punctuation && token->text == ".") {
            token = lexer.next(false);
            if (!token || token->kind != TokenKind::identifier) return false;
            out += '.';
            out.append(token->text);
            result.end = token->offset + token->text.size();
            token = lexer.next(false);
        }
        return true;
    };
    if (!token) return result;
    result.begin = token->offset;
    if (token->kind == TokenKind::punctuation && token->text == ":") {
        if (!allowLeadingPartition) return result;
        token = lexer.next(false);
        result.ok = read_dotted(result.partition);
        return result;
    }
    if (!read_dotted(result.module)) return result;
    if (token && token->kind == TokenKind::punctuation && token->text == ":") {
        token = lexer.next(false);
        if (!read_dotted(result.partition)) return result;
    }
    result.ok = true;
    return result;
}

void skip_attributes(Lexer& lexer, std::optional<Token>& token) {
    while (token && token->kind == TokenKind::punctuation && token->text == "[") {
        int depth { 0 };
        do {
            if (token->text == "[") ++depth;
            if (token->text == "]") --depth;
            token = lexer.next(false);
        } while (token && depth > 0);
    }
}

// module-name, read leniently: whatever complete identifiers were read before the name broke off
// (a trailing dot, a token that is not an identifier, the end of the file) -- and never across a
// physical line, even if the raw token stream would otherwise happily continue past a newline (as
// scan_source's own parse_name does; that is fine there, since an incomplete name there is simply
// not recorded, but here it would make a token that spans two lines, which LSP does not allow).
// Advances `token` to wherever reading stopped, so the caller's own scan can go on from there.
std::optional<std::pair<std::size_t, std::size_t>> read_dotted_lenient(std::string_view text, Lexer& lexer, std::optional<Token>& token) {
    if (!token || token->kind != TokenKind::identifier) return std::nullopt;
    const std::size_t begin { token->offset };
    const std::size_t lineEnd { [&] {
        const auto newline = text.find('\n', begin);
        return newline == std::string_view::npos ? text.size() : newline;
    }() };
    std::size_t end { token->offset + token->text.size() };
    token = lexer.next(false);
    while (token && token->kind == TokenKind::punctuation && token->text == "." && token->offset <= lineEnd) {
        std::optional<Token> afterDot { lexer.next(false) };
        if (!afterDot || afterDot->kind != TokenKind::identifier || afterDot->offset > lineEnd) {
            token = afterDot;
            break;
        }
        end = afterDot->offset + afterDot->text.size();
        token = lexer.next(false);
    }
    return std::make_pair(begin, end);
}

} // namespace

std::vector<SyntaxToken> scan_syntax_tokens(std::string_view text) {
    std::vector<SyntaxToken> tokens;
    Lexer lexer { text };
    int braceDepth { 0 };
    // Fix plan F8: once a module declaration (`module;`, `module m;`, `export module m;`) has been read, the
    // file is a module unit, and every `export` in it begins an export declaration -- `export namespace`,
    // `export {`, `export int f()`, inside a namespace too -- and is colored like `export module`'s.
    bool moduleUnit { false };
    std::optional<Token> token { lexer.next(false) };
    const auto push = [&](SyntaxTokenKind kind, std::size_t begin, std::size_t end, bool isDeclaration = false) {
        if (end <= begin) return;
        tokens.push_back(SyntaxToken { kind, base::Range { position_in(text, begin), position_in(text, end) }, isDeclaration });
    };
    const auto push_export = [&](const Token& exportToken) {
        if (moduleUnit) push(SyntaxTokenKind::keyword, exportToken.offset, exportToken.offset + exportToken.text.size());
    };
    while (token) {
        if (token->kind == TokenKind::punctuation) {
            if (token->text == "{") ++braceDepth;
            if (token->text == "}" && braceDepth > 0) --braceDepth;
            token = lexer.next(false);
            continue;
        }
        // C++26 (P2900): `contract_assert` is a keyword wherever it is, and no editor grammar knows it yet.
        if (token->kind == TokenKind::identifier && token->text == "contract_assert") {
            push(SyntaxTokenKind::keyword, token->offset, token->offset + token->text.size());
            token = lexer.next(false);
            continue;
        }
        if (token->kind == TokenKind::identifier && token->text == "export" && (braceDepth != 0 || !token->startsLine)) {
            // Inside a namespace, or after another declaration on its line: never module syntax.
            push_export(*token);
            token = lexer.next(false);
            continue;
        }
        if (token->kind != TokenKind::identifier || braceDepth != 0) {
            token = lexer.next(false);
            continue;
        }
        bool exported { false };
        std::size_t exportBegin { 0 };
        std::size_t exportEnd { 0 };
        if (token->text == "export" && token->startsLine) {
            const Token exportToken { *token };
            exportBegin = token->offset;
            exportEnd = token->offset + token->text.size();
            token = lexer.next(false);
            if (!token || token->kind != TokenKind::identifier || (token->text != "module" && token->text != "import")) {
                push_export(exportToken);   // a declaration's export; what follows it is read as usual
                continue;
            }
            exported = true;
        } else if (!token->startsLine || (token->text != "module" && token->text != "import")) {
            token = lexer.next(false);
            continue;
        }
        const bool isImport { token->text == "import" };
        if (!isImport) moduleUnit = true;
        if (exported) push(SyntaxTokenKind::keyword, exportBegin, exportEnd);
        push(SyntaxTokenKind::keyword, token->offset, token->offset + token->text.size());
        token = lexer.next(isImport);
        if (!token) break;

        if (isImport && (token->kind == TokenKind::header_name || token->kind == TokenKind::string)) {
            // `import <header>;` / `import "header";`: the header text is a string/header-name
            // literal, not a module-type token; other layers already color it.
            token = lexer.next(false);
            continue;
        }
        if (isImport && token->kind == TokenKind::punctuation && token->text == ":") {
            // `import :partition;`: a partition of the current translation unit's own module, no
            // module name of its own.
            token = lexer.next(false);
            if (const auto partition = read_dotted_lenient(text, lexer, token)) push(SyntaxTokenKind::partitionName, partition->first, partition->second);
            continue;
        }
        const auto name = read_dotted_lenient(text, lexer, token);
        if (name) push(SyntaxTokenKind::moduleName, name->first, name->second, !isImport);
        // A colon only introduces a partition once a module name was actually read: `module
        // :private;`'s colon is the private-module-fragment syntax, not `module`'s own partition.
        if (name && token && token->kind == TokenKind::punctuation && token->text == ":") {
            token = lexer.next(false);
            if (const auto partition = read_dotted_lenient(text, lexer, token)) push(SyntaxTokenKind::partitionName, partition->first, partition->second, !isImport);
        }
    }
    return tokens;
}

ScanResult scan_source(std::string_view text) {
    ScanResult result;
    Lexer lexer { text };
    int braceDepth { 0 };
    std::optional<Token> token { lexer.next(false) };
    while (token) {
        if (token->kind == TokenKind::punctuation) {
            if (token->text == "{") ++braceDepth;
            if (token->text == "}" && braceDepth > 0) --braceDepth;
            token = lexer.next(false);
            continue;
        }
        if (token->kind != TokenKind::identifier || braceDepth != 0) {
            token = lexer.next(false);
            continue;
        }
        bool exported { false };
        if (token->text == "export" && token->startsLine) {
            exported = true;
            token = lexer.next(false);
            if (!token || token->kind != TokenKind::identifier || (token->text != "module" && token->text != "import")) continue;
        } else if (!token->startsLine || (token->text != "module" && token->text != "import")) {
            token = lexer.next(false);
            continue;
        }
        const bool isImport { token->text == "import" };
        const bool conditional { lexer.conditional_depth() > 0 };
        token = lexer.next(isImport);
        if (!token) break;

        if (isImport && (token->kind == TokenKind::header_name || token->kind == TokenKind::string)) {
            ImportDeclaration import;
            import.isExported = exported;
            import.isHeaderUnit = true;
            import.header = std::string { token->text };
            import.conditional = conditional;
            import.nameRange = base::Range { position_in(text, token->offset), position_in(text, token->offset + token->text.size()) };
            token = lexer.next(false);
            skip_attributes(lexer, token);
            if (token && token->text == ";") {
                result.imports.push_back(std::move(import));
                if (conditional) result.uncertain = true;
                token = lexer.next(false);
            }
            continue;
        }
        // A contextual keyword is a declaration only when a name or partition follows.
        const bool nameFollows { token->kind == TokenKind::identifier || (isImport && token->kind == TokenKind::punctuation && token->text == ":") };
        if (!nameFollows) continue;
        NameParse name { parse_name(lexer, token, isImport) };
        if (!name.ok) continue;
        skip_attributes(lexer, token);
        if (!token || token->kind != TokenKind::punctuation || token->text != ";") {
            // The directive's line ended first: `import hello.greet` with its `;` not typed yet.
            if (isImport && (!token || token->startsLine)) {
                const base::Range range { position_in(text, name.begin), position_in(text, name.end) };
                result.unterminatedImports.push_back(ImportDeclaration { name.module, name.partition, exported, false, {}, conditional, range });
            }
            continue;
        }
        token = lexer.next(false);

        const base::Range range { position_in(text, name.begin), position_in(text, name.end) };
        if (conditional) result.uncertain = true;
        if (isImport) {
            result.imports.push_back(ImportDeclaration { name.module, name.partition, exported, false, {}, conditional, range });
        } else if (!result.declaration) {
            result.declaration = ModuleDeclaration { name.module, name.partition, exported, conditional, range };
        } else {
            result.uncertain = true;
        }
    }
    return result;
}

spec::Role role_of(const ScanResult& result) {
    if (!result.declaration) return result.uncertain ? spec::Role::unknown : spec::Role::non_module;
    const auto& declaration = *result.declaration;
    if (declaration.conditional) return spec::Role::unknown;
    if (declaration.isExported) {
        return declaration.partition.empty() ? spec::Role::module_interface : spec::Role::module_partition_interface;
    }
    return declaration.partition.empty() ? spec::Role::module_implementation : spec::Role::module_partition_implementation;
}

std::string provided_name(const ScanResult& result) {
    if (!result.declaration) return {};
    const spec::Role role { role_of(result) };
    if (role == spec::Role::module_implementation || role == spec::Role::non_module) return {};
    const auto& declaration = *result.declaration;
    return declaration.partition.empty() ? declaration.module : declaration.module + ":" + declaration.partition;
}

std::string imported_name(const ScanResult& result, const ImportDeclaration& import) {
    if (import.isHeaderUnit) return import.header;
    if (!import.module.empty()) return import.partition.empty() ? import.module : import.module + ":" + import.partition;
    if (!result.declaration) return ":" + import.partition;
    return result.declaration->module + ":" + import.partition;
}

bool is_module_name(std::string_view name) {
    const auto dotted = [](std::string_view part) {
        if (part.empty()) return false;
        bool atStart { true };
        for (const char c : part) {
            if (c == '.') {
                if (atStart) return false;
                atStart = true;
                continue;
            }
            const bool utf8 { static_cast<unsigned char>(c) >= 0x80 };
            if (!utf8 && !base::is_identifier_char(c)) return false;
            if (atStart && c >= '0' && c <= '9') return false;
            atStart = false;
        }
        return !atStart;
    };
    const std::size_t colon { name.find(':') };
    if (colon == std::string_view::npos) return dotted(name);
    return dotted(name.substr(0, colon)) && dotted(name.substr(colon + 1));
}

std::vector<std::string> required_names(const ScanResult& result) {
    std::vector<std::string> names;
    auto add = [&](std::string name) {
        if (std::ranges::find(names, name) == names.end()) names.push_back(std::move(name));
    };
    if (result.declaration && !result.declaration->isExported && result.declaration->partition.empty()) {
        add(result.declaration->module);
    }
    for (const auto& import : result.imports) {
        if (import.isHeaderUnit) continue;
        add(imported_name(result, import));
    }
    for (const auto& import : result.unterminatedImports) add(imported_name(result, import));
    return names;
}

bool is_cxx_source_name(std::string_view path) {
    static constexpr std::array<std::string_view, 12> EXTENSIONS {
        ".cpp", ".cc", ".cxx", ".c++", ".cppm", ".ccm", ".cxxm", ".c++m", ".ixx", ".mpp", ".mxx", ".cp",
    };
    const std::string_view extension { base::extension(path) };
    return std::ranges::any_of(EXTENSIONS, [&](std::string_view candidate) { return base::iequals_ascii(candidate, extension); });
}

bool compiled_by_c_family(std::string_view source, std::span<const std::string> arguments) {
    if (is_cxx_source_name(source)) return true;
    static constexpr std::array<std::string_view, 10> OTHERS { ".c", ".m", ".mm", ".cu", ".hip", ".i", ".ii", ".mi", ".mii", ".C" };
    const std::string_view extension { base::extension(source) };
    if (std::ranges::any_of(OTHERS, [&](std::string_view candidate) { return candidate == extension || (candidate != ".C" && base::iequals_ascii(candidate, extension)); })) {
        return true;
    }
    return std::ranges::any_of(arguments, [](const std::string& argument) {
        return argument.starts_with("-x") || argument.starts_with("--language") || argument.starts_with("/Tp") || argument.starts_with("/Tc")
               || argument == "/TP" || argument == "/TC";
    });
}

} // namespace mcppls::project
