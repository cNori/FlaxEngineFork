// Copyright (c) Wojciech Figat. All rights reserved.

#include "../VirtualShadowMapStage.h"
#include "../ILightData.h"
#include "../RenderPipelineGraphicSettings.h"
#include "../WorldWarpStage.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Content/Content.h"
#include "Engine/Core/Math/BoundingFrustum.h"
#include "Engine/Core/Math/Mathd.h"
#include "Engine/Core/Types/StringBuilder.h"
#include "Engine/Graphics/GPUBuffer.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/GPUPipelineState.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Graphics/Shaders/GPUConstantBuffer.h"
#include "Engine/Graphics/Shaders/GPUShader.h"
#include "Engine/Graphics/Textures/GPUTexture.h"
#include "Engine/Level/Actors/StaticModel.h"
#include "Engine/Profiler/Profiler.h"
#include "Engine/Render2D/Font.h"
#include "Engine/Render2D/FontAsset.h"
#include "Engine/Render2D/Render2D.h"
#include "Engine/Renderer/RenderList.h"

namespace
{
    constexpr int32 MaxGridResolution = 2048;
    constexpr int32 MinShadowPageResolution = 64;
    constexpr int32 MaxDirectionalSlices = 24;
    constexpr uint32 PointXZFaces = (1u << 0) | (1u << 1) | (1u << 4) | (1u << 5);
    const Char* const PointFaceNames[] = { TEXT("+X"), TEXT("-X"), TEXT("+Y"), TEXT("-Y"), TEXT("+Z"), TEXT("-Z") };

    GPU_CB_STRUCT(GridConstants
    {
        Matrix CameraInverseProjection;
        Matrix CameraInverseView;
        uint32 Width;
        uint32 Height;
        uint32 DebugMode;
        uint32 WorldShadowTableMask;
        Float4 WorldPageOriginAndSpan;
        uint32 WorldPageResolution;
        Float3 PagePadding;
        float CameraDepthClear;
        Float3 CameraForward;
    });

    GPU_CB_STRUCT(LightConstants
    {
        Matrix ViewProjections[MaxDirectionalSlices];
        Matrix InverseProjections[MaxDirectionalSlices];
        Matrix Views[MaxDirectionalSlices];
        Float4 PositionAndRange;
        Float4 DirectionAndOuterCos;
        Float4 ColorAndFalloff;
        Float4 Options; // Inner cosine, inverse-square falloff, shadow strength, directional slice count.
        Float4 DirectionalSlices[MaxDirectionalSlices]; // Nominal near/far depth and overlap start.
        Int4 ShadowPages[MaxDirectionalSlices]; // Atlas origin, resolution and array layer.
        uint32 Kind;
        uint32 DepthWidth;
        uint32 DepthHeight;
        uint32 FaceMask;
    });

    struct WorldShadowPages
    {
        GPUBuffer* Keys = nullptr;
        GPUBuffer* Texels = nullptr;
        GPUBuffer* Dispatch = nullptr;
        GPUBuffer* DispatchArgs = nullptr;
        GPUBuffer* Visibility = nullptr;
        GPUTexture* Screen = nullptr;

        ~WorldShadowPages()
        {
            SAFE_DELETE_GPU_RESOURCE(Keys);
            SAFE_DELETE_GPU_RESOURCE(Texels);
            SAFE_DELETE_GPU_RESOURCE(Dispatch);
            SAFE_DELETE_GPU_RESOURCE(DispatchArgs);
            SAFE_DELETE_GPU_RESOURCE(Visibility);
            SAFE_DELETE_GPU_RESOURCE(Screen);
        }

        bool Gather(GPUContext* context, GPUShader* shader, GPUConstantBuffer* gridCB, GPUTexture* depth,
                    GPUTexture* normals, GridConstants& grid)
        {
            auto* gather = shader->GetCS("CS_GatherWorldShadowTexels");
            auto* setDispatch = shader->GetCS("CS_SetWorldShadowDispatch");
            if (!gather || !setDispatch || !shader->GetCS("CS_ProjectWorldShadowPages"))
                return false;

            auto* device = context->GetDevice();
            if (!Keys)
                Keys = device->CreateBuffer(TEXT("World Shadow Page Keys"));
            if (!Texels)
                Texels = device->CreateBuffer(TEXT("World Shadow Page Receivers"));
            if (!Dispatch)
                Dispatch = device->CreateBuffer(TEXT("World Shadow Page Dispatch"));
            if (!DispatchArgs)
                DispatchArgs = device->CreateBuffer(TEXT("World Shadow Page Indirect Arguments"));
            if (!Visibility)
                Visibility = device->CreateBuffer(TEXT("World Shadow Page Visibility"));
            if (!Screen)
                Screen = device->CreateTexture(TEXT("World Shadow Page Lookup"));

            // Bound sparse storage. Receivers beyond the probe limit evaluate
            // the same virtual texel in resolve, so the memory cap cannot cut shadows.
            const uint32 capacity = Math::RoundUpToPowerOf2(Math::Clamp(grid.Width * grid.Height, 4096u, 1048576u));
            const auto indexDesc = GPUBufferDescription::Typed(capacity * 2, PixelFormat::R32_UInt, true);
            const auto texelDesc = GPUBufferDescription::Structured(capacity, sizeof(Float3) * 2 + sizeof(uint32), true);
            const auto argsDesc = GPUBufferDescription::Raw(sizeof(uint32) * 4,
                GPUBufferFlags::ShaderResource | GPUBufferFlags::UnorderedAccess);
            const auto visibilityDesc = GPUBufferDescription::Typed(capacity, PixelFormat::R32_Float, true);
            const auto screenDesc = GPUTextureDescription::New2D(grid.Width, grid.Height, PixelFormat::R32G32_UInt,
                GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
            if (!Keys || !Texels || !Dispatch || !DispatchArgs || !Visibility || !Screen ||
                (Keys->GetSize() < indexDesc.Size && Keys->Init(indexDesc)) ||
                (Texels->GetSize() < texelDesc.Size && Texels->Init(texelDesc)) ||
                (!Dispatch->GetSize() && Dispatch->Init(argsDesc)) ||
                (!DispatchArgs->GetSize() && DispatchArgs->Init(GPUBufferDescription::Raw(sizeof(GPUDispatchIndirectArgs),
                    GPUBufferFlags::UnorderedAccess | GPUBufferFlags::Argument))) ||
                (Visibility->GetSize() < visibilityDesc.Size && Visibility->Init(visibilityDesc)) ||
                ((!Screen->GetMemoryUsage() || Screen->Width() != grid.Width || Screen->Height() != grid.Height)
                    && Screen->Init(screenDesc)))
                return false;

            PROFILE_GPU_CPU_NAMED("Gather World Shadow Page Texels");
            grid.WorldShadowTableMask = capacity - 1;
            const uint32 zero[4] = {};
            context->ResetRenderTarget();
            context->ResetSR();
            context->ResetUA();
            context->ClearUA(Keys, zero);
            context->ClearUA(Dispatch, zero);
            context->UpdateCB(gridCB, &grid);
            context->BindCB(0, gridCB);
            context->BindSR(0, depth->View());
            context->BindSR(1, normals->View());
            context->BindUA(0, Keys->View());
            context->BindUA(1, Texels->View());
            context->BindUA(2, Dispatch->View());
            context->BindUA(3, Screen->View());
            context->Dispatch(gather, Math::DivideAndRoundUp((int32)grid.Width, 8), Math::DivideAndRoundUp((int32)grid.Height, 8));
            context->ResetUA();
            context->ResetSR();
            context->BindSR(10, Dispatch->View());
            context->BindUA(0, DispatchArgs->View());
            context->Dispatch(setDispatch, 1, 1);
            context->ResetUA();
            context->ResetSR();
            return true;
        }

        void Project(GPUContext* context, GPUShaderProgramCS* shader, GPUTexture* depth)
        {
            PROFILE_GPU_CPU_NAMED("Project Shadows to World Pages");
            // Dispatch arguments and SRVs must be distinct on D3D11.
            context->BindSR(5, Texels->View());
            context->BindSR(6, depth->ViewArray());
            context->BindSR(9, Keys->View());
            context->BindSR(10, Dispatch->View());
            context->BindUA(0, Visibility->View());
            context->DispatchIndirect(shader, DispatchArgs, 0);
            context->ResetUA();
            context->ResetSR();
        }

        void Bind(GPUContext* context) const
        {
            context->BindSR(4, Screen->View());
            context->BindSR(5, Texels->View());
            context->BindSR(7, Visibility->View());
        }
    };

    struct ShadowCaster
    {
        WarpedVisualData* Object;
        StaticModel* Actor;
        const Mesh* Geometry;
        MaterialBase* Material;
        BoundingBox Bounds;
        bool HasDeformation;
    };

