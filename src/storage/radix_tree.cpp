#include "storage/radix_tree.hpp"
#include "osal/thread.hpp"
#include <algorithm>
#include <new>
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

bool RadixTree::put(const std::string& key, std::string value, uint64_t version, 
                    bool ephemeral, bool overwrite, bool* out_is_new) {
    if (key.empty()) return false;
    return insert_recursive(root_.get(), key, value, version, ephemeral, overwrite, out_is_new);
}

// Every allocation of an insert happens before the tree changes, so a
// bad_alloc leaves it whole. The value is moved in, not copied.
bool RadixTree::insert_recursive(RadixNode* current, const std::string& key, 
                                 std::string& value, uint64_t version, 
                                 bool ephemeral, bool overwrite, bool* out_is_new) {
    auto set_leaf = [&](RadixNode& node) {
        node.is_leaf = true;
        node.value = std::move(value);
        node.resource_version = version;
        node.is_ephemeral = ephemeral;
        node.on_flash = false;
    };
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
                    set_leaf(*child);
                    return true;
                } else {
                    return insert_recursive(child.get(), key.substr(cpl), value, 
                                            version, ephemeral, overwrite, out_is_new);
                }
            } else {
                // Split edge
                auto split_node = std::make_unique<RadixNode>(child->prefix.substr(cpl));
                std::string head = child->prefix.substr(0, cpl);
                std::unique_ptr<RadixNode> new_child;
                if (cpl != key.size()) new_child = std::make_unique<RadixNode>(key.substr(cpl));
                std::vector<std::unique_ptr<RadixNode>> kids;
                kids.reserve(2);

                split_node->value = std::move(child->value);
                split_node->resource_version = child->resource_version;
                split_node->is_leaf = child->is_leaf;
                split_node->is_ephemeral = child->is_ephemeral;
                split_node->on_flash = child->on_flash;
                split_node->children = std::move(child->children);

                child->prefix = std::move(head);
                child->is_leaf = false;
                child->on_flash = false;
                child->value.clear();
                kids.push_back(std::move(split_node));
                if (out_is_new) *out_is_new = true;
                if (new_child) {
                    set_leaf(*new_child);
                    kids.push_back(std::move(new_child));
                } else {
                    set_leaf(*child);
                }
                child->children = std::move(kids);
                return true;
            }
        }
    }

    // push_back leaves the children as they were when it cannot grow them
    auto new_node = std::make_unique<RadixNode>(key);
    set_leaf(*new_node);
    current->children.push_back(std::move(new_node));
    if (out_is_new) *out_is_new = true;
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
                    uint64_t* out_version, bool* out_is_ephemeral, bool* out_on_flash) const {
    const RadixNode* node = find_node(root_.get(), key);
    if (node && node->is_leaf) {
        out_value = node->value;
        if (out_version) *out_version = node->resource_version;
        if (out_is_ephemeral) *out_is_ephemeral = node->is_ephemeral;
        if (out_on_flash) *out_on_flash = node->on_flash;
        return true;
    }
    return false;
}

const std::string* RadixTree::value_of(const std::string& key, uint64_t* out_version, bool* out_is_ephemeral,
                                      bool* out_on_flash) const {
    const RadixNode* node = find_node(root_.get(), key);
    if (!node || !node->is_leaf) return nullptr;
    if (out_version) *out_version = node->resource_version;
    if (out_is_ephemeral) *out_is_ephemeral = node->is_ephemeral;
    if (out_on_flash) *out_on_flash = node->on_flash;
    return &node->value;
}

bool RadixTree::stat(const std::string& key, size_t* out_value_bytes, uint64_t* out_version,
                     bool* out_is_ephemeral, bool* out_on_flash) const {
    const RadixNode* node = find_node(root_.get(), key);
    if (!node || !node->is_leaf) return false;
    if (out_value_bytes) *out_value_bytes = node->value.size();
    if (out_version) *out_version = node->resource_version;
    if (out_is_ephemeral) *out_is_ephemeral = node->is_ephemeral;
    if (out_on_flash) *out_on_flash = node->on_flash;
    return true;
}

bool RadixTree::release_value(const std::string& key, uint64_t version) {
    RadixNode* node = const_cast<RadixNode*>(find_node(root_.get(), key));
    if (!node || !node->is_leaf || node->resource_version != version) return false;
    std::string().swap(node->value);
    node->on_flash = true;
    return true;
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
                    child->on_flash = false;
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

// Compaction after a remove. Out of memory it is skipped: a node with a
// single child is still a valid tree.
void RadixTree::merge_node_with_child(RadixNode* node, size_t child_idx) {
    std::string merged;
    try {
        merged = node->prefix + node->children[child_idx]->prefix;
    } catch (const std::bad_alloc&) {
        return;
    }
    auto child = std::move(node->children[child_idx]);
    node->prefix = std::move(merged);
    node->value = std::move(child->value);
    node->resource_version = child->resource_version;
    node->is_leaf = child->is_leaf;
    node->is_ephemeral = child->is_ephemeral;
    node->on_flash = child->on_flash;
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
