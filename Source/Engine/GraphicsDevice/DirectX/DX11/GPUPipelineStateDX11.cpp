// Copyright (c) Wojciech Figat. All rights reserved.

#if GRAPHICS_API_DIRECTX11

#include "GPUPipelineStateDX11.h"

void GPUPipelineStateDX11::OnReleaseGPU()
{
    DepthStencilState = nullptr;
    ReversedDepthStencilState = nullptr;
    BlendState = nullptr;
    VS = nullptr;
#if GPU_ALLOW_TESSELLATION_SHADERS
    HS = nullptr;
    DS = nullptr;
#endif
#if GPU_ALLOW_GEOMETRY_SHADERS
    GS = nullptr;
#endif
    PS = nullptr;
}

ID3D11Resource* GPUPipelineStateDX11::GetResource()
{
    return nullptr;
}

bool GPUPipelineStateDX11::IsValid() const
{
    return !!_memoryUsage;
}

bool GPUPipelineStateDX11::Init(const Description& desc)
{
    if (IsValid())
        OnReleaseGPU();

    // Cache shaders
    VS = (GPUShaderProgramVSDX11*)desc.VS;
#if GPU_ALLOW_TESSELLATION_SHADERS
    HS = desc.HS ? (GPUShaderProgramHSDX11*)desc.HS : nullptr;
    DS = desc.DS ? (GPUShaderProgramDSDX11*)desc.DS : nullptr;
#endif
#if GPU_ALLOW_GEOMETRY_SHADERS
    GS = desc.GS ? (GPUShaderProgramGSDX11*)desc.GS : nullptr;
#endif
    PS = desc.PS ? (GPUShaderProgramPSDX11*)desc.PS : nullptr;

    // Primitive Topology
    const static D3D11_PRIMITIVE_TOPOLOGY D3D11_primTypes[] =
    {
        D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED,
        D3D11_PRIMITIVE_TOPOLOGY_POINTLIST,
        D3D11_PRIMITIVE_TOPOLOGY_LINELIST,
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
    };
    PrimitiveTopology = D3D11_primTypes[static_cast<int32>(desc.PrimitiveTopology)];
    DepthBounds = desc.DepthBoundsEnable;
#if GPU_ALLOW_TESSELLATION_SHADERS
    if (HS)
        PrimitiveTopology = (D3D11_PRIMITIVE_TOPOLOGY)((int32)D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + (HS->GetControlPointsCount() - 1));
#endif

    // States
    RasterizerStateIndex = static_cast<int32>(desc.CullMode) + (desc.Wireframe ? 0 : 3) + (desc.DepthClipEnable ? 0 : 6);
    DepthStencilState = _device->GetDepthStencilState(&desc);
    ReversedDepthStencilState = DepthStencilState;
    if (desc.DepthEnable && (desc.DepthFunc == ComparisonFunc::Less || desc.DepthFunc == ComparisonFunc::LessEqual ||
                             desc.DepthFunc == ComparisonFunc::Greater || desc.DepthFunc == ComparisonFunc::GreaterEqual))
    {
        auto reversed = desc;
        // Flip the ordering and preserve equality.
        switch (desc.DepthFunc)
        {
        case ComparisonFunc::Less: reversed.DepthFunc = ComparisonFunc::Greater; break;
        case ComparisonFunc::LessEqual: reversed.DepthFunc = ComparisonFunc::GreaterEqual; break;
        case ComparisonFunc::Greater: reversed.DepthFunc = ComparisonFunc::Less; break;
        case ComparisonFunc::GreaterEqual: reversed.DepthFunc = ComparisonFunc::LessEqual; break;
        default: break;
        }
        ReversedDepthStencilState = _device->GetDepthStencilState(&reversed);
    }
    BlendState = _device->GetBlendState(desc.BlendMode);

    // Calculate approx. memory usage (just to set sth)
    _memoryUsage = sizeof(D3D11_DEPTH_STENCIL_DESC) + sizeof(D3D11_RASTERIZER_DESC) + sizeof(D3D11_BLEND_DESC);

    return GPUPipelineState::Init(desc);
}

#endif
