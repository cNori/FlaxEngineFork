// Copyright (c) Wojciech Figat. All rights reserved.
#include "./Flax/Common.hlsl"
#include "./Flax/Utils/ReceiverSurface.hlsl"
META_CB_BEGIN(0, CameraSurfaceData)
float4x4 SurfaceInverseViewProjection;
float4 SurfaceCameraPosition;
uint2 SurfaceSize;
uint2 SurfacePadding;
META_CB_END
Texture2D<float> SurfaceDepth : register(t0);
RWTexture2D<float4> SurfaceNormals : register(u0);
META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(8,8,1)]
void CS_V2CameraReceiverSurface(uint3 id : SV_DispatchThreadID)
{
    if(any(id.xy>=SurfaceSize)) return;
    float3 world,normal;
    bool valid=V2ReceiverWorld(SurfaceDepth,int2(id.xy),SurfaceSize,SurfaceInverseViewProjection,world);
    if(valid) valid=V2ReceiverSurfaceNormal(SurfaceDepth,int2(id.xy),world,SurfaceSize,SurfaceInverseViewProjection,SurfaceCameraPosition.xyz,normal);
    SurfaceNormals[id.xy]=valid?float4(normal*.5+.5,1):float4(.5,.5,.5,0);
}
