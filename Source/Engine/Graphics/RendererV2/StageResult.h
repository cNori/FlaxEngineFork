// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Config.h"

/// <summary>The outcome of one stage or pipeline execution.</summary>
API_ENUM() enum class StageResult
{
    /// <summary>Work completed and published resources are available to later stages.</summary>
    Completed,
    /// <summary>No work was performed; resource-table changes are discarded.</summary>
    Skipped,
    /// <summary>Execution failed; later render stages are not executed.</summary>
    Failed,
};
