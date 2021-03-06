#ifndef KFUSION_H
#define KFUSION_H

#include <iostream>

#include <vector_types.h>
#include <vector_functions.h>
#include "cutil_math.h"

// undefine this on 2.x devices 
// 1.x devices don't have 3D grids, therefore we compute 3D mapping to 2D here, 
// based on the idea that slices are layed out in x direction as blocks
#define USE_PLANAR_3D 1

inline int divup(int a, int b) { return (a % b != 0) ? (a / b + 1) : (a / b); }
inline dim3 divup( uint2 a, dim3 b) { return dim3(divup(a.x, b.x), divup(a.y, b.y)); }
inline dim3 divup( dim3 a, dim3 b) { return dim3(divup(a.x, b.x), divup(a.y, b.y), divup(a.z, b.z)); }

struct KFusionConfig {
    uint3 volumeSize;           // size of the volume in voxels
    float3 volumeDimensions;    // real world dimensions spanned by the volume in meters

    bool fullFrame;             // operate on 640x480 input downscale to 320x240 input
    bool combinedTrackAndReduce;// combine tracking and calculating linear system in one
                                // this saves some time in tracking, but there is no per pixel output anymore
    
    float4 camera;              // camera configuration parameters
    float nearPlane, farPlane;  // values for raycasting in meters
    float mu;                   // width of linear ramp, left and right of 0 in meters
    float maxweight;            // maximal weight for volume integration, controls speed of updates
    
    int radius;                 // bilateral filter radius
    float delta;                // gaussian delta
    float e_delta;              // euclidean delta
    
    float dist_threshold;       // 3D distance threshold for ICP correspondences
    float normal_threshold;     // dot product normal threshold for ICP correspondences
    int iterations[3];          // max number of iterations per level
    
    dim3 imageBlock;            // block size for image operations
    dim3 raycastBlock;          // block size for raycasting
    
    KFusionConfig(){
        volumeSize = make_uint3(64);
        volumeDimensions = make_float3(1.f);
        
        fullFrame = false;
        combinedTrackAndReduce = false;
        
        nearPlane = 0.4f;
        farPlane = 4.0f;
        mu = 0.1f;
        maxweight = 100.0f;
        
        radius = 2;
        delta = 4.0f;
        e_delta = 0.1f;
        
        dist_threshold = 0.2f;
        normal_threshold = 0.7f;
        iterations[0] = 5;
        iterations[1] = 5;
        iterations[2] = 5;
        
        imageBlock = dim3(20,20);
        raycastBlock = dim3(16,16);
    }
    
    float stepSize() const {  return 0.5f * min(volumeDimensions)/max(volumeSize); }          // step size for raycasting
    uint2 renderSize() const { return fullFrame ? make_uint2(640,480) : make_uint2(320,240); } // image resolution for rendering

};

struct Matrix4 {
    float4 data[4];
    
    inline __host__ __device__ float3 get_translation() const {
        return make_float3(data[0].w, data[1].w, data[2].w);
    }
};

inline std::ostream & operator<<( std::ostream & out, const Matrix4 & m ){
    for(unsigned i = 0; i < 4; ++i)
        out << m.data[i].x << "  " << m.data[i].y << "  " << m.data[i].z << "  " << m.data[i].w << "\n";
    return out;
}

inline Matrix4 transpose( const Matrix4 & A ){
    Matrix4 T;
    T.data[0] = make_float4(A.data[0].x, A.data[1].x, A.data[2].x, A.data[3].x);
    T.data[1] = make_float4(A.data[0].y, A.data[1].y, A.data[2].y, A.data[3].y);
    T.data[2] = make_float4(A.data[0].z, A.data[1].z, A.data[2].z, A.data[3].z);
    T.data[3] = make_float4(A.data[0].w, A.data[1].w, A.data[2].w, A.data[3].w);
    return T;
}

inline Matrix4 operator*( const Matrix4 & A, const Matrix4 & B){
    const Matrix4 T = transpose(B);
    Matrix4 C;
    for(uint r = 0; r < 4; ++r){
        C.data[r] = make_float4(dot(A.data[r], T.data[0]), 
                                dot(A.data[r], T.data[1]),
                                dot(A.data[r], T.data[2]),
                                dot(A.data[r], T.data[3]));
    }
    return C;
}

