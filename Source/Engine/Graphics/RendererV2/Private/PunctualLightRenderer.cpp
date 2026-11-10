// Copyright (c) Wojciech Figat. All rights reserved.

#include "../PunctualLightRenderer.h"
#include "../ILightData.h"
#include "../WorldWarpStage.h"
#include "../ShadowBoxCasterStage.h"
#include "../RenderPipelineGraphicSettings.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Graphics/GPUBuffer.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/GPUPipelineState.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/Graphics/Shaders/GPUShader.h"
#include "Engine/Graphics/Shaders/GPUConstantBuffer.h"
#include "Engine/Graphics/Textures/GPUTexture.h"
#include "Engine/Profiler/Profiler.h"

namespace
{
    struct PunctualProfileScope
    {
#if COMPILE_WITH_PROFILER
        ScopeProfileBlockCPU CPU;
        ScopeProfileBlockGPU GPU;
        explicit PunctualProfileScope(const Char* name) : CPU(name), GPU(name) {}
#else
        explicit PunctualProfileScope(const Char*) {}
#endif
    };
    const Float3 PointDirections[6] = { Float3::Right, Float3::Left, Float3::Up, Float3::Down, Float3::Forward, Float3::Backward };
    Rectangle GetSpotScissor(const Float3& position, const Float3& direction, float range, float angle,
        const BoundingFrustum& camera, const Matrix& lightVP, const Matrix& cameraVP, int32 width, int32 height)
    {
        // Support of the actual finite circular cone (cone intersected with the
        // light's range sphere), not a sphere centred on the lamp. Looking away
        // must not keep a spotlight just because its radius contains the camera.
        const float cosine = Math::Cos(angle), sine = Math::Sin(angle);
        const Real tolerance = Math::Max(range, 1.0f) * 1e-5f;
        Plane cameraPlanes[6], lightPlanes[6];
        const BoundingFrustum light(lightVP);
        for (int32 i = 0; i < 6; i++)
        {
            cameraPlanes[i] = camera.GetPlane(i);
            lightPlanes[i] = light.GetPlane(i);
            const auto& plane = cameraPlanes[i];
            const float along = Math::Clamp(Float3::Dot(Float3(plane.Normal), direction), -1.0f, 1.0f);
            const float support = along >= cosine ? range : range * Math::Max(0.0f,
                along * cosine + Math::Sqrt(Math::Max(0.0f, 1 - along * along)) * sine);
            if (Vector3::Dot(plane.Normal, Vector3(position)) + plane.D + support < -tolerance)
                return Rectangle(0, 0, 0, 0);
        }

        // Project only the intersection of the light pyramid and camera view.
        // Clip edges from BOTH convex volumes: this also handles containment and
        // a light crossing the camera near plane without falling back fullscreen.
        Vector3 cameraCorners[8], lightCorners[8];
        camera.GetCorners(cameraCorners);
        light.GetCorners(lightCorners);
        // Lighting starts at the source; shadow depth alone starts at its near plane.
        lightPlanes[4] = Plane(Vector3(position), Vector3(direction));
        for (int32 i = 0; i < 4; i++) lightCorners[i] = Vector3(position);
        const int32 edges[12][2] = {
            {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}
        };
        Float2 minimum(1), maximum(-1);
        bool any = false;
        const auto include = [&](const Vector3& point)
        {
            const Float4 clip = Float4::Transform(Float4(Float3(point), 1), cameraVP);
            if (clip.W <= 0) return;
            const Float2 ndc(clip.X / clip.W, clip.Y / clip.W);
            minimum = Float2::Min(minimum, ndc);
            maximum = Float2::Max(maximum, ndc);
            any = true;
        };
        const auto clipEdges = [&](const Vector3* corners, const Plane* planes)
        {
            for (const auto& edge : edges)
            {
                const Vector3 start = corners[edge[0]], delta = corners[edge[1]] - start;
                Real enter = 0, leave = 1;
                bool hit = true;
                for (int32 i = 0; i < 6; i++)
                {
                    const Real distance = Vector3::Dot(planes[i].Normal, start) + planes[i].D + tolerance;
                    const Real step = Vector3::Dot(planes[i].Normal, delta);
                    if (step > 0) enter = Math::Max(enter, -distance / step);
                    else if (step < 0) leave = Math::Min(leave, -distance / step);
                    else if (distance < 0) { hit = false; break; }
                    if (enter > leave) { hit = false; break; }
                }
                if (hit) { include(start + delta * enter); include(start + delta * leave); }
            }
        };
        clipEdges(lightCorners, cameraPlanes);
        clipEdges(cameraCorners, lightPlanes);
        if (!any) return Rectangle(0, 0, 0, 0);
        const float left = Math::Clamp(Math::Floor((minimum.X * 0.5f + 0.5f) * width) - 1, 0.0f, (float)width);
        const float top = Math::Clamp(Math::Floor((0.5f - maximum.Y * 0.5f) * height) - 1, 0.0f, (float)height);
        const float right = Math::Clamp(Math::Ceil((maximum.X * 0.5f + 0.5f) * width) + 1, 0.0f, (float)width);
        const float bottom = Math::Clamp(Math::Ceil((0.5f - minimum.Y * 0.5f) * height) + 1, 0.0f, (float)height);
        return Rectangle(left, top, Math::Max(right - left, 0.0f), Math::Max(bottom - top, 0.0f));
    }

