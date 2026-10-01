module mcppls.engine.mcxx;

import std;
import nlohmann.json;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.version;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.lsp.protocol;
import mcppls.lsp.jsonrpc;
import mcppls.normalize.plan;
import mcppls.engine;
import mcxx.msa;
import mcxx.backend;
import mcxx.lsp;

namespace mcppls::engine::mcxx {

namespace {

namespace log = mcppls::base::log;
namespace fs = mcppls::platform::fs;
namespace msa = ::mcxx::msa;

// The requests this engine answers; everything else is left to others or to nobody.
bool answers(std::string_view method) {
    namespace m = lsp::method;
    // The outline and workspace symbols are the native engine's, from MC++'s own front end (M1.8).
    static const std::array<std::string_view, 14> METHODS {
        "textDocument/symbolInfo",   // clangd's extension, which mcppls's queries use
        "textDocument/prepareCallHierarchy", "callHierarchy/outgoingCalls",
        m::TEXT_DOCUMENT_DIAGNOSTIC,
        m::TEXT_DOCUMENT_DEFINITION,        m::TEXT_DOCUMENT_DECLARATION,     m::TEXT_DOCUMENT_TYPE_DEFINITION,
        m::TEXT_DOCUMENT_IMPLEMENTATION,    m::TEXT_DOCUMENT_HOVER,           m::TEXT_DOCUMENT_REFERENCES,
        m::TEXT_DOCUMENT_DOCUMENT_HIGHLIGHT,
        m::TEXT_DOCUMENT_COMPLETION,        m::TEXT_DOCUMENT_SIGNATURE_HELP,  m::TEXT_DOCUMENT_SEMANTIC_TOKENS_FULL,
    };
    return std::ranges::find(METHODS, method) != METHODS.end();
}

class McxxEngine final : public Engine {
public:
    explicit McxxEngine(Options options) : options_ { std::move(options) } {}
    ~McxxEngine() override { shut_down(); }

    std::string_view id() const override { return ENGINE_ID; }
    std::span<const MethodCapability> methods() const override { return methods_; }

    EngineTraits traits() const override {
        // None of clangd's compensations: libmc++ builds interfaces itself, reads the editor's
        // buffer, and does not stall on an import nothing provides.
        return EngineTraits { .importNavigation = false, .pushesDiagnostics = true, .tested = true };
    }

    EngineStatus status() const override {
        EngineStatus s { .name = std::string { ENGINE_ID }, .version = version_, .role = "core" };
        if (options_.payloadCorrupt) {
            s.state = "unavailable";
            s.failed = true;
            s.issues.push_back({ "payload-corrupt", "the extension's payload is corrupt or was modified; reinstall the extension", "mcppls.showLogs",
                                 "environment" });
            return s;
        }
        if (!workspace_) {
            s.state = "starting";
            return s;
        }
        const msa::Status w { workspace_->status() };
        s.accepting = true;
        s.prepared = w.modules_ready + w.modules_failed;
        s.toPrepare = w.modules;
        s.preparing = w.busy && s.prepared < s.toPrepare;
        s.state = s.preparing ? "preparing" : "ready";
        if (options_.resourceDirectory.empty())
            s.issues.push_back({ "engine-resource-missing", "no builtin header directory: the backend's own headers will not be found", "", "environment" });
        if (!w.rejected.empty()) {
            // The build's commands carry what the backend does not accept: the environment's problem (fix plan F6).
            s.issues.push_back({ "module-scan-failed",
                                 std::format("{} rejected the compile command: {} (first seen for {})", backend_.name, w.rejected.front().reason,
                                             base::file_name(w.rejected.front().file)),
                                 "mcppls.showLogs", "environment" });
        }
        if (std::ranges::any_of(w.failures, [](const msa::ModuleFailure& f) { return !f.command; })) {
            // A module that does not compile is a problem of the code (import-hang plan §6), told once here and
            // as a diagnostic on each import of it; nothing about the engine is degraded.
            const auto root = std::ranges::find_if(w.failures, [](const msa::ModuleFailure& f) { return f.cause == f.module && !f.command; });
            const msa::ModuleFailure& first { root != w.failures.end() ? *root : w.failures.front() };
            s.issues.push_back({ "modules-doomed",
                                 std::format("{} module{} cannot be prepared because {} failed: {}", w.failures.size(), w.failures.size() == 1 ? "" : "s",
                                             first.module, first.reason),
                                 "mcppls.showLogs", "code" });
        }
        return s;
    }

