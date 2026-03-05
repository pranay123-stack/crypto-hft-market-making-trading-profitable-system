#pragma once

/**
 * @file config_manager.hpp
 * @brief Configuration management system with YAML support and hot-reload capability
 *
 * This module provides a thread-safe, hierarchical configuration system for the HFT platform.
 * Features:
 * - YAML-based configuration loading using yaml-cpp
 * - Hot-reload support with file watching
 * - Configuration validation
 * - Default value handling
 * - Hierarchical access (e.g., config.get<int>("exchange.binance.rate_limit"))
 *
 * @author HFT System
 * @version 1.0
 */

#include <yaml-cpp/yaml.h>
#include <string>
#include <string_view>
#include <memory>
#include <shared_mutex>
#include <atomic>
#include <functional>
#include <unordered_map>
#include <vector>
#include <optional>
#include <chrono>
#include <filesystem>
#include <thread>
#include <stdexcept>
#include <type_traits>
#include <any>

namespace hft {
namespace config {

/**
 * @brief Exception class for configuration errors
 */
class ConfigException : public std::runtime_error {
public:
    explicit ConfigException(const std::string& message)
        : std::runtime_error("Configuration Error: " + message) {}

    ConfigException(const std::string& key, const std::string& message)
        : std::runtime_error("Configuration Error [" + key + "]: " + message) {}
};

/**
 * @brief Configuration validation rule
 */
struct ValidationRule {
    std::string path;
    std::function<bool(const YAML::Node&)> validator;
    std::string error_message;
    bool required;
};

/**
 * @brief File change event type
 */
enum class FileChangeEvent {
    Modified,
    Created,
    Deleted
};

/**
 * @brief Configuration change callback signature
 */
using ConfigChangeCallback = std::function<void(const std::string& path, FileChangeEvent event)>;

/**
 * @brief Thread-safe configuration manager with YAML support
 *
 * This class provides a centralized configuration management system with:
 * - Hierarchical configuration access using dot notation
 * - Type-safe value retrieval with defaults
 * - Hot-reload capability for runtime configuration updates
 * - Thread-safe access with read-write locking
 * - Configuration validation
 */
class ConfigManager {
public:
    /**
     * @brief Get the singleton instance of ConfigManager
     * @return Reference to the singleton ConfigManager instance
     */
    static ConfigManager& instance();

    // Prevent copying and moving
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
    ConfigManager(ConfigManager&&) = delete;
    ConfigManager& operator=(ConfigManager&&) = delete;

    /**
     * @brief Load configuration from a YAML file
     * @param filepath Path to the YAML configuration file
     * @param validate Whether to run validation after loading
     * @throws ConfigException if file cannot be loaded or validation fails
     */
    void load(const std::filesystem::path& filepath, bool validate = true);

    /**
     * @brief Load configuration from multiple YAML files
     * @param filepaths Vector of paths to YAML configuration files
     * @param validate Whether to run validation after loading
     * @throws ConfigException if any file cannot be loaded or validation fails
     */
    void load_multiple(const std::vector<std::filesystem::path>& filepaths, bool validate = true);

    /**
     * @brief Load configuration from a YAML string
     * @param yaml_content YAML content as string
     * @param source_name Name to identify this configuration source
     */
    void load_string(const std::string& yaml_content, const std::string& source_name = "string");

    /**
     * @brief Reload all previously loaded configuration files
     * @throws ConfigException if reload fails
     */
    void reload();

    /**
     * @brief Get a configuration value by path
     * @tparam T Type of the value to retrieve
     * @param path Dot-separated path (e.g., "exchange.binance.rate_limit")
     * @return The configuration value
     * @throws ConfigException if path doesn't exist or type conversion fails
     */
    template<typename T>
    T get(const std::string& path) const;

    /**
     * @brief Get a configuration value by path with default
     * @tparam T Type of the value to retrieve
     * @param path Dot-separated path
     * @param default_value Default value if path doesn't exist
     * @return The configuration value or default
     */
    template<typename T>
    T get(const std::string& path, const T& default_value) const;

    /**
     * @brief Get an optional configuration value
     * @tparam T Type of the value to retrieve
     * @param path Dot-separated path
     * @return Optional containing the value if it exists
     */
    template<typename T>
    std::optional<T> get_optional(const std::string& path) const;

    /**
     * @brief Check if a configuration path exists
     * @param path Dot-separated path
     * @return true if path exists
     */
    bool exists(const std::string& path) const;

    /**
     * @brief Get a YAML node at the specified path
     * @param path Dot-separated path
     * @return The YAML node at the path
     * @throws ConfigException if path doesn't exist
     */
    YAML::Node get_node(const std::string& path) const;

    /**
     * @brief Get all keys at a given path
     * @param path Dot-separated path to a map node
     * @return Vector of key names
     */
    std::vector<std::string> get_keys(const std::string& path) const;

    /**
     * @brief Set a configuration value programmatically
     * @tparam T Type of the value
     * @param path Dot-separated path
     * @param value Value to set
     */
    template<typename T>
    void set(const std::string& path, const T& value);

    /**
     * @brief Add a validation rule
     * @param rule Validation rule to add
     */
    void add_validation_rule(const ValidationRule& rule);

    /**
     * @brief Add multiple validation rules
     * @param rules Vector of validation rules
     */
    void add_validation_rules(const std::vector<ValidationRule>& rules);

