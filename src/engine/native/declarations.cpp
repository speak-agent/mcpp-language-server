module mcppls.engine.native.declarations;

import std;
import nlohmann.json;
import mcppls.base.path;
import mcppls.base.uri;
import mcxx.msa;
import mcxx.frontend;
import mcxx.lsp;

namespace mcppls::index {

using Json = nlohmann::json;
namespace f = mcxx::frontend;
namespace msa = mcxx::msa;

namespace {

// Whether a declaration is the definition an editor's "go to definition" is after: a function's,
// a class's or an enumeration's when it has its body; anything else is its own definition (a
// variable, a field, an alias, an enumerator, a namespace).
bool defines(const f::Declaration& d) {
    switch (d.kind) {
    case msa::Kind::function:
    case msa::Kind::method:
    case msa::Kind::constructor:
    case msa::Kind::destructor:
    case msa::Kind::conversion:
    case msa::Kind::class_:
    case msa::Kind::struct_:
    case msa::Kind::union_:
    case msa::Kind::enum_: return d.definition;
    default: return true;
    }
}

bool within(const msa::Range& range, msa::Position at) {
    return range.begin.line == at.line && range.begin.column <= at.column && at.column <= range.end.column;
}

std::string joined(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : std::string { separator }) + p;
    return out;
}

// A type the front end did not settle (`auto`, `decltype(...)`): what it stands for is the engine's to show.
bool deduced(std::string_view type) {
    if (type.empty()) return true;
    for (const std::string_view word : { std::string_view { "auto" }, std::string_view { "decltype" } })
        for (std::size_t at { type.find(word) }; at != std::string_view::npos; at = type.find(word, at + 1)) {
            const bool starts { at == 0 || !(std::isalnum(static_cast<unsigned char>(type[at - 1])) || type[at - 1] == '_') };
            const std::size_t end { at + word.size() };
            const bool ends { end >= type.size() || !(std::isalnum(static_cast<unsigned char>(type[end])) || type[end] == '_') };
            if (starts && ends) return true;
        }
    return false;
}

// A declaration as C++ writes it, for a hover: `int shapes::area(const Circle &, int = ...)`,
// `struct shapes::Circle : shapes::Point`, `namespace fs = std::filesystem`. None where it would
// have to show a type the front end did not settle.
std::optional<std::string> signature(const msa::fact::Declaration& d) {
    const std::string& name { d.qualified_name };
    const std::string templated { d.template_parameters.empty() ? std::string {} : std::format("template <{}>\n", joined(d.template_parameters, ", ")) };
    const auto parameters = [&] {
        if (!d.parameters) return std::string { "(...)" };
        std::vector<std::string> each;
        for (auto p : *d.parameters) each.push_back(p.ends_with(" =") ? p + " ..." : p);
        if (d.c_variadic) each.emplace_back("...");
        return "(" + joined(each, ", ") + ")";
    };
    switch (d.kind) {
    case msa::Kind::function:
    case msa::Kind::method:
        if (deduced(d.type)) return std::nullopt;
        return templated + d.type + " " + name + parameters();
    case msa::Kind::constructor:
    case msa::Kind::destructor:
    case msa::Kind::conversion: return templated + name + parameters();
    case msa::Kind::variable:
    case msa::Kind::field:
    case msa::Kind::parameter:
        if (deduced(d.type)) return std::nullopt;
        return d.type + " " + name;
    case msa::Kind::class_:
    case msa::Kind::struct_:
    case msa::Kind::union_: {
        const std::string_view key { d.kind == msa::Kind::class_ ? "class" : d.kind == msa::Kind::struct_ ? "struct" : "union" };
        return templated + std::format("{} {}", key, name) + (d.bases.empty() ? std::string {} : " : " + joined(d.bases, ", "));
    }
    case msa::Kind::enum_: return "enum " + name;
    case msa::Kind::enumerator: return name;
    case msa::Kind::type_alias:
        if (deduced(d.type)) return std::nullopt;
        return templated + "using " + name + " = " + d.type;
    case msa::Kind::namespace_: return "namespace " + name;
    case msa::Kind::namespace_alias:
        if (d.type.empty()) return std::nullopt;
        return "namespace " + name + " = " + d.type;
    default: return std::nullopt;
    }
}

// LSP's CompletionItemKind for what a member is.
int completion_kind(msa::Kind kind) {
    switch (kind) {
    case msa::Kind::method:
    case msa::Kind::conversion: return 2;   // Method
    case msa::Kind::function: return 3;     // Function
    case msa::Kind::constructor: return 4;  // Constructor
    case msa::Kind::field: return 5;        // Field
    case msa::Kind::variable: return 6;     // Variable
    case msa::Kind::class_:
    case msa::Kind::union_: return 7;       // Class
    case msa::Kind::namespace_:
    case msa::Kind::namespace_alias: return 9;   // Module
    case msa::Kind::enum_: return 13;       // Enum
    case msa::Kind::enumerator: return 20;  // EnumMember
    case msa::Kind::struct_: return 22;     // Struct
    case msa::Kind::type_alias: return 7;   // Class: what it names is a type
    case msa::Kind::concept_: return 25;    // TypeParameter
    default: return 1;                      // Text
    }
}

} // namespace

