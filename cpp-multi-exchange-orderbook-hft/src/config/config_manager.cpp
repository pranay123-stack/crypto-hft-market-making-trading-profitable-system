/**
 * @file config_manager.cpp
 * @brief Implementation of the configuration management system
 */

#include "config/config_manager.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace hft {
namespace config {

// ============================================================================
// Singleton Implementation
// ============================================================================

ConfigManager& ConfigManager::instance() {
    static ConfigManager instance;
    return instance;
}

ConfigManager::ConfigManager() : root_(YAML::NodeType::Map) {}

ConfigManager::~ConfigManager() {
    stop_watching();
}

// ============================================================================
// Loading Methods
// ============================================================================

void ConfigManager::load(const std::filesystem::path& filepath, bool validate_config) {
    if (!std::filesystem::exists(filepath)) {
        throw ConfigException("File does not exist: " + filepath.string());
    }

    YAML::Node new_config;
    try {
        new_config = YAML::LoadFile(filepath.string());
    } catch (const YAML::Exception& e) {
        throw ConfigException("Failed to parse YAML file " + filepath.string() + ": " + e.what());
    }

    {
        std::unique_lock lock(mutex_);

        // Merge with existing configuration
        merge_nodes(root_, new_config, true);

        // Track the file
        auto canonical_path = std::filesystem::canonical(filepath);
        if (std::find(loaded_files_.begin(), loaded_files_.end(), canonical_path) == loaded_files_.end()) {
            loaded_files_.push_back(canonical_path);
        }
        file_timestamps_[canonical_path.string()] = std::filesystem::last_write_time(canonical_path);
    }

    if (validate_config && !validation_rules_.empty()) {
        validate();
    }
}

void ConfigManager::load_multiple(const std::vector<std::filesystem::path>& filepaths, bool validate_config) {
    for (const auto& filepath : filepaths) {
        load(filepath, false);  // Defer validation until all files are loaded
    }

    if (validate_config && !validation_rules_.empty()) {
        validate();
    }
}

void ConfigManager::load_string(const std::string& yaml_content, const std::string& source_name) {
    YAML::Node new_config;
    try {
        new_config = YAML::Load(yaml_content);
    } catch (const YAML::Exception& e) {
        throw ConfigException("Failed to parse YAML from " + source_name + ": " + e.what());
    }

    std::unique_lock lock(mutex_);
    merge_nodes(root_, new_config, true);
}

void ConfigManager::reload() {
    std::vector<std::filesystem::path> files_to_reload;

    {
        std::shared_lock lock(mutex_);
        files_to_reload = loaded_files_;
    }

    // Clear current config and reload all files
    {
        std::unique_lock lock(mutex_);
        root_ = YAML::Node(YAML::NodeType::Map);
    }

    for (const auto& filepath : files_to_reload) {
        load(filepath, false);
    }

    if (!validation_rules_.empty()) {
        validate();
    }

    // Notify callbacks
    for (const auto& filepath : files_to_reload) {
        notify_callbacks(filepath.string(), FileChangeEvent::Modified);
    }
}

// ============================================================================
// Access Methods
// ============================================================================

bool ConfigManager::exists(const std::string& path) const {
    std::shared_lock lock(mutex_);
    auto node_opt = navigate_to_node(path);
    return node_opt.has_value() && node_opt.value().IsDefined();
}

YAML::Node ConfigManager::get_node(const std::string& path) const {
    std::shared_lock lock(mutex_);

    auto node_opt = navigate_to_node(path);
    if (!node_opt.has_value()) {
        throw ConfigException(path, "Path does not exist");
    }

    return YAML::Clone(node_opt.value());
}

std::vector<std::string> ConfigManager::get_keys(const std::string& path) const {
    std::shared_lock lock(mutex_);

    YAML::Node node;
    if (path.empty()) {
        node = root_;
    } else {
        auto node_opt = navigate_to_node(path);
        if (!node_opt.has_value()) {
            throw ConfigException(path, "Path does not exist");
        }
        node = node_opt.value();
    }

    if (!node.IsMap()) {
        throw ConfigException(path, "Node is not a map");
    }

    std::vector<std::string> keys;
    keys.reserve(node.size());

    for (const auto& kv : node) {
        keys.push_back(kv.first.as<std::string>());
    }

    return keys;
}

YAML::Node ConfigManager::get_root() const {
    std::shared_lock lock(mutex_);
    return YAML::Clone(root_);
}

// ============================================================================
// Validation
// ============================================================================

void ConfigManager::add_validation_rule(const ValidationRule& rule) {
    std::unique_lock lock(mutex_);
    validation_rules_.push_back(rule);
}

void ConfigManager::add_validation_rules(const std::vector<ValidationRule>& rules) {
    std::unique_lock lock(mutex_);
    validation_rules_.insert(validation_rules_.end(), rules.begin(), rules.end());
}

void ConfigManager::validate() const {
    std::shared_lock lock(mutex_);

    for (const auto& rule : validation_rules_) {
        auto node_opt = navigate_to_node(rule.path);

        if (!node_opt.has_value() || !node_opt.value().IsDefined()) {
            if (rule.required) {
                throw ConfigException(rule.path, "Required configuration missing: " + rule.error_message);
            }
            continue;
        }

        if (!rule.validator(node_opt.value())) {
            throw ConfigException(rule.path, rule.error_message);
        }
    }
}

// ============================================================================
// Hot-Reload / File Watching
// ============================================================================

uint64_t ConfigManager::on_change(ConfigChangeCallback callback) {
    std::unique_lock lock(callback_mutex_);
    uint64_t id = next_callback_id_++;
    change_callbacks_[id] = std::move(callback);
    return id;
}

void ConfigManager::remove_change_callback(uint64_t callback_id) {
    std::unique_lock lock(callback_mutex_);
    change_callbacks_.erase(callback_id);
}

void ConfigManager::start_watching(std::chrono::milliseconds poll_interval) {
    if (watching_.exchange(true)) {
        return;  // Already watching
    }

    poll_interval_ = poll_interval;
    watcher_thread_ = std::thread(&ConfigManager::watch_files, this);
}

void ConfigManager::stop_watching() {
    if (!watching_.exchange(false)) {
        return;  // Not watching
    }

    if (watcher_thread_.joinable()) {
        watcher_thread_.join();
    }
}

bool ConfigManager::is_watching() const {
    return watching_.load();
}

void ConfigManager::watch_files() {
    while (watching_.load()) {
        std::this_thread::sleep_for(poll_interval_);

        if (!watching_.load()) {
            break;
        }

        std::vector<std::pair<std::string, FileChangeEvent>> changes;

        {
            std::shared_lock lock(mutex_);

            for (const auto& filepath : loaded_files_) {
                if (!std::filesystem::exists(filepath)) {
                    // File was deleted
                    auto it = file_timestamps_.find(filepath.string());
                    if (it != file_timestamps_.end()) {
                        changes.emplace_back(filepath.string(), FileChangeEvent::Deleted);
                    }
                    continue;
                }

                auto current_time = std::filesystem::last_write_time(filepath);
                auto it = file_timestamps_.find(filepath.string());

                if (it == file_timestamps_.end()) {
                    // New file
                    changes.emplace_back(filepath.string(), FileChangeEvent::Created);
                } else if (it->second != current_time) {
                    // Modified file
                    changes.emplace_back(filepath.string(), FileChangeEvent::Modified);
                }
            }
        }

        // Process changes outside of the lock
        for (const auto& [path, event] : changes) {
            if (event == FileChangeEvent::Modified || event == FileChangeEvent::Created) {
                try {
                    reload();
                } catch (const ConfigException& e) {
                    // Log error but don't crash - configuration might be temporarily invalid
                    // In production, you'd want proper logging here
                }
            }
            notify_callbacks(path, event);
        }
    }
}

void ConfigManager::notify_callbacks(const std::string& path, FileChangeEvent event) {
    std::shared_lock lock(callback_mutex_);

    for (const auto& [id, callback] : change_callbacks_) {
        try {
            callback(path, event);
        } catch (...) {
            // Silently catch callback exceptions to prevent one bad callback
            // from affecting others
        }
    }
}

// ============================================================================
// Utility Methods
// ============================================================================

std::optional<YAML::Node> ConfigManager::navigate_to_node(const std::string& path) const {
    if (path.empty()) {
        return root_;
    }

    auto components = parse_path(path);
    YAML::Node current = root_;

    for (const auto& component : components) {
        if (!current.IsMap()) {
            return std::nullopt;
        }

        if (!current[component]) {
            return std::nullopt;
        }

        current = current[component];
    }

    return current;
}

std::vector<std::string> ConfigManager::parse_path(const std::string& path) const {
    std::vector<std::string> components;

    if (path.empty()) {
        return components;
    }

    std::string current;
    bool in_brackets = false;

    for (char c : path) {
        if (c == '[') {
            if (!current.empty()) {
                components.push_back(current);
                current.clear();
            }
            in_brackets = true;
        } else if (c == ']') {
            if (!current.empty()) {
                components.push_back(current);
                current.clear();
            }
            in_brackets = false;
        } else if (c == '.' && !in_brackets) {
            if (!current.empty()) {
                components.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }

    if (!current.empty()) {
        components.push_back(current);
    }

    return components;
}

void ConfigManager::merge_nodes(YAML::Node& target, const YAML::Node& source, bool overwrite) {
    if (!source.IsDefined() || source.IsNull()) {
        return;
    }

    if (!target.IsDefined() || target.IsNull()) {
        target = YAML::Clone(source);
        return;
    }

    if (source.IsMap() && target.IsMap()) {
        for (const auto& kv : source) {
            std::string key = kv.first.as<std::string>();

            if (!target[key] || !target[key].IsDefined()) {
                target[key] = YAML::Clone(kv.second);
            } else if (kv.second.IsMap() && target[key].IsMap()) {
                YAML::Node target_child = target[key];
                merge_nodes(target_child, kv.second, overwrite);
            } else if (overwrite) {
                target[key] = YAML::Clone(kv.second);
            }
        }
    } else if (overwrite) {
        target = YAML::Clone(source);
    }
}

void ConfigManager::clear() {
    std::unique_lock lock(mutex_);
    root_ = YAML::Node(YAML::NodeType::Map);
    loaded_files_.clear();
    file_timestamps_.clear();
}

std::unordered_map<std::string, std::filesystem::file_time_type> ConfigManager::get_file_timestamps() const {
    std::shared_lock lock(mutex_);
    return file_timestamps_;
}

void ConfigManager::merge(const YAML::Node& other, bool overwrite) {
    std::unique_lock lock(mutex_);
    merge_nodes(root_, other, overwrite);
}

std::string ConfigManager::to_yaml_string() const {
    std::shared_lock lock(mutex_);
    YAML::Emitter emitter;
    emitter << root_;
    return emitter.c_str();
}

void ConfigManager::save(const std::filesystem::path& filepath) const {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        throw ConfigException("Cannot open file for writing: " + filepath.string());
    }

    std::shared_lock lock(mutex_);
    YAML::Emitter emitter;
    emitter << root_;
    file << emitter.c_str();
}

} // namespace config
} // namespace hft
