#pragma once

#include <memory>
#include <type_traits>
#include <utility>

namespace kube32::mem {

// A non-owning reference to a callable, for calls that must not allocate:
// std::function copies a lambda that captures more than two pointers to the
// heap. The callable is invoked in place and must outlive the reference
// (pass it down a call, do not store it).
template <typename Signature>
class FunctionRef;

template <typename R, typename... Args>
class FunctionRef<R(Args...)> {
public:
    template <typename F, typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, FunctionRef> &&
                                                      std::is_invocable_r_v<R, F&, Args...>>>
    FunctionRef(F&& fn) noexcept   // NOLINT: implicit, like std::function
        : obj_(const_cast<void*>(static_cast<const void*>(std::addressof(fn)))),
          call_([](void* obj, Args... args) -> R {
              return (*static_cast<std::remove_reference_t<F>*>(obj))(std::forward<Args>(args)...);
          }) {}

    R operator()(Args... args) const { return call_(obj_, std::forward<Args>(args)...); }

private:
    void* obj_;
    R (*call_)(void*, Args...);
};

} // namespace kube32::mem
