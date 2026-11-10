// Copyright (c) Wojciech Figat. All rights reserved.

#ifndef V2_RECEIVER_SURFACE
#define V2_RECEIVER_SURFACE
bool V2ReceiverWorld(Texture2D<float> source, int2 pixel, uint2 size, float4x4 inverse, out float3 world)
{
    world = 0;
    if (any(pixel < 0) || any(pixel >= int2(size))) return false;
    float z = source.Load(int3(pixel,0));
    if (z == DEPTH_RANGE_MAX) return false;
    float2 ndc = (float2(pixel)+0.5)/float2(size)*float2(2,-2)+float2(-1,1);
    float4 h = PROJECT_POINT(float4(ndc,z,1),inverse);
    if (abs(h.w) < 1e-12) return false;
    world = h.xyz/h.w;
    return true;
}
bool V2ReceiverSurfaceNormal(Texture2D<float> source, int2 pixel, float3 world, uint2 size, float4x4 inverse, float3 camera, out float3 normal)
{
    // Use a wider baseline to suppress depth quantisation on distant flat
    // surfaces. Choose each derivative by second-order depth continuity, not
    // by distance alone: at a wall/floor crease the nearer neighbour can belong
    // to the OTHER surface and tilt a wall normal into a light-facing normal.
    const int radius=2;
    float3 left,right,top,bottom,left2,right2,top2,bottom2;
    bool l=V2ReceiverWorld(source,pixel+int2(-radius,0),size,inverse,left);
    bool r=V2ReceiverWorld(source,pixel+int2(radius,0),size,inverse,right);
    bool t=V2ReceiverWorld(source,pixel+int2(0,-radius),size,inverse,top);
    bool b=V2ReceiverWorld(source,pixel+int2(0,radius),size,inverse,bottom);
    bool ll=V2ReceiverWorld(source,pixel+int2(-radius*2,0),size,inverse,left2);
    bool rr=V2ReceiverWorld(source,pixel+int2(radius*2,0),size,inverse,right2);
    bool tt=V2ReceiverWorld(source,pixel+int2(0,-radius*2),size,inverse,top2);
    bool bb=V2ReceiverWorld(source,pixel+int2(0,radius*2),size,inverse,bottom2);
    normal=0;
    if((!l&&!r)||(!t&&!b))return false;
    float z=source.Load(int3(pixel,0));
    float le=l&&ll?abs(2*source.Load(int3(pixel+int2(-radius,0),0))-source.Load(int3(pixel+int2(-radius*2,0),0))-z):1e20;
    float re=r&&rr?abs(2*source.Load(int3(pixel+int2(radius,0),0))-source.Load(int3(pixel+int2(radius*2,0),0))-z):1e20;
    float te=t&&tt?abs(2*source.Load(int3(pixel+int2(0,-radius),0))-source.Load(int3(pixel+int2(0,-radius*2),0))-z):1e20;
    float be=b&&bb?abs(2*source.Load(int3(pixel+int2(0,radius),0))-source.Load(int3(pixel+int2(0,radius*2),0))-z):1e20;
    float3 dx=l&&(!r||le<=re)?world-left:right-world;
    float3 dy=t&&(!b||te<=be)?world-top:bottom-world;
    float3 n=cross(dx,dy);
    float n2=dot(n,n),x2=dot(dx,dx),y2=dot(dy,dy);
    if (x2<1e-14||y2<1e-14||n2<x2*y2*1e-8) return false;
    normal=n*rsqrt(n2);
    if (dot(normal,camera-world)<0) normal=-normal;
    return true;
}
#endif