const DeclarationIndex::File* DeclarationIndex::file_(std::string_view path) const {
    const auto it = files_.find(base::path_key(path));
    return it == files_.end() ? nullptr : it->second.get();
}

const f::Syntax& DeclarationIndex::update(std::string_view path, std::string_view text) {
    auto file { std::make_shared<File>() };
    file->path = std::string { path };
    file->text = std::string { text };
    file->syntax = f::parse(file->text, { .file = file->path });
    // Its declarations as an importer sees them: names, kinds, types, bases and template parameters,
    // each matched to its declaration (the same name, the same kind) for whether it defines.
    file->facts = f::facts(file->syntax, f::Imported {}).declarations;
    std::map<std::tuple<std::uint32_t, std::uint32_t, msa::Kind>, bool> defining;
    for (const auto& d : file->syntax.declarations) {
        const auto at { f::fact_name(file->syntax, d) };
        defining.try_emplace({ at.begin.line, at.begin.column, d.kind }, defines(d));
    }
    for (const auto& fact : file->facts) {
        const auto it = defining.find({ fact.name.begin.line, fact.name.begin.column, fact.kind });
        file->defines.push_back(it != defining.end() && it->second);
    }
    auto& slot = files_[base::path_key(path)];
    slot = std::move(file);
    return slot->syntax;
}

void DeclarationIndex::remove(std::string_view path) { files_.erase(base::path_key(path)); }

void DeclarationIndex::clear() { files_.clear(); }

f::Imported DeclarationIndex::imported_by(const File& self, const Providers& providers,
                                          std::vector<std::pair<const File*, std::size_t>>& origin) const {
    const File* file { &self };
    f::Imported imported;
    const auto& pp = file->syntax.pp;
    const std::string own { pp.module.present ? pp.module.name : std::string {} };
    std::vector<std::string> queue;
    if (pp.module.present && !pp.module.exported && pp.module.partition.empty()) queue.push_back(own);
    for (const auto& import : pp.imports) queue.push_back(import.name.starts_with(':') ? own + import.name : import.name);
    std::set<std::string, std::less<>> seen;
    for (std::size_t i { 0 }; i < queue.size(); ++i) {
        const std::string name { queue[i] };
        if (name.empty() || !seen.insert(name).second) continue;
        const std::string module { name.substr(0, name.find(':')) };
        const bool same { !own.empty() && module == own };
        for (const auto& unit : providers(name)) {
            const File* g { file_(unit) };
            if (g == nullptr || g == file) continue;
            for (std::size_t k { 0 }; k < g->facts.size(); ++k) {
                const auto& d = g->facts[k];
                if (d.local || d.kind == msa::Kind::parameter || (!same && !d.exported)) continue;
                imported.declarations.push_back(d);
                origin.emplace_back(g, k);
            }
            for (const auto& reexport : g->syntax.pp.imports)
                if (reexport.exported) queue.push_back(reexport.name.starts_with(':') ? module + reexport.name : reexport.name);
        }
    }
    return imported;
}

