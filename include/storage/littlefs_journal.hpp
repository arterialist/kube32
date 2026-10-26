#pragma once

#include <string>
#include <vector>
#include <utility>

namespace kube32::storage {

class LittleFSJournal {
public:
    explicit LittleFSJournal(std::string base_path = "/spiffs");
    ~LittleFSJournal() = default;

    std::string key_to_filepath(const std::string& key) const;
    std::string filepath_to_key(const std::string& filepath) const;

    bool atomic_write(const std::string& key, const std::string& content);
    bool remove_file(const std::string& key);
    void scan_and_recover(std::vector<std::pair<std::string, std::string>>& recovered_items);

    const std::string& base_path() const { return base_path_; }

private:
    std::string base_path_;

    static bool ensure_parent_dirs(const std::string& path);
    void scan_dir_recursive(const std::string& dir_path, 
                            std::vector<std::pair<std::string, std::string>>& out_items);
};

} // namespace kube32::storage
