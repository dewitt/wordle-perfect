// gpu_score.mm — Metal implementation of GpuScorer (Objective-C++).
//
// One GPU thread per guess. Each thread partitions the candidate set by the
// precomputed pattern (pm[g*N + a]) into a 243-bucket histogram held in thread
// memory, then reduces it to (max_bucket, entropy). The pattern matrix lives in
// device memory (uploaded once at construction); only the candidate index list
// is uploaded per call.

#include "gpu_score.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <format>

namespace {

// Metal Shading Language source, compiled at runtime (no separate .metal build).
// PATTERN_COUNT = 243 (3^5). Entropy in bits.
constexpr const char* kShaderSrc = R"METAL(
#include <metal_stdlib>
using namespace metal;

constant uint PATTERN_COUNT = 243u;

struct GuessScore {
    uint  max_bucket;
    float entropy;
};

// pmT is the TRANSPOSED pattern matrix: pmT[a*N + g] = pattern(guess g, answer a).
// With one thread per guess g, consecutive threads (g, g+1, …) read consecutive
// addresses pmT[a*N + g] for each candidate a → coalesced device reads.
kernel void score_guesses(
    device const uchar*    pmT       [[buffer(0)]],  // N*N transposed matrix
    device const uint16_t* cand      [[buffer(1)]],  // candidate indices
    constant uint&         n         [[buffer(2)]],  // matrix dimension N
    constant uint&         ncand     [[buffer(3)]],  // candidate count
    device GuessScore*     out       [[buffer(4)]],  // per-guess result
    uint                   gid       [[thread_position_in_grid]])
{
    if (gid >= n) return;

    // Per-thread histogram of 243 buckets.
    ushort hist[PATTERN_COUNT];
    for (uint i = 0; i < PATTERN_COUNT; ++i) hist[i] = 0;

    uint maxb = 0;
    for (uint i = 0; i < ncand; ++i) {
        uint a = (uint)cand[i];
        uchar p = pmT[(ulong)a * (ulong)n + gid];  // coalesced across threads
        ushort c = ++hist[p];
        if (c > maxb) maxb = c;
    }

    // Shannon entropy (bits) over the partition.
    float total = (float)ncand;
    float H = 0.0f;
    for (uint i = 0; i < PATTERN_COUNT; ++i) {
        ushort c = hist[i];
        if (c != 0) {
            float pr = (float)c / total;
            H -= pr * log2(pr);
        }
    }

    out[gid].max_bucket = maxb;
    out[gid].entropy    = H;
}

// Batched variant: score ALL guesses against MANY candidate sets in one
// dispatch. Grid is 2D: x = guess (0..n), y = set (0..nsets). Candidate sets are
// packed contiguously in `cand`, with `offsets[s]..offsets[s+1]` delimiting set
// s. Output is row-major [set][guess]. This amortises dispatch latency across
// the whole frontier of sets at a search level — the regime where per-set
// dispatch would otherwise dominate.
kernel void score_sets(
    device const uchar*    pmT       [[buffer(0)]],  // N*N transposed matrix
    device const uint16_t* cand      [[buffer(1)]],  // all sets' candidates packed
    device const uint*     offsets   [[buffer(2)]],  // nsets+1 prefix offsets
    constant uint&         n         [[buffer(3)]],  // matrix dimension N
    device GuessScore*     out       [[buffer(4)]],  // [nsets * n] row-major
    uint2                  gid       [[thread_position_in_grid]])
{
    const uint g = gid.x;     // guess
    const uint s = gid.y;     // set
    if (g >= n) return;

    const uint begin = offsets[s];
    const uint end   = offsets[s + 1];
    const uint ncand = end - begin;

    ushort hist[PATTERN_COUNT];
    for (uint i = 0; i < PATTERN_COUNT; ++i) hist[i] = 0;

    uint maxb = 0;
    for (uint i = begin; i < end; ++i) {
        uint a = (uint)cand[i];
        uchar p = pmT[(ulong)a * (ulong)n + g];
        ushort c = ++hist[p];
        if (c > maxb) maxb = c;
    }

    float total = (float)ncand;
    float H = 0.0f;
    for (uint i = 0; i < PATTERN_COUNT; ++i) {
        ushort c = hist[i];
        if (c != 0) { float pr = (float)c / total; H -= pr * log2(pr); }
    }

    const ulong oidx = (ulong)s * (ulong)n + g;
    out[oidx].max_bucket = maxb;
    out[oidx].entropy    = H;
}
)METAL";

}  // namespace

