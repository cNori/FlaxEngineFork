// Copyright (c) Wojciech Figat. All rights reserved.
#pragma once
#include "NativeReflection.h"
#include <string>

namespace NativeReflection
{
    struct SnapshotIssue
    {
        std::string Path;
        Status Code;
    };

    /// <summary>Field-level progress. Restore is not a transaction across the whole object.</summary>
    struct SnapshotResult
    {
        unsigned Applied = 0;
        unsigned Skipped = 0;
        std::vector<SnapshotIssue> Issues;
        bool Succeeded() const { return Issues.empty(); }
    };

    /// <summary>
    /// Captures writable public instance fields into an owning, versioned Variant document.
    /// Private/protected, static, readonly, union, and NoSerialize fields are excluded.
    /// Native addresses and runtime object handles are never persisted.
    /// </summary>
    FLAXENGINE_API SnapshotResult Capture(const Type& type, const void* instance, Variant& output);

    /// <summary>
    /// Restores matching fields; missing/new fields are left untouched. Native type spellings
    /// must match. Malformed document structure is rejected before applying any changes.
    /// Value/converter failures are reported per field; other fields can still be restored.
    /// The caller owns the instance lifetime and synchronization.
    /// </summary>
    FLAXENGINE_API SnapshotResult Restore(const Type& type, void* instance, const Variant& input);

    template<typename T> SnapshotResult Capture(const T& instance, Variant& output)
    { return Capture(*TypeOf<T>(), std::addressof(instance), output); }
    template<typename T> SnapshotResult Restore(T& instance, const Variant& input)
    { return Restore(*TypeOf<T>(), std::addressof(instance), input); }
}