Json DeclarationIndex::find(std::string_view path, const Json& position, bool definition, const Providers& providers) const {
    const File* file { file_(path) };
    if (file == nullptr) return nullptr;
    const msa::Position at { mcxx::lsp::from_lsp(position, file->text) };
    std::vector<std::pair<const File*, std::size_t>> origin;   // each imported declaration's file and fact
    const auto imported { imported_by(*file, providers, origin) };

    // The name at the position, if the front end is sure of what it names.
    const auto references { f::references(file->syntax, imported) };
    const auto hit = std::ranges::find_if(references, [&](const f::Reference& r) { return r.certain && within(r.range, at); });
    if (hit == references.end()) return nullptr;

    // Where it is declared: a local only where it is; anything else wherever its qualified name is
    // declared, in this file and in the interfaces read.
    struct Site {
        const File* file;
        msa::Range range;
        bool defines;
    };
    std::vector<Site> sites;
    const auto add = [&](const File* in, const msa::fact::Declaration& d, bool defining) {
        sites.push_back({ in, d.name, defining });
    };
    const bool local_target { hit->declaration >= 0 && [&] {
        const auto& d = file->syntax.declarations[static_cast<std::size_t>(hit->declaration)];
        return d.kind == msa::Kind::parameter || d.kind == msa::Kind::template_parameter || hit->target.find("::") == std::string::npos;
    }() };
    if (local_target) {
        const auto& d = file->syntax.declarations[static_cast<std::size_t>(hit->declaration)];
        sites.push_back({ file, f::fact_name(file->syntax, d), defines(d) });
    } else {
        for (std::size_t k { 0 }; k < file->facts.size(); ++k)
            if (file->facts[k].qualified_name == hit->target && file->facts[k].kind == hit->kind) add(file, file->facts[k], file->defines[k]);
        for (const auto& [g, k] : origin)
            if (g->facts[k].qualified_name == hit->target && g->facts[k].kind == hit->kind) add(g, g->facts[k], g->defines[k]);
    }
    if (definition) std::erase_if(sites, [](const Site& s) { return !s.defines; });
    // Not known here (a function defined in a unit no importer reads): the engine's to answer.
    if (sites.empty()) return nullptr;
    Json locations = Json::array();
    for (const auto& s : sites)
        locations.push_back(Json { { "uri", base::path_to_uri(s.file->path) }, { "range", mcxx::lsp::to_lsp(s.range, s.file->text) } });
    return locations;
}

Json DeclarationIndex::hover(std::string_view path, const Json& position, const Providers& providers) const {
    const File* file { file_(path) };
    if (file == nullptr) return nullptr;
    const msa::Position at { mcxx::lsp::from_lsp(position, file->text) };
    std::vector<std::pair<const File*, std::size_t>> origin;
    const auto imported { imported_by(*file, providers, origin) };
    const auto references { f::references(file->syntax, imported) };
    const auto hit = std::ranges::find_if(references, [&](const f::Reference& r) { return r.certain && within(r.range, at); });
    if (hit == references.end()) return nullptr;

    // Its declaration's facts: the file's own it names, where it is; an imported one by its qualified
    // name and kind (its definition's when a unit read has it).
    const msa::fact::Declaration* fact { nullptr };
    const File* in { nullptr };
    if (hit->declaration >= 0) {
        const auto& d = file->syntax.declarations[static_cast<std::size_t>(hit->declaration)];
        const auto name { f::fact_name(file->syntax, d) };
        for (const auto& candidate : file->facts)
            if (candidate.name == name && candidate.kind == hit->kind) {
                fact = &candidate;
                in = file;
                break;
            }
    }
    if (fact == nullptr)
        for (const auto& [g, k] : origin)
            if (g->facts[k].qualified_name == hit->target && g->facts[k].kind == hit->kind) {
                fact = &g->facts[k];
                in = g;
                if (g->defines[k]) break;
            }
    if (fact == nullptr) return nullptr;
    const auto code { signature(*fact) };
    if (!code) return nullptr;
    std::string value { std::format("```cpp\n{}\n```\n", *code) };
    if (fact->kind == msa::Kind::enumerator && !fact->type.empty()) value += std::format("\nAn enumerator of `{}`.\n", fact->type);
    if (in != file) value += std::format("\nDeclared in `{}`.\n", base::file_name(in->path));
    return Json { { "contents", Json { { "kind", "markdown" }, { "value", value } } }, { "range", mcxx::lsp::to_lsp(hit->range, file->text) } };
}

Json DeclarationIndex::completion(std::string_view path, const Json& position, const Providers& providers) const {
    const File* file { file_(path) };
    if (file == nullptr) return nullptr;
    const msa::Position at { mcxx::lsp::from_lsp(position, file->text) };
    std::vector<std::pair<const File*, std::size_t>> origin;
    const auto imported { imported_by(*file, providers, origin) };
    const auto members { f::members_at(file->syntax, imported, at) };
    if (!members) return nullptr;
    Json items = Json::array();
    for (const auto& m : *members) {
        Json item = Json::object();
        item["label"] = m.name;
        item["kind"] = completion_kind(m.kind);
        if (!m.type.empty() && !deduced(m.type)) item["detail"] = m.type;
        items.push_back(std::move(item));
    }
    return Json { { "isIncomplete", false }, { "items", std::move(items) } };
}

} // namespace mcppls::index