    /**
     * @brief Validate the current configuration
     * @throws ConfigException if validation fails
     */
    void validate() const;

    /**
     * @brief Register a callback for configuration changes
     * @param callback Function to call when configuration changes
     * @return Callback ID for unregistering
     */
    uint64_t on_change(ConfigChangeCallback callback);

    /**
     * @brief Unregister a configuration change callback
     * @param callback_id ID returned by on_change
     */
    void remove_change_callback(uint64_t callback_id);

    /**
     * @brief Start watching configuration files for changes
     * @param poll_interval_ms Polling interval in milliseconds
     */
    void start_watching(std::chrono::milliseconds poll_interval = std::chrono::milliseconds(1000));

    /**
     * @brief Stop watching configuration files
     */
    void stop_watching();

    /**
     * @brief Check if file watching is active
     * @return true if watching is active
     */
    bool is_watching() const;

    /**
     * @brief Get the root YAML node (for advanced usage)
     * @return Copy of the root YAML node
     */
    YAML::Node get_root() const;

    /**
     * @brief Clear all loaded configuration
     */
    void clear();

    /**
     * @brief Get the last modification time of loaded configs
     * @return Map of filepath to last modification time
     */
    std::unordered_map<std::string, std::filesystem::file_time_type> get_file_timestamps() const;

    /**
     * @brief Merge another configuration into current
     * @param other YAML node to merge
     * @param overwrite Whether to overwrite existing values
     */
    void merge(const YAML::Node& other, bool overwrite = true);

    /**
     * @brief Export current configuration to YAML string
     * @return YAML string representation
     */
    std::string to_yaml_string() const;

    /**
     * @brief Save current configuration to file
     * @param filepath Path to save to
     */
    void save(const std::filesystem::path& filepath) const;

private:
    ConfigManager();
    ~ConfigManager();

    /**
     * @brief Navigate to a node given a dot-separated path
     * @param path Dot-separated path
     * @return Optional containing the node if found
     */
    std::optional<YAML::Node> navigate_to_node(const std::string& path) const;

    /**
     * @brief Parse a path into components
     * @param path Dot-separated path
     * @return Vector of path components
     */
    std::vector<std::string> parse_path(const std::string& path) const;

    /**
     * @brief Recursively merge two YAML nodes
     * @param target Target node
     * @param source Source node to merge from
     * @param overwrite Whether to overwrite existing values
     */
    void merge_nodes(YAML::Node& target, const YAML::Node& source, bool overwrite);

    /**
     * @brief File watcher thread function
     */
    void watch_files();

    /**
     * @brief Notify all registered callbacks
     * @param path Changed file path
     * @param event Type of change
     */
    void notify_callbacks(const std::string& path, FileChangeEvent event);

    // Configuration data
    YAML::Node root_;
    mutable std::shared_mutex mutex_;

    // File tracking
    std::vector<std::filesystem::path> loaded_files_;
    std::unordered_map<std::string, std::filesystem::file_time_type> file_timestamps_;

    // Validation
    std::vector<ValidationRule> validation_rules_;

    // Hot-reload
    std::atomic<bool> watching_{false};
    std::thread watcher_thread_;
    std::chrono::milliseconds poll_interval_{1000};

    // Callbacks
    std::unordered_map<uint64_t, ConfigChangeCallback> change_callbacks_;
    std::atomic<uint64_t> next_callback_id_{0};
    mutable std::shared_mutex callback_mutex_;
};

// ============================================================================
// Template Implementation
// ============================================================================

template<typename T>
T ConfigManager::get(const std::string& path) const {
    std::shared_lock lock(mutex_);

    auto node_opt = navigate_to_node(path);
    if (!node_opt.has_value()) {
        throw ConfigException(path, "Path does not exist");
    }

    try {
        return node_opt.value().as<T>();
    } catch (const YAML::Exception& e) {
        throw ConfigException(path, "Type conversion failed: " + std::string(e.what()));
    }
}

template<typename T>
T ConfigManager::get(const std::string& path, const T& default_value) const {
    std::shared_lock lock(mutex_);

    auto node_opt = navigate_to_node(path);
    if (!node_opt.has_value() || !node_opt.value().IsDefined()) {
        return default_value;
    }

    try {
        return node_opt.value().as<T>();
    } catch (const YAML::Exception&) {
        return default_value;
    }
}

template<typename T>
std::optional<T> ConfigManager::get_optional(const std::string& path) const {
    std::shared_lock lock(mutex_);

    auto node_opt = navigate_to_node(path);
    if (!node_opt.has_value() || !node_opt.value().IsDefined()) {
        return std::nullopt;
    }

    try {
        return node_opt.value().as<T>();
    } catch (const YAML::Exception&) {
        return std::nullopt;
    }
}

template<typename T>
void ConfigManager::set(const std::string& path, const T& value) {
    std::unique_lock lock(mutex_);

    auto components = parse_path(path);
    if (components.empty()) {
        throw ConfigException(path, "Invalid path");
    }

    // Navigate to parent node, creating intermediate nodes as needed
    YAML::Node current = root_;
    for (size_t i = 0; i < components.size() - 1; ++i) {
        if (!current[components[i]]) {
            current[components[i]] = YAML::Node(YAML::NodeType::Map);
        }
        current = current[components[i]];
    }

    current[components.back()] = value;
}

} // namespace config
} // namespace hft
