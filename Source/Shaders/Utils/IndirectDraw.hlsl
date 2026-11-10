// Copyright (c) Wojciech Figat. All rights reserved.

#ifndef __V2_INDIRECT_DRAW__
#define __V2_INDIRECT_DRAW__

StructuredBuffer<uint> ShadowSelectionFlags : register(t0);
StructuredBuffer<uint2> ShadowIndirectInputs : register(t1);
RWBuffer<uint> ShadowIndirectOutput : register(u0);

// Camera and shadow draws consume the same selected-instance argument layout.
void V2BuildIndirectDraw(uint index : SV_DispatchThreadID)
{
    if (index >= ShadowIndirectInputs[0].x)
        return;
    uint2 input = ShadowIndirectInputs[index + 1];
    uint offset = index * 5;
    ShadowIndirectOutput[offset + 0] = input.y;
    uint mask = ShadowIndirectInputs[0].y;
    uint flags = ShadowSelectionFlags[input.x];
    ShadowIndirectOutput[offset + 1] = (mask != 0 ? (flags & mask) : flags) != 0 ? 1 : 0;
    ShadowIndirectOutput[offset + 2] = 0;
    ShadowIndirectOutput[offset + 3] = 0;
    ShadowIndirectOutput[offset + 4] = 0;
}

#endif