    struct ShadowLight
    {
        LightConstants Constants = {};
        Matrix Views[MaxDirectionalSlices];
        Matrix Projections[MaxDirectionalSlices];
        Float2 ReceiverDepthMapping[MaxDirectionalSlices]; // Original M33 and M43 used by the mask.
        Float3 Directions[MaxDirectionalSlices];
        int32 FaceCount = 0;
        uint32 FaceMask = 0;
        uint32 OutsideCameraFaces = 0;
        int32 ReceiverMaskOffset = 0;
        Int4 ReceiverTileRect[MaxDirectionalSlices]; // Square crop in the original receiver mask.
        Int4 ReceiverScissors[MaxDirectionalSlices] = {}; // Occupied subrect in the cropped viewport.
        Float2 ReceiverFarPlanes[MaxDirectionalSlices] = {}; // Original and fitted light-view far plane.
    };


    constexpr int32 CameraReceiverTileSize = 16;
    constexpr int32 ShadowReceiverTileSize = 16;
    constexpr int32 ShadowReceiverTileCount = ShadowReceiverTileSize * ShadowReceiverTileSize;
    constexpr int32 BlockerDemandMaxLog2 = 20;
    constexpr int32 BlockerDemandBins = BlockerDemandMaxLog2 + 1;
    // Depth tiles, fallback demand, a histogram of measured demand, and light-view far depth.
    constexpr int32 ShadowReceiverFarOffset = ShadowReceiverTileCount + 1 + BlockerDemandBins;
    constexpr int32 ShadowReceiverFaceStride = ShadowReceiverFarOffset + 1;

    GPU_CB_STRUCT(ShadowReceiverMaskConstants
    {
        uint32 Offset;
        uint32 FaceMask;
        uint32 Padding0;
        uint32 Padding;
    });

    bool ReachesReceiver(const BoundingBox& bounds, const Matrix& viewProjection, const Int4& tileRect,
                         int32 resolution, const uint32* mask)
    {
        Vector3 corners[8];
        bounds.GetCorners(corners);
        Float2 minimum = Float2::One;
        Float2 maximum = Float2::Zero;
        float closest = 1.0f;
        for (const auto& corner : corners)
        {
            const Float4 clip = Float4::Transform(Float4(Float3(corner), 1), viewProjection);
            if (clip.W <= 0.0001f || clip.IsNanOrInfinity())
                return true;

            const float inverseW = 1.0f / clip.W;
            const Float2 uv(0.5f + 0.5f * clip.X * inverseW, 0.5f - 0.5f * clip.Y * inverseW);
            minimum = Float2::Min(minimum, uv);
            maximum = Float2::Max(maximum, uv);
#if REVERSE_Z
            closest = Math::Min(closest, 1.0f - clip.Z * inverseW);
#else
            closest = Math::Min(closest, clip.Z * inverseW);
#endif
        }

        // Include blocker texels that straddle a tile boundary and allow for
        // projection roundoff in the two depth spaces.
        const Float2 guard(2.0f / resolution);
        const Float2 tileStart((float)tileRect.X, (float)tileRect.Y);
        const Float2 tileSize((float)tileRect.Z, (float)tileRect.W);
        const Float2 first = tileStart + Float2::Clamp(minimum - guard, Float2::Zero, Float2::One) * tileSize;
        const Float2 last = tileStart + Float2::Clamp(maximum + guard, Float2::Zero, Float2::One) * tileSize;
        const float depthThreshold = 1.0f + Math::Saturate(closest - 0.0001f);
        uint32 encodedThreshold;
        Platform::MemoryCopy(&encodedThreshold, &depthThreshold, sizeof(encodedThreshold));
        for (int32 y = Math::Min((int32)first.Y, ShadowReceiverTileSize - 1); y <= Math::Min((int32)last.Y, ShadowReceiverTileSize - 1); y++)
        {
            for (int32 x = Math::Min((int32)first.X, ShadowReceiverTileSize - 1); x <= Math::Min((int32)last.X, ShadowReceiverTileSize - 1); x++)
            {
                if (mask[y * ShadowReceiverTileSize + x] >= encodedThreshold)
                    return true;
            }
        }
        return false;
    }

    struct ShadowReceiverMask
    {
        GPUBuffer* CameraTiles = nullptr;
        GPUBuffer* Depth = nullptr;
        GPUBuffer* Readback = nullptr;

        ~ShadowReceiverMask()
        {
            SAFE_DELETE_GPU_RESOURCE(CameraTiles);
            SAFE_DELETE_GPU_RESOURCE(Depth);
            SAFE_DELETE_GPU_RESOURCE(Readback);
        }

        const uint32* Build(GPUContext* context, GPUShader* shader, GPUConstantBuffer* gridCB,
                            GPUTexture* depth, GPUTexture* normals, const GridConstants& grid, Array<ShadowLight>& lights)
        {
            auto* build = shader->GetCS("CS_BuildCameraReceiverTiles");
            auto* mark = shader->GetCS("CS_MarkShadowReceiverDepth");
            auto* lightCB = shader->GetCB(1);
            auto* maskCB = shader->GetCB(2);
            if (!build || !mark || !lightCB || lightCB->GetSize() != sizeof(LightConstants) ||
                !maskCB || maskCB->GetSize() != sizeof(ShadowReceiverMaskConstants))
                return nullptr;

            int32 maskCount = 0;
            for (auto& light : lights)
            {
                if (!light.FaceMask)
                    continue;
                light.ReceiverMaskOffset = maskCount;
                maskCount += light.FaceCount * ShadowReceiverFaceStride;
            }
            if (maskCount == 0)
                return nullptr;

            const int32 width = Math::DivideAndRoundUp((int32)grid.Width, CameraReceiverTileSize);
            const int32 height = Math::DivideAndRoundUp((int32)grid.Height, CameraReceiverTileSize);
            auto* device = context->GetDevice();
            if (!CameraTiles)
                CameraTiles = device->CreateBuffer(TEXT("Shadow Camera Receiver Tiles"));
            if (!Depth)
                Depth = device->CreateBuffer(TEXT("Shadow Receiver Depth Mask"));
            if (!Readback)
                Readback = device->CreateBuffer(TEXT("Shadow Receiver Depth Mask Readback"));
            const auto tileDesc = GPUBufferDescription::Structured(width * height, sizeof(Float4) * 4, true);
            const auto maskDesc = GPUBufferDescription::Typed(maskCount, PixelFormat::R32_UInt, true);
            if (!CameraTiles || !Depth || !Readback ||
                (CameraTiles->GetSize() < tileDesc.Size && CameraTiles->Init(tileDesc)) ||
                (Depth->GetSize() < maskDesc.Size && Depth->Init(maskDesc)) ||
                (Readback->GetSize() < maskDesc.Size && Readback->Init(maskDesc.ToStagingReadback())))
                return nullptr;

            {
                PROFILE_GPU_CPU_NAMED("Build Shadow Receiver Depth Mask");
                context->ResetRenderTarget();
                context->ResetSR();
                context->ResetUA();
                context->UpdateCB(gridCB, &grid);
                context->BindCB(0, gridCB);
                context->BindSR(0, depth->View());
                context->BindSR(1, normals->View());
                context->BindUA(0, CameraTiles->View());
                context->Dispatch(build, width, height);
                context->ResetUA();
                context->ResetSR();

                const uint32 zero[4] = {};
                context->ClearUA(Depth, zero);
                context->BindCB(1, lightCB);
                context->BindCB(2, maskCB);
                context->BindSR(8, CameraTiles->View());
                context->BindUA(0, Depth->View());
                for (const auto& light : lights)
                {
                    if (!light.FaceMask)
                        continue;
                    const ShadowReceiverMaskConstants constants = {
                        (uint32)light.ReceiverMaskOffset, light.FaceMask, 0, 0
                    };
                    context->UpdateCB(lightCB, &light.Constants);
                    context->UpdateCB(maskCB, &constants);
                    context->Dispatch(mark, Math::DivideAndRoundUp(width * height, 64), 1, light.FaceCount);
                }
                context->ResetUA();
                context->ResetSR();
                context->CopyBuffer(Readback, Depth, maskDesc.Size);
            }
            PROFILE_CPU_NAMED("Read Shadow Receiver Depth Mask");
            return static_cast<const uint32*>(Readback->Map(GPUResourceMapMode::Read));
        }
    };

    Matrix InvertShadowProjection(const Matrix& projection)
    {
        if (projection.M34 != 0.0f || projection.M44 != 1.0f ||
            projection.M11 == 0.0f || projection.M22 == 0.0f || projection.M33 == 0.0f ||
            projection.M12 != 0.0f || projection.M13 != 0.0f || projection.M14 != 0.0f ||
            projection.M21 != 0.0f || projection.M23 != 0.0f || projection.M24 != 0.0f ||
            projection.M31 != 0.0f || projection.M32 != 0.0f)
            return Matrix::Invert(projection);

        // The determinant of a wide orthographic projection can fall below
        // the general inverse's singularity threshold even with finite axes.
        Matrix inverse = Matrix::Scaling(1.0f / projection.M11, 1.0f / projection.M22, 1.0f / projection.M33);
        inverse.M41 = -projection.M41 * inverse.M11;
        inverse.M42 = -projection.M42 * inverse.M22;
        inverse.M43 = -projection.M43 * inverse.M33;
        return inverse;
    }

