// Copyright (c) Wojciech Figat. All rights reserved.

#ifndef V2_SHADOW_RECEIVER
#define V2_SHADOW_RECEIVER

// Fit the CAMERA device-depth slope along one screen axis. Return (slope, valid).
// All sampled depths and the fit tolerance below are raw device depth, not centimetres.
// This assumes samples chosen for a fit belong to the same planar surface.
float2 V2DepthAxisGradient(Texture2D<float> depthTexture, float depth, int2 pixel, int2 axis, int2 size)
{
    // Fit device depth, which is affine over a rasterized triangle. A wider
    // stencil reduces D24 quantization, while extrapolation rejects silhouettes.
    // Edge selection follows https://atyuwen.github.io/posts/normal-reconstruction/
    // Sample at -8, -4, +4 and +8 pixels. A planar triangle's device depth is affine:
    // depth(x +/- 8) - depth(x) should be twice depth(x +/- 4) - depth(x).
    const int radius = 4;
    int2 l1 = pixel - axis * radius;
    int2 l2 = pixel - axis * (2 * radius);
    int2 r1 = pixel + axis * radius;
    int2 r2 = pixel + axis * (2 * radius);
    float a = depthTexture.Load(int3(clamp(l1, 0, size - 1), 0));
    float b = depthTexture.Load(int3(clamp(l2, 0, size - 1), 0));
    float c = depthTexture.Load(int3(clamp(r1, 0, size - 1), 0));
    float d = depthTexture.Load(int3(clamp(r2, 0, size - 1), 0));
    // Allow four D24 quantization steps in that extrapolation check.
    // left/right indicate an acceptable fit on that side, not camera visibility.
    const float tolerance = 4.0 / 16777215.0;
    bool left = all(l2 >= 0) && a != DEPTH_RANGE_MAX && b != DEPTH_RANGE_MAX &&
        abs((a - depth) * 2.0 - (b - depth)) <= tolerance;
    bool right = all(r2 < size) && c != DEPTH_RANGE_MAX && d != DEPTH_RANGE_MAX &&
        abs((c - depth) * 2.0 - (d - depth)) <= tolerance;
    // Weighted least-squares slopes. Dividing by the sample spacing converts the
    // differences back to device-depth change per ONE camera pixel.
    // When both sides pass, they are combined; passing does not prove both sides
    // are the same physical surface at a wall/floor boundary.
    if (left && right)
        return float2(((c - a) + 2.0 * (d - b)) / (10.0 * radius), 1);
    if (left)
        return float2(((depth - a) + 2.0 * (depth - b)) / (5.0 * radius), 1);
    if (right)
        return float2(((c - depth) + 2.0 * (d - depth)) / (5.0 * radius), 1);
    return 0;
}

// Reconstruct a geometric world-space normal from CAMERA depth.
// receiver is already unprojected into the same world/origin space as the matrices.
// shadingNormal is the decoded, normalized GBuffer normal used for orientation/fallback.
// Return XYZ = chosen normal; W = 1 if the depth-plane checks pass, otherwise 0.
// W is a binary confidence flag, not a probability or an occlusion result.
float4 V2ReceiverPlaneNormalChecked(Texture2D<float> depthTexture, float3 receiver, float depth, int2 pixel, float2 size,
    float4x4 inverseViewProjection, float3 shadingNormal)
{
    float2 xGradient = V2DepthAxisGradient(depthTexture, depth, pixel, int2(1, 0), int2(size));
    float2 yGradient = V2DepthAxisGradient(depthTexture, depth, pixel, int2(0, 1), int2(size));
    if (xGradient.y == 0 || yGradient.y == 0)
        return float4(shadingNormal, 0);
    float2 gradient = float2(xGradient.x, yGradient.x);

    // Differentiate homogeneous unprojection without subtracting large world positions.
    // A camera pixel moves NDC by (2/width, -2/height). Combine that screen
    // displacement with the fitted depth change, then differentiate P = q.xyz/q.w.
    // dx.xyz - receiver*dx.w and dy.xyz - receiver*dy.w are proportional to
    // world-space surface tangents; their cross product gives the plane normal.
    float4 dx = PROJECT_POINT(float4(2.0 / size.x, 0, gradient.x, 0), inverseViewProjection);
    float4 dy = PROJECT_POINT(float4(0, -2.0 / size.y, gradient.y, 0), inverseViewProjection);
    float3 planeNormal = cross(dx.xyz - receiver * dx.w, dy.xyz - receiver * dy.w);
    float lengthSquared = dot(planeNormal, planeNormal);
    float agreement = dot(planeNormal, shadingNormal);
    // At a silhouette the quad may straddle two surfaces.
    // Depth extrapolation above validates the plane. Use the shading normal
    // only to orient it; normal-map detail must not replace a valid geometric slope.
    if (lengthSquared <= 1e-20)
        return float4(shadingNormal, 0);
    return float4(planeNormal * ((agreement < 0 ? -1.0 : 1.0) * rsqrt(lengthSquared)), 1);
}

