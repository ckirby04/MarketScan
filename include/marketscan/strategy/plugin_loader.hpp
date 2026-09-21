#ifndef MARKETSCAN_STRATEGY_PLUGIN_LOADER_HPP
#define MARKETSCAN_STRATEGY_PLUGIN_LOADER_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "marketscan/abi/plugin_abi.h"

namespace marketscan {

class PluginLoader;

enum class PluginState : std::uint8_t {
    /// Found on disk, not yet loaded.
    Discovered = 0,
    Loaded,
    /// Refused. `error` says why, verbatim, and the GUI shows it.
    Failed,
};

[[nodiscard]] constexpr std::string_view to_string(PluginState state) noexcept {
    switch (state) {
        case PluginState::Discovered:
            return "discovered";
        case PluginState::Loaded:
            return "loaded";
        case PluginState::Failed:
            return "failed";
    }
    return "?";
}

/// A parameter description, copied out of the plugin's memory.
///
/// Copied rather than pointed at: the MS_ParamDesc strings belong to the DLL,
/// and the GUI must still be able to render a row explaining what it just
/// unloaded.
struct ParamInfo {
    std::string name;
    std::string description;
    MS_ParamType type = MS_PARAM_DOUBLE;
    double min_value = 0.0;
    double max_value = 0.0;
    double default_value = 0.0;
    std::vector<std::string> enum_labels;
};

struct PluginInfo {
    /// Where it was found, under one of the loader's roots.
    std::filesystem::path path;

    /// The copy that was actually loaded. See PluginLoader for why.
    std::filesystem::path shadow_path;

    std::string id;
    std::string display_name;
    std::string author;
    std::string description;

    std::uint32_t abi_version = 0;
    std::vector<ParamInfo> params;
    std::vector<std::string> required_symbols;

    PluginState state = PluginState::Discovered;

    /// Empty unless state is Failed. Shown to the user exactly as written.
    std::string error;

    std::filesystem::file_time_type last_write{};
    std::uintmax_t file_size = 0;

    /// Instances handed out and not yet destroyed. A plugin cannot be unloaded
    /// while this is non-zero.
    std::size_t live_instances = 0;

    [[nodiscard]] bool loaded() const noexcept { return state == PluginState::Loaded; }
};

/// One live strategy object from a plugin, owned.
///
/// Calls on_stop and then destroy when it goes out of scope, and tells the
/// loader that the module has one fewer reason to stay in memory. Move only:
/// two of these pointing at one instance would destroy it twice.
class PluginInstance {
public:
    PluginInstance() = default;
    PluginInstance(const PluginInstance&) = delete;
    PluginInstance& operator=(const PluginInstance&) = delete;
    PluginInstance(PluginInstance&& other) noexcept;
    PluginInstance& operator=(PluginInstance&& other) noexcept;
    ~PluginInstance();

    [[nodiscard]] bool valid() const noexcept { return self_ != nullptr; }

    [[nodiscard]] const std::string& plugin_id() const noexcept { return plugin_id_; }

    // Each of these checks the vtable slot first: the ABI says any callback
    // may be null, and a strategy that only wants bars should not have to
    // write five empty functions to say so.

    void start(const MS_HostContext* host);
    void bar(const MS_Bar& bar);
    void quote(const MS_Quote& quote);
    void trade(const MS_Trade& trade);
    void fill(const MS_Fill& fill);

    /// ABI v2. A no-op on a v1 plugin, whose vtable slot the loader trimmed to
    /// null -- which is the migration working rather than a failure to deliver.
    void order_status(const MS_OrderStatus& status);
    void stop();
    void set_param(const char* name, double value);
    [[nodiscard]] double get_param(const char* name) const;

    /// Releases the instance early, exactly as the destructor would.
    void reset() noexcept;

    /// Takes ownership of an instance no loader created -- one from the
    /// built-in registry, or one constructed directly in a test.
    ///
    /// The result behaves identically to one that came out of a DLL, which is
    /// the point: a strategy compiled into the host and a strategy loaded from
    /// disk travel the same path from here on.
    [[nodiscard]] static PluginInstance
    adopt(void* self, const MS_StrategyVTable& vtable, std::string id);

private:
    friend class PluginLoader;