    void start(Host& host) override {
        host_ = &host;
        sink_ = host.event_sink(ENGINE_ID);
        if (options_.payloadCorrupt) {
            log::error("mcxx engine: the payload is corrupt; not starting");
            host.engine_settled(ENGINE_ID, Json::object());
            host.status_changed();
            return;
        }
        const std::string cache { base::join_path(host.cache_directory(), "mcxx") };
        stubDirectory_ = base::join_path(cache, "stubs");
        databaseDirectory_ = base::join_path(host.cache_directory(), "contexts/default/cdb");
        (void)fs::create_directories(cache);
        msa::Workspace::Options w;
        w.cache_directory = cache;
        w.resource_directory = options_.resourceDirectory;
        w.workers = options_.workers;
        w.background_index = options_.backgroundIndex;
        // libmc++ says how much each line matters (a failure is info or above; MCXX_LOG widens the rest).
        w.log = [](msa::LogLevel level, std::string_view category, std::string_view message) {
            switch (level) {
            case msa::LogLevel::debug: log::debug("mcxx {}: {}", category, message); break;
            case msa::LogLevel::info: log::info("mcxx {}: {}", category, message); break;
            case msa::LogLevel::warning: log::warning("mcxx {}: {}", category, message); break;
            case msa::LogLevel::error: log::error("mcxx {}: {}", category, message); break;
            }
        };
        auto sink = sink_;
        w.changed = [sink] { sink(Json { { "kind", "changed" } }); };
        workspace_ = ::mcxx::backend::make_workspace(std::move(w));
        service_ = std::make_unique<::mcxx::lsp::Service>(*workspace_, ::mcxx::lsp::Options {}, [sink](std::string_view method, Json params) {
            if (method == lsp::method::TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS) sink(Json { { "kind", "diagnostics" }, { "params", std::move(params) } });
        });
        for (unsigned i { 0 }; i < 4; ++i) workers_.emplace_back([this](std::stop_token stop) { work_(stop); });
        log::info("mcxx engine: {} {}, resource directory {}", backend_.name, backend_.version,
                  options_.resourceDirectory.empty() ? "(none)" : options_.resourceDirectory);
        host.engine_settled(ENGINE_ID, ::mcxx::lsp::Service::capabilities());
    }

    void shut_down() override {
        {
            std::lock_guard lock { mutex_ };
            if (stopping_) return;
            stopping_ = true;
            jobs_.clear();
        }
        cv_.notify_all();
        for (auto& w : workers_) w.request_stop();
        cv_.notify_all();
        workers_.clear();
        service_.reset();
        workspace_.reset();
    }

    void configure_plan(normalize::PlanInput& input) const override {
        // Only a name for argv[0] of synthesized commands: the builtin headers are passed by path.
        input.engineDriverDirectory = options_.resourceDirectory.empty() ? std::string {} : base::join_path(base::parent_path(options_.resourceDirectory), "bin");
        input.primeDirectory.clear();
        input.moduleHintDirectory.clear();
        input.stubDirectory = stubDirectory_;
        input.excludeUnresolvedImports = false;
        input.noAlignedAllocationWithMsvcStl = false;
    }

