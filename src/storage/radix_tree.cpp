#include "storage/radix_tree.hpp"
#include "osal/thread.hpp"
#include <algorithm>
#include <utility>

namespace kube32::storage {

RadixTree::RadixTree() : root_(std::make_unique<RadixNode>()) {}

size_t RadixTree::common_prefix_len(const std::string& a, const std::string& b) {
    size_t len = 0;
    while (len < a.size() && len < b.size() && a[len] == b[len]) {
        ++len;
    }
    return len;
}

bool RadixTree::put(const std::string& key, const std::string& value, uint64_t version, 
                    bool ephemeral, bool overwrite, bool* out_is_new) {
    if (key.empty()) return false;
    return insert_recursive(root_.get(), key, value, version, ephemeral, overwrite, out_is_new);
}

bool RadixTree::insert_recursive(RadixNode* current, const std::string& key, 
                                 const std::string& value, uint64_t version, 
                                 bool ephemeral, bool overwrite, bool* out_is_new) {
    for (auto& child : current->children) {
        if (child->prefix.empty()) continue;
        if (child->prefix[0] == key[0]) {
            size_t cpl = common_prefix_len(child->prefix, key);
            if (cpl == child->prefix.size()) {
                if (cpl == key.size()) {
                    if (child->is_leaf) {
                        if (!overwrite) return false;
                        if (out_is_new) *out_is_new = false;
                    } else {
                        if (out_is_new) *out_is_new = true;
                    }
                    child->is_leaf = true;
                    child->value = value;
                    child->resource_version = version;
                    child->is_ephemeral = ephemeral;
                    return true;
                } else {
                    return insert_recursive(child.get(), key.substr(cpl), value, 
                                            version, ephemeral, overwrite, out_is_new);
                }
            } else {
                // Split edge
                auto split_node = std::make_unique<RadixNode>(child->prefix.substr(cpl));
                split_node->value = std::move(child->value);
                split_node->resource_version = child->resource_version;
                split_node->is_leaf = child->is_leaf;
                split_node->is_ephemeral = child->is_ephemeral;
                split_node->children = std::move(child->children);

                child->prefix = child->prefix.substr(0, cpl);
                child->is_leaf = false;
                child->value.clear();
                child->children.clear();
                child->children.push_back(std::move(split_node));

                if (cpl == key.size()) {
                    if (out_is_new) *out_is_new = true;
                    child->is_leaf = true;
                    child->value = value;
                    child->resource_version = version;
                    child->is_ephemeral = ephemeral;
                } else {
                    if (out_is_new) *out_is_new = true;
                    auto new_child = std::make_unique<RadixNode>(key.substr(cpl));
                    new_child->is_leaf = true;
                    new_child->value = value;
                    new_child->resource_version = version;
                    new_child->is_ephemeral = ephemeral;
                    child->children.push_back(std::move(new_child));
                }
                return true;
            }
        }
    }

    if (out_is_new) *out_is_new = true;
    auto new_node = std::make_unique<RadixNode>(key);
    new_node->is_leaf = true;
    new_node->value = value;
    new_node->resource_version = version;
    new_node->is_ephemeral = ephemeral;
    current->children.push_back(std::move(new_node));
    return true;
}

const RadixNode* RadixTree::find_node(const RadixNode* current, const std::string& key) const {
    for (const auto& child : current->children) {
        if (child->prefix.empty()) continue;
        if (child->prefix[0] == key[0]) {
            size_t cpl = common_prefix_len(child->prefix, key);
            if (cpl == child->prefix.size()) {
                if (cpl == key.size()) {
                    return child.get();
                } else {
                    return find_node(child.get(), key.substr(cpl));
                }
            }
            return nullptr;
        }
    }
    return nullptr;
}

bool RadixTree::get(const std::string& key, std::string& out_value, 
                    uint64_t* out_version, bool* out_is_ephemeral) const {
    const RadixNode* node = find_node(root_.get(), key);
    if (node && node->is_leaf) {
        out_value = node->value;
        if (out_version) *out_version = node->resource_version;
        if (out_is_ephemeral) *out_is_ephemeral = node->is_ephemeral;
        return true;
    }
    return false;
}

void RadixTree::collect_all(const RadixNode* node, const std::string& current_path, 
                            std::vector<std::string>& out_values, 
                            std::vector<std::string>* out_keys) const {
    std::string full_path = current_path + node->prefix;
    if (node->is_leaf) {
        out_values.push_back(node->value);
        if (out_keys) out_keys->push_back(full_path);
    }
    for (const auto& child : node->children) {
        collect_all(child.get(), full_path, out_values, out_keys);
    }
    osal::Thread::yield();
}

void RadixTree::list_prefix(const RadixNode* current, const std::string& remaining_prefix, 
                            const std::string& current_path, std::vector<std::string>& out_values, 
                            std::vector<std::string>* out_keys) const {
    if (remaining_prefix.empty()) {
        for (const auto& child : current->children) {
            collect_all(child.get(), current_path, out_values, out_keys);
        }
        return;
    }

    for (const auto& child : current->children) {
        if (child->prefix.empty()) continue;
        if (child->prefix[0] == remaining_prefix[0]) {
            size_t cpl = common_prefix_len(child->prefix, remaining_prefix);
            if (cpl == remaining_prefix.size()) {
                // Prefix is satisfied within child prefix
                collect_all(child.get(), current_path, out_values, out_keys);
                return;
            } else if (cpl == child->prefix.size()) {
                // Child prefix fully matches initial part of remaining_prefix, recurse
                list_prefix(child.get(), remaining_prefix.substr(cpl), 
                            current_path + child->prefix, out_values, out_keys);
                return;
            } else {
                return; // Diverged
            }
        }
    }
}

void RadixTree::list(const std::string& prefix, std::vector<std::string>& out_values, 
                     std::vector<std::string>* out_keys) const {
    list_prefix(root_.get(), prefix, "", out_values, out_keys);
}

void RadixTree::iterate_recursive(const RadixNode* current, const std::string& current_path,
                                  const std::function<void(const std::string&, const std::string&, 
                                                           uint64_t, bool)>& fn) const {
    std::string full_path = current_path + current->prefix;
    if (current->is_leaf) {
        fn(full_path, current->value, current->resource_version, current->is_ephemeral);
    }
    for (const auto& child : current->children) {
        iterate_recursive(child.get(), full_path, fn);
    }
    osal::Thread::yield();
}

void RadixTree::iterate_all(const std::function<void(const std::string& key, 
                                                     const std::string& value, 
                                                     uint64_t ver, bool ephemeral)>& fn) const {
    iterate_recursive(root_.get(), "", fn);
}

bool RadixTree::remove(const std::string& key, std::string* out_deleted_value, 
                       uint64_t* out_deleted_version) {
    return remove_recursive(root_.get(), key, out_deleted_value, out_deleted_version);
}

bool RadixTree::remove_recursive(RadixNode* current, const std::string& key, 
                                 std::string* out_val, uint64_t* out_ver) {
    for (size_t i = 0; i < current->children.size(); ++i) {
        auto& child = current->children[i];
        if (child->prefix.empty()) continue;
        if (child->prefix[0] == key[0]) {
            size_t cpl = common_prefix_len(child->prefix, key);
            if (cpl == child->prefix.size()) {
                if (cpl == key.size()) {
                    if (!child->is_leaf) return false;
                    if (out_val) *out_val = std::move(child->value);
                    if (out_ver) *out_ver = child->resource_version;
                    child->is_leaf = false;
                    child->value.clear();

                    if (child->children.empty()) {
                        current->children.erase(current->children.begin() + i);
                        maybe_merge_with_single_child(current);
                    } else if (child->children.size() == 1) {
                        merge_node_with_child(child.get(), 0);
                    }
                    return true;
                } else {
                    bool res = remove_recursive(child.get(), key.substr(cpl), out_val, out_ver);
                    if (res) {
                        if (child->children.empty() && !child->is_leaf) {
                            current->children.erase(current->children.begin() + i);
                            maybe_merge_with_single_child(current);
                        } else if (child->children.size() == 1 && !child->is_leaf) {
                            merge_node_with_child(child.get(), 0);
                        }
                    }
                    return res;
                }
            }
            return false;
        }
    }
    return false;
}

void RadixTree::merge_node_with_child(RadixNode* node, size_t child_idx) {
    auto child = std::move(node->children[child_idx]);
    node->prefix += child->prefix;
    node->value = std::move(child->value);
    node->resource_version = child->resource_version;
    node->is_leaf = child->is_leaf;
    node->is_ephemeral = child->is_ephemeral;
    node->children = std::move(child->children);
}

void RadixTree::maybe_merge_with_single_child(RadixNode* node) {
    if (!node->is_leaf && node->children.size() == 1) {
        if (!node->prefix.empty()) { // Do not merge into virtual empty root
            merge_node_with_child(node, 0);
        }
    }
}

void RadixTree::clear() {
    root_ = std::make_unique<RadixNode>();
}

size_t RadixTree::count_nodes() const {
    return count_nodes_recursive(root_.get());
}

size_t RadixTree::count_nodes_recursive(const RadixNode* node) const {
    size_t count = 1;
    for (const auto& child : node->children) {
        count += count_nodes_recursive(child.get());
    }
    return count;
}

size_t RadixTree::count_leaves() const {
    return count_leaves_recursive(root_.get());
}

size_t RadixTree::count_leaves_recursive(const RadixNode* node) const {
    size_t count = node->is_leaf ? 1 : 0;
    for (const auto& child : node->children) {
        count += count_leaves_recursive(child.get());
    }
    return count;
}

} // namespace kube32::storage
