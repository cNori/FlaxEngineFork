// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipelineStage.h"
#include "Engine/Content/AssetReference.h"
#include "Engine/Core/Math/Vector3.h"
#include "Engine/Core/Math/Color.h"
#include "Engine/Core/Math/Vector4.h"
#include "Engine/Core/Math/Matrix.h"

class FrustumCullStage;
class OcclusionCullStage;
class Shader;
class GPUBuffer;
class GPUDevice;
class GPUTexture;
class GPUPipelineState;
class RenderBuffers;
struct RenderView;
class RenderPipelineGraphicSettings;

/// <summary>Selects directional shadow casters using projected world-space bounds.</summary>
API_CLASS() class FLAXENGINE_API ShadowBoxCasterStage : public RenderPipelineStage
{
    DECLARE_SCRIPTING_TYPE(ShadowBoxCasterStage);
private:
    const FrustumCullStage* FrustumStage = nullptr;
    const OcclusionCullStage* OcclusionStage = nullptr;
    Array<IVisualData*> Casters;
    // One current-view result per light projection (six for a point light).
    // GPU storage is reused; borrowed object pointers expire at EndFrame.
    struct PreparedSelection
    {
        GPUBuffer* Flags = nullptr;
        Matrix Projection;
        float Distance = 0;
        bool Perspective = false;
        int32 ReceiverWidth = 0;
        int32 ReceiverHeight = 0;
        Array<IVisualData*> Objects;
    };
    Array<PreparedSelection> PreparedSelections;
    int32 PreparedSelectionCount = 0;
    bool PreparingSelection = false;
    const RenderView* ProjectedView = nullptr;
    GPUTexture* ProjectedDepth = nullptr;
    GPUTexture* ProjectedResult = nullptr;
    Vector3 LightDirection = Vector3::Zero;
    float ProjectionDistance = 0.0f;
    int32 LastCasterCount = 0;
    int32 LastRejectedProxyCount = 0;
    int32 LastProxyCasterCount = 0;
    int32 LastSelectionDispatchCount = 0;
    int32 LastCascadeCount = 0;
    int32 LastReceiverWidth = 0;
    int32 LastReceiverHeight = 0;
    bool LastUsedSelection = false;
    AssetReference<Shader> SelectionShader;
    AssetReference<Shader> LightingShader;
    GPUBuffer* BoundsBuffer = nullptr;
    GPUBuffer* SelectionBuffer = nullptr;
    GPUBuffer* SelectionReadback = nullptr;
    GPUBuffer* IndirectInputs = nullptr;
    GPUBuffer* IndirectOutput = nullptr;
    GPUBuffer* IndirectArguments = nullptr;
    GPUTexture* CandidateOccluderDepth = nullptr;
    GPUBuffer* LightVisibilityBuffer = nullptr;
    GPUTexture* CameraReceiverMask = nullptr;
    GPUTexture* CameraReceiverNear = nullptr;
    GPUTexture* CameraReceiverFar = nullptr;
    GPUBuffer* CameraReceiverCasters = nullptr;
    GPUTexture* CameraReceiverTileMax = nullptr;
    // One geometry-depth target shared by every radial shadow band.
    GPUTexture* SharedCascadeDepth = nullptr;
    int32 SharedCascadeRequestedResolution = 0;
    // Cached derived depth maps. They share the source projection, not another geometry draw.
    struct BandDepthMap
    {
        GPUTexture* Texture = nullptr;
        // Maximum selected-sample displacement from the output texel centre, in UV.
        Float2 ReceiverFootprint = Float2::Zero;
    };
    Array<BandDepthMap> BandDepthMaps;
    GPUPipelineState* DepthReductionState = nullptr;
    GPUBuffer* BlurBandSettings = nullptr;
    GPUBuffer* CascadeResolveSettings = nullptr;
    GPUPipelineState* CombinedCascadeMaskState = nullptr;
    Float2 GetBandDepthFootprint(const GPUTexture* depth) const;
    GPUTexture* GetBandDepth(GPUContext* context, GPUTexture* source, int32 resolution, Array<GPUTexture*>& usedMaps);
    RenderBuffers* LightDepthBuffers = nullptr;
    int32 RequestedAtlasWidth = 0;
    int32 RequestedAtlasHeight = 0;
    GPUPipelineState* CasterBoundsDebugState = nullptr;
    GPUPipelineState* ResolveState = nullptr;
    GPUPipelineState* BaseLightingState = nullptr;
    int32 LastDirectionalLightCount = 0;
    StageResult RenderDirectionalLight(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources, int32 projectedResolution = 0, bool selectionOnly = false, bool drawCasterDebug = true);
    StageResult RenderBaseLighting(GPUContext* context, ICamera* viewpoint, RenderResourceTable& resources);
    GPUPipelineState* CascadeMaskState = nullptr;
    GPUPipelineState* PixelCascadeMaskState = nullptr;
    GPUPipelineState* CascadeDebugState = nullptr;
    GPUTexture* CascadeDebug = nullptr;
    GPUTexture* ShadowMask = nullptr;
    void OnShaderReloading(Asset* asset);
    int32 PrepareShadowAtlas(GPUDevice* device, int32 resolution, int32 farTileWidth);
    bool PrepareSharedShadowDepth(GPUDevice* device, const RenderPipelineGraphicSettings* settings);
public:
    ~ShadowBoxCasterStage();
    /// <summary>Runs current-view directional caster selection before material shading.</summary>
    StageResult PrepareSelections(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources);

    /// <summary>Meshes above this triangle count use a box for GPU caster selection.</summary>
    API_FIELD() int32 ProxyTriangleThreshold = 4096;

    /// <summary>Draws green wire boxes for expensive casters accepted by the potential-receiver test.</summary>
    API_FIELD() bool ShowSelectedExpensiveCasters = false;

    /// <summary>Draws red wire boxes for expensive casters rejected by the potential-receiver test. Boxes are visible through geometry.</summary>
    API_FIELD() bool ShowRejectedExpensiveCasters = false;

    /// <summary>Submits expensive shadow draws using GPU selection without a CPU readback.</summary>
    API_FIELD() bool UseIndirectShadowDraws = true;

    /// <summary>View-space distance of the high-detail directional shadow tier. Zero uses a single shadow map.</summary>
    API_FIELD() float NearShadowDistance = 2000.0f;

    /// <summary>Requested near-tile size, or full map size in single-map mode. Limited by the GPU and atlas layout.</summary>
    API_FIELD(Attributes="Limit(512, 16384, 512)") int32 ShadowMapResolution = 2048;

    /// <summary>Gets the shared shadow-depth size, or the legacy atlas near-tile size.</summary>
    API_PROPERTY() int32 GetAllocatedShadowMapResolution() const;





    /// <summary>Linear color and strength of the directional light used by the preview.</summary>
    API_FIELD() Color LightColor = Color::White;
    API_FIELD() float LightIntensity = 3.0f;

    /// <summary>Strength of the uniform ambient term used by the preview.</summary>
    API_FIELD() float AmbientIntensity = 0.2f;

    void SetFrustumStage(const FrustumCullStage* stage) { FrustumStage = stage; }
    void SetOcclusionStage(const OcclusionCullStage* stage) { OcclusionStage = stage; }

    /// <summary>Sets the direction light travels and the maximum caster-to-receiver distance.</summary>
    API_FUNCTION() void SetProjection(const Vector3& lightDirection, float distance);

    /// <summary>Uses the shared light/receiver selection and indirect depth submission with a supplied perspective light view.</summary>
    StageResult RenderProjectedShadowMap(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint,
        RenderResourceTable& resources, const RenderView& lightView, GPUTexture*& output, int32 resolution = 0, bool selectionOnly = false);

    /// <summary>Number of positive-intensity directional lights collected for this view. Manual fallback counts as one.</summary>
    API_PROPERTY() int32 GetDirectionalLightCount() const { return LastDirectionalLightCount; }

    /// <summary>The number of entries submitted for GPU selection in the last view.</summary>
    API_PROPERTY() int32 GetCasterCount() const { return LastCasterCount; }

    /// <summary>Gets the number of expensive casters skipped by the CPU readback path.</summary>
    API_PROPERTY() int32 GetRejectedProxyCount() const { return LastRejectedProxyCount; }

    /// <summary>Expensive caster candidates requiring a receiver scan in the last view, before GPU selection.</summary>
    API_PROPERTY() int32 GetProxyCasterCount() const { return LastProxyCasterCount; }

    /// <summary>Potential-caster receiver-search dispatches issued for the last view. One dispatch is shared by all cascades.</summary>
    API_PROPERTY() int32 GetSelectionDispatchCount() const { return LastSelectionDispatchCount; }

    /// <summary>Configured cascade slices processed in the last view. Zero uses the legacy atlas path.</summary>
    API_PROPERTY() int32 GetRenderedCascadeCount() const { return LastCascadeCount; }

    /// <summary>Width of the camera-depth input used for receiver selection.</summary>
    API_PROPERTY() int32 GetReceiverWidth() const { return LastReceiverWidth; }

    /// <summary>Height of the camera-depth input used for receiver selection.</summary>
    API_PROPERTY() int32 GetReceiverHeight() const { return LastReceiverHeight; }

    /// <summary>Whether the last shadow draw consumed the GPU selection results.</summary>
    API_PROPERTY() bool GetUsedSelection() const { return LastUsedSelection; }

    /// <summary>Borrowed caster entries indexed by the Shadow.CasterSelection GPU buffer.</summary>
    const Array<IVisualData*>& GetCasters() const { return Casters; }

    StageResult Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources) override;
    void OnEndFrame(GPUContext* context) override;
};
