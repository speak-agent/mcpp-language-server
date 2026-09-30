// Declarations answered without the engine (M2.3, E-LS-4): what a name a file writes names, by MC++'s
// own front end -- its name lookup (mcxx.frontend:lookup) over the file and the interfaces of the
// modules it imports, read from their sources. No BMI and no build: what an editor can answer the
// moment a project opens, while the engine is still preparing. Only what the front end is sure of is
// answered; a name it cannot resolve for certain (a member of a type it cannot tell, a name `std`
// declares, an overload it would have to choose) is the engine's.
export module mcppls.engine.native.declarations;

import std;
import nlohmann.json;
import mcxx.msa;
import mcxx.frontend;

export namespace mcppls::index {

class DeclarationIndex {
public:
    // The files of a module's interface units (its primary interface and partitions).
    using Providers = std::function<std::vector<std::string>(std::string_view module)>;

    // A file's text as it is now (an open buffer, or the disk's); its parse, for its outline too.
    const mcxx::frontend::Syntax& update(std::string_view path, std::string_view text);
    void remove(std::string_view path);
    void clear();

    // The name at `position` (LSP, UTF-16) in `path`: where what it names is defined (`definition`),
    // or declared, as LSP Location[] -- null when the front end cannot say for certain.
    nlohmann::json find(std::string_view path, const nlohmann::json& position, bool definition, const Providers& providers) const;

    // The declaration of the name at `position`, as an LSP Hover: what it is (its kind, its qualified
    // name, its type or a function's parameters, a class's bases) and where it is declared -- null when
    // the front end cannot say for certain, or its type is one deduced (the engine's to tell).
    nlohmann::json hover(std::string_view path, const nlohmann::json& position, const Providers& providers) const;

    // What may be written at `position` after a member access or a qualification (`x.`, `p->`, `S::`):
    // an LSP CompletionList of the members the front end knows -- null when it cannot tell the object's
    // class or what the qualifier names.
    nlohmann::json completion(std::string_view path, const nlohmann::json& position, const Providers& providers) const;

    // Parsed files, and name lookups answered or left to the engine (what a report shows).
    std::size_t files() const { return files_.size(); }

private:
    struct File {
        std::string path;
        std::string text;   // the syntax's tokens view it
        mcxx::frontend::Syntax syntax;
        // Its declarations as MC3 facts (what an importer of its module sees), and for each whether
        // it is a definition (a body, a class's members; a variable, an alias).
        std::vector<mcxx::msa::fact::Declaration> facts;
        std::vector<bool> defines;
    };
    std::map<std::string, std::shared_ptr<const File>, std::less<>> files_;   // by path key

    const File* file_(std::string_view path) const;
    // What `file` names but does not declare: its module's interface (an implementation unit's), what
    // it imports and what those re-export, each unit's declarations from its source; `origin`, each
    // one's file and fact.
    mcxx::frontend::Imported imported_by(const File& file, const Providers& providers,
                                         std::vector<std::pair<const File*, std::size_t>>& origin) const;
};

} // namespace mcppls::index
