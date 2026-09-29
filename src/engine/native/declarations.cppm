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
    // A file's text as it is now (an open buffer, or the disk's); its parse, for its outline too.
    const mcxx::frontend::Syntax& update(std::string_view path, std::string_view text);
    void remove(std::string_view path);
    void clear();

    // The name at `position` (LSP, UTF-16) in `path`: where what it names is defined (`definition`),
    // or declared, as LSP Location[] -- null when the front end cannot say for certain. `providers`:
    // the files of a module's interface units (its primary interface and partitions).
    nlohmann::json find(std::string_view path, const nlohmann::json& position, bool definition,
                        const std::function<std::vector<std::string>(std::string_view module)>& providers) const;

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
};

} // namespace mcppls::index