    void SetProjection(ShadowLight& light, int32 face, const Matrix& view, const Matrix& projection, const Float3& direction)
    {
        light.Views[face] = view;
        light.Projections[face] = projection;
        light.Directions[face] = direction;
        light.Constants.ShadowPages[face].Z = light.Constants.DepthWidth;
        const Matrix viewProjection = view * projection;
        Matrix::Transpose(viewProjection, light.Constants.ViewProjections[face]);
        Matrix::Transpose(InvertShadowProjection(projection), light.Constants.InverseProjections[face]);
        Matrix::Transpose(view, light.Constants.Views[face]);
    }

    uint32 GetBlockerDemand(const uint32* mask)
    {
        const auto* bins = mask + ShadowReceiverTileCount + 1;
        uint32 samples = 0;
        int32 peak = 0;
        for (int32 i = 0; i < BlockerDemandBins; ++i)
        {
            samples += bins[i];
            if (bins[i])
                peak = i;
        }
        // Sparse or unmeasured coverage keeps the conservative demand.
        if (!samples)
            return mask[ShadowReceiverTileCount];
        if (samples < 16)
            return 1u << peak;

        // Keep the 95th percentile at full density and reserve one more
        // power-of-two tier for receivers whose demand is above it.
        const uint32 rank = samples - samples / 20;
        uint32 cumulative = 0;
        int32 tier = 0;
        for (; tier < peak; ++tier)
        {
            cumulative += bins[tier];
            if (cumulative >= rank)
                break;
        }
        return Math::Min(1u << peak, 2u << tier);
    }

    void FitReceiverFarPlane(Matrix& projection, uint32 encodedFar, Float2& fittedPlanes)
    {
        if (!encodedFar || encodedFar >= 0x7f800000u)
            return;

        // The near plane belongs to the blocker volume. Only the far plane
        // is bounded by visible receivers, so offscreen blockers survive.
        const Matrix inverse = InvertShadowProjection(projection);
        const Float4 nearClip = Float4::Transform(Float4(0, 0, GPU_DEPTH_RANGE_MIN, 1), inverse);
        const Float4 farClip = Float4::Transform(Float4(0, 0, GPU_DEPTH_RANGE_MAX, 1), inverse);
        if (nearClip.IsNanOrInfinity() || farClip.IsNanOrInfinity() || nearClip.W <= 0 || farClip.W <= 0)
            return;
        const float nearPlane = nearClip.Z / nearClip.W;
        const float originalFar = farClip.Z / farClip.W;
        if (!(originalFar > nearPlane))
            return;
        float receiverFar;
        Platform::MemoryCopy(&receiverFar, &encodedFar, sizeof(receiverFar));
        const float guard = Math::Max(1.0f, Math::Abs(receiverFar) * 1e-5f);
        const float farPlane = Math::Max(receiverFar, nearPlane) + guard;
        if (originalFar - farPlane <= guard)
            return;

        Matrix depthMapping;
        if (projection.M34 != 0.0f)
            Matrix::PerspectiveOffCenter(-nearPlane, nearPlane, -nearPlane, nearPlane, nearPlane, farPlane, depthMapping);
        else
            Matrix::OrthoOffCenter(-1, 1, -1, 1, nearPlane, farPlane, depthMapping);
        projection.M33 = depthMapping.M33;
        projection.M43 = depthMapping.M43;
        fittedPlanes = Float2(originalFar, farPlane);
    }

    bool FitBlockerPage(ShadowLight& light, int32 face, const uint32* mask, bool cullShadows)
    {
        int32 minX = ShadowReceiverTileSize;
        int32 minY = ShadowReceiverTileSize;
        int32 maxX = -1;
        int32 maxY = -1;
        for (int32 y = 0; y < ShadowReceiverTileSize; ++y)
        {
            for (int32 x = 0; x < ShadowReceiverTileSize; ++x)
            {
                if (!mask[y * ShadowReceiverTileSize + x])
                    continue;
                minX = Math::Min(minX, x);
                minY = Math::Min(minY, y);
                maxX = Math::Max(maxX, x);
                maxY = Math::Max(maxY, y);
            }
        }
        if (maxX < 0)
            return false;

        // Use a square crop so the depth draw cannot stretch the source texels.
        const int32 width = maxX + 1 - minX;
        const int32 height = maxY + 1 - minY;
        const int32 side = Math::Max(width, height);
        const Int2 occupiedStart(minX, minY);
        const Int2 occupiedEnd(maxX + 1, maxY + 1);
        minX = Math::Clamp(minX - (side - width) / 2, 0, ShadowReceiverTileSize - side);
        minY = Math::Clamp(minY - (side - height) / 2, 0, ShadowReceiverTileSize - side);
        maxX = minX + side - 1;
        maxY = minY + side - 1;
        light.ReceiverTileRect[face] = Int4(minX, minY, side, side);
        Matrix crop = Matrix::Identity;
        crop.M11 = crop.M22 = (float)ShadowReceiverTileSize / side;
        crop.M41 = (float)(ShadowReceiverTileSize - minX - maxX - 1) / side;
        crop.M42 = (float)(minY + maxY + 1 - ShadowReceiverTileSize) / side;
        Matrix projection = light.Projections[face] * crop;
        light.ReceiverDepthMapping[face] = Float2(projection.M33, projection.M43);
        if (cullShadows)
            FitReceiverFarPlane(projection, mask[ShadowReceiverFarOffset], light.ReceiverFarPlanes[face]);
        SetProjection(light, face, light.Views[face], projection, light.Directions[face]);

        // The GPU demand is for the full frustum. Allocate only the part
        // containing visible world-page texels, including the mask guard.
        const uint32 demand = GetBlockerDemand(mask);
        const uint32 size = Math::DivideAndRoundUp(demand * (uint32)side, (uint32)ShadowReceiverTileSize);
        const int32 resolution = (int32)Math::RoundUpToPowerOf2(
            Math::Clamp(size, (uint32)MinShadowPageResolution, light.Constants.DepthWidth));
        light.Constants.ShadowPages[face].Z = resolution;

        // The projection remains square. Scissor to the actual receiver
        // rectangle and include a two-texel guard for the depth lookup.
        const int32 firstX = Math::Max(0, (occupiedStart.X - minX) * resolution / side - 2);
        const int32 firstY = Math::Max(0, (occupiedStart.Y - minY) * resolution / side - 2);
        const int32 lastX = Math::Min(resolution, Math::DivideAndRoundUp((occupiedEnd.X - minX) * resolution, side) + 2);
        const int32 lastY = Math::Min(resolution, Math::DivideAndRoundUp((occupiedEnd.Y - minY) * resolution, side) + 2);
        light.ReceiverScissors[face] = Int4(firstX, firstY, lastX - firstX, lastY - firstY);
        return true;
    }

    // The vertices of a frustum/box intersection lie on edges of one of the two volumes.
    bool ClipEdge(Vector3& start, Vector3& end, const Plane (&planes)[6])
    {
        const Vector3 delta = end - start;
        Real first = 0;
        Real last = 1;
        for (const auto& plane : planes)
        {
            const Real a = Vector3::Dot(plane.Normal, start) + plane.D;
            const Real b = Vector3::Dot(plane.Normal, end) + plane.D;
            if (a < 0 && b < 0)
                return false;
            if (a < 0)
                first = Math::Max(first, a / (a - b));
            if (b < 0)
                last = Math::Min(last, a / (a - b));
            if (first > last)
                return false;
        }
        end = start + delta * last;
        start += delta * first;
        return true;
    }

    using ReceiverCorners = Array<Vector3, FixedAllocation<48>>;

    void AddClippedEdges(const Vector3 (&corners)[8], const Plane (&planes)[6], ReceiverCorners& receivers)
    {
        for (int32 i = 0; i < 4; i++)
        {
            const int32 next = (i + 1) % 4;
            const int32 edges[3][2] = { { i, next }, { i + 4, next + 4 }, { i, i + 4 } };
            for (const auto& edge : edges)
            {
                Vector3 start = corners[edge[0]];
                Vector3 end = corners[edge[1]];
                if (ClipEdge(start, end, planes))
                {
                    receivers.Add(start);
                    receivers.Add(end);
                }
            }
        }
    }

    BoundingBox GetCameraReceiverBounds(const Array<IVisualData*>& objects, const BoundingFrustum& frustum)
    {
        BoundingBox bounds = BoundingBox::Empty;
        for (auto* object : objects)
        {
            const auto* warped = dynamic_cast<const WarpedVisualData*>(object);
            if (warped && !warped->Bounds.Minimum.IsNanOrInfinity() && !warped->Bounds.Maximum.IsNanOrInfinity() &&
                frustum.Intersects(warped->Bounds))
                bounds.Merge(warped->Bounds);
        }
        return bounds;
    }

