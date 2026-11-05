// Copyright (c) Wojciech Figat. All rights reserved.

#include "../WorldWarpStage.h"
#include "Engine/Platform/StringUtils.h"

uint64 WarpedViewpoint::GetIC_RenderTypeId()
{
    if (Source)
        return Source->GetIC_RenderTypeId();
    static const uint64 typeId = StringUtils::GetHashCode(TEXT("WarpedCamera"));
    return typeId;
}
