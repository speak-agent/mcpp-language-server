// The mcxx engine: C++ semantics from libmc++ in process (mcxx.backend, served by mcxx.lsp), in
// place of a clangd process. The engine names no front end: libmc++ decides which backend answers.
// Module interfaces are built by libmc++ itself, in dependency order and cached by content; the
// engine database, prime units and module hints that clangd needs are not used.
export module mcppls.engine.mcxx;

import std;
import mcppls.engine;

export namespace mcppls::engine::mcxx {

inline constexpr std::string_view ENGINE_ID { "mcxx" };

struct Options {
    // The backend's builtin headers (a directory with include/): <payload>/mcxx/resource, else what
    // MCPPLS_MCXX_RESOURCE_DIR names, else what a clangd payload ships beside its clangd.
    std::string resourceDirectory;
    unsigned workers { 0 };        // 0: libmc++ decides (a quarter of the hardware threads)
    bool backgroundIndex { true };
};

// Where the resource directory is for a payload rooted at `payloadDirectory` and a clangd at `clangd`.
std::string resource_directory(std::string_view payloadDirectory, std::string_view clangd);

std::unique_ptr<Engine> make_engine(Options options);

} // namespace mcppls::engine::mcxx