    void GetCameraReceiverHull(const BoundingBox& bounds, const BoundingFrustum& frustum, ReceiverCorners& receivers)
    {
        if (bounds.Minimum.X > bounds.Maximum.X)
            return;

        const Plane boxPlanes[6] = {
            Plane(Vector3::Right, -bounds.Minimum.X), Plane(Vector3::Left, bounds.Maximum.X),
            Plane(Vector3::Up, -bounds.Minimum.Y), Plane(Vector3::Down, bounds.Maximum.Y),
            Plane(Vector3::Forward, -bounds.Minimum.Z), Plane(Vector3::Backward, bounds.Maximum.Z)
        };
        Plane frustumPlanes[6];
        for (int32 i = 0; i < 6; i++)
            frustumPlanes[i] = frustum.GetPlane(i);

        Vector3 corners[8];
        frustum.GetCorners(corners);
        AddClippedEdges(corners, boxPlanes, receivers);
        bounds.GetCorners(corners);
        AddClippedEdges(corners, frustumPlanes, receivers);
    }

    void SetDirectionalPage(ShadowLight& light, int32 page, Matrix view, const Float3& direction,
                            const Vector3& origin, const Vector3& center, const BoundingBox& bounds,
                            Real texelSize, const Array<ShadowCaster>& casters)
    {
        const Vector3 right(view.M11, view.M21, view.M31);
        const Vector3 vertical(view.M12, view.M22, view.M32);
        const Vector3 forward(view.M13, view.M23, view.M33);
        // Snap in absolute light space before narrowing to the render origin.
        const double originX = Double3::Dot(Double3(origin), Double3(right));
        const double originY = Double3::Dot(Double3(origin), Double3(vertical));
        // Align all resolution tiers to the same lattice when the page moves.
        const double snap = (double)texelSize * (light.Constants.DepthWidth / MinShadowPageResolution);
        const Real centerX = (Real)(Math::Round((center.X + originX) / snap) * snap - originX);
        const Real centerY = (Real)(Math::Round((center.Y + originY) / snap) * snap - originY);
        view.M41 = (float)-centerX;
        view.M42 = (float)-centerY;
        view.M43 = (float)-center.Z;
        const float extentX = (float)(texelSize * light.Constants.DepthWidth * 0.5);
        const float extentY = (float)(texelSize * light.Constants.DepthHeight * 0.5);

        // Off-camera geometry can still block light traveling into the canvas.
        float nearPlane = (float)(bounds.Minimum.Z - center.Z);
        const Vector3 canvasCenter = right * centerX + vertical * centerY + forward * center.Z;
        const Vector3 canvasRadius = Vector3::Abs(right) * extentX + Vector3::Abs(vertical) * extentY;
        for (const auto& caster : casters)
        {
            const auto& casterBounds = caster.Bounds;
            const Vector3 casterCenter = casterBounds.GetCenter();
            const Vector3 halfSize = casterBounds.GetSize() * (Real)0.5;
            const Real x = Vector3::Dot(casterCenter, right) - centerX;
            const Real y = Vector3::Dot(casterCenter, vertical) - centerY;
            if (Math::Abs(x) > extentX + Vector3::Dot(halfSize, Vector3::Abs(right)) ||
                Math::Abs(y) > extentY + Vector3::Dot(halfSize, Vector3::Abs(vertical)))
                continue;

            Real minDepth = Vector3::Dot(casterCenter, forward) - center.Z - Vector3::Dot(halfSize, Vector3::Abs(forward));
            // Bound the earliest possible intersection with the shadow canvas.
            // Each world axis excludes any depth where the entire canvas lies
            // outside the caster's bounds, even for very large floors and walls.
            for (int32 axis = 0; axis < 3; axis++)
            {
                const Real alongLight = forward.Raw[axis];
                if (Math::Abs(alongLight) <= (Real)0.001)
                    continue;
                const Real edge = alongLight > 0 ? casterBounds.Minimum.Raw[axis] : casterBounds.Maximum.Raw[axis];
                const Real offset = alongLight > 0 ? edge - canvasCenter.Raw[axis] : canvasCenter.Raw[axis] - edge;
                const Real firstDepth = (offset - canvasRadius.Raw[axis]) / Math::Abs(alongLight);
                minDepth = Math::Max(minDepth, firstDepth);
            }
            nearPlane = Math::Min(nearPlane, (float)minDepth);
        }
        Matrix projection;
        Matrix::OrthoOffCenter(-extentX, extentX, -extentY, extentY, nearPlane - 1.0f,
                               (float)(bounds.Maximum.Z - center.Z) + 1.0f, projection);
        SetProjection(light, page, view, projection, direction);
    }

    void SetDirectionalProjections(ShadowLight& light, const WarpedViewpoint& camera, const BoundingBox& receiverBounds,
                                   Real cellSize, const Array<ShadowCaster>& casters)
    {
        const Matrix inverse = InvertShadowProjection(camera.Projection);
        const Float4 nearCorner = Float4::Transform(Float4(1, 1, GPU_DEPTH_RANGE_MIN, 1), inverse);
        const Float4 farCorner = Float4::Transform(Float4(1, 1, GPU_DEPTH_RANGE_MAX, 1), inverse);
        if (nearCorner.IsNanOrInfinity() || farCorner.IsNanOrInfinity() || nearCorner.W <= 0 || farCorner.W <= 0 ||
            receiverBounds.Minimum.X > receiverBounds.Maximum.X)
            return;
        const float nearDistance = nearCorner.Z / nearCorner.W;
        const float farDistance = farCorner.Z / farCorner.W;
        if (farDistance <= nearDistance)
            return;
        const bool perspective = camera.Projection.M34 != 0;
        const float orthoDiameter = perspective ? 0.0f :
            2.0f * Float2(nearCorner.X / nearCorner.W, nearCorner.Y / nearCorner.W).Length();
        const float perspectiveScale = perspective ?
            Float3(2 * farCorner.X / farCorner.Z, 2 * farCorner.Y / farCorner.Z, 1).Length() : 1.0f;

        const Float3 direction(light.Constants.DirectionAndOuterCos);
        const Float3 up = Math::Abs(direction.Y) > 0.98f ? Float3::Forward : Float3::Up;
        Matrix view;
        Matrix::LookAt(Float3::Zero, direction, up, view);
        const Vector3 right(view.M11, view.M21, view.M31);
        const Vector3 vertical(view.M12, view.M22, view.M32);
        const Vector3 forward(view.M13, view.M23, view.M33);
        float sliceNear = nearDistance;
        float overlapNear = nearDistance;
        int32 slice = 0;
        // Use one level per fourfold increase in world-space texel spacing. A sphere around
        // the camera slice bounds its diameter in any light orientation.
        // Leave room to snap the center to the coarsest page's texel lattice.
        const float usableTexels = (float)(light.Constants.DepthWidth - light.Constants.DepthWidth / MinShadowPageResolution - 4);
        for (Real pitch = cellSize; slice < MaxDirectionalSlices && sliceNear < farDistance; pitch *= 4)
        {
            const Real coverage = pitch * usableTexels;
            if (coverage <= orthoDiameter)
                continue;
            const Real maximumDepth = perspective ? coverage / perspectiveScale :
                overlapNear + Math::Sqrt((coverage - orthoDiameter) * (coverage + orthoDiameter));
            const float sliceFar = (float)Math::Min((Real)farDistance, maximumDepth);
            if (sliceFar <= sliceNear || (slice == MaxDirectionalSlices - 1 && sliceFar < farDistance))
                continue;
            const float blendStart = sliceFar < farDistance ? Math::Lerp(sliceFar, sliceNear, 0.08f) : sliceFar;
            light.Constants.DirectionalSlices[slice] = Float4(overlapNear, sliceFar, blendStart, 0);
            light.FaceCount = slice + 1;

            // Replace only the depth mapping to preserve the camera's XY projection.
            Matrix depthMapping;
            if (perspective)
                Matrix::PerspectiveOffCenter(-overlapNear, overlapNear, -overlapNear, overlapNear, overlapNear, sliceFar, depthMapping);
            else
                Matrix::OrthoOffCenter(-1, 1, -1, 1, overlapNear, sliceFar, depthMapping);
            Matrix sliceProjection = camera.Projection;
            sliceProjection.M33 = depthMapping.M33;
            sliceProjection.M43 = depthMapping.M43;
            const BoundingFrustum frustum(camera.View * sliceProjection);
            ReceiverCorners corners;
            GetCameraReceiverHull(receiverBounds, frustum, corners);
            if (!corners.IsEmpty())
            {
                BoundingBox bounds = BoundingBox::Empty;
                for (const auto& point : corners)
                    bounds.Merge(Vector3(Vector3::Dot(point, right), Vector3::Dot(point, vertical), Vector3::Dot(point, forward)));

                light.FaceMask |= 1u << slice;
                SetDirectionalPage(light, slice, view, direction, camera.Origin, bounds.GetCenter(), bounds, pitch, casters);
            }

            sliceNear = sliceFar;
            overlapNear = blendStart;
            slice++;
        }
        light.Constants.Options.W = (float)slice;
    }

