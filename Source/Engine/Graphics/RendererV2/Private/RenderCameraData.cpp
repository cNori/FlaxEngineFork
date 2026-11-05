// Copyright (c) Wojciech Figat. All rights reserved.

#include "../RenderCameraData.h"
#include "Engine/Platform/StringUtils.h"

uint64 RenderCameraData::GetRenderTypeId()
{
    static const uint64 typeId = StringUtils::GetHashCode(TEXT("RenderCameraData"));
    return typeId;
}
