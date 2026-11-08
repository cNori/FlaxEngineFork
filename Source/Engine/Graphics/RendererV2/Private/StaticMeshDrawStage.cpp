// Copyright (c) Wojciech Figat. All rights reserved.

#include "../StaticMeshDrawStage.h"
#include "../WorldWarpStage.h"
#include "Engine/Graphics/GPUPipelineState.h"
#include "Engine/Content/Assets/MaterialBase.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Content/Content.h"
#include "Engine/Graphics/GPUBuffer.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/RenderTools.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/RenderBuffers.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/Graphics/Shaders/GPUShader.h"
#include "Engine/Graphics/Textures/GPUTexture.h"
#include "Engine/Level/Actors/Camera.h"
#include "Engine/Level/Actors/StaticModel.h"
#include "Engine/Profiler/Profiler.h"
#include "Engine/Renderer/GBufferPass.h"
#include "Engine/Renderer/RenderList.h"
#if GRAPHICS_API_DIRECTX11 && !REVERSE_Z
#include "Engine/GraphicsDevice/DirectX/DX11/GPUContextDX11.h"
#endif

RenderPipelineDrawStage::RenderPipelineDrawStage(const SpawnParams& params)
    : RenderPipelineStage(params)
{
}

RenderPipelineDrawStage::~RenderPipelineDrawStage()
{

}

StageResult RenderPipelineDrawStage::Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources)
{
    return StageResult::Completed;
}