    struct VisiblePunctualLight
    {
        ILightData* Source;
        Float3 Position, Direction, Up;
        float Range, Outer, Inner, NearPlane;
        Matrix View, Projection, ViewProjection;
        Matrix FaceViews[6];
        Rectangle FaceScissors[6];
        Rectangle Scissor;
    };

    GPU_CB_STRUCT(PunctualConstants
    {
        Matrix InverseViewProjection;
        Matrix LightViewProjection;
        Float4 CameraPosition; // XYZ in render space; W: orthographic view.
        Float4 CameraForward;
        Float4 LightPositionRange;
        Float4 LightDirectionOuter; // Travel direction, cosine of outer half-angle.
        Float4 LightRadiance;
        Float4 ConeFalloff; // Inner cosine, exponent, inverse-square mode, shadow opacity.
        Float4 ShadowInfo; // Map size, near, far, fixed-point depth precision.
        Float4 Options; // Has shadows, white surfaces, Pixel mode, filter-band count.
        Float4 DefaultFilter; // Radius, depth bias, normal bias, tan(outer half-angle).
    });
    struct PunctualFilterBand
    {
        Float4 Range;
        Float4 Parameters; // Filter radius in full map texels, depth bias, normal bias, reserved.
    };
}

PunctualLightRenderer::~PunctualLightRenderer()
{
    if (LightingShader)
        LightingShader->Reloading.Unbind<PunctualLightRenderer, &PunctualLightRenderer::OnShaderReloading>(this);
    SAFE_DELETE_GPU_RESOURCE(LightingState);
    SAFE_DELETE_GPU_RESOURCE(FilterBands);
    SAFE_DELETE_GPU_RESOURCE(PointShadowMaps);
    SAFE_DELETE_GPU_RESOURCE(PointFaceMatrices);
}

void PunctualLightRenderer::OnShaderReloading(Asset* asset)
{
    if (LightingState)
        LightingState->ReleaseGPU();
}