namespace wp {

struct GpuScorerImpl {
    id<MTLDevice>              device        = nil;
    id<MTLCommandQueue>        queue         = nil;
    id<MTLComputePipelineState> pipeline     = nil;  // score_guesses (single set)
    id<MTLComputePipelineState> pipeline_sets = nil; // score_sets (batched)
    id<MTLBuffer>             pm_buf         = nil;  // N*N transposed matrix (shared)
    std::uint32_t             n              = 0;
};

std::expected<GpuScorer, std::string>
GpuScorer::create(const Pattern* pattern_matrix, std::uint32_t n) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) return std::unexpected("no Metal device available");

        NSError* err = nil;
        MTLCompileOptions* opts = [[MTLCompileOptions alloc] init];
        id<MTLLibrary> lib =
            [device newLibraryWithSource:[NSString stringWithUTF8String:kShaderSrc]
                                 options:opts
                                   error:&err];
        if (!lib)
            return std::unexpected(std::format("shader compile failed: {}",
                err ? err.localizedDescription.UTF8String : "unknown"));

        id<MTLFunction> fn = [lib newFunctionWithName:@"score_guesses"];
        if (!fn) return std::unexpected("kernel function not found");

        id<MTLComputePipelineState> pipe =
            [device newComputePipelineStateWithFunction:fn error:&err];
        if (!pipe)
            return std::unexpected(std::format("pipeline failed: {}",
                err ? err.localizedDescription.UTF8String : "unknown"));

        id<MTLFunction> fn_sets = [lib newFunctionWithName:@"score_sets"];
        if (!fn_sets) return std::unexpected("score_sets kernel not found");
        id<MTLComputePipelineState> pipe_sets =
            [device newComputePipelineStateWithFunction:fn_sets error:&err];
        if (!pipe_sets)
            return std::unexpected(std::format("score_sets pipeline failed: {}",
                err ? err.localizedDescription.UTF8String : "unknown"));

        // Upload the TRANSPOSED matrix (pmT[a*N+g]) so the kernel's per-guess
        // threads read coalesced. Allocate the device buffer, then transpose
        // directly into its shared-memory contents.
        const std::size_t pm_bytes = static_cast<std::size_t>(n) * n;
        id<MTLBuffer> pm_buf =
            [device newBufferWithLength:pm_bytes options:MTLResourceStorageModeShared];
        if (!pm_buf) return std::unexpected("failed to allocate pattern buffer");
        {
            // dst is the raw Metal device buffer (bytes); the source is Pattern
            // data. Pattern is a byte, so the transposed copy is a byte copy.
            auto* dst = static_cast<std::uint8_t*>(pm_buf.contents);
            for (std::uint32_t g = 0; g < n; ++g) {
                const Pattern* row = pattern_matrix + static_cast<std::size_t>(g) * n;
                for (std::uint32_t a = 0; a < n; ++a)
                    dst[static_cast<std::size_t>(a) * n + g] = row[a];
            }
        }

        auto* impl = new GpuScorerImpl{};
        impl->device        = device;
        impl->queue         = [device newCommandQueue];
        impl->pipeline      = pipe;
        impl->pipeline_sets = pipe_sets;
        impl->pm_buf        = pm_buf;
        impl->n             = n;

        GpuScorer s;
        s.impl_ = impl;
        s.n_    = n;
        return s;
    }
}

GpuScorer::~GpuScorer() {
    if (impl_) {
        auto* impl = static_cast<GpuScorerImpl*>(impl_);
        // ARC releases the held objects when the impl is destroyed; we used
        // strong ObjC refs in a C++ struct, so null them to drop references.
        @autoreleasepool {
            impl->pipeline      = nil;
            impl->pipeline_sets = nil;
            impl->pm_buf        = nil;
            impl->queue         = nil;
            impl->device        = nil;
        }
        delete impl;
        impl_ = nullptr;
    }
}

GpuScorer::GpuScorer(GpuScorer&& o) noexcept : impl_{o.impl_}, n_{o.n_} {
    o.impl_ = nullptr;
}
GpuScorer& GpuScorer::operator=(GpuScorer&& o) noexcept {
    if (this != &o) {
        this->~GpuScorer();
        impl_ = o.impl_; n_ = o.n_; o.impl_ = nullptr;
    }
    return *this;
}

