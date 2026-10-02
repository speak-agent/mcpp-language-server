module mcppls.pack.resource;

import std;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.pack.archive;
import mcppls.pack.fetch;
import mcppls.pack.lock;

namespace mcppls::pack::resource {
namespace fs = mcppls::platform::fs;
namespace ar = mcppls::pack::archive;

namespace {

constexpr std::string_view SOURCE_ENTRY { "llvm-project-src" };
constexpr std::string_view HEADERS { "clang/lib/Headers/" };
constexpr std::string_view LICENSE { "llvm/LICENSE.TXT" };
// The builtin headers Clang's build generates (arm_neon.h, the other ARM, AArch64 and RISC-V
// intrinsics), which clang/lib/Headers does not hold: from the llvm.clang-dev revision libmc++
// builds against, its llvm-generated/clang-lib/Headers, when the lock names that archive.
constexpr std::string_view GENERATED_ENTRY { "llvm-clang-dev" };
constexpr std::string_view GENERATED { "llvm-generated/clang-lib/Headers/" };

bool wanted(std::string_view path) {
    if (path == LICENSE) return true;
    return path.starts_with(HEADERS) && !path.ends_with("/CMakeLists.txt") && path != "clang/lib/Headers/CMakeLists.txt";
}

} // namespace

base::Result<Result> stage(const Options& options, const lock::Lock& lockData) {
    std::string archive { options.sourceArchive.value_or(std::string {}) };
    if (archive.empty()) {
        auto entry = lock::entry(lockData, SOURCE_ENTRY);
        if (!entry) return std::unexpected { entry.error() };
        auto fetched = fetch::get(*entry, options.cacheDirectory);
        if (!fetched) return std::unexpected { fetched.error() };
        archive = *fetched;
    }
    const std::string out { options.outDirectory };
    const std::string work { out + ".extract" };
    fs::remove_all(work);
    auto extracted = ar::extract(archive, work, wanted);
    if (!extracted) return std::unexpected { extracted.error() };
    if (!fs::is_regular_file(base::join_path(work, "clang/lib/Headers/stddef.h"))) {
        fs::remove_all(work);
        return base::fail("resource-missing", std::format("{} has no clang/lib/Headers/stddef.h", archive));
    }
    fs::remove_all(out);
    if (auto made = fs::create_directories(out); !made) return std::unexpected { made.error() };
    std::error_code failed;
    std::filesystem::rename(base::join_path(work, "clang/lib/Headers"), base::join_path(out, "include"), failed);
    if (failed) return base::fail("resource-copy", std::format("cannot place the headers in {}: {}", out, failed.message()));
    std::filesystem::rename(base::join_path(work, std::string { LICENSE }), base::join_path(out, "LICENSE.TXT"), failed);
    if (failed) return base::fail("resource-copy", std::format("cannot place LICENSE.TXT in {}: {}", out, failed.message()));
    fs::remove_all(work);

    if (auto generated = lock::entry(lockData, GENERATED_ENTRY); generated && options.generatedHeaders) {
        auto fetched = fetch::get(*generated, options.cacheDirectory);
        if (!fetched) return std::unexpected { fetched.error() };
        const std::string more { out + ".generated" };
        fs::remove_all(more);
        auto got = ar::extract(*fetched, more, [](std::string_view path) { return path.starts_with(GENERATED); });
        if (!got) return std::unexpected { got.error() };
        if (got->empty()) {
            fs::remove_all(more);
            return base::fail("resource-missing", std::format("{} has no {}", *fetched, GENERATED));
        }
        for (const auto& written : *got) {
            if (!fs::is_regular_file(base::join_path(more, written))) continue;
            const std::string name { base::file_name(written) };
            std::filesystem::rename(base::join_path(more, written), base::join_path(out, "include/" + name), failed);
            if (failed) return base::fail("resource-copy", std::format("cannot place {} in {}: {}", name, out, failed.message()));
        }
        fs::remove_all(more);
    }

    Result result { .directory = out };
    auto it = std::filesystem::recursive_directory_iterator(base::join_path(out, "include"), failed);
    for (; !failed && it != std::filesystem::recursive_directory_iterator(); it.increment(failed)) {
        if (!it->is_regular_file(failed)) continue;
        ++result.files;
        result.totalBytes += it->file_size(failed);
    }
    return result;
}

} // namespace mcppls::pack::resource