inline __host__ __device__ float3 operator*( const Matrix4 & M, const float3 & v){
    return make_float3(
        dot(make_float3(M.data[0]), v) + M.data[0].w,
        dot(make_float3(M.data[1]), v) + M.data[1].w,
        dot(make_float3(M.data[2]), v) + M.data[2].w);
}

inline __host__ __device__ float3 rotate( const Matrix4 & M, const float3 & v){
    return make_float3(
        dot(make_float3(M.data[0]), v),
        dot(make_float3(M.data[1]), v),
        dot(make_float3(M.data[2]), v));
}

inline Matrix4 getCameraMatrix( const float4 & k ){
    Matrix4 K;
    K.data[0] = make_float4(k.x, 0, k.z, 0);
    K.data[1] = make_float4(0, k.y, k.w, 0);
    K.data[2] = make_float4(0, 0, 1, 0);
    K.data[3] = make_float4(0, 0, 0, 1);
    return K;
}

inline Matrix4 getInverseCameraMatrix( const float4 & k ){
    Matrix4 invK;
    invK.data[0] = make_float4(1.0f/k.x, 0, -k.z/k.x, 0);
    invK.data[1] = make_float4(0, 1.0f/k.y, -k.w/k.y, 0);
    invK.data[2] = make_float4(0, 0, 1, 0);
    invK.data[3] = make_float4(0, 0, 0, 1);
    return invK;
} 

inline void computeVolumeConfiguration( dim3 & grid, dim3 & block, const uint3 size ){
#ifdef USE_PLANAR_3D
    if(size.z <= 64){
        block = dim3(2,2,size.z);
        grid = dim3(size.x/2, size.y/2, 1);
    } else {
        block = dim3(8,8,8);
        grid = dim3(size.x/8 * size.z/8, size.y/8, 1);
    }
#else
    block = dim3(8,8,8);
    grid = dim3(divup(size.x,8), divup(size.y,8), divup(size.z,8));
#endif
}

inline __device__ uint3 thr2pos3(){
#ifdef __CUDACC__
#ifdef USE_PLANAR_3D     // 1.x devices don't have 3D grids, therefore we
                         // compute 3D mapping to 2D here, based on the idea 
                         // that slices are layed out in x direction as blocks

    const uint size = __umul24(gridDim.y, blockDim.y); // this is what we are looking for
    const uint total_x = __umul24(blockDim.x, blockIdx.x) + threadIdx.x;
    const uint x = total_x % size;
    const uint y = __umul24(blockDim.y, blockIdx.y) + threadIdx.y;
    
    const uint z_layer = total_x / size;
    const uint z = __umul24(z_layer, __umul24(gridDim.z, blockDim.z)) + __umul24(blockDim.z, blockIdx.z) + threadIdx.z;
    
    return make_uint3(x, y, z);
#else
    return make_uint3( __umul24(blockDim.x, blockIdx.x) + threadIdx.x, 
                       __umul24(blockDim.y, blockIdx.y) + threadIdx.y, 
                       __umul24(blockDim.z, blockIdx.z) + threadIdx.z);
#endif
#else
    return make_uint3(0);
#endif
}

inline __device__ uint2 thr2pos2(){
#ifdef __CUDACC__
    return make_uint2( __umul24(blockDim.x, blockIdx.x) + threadIdx.x, 
                       __umul24(blockDim.y, blockIdx.y) + threadIdx.y);
#else
    return make_uint2(0);
#endif
}

inline __device__ float2 toFloat( const short2 & data ){
    return make_float2(data.x / 32766.0f, data.y);
}

inline __device__ short2 fromFloat( const float2 & data ){
    return make_short2(data.x * 32766.0f, data.y);
}

struct Volume {
    uint3 size;
    float3 dim;
    short2 * data;
    
    Volume() { size = make_uint3(0); dim = make_float3(1); data = NULL; }

    __device__ float2 el() const {
        return operator[](thr2pos3());
    }

    __device__ float2 operator[]( const uint3 & pos ) const {
        return toFloat(data[pos.x + pos.y * size.x + pos.z * size.x * size.y]);
    }

