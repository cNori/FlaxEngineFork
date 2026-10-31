// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#ifndef FLAX_DELEGATE_BINDINGS_ENABLED
#if defined(_WIN64) && (defined(_CPPUNWIND) || defined(__EXCEPTIONS) || defined(__cpp_exceptions))
#define FLAX_DELEGATE_BINDINGS_ENABLED 1
#else
#define FLAX_DELEGATE_BINDINGS_ENABLED 0
#endif
#endif

#include <memory>
#if FLAX_DELEGATE_BINDINGS_ENABLED
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#endif

// Optional lifetime leases for external subscriptions. Native Delegate objects
// do not allocate a lease until an external subscriber explicitly tracks them.
namespace DelegateBindings
{
#if FLAX_DELEGATE_BINDINGS_ENABLED
    struct Lifetime
    {
        std::recursive_mutex Gate;
        void* Source;
        std::uint64_t Next = 0;
        std::map<std::uint64_t, std::function<void(void*)>> Closing;
        std::mutex CopyGate;
        std::set<const void*> NonCopyable;

        explicit Lifetime(void* source) : Source(source) {}
    };
#else
    struct Lifetime;
#endif

    // The caller must supply a live Delegate. Hold Lifetime::Gate while binding
    // or detaching, and check Source before accessing the original Delegate.
    FLAXENGINE_API std::shared_ptr<Lifetime> Track(void* source);

    // Called at the start of Delegate destruction, before its callback list is
    // destroyed. Closing actions receive a still-live Delegate and must detach
    // their callbacks and drain reverse calls before returning. They cannot throw.
    FLAXENGINE_API void Retire(void* source) noexcept;
    FLAXENGINE_API bool ShouldCopy(const void* source, const void* callee) noexcept;
}
