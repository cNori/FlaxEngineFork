// Copyright (c) Wojciech Figat. All rights reserved.

#include "DelegateBindings.h"
#if FLAX_DELEGATE_BINDINGS_ENABLED
#include <stdexcept>
#include <unordered_map>
#include <atomic>
#include <exception>

namespace DelegateBindings
{
    namespace
    {
        std::atomic<std::size_t> TrackedSources{0};
        struct Registry
        {
            std::mutex Gate;
            std::unordered_map<void*, std::weak_ptr<Lifetime>> Sources;
        };

        Registry& GetRegistry()
        {
            // Native-only bookkeeping remains valid during static teardown.
            static Registry* value = new Registry();
            return *value;
        }
    }

    std::shared_ptr<Lifetime> Track(void* source)
    {
        if (!source)
            throw std::invalid_argument("Cannot track a null Delegate.");
        auto& registry = GetRegistry();
        std::lock_guard<std::mutex> lock(registry.Gate);
        auto inserted = registry.Sources.emplace(source, std::weak_ptr<Lifetime>{});
        if (inserted.second)
            TrackedSources.fetch_add(1, std::memory_order_release);
        auto& entry = inserted.first->second;
        if (auto live = entry.lock())
            return live;
        auto live = std::make_shared<Lifetime>(source);
        entry = live;
        return live;
    }

    void Retire(void* source) noexcept
    {
        if (TrackedSources.load(std::memory_order_acquire) == 0)
            return;
        try
        {
            std::shared_ptr<Lifetime> lifetime;
            auto& registry = GetRegistry();
            {
                std::lock_guard<std::mutex> lock(registry.Gate);
                auto entry = registry.Sources.find(source);
                if (entry == registry.Sources.end())
                    return;
                lifetime = entry->second.lock();
            }
            if (!lifetime)
            {
                std::lock_guard<std::mutex> lock(registry.Gate);
                auto entry = registry.Sources.find(source);
                if (entry != registry.Sources.end() && entry->second.expired()) { registry.Sources.erase(entry); TrackedSources.fetch_sub(1, std::memory_order_release); }
                return;
            }
            std::map<std::uint64_t, std::function<void(void*)>> closing;
            {
                std::lock_guard<std::recursive_mutex> lock(lifetime->Gate);
                if (!lifetime->Source) return;
                lifetime->Source = nullptr;
                closing.swap(lifetime->Closing);
            }
            // Never run reverse calls under the registry or lifetime lock.
            for (auto& callback : closing)
            {
                try { callback.second(source); }
                catch (...) { /* Destruction cannot propagate callback errors. */ }
            }
            {
                std::lock_guard<std::mutex> lock(registry.Gate);
                auto entry = registry.Sources.find(source);
                if (entry != registry.Sources.end() && entry->second.lock() == lifetime)
                {
                    registry.Sources.erase(entry);
                    TrackedSources.fetch_sub(1, std::memory_order_release);
                }
            }
        }
        catch (...)
        {
            // Continuing destruction without retiring a live external lease
            // could leave callable pointers into released storage.
            std::terminate();
        }
    }

    bool ShouldCopy(const void* source, const void* callee) noexcept
    {
        if (TrackedSources.load(std::memory_order_acquire) == 0)
            return true;
        try
        {
            std::shared_ptr<Lifetime> lifetime;
            auto& registry = GetRegistry();
            {
                std::lock_guard<std::mutex> lock(registry.Gate);
                auto entry = registry.Sources.find(const_cast<void*>(source));
                if (entry == registry.Sources.end()) return true;
                lifetime = entry->second.lock();
            }
            if (!lifetime) return true;
            std::lock_guard<std::mutex> lock(lifetime->CopyGate);
            return lifetime->NonCopyable.find(callee) == lifetime->NonCopyable.end();
        }
        catch (...) { return false; }
    }
}
#else
namespace DelegateBindings
{
    std::shared_ptr<Lifetime> Track(void*) { return {}; }
    void Retire(void*) noexcept {}
    bool ShouldCopy(const void*, const void*) noexcept { return true; }
}
#endif