    __device__ float v(const uint3 & pos) const {
        return operator[](pos).x;
    }

    __device__ float w(const uint3 & pos) const {
        return operator[](pos).y;
    }

    __device__ void set(const uint3 & pos, const float2 & d ){
        data[pos.x + pos.y * size.x + pos.z * size.x * size.y] = fromFloat(d);
    }

    __device__ void set(const float2 d){
        set(thr2pos3(), d);
    }

    __device__ float3 pos( const uint3 p = thr2pos3() ) const {
        return make_float3((p.x + 0.5f) * dim.x / size.x, (p.y + 0.5f) * dim.y / size.y, (p.z + 0.5f) * dim.z / size.z);
    }

    __device__ float interp( const float3 & pos ) const {
#if 0   // only for testing without linear interpolation
        const float3 scaled_pos = make_float3((pos.x * size.x / dim.x) , (pos.y * size.y / dim.y) , (pos.z * size.z / dim.z) );
        return operator[](make_uint3(clamp(make_int3(scaled_pos), make_int3(0), make_int3(size) - make_int3(1))));
        
#else
        const float3 scaled_pos = make_float3((pos.x * size.x / dim.x) - 0.5f, (pos.y * size.y / dim.y) - 0.5f, (pos.z * size.z / dim.z) - 0.5f);
        const int3 base = make_int3(floorf(scaled_pos));
        const float3 factor = fracf(scaled_pos);
        const int3 lower = max(base, make_int3(0));
        const int3 upper = min(base + make_int3(1), make_int3(size) - make_int3(1));
        return v(make_uint3(lower.x, lower.y, lower.z)) * (1-factor.x) * (1-factor.y) * (1-factor.z)
            + v(make_uint3(upper.x, lower.y, lower.z)) * factor.x * (1-factor.y) * (1-factor.z)
            + v(make_uint3(lower.x, upper.y, lower.z)) * (1-factor.x) * factor.y * (1-factor.z)
            + v(make_uint3(upper.x, upper.y, lower.z)) * factor.x * factor.y * (1-factor.z)
            + v(make_uint3(lower.x, lower.y, upper.z)) * (1-factor.x) * (1-factor.y) * factor.z
            + v(make_uint3(upper.x, lower.y, upper.z)) * factor.x * (1-factor.y) * factor.z
            + v(make_uint3(lower.x, upper.y, upper.z)) * (1-factor.x) * factor.y * factor.z
            + v(make_uint3(upper.x, upper.y, upper.z)) * factor.x * factor.y * factor.z;
#endif
    }
    
