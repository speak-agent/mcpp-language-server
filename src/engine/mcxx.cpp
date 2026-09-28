module mcppls.engine.mcxx;

import std;
import nlohmann.json;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.version;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.lsp.protocol;
import mcppls.normalize.plan;
import mcppls.engine;
import mcxx.msa;
import mcxx.clang;
import mcxx.lsp;

namespace mcppls::engine::mcxx {

namespace {

namespace log = mcppls::base::log;
namespace fs = mcppls::platform::fs;
namespace msa = ::mcxx::msa;

// The requests this engine answers; everything else is left to others or to nobody.
bool answers(std::string_view method) {
    namespace m = lsp::method;
    static const std::array<std::string_view, 12> METHODS {
        m::TEXT_DOCUMENT_DEFINITION,        m::TEXT_DOCUMENT_DECLARATION,     m::TEXT_DOCUMENT_TYPE_DEFINITION,
        m::TEXT_DOCUMENT_IMPLEMENTATION,    m::TEXT_DOCUMENT_HOVER,           m::TEXT_DOCUMENT_REFERENCES,
        m::TEXT_DOCUMENT_DOCUMENT_HIGHLIGHT, m::TEXT_DOCUMENT_DOCUMENT_SYMBOL, m::WORKSPACE_SYMBOL,
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
            s.issues.push_back({ "engine-resource-missing", "no Clang resource directory: builtin headers will not be found", "", "environment" });
        return s;
    }

    void start(Host& host) override {
        host_ = &host;
        sink_ = host.event_sink(ENGINE_ID);
        const std::string cache { base::join_path(host.cache_directory(), "mcxx") };
        stubDirectory_ = base::join_path(cache, "stubs");
        (void)fs::create_directories(cache);
        msa::Workspace::Options w;
        w.cache_directory = cache;
        w.resource_directory = options_.resourceDirectory;
        w.workers = options_.workers;
        w.background_index = options_.backgroundIndex;
        w.log = [](std::string_view line) { log::info("mcxx: {}", line); };
        auto sink = sink_;
        w.changed = [sink] { sink(Json { { "kind", "changed" } }); };
        workspace_ = ::mcxx::clang::make_workspace(std::move(w));
        service_ = std::make_unique<::mcxx::lsp::Service>(*workspace_, ::mcxx::lsp::Options {}, [sink](std::string_view method, Json params) {
            if (method == lsp::method::TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS) sink(Json { { "kind", "diagnostics" }, { "params", std::move(params) } });
        });
        for (unsigned i { 0 }; i < 4; ++i) workers_.emplace_back([this](std::stop_token stop) { work_(stop); });
        log::info("mcxx engine: libmc++ over clang {}, resource directory {}", ::mcxx::clang::version(),
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
        input.engineDriverDirectory = options_.resourceDirectory.empty() ? std::string {} : base::parent_path(base::parent_path(base::parent_path(options_.resourceDirectory)));
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
        post_([this, commands = std::move(commands), stubs = std::move(stubs)]() mutable {
            if (!stubs.empty()) (void)fs::create_directories(stubDirectory_);
            for (const auto& [file, content] : stubs)
                if (fs::read_file(file).value_or("") != content) (void)fs::write_file(file, content);
            workspace_->set_commands(std::move(commands));
            service_->refresh();
        });
    }

    void document(const DocumentEvent& event) override {
        if (!service_ || event.document.path.empty()) return;
        const std::string uri { host_->engine_uri(event.document.uri) };
        log::info("mcxx: document {} {} v{}", static_cast<int>(event.change), uri, event.document.version);
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

    void sources_changed() override {}

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
        auto stop = std::make_shared<std::stop_source>();
        log::info("mcxx: request {} {} {}", request.method, key, params.contains("textDocument") ? params["textDocument"].dump() : std::string {});
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
            log::info("mcxx: reply {} {}", event.value("key", std::string {}), event.dump().substr(0, 300));
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
        } else if (kind == "changed") {
            host_->status_changed();
        }
    }

    std::optional<Clock::time_point> next_deadline() const override { return std::nullopt; }
    void handle_timers() override {}

    Json report() const override {
        Json r { { "version", version_ }, { "resourceDirectory", options_.resourceDirectory } };
        if (workspace_) {
            const msa::Status s { workspace_->status() };
            Json failures = Json::array();
            for (const auto& [module, why] : s.failures) failures.push_back(Json { { "module", module }, { "reason", why } });
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
    std::string version_ { std::format("{} (libmc++ over clang {})", base::VERSION, ::mcxx::clang::version()) };
    std::vector<MethodCapability> methods_ { { std::string { EVERY_METHOD }, Role::answer, 0 } };
    Host* host_ { nullptr };
    std::function<void(Json)> sink_;
    std::string stubDirectory_;
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

std::string resource_directory(std::string_view payloadDirectory, std::string_view clangd) {
    const std::string major { "23" };
    if (!payloadDirectory.empty()) {
        const std::string own { base::join_path(base::join_path(std::string { payloadDirectory }, "mcxx/lib/clang"), major) };
        if (fs::is_directory(base::join_path(own, "include"))) return own;
    }
    if (const auto named = platform::env::get("MCPPLS_MCXX_RESOURCE_DIR"); named && !named->empty()) return *named;
    if (!clangd.empty()) {
        const std::string beside { base::join_path(base::join_path(base::parent_path(base::parent_path(clangd)), "lib/clang"), major) };
        if (fs::is_directory(base::join_path(beside, "include"))) return beside;
    }
    return {};
}

std::unique_ptr<Engine> make_engine(Options options) { return std::make_unique<McxxEngine>(std::move(options)); }

} // namespace mcppls::engine::mcxx
