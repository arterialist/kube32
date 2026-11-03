// JSON Patch (RFC 6902) with JSON Pointer (RFC 6901) paths.
//
// The patch applies in place. Every step that changes the document appends
// its inverse to an undo log, and a failing operation replays the log in
// reverse, so a patch is applied whole or not at all without a copy of the
// document. Pointer walks are loops, never recursion, and value comparison
// keeps its work list on the heap.
#include "models/kube_models.hpp"

#include <utility>

namespace kube32::models {

namespace {

// The most operations one patch may carry (kube-apiserver's limit)
constexpr size_t kMaxOperations = 10000;

using Tokens = std::vector<std::string>;

bool split_pointer(std::string_view pointer, Tokens& out, std::string& error) {
    out.clear();
    if (pointer.empty()) return true;
    if (pointer[0] != '/') {
        error = "JSON pointer \"" + std::string(pointer) + "\" must be empty or start with \"/\"";
        return false;
    }
    std::string token;
    for (size_t i = 1; i <= pointer.size(); ++i) {
        if (i == pointer.size() || pointer[i] == '/') {
            out.push_back(std::move(token));
            token.clear();
            continue;
        }
        if (pointer[i] == '~') {
            if (i + 1 >= pointer.size() || (pointer[i + 1] != '0' && pointer[i + 1] != '1')) {
                error = "JSON pointer \"" + std::string(pointer) + "\" has an invalid escape";
                return false;
            }
            token.push_back(pointer[++i] == '0' ? '~' : '/');
            continue;
        }
        token.push_back(pointer[i]);
    }
    return true;
}

// An array index: digits without a leading zero
bool parse_index(const std::string& token, size_t& out) {
    if (token.empty() || token.size() > 9) return false;
    if (token.size() > 1 && token[0] == '0') return false;
    size_t v = 0;
    for (char c : token) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<size_t>(c - '0');
    }
    out = v;
    return true;
}

size_t find_member(const JsonValue& object, const std::string& key) {
    for (size_t i = 0; i < object.obj_val.size(); ++i) {
        if (object.obj_val[i].first == key) return i;
    }
    return object.obj_val.size();
}

// The value `tokens[0, count)` names, or null
JsonValue* walk(JsonValue& root, const Tokens& tokens, size_t count) {
    JsonValue* at = &root;
    for (size_t i = 0; i < count; ++i) {
        const std::string& token = tokens[i];
        if (at->is_object()) {
            size_t m = find_member(*at, token);
            if (m == at->obj_val.size()) return nullptr;
            at = &at->obj_val[m].second;
        } else if (at->is_array()) {
            size_t idx = 0;
            if (!parse_index(token, idx) || idx >= at->arr_val.size()) return nullptr;
            at = &at->arr_val[idx];
        } else {
            return nullptr;
        }
    }
    return at;
}

std::string quote_path(const std::string& path) { return "\"" + path + "\""; }

// Deep equality, comparing numbers by value and objects without regard to
// member order
bool same_value(const JsonValue& a, const JsonValue& b) {
    std::vector<std::pair<const JsonValue*, const JsonValue*>> work;
    work.emplace_back(&a, &b);
    while (!work.empty()) {
        auto [x, y] = work.back();
        work.pop_back();
        if (x->type != y->type) return false;
        switch (x->type) {
            case JsonType::Null: break;
            case JsonType::Boolean:
                if (x->bool_val != y->bool_val) return false;
                break;
            case JsonType::Number:
                if (x->num_val != y->num_val) return false;
                break;
            case JsonType::String:
                if (x->str_val != y->str_val) return false;
                break;
            case JsonType::Array:
                if (x->arr_val.size() != y->arr_val.size()) return false;
                for (size_t i = 0; i < x->arr_val.size(); ++i) work.emplace_back(&x->arr_val[i], &y->arr_val[i]);
                break;
            case JsonType::Object:
                if (x->obj_val.size() != y->obj_val.size()) return false;
                for (const auto& kv : x->obj_val) {
                    const JsonValue* other = y->get(kv.first);
                    if (!other) return false;
                    work.emplace_back(&kv.second, other);
                }
                break;
        }
    }
    return true;
}

class Patcher {
public:
    explicit Patcher(JsonValue& root) : root_(root) {}

    // false with `error` set; the document is then as it was
    bool apply(const JsonValue& op) {
        if (run(op)) return true;
        rollback();
        return false;
    }