    __device__ float3 grad( const float3 & pos ) const {
        const float3 scaled_pos = make_float3((pos.x * size.x / dim.x) - 0.5f, (pos.y * size.y / dim.y) - 0.5f, (pos.z * size.z / dim.z) - 0.5f);
        const int3 base = make_int3(floorf(scaled_pos));
        const float3 factor = fracf(scaled_pos);
        const int3 lower_lower = max(base - make_int3(1), make_int3(0));
        const int3 lower_upper = max(base, make_int3(0));
        const int3 upper_lower = min(base + make_int3(1), make_int3(size) - make_int3(1));
        const int3 upper_upper = min(base + make_int3(2), make_int3(size) - make_int3(1));
        const int3 & lower = lower_upper;
        const int3 & upper = upper_lower;

        float3 gradient;

        gradient.x =
              (v(make_uint3(upper_lower.x, lower.y, lower.z)) - v(make_uint3(lower_lower.x, lower.y, lower.z))) * (1-factor.x) * (1-factor.y) * (1-factor.z)
            + (v(make_uint3(upper_upper.x, lower.y, lower.z)) - v(make_uint3(lower_upper.x, lower.y, lower.z))) * factor.x * (1-factor.y) * (1-factor.z)
            + (v(make_uint3(upper_lower.x, upper.y, lower.z)) - v(make_uint3(lower_lower.x, upper.y, lower.z))) * (1-factor.x) * factor.y * (1-factor.z)
            + (v(make_uint3(upper_upper.x, upper.y, lower.z)) - v(make_uint3(lower_upper.x, upper.y, lower.z))) * factor.x * factor.y * (1-factor.z)
            + (v(make_uint3(upper_lower.x, lower.y, upper.z)) - v(make_uint3(lower_lower.x, lower.y, upper.z))) * (1-factor.x) * (1-factor.y) * factor.z
            + (v(make_uint3(upper_upper.x, lower.y, upper.z)) - v(make_uint3(lower_upper.x, lower.y, upper.z))) * factor.x * (1-factor.y) * factor.z
            + (v(make_uint3(upper_lower.x, upper.y, upper.z)) - v(make_uint3(lower_lower.x, upper.y, upper.z))) * (1-factor.x) * factor.y * factor.z
            + (v(make_uint3(upper_upper.x, upper.y, upper.z)) - v(make_uint3(lower_upper.x, upper.y, upper.z))) * factor.x * factor.y * factor.z;

        gradient.y =
              (v(make_uint3(lower.x, upper_lower.y, lower.z)) - v(make_uint3(lower.x, lower_lower.y, lower.z))) * (1-factor.x) * (1-factor.y) * (1-factor.z)
            + (v(make_uint3(upper.x, upper_lower.y, lower.z)) - v(make_uint3(upper.x, lower_lower.y, lower.z))) * factor.x * (1-factor.y) * (1-factor.z)
            + (v(make_uint3(lower.x, upper_upper.y, lower.z)) - v(make_uint3(lower.x, lower_upper.y, lower.z))) * (1-factor.x) * factor.y * (1-factor.z)
            + (v(make_uint3(upper.x, upper_upper.y, lower.z)) - v(make_uint3(upper.x, lower_upper.y, lower.z))) * factor.x * factor.y * (1-factor.z)
            + (v(make_uint3(lower.x, upper_lower.y, upper.z)) - v(make_uint3(lower.x, lower_lower.y, upper.z))) * (1-factor.x) * (1-factor.y) * factor.z
            + (v(make_uint3(upper.x, upper_lower.y, upper.z)) - v(make_uint3(upper.x, lower_lower.y, upper.z))) * factor.x * (1-factor.y) * factor.z
            + (v(make_uint3(lower.x, upper_upper.y, upper.z)) - v(make_uint3(lower.x, lower_upper.y, upper.z))) * (1-factor.x) * factor.y * factor.z
            + (v(make_uint3(upper.x, upper_upper.y, upper.z)) - v(make_uint3(upper.x, lower_upper.y, upper.z))) * factor.x * factor.y * factor.z;

        gradient.z =
              (v(make_uint3(lower.x, lower.y, upper_lower.z)) - v(make_uint3(lower.x, lower.y, lower_lower.z))) * (1-factor.x) * (1-factor.y) * (1-factor.z)
            + (v(make_uint3(upper.x, lower.y, upper_lower.z)) - v(make_uint3(upper.x, lower.y, lower_lower.z))) * factor.x * (1-factor.y) * (1-factor.z)
            + (v(make_uint3(lower.x, upper.y, upper_lower.z)) - v(make_uint3(lower.x, upper.y, lower_lower.z))) * (1-factor.x) * factor.y * (1-factor.z)
            + (v(make_uint3(upper.x, upper.y, upper_lower.z)) - v(make_uint3(upper.x, upper.y, lower_lower.z))) * factor.x * factor.y * (1-factor.z)
            + (v(make_uint3(lower.x, lower.y, upper_upper.z)) - v(make_uint3(lower.x, lower.y, lower_upper.z))) * (1-factor.x) * (1-factor.y) * factor.z
            + (v(make_uint3(upper.x, lower.y, upper_upper.z)) - v(make_uint3(upper.x, lower.y, lower_upper.z))) * factor.x * (1-factor.y) * factor.z
            + (v(make_uint3(lower.x, upper.y, upper_upper.z)) - v(make_uint3(lower.x, upper.y, lower_upper.z))) * (1-factor.x) * factor.y * factor.z
            + (v(make_uint3(upper.x, upper.y, upper_upper.z)) - v(make_uint3(upper.x, upper.y, lower_upper.z))) * factor.x * factor.y * factor.z;

        return gradient * make_float3(dim.x/size.x, dim.y/size.y, dim.z/size.z) * 0.5f;
    }
    