    void apply(const normalize::EnginePlan* plan) override {
        if (plan == nullptr || !workspace_) return;
        std::vector<msa::Command> commands;
        commands.reserve(plan->entries.size());
        for (const auto& e : plan->entries) commands.push_back(msa::Command { e.directory, e.file, e.arguments });
        auto stubs = plan->stubSources;
        ++generation_;
        // The database as a compile_commands.json where the clangd engine keeps its own: what a
        // diagnostic bundle and a person debugging read. libmc++ is given the commands directly.
        // Written here, not on the engine's queue: behind a parse that waits for a module build, the
        // queue reached it seconds later, and a bundle asked for meanwhile had no database (win32-x64,
        // diagnostic-bundle's B3: std took 5 s to build).
        (void)fs::create_directories(databaseDirectory_);
        (void)fs::write_file_atomic(base::join_path(databaseDirectory_, "compile_commands.json"), normalize::to_compile_commands(*plan, false).dump(2));
        post_([this, commands = std::move(commands), stubs = std::move(stubs)]() mutable {
            if (!stubs.empty()) (void)fs::create_directories(stubDirectory_);
            for (const auto& [file, content] : stubs)
                if (fs::read_file(file).value_or("") != content) (void)fs::write_file(file, content);
            workspace_->set_commands(std::move(commands));
            service_->refresh();
            sink_(Json { { "kind", "applied" } });
        });
        // From here until libmc++ has the commands, the index counts as being built: a query that
        // waits for the index must not find it idle in between.
        ++pendingPlans_;
        update_index_progress_();
    }

    void document(const DocumentEvent& event) override {
        if (!service_ || event.document.path.empty()) return;
        const std::string uri { host_->engine_uri(event.document.uri) };
        log::debug("mcxx: document {} {} v{}", static_cast<int>(event.change), uri, event.document.version);
        switch (event.change) {
        case DocumentChange::opened: service_->open(uri, std::string { event.document.text }, event.document.version); break;
        case DocumentChange::changed:
            service_->change(uri, Json::array({ Json { { "text", std::string { event.document.text } } } }), event.document.version);
            break;
        case DocumentChange::closed: service_->close(uri); break;
        case DocumentChange::saved: service_->saved(uri); break;
        }
    }

    void notify(const Json& message) override {
        if (!service_ || message.value("method", std::string {}) != "workspace/didChangeWatchedFiles") return;
        const Json params = message.contains("params") ? message["params"] : Json::object();
        for (const auto& change : params.value("changes", Json::array()))
            if (change.contains("uri")) service_->changed_on_disk(host_->engine_uri(change["uri"].get<std::string>()));
    }

    // Sources or build descriptions changed on disk: what is open is read again (a changed interface
    // is rebuilt when an importer's parse requires it).
    void sources_changed() override {
        if (service_) post_([this] { service_->refresh(); });
    }

    bool claims(const RequestView& request) const override { return service_ && answers(request.method); }

    void request(const RequestView& request, const Json& message, Reply reply) override {
        if (!service_) {
            reply(Answer { Answer::Kind::unavailable, nullptr });
            return;
        }
        const std::string key { message.contains("id") ? message["id"].dump() : std::to_string(++anonymous_) };
        Json params = request.params != nullptr ? *request.params : (message.contains("params") ? message["params"] : Json::object());
        if (params.contains("textDocument") && params["textDocument"].contains("uri"))
            params["textDocument"]["uri"] = host_->engine_uri(params["textDocument"]["uri"].get<std::string>());
        if (params.contains("item") && params["item"].is_object() && params["item"].contains("uri"))
            params["item"]["uri"] = host_->engine_uri(params["item"]["uri"].get<std::string>());
        auto stop = std::make_shared<std::stop_source>();
        log::debug("mcxx: request {} {} {}", request.method, key, params.contains("textDocument") ? params["textDocument"].dump() : std::string {});
        pending_[key] = Pending { std::move(reply), stop };
        post_([this, key, method = std::string { request.method }, params = std::move(params), stop] {
            const auto result = service_->request(method, params, stop->get_token());
            Json event { { "kind", "reply" }, { "key", key } };
            if (result) event["result"] = *result;
            else event["error"] = Json { { "code", result.error().code }, { "message", result.error().message } };
            sink_(std::move(event));
        });
    }