    const std::string& error() const { return error_; }

private:
    bool run(const JsonValue& op) {
        const std::string kind = op.is_object() ? op.get_string("op") : std::string();
        const JsonValue* path = op.is_object() ? op.get("path") : nullptr;
        if (!path || !path->is_string()) {
            error_ = kind.empty() ? "Unexpected kind: " : kind + " operation does not apply: missing \"path\"";
            return false;
        }
        const JsonValue* value = op.get("value");
        const JsonValue* from = op.get("from");
        bool ok = false;
        if (kind == "add" || kind == "replace" || kind == "test") {
            if (!value) {
                error_ = kind + " operation does not apply: missing \"value\"";
            } else if (kind == "add") {
                ok = add(path->str_val, JsonValue(*value));
            } else if (kind == "replace") {
                ok = replace(path->str_val, JsonValue(*value));
            } else {
                ok = test(path->str_val, *value);
            }
        } else if (kind == "remove") {
            ok = remove(path->str_val, nullptr);
        } else if (kind == "move" || kind == "copy") {
            if (!from || !from->is_string()) {
                error_ = kind + " operation does not apply: missing \"from\"";
            } else {
                ok = kind == "move" ? move(from->str_val, path->str_val) : copy(from->str_val, path->str_val);
            }
        } else {
            error_ = "Unexpected kind: " + kind;
        }
        return ok;
    }

    enum class Step { Erase, Insert, Set, SetRoot };
    struct Undo {
        Step step;
        Tokens parent; // the container, as a path
        size_t index;
        std::string key; // Insert into an object
        JsonValue value; // Insert, Set and SetRoot
    };

    JsonValue& root_;
    std::vector<Undo> log_;
    std::string error_;

    void rollback() {
        while (!log_.empty()) {
            Undo u = std::move(log_.back());
            log_.pop_back();
            if (u.step == Step::SetRoot) {
                root_ = std::move(u.value);
                continue;
            }
            JsonValue* at = walk(root_, u.parent, u.parent.size());
            if (!at) continue;
            const bool object = at->is_object();
            switch (u.step) {
                case Step::Erase:
                    if (object) at->obj_val.erase(at->obj_val.begin() + static_cast<std::ptrdiff_t>(u.index));
                    else at->arr_val.erase(at->arr_val.begin() + static_cast<std::ptrdiff_t>(u.index));
                    break;
                case Step::Insert:
                    if (object) at->obj_val.insert(at->obj_val.begin() + static_cast<std::ptrdiff_t>(u.index), {std::move(u.key), std::move(u.value)});
                    else at->arr_val.insert(at->arr_val.begin() + static_cast<std::ptrdiff_t>(u.index), std::move(u.value));
                    break;
                case Step::Set:
                    if (object) at->obj_val[u.index].second = std::move(u.value);
                    else at->arr_val[u.index] = std::move(u.value);
                    break;
                case Step::SetRoot: break;
            }
        }
    }

    bool fail(const char* what, const std::string& path, const char* why) {
        error_ = std::string(what) + " operation does not apply: doc is missing " + why + ": " + quote_path(path) + ": missing value";
        return false;
    }

    static Tokens parent_of(const Tokens& t) { return Tokens(t.begin(), t.end() - 1); }

    bool add(const std::string& path, JsonValue value) {
        Tokens t;
        if (!split_pointer(path, t, error_)) return false;
        if (t.empty()) {
            log_.push_back({Step::SetRoot, {}, 0, {}, std::move(root_)});
            root_ = std::move(value);
            return true;
        }
        JsonValue* parent = walk(root_, t, t.size() - 1);
        if (!parent || !(parent->is_object() || parent->is_array())) return fail("add", path, "path");
        const std::string& last = t.back();
        if (parent->is_object()) {
            size_t m = find_member(*parent, last);
            if (m < parent->obj_val.size()) {
                log_.push_back({Step::Set, parent_of(t), m, {}, std::move(parent->obj_val[m].second)});
                parent->obj_val[m].second = std::move(value);
            } else {
                log_.push_back({Step::Erase, parent_of(t), parent->obj_val.size(), {}, {}});
                parent->obj_val.emplace_back(last, std::move(value));
            }
            return true;
        }
        size_t idx = parent->arr_val.size();
        if (last != "-" && (!parse_index(last, idx) || idx > parent->arr_val.size())) return fail("add", path, "path");
        log_.push_back({Step::Erase, parent_of(t), idx, {}, {}});
        parent->arr_val.insert(parent->arr_val.begin() + static_cast<std::ptrdiff_t>(idx), std::move(value));
        return true;
    }