    void init(uint3 s, float3 d){
        size = s; 
        dim = d;
        cudaMalloc(&data, size.x * size.y * size.z * sizeof(short2));
    }
    
    void release(){
        cudaFree(data);
        data = NULL;
    }

    void get( void * target ) const {
        cudaMemcpy(target, data, size.x * size.y * size.y * sizeof(short2), cudaMemcpyDeviceToHost);
    }
};

template <typename T>
struct Image {
    typedef T PIXEL_TYPE;
    uint2 size;
    T * data;
    
    Image() { size = make_uint2(0); data = NULL; }
    
    __device__ T & el(){
        return operator[](thr2pos2());
    }
    
    __device__ const T & el() const {
        return operator[](thr2pos2());
    }
    
    __device__ T & operator[](const uint2 & pos ){
        return data[pos.x + size.x * pos.y];
    }

    __device__ const T & operator[](const uint2 & pos ) const {
        return data[pos.x + size.x * pos.y];
    }
    
    void init( uint2 s ){
        size = s;
        cudaMalloc(&data, size.x * size.y * sizeof(T));
    }
    
    void release(){
        cudaFree(data);
        data = NULL;
    }

    void get( void * target ) const {
        cudaMemcpy(target, data, size.x * size.y * sizeof(T), cudaMemcpyDeviceToHost);
    }
};

struct TrackData {
    int result;
    float error;
    float J[6];
};

struct KFusion {
    Volume integration, hand;
    Image<TrackData> reduction;
    Image<float3> vertex, normal, inputVertex[3], inputNormal[3];
    Image<float> depth, inputDepth[3];
    
    Image<float> rawDepth;
    Image<ushort> rawKinectDepth;
    Image<float> output;
    
    Image<float> gaussian;
    
    KFusionConfig configuration;

    Matrix4 pose, invPose;

    void Init( const KFusionConfig & config ); // allocates the volume and image data on the device
    void Clear();  // releases the allocated device memory

    void setPose( const Matrix4 & p, const Matrix4 & invP ); // sets the current pose of the camera

    // high level API to run a simple tracking - reconstruction loop
    void Reset(); // removes all reconstruction information

    void setKinectDepth( ushort * ); // passes in raw 11-bit kinect data as an array of ushort
    void setDeviceDepth( float * ); // passes in a metric depth buffer as float array residing in device memory 
    void setHostDepth( float * ); // passes in  a metric depth buffer as float array residing in host memory

    bool Track(); // Estimates new camera position based on the last depth map set and the volume
    void Integrate(); // Integrates the current depth map using the current camera pose
    void IntegrateHand();
    
    void FilterRawDepth();
};

int printCUDAError(); // print the last error

// low level API without any state. These are the kernel functions

__global__ void initVolume( Volume volume, const float2 val );
__global__ void raycast( Image<float3> pos3D, Image<float3> normal, Image<float> depth, const Volume volume, const Matrix4 view, const float near, const float far, const float step, const float largestep);
__global__ void integrate( Volume vol, Volume weight, const Image<float> depth, const Matrix4 view, const float mu, const float maxweight);
__global__ void depth2vertex( Image<float3> vertex, const Image<float> depth, const Matrix4 invK );
__global__ void vertex2normal( Image<float3> normal, const Image<float3> vertex );
__global__ void bilateral_filter(Image<float> out, const Image<float> in, const Image<float> gaussian, float e_d, int r);
__global__ void track( Image<TrackData> output, const Image<float3> inVertex, const Image<float3> inNormal , const Image<float3> refVertex, const Image<float3> refNormal, const Matrix4 Ttrack, const Matrix4 view, const float dist_threshold, const float normal_threshold ) ;
__global__ void reduce( float * out, const Image<TrackData> J, const uint2 size);
__global__ void trackAndReduce( float * out, const Image<float3> inVertex, const Image<float3> inNormal , const Image<float3> refVertex, const Image<float3> refNormal, const Matrix4 Ttrack, const Matrix4 view, const float dist_threshold, const float normal_threshold );

#endif // KFUSION_H
