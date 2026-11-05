// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Math/Viewport.h"
class GPUTextureView;

/// <summary>Borrowed destination for one pipeline. Binding is performed by its stages.</summary>
struct RenderOutputTarget
{
    GPUTextureView* Color = nullptr;
    Viewport View;
};