    void* self_ = nullptr;
    MS_StrategyVTable vtable_{};
    PluginLoader* owner_ = nullptr;
    std::string plugin_id_;
    bool started_ = false;
};

/// Discovers, validates and loads theory plugins.
///
/// **Shadow copying.** Windows locks a DLL for as long as it is loaded, so an
/// author who rebuilds a theory while the app is running gets a link error
/// from their own build rather than a new plugin. Every discovered DLL is
/// therefore copied into a per-process directory under the system temp folder
/// and the *copy* is loaded. The original stays writable, which is what makes
/// rebuild-and-rescan work at all.
///
/// **Refusing, rather than crashing.** A plugin is hostile until proven
/// otherwise: it may be built against a different ABI, be missing exports, be
/// some unrelated DLL that happened to be in the folder, or fault outright
/// while being asked what it is. Every one of those produces a Failed entry
/// with a readable error and leaves the application running. On MSVC the calls
/// into plugin code during load are additionally wrapped in SEH, because an
/// access violation is not a C++ exception and no catch(...) will see it.
///
/// **Ordering.** Discovered plugins are sorted by path, so the set of loaded
/// theories -- and therefore the order they are given events in -- is the same
/// on every run regardless of what order the filesystem enumerated them in.
class PluginLoader {
public:
    /// `roots` are searched recursively for *.dll. Missing roots are skipped
    /// quietly; an empty theories directory is the normal state of a fresh
    /// checkout, not an error.
    explicit PluginLoader(std::vector<std::filesystem::path> roots);

    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;
    PluginLoader(PluginLoader&&) = delete;
    PluginLoader& operator=(PluginLoader&&) = delete;
    ~PluginLoader();

    /// Re-enumerates the roots and loads anything new or changed. Plugins that
    /// disappeared from disk are unloaded if nothing is using them.
    ///
    /// Returns the number of plugins currently in the Loaded state.
    std::size_t rescan();

    /// Loads one file directly, whether or not it lives under a root. Returns
    /// false and records a Failed entry on any problem.
    bool load_file(const std::filesystem::path& path);

    /// Frees the module.
    ///
    /// Refuses while any instance is still alive, because freeing a module out
    /// from under a live object turns its next virtual call into a jump into
    /// unmapped memory. Instances stop and destroy themselves when their owner
    /// drops them -- see PluginInstance -- so the sequence is: drop the
    /// strategies, then unload. That is what makes unloading safe to do while
    /// the engine is running.
    bool unload(std::string_view id);

    /// Unload, re-copy, load again. The point of the shadow copy.
    bool reload(std::string_view id);

    /// Instantiates a strategy. Returns an invalid instance if the plugin is
    /// not loaded or its create entry point refused.
    [[nodiscard]] PluginInstance create(std::string_view id);

    [[nodiscard]] const std::vector<PluginInfo>& plugins() const noexcept { return plugins_; }

    [[nodiscard]] const PluginInfo* find(std::string_view id) const noexcept;

    [[nodiscard]] std::size_t loaded_count() const noexcept;

    /// True when the files on disk differ from what was seen at the last
    /// rescan. Cheap enough to poll at 1 Hz; this is what raises the GUI's
    /// "reload available" badge.
    ///
    /// Reloading is never automatic. Swapping a strategy's code out from under
    /// a run mid-flight would silently invalidate every number on screen.
    [[nodiscard]] bool changes_available() const;

    [[nodiscard]] const std::vector<std::filesystem::path>& roots() const noexcept {
        return roots_;
    }

    [[nodiscard]] const std::filesystem::path& shadow_root() const noexcept { return shadow_root_; }

    /// The default place to look: a `theories` directory beside the
    /// executable, then one beside the source tree if it exists.
    [[nodiscard]] static std::vector<std::filesystem::path>
    default_roots(const std::filesystem::path& executable_dir);

private:
    friend class PluginInstance;

    struct Module;

    void release_instance(std::string_view id) noexcept;

    [[nodiscard]] PluginInfo* find_mutable(std::string_view id) noexcept;

    /// Enumerates every *.dll under the roots, sorted by path.
    [[nodiscard]] std::vector<std::filesystem::path> enumerate() const;

    void unload_all() noexcept;

    std::vector<std::filesystem::path> roots_;
    std::filesystem::path shadow_root_;
    std::vector<PluginInfo> plugins_;

    /// Native module handles, keyed by plugin id. Held behind a pointer to an
    /// incomplete type so that windows.h stays out of this header.
    std::unique_ptr<std::vector<Module>> modules_;

    std::uint64_t shadow_counter_ = 0;
};

}  // namespace marketscan

#endif  // MARKETSCAN_STRATEGY_PLUGIN_LOADER_HPP
