#include "storage/littlefs_journal.hpp"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>

namespace kube32::storage {

LittleFSJournal::LittleFSJournal(std::string base_path) : base_path_(std::move(base_path)) {
    // Strip trailing slashes
    while (base_path_.size() > 1 && base_path_.back() == '/') {
        base_path_.pop_back();
    }
}

std::string LittleFSJournal::key_to_filepath(const std::string& key) const {
    std::string cleaned_key = key;
    if (!cleaned_key.empty() && cleaned_key.front() == '/') {
        cleaned_key = cleaned_key.substr(1);
    }
    return base_path_ + "/" + cleaned_key + ".json";
}

std::string LittleFSJournal::filepath_to_key(const std::string& filepath) const {
    if (filepath.rfind(base_path_, 0) == 0) {
        std::string sub = filepath.substr(base_path_.size());
        if (!sub.empty() && sub.front() != '/') {
            sub = "/" + sub;
        }
        if (sub.size() > 5 && sub.substr(sub.size() - 5) == ".json") {
            return sub.substr(0, sub.size() - 5);
        }
    }
    return "";
}

bool LittleFSJournal::ensure_parent_dirs(const std::string& path) {
    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        std::string subdir = path.substr(0, pos);
        if (subdir.empty()) continue;
        struct stat st;
        if (stat(subdir.c_str(), &st) != 0) {
            if (mkdir(subdir.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
    }
    return true;
}

bool LittleFSJournal::atomic_write(const std::string& key, const std::string& content) {
    std::string target_file = key_to_filepath(key);
    std::string tmp_file = target_file + ".tmp";

    if (!ensure_parent_dirs(target_file)) {
        return false;
    }

    int fd = open(tmp_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;

    if (!content.empty()) {
        const char* ptr = content.data();
        size_t remaining = content.size();
        while (remaining > 0) {
            ssize_t written = write(fd, ptr, remaining);
            if (written <= 0) {
                close(fd);
                unlink(tmp_file.c_str());
                return false;
            }
            ptr += written;
            remaining -= static_cast<size_t>(written);
        }
    }

    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_file.c_str());
        return false;
    }

    if (close(fd) != 0) {
        unlink(tmp_file.c_str());
        return false;
    }

    if (rename(tmp_file.c_str(), target_file.c_str()) != 0) {
        unlink(tmp_file.c_str());
        return false;
    }

    return true;
}

bool LittleFSJournal::remove_file(const std::string& key) {
    std::string target_file = key_to_filepath(key);
    return unlink(target_file.c_str()) == 0;
}

void LittleFSJournal::scan_and_recover(std::vector<std::pair<std::string, std::string>>& recovered_items) {
    ensure_parent_dirs(base_path_ + "/dummy");
    scan_dir_recursive(base_path_, recovered_items);
}

void LittleFSJournal::scan_dir_recursive(const std::string& dir_path, 
                                        std::vector<std::pair<std::string, std::string>>& out_items) {
    DIR* d = opendir(dir_path.c_str());
    if (!d) return;

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        std::string full_path = dir_path + "/" + name;

        struct stat st;
        if (stat(full_path.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                scan_dir_recursive(full_path, out_items);
            } else if (S_ISREG(st.st_mode)) {
                // Orphan sweep: delete .tmp power-cut artifacts
                if (name.size() > 4 && name.substr(name.size() - 4) == ".tmp") {
                    unlink(full_path.c_str());
                } else if (name.size() > 5 && name.substr(name.size() - 5) == ".json") {
                    FILE* fp = fopen(full_path.c_str(), "rb");
                    if (fp) {
                        fseek(fp, 0, SEEK_END);
                        long sz = ftell(fp);
                        fseek(fp, 0, SEEK_SET);
                        std::string data(sz > 0 ? sz : 0, '\0');
                        if (sz > 0) {
                            size_t read_bytes = fread(&data[0], 1, sz, fp);
                            (void)read_bytes;
                        }
                        fclose(fp);
                        std::string key = filepath_to_key(full_path);
                        if (!key.empty()) {
                            out_items.emplace_back(std::move(key), std::move(data));
                        }
                    }
                }
            }
        }
    }
    closedir(d);
}

} // namespace kube32::storage
