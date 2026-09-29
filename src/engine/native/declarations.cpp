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

Json DeclarationIndex::find(std::string_view path, const Json& position, bool definition,
                            const std::function<std::vector<std::string>(std::string_view module)>& providers) const {
    const File* file { file_(path) };
    if (file == nullptr) return nullptr;
    const msa::Position at { mcxx::lsp::from_lsp(position, file->text) };

    // What the file names but does not declare: its module's interface (an implementation unit's),
    // what it imports, and what those re-export -- each unit's declarations from its source.
    f::Imported imported;
    std::vector<std::pair<const File*, std::size_t>> origin;   // each imported declaration's file and fact
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

} // namespace mcppls::index
