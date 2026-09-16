#include "marketscan/strategy/plugin_loader.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#    include <windows.h>
#endif

namespace marketscan {
namespace {

// ---------------------------------------------------------------------------
// Descriptor snapshot
//
// A plugin's MS_StrategyDesc points into the plugin's own memory. It may point
// anywhere at all -- it is, after all, an arbitrary DLL that happened to
// export three symbols with the right names. Everything is therefore copied
// out through bounded reads inside a structured exception handler, and only
// turned into std::string afterwards, outside it.
//
// Sizes are caps, not expectations. A descriptor that exceeds one is truncated
// rather than rejected: a theory with a very long description is eccentric,
// not broken.
// ---------------------------------------------------------------------------

constexpr std::size_t kMaxText = 256;
constexpr std::size_t kMaxParams = 32;
constexpr std::size_t kMaxSymbols = 32;
constexpr std::size_t kMaxEnumLabels = 16;

struct ParamSnapshot {
    char name[kMaxText];
    char description[kMaxText];
    int type;
    double min_value;
    double max_value;
    double default_value;
    std::uint32_t enum_count;
    char enum_labels[kMaxEnumLabels][kMaxText];
};

struct DescSnapshot {
    std::uint32_t abi_version;
    char id[kMaxText];
    char display_name[kMaxText];
    char author[kMaxText];
    char description[kMaxText];
    std::uint32_t param_count;
    ParamSnapshot params[kMaxParams];
    std::uint32_t symbol_count;
    char symbols[kMaxSymbols][kMaxText];
};

/// Bounded copy of a C string that may not be a C string at all. Plain C so
/// that it is callable from inside __try.
void copy_text(char* destination, const char* source) noexcept {
    destination[0] = '\0';
    if (source == nullptr) {
        return;
    }
    std::size_t i = 0;
    for (; i + 1 < kMaxText && source[i] != '\0'; ++i) {
        destination[i] = source[i];
    }
    destination[i] = '\0';
}

/// Reads the whole descriptor. No C++ objects live in here, which is what lets
/// it use __try at all.
bool snapshot_descriptor_unguarded(const MS_StrategyDesc* desc, DescSnapshot* out) noexcept {
    out->abi_version = desc->abi_version;
    copy_text(out->id, desc->id);
    copy_text(out->display_name, desc->display_name);
    copy_text(out->author, desc->author);
    copy_text(out->description, desc->description);

    out->param_count = desc->params == nullptr ? 0U
                                               : (desc->param_count > kMaxParams
                                                      ? static_cast<std::uint32_t>(kMaxParams)
                                                      : desc->param_count);
    for (std::uint32_t i = 0; i < out->param_count; ++i) {
        const MS_ParamDesc& source = desc->params[i];
        ParamSnapshot& target = out->params[i];
        copy_text(target.name, source.name);
        copy_text(target.description, source.description);
        target.type = static_cast<int>(source.type);
        target.min_value = source.min_value;
        target.max_value = source.max_value;
        target.default_value = source.default_value;
        target.enum_count =
            source.enum_labels == nullptr
                ? 0U
                : (source.enum_count > kMaxEnumLabels ? static_cast<std::uint32_t>(kMaxEnumLabels)
                                                      : source.enum_count);
        for (std::uint32_t j = 0; j < target.enum_count; ++j) {
            copy_text(target.enum_labels[j], source.enum_labels[j]);
        }
    }

    out->symbol_count =
        desc->required_symbols == nullptr
            ? 0U
            : (desc->required_symbol_count > kMaxSymbols ? static_cast<std::uint32_t>(kMaxSymbols)
                                                         : desc->required_symbol_count);
    for (std::uint32_t i = 0; i < out->symbol_count; ++i) {
        copy_text(out->symbols[i], desc->required_symbols[i]);
    }
    return true;
}

// --- SEH wrappers ------------------------------------------------------
//
// An access violation inside a plugin is not a C++ exception, and catch(...)
// will not see it. These are the only places that call into a plugin before it
// has proven it can be trusted, so these are the places that need SEH.
//
// Each one is deliberately tiny and holds no object with a destructor: MSVC
// refuses __try in any function that requires unwinding.

#if defined(_MSC_VER)

bool guarded_abi_version(MS_PluginAbiVersionFn fn, std::uint32_t* out) noexcept {
    __try {
        *out = fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool guarded_describe(MS_PluginDescribeFn fn, const MS_StrategyDesc** out) noexcept {
    __try {
        *out = fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool guarded_snapshot(const MS_StrategyDesc* desc, DescSnapshot* out) noexcept {
    __try {
        return snapshot_descriptor_unguarded(desc, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool guarded_create(MS_PluginCreateFn fn, MS_StrategyVTable* vtable, void** out) noexcept {
    __try {
        *out = fn(vtable);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

#else

// No SEH outside MSVC. The ABI checks still run; a fault is simply fatal,
// which is the same behaviour every other non-Windows host has.

bool guarded_abi_version(MS_PluginAbiVersionFn fn, std::uint32_t* out) noexcept {
    *out = fn();
    return true;
}

bool guarded_describe(MS_PluginDescribeFn fn, const MS_StrategyDesc** out) noexcept {
    *out = fn();
    return true;
}

bool guarded_snapshot(const MS_StrategyDesc* desc, DescSnapshot* out) noexcept {
    return snapshot_descriptor_unguarded(desc, out);
}

bool guarded_create(MS_PluginCreateFn fn, MS_StrategyVTable* vtable, void** out) noexcept {
    *out = fn(vtable);
    return true;
}

#endif

// --- Platform shims ----------------------------------------------------

[[nodiscard]] std::string last_os_error() {
#if defined(_WIN32)
    const DWORD code = ::GetLastError();
    if (code == 0) {
        return "no error reported";
    }
    char* buffer = nullptr;
    const DWORD length = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&buffer),
        0,
        nullptr);
    std::string message =
        length > 0 && buffer != nullptr ? std::string(buffer, length) : std::string("unknown");
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' ||
                                message.back() == ' ' || message.back() == '.')) {
        message.pop_back();
    }
    return message + " (code " + std::to_string(code) + ")";
#else
    return "unsupported platform";
#endif
}

[[nodiscard]] void* open_library(const std::filesystem::path& path) {
#if defined(_WIN32)
    // Suppress the hard-error dialog first.
    //
    // Handed a file that is not a valid PE image -- a text file someone
    // renamed, a truncated build, a 32-bit DLL -- LoadLibrary does not simply
    // fail. It raises a critical error, and Windows puts up a modal "bad
    // image" message box. There is nobody to dismiss it on a build machine or
    // behind a render loop, so the process stops there permanently. This
    // turned a CI run into a 26-minute hang, and it would have done the same
    // to a user with one corrupt file in theories/.
    //
    // SetThreadErrorMode rather than SetErrorMode: the process-wide version
    // would race with any other thread doing file work.
    DWORD previous_mode = 0;
    const BOOL changed =
        ::SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_mode);

    // LOAD_WITH_ALTERED_SEARCH_PATH makes the DLL's own directory the first
    // place its dependencies are looked for. A theory with sibling DLLs of its
    // own is not supported here, because only the DLL itself is shadow copied;
    // such a theory should place its dependencies beside the executable.
    void* handle = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (changed != FALSE) {
        ::SetThreadErrorMode(previous_mode, nullptr);
    }
    return handle;
#else
    (void)path;
    return nullptr;
#endif
}

void close_library(void* handle) noexcept {
#if defined(_WIN32)
    if (handle != nullptr) {
        ::FreeLibrary(static_cast<HMODULE>(handle));
    }
#else
    (void)handle;
#endif
}

[[nodiscard]] void* find_symbol(void* handle, const char* name) noexcept {
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    (void)handle;
    (void)name;
    return nullptr;
#endif
}

[[nodiscard]] std::uint32_t current_process_id() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
    return 0;
#endif
}

[[nodiscard]] bool is_dll(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return extension == ".dll";
}

}  // namespace

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

struct PluginLoader::Module {
    std::string id;
    void* handle = nullptr;
    MS_PluginCreateFn create = nullptr;
};

// ---------------------------------------------------------------------------
// PluginInstance
// ---------------------------------------------------------------------------

PluginInstance::PluginInstance(PluginInstance&& other) noexcept
    : self_(other.self_),
      vtable_(other.vtable_),
      owner_(other.owner_),
      plugin_id_(std::move(other.plugin_id_)),
      started_(other.started_) {
    other.self_ = nullptr;
    other.owner_ = nullptr;
    other.started_ = false;
}

PluginInstance& PluginInstance::operator=(PluginInstance&& other) noexcept {
    if (this != &other) {
        reset();
        self_ = other.self_;
        vtable_ = other.vtable_;
        owner_ = other.owner_;
        plugin_id_ = std::move(other.plugin_id_);
        started_ = other.started_;
        other.self_ = nullptr;
        other.owner_ = nullptr;
        other.started_ = false;
    }
    return *this;
}

PluginInstance::~PluginInstance() {
    reset();
}

PluginInstance PluginInstance::adopt(void* self, const MS_StrategyVTable& vtable, std::string id) {
    PluginInstance instance;
    if (self == nullptr || vtable.destroy == nullptr) {
        return instance;
    }
    instance.self_ = self;
    instance.vtable_ = vtable;
    // No owner: nothing needs telling when this one goes away, because no
    // module's lifetime depends on it.
    instance.owner_ = nullptr;
    instance.plugin_id_ = std::move(id);
    return instance;
}

void PluginInstance::reset() noexcept {
    if (self_ == nullptr) {
        return;
    }
    // on_stop, then destroy, then let go of the module. In that order: a
    // strategy is entitled to be told the run is over before it is deleted.
    if (started_ && vtable_.on_stop != nullptr) {
        vtable_.on_stop(self_);
    }
    if (vtable_.destroy != nullptr) {
        vtable_.destroy(self_);
    }
    self_ = nullptr;
    started_ = false;
    if (owner_ != nullptr) {
        owner_->release_instance(plugin_id_);
        owner_ = nullptr;
    }
}

void PluginInstance::start(const MS_HostContext* host) {
    if (self_ != nullptr && vtable_.on_start != nullptr) {
        vtable_.on_start(self_, host);
    }
    started_ = self_ != nullptr;
}

void PluginInstance::bar(const MS_Bar& value) {
    if (self_ != nullptr && vtable_.on_bar != nullptr) {
        vtable_.on_bar(self_, &value);
    }
}

void PluginInstance::quote(const MS_Quote& value) {
    if (self_ != nullptr && vtable_.on_quote != nullptr) {
        vtable_.on_quote(self_, &value);
    }
}

void PluginInstance::trade(const MS_Trade& value) {
    if (self_ != nullptr && vtable_.on_trade != nullptr) {
        vtable_.on_trade(self_, &value);
    }
}

void PluginInstance::fill(const MS_Fill& value) {
    if (self_ != nullptr && vtable_.on_fill != nullptr) {
        vtable_.on_fill(self_, &value);
    }
}

void PluginInstance::stop() {
    if (self_ != nullptr && started_ && vtable_.on_stop != nullptr) {
        vtable_.on_stop(self_);
    }
    started_ = false;
}

void PluginInstance::set_param(const char* name, double value) {
    if (self_ != nullptr && vtable_.set_param != nullptr) {
        vtable_.set_param(self_, name, value);
    }
}

double PluginInstance::get_param(const char* name) const {
    if (self_ != nullptr && vtable_.get_param != nullptr) {
        return vtable_.get_param(self_, name);
    }
    return 0.0;
}

// ---------------------------------------------------------------------------
// PluginLoader
// ---------------------------------------------------------------------------

PluginLoader::PluginLoader(std::vector<std::filesystem::path> roots)
    : roots_(std::move(roots)), modules_(std::make_unique<std::vector<Module>>()) {
    // Per process *and* per loader. The destructor removes this directory, and
    // two loaders alive at once -- which the test suite does, and a future
    // sandboxed run would too -- must not delete each other's copies.
    static std::atomic<std::uint64_t> instance_counter{0};
    const std::uint64_t instance = instance_counter.fetch_add(1);

    std::error_code ec;
    shadow_root_ = std::filesystem::temp_directory_path(ec) / "marketscan" /
                   (std::to_string(current_process_id()) + "_" + std::to_string(instance));
    std::filesystem::create_directories(shadow_root_, ec);
}

PluginLoader::~PluginLoader() {
    unload_all();
    std::error_code ec;
    std::filesystem::remove_all(shadow_root_, ec);
}

void PluginLoader::unload_all() noexcept {
    for (Module& module : *modules_) {
        close_library(module.handle);
        module.handle = nullptr;
    }
    modules_->clear();
}

std::vector<std::filesystem::path>
PluginLoader::default_roots(const std::filesystem::path& executable_dir) {
    std::vector<std::filesystem::path> roots;
    roots.push_back(executable_dir / "theories");
    return roots;
}

std::vector<std::filesystem::path> PluginLoader::enumerate() const {
    std::vector<std::filesystem::path> found;
    for (const std::filesystem::path& root : roots_) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) {
            continue;  // an absent theories/ is the normal state, not an error
        }
        for (std::filesystem::recursive_directory_iterator it(
                 root, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator();
             it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_regular_file(ec) && is_dll(it->path())) {
                found.push_back(it->path());
            }
        }
    }
    // Sorted, so that the set of theories and the order they are registered in
    // is the same on every run whatever order the filesystem enumerated them.
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

const PluginInfo* PluginLoader::find(std::string_view id) const noexcept {
    for (const PluginInfo& info : plugins_) {
        if (info.id == id) {
            return &info;
        }
    }
    return nullptr;
}

PluginInfo* PluginLoader::find_mutable(std::string_view id) noexcept {
    for (PluginInfo& info : plugins_) {
        if (info.id == id) {
            return &info;
        }
    }
    return nullptr;
}

std::size_t PluginLoader::loaded_count() const noexcept {
    std::size_t count = 0;
    for (const PluginInfo& info : plugins_) {
        if (info.loaded()) {
            ++count;
        }
    }
    return count;
}

bool PluginLoader::load_file(const std::filesystem::path& path) {
    std::error_code ec;

    PluginInfo info;
    info.path = path;
    info.state = PluginState::Failed;
    info.last_write = std::filesystem::last_write_time(path, ec);
    info.file_size = std::filesystem::file_size(path, ec);

    const auto fail = [&](std::string message) {
        info.error = std::move(message);
        // Key a failed entry by path when its id could not be read, so the GUI
        // can still show the user which file misbehaved.
        if (info.id.empty()) {
            info.id = path.filename().string();
        }
        if (PluginInfo* existing = find_mutable(info.id); existing != nullptr) {
            if (existing->live_instances > 0) {
                return false;  // never disturb a plugin that is in use
            }
            *existing = std::move(info);
        } else {
            plugins_.push_back(std::move(info));
        }
        return false;
    };

    if (!std::filesystem::is_regular_file(path, ec)) {
        return fail("not a file: " + path.string());
    }

    // --- Shadow copy ----------------------------------------------------
    //
    // Windows locks a loaded DLL. Loading a copy is what lets the author
    // rebuild the original while the application is still running.
    const std::filesystem::path shadow =
        shadow_root_ / (std::to_string(shadow_counter_++) + "_" + path.filename().string());
    std::filesystem::copy_file(path, shadow, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return fail("could not shadow-copy to '" + shadow.string() + "': " + ec.message());
    }
    info.shadow_path = shadow;

    // --- Load -----------------------------------------------------------
    void* handle = open_library(shadow);
    if (handle == nullptr) {
        return fail("LoadLibrary failed: " + last_os_error());
    }

    const auto fail_and_close = [&](std::string message) {
        close_library(handle);
        return fail(std::move(message));
    };

    const auto abi_fn =
        reinterpret_cast<MS_PluginAbiVersionFn>(find_symbol(handle, "ms_plugin_abi_version"));
    const auto describe_fn =
        reinterpret_cast<MS_PluginDescribeFn>(find_symbol(handle, "ms_plugin_describe"));
    const auto create_fn =
        reinterpret_cast<MS_PluginCreateFn>(find_symbol(handle, "ms_plugin_create"));

    if (abi_fn == nullptr || describe_fn == nullptr || create_fn == nullptr) {
        std::string missing;
        if (abi_fn == nullptr) {
            missing += "ms_plugin_abi_version ";
        }
        if (describe_fn == nullptr) {
            missing += "ms_plugin_describe ";
        }
        if (create_fn == nullptr) {
            missing += "ms_plugin_create ";
        }
        return fail_and_close("not a MarketScan theory: missing export(s) " + missing +
                              "-- a theory must invoke MARKETSCAN_EXPORT_STRATEGY at file scope");
    }

    // --- ABI version, before anything else ------------------------------
    //
    // This is the first thing called and the first thing checked. A plugin
    // built against a different ABI has different struct layouts, and asking
    // it to describe itself would read the wrong bytes.
    std::uint32_t plugin_abi = 0;
    if (!guarded_abi_version(abi_fn, &plugin_abi)) {
        return fail_and_close("ms_plugin_abi_version faulted");
    }
    info.abi_version = plugin_abi;
    if (plugin_abi != MS_ABI_VERSION) {
        return fail_and_close("ABI version mismatch: plugin reports " + std::to_string(plugin_abi) +
                              ", this host speaks " + std::to_string(MS_ABI_VERSION) +
                              ". Rebuild the theory.");
    }

    // --- Describe -------------------------------------------------------
    const MS_StrategyDesc* desc = nullptr;
    if (!guarded_describe(describe_fn, &desc)) {
        return fail_and_close("ms_plugin_describe faulted");
    }
    if (desc == nullptr) {
        return fail_and_close("ms_plugin_describe returned null");
    }

    auto snapshot = std::make_unique<DescSnapshot>();
    if (!guarded_snapshot(desc, snapshot.get())) {
        return fail_and_close("ms_plugin_describe returned an unreadable descriptor");
    }
    if (snapshot->abi_version != MS_ABI_VERSION) {
        return fail_and_close("descriptor ABI version is " + std::to_string(snapshot->abi_version) +
                              ", expected " + std::to_string(MS_ABI_VERSION));
    }
    if (snapshot->id[0] == '\0') {
        return fail_and_close("descriptor has an empty id");
    }

    info.id = snapshot->id;
    info.display_name = snapshot->display_name[0] != '\0' ? snapshot->display_name : info.id;
    info.author = snapshot->author;
    info.description = snapshot->description;

    info.params.reserve(snapshot->param_count);
    for (std::uint32_t i = 0; i < snapshot->param_count; ++i) {
        const ParamSnapshot& source = snapshot->params[i];
        ParamInfo param;
        param.name = source.name;
        param.description = source.description;
        param.type = static_cast<MS_ParamType>(source.type);
        param.min_value = source.min_value;
        param.max_value = source.max_value;
        param.default_value = source.default_value;
        for (std::uint32_t j = 0; j < source.enum_count; ++j) {
            param.enum_labels.emplace_back(source.enum_labels[j]);
        }
        info.params.push_back(std::move(param));
    }

    info.required_symbols.reserve(snapshot->symbol_count);
    for (std::uint32_t i = 0; i < snapshot->symbol_count; ++i) {
        info.required_symbols.emplace_back(snapshot->symbols[i]);
    }

    // --- Duplicate ids --------------------------------------------------
    if (PluginInfo* existing = find_mutable(info.id); existing != nullptr) {
        if (existing->loaded() && existing->path != path) {
            return fail_and_close("id '" + info.id + "' is already provided by '" +
                                  existing->path.string() + "'; ids must be unique");
        }
        if (existing->live_instances > 0) {
            return fail_and_close("'" + info.id + "' has live instances and cannot be replaced");
        }
        // Same file, reloading: drop the previous module first.
        unload(info.id);
    }

    info.state = PluginState::Loaded;
    info.error.clear();

    Module module;
    module.id = info.id;
    module.handle = handle;
    module.create = create_fn;
    modules_->push_back(std::move(module));

    if (PluginInfo* existing = find_mutable(info.id); existing != nullptr) {
        *existing = std::move(info);
    } else {
        plugins_.push_back(std::move(info));
    }
    return true;
}

std::size_t PluginLoader::rescan() {
    const std::vector<std::filesystem::path> found = enumerate();

    // Drop entries whose file is gone, provided nothing is using them.
    for (auto it = plugins_.begin(); it != plugins_.end();) {
        const bool still_there = std::find(found.begin(), found.end(), it->path) != found.end();
        if (!still_there && it->live_instances == 0) {
            const std::string id = it->id;
            it = plugins_.erase(it);
            for (auto m = modules_->begin(); m != modules_->end(); ++m) {
                if (m->id == id) {
                    close_library(m->handle);
                    modules_->erase(m);
                    break;
                }
            }
        } else {
            ++it;
        }
    }

    for (const std::filesystem::path& path : found) {
        std::error_code ec;
        const auto last_write = std::filesystem::last_write_time(path, ec);
        const auto size = std::filesystem::file_size(path, ec);

        // Already loaded and unchanged: leave it alone. Reloading an unchanged
        // plugin would throw away strategy state for no reason.
        const PluginInfo* existing = nullptr;
        for (const PluginInfo& info : plugins_) {
            if (info.path == path) {
                existing = &info;
                break;
            }
        }
        if (existing != nullptr && existing->loaded() && existing->last_write == last_write &&
            existing->file_size == size) {
            continue;
        }
        if (existing != nullptr && existing->live_instances > 0) {
            continue;  // in use; the user must drop it before it can change
        }
        (void)load_file(path);
    }

    return loaded_count();
}

bool PluginLoader::unload(std::string_view id) {
    PluginInfo* info = find_mutable(id);
    if (info == nullptr) {
        return false;
    }
    if (info->live_instances > 0) {
        info->error = "cannot unload '" + info->id + "': " + std::to_string(info->live_instances) +
                      " instance(s) still live. Remove the theory from the run first.";
        return false;
    }

    for (auto it = modules_->begin(); it != modules_->end(); ++it) {
        if (it->id == info->id) {
            close_library(it->handle);
            modules_->erase(it);
            break;
        }
    }

    info->state = PluginState::Discovered;
    info->error.clear();

    std::error_code ec;
    std::filesystem::remove(info->shadow_path, ec);
    info->shadow_path.clear();
    return true;
}

bool PluginLoader::reload(std::string_view id) {
    const PluginInfo* info = find(id);
    if (info == nullptr) {
        return false;
    }
    const std::filesystem::path path = info->path;
    const std::string owned_id(id);
    if (!unload(owned_id)) {
        return false;
    }
    return load_file(path);
}

PluginInstance PluginLoader::create(std::string_view id) {
    PluginInstance instance;

    PluginInfo* info = find_mutable(id);
    if (info == nullptr || !info->loaded()) {
        return instance;
    }

    MS_PluginCreateFn create_fn = nullptr;
    for (const Module& module : *modules_) {
        if (module.id == info->id) {
            create_fn = module.create;
            break;
        }
    }
    if (create_fn == nullptr) {
        info->error = "'" + info->id + "' is marked loaded but has no module";
        info->state = PluginState::Failed;
        return instance;
    }

    MS_StrategyVTable vtable{};
    void* self = nullptr;
    if (!guarded_create(create_fn, &vtable, &self)) {
        info->state = PluginState::Failed;
        info->error = "ms_plugin_create faulted";
        return instance;
    }
    if (self == nullptr) {
        info->state = PluginState::Failed;
        info->error = "ms_plugin_create returned null; the theory's constructor refused";
        return instance;
    }
    if (vtable.destroy == nullptr) {
        // Without destroy there is no way to free the instance. Leaking it is
        // the lesser evil compared to guessing at how to delete it.
        info->state = PluginState::Failed;
        info->error = "ms_plugin_create returned a vtable with no destroy entry";
        return instance;
    }

    instance.self_ = self;
    instance.vtable_ = vtable;
    instance.owner_ = this;
    instance.plugin_id_ = info->id;
    ++info->live_instances;
    return instance;
}

void PluginLoader::release_instance(std::string_view id) noexcept {
    if (PluginInfo* info = find_mutable(id); info != nullptr && info->live_instances > 0) {
        --info->live_instances;
    }
}

bool PluginLoader::changes_available() const {
    const std::vector<std::filesystem::path> found = enumerate();

    std::size_t tracked = 0;
    for (const PluginInfo& info : plugins_) {
        if (std::find(found.begin(), found.end(), info.path) != found.end()) {
            ++tracked;
        } else {
            return true;  // something we know about is gone
        }
    }
    if (tracked != found.size()) {
        return true;  // something new appeared
    }

    for (const std::filesystem::path& path : found) {
        std::error_code ec;
        const auto last_write = std::filesystem::last_write_time(path, ec);
        const auto size = std::filesystem::file_size(path, ec);
        for (const PluginInfo& info : plugins_) {
            if (info.path == path && (info.last_write != last_write || info.file_size != size)) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace marketscan
