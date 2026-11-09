// Copyright (c) Wojciech Figat. All rights reserved.

#include "../ShadowBoxCasterStage.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/GPUPipelineState.h"
#include "Engine/Graphics/Shaders/GPUConstantBuffer.h"
#include "Engine/Graphics/Shaders/GPUShader.h"
#include "Engine/Graphics/Textures/GPUTexture.h"
#include "Engine/Profiler/Profiler.h"

GPUTexture* ShadowBoxCasterStage::GetBandDepth(GPUContext* context, GPUTexture* source, int32 resolution, Array<GPUTexture*>& usedMaps)
{
    if (resolution >= source->Width())
        return source;

    // Intermediate steps cap reduction work at a 2:1 ratio per axis. This also
    // supports sizes such as 6144 and 12288 without rounding them to powers of two.
    while (source->Width() > resolution * 2)
    {
        source = GetBandDepth(context, source, Math::Max(resolution, source->Width() / 2), usedMaps);
        if (!source)
            return nullptr;
    }
    for (auto* map : usedMaps)
    {
        if (map->Width() == resolution)
            return map;
    }

    auto* device = context->GetDevice();
    GPUTexture* target = nullptr;
    int32 targetIndex = -1;
    for (int32 i = 0; i < BandDepthMaps.Count(); i++)
    {
        auto* map = BandDepthMaps[i].Texture;
        if (map->Width() == resolution && map->Format() == source->Format())
        {
            target = map;
            targetIndex = i;
            break;
        }
    }
    if (!target)
    {
        target = device->CreateTexture(TEXT("V2 Band Shadow Depth"));
        if (!target)
            return nullptr;
        BandDepthMap entry;
        entry.Texture = target;
        targetIndex = BandDepthMaps.Count();
        BandDepthMaps.Add(entry);
    }
    if (!target->GetMemoryUsage() && target->Init(GPUTextureDescription::New2D(resolution, resolution, source->Format(),
        GPUTextureFlags::ShaderResource | GPUTextureFlags::DepthStencil)))
        return nullptr;

    auto* shader = SelectionShader->GPU->GetPS("PS_V2ShadowDepthReduce");
    auto* constants = SelectionShader->GPU->GetCB(2);
    if (!DepthReductionState)
        DepthReductionState = device->CreatePipelineState();
    if (!shader || !constants || constants->GetSize() != sizeof(uint32) * 20 || !DepthReductionState)
        return nullptr;
    if (!DepthReductionState->IsValid())
    {
        auto desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        desc.PS = shader;
        desc.DepthEnable = true;
        desc.DepthWriteEnable = true;
        desc.DepthFunc = ComparisonFunc::Always;
        desc.BlendMode.RenderTargetWriteMask = BlendingMode::ColorWrite::None;
        if (DepthReductionState->Init(desc))
            return nullptr;
    }

    PROFILE_GPU_CPU_NAMED("V2 Shadow Reduce Depth");
    // Slot 2 shares the existing occlusion constant layout: matrix, then dimensions.
    uint32 data[20] = {};
    data[16] = data[17] = (uint32)resolution;
    context->ResetRenderTarget();
    context->ResetSR();
    context->UpdateCB(constants, data);
    context->BindCB(2, constants);
    context->BindSR(7, source);
    context->SetViewportAndScissors((float)resolution, (float)resolution);
    context->SetRenderTarget(target->View(), static_cast<GPUTextureView*>(nullptr));
    context->SetState(DepthReductionState);
    // Every output texel is overwritten, so no full-surface clear is necessary.
    context->DrawFullscreenTriangle();
    context->ResetRenderTarget();
    context->ResetSR();
    // Nearest-blocker reduction moves the stored sample away from the output
    // centre. Carry that support through the chain so receiver-plane comparison
    // does not mistake an ordinary sloping surface for a nearer blocker.
    const bool aligned = source->Width() % resolution == 0;
    const float radius = 0.5f / resolution + (aligned ? -0.5f : 0.5f) / source->Width();
    BandDepthMaps[targetIndex].ReceiverFootprint = GetBandDepthFootprint(source) + Float2(radius);
    usedMaps.Add(target);
    return target;
}

Float2 ShadowBoxCasterStage::GetBandDepthFootprint(const GPUTexture* depth) const
{
    for (const auto& map : BandDepthMaps)
    {
        if (map.Texture == depth)
            return map.ReceiverFootprint;
    }
    return Float2::Zero; // The original geometry depth is sampled at texel centres.
}
