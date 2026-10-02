// The mcxx engine's builtin headers, as a payload ships them (<payload>/mcxx/resource/include):
// clang/lib/Headers of the LLVM source release the lock names (`llvm-project-src`, the release
// libmc++'s backend is built from), plus LLVM's license text.
//
// These are what a clangd release carries as lib/clang/<major>/include, less the few a Clang build
// generates (arm_neon.h, arm_sve.h, riscv_vector.h and the like, target intrinsics mcppls has no
// use for); the semantic kit's standard library needs none of those.
export module mcppls.pack.resource;

import std;
import mcppls.base.error;
import mcppls.pack.lock;

export namespace mcppls::pack::resource {

struct Options {
    std::string outDirectory;                 // replaced if it exists; gets include/ and LICENSE.TXT
    std::optional<std::string> sourceArchive; // an already-downloaded llvm-project source archive
    std::string cacheDirectory;               // where a fetched archive is cached
    // The headers Clang's build generates (arm_neon.h, the other ARM and RISC-V intrinsics, 7 MB):
    // for a payload whose host is ARM, whose projects are; an x64 payload stays within its size.
    bool generatedHeaders { true };
};

struct Result {
    std::string directory;
    std::size_t files { 0 };
    std::uint64_t totalBytes { 0 };
};

base::Result<Result> stage(const Options& options, const lock::Lock& lockData);

} // namespace mcppls::pack::resource