    bool remove(const std::string& path, JsonValue* gone) {
        Tokens t;
        if (!split_pointer(path, t, error_)) return false;
        if (t.empty()) {
            error_ = "remove operation does not apply: the document root cannot be removed";
            return false;
        }
        JsonValue* parent = walk(root_, t, t.size() - 1);
        if (!parent) return fail("remove", path, "path");
        if (parent->is_object()) {
            size_t m = find_member(*parent, t.back());
            if (m == parent->obj_val.size()) return fail("remove", path, "path");
            if (gone) *gone = parent->obj_val[m].second;
            log_.push_back({Step::Insert, parent_of(t), m, std::move(parent->obj_val[m].first), std::move(parent->obj_val[m].second)});
            parent->obj_val.erase(parent->obj_val.begin() + static_cast<std::ptrdiff_t>(m));
            return true;
        }
        size_t idx = 0;
        if (!parent->is_array() || !parse_index(t.back(), idx) || idx >= parent->arr_val.size()) return fail("remove", path, "path");
        if (gone) *gone = parent->arr_val[idx];
        log_.push_back({Step::Insert, parent_of(t), idx, {}, std::move(parent->arr_val[idx])});
        parent->arr_val.erase(parent->arr_val.begin() + static_cast<std::ptrdiff_t>(idx));
        return true;
    }

    bool replace(const std::string& path, JsonValue value) {
        Tokens t;
        if (!split_pointer(path, t, error_)) return false;
        if (t.empty()) {
            log_.push_back({Step::SetRoot, {}, 0, {}, std::move(root_)});
            root_ = std::move(value);
            return true;
        }
        JsonValue* parent = walk(root_, t, t.size() - 1);
        if (!parent) return fail("replace", path, "path");
        if (parent->is_object()) {
            size_t m = find_member(*parent, t.back());
            if (m == parent->obj_val.size()) return fail("replace", path, "path");
            log_.push_back({Step::Set, parent_of(t), m, {}, std::move(parent->obj_val[m].second)});
            parent->obj_val[m].second = std::move(value);
            return true;
        }
        size_t idx = 0;
        if (!parent->is_array() || !parse_index(t.back(), idx) || idx >= parent->arr_val.size()) return fail("replace", path, "path");
        log_.push_back({Step::Set, parent_of(t), idx, {}, std::move(parent->arr_val[idx])});
        parent->arr_val[idx] = std::move(value);
        return true;
    }

    bool test(const std::string& path, const JsonValue& want) {
        Tokens t;
        if (!split_pointer(path, t, error_)) return false;
        const JsonValue* at = walk(root_, t, t.size());
        if (!at || !same_value(*at, want)) {
            error_ = "testing value " + path + " failed: test failed";
            return false;
        }
        return true;
    }

    bool copy(const std::string& from, const std::string& path) {
        Tokens t;
        if (!split_pointer(from, t, error_)) return false;
        const JsonValue* src = walk(root_, t, t.size());
        if (!src) return fail("copy", from, "from path");
        return add(path, JsonValue(*src));
    }

    bool move(const std::string& from, const std::string& path) {
        Tokens f, t;
        if (!split_pointer(from, f, error_) || !split_pointer(path, t, error_)) return false;
        if (!walk(root_, f, f.size())) return fail("move", from, "from path");
        if (f == t) return true;
        if (f.size() < t.size() && std::equal(f.begin(), f.end(), t.begin())) {
            error_ = "move operation does not apply: cannot move " + quote_path(from) + " into its own child " + quote_path(path);
            return false;
        }
        JsonValue moved;
        if (!remove(from, &moved)) return false;
        return add(path, std::move(moved));
    }
};

} // namespace

bool is_json_patch(std::string_view content_type) { return content_type.rfind("application/json-patch+json", 0) == 0; }

int apply_json_patch(JsonValue& target, const JsonValue& patch, std::string& error) {
    if (!patch.is_array()) {
        error = "the JSON patch must be an array of operations";
        return 400;
    }
    if (patch.arr_val.size() > kMaxOperations) {
        error = "Request entity too large: The allowed maximum operations in a JSON patch is " + std::to_string(kMaxOperations) + ", got " +
                std::to_string(patch.arr_val.size());
        return 413;
    }
    Patcher patcher(target);
    for (const JsonValue& op : patch.arr_val) {
        if (!patcher.apply(op)) {
            error = patcher.error();
            return 422;
        }
    }
    return 0;
}

} // namespace kube32::models