float3 V2ReceiverPlaneNormal(Texture2D<float> depthTexture, float3 receiver, float depth, int2 pixel, float2 size,
    float4x4 inverseViewProjection, float3 shadingNormal)
{
    return V2ReceiverPlaneNormalChecked(depthTexture, receiver, depth, pixel, size, inverseViewProjection, shadingNormal).xyz;
}

float3 V2OffsetShadowReceiver(float3 receiver, float3 normal, float3 directionToLight, float normalBias)
{
    float cosine = saturate(dot(normal, directionToLight));
    float offset = normalBias * sqrt(saturate(1.0 - cosine * cosine));
    return receiver + normal * offset;
}

// Estimate WORLD-SPACE position error caused by two CAMERA D24 depth steps.
// homogeneousW is q.w BEFORE the perspective divide, where q = clip*inverseVP.
// dP/dDepth = (inverseVP_depth.xyz - P*inverseVP_depth.w) / q.w.
// Return a world-space error vector (engine units/cm), not a scalar depth bias.
float3 V2CameraDepthUncertainty(float3 receiver, float homogeneousW, float4x4 inverseViewProjection)
{
    // The opaque GBuffer depth is D24. Allow two steps for rasterization and storage error.
    const float cameraDepthError = 2.0 / 16777215.0;
    float4 depthDerivative = PROJECT_POINT(float4(0, 0, 1, 0), inverseViewProjection);
    return (depthDerivative.xyz - receiver * depthDerivative.w) * (cameraDepthError / homogeneousW);
}

// Convert camera position uncertainty to a tolerance in LIGHT device depth.
// depthPerTexel is the light-space receiver-plane slope; size is light-map texels.
// precisionBias, cameraBias and the returned sum are normalized light depth.
// Debug converts the result to cm using SelectionReceiverRange.z.
float V2ReceiverDepthBias(float precisionBias, float2 depthPerTexel, float2 size,
    float4x4 lightProjection, float3 receiverUncertainty)
{
    // Carry camera-depth error through both light depth and the receiver-plane UV correction.
    float4 lightError = PROJECT_POINT(float4(receiverUncertainty, 0), lightProjection);
    float2 texelError = lightError.xy * size * float2(0.5, -0.5);
    // The same position error changes both the light depth and the light UV.
    // Remove the depth change explained by motion along the fitted receiver plane.
    float cameraBias = abs(lightError.z - dot(depthPerTexel, texelError));

    // Add a small slope allowance (1/8 of the summed per-texel slopes), capped
    // before scaling so a grazing plane cannot produce an unlimited allowance.
    float slope = dot(abs(depthPerTexel), float2(1, 1));
    return precisionBias + cameraBias + 0.125 * min(slope, rcp(min(size.x, size.y)));
}

// Device depth is affine over a projected plane, including a perspective view.
// Differentiate two world-space tangents through the homogeneous divide, then
// solve dz = gradient.x * du + gradient.y * dv. Unlike directional shadows this
// must include clip.w; orthographic axis-length correction is not valid here.
float2 V2PerspectiveShadowGradient(float3 receiver, float3 normal, float4x4 projection, float2 size)
{
    float3 tangent = normalize(cross(normal, abs(normal.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0)));
    float3 bitangent = cross(normal, tangent);
    float4 h = PROJECT_POINT(float4(receiver, 1), projection);
    float3 ndc = h.xyz / h.w;
    float4 a = PROJECT_POINT(float4(tangent, 0), projection);
    float4 b = PROJECT_POINT(float4(bitangent, 0), projection);
    float3 da = (a.xyz - ndc * a.w) / h.w;
    float3 db = (b.xyz - ndc * b.w) / h.w;
    float2 uvA = da.xy * float2(0.5, -0.5);
    float2 uvB = db.xy * float2(0.5, -0.5);
    float determinant = uvA.x * uvB.y - uvB.x * uvA.y;
    if (abs(determinant) < 1e-20) return 0;
    return float2(da.z * uvB.y - db.z * uvA.y, uvA.x * db.z - uvB.x * da.z) /
        (determinant * size);
}

#endif
