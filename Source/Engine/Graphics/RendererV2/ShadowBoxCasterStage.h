// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipelineStage.h"
#include "Engine/Content/AssetReference.h"
#include "Engine/Core/Math/Vector3.h"
#include "Engine/Core/Math/Color.h"

class FrustumCullStage;
class OcclusionCullStage;
class Shader;
class GPUBuffer;
class GPUDevice;
class GPUTexture;
class GPUPipelineState;
class RenderBuffers;
class RenderPipelineGraphicSettings;

/// <summary>Selects directional shadow casters using projected world-space bounds.</summary>
API_CLASS() class FLAXENGINE_API ShadowBoxCasterStage : public RenderPipelineStage
{
    DECLARE_SCRIPTING_TYPE(ShadowBoxCasterStage);
private:
    const FrustumCullStage* FrustumStage = nullptr;
    const OcclusionCullStage* OcclusionStage = nullptr;
    Array<IVisualData*> Casters;
    Vector3 LightDirection = Vector3::Zero;
    float ProjectionDistance = 0.0f;
    int32 LastCasterCount = 0;
    int32 LastRejectedProxyCount = 0;
    bool LastUsedSelection = false;
    AssetReference<Shader> SelectionShader;
    GPUBuffer* BoundsBuffer = nullptr;
    GPUBuffer* SelectionBuffer = nullptr;
    GPUBuffer* SelectionReadback = nullptr;
    GPUBuffer* IndirectInputs = nullptr;
    GPUBuffer* IndirectOutput = nullptr;
    GPUBuffer* IndirectArguments = nullptr;
    GPUTexture* DebugMask = nullptr;
    struct ShadowCascadeMap
    {
        GPUTexture* Depth = nullptr;
        int32 RequestedResolution = 0;
    };
    Array<ShadowCascadeMap> ShadowCascadeMaps;
    RenderBuffers* LightDepthBuffers = nullptr;
    int32 RequestedAtlasWidth = 0;
    int32 RequestedAtlasHeight = 0;
    GPUPipelineState* DebugState = nullptr;
    GPUPipelineState* ResolveState = nullptr;
    GPUPipelineState* ShadowMaskState = nullptr;
    GPUPipelineState* CascadeMaskState = nullptr;
    GPUPipelineState* CascadeDebugState = nullptr;
    GPUTexture* CascadeDebug = nullptr;
    GPUPipelineState* ShadowFilterState = nullptr;
    GPUTexture* ShadowMask = nullptr;
    GPUTexture* FilteredShadowMask = nullptr;
    void OnShaderReloading(Asset* asset);
    int32 PrepareShadowAtlas(GPUDevice* device, int32 resolution, int32 farTileWidth);
    void PrepareShadowCascadeMaps(GPUDevice* device, const RenderPipelineGraphicSettings* settings);
public:
    ~ShadowBoxCasterStage();

    /// <summary>Meshes above this triangle count use a box for GPU caster selection.</summary>
    API_FIELD() int32 ProxyTriangleThreshold = 4096;

    /// <summary>Overlays camera pixels reached by an expensive caster's box.</summary>
    API_FIELD() bool ShowReceiverHits = false;

    /// <summary>Submits expensive shadow draws using GPU selection without a CPU readback.</summary>
    API_FIELD() bool UseIndirectShadowDraws = true;

    /// <summary>View-space distance of the high-detail directional shadow tier. Zero uses a single shadow map.</summary>
    API_FIELD() float NearShadowDistance = 2000.0f;

    /// <summary>Requested near-tile size, or full map size in single-map mode. Limited by the GPU and atlas layout.</summary>
    API_FIELD(Attributes="Limit(512, 16384, 512)") int32 ShadowMapResolution = 2048;

    /// <summary>Gets the allocated near-tile size, or full map size in single-map mode.</summary>
    API_PROPERTY() int32 GetAllocatedShadowMapResolution() const;

    /// <summary>Uses adaptive light-space sampling before falling back to the other filter modes.</summary>
    API_FIELD() bool UseAdaptiveShadowFilter = true;

    /// <summary>Adaptive filter radius in shadow-map texels.</summary>
    API_FIELD() float AdaptiveShadowRadius = 2.5f;

    /// <summary>Softens the directional shadow mask before applying lighting.</summary>
    API_FIELD() bool UseScreenSpaceShadowFilter = true;

    /// <summary>Directional light angular radius in degrees for contact-hardening shadows.</summary>
    API_FIELD() float ShadowAngularRadius = 0.27f;

    /// <summary>Linear color and strength of the directional light used by the preview.</summary>
    API_FIELD() Color LightColor = Color::White;
    API_FIELD() float LightIntensity = 3.0f;

    /// <summary>Strength of the uniform ambient term used by the preview.</summary>
    API_FIELD() float AmbientIntensity = 0.2f;

    void SetFrustumStage(const FrustumCullStage* stage) { FrustumStage = stage; }
    void SetOcclusionStage(const OcclusionCullStage* stage) { OcclusionStage = stage; }

    /// <summary>Sets the direction light travels and the maximum caster-to-receiver distance.</summary>
    API_FUNCTION() void SetProjection(const Vector3& lightDirection, float distance);

    /// <summary>The number of entries submitted for GPU selection in the last view.</summary>
    API_PROPERTY() int32 GetCasterCount() const { return LastCasterCount; }

    /// <summary>Gets the number of expensive casters skipped by the CPU readback path.</summary>
    API_PROPERTY() int32 GetRejectedProxyCount() const { return LastRejectedProxyCount; }

    /// <summary>Whether the last shadow draw consumed the GPU selection results.</summary>
    API_PROPERTY() bool GetUsedSelection() const { return LastUsedSelection; }

    /// <summary>Borrowed caster entries indexed by the Shadow.CasterSelection GPU buffer.</summary>
    const Array<IVisualData*>& GetCasters() const { return Casters; }

    StageResult Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources) override;
    void OnEndFrame(GPUContext* context) override;
};