    // Test both sets of frustum planes against the opposite corners. The
    // remaining separating axes are cross products of frustum edges.
    bool SeparatedOnAxis(const Vector3 (&a)[8], const Vector3 (&b)[8], const Vector3& axis, Real guard)
    {
        const Real lengthSquared = axis.LengthSquared();
        if (axis.IsNanOrInfinity() || lengthSquared < (Real)1e-10)
            return false;
        Real minA = Vector3::Dot(a[0], axis);
        Real maxA = minA;
        Real minB = Vector3::Dot(b[0], axis);
        Real maxB = minB;
        for (int32 i = 1; i < 8; ++i)
        {
            const Real p = Vector3::Dot(a[i], axis);
            const Real q = Vector3::Dot(b[i], axis);
            minA = Math::Min(minA, p);
            maxA = Math::Max(maxA, p);
            minB = Math::Min(minB, q);
            maxB = Math::Max(maxB, q);
        }
        const Real magnitude = Math::Max(Math::Max(Math::Abs(minA), Math::Abs(maxA)),
            Math::Max(Math::Abs(minB), Math::Abs(maxB)));
        const Real margin = guard * Math::Sqrt(lengthSquared) + magnitude * (Real)1e-6;
        return maxA + margin < minB || maxB + margin < minA;
    }

    bool DisjointFrustums(const BoundingFrustum& camera, const Vector3 (&cameraCorners)[8],
                          const BoundingFrustum& face, Real guard)
    {
        Vector3 faceCorners[8];
        face.GetCorners(faceCorners);
        for (int32 i = 0; i < 8; ++i)
            if (cameraCorners[i].IsNanOrInfinity() || faceCorners[i].IsNanOrInfinity())
                return false;
        if ((cameraCorners[0] - cameraCorners[4]).LengthSquared() == 0 ||
            (faceCorners[0] - faceCorners[4]).LengthSquared() == 0)
            return false;

        for (int32 plane = 0; plane < 6; ++plane)
            if (SeparatedOnAxis(cameraCorners, faceCorners, camera.GetPlane(plane).Normal, guard) ||
                SeparatedOnAxis(cameraCorners, faceCorners, face.GetPlane(plane).Normal, guard))
                return true;

        // Two directions along the clip planes and four connecting edges.
        const auto getEdges = [](const Vector3 (&corners)[8], Vector3 (&edges)[6])
        {
            edges[0] = (corners[1] - corners[0]).GetNormalized();
            edges[1] = (corners[3] - corners[0]).GetNormalized();
            for (int32 i = 0; i < 4; ++i)
                edges[i + 2] = (corners[i + 4] - corners[i]).GetNormalized();
        };
        Vector3 cameraEdges[6], faceEdges[6];
        getEdges(cameraCorners, cameraEdges);
        getEdges(faceCorners, faceEdges);
        for (const auto& a : cameraEdges)
            for (const auto& b : faceEdges)
                if (SeparatedOnAxis(cameraCorners, faceCorners, Vector3::Cross(a, b), guard))
                    return true;
        return false;
    }

    void SetLocalLightProjections(ShadowLight& light, const BoundingBox& receivers, const Array<ShadowCaster>& casters,
                                  const BoundingFrustum& cameraFrustum, const Vector3 (&cameraCorners)[8], Real pageSpan, bool cullPointFaces)
    {
        const auto kind = (RenderLightKind)light.Constants.Kind;
        const Float3 direction(light.Constants.DirectionAndOuterCos);
        const Float3 up = Math::Abs(direction.Y) > 0.98f ? Float3::Forward : Float3::Up;

        const Float3 directions[6] = {
            Float3::Right, Float3::Left, Float3::Up, Float3::Down, Float3::Forward, Float3::Backward
        };
        const Float3 upVectors[6] = {
            Float3::Up, Float3::Up, Float3::Backward, Float3::Forward, Float3::Up, Float3::Up
        };
        const Float3 position(light.Constants.PositionAndRange);
        const float range = light.Constants.PositionAndRange.W;
        const bool point = kind == RenderLightKind::Point;
        const float tangent = point ? 1.0f : Math::Tan(Math::Acos(light.Constants.DirectionAndOuterCos.W));
        const float minNear = Math::Min(1.0f, range * 0.01f);
        light.FaceCount = point ? 6 : 1;
        light.FaceMask = (1u << light.FaceCount) - 1u;
        for (int32 face = 0; face < light.FaceCount; face++)
        {
            const Float3 forward = point ? directions[face] : direction;
            Matrix view;
            Matrix::LookAt(Float3::Zero, forward, point ? upVectors[face] : up, view);
            const Float3 right(view.M11, view.M21, view.M31);
            const Float3 vertical(view.M12, view.M22, view.M32);
            view.M41 = -Float3::Dot(position, right);
            view.M42 = -Float3::Dot(position, vertical);
            view.M43 = -Float3::Dot(position, forward);

            if (point && cullPointFaces && (PointXZFaces & (1u << face)))
            {
                // Use the entire cubemap face for rejection. A fitted or cropped
                // projection can exclude a receiver close to a cube seam.
                Matrix faceProjection;
                Matrix::PerspectiveOffCenter(-minNear, minNear, -minNear, minNear, minNear, range, faceProjection);
                const BoundingFrustum faceFrustum(view * faceProjection);
                // World-page lookup can move a receiver within its texel,
                // including on sloped surfaces or past the camera near plane.
                if (DisjointFrustums(cameraFrustum, cameraCorners, faceFrustum, 2 * pageSpan))
                {
                    light.FaceMask &= ~(1u << face);
                    light.OutsideCameraFaces |= 1u << face;
                    continue;
                }
            }

            float nearPlane = minNear;
            float farPlane = range;
            Float2 low(-tangent), high(tangent);
            if (receivers.Minimum.X <= receivers.Maximum.X)
            {
                Vector3 corners[8];
                receivers.GetCorners(corners);
                float receiverNear = MAX_float;
                float receiverFar = -MAX_float;
                Float2 first(MAX_float), last(-MAX_float);
                for (const auto& corner : corners)
                {
                    const Float3 relative = Float3(corner) - position;
                    const float distance = Float3::Dot(relative, forward);
                    receiverNear = Math::Min(receiverNear, distance);
                    receiverFar = Math::Max(receiverFar, distance);
                    const Float2 slope = Float2(Float3::Dot(relative, right), Float3::Dot(relative, vertical)) / Math::Max(distance, minNear);
                    first = Float2::Min(first, slope);
                    last = Float2::Max(last, slope);
                }
                if (receiverNear > minNear && receiverNear < range)
                {
                    // Fit the light projection to the receiving meshes. Include
                    // the nearby world-grid samples and the texels they can read.
                    const float padding = 4.0f * tangent / light.Constants.DepthWidth;
                    const Float2 cropLow = Float2::Max(first - Float2(padding), low);
                    const Float2 cropHigh = Float2::Min(last + Float2(padding), high);
                    if (cropLow.X < cropHigh.X && cropLow.Y < cropHigh.Y)
                    {
                        low = cropLow;
                        high = cropHigh;
                    }

                    // Occluders can be outside the camera, between the light
                    // and the receiver. Keep their nearest possible depth.
                    float firstBlocker = receiverNear;
                    for (const auto& caster : casters)
                    {
                        const auto& bounds = caster.Bounds;
                        const Float3 center(bounds.GetCenter());
                        const Float3 extent = Float3(bounds.GetSize()) * 0.5f;
                        const float firstDepth = Float3::Dot(center - position, forward) - Float3::Dot(extent, Float3::Abs(forward));
                        firstBlocker = Math::Min(firstBlocker, firstDepth);
                    }
                    // Guard clip planes by a world-space margin and keep some
                    // room for floating point rounding at large translations.
                    const float margin = Math::Max(1.0f, receiverNear * 1e-6f);
                    nearPlane = Math::Max(minNear, firstBlocker - margin);
                    farPlane = Math::Min(range, receiverFar + margin);
                }
            }

            Matrix projection;
            Matrix::PerspectiveOffCenter(low.X * nearPlane, high.X * nearPlane, low.Y * nearPlane, high.Y * nearPlane,
                nearPlane, farPlane, projection);
            SetProjection(light, face, view, projection, forward);
        }
    }


    // Reject faces outside the conservative camera receiver hull before building the depth mask.
    void CullLocalLightFaces(ShadowLight& light, const ReceiverCorners& receivers, const BoundingBox& bounds)
    {
        if (receivers.IsEmpty())
            return;
        const BoundingSphere range(Vector3(Float3(light.Constants.PositionAndRange)), light.Constants.PositionAndRange.W);
        if (!bounds.Intersects(range))
        {
            light.FaceMask = 0;
            return;
        }
        for (int32 face = 0; face < light.FaceCount; face++)
        {
            if ((light.FaceMask & (1u << face)) == 0)
                continue;
            const BoundingFrustum projection(light.Views[face] * light.Projections[face]);
            for (int32 planeIndex = 0; planeIndex < 6; planeIndex++)
            {
                const Plane plane = projection.GetPlane(planeIndex);
                bool overlaps = false;
                for (const auto& receiver : receivers)
                {
                    if (Vector3::Dot(receiver, plane.Normal) + plane.D >= (Real)-0.1)
                    {
                        overlaps = true;
                        break;
                    }
                }
                if (!overlaps)
                {
                    light.FaceMask &= ~(1u << face);
                    break;
                }
            }
        }
    }