    void cancel(const Json& clientRequestId) override {
        if (const auto it = pending_.find(clientRequestId.dump()); it != pending_.end()) it->second.stop->request_stop();
    }

    void client_response(int, const Json&, const Json&) override {}

    void handle_event(const Json& event) override {
        const std::string kind { event.value("kind", std::string {}) };
        if (kind == "reply") {
            log::debug("mcxx: reply {} {}", event.value("key", std::string {}), event.dump().substr(0, 300));
            const auto it = pending_.find(event.value("key", std::string {}));
            if (it == pending_.end()) return;
            Reply reply { std::move(it->second.reply) };
            const bool cancelled { it->second.stop->stop_requested() };
            pending_.erase(it);
            if (cancelled) {
                reply(Answer { Answer::Kind::cancelled, nullptr });
            } else if (event.contains("error")) {
                reply(Answer { Answer::Kind::error, event["error"] });
            } else {
                Json value = event.contains("result") ? event["result"] : Json(nullptr);
                host_->client_view(value);
                reply(Answer { Answer::Kind::result, std::move(value) });
            }
        } else if (kind == "diagnostics") {
            Json params = event["params"];
            const std::string clientUri { host_->client_uri(params.value("uri", std::string {})) };
            Json diagnostics = params.value("diagnostics", Json::array());
            host_->client_view(diagnostics);
            std::optional<std::int64_t> version;
            if (params.contains("version") && params["version"].is_number_integer()) version = params["version"].get<std::int64_t>();
            host_->publish_engine_diagnostics(ENGINE_ID, clientUri, std::move(diagnostics), version);
        } else if (kind == "applied") {
            if (pendingPlans_ > 0) --pendingPlans_;
            update_index_progress_();
        } else if (kind == "changed") {
            host_->status_changed();
            update_index_progress_();
        }
    }

    std::optional<Clock::time_point> next_deadline() const override { return std::nullopt; }
    void handle_timers() override {}

    Json report() const override {
        // In process: nothing restarts and no file is ever set aside (the fields a clangd engine reports).
        Json r { { "version", version_ }, { "resourceDirectory", options_.resourceDirectory }, { "restarts", Json::array() },
                 { "filesSetAside", Json::array() }, { "generation", generation_ }, { "databaseDirectory", databaseDirectory_ } };
        if (workspace_) {
            const msa::Status s { workspace_->status() };
            Json failures = Json::array();
            for (const auto& f : s.failures) failures.push_back(Json { { "module", f.module }, { "cause", f.cause }, { "reason", f.reason } });
            Json files = Json::array();
            for (const auto& rejected : s.rejected) files.push_back(rejected.file);
            r["scanFailures"] = Json { { "count", s.commands_rejected }, { "files", std::move(files) },
                                       { "firstReason", s.rejected.empty() ? std::string {} : s.rejected.front().reason } };
            Json counters = Json::object();
            for (const auto& [name, value] : s.counters) counters[name] = value;
            r["counters"] = std::move(counters);
            r["status"] = Json { { "units", s.units }, { "modules", s.modules }, { "modulesReady", s.modules_ready },
                                 { "modulesFailed", s.modules_failed }, { "indexed", s.indexed }, { "busy", s.busy }, { "failures", std::move(failures) } };
        }
        return r;
    }

private:
    struct Pending {
        Reply reply;
        std::shared_ptr<std::stop_source> stop;
    };

    Options options_;
    msa::BackendInfo backend_ { ::mcxx::backend::info() };
    std::string version_ { std::format("{} ({} {})", base::VERSION, backend_.name, backend_.version) };
    std::vector<MethodCapability> methods_ { { std::string { EVERY_METHOD }, Role::answer, 0 } };
    Host* host_ { nullptr };
    std::function<void(Json)> sink_;
    std::string stubDirectory_;
    std::string databaseDirectory_;
    std::uint64_t generation_ { 0 };   // plans applied
    std::string progressToken_;        // non-empty while the index's $/progress is open
    int progressPercent_ { -1 };
    int pendingPlans_ { 0 };           // plans applied that libmc++ has not taken yet
    std::uint64_t progressTokens_ { 0 };

