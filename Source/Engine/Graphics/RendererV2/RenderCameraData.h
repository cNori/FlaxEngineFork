// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "ICamera.h"
#include "Engine/Core/Math/Matrix.h"
#include "Engine/Core/Math/Vector3.h"

/// <summary>A view-projection matrix operating on float positions relative to Origin.</summary>
struct RenderCameraData : ICamera
{
    // Borrowed source camera, when this view was prepared from a camera actor.
    ICamera* Source = nullptr;
    Matrix ViewProjection = Matrix::Identity;
    Vector3 Origin = Vector3::Zero;
    uint64 GetRenderTypeId() override;
};