    void CollectCasters(const Array<IVisualData*>& objects, GPUDevice* device, Array<ShadowCaster>& casters)
    {
        for (auto* object : objects)
        {
            auto* warped = dynamic_cast<WarpedVisualData*>(object);
            auto* actor = warped ? dynamic_cast<StaticModel*>(warped->GetIVD_Source()) : nullptr;
            auto* model = actor ? actor->Model.Get() : nullptr;
            if (!model || !model->IsLoaded() || !EnumHasAnyFlags(actor->GetDrawModes(), DrawPass::Depth))
                continue;
            const int32 residentLOD = model->HighestResidentLODIndex();
            const int32 lod = actor->GetForcedLOD() >= 0 ? model->ClampLODIndex(actor->GetForcedLOD()) : residentLOD;
            if (lod < 0 || lod >= model->LODs.Count())
                continue;

            const auto& entries = actor->GetEntries();
            for (const auto& mesh : model->LODs[lod].Meshes)
            {
                const int32 slot = mesh.GetMaterialSlotIndex();
                if (!mesh.IsInitialized() || slot < 0 || slot >= model->MaterialSlots.Count())
                    continue;
                const auto& modelSlot = model->MaterialSlots[slot];
                const auto* entry = slot < entries.Count() ? &entries[slot] : nullptr;
                const auto mode = entry ? entry->ShadowsMode : modelSlot.ShadowsMode;
                if ((entry && !entry->Visible) || !EnumHasAnyFlags(mode, ShadowsCastingMode::DynamicOnly))
                    continue;
                auto* material = entry ? entry->Material.Get() : nullptr;
                if (!material || !material->IsReady())
                    material = modelSlot.Material.Get();
                if (!material || !material->IsReady())
                    material = device->GetDefaultMaterial();
                if (!material || !material->IsReady() ||
                    !EnumHasAllFlags(material->GetDrawModes(), DrawPass::Depth | DrawPass::GBuffer))
                    continue;
                const bool deformed = EnumHasAnyFlags(material->GetInfo().UsageFlags,
                    MaterialUsageFlags::UsePositionOffset | MaterialUsageFlags::UseDisplacement);
                const BoundingBox bounds = deformed ? warped->Bounds : BoundingBox::Transform(mesh.GetBox(), warped->WorldMatrix);
                casters.Add({ warped, actor, &mesh, material, bounds, deformed });
            }
        }
    }
    Int2 PackShadowPages(LightConstants& constants, int32 faceCount)
    {
        int32 atlasSize = 0;
        for (int32 face = 0; face < faceCount; face++)
        {
            if (constants.FaceMask & (1u << face))
                atlasSize = Math::Max(atlasSize, constants.ShadowPages[face].Z);
        }
        const int32 layerArea = atlasSize * atlasSize;
        int32 usedArea = 0;
        for (int32 size = atlasSize; size >= MinShadowPageResolution; size /= 2)
        {
            const int32 pageArea = size * size;
            for (int32 face = 0; face < faceCount; face++)
            {
                auto& page = constants.ShadowPages[face];
                if ((constants.FaceMask & (1u << face)) == 0 || page.Z != size)
                    continue;

                // Largest pages first. Morton order keeps every smaller power-of-two square aligned.
                int32 slot = (usedArea % layerArea) / pageArea;
                page = Int4(0, 0, size, usedArea / layerArea);
                for (int32 step = size; slot; step *= 2, slot >>= 2)
                {
                    page.X += (slot & 1) * step;
                    page.Y += ((slot >> 1) & 1) * step;
                }
                usedArea += pageArea;
            }
        }
        return Int2(atlasSize, Math::DivideAndRoundUp(usedArea, layerArea));
    }

    void DrawBlockerDepth(GPUContext* context, const ShadowLight& light, const LightConstants& constants,
                          const Array<ShadowCaster>& casters, const Vector3& origin, GPUTexture* depthTexture, const uint32* receiverMask)
    {
        PROFILE_GPU_CPU_NAMED("World Grid Blocker Depth");
        for (int32 layer = 0; layer < depthTexture->ArraySize(); layer++)
            context->ClearDepth(depthTexture->View(layer));

        for (int32 face = 0; face < light.FaceCount; face++)
        {
            if ((light.FaceMask & (1u << face)) == 0)
                continue;
            const Int4& page = constants.ShadowPages[face];
            const float size = (float)page.Z;
            context->SetViewportAndScissors(Viewport((float)page.X, (float)page.Y, size, size));

            RenderContext depth;
            depth.List = RenderList::GetFromPool();
            auto& view = depth.View;
            view.Pass = DrawPass::Depth;
            view.Flags = ViewFlags::None;
            view.Origin = origin;
            view.Position = light.Constants.Kind == (uint32)RenderLightKind::Directional ?
                Float3(Matrix::Invert(light.Views[face]).GetTranslation()) : Float3(light.Constants.PositionAndRange);
            view.Direction = light.Directions[face];
            view.SetUp(light.Views[face], light.Projections[face]);
            view.PrevViewProjection = view.ViewProjection();
            view.PrepareCache(depth, size, size, Float2::Zero);
            depth.List->Init(depth);
            const Matrix viewProjection = view.ViewProjection();
            const BoundingFrustum frustum(viewProjection);
            const auto* faceReceivers = receiverMask ? receiverMask + face * ShadowReceiverFaceStride : nullptr;
            Matrix receiverViewProjection = viewProjection;
            if (light.ReceiverFarPlanes[face].X != light.ReceiverFarPlanes[face].Y)
            {
                // Caster frustum uses the fitted range. The existing tile
                // mask is encoded with the original depth coefficients.
                Matrix originalDepth = light.Projections[face];
                originalDepth.M33 = light.ReceiverDepthMapping[face].X;
                originalDepth.M43 = light.ReceiverDepthMapping[face].Y;
                receiverViewProjection = light.Views[face] * originalDepth;
            }
            if (light.ReceiverScissors[face].Z > 0)
            {
                const Int4& scissor = light.ReceiverScissors[face];
                context->SetScissor(Rectangle((float)(page.X + scissor.X), (float)(page.Y + scissor.Y),
                    (float)scissor.Z, (float)scissor.W));
            }
            bool overlaps = false;
            const WarpedVisualData* previous = nullptr;
            for (const auto& caster : casters)
            {
                if (previous != caster.Object)
                {
                    previous = caster.Object;
                    const auto& bounds = caster.Object->Bounds;
                    overlaps = frustum.Intersects(bounds);
                }
                if (overlaps && (!faceReceivers || caster.HasDeformation || ReachesReceiver(caster.Bounds, receiverViewProjection, light.ReceiverTileRect[face], page.Z, faceReceivers)))
                    caster.Geometry->Draw(depth, caster.Material, caster.Object->WorldMatrix,
                        caster.Actor->GetStaticFlags(), false, DrawPass::Depth, caster.Actor->GetPerInstanceRandom());
            }

            context->SetRenderTarget(depthTexture->View(page.W), static_cast<GPUTextureView*>(nullptr));
            if (!depth.List->DrawCallsLists[(int32)DrawCallsListType::Depth].IsEmpty())
            {
                depth.List->SortDrawCalls(depth, false, DrawCallsListType::Depth, DrawPass::Depth);
                depth.List->ExecuteDrawCalls(depth, DrawCallsListType::Depth);
            }
            context->ResetRenderTarget();
            context->ResetSR();
            RenderList::ReturnToPool(depth.List);
        }
    }

#if USE_EDITOR
    void DrawShadowPageLegend(GPUContext* context, GPUTexture* output, const Viewport& viewport, Font* font,
                              int32 lightIndex, const ShadowLight* light, const GPUTexture* atlas, const GridConstants& grid)
    {
        if (!font)
            return;
        StringBuilder text;
        text.AppendFormat(TEXT("Shadow light {}"), lightIndex);
        if (light)
        {
            const auto kind = (RenderLightKind)light->Constants.Kind;
            const Char* type = kind == RenderLightKind::Directional ? TEXT("Directional") :
                (kind == RenderLightKind::Point ? TEXT("Point") : TEXT("Spot"));
            text.AppendFormat(TEXT("  {}"), type);
            if (atlas)
            {
                text.AppendFormat(TEXT("\nWorld page {:.2f} m | {} px max | {:.3f} cm finest texel"),
                    grid.WorldPageOriginAndSpan.W * 0.01f, grid.WorldPageResolution,
                    grid.WorldPageOriginAndSpan.W / grid.WorldPageResolution);
                text.AppendLine();
                text.Append(TEXT("Fine to coarse: cyan, green, yellow, orange, pink, violet"));
                text.AppendLine();
                text.AppendFormat(TEXT("Blocker atlas: {} px per layer"), atlas->Width());
                text.AppendLine();
                text.Append(TEXT("Blocker LOD / crop px:"));
                for (int32 face = 0; face < light->FaceCount; ++face)
                {
                    if ((light->FaceMask & (1u << face)) == 0)
                        continue;
                    const int32 size = light->Constants.ShadowPages[face].Z;
                    const Int4& crop = light->ReceiverScissors[face];
                    if (kind == RenderLightKind::Point)
                        text.AppendFormat(TEXT(" {}"), PointFaceNames[face]);
                    if (crop.Z > 0 && (crop.Z < size || crop.W < size))
                        text.AppendFormat(TEXT(" {}/{}x{}"), size, crop.Z, crop.W);
                    else
                        text.AppendFormat(TEXT(" {}"), size);
                }
                bool showFar = true;
                for (int32 face = 0; face < light->FaceCount; ++face)
                {
                    const Float2& range = light->ReceiverFarPlanes[face];
                    if ((light->FaceMask & (1u << face)) == 0 || range.X == range.Y)
                        continue;
                    if (showFar)
                    {
                        text.AppendLine();
                        text.Append(TEXT("Blocker far (m):"));
                        showFar = false;
                    }
                    if (kind == RenderLightKind::Point)
                        text.AppendFormat(TEXT(" {}"), PointFaceNames[face]);
                    else
                        text.AppendFormat(TEXT(" #{}"), face);
                    text.AppendFormat(TEXT(" {:.1f}->{:.1f}"), range.X * 0.01f, range.Y * 0.01f);
                }
            }
            else
            {
                text.AppendLine();
                text.Append(TEXT("No shadow pages"));
            }
            if (light->OutsideCameraFaces)
            {
                text.AppendLine();
                text.Append(TEXT("Outside camera:"));
                for (int32 face = 0; face < light->FaceCount; ++face)
                    if (light->OutsideCameraFaces & (1u << face))
                        text.AppendFormat(TEXT(" {}"), PointFaceNames[face]);
            }
        }
        else
        {
            text.AppendLine();
            text.Append(TEXT("No light at this index"));
        }
        const String label = text.ToString();
        const Float2 size = font->MeasureText(label);
        Render2D::Begin(context, output->View(), nullptr, viewport);
        Render2D::FillRectangle(Rectangle(6.0f, 6.0f, size.X + 12.0f, size.Y + 6.0f), Color(0.0f, 0.0f, 0.0f, 0.8f));
        Render2D::DrawText(font, label, Color::White, Float2(12.0f, 9.0f));
        Render2D::End();
    }
#endif