    // The program index being built, as standard `$/progress` every editor shows (and mcppls's own
    // queries wait on): begun when units wait to be indexed, reported by the percent, ended when none do.
    void update_index_progress_() {
        const Json* supported { lsp::find_path(host_->client_initialize_params(), { "capabilities", "window", "workDoneProgress" }) };
        if (supported == nullptr || !supported->is_boolean() || !supported->get<bool>() || !workspace_) return;
        const msa::Status w { workspace_->status() };
        const bool indexing { pendingPlans_ > 0 || (w.busy && w.units > 0 && w.indexed < w.units) };
        if (!indexing) {
            if (progressToken_.empty()) return;
            host_->send_to_client(lsp::make_notification("$/progress", Json { { "token", progressToken_ }, { "value", Json { { "kind", "end" } } } }));
            progressToken_.clear();
            progressPercent_ = -1;
            return;
        }
        const int percent { w.units > 0 ? static_cast<int>(std::min(w.indexed, w.units) * 100 / w.units) : 0 };
        const std::string message { std::format("{}/{} units", w.indexed, w.units) };
        if (progressToken_.empty()) {
            progressToken_ = std::format("mcxx/index/{}", ++progressTokens_);
            host_->send_to_client(lsp::make_request(host_->client_request_id(ENGINE_ID, 0, Json(progressToken_)), "window/workDoneProgress/create",
                                                    Json { { "token", progressToken_ } }));
            host_->send_to_client(lsp::make_notification("$/progress", Json { { "token", progressToken_ },
                { "value", Json { { "kind", "begin" }, { "title", "indexing" }, { "message", message }, { "percentage", percent }, { "cancellable", false } } } }));
            progressPercent_ = percent;
            return;
        }
        if (percent == progressPercent_) return;
        progressPercent_ = percent;
        host_->send_to_client(lsp::make_notification("$/progress", Json { { "token", progressToken_ },
            { "value", Json { { "kind", "report" }, { "message", message }, { "percentage", percent } } } }));
    }
    std::unique_ptr<msa::Workspace> workspace_;
    std::unique_ptr<::mcxx::lsp::Service> service_;
    std::map<std::string, Pending> pending_;   // event loop only
    std::uint64_t anonymous_ { 0 };
    // Work off the event loop: requests and plan changes.
    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<std::function<void()>> jobs_;
    bool stopping_ { false };
    std::vector<std::jthread> workers_;

    void post_(std::function<void()> job) {
        {
            std::lock_guard lock { mutex_ };
            if (stopping_) return;
            jobs_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

    void work_(std::stop_token stop) {
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock lock { mutex_ };
                cv_.wait(lock, stop, [&] { return stopping_ || !jobs_.empty(); });
                if (stopping_ || stop.stop_requested()) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
    }
};

} // namespace

std::string kit_stdlib_version() { return ::mcxx::backend::info().kit_stdlib_version; }

std::string resource_directory(std::string_view payloadDirectory, std::string_view clangd) {
    // The payload's own: the backend's builtin headers under mcxx/resource/include.
    if (!payloadDirectory.empty()) {
        const std::string own { base::join_path(std::string { payloadDirectory }, "mcxx/resource") };
        if (fs::is_directory(base::join_path(own, "include"))) return own;
    }
    if (const auto named = platform::env::get("MCPPLS_MCXX_RESOURCE_DIR"); named && !named->empty()) return *named;
    // A payload of the clangd engine (transitional): the builtin headers that ship beside its clangd.
    if (!clangd.empty()) {
        const std::string lib { base::join_path(base::parent_path(base::parent_path(clangd)), "lib/clang") };
        for (const auto& entry : fs::list_directory(lib))
            if (fs::is_directory(base::join_path(entry, "include"))) return entry;
    }
    return {};
}

std::unique_ptr<Engine> make_engine(Options options) { return std::make_unique<McxxEngine>(std::move(options)); }

} // namespace mcppls::engine::mcxx
