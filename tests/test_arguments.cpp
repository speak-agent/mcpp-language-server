// A0.5.2 (MC5-6-2, lesson of mcpp-language-server#30): a unit gets byte-for-byte the same engine
// arguments whichever way its model arrives -- from the producer, or read back from the model cache
// on a warm start -- so a module interface built on the cold start is the one the warm start finds.
import std;
import mcppls.testing;
import nlohmann.json;
import mcppls.spec.database;
import mcppls.spec.metadata;
import mcppls.toolchain.probe;
import mcppls.project.scan;
import mcppls.project.detect;
import mcppls.project.model;
import mcppls.project.modelcache;
import mcppls.normalize.plan;

namespace s = mcppls::spec;
namespace n = mcppls::normalize;
namespace p = mcppls::project;
using mcppls::toolchain::ToolchainFacts;

namespace {

// Upward from the working directory, as the other tests find the repository: a cross-built test's
// source path is the build machine's.
std::filesystem::path repository() {
    std::filesystem::path directory { std::filesystem::current_path() };
    while (!std::filesystem::is_regular_file(directory / "docs/specs/README.md") && directory.parent_path() != directory) directory = directory.parent_path();
    return directory;
}

ToolchainFacts gcc_facts() {
    ToolchainFacts facts;
    facts.toolchain.family = s::Family::gcc;
    facts.toolchain.version = "16.1.0";
    facts.toolchain.driver = "/opt/xpkgs/gcc/16.1.0/bin/g++";
    facts.toolchain.target = "x86_64-linux-gnu";
    facts.toolchain.stdlib = s::Stdlib { "libstdc++", "16.1.0", "/opt/xpkgs/gcc/16.1.0/lib64/libstdc++.modules.json" };
    facts.gccInstallDirectory = "/opt/xpkgs/gcc/16.1.0/lib/gcc/x86_64-linux-gnu/16.1.0";
    return facts;
}

// The producer's model: the specification's level-3 example as `mcpp emit build-database` writes
// it, with arguments in the producer's order (optimization, sysroot and definitions among them).
p::ProjectModel producer_model() {
    std::ifstream in { repository() / "docs/specs/examples/s1-level3-gcc.json" };
    nlohmann::json document = nlohmann::json::parse(in);
    for (auto& unit : document["sets"][0]["translation-units"]) {
        auto& args = unit["arguments"];
        nlohmann::json reordered = nlohmann::json::array({ args[0], "-I/home/u/hello/include", "-std=c++23", "-O2", "--sysroot=/opt/xpkgs/sysroot",
                                                           "-D__mcpp_target_linux__=1", "-DNDEBUG" });
        for (std::size_t i { 1 }; i < args.size(); ++i)
            if (args[i] != "-std=c++23") reordered.push_back(args[i]);
        args = reordered;
    }
    // What changes meaning, as the producer states it: structured options (S1 section 9).
    auto& options = document["sets"][0]["ide"]["options"];
    options["macros"] = nlohmann::json::array({ nlohmann::json { { "define", "__mcpp_target_linux__" }, { "value", "1" } },
                                                nlohmann::json { { "define", "NDEBUG" }, { "value", nullptr } } });
    options["include-directories"]["user"] = nlohmann::json::array({ "/home/u/hello/include" });
    auto database = s::from_json(document, "/home/u/hello");
    if (!database) throw std::runtime_error(database.error().message);
    p::ProjectModel model;
    model.root = "/home/u/hello";
    model.source = model.detected = p::SourceKind::mcpp;
    model.level = 3;
    model.tier = 1;
    model.database = std::move(*database);
    model.facts.emplace(document["sets"][0]["ide"]["toolchain"].get<std::string>(), gcc_facts());
    model.producer = "mcpp";
    model.producerVersion = "2026.9.28.2";
    return model;
}

n::EnginePlan plan(const p::ProjectModel& model) {
    n::PlanInput input;
    input.database = &model.database;
    input.facts = &model.facts;
    input.engineDriverDirectory = "/payload/mcxx/bin";
    input.stubDirectory = "/cache/stubs";
    input.excludeUnresolvedImports = false;
    input.scanner = [](std::string_view) { return p::ScanResult {}; };
    input.metadataReader = [](std::string_view) {
        return std::vector<s::ModuleEntry> { { "std", "/opt/xpkgs/gcc/16.1.0/include/c++/16.1.0/bits/std.cc", true, {}, {} } };
    };
    return n::plan_engine(input);
}

std::string entry_text(const n::EngineEntry& e) {
    std::string out { e.directory + "\n" + e.file + "\n" };
    for (const auto& a : e.arguments) out += a + '\0';
    return out;
}

} // namespace

int main() {
    using namespace mcppls::testing;
    const p::ProjectModel producer { producer_model() };

    "the model read back from the cache is the model the producer gave, database and toolchain facts alike"_test = [&] {
        const auto cached = p::model_from_json(p::model_to_json(producer));
        expect(fatal(cached.has_value())) << (cached ? "" : cached.error().message);
        expect(p::model_to_json(*cached) == p::model_to_json(producer));
        expect(cached->facts.size() == producer.facts.size());
        for (const auto& [id, facts] : producer.facts) {
            const auto it = cached->facts.find(id);
            expect(fatal(it != cached->facts.end())) << id;
            expect(mcppls::toolchain::facts_to_json(it->second) == mcppls::toolchain::facts_to_json(facts)) << id;
        }
    };

    "cold and warm starts give every unit byte-for-byte the same engine arguments"_test = [&] {
        const auto cached = p::model_from_json(p::model_to_json(producer));
        expect(fatal(cached.has_value()));
        const n::EnginePlan cold { plan(producer) };
        const n::EnginePlan warm { plan(*cached) };
        expect(fatal(!cold.entries.empty()));
        expect(fatal(cold.entries.size() == warm.entries.size())) << cold.entries.size() << " vs " << warm.entries.size();
        for (std::size_t i { 0 }; i < cold.entries.size(); ++i)
            expect(entry_text(cold.entries[i]) == entry_text(warm.entries[i])) << cold.entries[i].file << ": "
                                                                               << std::format("{}", cold.entries[i].arguments) << " vs "
                                                                               << std::format("{}", warm.entries[i].arguments);
        // What changes the program's meaning stays said, in the producer's words.
        const auto& first = cold.entries.front().arguments;
        expect(std::ranges::find(first, "-D__mcpp_target_linux__=1") != first.end() && std::ranges::find(first, "-I/home/u/hello/include") != first.end())
            << std::format("{}", first);
    };

    "a model cached twice is the same text: the cache does not drift from one start to the next"_test = [&] {
        const auto once = p::model_from_json(p::model_to_json(producer));
        expect(fatal(once.has_value()));
        const auto twice = p::model_from_json(p::model_to_json(*once));
        expect(fatal(twice.has_value()));
        expect(p::model_to_json(*twice).dump() == p::model_to_json(*once).dump());
    };

    return report();
}