    bool InitResolveState(GPUDevice* device, GPUShader* shader, GPUPipelineState*& state, const char* entry,
                          int32 permutation, const BlendingMode& blend)
    {
        if (state && state->IsValid())
            return true;
        if (!state)
            state = device->CreatePipelineState();
        auto desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        desc.PS = shader->GetPS(entry, permutation);
        desc.BlendMode = blend;
        return state && desc.PS && !state->Init(desc);
    }
}

struct VirtualShadowMapData
{
    AssetReference<Shader> ShaderAsset;
    Array<ShadowLight> Lights;
    Array<ShadowCaster> Casters;
    Vector3 Origin = Vector3::Zero;
    GridConstants Grid = {};
    ShadowReceiverMask ReceiverMask;
    Array<uint32> ReceiverDepths;
    WorldShadowPages Pages;
#if USE_EDITOR
    AssetReference<FontAsset> DebugFont;
#endif
    bool Ready = false;
};
VirtualShadowMapStage::VirtualShadowMapStage(const SpawnParams& params)
    : RenderPipelineStage(params)
{
}

VirtualShadowMapStage::~VirtualShadowMapStage()
{
    Delete(Data);
}

void VirtualShadowMapStage::OnEndFrame(GPUContext* context)
{
    if (Data)
    {
        Data->Ready = false;
        Data->Lights.Clear();
        Data->Casters.Clear();
    }
}

StageResult VirtualShadowMapStage::Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources)
{
    if (Data)
        Data->Ready = false;
    auto* depth = resources.Get<GPUTexture>(TEXT("Scene.Depth"));
    auto* camera = dynamic_cast<WarpedViewpoint*>(viewpoint);
    auto* device = context->GetDevice();
    if (!depth || !camera || !device->Limits.HasCompute || device->GetRendererType() != RendererType::DirectX11 ||
        context != device->GetMainContext())
        return StageResult::Skipped;
    if (!Data)
        Data = New<VirtualShadowMapData>();
    auto& data = *Data;
    data.Lights.Clear();
    data.Casters.Clear();
    data.ReceiverDepths.Clear();
    if (!data.ShaderAsset)
        data.ShaderAsset = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/Utils/VirtualShadowMaps"));
    if (!data.ShaderAsset || !data.ShaderAsset->IsLoaded() || !data.ShaderAsset->GPU)
        return StageResult::Skipped;
    auto* shader = data.ShaderAsset->GPU;
    auto* gridCB = shader->GetCB(0);
    if (!gridCB || gridCB->GetSize() != sizeof(GridConstants))
        return StageResult::Skipped;
    const auto* settings = resources.GetGraphicsSettings();
    const int32 requestedResolution = settings ? settings->VirtualShadowMapResolution : 512;
    const int32 resolution = 1 << Math::FloorLog2(Math::Clamp(requestedResolution, 512, MaxGridResolution));
    const Real span = settings ? Math::Max(100.0f, settings->VirtualShadowWorldGridSpan) : 10000.0f;
    const Real finestTexelSize = span / resolution;
    data.Origin = camera->Origin;
    auto& grid = data.Grid;
    Matrix::Transpose(InvertShadowProjection(camera->SceneDepthProjection), grid.CameraInverseProjection);
    Matrix::Transpose(Matrix::Invert(camera->View), grid.CameraInverseView);
    grid.Width = depth->Width();
    grid.Height = depth->Height();
    grid.CameraDepthClear = camera->SceneDepthClear;
    grid.WorldShadowTableMask = 0;
    const auto pageAnchor = [](Real value, double pageSpan)
    {
        const double coordinate = (double)value;
        return (float)(Math::Floor(coordinate / pageSpan) * pageSpan - coordinate);
    };
    grid.WorldPageOriginAndSpan = Float4(
        pageAnchor(data.Origin.X, span), pageAnchor(data.Origin.Y, span), pageAnchor(data.Origin.Z, span), (float)span);
    grid.WorldPageResolution = resolution;
    grid.CameraForward = Float3(camera->View.M13, camera->View.M23, camera->View.M33);

    bool needsLocalShadows = false;
    bool needsDirectionalShadows = false;
    for (auto* input : resources.GetLights())
    {
        if (!input)
            continue;
        const auto kind = input->GetILD_Type();
        const float range = input->GetILD_Range();
        const Color color = input->GetILD_Color();
        const float intensity = input->GetILD_Intensity() * color.A;
        if (intensity <= 0 || (kind != RenderLightKind::Directional && range <= 0))
            continue;
        auto& constants = data.Lights.AddOne().Constants;
        const Float3 position(input->GetILD_WorldPosition() - data.Origin);
        const Float3 direction = Float3::Normalize(Float3(input->GetILD_Direction()));
        const float outer = Math::Clamp(input->GetILD_OuterConeAngle(), 0.5f, 89.5f);
        const float inner = Math::Clamp(input->GetILD_InnerConeAngle(), 0.0f, outer);
        constants.PositionAndRange = Float4(position, range);
        constants.DirectionAndOuterCos = Float4(direction, Math::Cos(outer * DegreesToRadians));
        constants.ColorAndFalloff = Float4(color.R * intensity, color.G * intensity, color.B * intensity, Math::Max(0.1f, input->GetILD_FalloffExponent()));
        constants.Options = Float4(Math::Cos(inner * DegreesToRadians), input->GetILD_InverseSquaredFalloff() ? 1.0f : 0.0f,
            input->GetILD_CastShadows() ? Math::Saturate(input->GetILD_ShadowStrength()) : 0.0f, 0);
        constants.Kind = (uint32)kind;
        constants.DepthWidth = resolution;
        constants.DepthHeight = resolution;
        needsLocalShadows |= kind != RenderLightKind::Directional && constants.Options.Z > 0;
        needsDirectionalShadows |= kind == RenderLightKind::Directional && constants.Options.Z > 0;
    }

    if (needsDirectionalShadows || needsLocalShadows)
        CollectCasters(objects, device, data.Casters);
    const bool cullShadows = !settings || settings->EnableOcclusionCulling;
    const BoundingFrustum cameraFrustum(camera->ViewProjection);
    Vector3 cameraCorners[8];
    if (needsLocalShadows && cullShadows)
        cameraFrustum.GetCorners(cameraCorners);
    const BoundingBox allReceiverBounds = data.Casters.IsEmpty() ? BoundingBox::Empty : GetCameraReceiverBounds(objects, cameraFrustum);
    ReceiverCorners receivers;
    if (needsLocalShadows)
        GetCameraReceiverHull(allReceiverBounds, cameraFrustum, receivers);

    for (auto& light : data.Lights)
    {
        if (light.Constants.Options.Z <= 0)
            continue;
        if (light.Constants.Kind == (uint32)RenderLightKind::Directional)
        {
            SetDirectionalProjections(light, *camera, allReceiverBounds, finestTexelSize, data.Casters);
        }
        else
        {
            SetLocalLightProjections(light, allReceiverBounds, data.Casters, cameraFrustum, cameraCorners, span, cullShadows);
            CullLocalLightFaces(light, receivers, allReceiverBounds);
        }
    }

    auto* normals = resources.Get<GPUTexture>(TEXT("Scene.Normals"));
    if (!data.Casters.IsEmpty() && normals)
    {
        const auto* mask = data.ReceiverMask.Build(context, shader, gridCB, depth, normals, grid, data.Lights);
        if (mask)
        {
            int32 count = 0;
            for (auto& light : data.Lights)
            {
                if (!light.FaceMask)
                    continue;
                const int32 offset = light.ReceiverMaskOffset;
                count = offset + light.FaceCount * ShadowReceiverFaceStride;
                for (int32 face = 0; face < light.FaceCount; face++)
                {
                    if ((light.FaceMask & (1u << face)) == 0)
                        continue;
                    const auto* faceMask = mask + offset + face * ShadowReceiverFaceStride;
                    if (!FitBlockerPage(light, face, faceMask, cullShadows) && cullShadows)
                    {
                        light.FaceMask &= ~(1u << face);
                    }
                }
            }
            if (cullShadows)
            {
                data.ReceiverDepths.Resize(count);
                Platform::MemoryCopy(data.ReceiverDepths.Get(), mask, count * sizeof(uint32));
            }
            data.ReceiverMask.Readback->Unmap();
        }
    }

    for (const auto& light : data.Lights)
    {
        if (!light.FaceMask)
            continue;
        if (normals)
            data.Pages.Gather(context, shader, gridCB, depth, normals, grid);
        break;
    }
    data.Ready = true;
    return StageResult::Completed;
}

