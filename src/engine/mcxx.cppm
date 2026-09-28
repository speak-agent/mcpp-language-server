// The mcxx engine: C++ semantics from libmc++ in process (mcxx.clang over Clang 23.1, served by
// mcxx.lsp), in place of a clangd process. Module interfaces are built by libmc++ itself, in
// dependency order and cached by content; the engine database, prime units and module hints that
// clangd needs are not used.
export module mcppls.engine.mcxx;

import std;
import mcppls.engine;

export namespace mcppls::engine::mcxx {

inline constexpr std::string_view ENGINE_ID { "mcxx" };

struct Options {
    // Clang's builtin headers (lib/clang/<major>): <payload>/mcxx/lib/clang/<major>, else what
    // MCPPLS_MCXX_RESOURCE_DIR names, else a payload clangd's own.
    std::string resourceDirectory;
    unsigned workers { 0 };        // 0: libmc++ decides (a quarter of the hardware threads)
    bool backgroundIndex { true };
};

// Where the resource directory is for a payload rooted at `payloadDirectory` and a clangd at `clangd`.
std::string resource_directory(std::string_view payloadDirectory, std::string_view clangd);

std::unique_ptr<Engine> make_engine(Options options);

} // namespace mcppls::engine::mcxx