StageResult PunctualLightRenderer::Render(RenderLightKind kind, GPUContext* context,
    Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources, bool selectionOnly)
{
    const bool point = kind == RenderLightKind::Point;
    LastLightCount = LastCasterCount = 0;
    const auto* view = dynamic_cast<const WarpedViewpoint*>(viewpoint);
    if (!view || objects.IsEmpty())
        return StageResult::Skipped;
    auto* colour = resources.Get<GPUTexture>(TEXT("Scene.Color"));
    auto* depth = resources.Get<GPUTexture>(TEXT("Spot.ReceiverDepth"));
    auto* normals = resources.Get<GPUTexture>(TEXT("Scene.Normals"));
    auto* base = resources.Get<GPUTexture>(TEXT("Scene.BaseColor"));
    auto* material = resources.Get<GPUTexture>(TEXT("Scene.Material"));
    if (!colour || !depth || (!selectionOnly && (!normals || !base || !material)))
        return StageResult::Skipped;

    const auto* settings = resources.GetGraphicsSettings();
    if (settings && (settings->ShowShadowCascades || settings->ShowShadowBlurBands ||
        settings->ShowBlockerDistance || settings->ShowContactShadows))
        return StageResult::Skipped; // Keep the selected directional diagnostic unmodified.

    Vector3 origin = Vector3::Zero;
    if (const auto* pose = static_cast<const Transform*>(viewpoint->GetIC_Source()->GetIC_WorldTransform()))
        origin = pose->Translation - view->LocalTransform.Translation;
    const BoundingFrustum cameraFrustum(view->ViewProjection);
    Array<VisiblePunctualLight, InlinedAllocation<4>> lights;
    for (auto* light : resources.GetLights())
    {
        if (!light || light->GetILD_Type() != kind ||
            !(light->GetILD_Intensity() * light->GetILD_Color().A > 0) || !(light->GetILD_Range() > 0) ||
            (!point && (!(light->GetILD_OuterConeAngle() > 0) || light->GetILD_Direction().IsZero())))
            continue;
        VisiblePunctualLight item;
        item.Source = light;
        item.Position = Float3(light->GetILD_WorldPosition() - origin);
        item.Range = light->GetILD_Range();
        if (!cameraFrustum.Intersects(BoundingSphere(Vector3(item.Position), item.Range))) continue;
        if (point)
        {
            // A point light has no cone. Face scissoring below narrows its work;
            // a source behind the camera may still illuminate visible receivers.
            item.Direction = Float3::Forward;
            item.Outer = 45;
            item.Inner = 0;
            item.NearPlane = Math::Min(1.0f, item.Range * 0.001f);
            Matrix::PerspectiveFov(90 * DegreesToRadians, 1, item.NearPlane, item.Range, item.Projection);
            Float2 minimum((float)colour->Width(), (float)colour->Height()), maximum(0.0f);
            for (int32 face = 0; face < 6; face++)
            {
                const auto& direction = PointDirections[face];
                const Float3 up = Math::Abs(direction.Y) > 0.99f ? Float3::Forward : Float3::Up;
                Matrix vp;
                Matrix::LookAt(item.Position, item.Position + direction, up, item.FaceViews[face]);
                Matrix::Multiply(item.FaceViews[face], item.Projection, vp);
                auto& rect = item.FaceScissors[face];
                rect = GetSpotScissor(item.Position, direction, item.Range, Math::Atan(Math::Sqrt(2.0f)),
                    cameraFrustum, vp, view->ViewProjection, colour->Width(), colour->Height());
                if (rect.Size.X <= 0 || rect.Size.Y <= 0) continue;
                minimum = Float2::Min(minimum, rect.Location);
                maximum = Float2::Max(maximum, rect.Location + rect.Size);
            }
            item.Scissor = Rectangle(minimum, Float2::Max(maximum - minimum, Float2::Zero));
            if (item.Scissor.Size.X > 0 && item.Scissor.Size.Y > 0) lights.Add(item);
            continue;
        }
        item.Direction = Float3(light->GetILD_Direction()).GetNormalized();
        item.Outer = Math::Clamp(light->GetILD_OuterConeAngle(), 0.01f, 89.0f);
        item.Inner = Math::Clamp(light->GetILD_InnerConeAngle(), 0.0f, item.Outer);
        item.NearPlane = Math::Min(1.0f, item.Range * 0.001f);
        item.Up = Math::Abs(item.Direction.Y) > 0.99f ? Float3::Right : Float3::Up;
        Matrix::LookAt(item.Position, item.Position + item.Direction, item.Up, item.View);
        Matrix::PerspectiveFov(2 * item.Outer * DegreesToRadians, 1, item.NearPlane, item.Range, item.Projection);
        Matrix::Multiply(item.View, item.Projection, item.ViewProjection);
        item.Scissor = GetSpotScissor(item.Position, item.Direction, item.Range, item.Outer * DegreesToRadians,
            cameraFrustum, item.ViewProjection, view->ViewProjection, colour->Width(), colour->Height());
        if (item.Scissor.Size.X > 0 && item.Scissor.Size.Y > 0) lights.Add(item);
    }
    // No map clear, GPU upload, selection, material draw or lighting pass for a
    // spotlight whose actual cone cannot illuminate any part of the camera view.
    if (lights.IsEmpty()) return StageResult::Skipped;

    if (selectionOnly)
    {
        if (!ShadowStage) return StageResult::Completed;
        const int32 faceResolution = settings ? settings->PointShadowMapResolution : 1024;
        resources.Set(TEXT("Shadow.ReceiverDepth"), depth);
        // Keep the camera surface shared by all early light-selection passes.
        for (const auto& item : lights)
        {
            if (!item.Source->GetILD_CastShadows() || !(item.Source->GetILD_ShadowStrength() > 0)) continue;
            for (int32 face = 0; face < (point ? 6 : 1); face++)
            {
                if (point && (item.FaceScissors[face].Size.X <= 0 || item.FaceScissors[face].Size.Y <= 0)) continue;
                RenderView projected;
                projected.Origin = origin;
                projected.Position = item.Position;
                projected.Direction = point ? PointDirections[face] : item.Direction;
                projected.Near = item.NearPlane;
                projected.Far = item.Range;
                projected.SetUp(point ? item.FaceViews[face] : item.View, item.Projection);
                GPUTexture* unused = nullptr;
                const auto result = ShadowStage->RenderProjectedShadowMap(context, objects, viewpoint, resources,
                    projected, unused, point ? faceResolution : 0, true);
                if (result == StageResult::Failed) return result;
            }
        }
        return StageResult::Completed;
    }

    PunctualProfileScope lightsProfile(point ? TEXT("V2 Point Lights") : TEXT("V2 Spot Lights"));
    if (!LightingShader)
    {
        LightingShader = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/Utils/SpotLight"));
        if (LightingShader)
            LightingShader->Reloading.Bind<PunctualLightRenderer, &PunctualLightRenderer::OnShaderReloading>(this);
    }
    if (!LightingShader || !LightingShader->IsLoaded() || !LightingShader->GPU)
        return StageResult::Skipped;
    auto* constants = LightingShader->GPU->GetCB(0);
    auto* ps = LightingShader->GPU->GetPS(point ? "PS_V2PointLighting" : "PS_V2SpotLighting");
    if (!constants || constants->GetSize() != sizeof(PunctualConstants) || !ps)
        return StageResult::Failed;
    auto* device = context->GetDevice();
    if (!LightingState)
        LightingState = device->CreatePipelineState();
    if (!LightingState)
        return StageResult::Failed;
    if (!LightingState->IsValid())
    {
        auto desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        desc.PS = ps;
        desc.BlendMode = BlendingMode::Add;
        desc.BlendMode.RenderTargetWriteMask = BlendingMode::ColorWrite::RGB;
        if (LightingState->Init(desc))
            return StageResult::Failed;
    }

    const int32 resolution = Math::Clamp(point ? (settings ? settings->PointShadowMapResolution : 1024) : (settings ? settings->ShadowDepthResolution : 2048),
        512, Math::Min(16384, device->Limits.MaximumTexture2DSize));
    const bool pixel = settings && settings->ShadowRendering == ShadowRenderingMode::Pixel;
    Array<PunctualFilterBand, InlinedAllocation<8>> bands;
    float previousEnd = 0, previousFade = 0;
    if (settings)
    {
        for (int32 i = 0; i < settings->ShadowCascades.Count(); i++)
        {
            const auto& band = settings->ShadowCascades[i];
            const bool last = i == settings->ShadowCascades.Count() - 1;
            const float end = last ? MAX_float : band.EndDistance;
            if (!(end > previousEnd))
                continue;
            const float fade = last || pixel ? end : Math::Lerp(previousEnd, end, 1 - Math::Clamp(band.BlendFraction, 0.0f, 1.0f));
            const int32 referenceSize = band.TextureSize <= 0 ? resolution : Math::Clamp(band.TextureSize, 512, resolution);
            bands.Add({ Float4(previousFade, fade, end, 0), Float4(pixel ? 0 : Math::Clamp(band.FilterRadius, 0.0f, 32.0f) * resolution / referenceSize,
                Math::Clamp(band.DepthBias, 0.0f, 10.0f), Math::Clamp(band.NormalBias, 0.0f, 10.0f), 0) });
            previousEnd = end;
            previousFade = fade;
        }
    }
    if (!FilterBands)
        FilterBands = device->CreateBuffer(TEXT("V2 Punctual Shadow Filter Bands"));
    const int32 bandCount = Math::Max(1, bands.Count());
    if (!FilterBands || (FilterBands->GetElementsCount() < (uint32)bandCount &&
        FilterBands->Init(GPUBufferDescription::Structured(bandCount, sizeof(PunctualFilterBand)))))
        return StageResult::Failed;
    if (bands.HasItems())
        context->UpdateBuffer(FilterBands, bands.Get(), bands.Count() * sizeof(PunctualFilterBand));

    for (const auto& item : lights)
    {
        PunctualProfileScope lightProfile(point ? TEXT("V2 Point Light") : TEXT("V2 Spot Light"));
        auto* light = item.Source;
        const auto& position = item.Position;
        const auto& direction = item.Direction;
        const auto& scissor = item.Scissor;
        const float range = item.Range, outer = item.Outer, inner = item.Inner, nearPlane = item.NearPlane;
        const auto& lightVP = item.ViewProjection;
        const bool requestShadows = light->GetILD_CastShadows() && light->GetILD_ShadowStrength() > 0;
        GPUTexture* shadowDepth = nullptr;
        Matrix faceMatrices[6];
        if (point && requestShadows && ShadowStage)
        {
            PROFILE_GPU_CPU_NAMED("V2 Point Shared Shadow Selection");
            if (!PointShadowMaps)
                PointShadowMaps = device->CreateTexture(TEXT("V2 Point Shadow Faces"));
            if (!PointFaceMatrices)
                PointFaceMatrices = device->CreateBuffer(TEXT("V2 Point Face Matrices"));
            if (!PointShadowMaps || !PointFaceMatrices ||
                ((PointShadowMaps->Width() != resolution || !PointShadowMaps->GetMemoryUsage()) &&
                 PointShadowMaps->Init(GPUTextureDescription::New2D(resolution, resolution,
                     PixelFormat::D32_Float, GPUTextureFlags::DepthStencil | GPUTextureFlags::ShaderResource, 6))) ||
                (PointFaceMatrices->GetElementsCount() < 6 &&
                 PointFaceMatrices->Init(GPUBufferDescription::Structured(6, sizeof(Matrix)))))
                return StageResult::Failed;

            resources.Set(TEXT("Shadow.ReceiverDepth"), depth);
            resources.Set(TEXT("Shadow.ReceiverNormals"), normals);
            for (int32 face = 0; face < 6; face++)
            {
                PROFILE_GPU_CPU_NAMED("V2 Point Shadow Face");
                RenderView projected;
                projected.Origin = origin;
                projected.Position = position;
                projected.Direction = PointDirections[face];
                projected.Near = nearPlane;
                projected.Far = range;
                projected.SetUp(item.FaceViews[face], item.Projection);
                Matrix::Transpose(projected.ViewProjection(), faceMatrices[face]);
                const auto& faceRect = item.FaceScissors[face];
                context->ResetRenderTarget();
                context->ResetSR();
                context->ClearDepth(PointShadowMaps->View(face));
                if (faceRect.Size.X <= 0 || faceRect.Size.Y <= 0)
                    continue;
                GPUTexture* faceDepth = nullptr;
                const auto result = ShadowStage->RenderProjectedShadowMap(context, objects, viewpoint, resources,
                    projected, faceDepth, resolution);
                if (result == StageResult::Failed) return result;
                LastCasterCount += ShadowStage->GetCasterCount();
                if (faceDepth)
                {
                    context->ResetRenderTarget();
                    context->ResetSR();
                    context->CopySubresource(PointShadowMaps, (uint32)face, faceDepth, 0);
                }
            }
            context->UpdateBuffer(PointFaceMatrices, faceMatrices, sizeof(faceMatrices));
            shadowDepth = PointShadowMaps;
            resources.SetOutput(TEXT("PointLight.ShadowDepth"), PointShadowMaps);
            resources.SetOutput(TEXT("PointLight.FaceMatrices"), PointFaceMatrices);
        }
        else if (!point)
        {
            if (requestShadows && ShadowStage)
            {
                PROFILE_GPU_CPU_NAMED("V2 Spot Shared Shadow Selection");
                RenderView projected;
                projected.Origin = origin;
                projected.Position = position;
                projected.Direction = direction;
                projected.Near = nearPlane;
                projected.Far = range;
                projected.SetUp(item.View, item.Projection);
                resources.Set(TEXT("Shadow.ReceiverDepth"), depth);
                resources.Set(TEXT("Shadow.ReceiverNormals"), normals);
                const auto result = ShadowStage->RenderProjectedShadowMap(context, objects, viewpoint, resources, projected, shadowDepth);
                if (result == StageResult::Failed) return result;
                LastCasterCount += ShadowStage->GetCasterCount();
                if (shadowDepth) resources.SetOutput(TEXT("SpotLight.ShadowDepth"), shadowDepth);
            }
        }
        const bool castShadows = shadowDepth != nullptr;

        PunctualConstants data = {};
        Matrix inverse;
        Matrix::Invert(view->ViewProjection, inverse);
        Matrix::Transpose(inverse, data.InverseViewProjection);
        if (!point) Matrix::Transpose(lightVP, data.LightViewProjection);
        data.CameraPosition = Float4(Float3(view->LocalTransform.Translation), view->Projection.M34 == 0 ? 1.0f : 0.0f);
        data.CameraForward = Float4(Float3(Vector3::Transform(Vector3::Forward, view->LocalTransform.Orientation)), 0);
        data.LightPositionRange = Float4(position, range);
        data.LightDirectionOuter = Float4(direction, Math::Cos(outer * DegreesToRadians));
        const auto colourValue = light->GetILD_Color();
        data.LightRadiance = Float4(colourValue.R, colourValue.G, colourValue.B, light->GetILD_Intensity() * colourValue.A);
        data.ConeFalloff = Float4(Math::Cos(inner * DegreesToRadians), Math::Max(light->GetILD_FalloffExponent(), 0.0f),
            light->GetILD_InverseSquaredFalloff() ? 1.0f : 0.0f, Math::Clamp(light->GetILD_ShadowStrength(), 0.0f, 1.0f));
        data.ShadowInfo = Float4((float)(shadowDepth ? shadowDepth->Width() : resolution), nearPlane, range, 4.0f * 1.192092896e-7f);
        // Account for the same projection/rasterization roundoff as the shared
        // selector. ULP(coord.z) alone misses arithmetic error on a face whose
        // receiver plane has zero slope (notably ground under a point light).
        data.Options = Float4(castShadows ? 1.0f : 0.0f, settings && settings->ShowWhiteSurfaceLighting ? 1.0f : 0.0f,
            pixel ? 1.0f : 0.0f, (float)bands.Count());
        data.DefaultFilter = Float4(pixel ? 0.0f : 2.5f, 0, 0, Math::Tan(outer * DegreesToRadians));
        {
            PunctualProfileScope resolveProfile(point ? TEXT("V2 Point Lighting Resolve") : TEXT("V2 Spot Lighting Resolve"));
            context->ResetRenderTarget();
            context->ResetSR();
            context->UpdateCB(constants, &data);
            context->BindCB(0, constants);
            context->BindSR(0, FilterBands->View());
            context->BindSR(2, depth);
            context->BindSR(7, point ? nullptr : shadowDepth);
            if (point)
            {
                context->BindSR(12, castShadows ? PointFaceMatrices->View() : nullptr);
                context->BindSR(13, shadowDepth ? shadowDepth->ViewArray() : nullptr);
            }
            context->BindSR(8, normals);
            context->BindSR(9, base);
            context->BindSR(10, material);
            context->SetViewportAndScissors((float)colour->Width(), (float)colour->Height());
            context->SetScissor(scissor);
            context->SetRenderTarget(colour->View());
            context->SetState(LightingState);
            context->DrawFullscreenTriangle();
            context->ResetRenderTarget();
            context->ResetSR();
        }
        LastLightCount++;
    }
    return StageResult::Completed;
}
