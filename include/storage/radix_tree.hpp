#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>

namespace kube32::storage {

struct RadixNode {
    std::string prefix;
    std::string value;
    uint64_t resource_version{0};
    bool is_leaf{false};
    bool is_ephemeral{false};
    std::vector<std::unique_ptr<RadixNode>> children;

    explicit RadixNode(std::string p = "") : prefix(std::move(p)) {}
};

class RadixTree {
public:
    RadixTree();
    ~RadixTree() = default;

    // Disallow copy, allow move
    RadixTree(const RadixTree&) = delete;
    RadixTree& operator=(const RadixTree&) = delete;
    RadixTree(RadixTree&&) noexcept = default;
    RadixTree& operator=(RadixTree&&) noexcept = default;

    bool put(const std::string& key, const std::string& value, uint64_t version, 
             bool ephemeral, bool overwrite, bool* out_is_new = nullptr);
    bool get(const std::string& key, std::string& out_value, 
             uint64_t* out_version = nullptr, bool* out_is_ephemeral = nullptr) const;
    bool remove(const std::string& key, std::string* out_deleted_value = nullptr, 
                uint64_t* out_deleted_version = nullptr);
    void list(const std::string& prefix, std::vector<std::string>& out_values, 
              std::vector<std::string>* out_keys = nullptr) const;
    void iterate_all(const std::function<void(const std::string& key, 
                                              const std::string& value, 
                                              uint64_t ver, bool ephemeral)>& fn) const;
    void clear();
    size_t count_nodes() const;
    size_t count_leaves() const;

private:
    std::unique_ptr<RadixNode> root_;

    static size_t common_prefix_len(const std::string& a, const std::string& b);
    bool insert_recursive(RadixNode* current, const std::string& key, 
                          const std::string& value, uint64_t version, 
                          bool ephemeral, bool overwrite, bool* out_is_new);
    const RadixNode* find_node(const RadixNode* current, const std::string& key) const;
    void collect_all(const RadixNode* node, const std::string& current_path, 
                     std::vector<std::string>& out_values, 
                     std::vector<std::string>* out_keys) const;
    void list_prefix(const RadixNode* current, const std::string& remaining_prefix, 
                     const std::string& current_path, std::vector<std::string>& out_values, 
                     std::vector<std::string>* out_keys) const;
    void iterate_recursive(const RadixNode* current, const std::string& current_path,
                           const std::function<void(const std::string&, const std::string&, 
                                                    uint64_t, bool)>& fn) const;
    bool remove_recursive(RadixNode* current, const std::string& key, 
                          std::string* out_val, uint64_t* out_ver);
    static void merge_node_with_child(RadixNode* node, size_t child_idx);
    static void maybe_merge_with_single_child(RadixNode* node);
    size_t count_nodes_recursive(const RadixNode* node) const;
    size_t count_leaves_recursive(const RadixNode* node) const;
};

} // namespace kube32::storage