VirtualShadowMapDrawStage::VirtualShadowMapDrawStage(const SpawnParams& params)
    : RenderPipelineStage(params)
{
}

VirtualShadowMapDrawStage::~VirtualShadowMapDrawStage()
{
    SAFE_DELETE_GPU_RESOURCE(AmbientState);
    SAFE_DELETE_GPU_RESOURCE(LightState);
    SAFE_DELETE_GPU_RESOURCE(ShadowPagesState);
}

StageResult VirtualShadowMapDrawStage::Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources)
{
    auto* data = PageStage ? PageStage->Data : nullptr;
    if (!data || !data->Ready)
        return StageResult::Skipped;
    auto* depth = resources.Get<GPUTexture>(TEXT("Scene.Depth"));
    auto* normals = resources.Get<GPUTexture>(TEXT("Scene.Normals"));
    auto* base = resources.Get<GPUTexture>(TEXT("Scene.BaseColor"));
    auto* emissive = resources.Get<GPUTexture>(TEXT("Scene.Emissive"));
    auto* color = resources.Get<GPUTexture>(TEXT("Scene.Color"));
    if (!depth || !normals || !base || !emissive || !color)
        return StageResult::Skipped;

    auto* device = context->GetDevice();
    auto* shader = data->ShaderAsset->GPU;
    auto* gridCB = shader->GetCB(0);
    auto* lightCB = shader->GetCB(1);

    const auto* settings = resources.GetGraphicsSettings();
    const bool showVisibility = settings && settings->ShowVirtualShadowPages;
    const bool showGrid = settings && settings->ShowVirtualShadowWorldGrid;
    if (!gridCB || !lightCB || lightCB->GetSize() != sizeof(LightConstants) ||
        !InitResolveState(device, shader, AmbientState, "PS_ShadowAmbient", 0, BlendingMode::Opaque) ||
        !InitResolveState(device, shader, LightState, "PS_ResolveWorldGridLight", 0, BlendingMode::Add) ||
        ((showGrid || showVisibility) && !InitResolveState(device, shader, ShadowPagesState, "PS_ShowShadowPages", 0, BlendingMode::AlphaBlend)))
        return StageResult::Skipped;

    const Viewport viewport(0, 0, (float)color->Width(), (float)color->Height());
    context->ResetRenderTarget();
    context->ResetSR();
    context->SetViewportAndScissors(viewport);
    context->SetRenderTarget(color->View());
    context->BindSR(0, depth->View());
    context->BindSR(1, normals->View());
    context->BindSR(2, base->View());
    context->BindSR(3, emissive->View());
    context->UpdateCB(gridCB, &data->Grid);
    context->BindCB(0, gridCB);
    context->SetState(AmbientState);
    context->DrawFullscreenTriangle();
    context->ResetSR();
    context->ResetRenderTarget();

    auto* projectPages = shader->GetCS("CS_ProjectWorldShadowPages");
    const bool usePages = projectPages && data->Grid.WorldShadowTableMask != 0;
    GPUTexture* debugDepth = nullptr;
    const ShadowLight* debugLight = nullptr;
    LightConstants debugConstants = {};
    int32 shadowIndex = 0;
    for (const auto& light : data->Lights)
    {
        auto constants = light.Constants;
        constants.FaceMask = light.FaceMask;
        GPUTexture* lightDepth = nullptr;
        if (constants.Options.Z > 0)
        {
            const bool capture = (showVisibility || showGrid) && shadowIndex == settings->VirtualShadowDebugLight;
            shadowIndex++;
            if (capture)
                debugLight = &light;
            if (light.FaceMask)
            {
                const Int2 atlas = PackShadowPages(constants, light.FaceCount);
                const auto depthDesc = GPUTextureDescription::New2D(atlas.X, atlas.X, PixelFormat::D32_Float,
                    GPUTextureFlags::DepthStencil | GPUTextureFlags::ShaderResource, Math::Max(2, atlas.Y));
                lightDepth = RenderTargetPool::Get(depthDesc);
                if (lightDepth)
                    RENDER_TARGET_POOL_SET_NAME(lightDepth, "World Shadow Depth Atlas");
            }
            if (!lightDepth)
                constants.Options.Z = 0;
            else
            {
                const auto* receiverMask = data->ReceiverDepths.IsEmpty() ? nullptr :
                    data->ReceiverDepths.Get() + light.ReceiverMaskOffset;
                DrawBlockerDepth(context, light, constants, data->Casters, data->Origin, lightDepth, receiverMask);
                if (usePages)
                {
                    context->UpdateCB(lightCB, &constants);
                    context->BindCB(0, gridCB);
                    context->BindCB(1, lightCB);
                    data->Pages.Project(context, projectPages, lightDepth);
                }
                if (capture)
                {
                    debugDepth = lightDepth;
                    debugConstants = constants;
                }
            }
        }

        {
            PROFILE_GPU_CPU_NAMED("Resolve World Grid Light");
            context->SetViewportAndScissors(viewport);
            context->SetRenderTarget(color->View());
            context->BindSR(0, depth->View());
            context->BindSR(1, normals->View());
            context->BindSR(2, base->View());
            if (lightDepth)
            {
                context->BindSR(6, lightDepth->ViewArray());
                if (usePages)
                    data->Pages.Bind(context);
            }
            context->UpdateCB(lightCB, &constants);
            context->BindCB(0, gridCB);
            context->BindCB(1, lightCB);
            context->SetState(LightState);
            context->DrawFullscreenTriangle();
            context->ResetSR();
            context->ResetRenderTarget();
        }
        if (lightDepth != debugDepth)
            RenderTargetPool::Release(lightDepth);
    }

    if (debugDepth)
    {
        PROFILE_GPU_CPU_NAMED("Show Shadow Pages");
        auto grid = data->Grid;
        grid.DebugMode = (showGrid ? 1u : 0u) | (showVisibility ? 2u : 0u);
        context->SetViewportAndScissors(viewport);
        context->SetRenderTarget(color->View());
        context->BindSR(0, depth->View());
        context->BindSR(1, normals->View());
        // Rebuild visibility for the selected light because the same
        // physical texel buffer is reused for each light.
        if (showVisibility && usePages)
        {
            context->ResetRenderTarget();
            context->ResetSR();
            context->UpdateCB(lightCB, &debugConstants);
            context->BindCB(0, gridCB);
            context->BindCB(1, lightCB);
            data->Pages.Project(context, projectPages, debugDepth);
            context->SetRenderTarget(color->View());
            context->BindSR(0, depth->View());
            context->BindSR(1, normals->View());
            data->Pages.Bind(context);
        }
        context->BindSR(6, debugDepth->ViewArray());
        context->UpdateCB(lightCB, &debugConstants);
        context->BindCB(1, lightCB);
        context->UpdateCB(gridCB, &grid);
        context->BindCB(0, gridCB);
        context->SetState(ShadowPagesState);
        context->DrawFullscreenTriangle();
        context->ResetSR();
        context->ResetRenderTarget();
    }

#if USE_EDITOR
    if (showGrid || showVisibility)
    {
        if (!data->DebugFont)
            data->DebugFont = Content::LoadAsyncInternal<FontAsset>(TEXT("Editor/Fonts/Roboto-Regular"));
        if (data->DebugFont && data->DebugFont->IsLoaded())
            DrawShadowPageLegend(context, color, viewport, data->DebugFont->CreateFont(15.0f),
                settings->VirtualShadowDebugLight, debugLight, debugDepth, data->Grid);
    }
#endif
    RenderTargetPool::Release(debugDepth);
    return StageResult::Completed;
}
