// M2.3 (E-LS-4): what a name names, answered by the native index before the engine -- MC++'s own front
// end over the file and the sources of the interfaces it imports, no BMI and no build -- and left to
// the engine when the front end is not sure (src/engine/native/declarations.cppm).
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.text;
import mcppls.engine.native.index;

namespace idx = mcppls::index;
using mcppls::base::Position;
using Json = nlohmann::json;

namespace {

// The files a Location[] answer names.
std::vector<std::string> files_of(const Json& locations) {
    std::vector<std::string> out;
    if (!locations.is_array()) return out;
    for (const auto& l : locations) {
        const std::string uri { l.value("uri", std::string {}) };
        out.push_back(uri.substr(uri.rfind('/') + 1));
    }
    return out;
}

int line_of(const Json& locations) { return locations.at(0).at("range").at("start").at("line").get<int>(); }

} // namespace

int main() {
    using namespace mcppls::testing;

    idx::ModuleIndex index;
    index.update("/p/cli.cppm", "export module cli;\n"
                                "export namespace cli {\n"
                                "int run(int argc);\n"
                                "struct Options { int verbose; };\n"
                                "inline int twice(int n) { return n * 2; }\n"
                                "}\n");
    index.update("/p/cli.cpp", "module cli;\n"
                               "namespace cli {\n"
                               "int run(int argc) { return argc; }\n"
                               "}\n");
    index.update("/p/main.cpp", "import cli;\n"
                                "int main(int argc, char**) {\n"
                                "    cli::Options o { 1 };\n"
                                "    return cli::run(argc) + cli::twice(o.verbose) + std::size(argc);\n"
                                "}\n");

    "a name an import declares is found in the interface's source, before any build"_test = [&] {
        const Json declared = index.declaration("/p/main.cpp", Position { 3, 16 });
        expect(files_of(declared) == std::vector<std::string> { "cli.cppm" } && line_of(declared) == 2) << declared.dump();
        const Json options = index.definition("/p/main.cpp", Position { 2, 9 });
        expect(files_of(options) == std::vector<std::string> { "cli.cppm" } && line_of(options) == 3) << "a class's definition: " << options.dump();
        const Json twice = index.definition("/p/main.cpp", Position { 3, 33 });
        expect(files_of(twice) == std::vector<std::string> { "cli.cppm" } && line_of(twice) == 4) << "an inline function's body: " << twice.dump();
        const Json verbose = index.definition("/p/main.cpp", Position { 3, 41 });
        expect(files_of(verbose) == std::vector<std::string> { "cli.cppm" } && line_of(verbose) == 3) << "a member, through the object's type: " << verbose.dump();
        const Json argc = index.definition("/p/main.cpp", Position { 3, 20 });
        expect(files_of(argc) == std::vector<std::string> { "main.cpp" } && line_of(argc) == 1) << "a parameter: " << argc.dump();
    };

    "what the front end cannot place is the engine's"_test = [&] {
        // run() is defined in an implementation unit no importer reads: its definition is not known here.
        expect(index.definition("/p/main.cpp", Position { 3, 16 }).is_null()) << index.definition("/p/main.cpp", Position { 3, 16 }).dump();
        // A name std declares (no interface of std is read here).
        expect(index.definition("/p/main.cpp", Position { 3, 57 }).is_null());
    };

    "an edit is what is answered next"_test = [&] {
        index.update("/p/cli.cppm", "export module cli;\n"
                                    "export namespace cli {\n"
                                    "\n"
                                    "int run(int argc);\n"
                                    "struct Options { int verbose; };\n"
                                    "inline int twice(int n) { return n * 2; }\n"
                                    "}\n");
        const Json declared = index.declaration("/p/main.cpp", Position { 3, 16 });
        expect(files_of(declared) == std::vector<std::string> { "cli.cppm" } && line_of(declared) == 3) << declared.dump();
    };

    return report();
}