std::expected<std::vector<GpuScorer::GuessScore>, std::string>
GpuScorer::score_all(std::span<const WordIndex> candidates) const {
    auto* impl = static_cast<GpuScorerImpl*>(impl_);
    if (!impl) return std::unexpected("scorer not initialized");
    const std::uint32_t ncand = static_cast<std::uint32_t>(candidates.size());

    // The MSL kernel declares the candidate buffer as uint16_t; WordIndex must
    // stay 16-bit for this upload to match.
    static_assert(sizeof(WordIndex) == sizeof(std::uint16_t));

    @autoreleasepool {
        id<MTLBuffer> cand_buf =
            [impl->device newBufferWithBytes:candidates.data()
                                      length:std::max<std::size_t>(1, ncand * sizeof(WordIndex))
                                     options:MTLResourceStorageModeShared];
        id<MTLBuffer> out_buf =
            [impl->device newBufferWithLength:static_cast<std::size_t>(impl->n) * sizeof(GuessScore)
                                     options:MTLResourceStorageModeShared];
        if (!cand_buf || !out_buf) return std::unexpected("buffer alloc failed");

        id<MTLCommandBuffer> cb = [impl->queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:impl->pipeline];
        [enc setBuffer:impl->pm_buf offset:0 atIndex:0];
        [enc setBuffer:cand_buf     offset:0 atIndex:1];
        [enc setBytes:&impl->n      length:sizeof(std::uint32_t) atIndex:2];
        [enc setBytes:&ncand        length:sizeof(std::uint32_t) atIndex:3];
        [enc setBuffer:out_buf      offset:0 atIndex:4];

        const NSUInteger tew = impl->pipeline.threadExecutionWidth;
        MTLSize grid    = MTLSizeMake(impl->n, 1, 1);
        MTLSize tgroup  = MTLSizeMake(tew, 1, 1);
        [enc dispatchThreads:grid threadsPerThreadgroup:tgroup];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];

        if (cb.status == MTLCommandBufferStatusError)
            return std::unexpected("GPU command buffer error");

        std::vector<GuessScore> out(impl->n);
        std::memcpy(out.data(), out_buf.contents, out.size() * sizeof(GuessScore));
        return out;
    }
}

std::expected<std::vector<GpuScorer::GuessScore>, std::string>
GpuScorer::score_sets(std::span<const WordIndex> packed,
                      std::span<const std::uint32_t> offsets) const {
    auto* impl = static_cast<GpuScorerImpl*>(impl_);
    if (!impl) return std::unexpected("scorer not initialized");
    if (offsets.size() < 2) return std::unexpected("score_sets needs >=1 set");
    static_assert(sizeof(WordIndex) == sizeof(std::uint16_t));

    const std::uint32_t nsets = static_cast<std::uint32_t>(offsets.size() - 1);

    @autoreleasepool {
        id<MTLBuffer> cand_buf =
            [impl->device newBufferWithBytes:packed.data()
                                      length:std::max<std::size_t>(1, packed.size() * sizeof(WordIndex))
                                     options:MTLResourceStorageModeShared];
        id<MTLBuffer> off_buf =
            [impl->device newBufferWithBytes:offsets.data()
                                      length:offsets.size() * sizeof(std::uint32_t)
                                     options:MTLResourceStorageModeShared];
        id<MTLBuffer> out_buf =
            [impl->device newBufferWithLength:
                static_cast<std::size_t>(nsets) * impl->n * sizeof(GuessScore)
                                     options:MTLResourceStorageModeShared];
        if (!cand_buf || !off_buf || !out_buf)
            return std::unexpected("buffer alloc failed");

        id<MTLCommandBuffer> cb = [impl->queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:impl->pipeline_sets];
        [enc setBuffer:impl->pm_buf offset:0 atIndex:0];
        [enc setBuffer:cand_buf     offset:0 atIndex:1];
        [enc setBuffer:off_buf      offset:0 atIndex:2];
        [enc setBytes:&impl->n      length:sizeof(std::uint32_t) atIndex:3];
        [enc setBuffer:out_buf      offset:0 atIndex:4];

        const NSUInteger tew = impl->pipeline_sets.threadExecutionWidth;
        MTLSize grid   = MTLSizeMake(impl->n, nsets, 1);
        MTLSize tgroup = MTLSizeMake(tew, 1, 1);
        [enc dispatchThreads:grid threadsPerThreadgroup:tgroup];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];

        if (cb.status == MTLCommandBufferStatusError)
            return std::unexpected("GPU command buffer error");

        std::vector<GuessScore> out(static_cast<std::size_t>(nsets) * impl->n);
        std::memcpy(out.data(), out_buf.contents, out.size() * sizeof(GuessScore));
        return out;
    }
}

} // namespace wp
