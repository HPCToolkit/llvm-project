//===- SuperVectorize.cpp - Vectorize Pass Impl ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements vectorization of loops, operations and data types to
// a target-independent, n-D super-vector abstraction.
//
//===----------------------------------------------------------------------===//

#include "PassDetail.h"
#include "mlir/Analysis/LoopAnalysis.h"
#include "mlir/Analysis/NestedMatcher.h"
#include "mlir/Analysis/Utils.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Affine/Utils.h"
#include "mlir/Dialect/Vector/VectorOps.h"
#include "mlir/Dialect/Vector/VectorUtils.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/Support/LLVM.h"

using namespace mlir;
using namespace vector;

///
/// Implements a high-level vectorization strategy on a Function.
/// The abstraction used is that of super-vectors, which provide a single,
/// compact, representation in the vector types, information that is expected
/// to reduce the impact of the phase ordering problem
///
/// Vector granularity:
/// ===================
/// This pass is designed to perform vectorization at a super-vector
/// granularity. A super-vector is loosely defined as a vector type that is a
/// multiple of a "good" vector size so the HW can efficiently implement a set
/// of high-level primitives. Multiple is understood along any dimension; e.g.
/// both vector<16xf32> and vector<2x8xf32> are valid super-vectors for a
/// vector<8xf32> HW vector. Note that a "good vector size so the HW can
/// efficiently implement a set of high-level primitives" is not necessarily an
/// integer multiple of actual hardware registers. We leave details of this
/// distinction unspecified for now.
///
/// Some may prefer the terminology a "tile of HW vectors". In this case, one
/// should note that super-vectors implement an "always full tile" abstraction.
/// They guarantee no partial-tile separation is necessary by relying on a
/// high-level copy-reshape abstraction that we call vector.transfer. This
/// copy-reshape operations is also responsible for performing layout
/// transposition if necessary. In the general case this will require a scoped
/// allocation in some notional local memory.
///
/// Whatever the mental model one prefers to use for this abstraction, the key
/// point is that we burn into a single, compact, representation in the vector
/// types, information that is expected to reduce the impact of the phase
/// ordering problem. Indeed, a vector type conveys information that:
///   1. the associated loops have dependency semantics that do not prevent
///      vectorization;
///   2. the associate loops have been sliced in chunks of static sizes that are
///      compatible with vector sizes (i.e. similar to unroll-and-jam);
///   3. the inner loops, in the unroll-and-jam analogy of 2, are captured by
///   the
///      vector type and no vectorization hampering transformations can be
///      applied to them anymore;
///   4. the underlying memrefs are accessed in some notional contiguous way
///      that allows loading into vectors with some amount of spatial locality;
/// In other words, super-vectorization provides a level of separation of
/// concern by way of opacity to subsequent passes. This has the effect of
/// encapsulating and propagating vectorization constraints down the list of
/// passes until we are ready to lower further.
///
/// For a particular target, a notion of minimal n-d vector size will be
/// specified and vectorization targets a multiple of those. In the following
/// paragraph, let "k ." represent "a multiple of", to be understood as a
/// multiple in the same dimension (e.g. vector<16 x k . 128> summarizes
/// vector<16 x 128>, vector<16 x 256>, vector<16 x 1024>, etc).
///
/// Some non-exhaustive notable super-vector sizes of interest include:
///   - CPU: vector<k . HW_vector_size>,
///          vector<k' . core_count x k . HW_vector_size>,
///          vector<socket_count x k' . core_count x k . HW_vector_size>;
///   - GPU: vector<k . warp_size>,
///          vector<k . warp_size x float2>,
///          vector<k . warp_size x float4>,
///          vector<k . warp_size x 4 x 4x 4> (for tensor_core sizes).
///
/// Loops and operations are emitted that operate on those super-vector shapes.
/// Subsequent lowering passes will materialize to actual HW vector sizes. These
/// passes are expected to be (gradually) more target-specific.
///
/// At a high level, a vectorized load in a loop will resemble:
/// ```mlir
///   affine.for %i = ? to ? step ? {
///     %v_a = vector.transfer_read A[%i] : memref<?xf32>, vector<128xf32>
///   }
/// ```
/// It is the responsibility of the implementation of vector.transfer_read to
/// materialize vector registers from the original scalar memrefs. A later (more
/// target-dependent) lowering pass will materialize to actual HW vector sizes.
/// This lowering may be occur at different times:
///   1. at the MLIR level into a combination of loops, unrolling, DmaStartOp +
///      DmaWaitOp + vectorized operations for data transformations and shuffle;
///      thus opening opportunities for unrolling and pipelining. This is an
///      instance of library call "whiteboxing"; or
///   2. later in the a target-specific lowering pass or hand-written library
///      call; achieving full separation of concerns. This is an instance of
///      library call; or
///   3. a mix of both, e.g. based on a model.
/// In the future, these operations will expose a contract to constrain the
/// search on vectorization patterns and sizes.
///
/// Occurrence of super-vectorization in the compiler flow:
/// =======================================================
/// This is an active area of investigation. We start with 2 remarks to position
/// super-vectorization in the context of existing ongoing work: LLVM VPLAN
/// and LLVM SLP Vectorizer.
///
/// LLVM VPLAN:
/// -----------
/// The astute reader may have noticed that in the limit, super-vectorization
/// can be applied at a similar time and with similar objectives than VPLAN.
/// For instance, in the case of a traditional, polyhedral compilation-flow (for
/// instance, the PPCG project uses ISL to provide dependence analysis,
/// multi-level(scheduling + tiling), lifting footprint to fast memory,
/// communication synthesis, mapping, register optimizations) and before
/// unrolling. When vectorization is applied at this *late* level in a typical
/// polyhedral flow, and is instantiated with actual hardware vector sizes,
/// super-vectorization is expected to match (or subsume) the type of patterns
/// that LLVM's VPLAN aims at targeting. The main difference here is that MLIR
/// is higher level and our implementation should be significantly simpler. Also
/// note that in this mode, recursive patterns are probably a bit of an overkill
/// although it is reasonable to expect that mixing a bit of outer loop and
/// inner loop vectorization + unrolling will provide interesting choices to
/// MLIR.
///
/// LLVM SLP Vectorizer:
/// --------------------
/// Super-vectorization however is not meant to be usable in a similar fashion
/// to the SLP vectorizer. The main difference lies in the information that
/// both vectorizers use: super-vectorization examines contiguity of memory
/// references along fastest varying dimensions and loops with recursive nested
/// patterns capturing imperfectly-nested loop nests; the SLP vectorizer, on
/// the other hand, performs flat pattern matching inside a single unrolled loop
/// body and stitches together pieces of load and store operations into full
/// 1-D vectors. We envision that the SLP vectorizer is a good way to capture
/// innermost loop, control-flow dependent patterns that super-vectorization may
/// not be able to capture easily. In other words, super-vectorization does not
/// aim at replacing the SLP vectorizer and the two solutions are complementary.
///
/// Ongoing investigations:
/// -----------------------
/// We discuss the following *early* places where super-vectorization is
/// applicable and touch on the expected benefits and risks . We list the
/// opportunities in the context of the traditional polyhedral compiler flow
/// described in PPCG. There are essentially 6 places in the MLIR pass pipeline
/// we expect to experiment with super-vectorization:
/// 1. Right after language lowering to MLIR: this is the earliest time where
///    super-vectorization is expected to be applied. At this level, all the
///    language/user/library-level annotations are available and can be fully
///    exploited. Examples include loop-type annotations (such as parallel,
///    reduction, scan, dependence distance vector, vectorizable) as well as
///    memory access annotations (such as non-aliasing writes guaranteed,
///    indirect accesses that are permutations by construction) accesses or
///    that a particular operation is prescribed atomic by the user. At this
///    level, anything that enriches what dependence analysis can do should be
///    aggressively exploited. At this level we are close to having explicit
///    vector types in the language, except we do not impose that burden on the
///    programmer/library: we derive information from scalar code + annotations.
/// 2. After dependence analysis and before polyhedral scheduling: the
///    information that supports vectorization does not need to be supplied by a
///    higher level of abstraction. Traditional dependence analysis is available
///    in MLIR and will be used to drive vectorization and cost models.
///
/// Let's pause here and remark that applying super-vectorization as described
/// in 1. and 2. presents clear opportunities and risks:
///   - the opportunity is that vectorization is burned in the type system and
///   is protected from the adverse effect of loop scheduling, tiling, loop
///   interchange and all passes downstream. Provided that subsequent passes are
///   able to operate on vector types; the vector shapes, associated loop
///   iterator properties, alignment, and contiguity of fastest varying
///   dimensions are preserved until we lower the super-vector types. We expect
///   this to significantly rein in on the adverse effects of phase ordering.
///   - the risks are that a. all passes after super-vectorization have to work
///   on elemental vector types (not that this is always true, wherever
///   vectorization is applied) and b. that imposing vectorization constraints
///   too early may be overall detrimental to loop fusion, tiling and other
///   transformations because the dependence distances are coarsened when
///   operating on elemental vector types. For this reason, the pattern
///   profitability analysis should include a component that also captures the
///   maximal amount of fusion available under a particular pattern. This is
///   still at the stage of rough ideas but in this context, search is our
///   friend as the Tensor Comprehensions and auto-TVM contributions
///   demonstrated previously.
/// Bottom-line is we do not yet have good answers for the above but aim at
/// making it easy to answer such questions.
///
/// Back to our listing, the last places where early super-vectorization makes
/// sense are:
/// 3. right after polyhedral-style scheduling: PLUTO-style algorithms are known
///    to improve locality, parallelism and be configurable (e.g. max-fuse,
///    smart-fuse etc). They can also have adverse effects on contiguity
///    properties that are required for vectorization but the vector.transfer
///    copy-reshape-pad-transpose abstraction is expected to help recapture
///    these properties.
/// 4. right after polyhedral-style scheduling+tiling;
/// 5. right after scheduling+tiling+rescheduling: points 4 and 5 represent
///    probably the most promising places because applying tiling achieves a
///    separation of concerns that allows rescheduling to worry less about
///    locality and more about parallelism and distribution (e.g. min-fuse).
///
/// At these levels the risk-reward looks different: on one hand we probably
/// lost a good deal of language/user/library-level annotation; on the other
/// hand we gained parallelism and locality through scheduling and tiling.
/// However we probably want to ensure tiling is compatible with the
/// full-tile-only abstraction used in super-vectorization or suffer the
/// consequences. It is too early to place bets on what will win but we expect
/// super-vectorization to be the right abstraction to allow exploring at all
/// these levels. And again, search is our friend.
///
/// Lastly, we mention it again here:
/// 6. as a MLIR-based alternative to VPLAN.
///
/// Lowering, unrolling, pipelining:
/// ================================
/// TODO: point to the proper places.
///
/// Algorithm:
/// ==========
/// The algorithm proceeds in a few steps:
///  1. defining super-vectorization patterns and matching them on the tree of
///     AffineForOp. A super-vectorization pattern is defined as a recursive
///     data structures that matches and captures nested, imperfectly-nested
///     loops that have a. conformable loop annotations attached (e.g. parallel,
///     reduction, vectorizable, ...) as well as b. all contiguous load/store
///     operations along a specified minor dimension (not necessarily the
///     fastest varying) ;
///  2. analyzing those patterns for profitability (TODO: and
///     interference);
///  3. then, for each pattern in order:
///    a. applying iterative rewriting of the loops and all their nested
///       operations in topological order. Rewriting is implemented by
///       coarsening the loops and converting operations and operands to their
///       vector forms. Processing operations in topological order is relatively
///       simple due to the structured nature of the control-flow
///       representation. This order ensures that all the operands of a given
///       operation have been vectorized before the operation itself in a single
///       traversal, except for operands defined outside of the loop nest. The
///       algorithm can convert the following operations to their vector form:
///         * Affine load and store operations are converted to opaque vector
///           transfer read and write operations.
///         * Scalar constant operations/operands are converted to vector
///           constant operations (splat).
///         * Uniform operands (only operands defined outside of the loop nest,
///           for now) are broadcasted to a vector.
///           TODO: Support more uniform cases.
///         * The remaining operations in the loop nest are vectorized by
///           widening their scalar types to vector types.
///         * TODO: Add vectorization support for loops with 'iter_args' and
///           more complex loops with divergent lbs and/or ubs.
///    b. if everything under the root AffineForOp in the current pattern
///       is vectorized properly, we commit that loop to the IR and remove the
///       scalar loop. Otherwise, we discard the vectorized loop and keep the
///       original scalar loop.
///    c. vectorization is applied on the next pattern in the list. Because
///       pattern interference avoidance is not yet implemented and that we do
///       not support further vectorizing an already vector load we need to
///       re-verify that the pattern is still vectorizable. This is expected to
///       make cost models more difficult to write and is subject to improvement
///       in the future.
///
/// Choice of loop transformation to support the algorithm:
/// =======================================================
/// The choice of loop transformation to apply for coarsening vectorized loops
/// is still subject to exploratory tradeoffs. In particular, say we want to
/// vectorize by a factor 128, we want to transform the following input:
/// ```mlir
///   affine.for %i = %M to %N {
///     %a = affine.load %A[%i] : memref<?xf32>
///   }
/// ```
///
/// Traditionally, one would vectorize late (after scheduling, tiling,
/// memory promotion etc) say after stripmining (and potentially unrolling in
/// the case of LLVM's SLP vectorizer):
/// ```mlir
///   affine.for %i = floor(%M, 128) to ceil(%N, 128) {
///     affine.for %ii = max(%M, 128 * %i) to min(%N, 128*%i + 127) {
///       %a = affine.load %A[%ii] : memref<?xf32>
///     }
///   }
/// ```
///
/// Instead, we seek to vectorize early and freeze vector types before
/// scheduling, so we want to generate a pattern that resembles:
/// ```mlir
///   affine.for %i = ? to ? step ? {
///     %v_a = vector.transfer_read %A[%i] : memref<?xf32>, vector<128xf32>
///   }
/// ```
///
/// i. simply dividing the lower / upper bounds by 128 creates issues
///    when representing expressions such as ii + 1 because now we only
///    have access to original values that have been divided. Additional
///    information is needed to specify accesses at below-128 granularity;
/// ii. another alternative is to coarsen the loop step but this may have
///    consequences on dependence analysis and fusability of loops: fusable
///    loops probably need to have the same step (because we don't want to
///    stripmine/unroll to enable fusion).
/// As a consequence, we choose to represent the coarsening using the loop
/// step for now and reevaluate in the future. Note that we can renormalize
/// loop steps later if/when we have evidence that they are problematic.
///
/// For the simple strawman example above, vectorizing for a 1-D vector
/// abstraction of size 128 returns code similar to:
/// ```mlir
///   affine.for %i = %M to %N step 128 {
///     %v_a = vector.transfer_read %A[%i] : memref<?xf32>, vector<128xf32>
///   }
/// ```
///
/// Unsupported cases, extensions, and work in progress (help welcome :-) ):
/// ========================================================================
///   1. lowering to concrete vector types for various HW;
///   2. reduction support;
///   3. non-effecting padding during vector.transfer_read and filter during
///      vector.transfer_write;
///   4. misalignment support vector.transfer_read / vector.transfer_write
///      (hopefully without read-modify-writes);
///   5. control-flow support;
///   6. cost-models, heuristics and search;
///   7. Op implementation, extensions and implication on memref views;
///   8. many TODOs left around.
///
/// Examples:
/// =========
/// Consider the following Function:
/// ```mlir
/// func @vector_add_2d(%M : index, %N : index) -> f32 {
///   %A = alloc (%M, %N) : memref<?x?xf32, 0>
///   %B = alloc (%M, %N) : memref<?x?xf32, 0>
///   %C = alloc (%M, %N) : memref<?x?xf32, 0>
///   %f1 = constant 1.0 : f32
///   %f2 = constant 2.0 : f32
///   affine.for %i0 = 0 to %M {
///     affine.for %i1 = 0 to %N {
///       // non-scoped %f1
///       affine.store %f1, %A[%i0, %i1] : memref<?x?xf32, 0>
///     }
///   }
///   affine.for %i2 = 0 to %M {
///     affine.for %i3 = 0 to %N {
///       // non-scoped %f2
///       affine.store %f2, %B[%i2, %i3] : memref<?x?xf32, 0>
///     }
///   }
///   affine.for %i4 = 0 to %M {
///     affine.for %i5 = 0 to %N {
///       %a5 = affine.load %A[%i4, %i5] : memref<?x?xf32, 0>
///       %b5 = affine.load %B[%i4, %i5] : memref<?x?xf32, 0>
///       %s5 = addf %a5, %b5 : f32
///       // non-scoped %f1
///       %s6 = addf %s5, %f1 : f32
///       // non-scoped %f2
///       %s7 = addf %s5, %f2 : f32
///       // diamond dependency.
///       %s8 = addf %s7, %s6 : f32
///       affine.store %s8, %C[%i4, %i5] : memref<?x?xf32, 0>
///     }
///   }
///   %c7 = constant 7 : index
///   %c42 = constant 42 : index
///   %res = load %C[%c7, %c42] : memref<?x?xf32, 0>
///   return %res : f32
/// }
/// ```
///
/// The -affine-vectorize pass with the following arguments:
/// ```
/// -affine-vectorize="virtual-vector-size=256 test-fastest-varying=0"
/// ```
///
/// produces this standard innermost-loop vectorized code:
/// ```mlir
/// func @vector_add_2d(%arg0 : index, %arg1 : index) -> f32 {
///   %0 = alloc(%arg0, %arg1) : memref<?x?xf32>
///   %1 = alloc(%arg0, %arg1) : memref<?x?xf32>
///   %2 = alloc(%arg0, %arg1) : memref<?x?xf32>
///   %cst = constant 1.0 : f32
///   %cst_0 = constant 2.0 : f32
///   affine.for %i0 = 0 to %arg0 {
///     affine.for %i1 = 0 to %arg1 step 256 {
///       %cst_1 = constant dense<vector<256xf32>, 1.0> :
///                vector<256xf32>
///       vector.transfer_write %cst_1, %0[%i0, %i1] :
///                vector<256xf32>, memref<?x?xf32>
///     }
///   }
///   affine.for %i2 = 0 to %arg0 {
///     affine.for %i3 = 0 to %arg1 step 256 {
///       %cst_2 = constant dense<vector<256xf32>, 2.0> :
///                vector<256xf32>
///       vector.transfer_write %cst_2, %1[%i2, %i3] :
///                vector<256xf32>, memref<?x?xf32>
///     }
///   }
///   affine.for %i4 = 0 to %arg0 {
///     affine.for %i5 = 0 to %arg1 step 256 {
///       %3 = vector.transfer_read %0[%i4, %i5] :
///            memref<?x?xf32>, vector<256xf32>
///       %4 = vector.transfer_read %1[%i4, %i5] :
///            memref<?x?xf32>, vector<256xf32>
///       %5 = addf %3, %4 : vector<256xf32>
///       %cst_3 = constant dense<vector<256xf32>, 1.0> :
///                vector<256xf32>
///       %6 = addf %5, %cst_3 : vector<256xf32>
///       %cst_4 = constant dense<vector<256xf32>, 2.0> :
///                vector<256xf32>
///       %7 = addf %5, %cst_4 : vector<256xf32>
///       %8 = addf %7, %6 : vector<256xf32>
///       vector.transfer_write %8, %2[%i4, %i5] :
///                vector<256xf32>, memref<?x?xf32>
///     }
///   }
///   %c7 = constant 7 : index
///   %c42 = constant 42 : index
///   %9 = load %2[%c7, %c42] : memref<?x?xf32>
///   return %9 : f32
/// }
/// ```
///
/// The -affine-vectorize pass with the following arguments:
/// ```
/// -affine-vectorize="virtual-vector-size=32,256 test-fastest-varying=1,0"
/// ```
///
/// produces this more interesting mixed outer-innermost-loop vectorized code:
/// ```mlir
/// func @vector_add_2d(%arg0 : index, %arg1 : index) -> f32 {
///   %0 = alloc(%arg0, %arg1) : memref<?x?xf32>
///   %1 = alloc(%arg0, %arg1) : memref<?x?xf32>
///   %2 = alloc(%arg0, %arg1) : memref<?x?xf32>
///   %cst = constant 1.0 : f32
///   %cst_0 = constant 2.0 : f32
///   affine.for %i0 = 0 to %arg0 step 32 {
///     affine.for %i1 = 0 to %arg1 step 256 {
///       %cst_1 = constant dense<vector<32x256xf32>, 1.0> :
///                vector<32x256xf32>
///       vector.transfer_write %cst_1, %0[%i0, %i1] :
///                vector<32x256xf32>, memref<?x?xf32>
///     }
///   }
///   affine.for %i2 = 0 to %arg0 step 32 {
///     affine.for %i3 = 0 to %arg1 step 256 {
///       %cst_2 = constant dense<vector<32x256xf32>, 2.0> :
///                vector<32x256xf32>
///       vector.transfer_write %cst_2, %1[%i2, %i3] :
///                vector<32x256xf32>, memref<?x?xf32>
///     }
///   }
///   affine.for %i4 = 0 to %arg0 step 32 {
///     affine.for %i5 = 0 to %arg1 step 256 {
///       %3 = vector.transfer_read %0[%i4, %i5] :
///                memref<?x?xf32> vector<32x256xf32>
///       %4 = vector.transfer_read %1[%i4, %i5] :
///                memref<?x?xf32>, vector<32x256xf32>
///       %5 = addf %3, %4 : vector<32x256xf32>
///       %cst_3 = constant dense<vector<32x256xf32>, 1.0> :
///                vector<32x256xf32>
///       %6 = addf %5, %cst_3 : vector<32x256xf32>
///       %cst_4 = constant dense<vector<32x256xf32>, 2.0> :
///                vector<32x256xf32>
///       %7 = addf %5, %cst_4 : vector<32x256xf32>
///       %8 = addf %7, %6 : vector<32x256xf32>
///       vector.transfer_write %8, %2[%i4, %i5] :
///                vector<32x256xf32>, memref<?x?xf32>
///     }
///   }
///   %c7 = constant 7 : index
///   %c42 = constant 42 : index
///   %9 = load %2[%c7, %c42] : memref<?x?xf32>
///   return %9 : f32
/// }
/// ```
///
/// Of course, much more intricate n-D imperfectly-nested patterns can be
/// vectorized too and specified in a fully declarative fashion.

#define DEBUG_TYPE "early-vect"

using llvm::dbgs;

/// Forward declaration.
static FilterFunctionType
isVectorizableLoopPtrFactory(const DenseSet<Operation *> &parallelLoops,
                             int fastestVaryingMemRefDimension);

/// Creates a vectorization pattern from the command line arguments.
/// Up to 3-D patterns are supported.
/// If the command line argument requests a pattern of higher order, returns an
/// empty pattern list which will conservatively result in no vectorization.
static Optional<NestedPattern>
makePattern(const DenseSet<Operation *> &parallelLoops, int vectorRank,
            ArrayRef<int64_t> fastestVaryingPattern) {
  using matcher::For;
  int64_t d0 = fastestVaryingPattern.empty() ? -1 : fastestVaryingPattern[0];
  int64_t d1 = fastestVaryingPattern.size() < 2 ? -1 : fastestVaryingPattern[1];
  int64_t d2 = fastestVaryingPattern.size() < 3 ? -1 : fastestVaryingPattern[2];
  switch (vectorRank) {
  case 1:
    return For(isVectorizableLoopPtrFactory(parallelLoops, d0));
  case 2:
    return For(isVectorizableLoopPtrFactory(parallelLoops, d0),
               For(isVectorizableLoopPtrFactory(parallelLoops, d1)));
  case 3:
    return For(isVectorizableLoopPtrFactory(parallelLoops, d0),
               For(isVectorizableLoopPtrFactory(parallelLoops, d1),
                   For(isVectorizableLoopPtrFactory(parallelLoops, d2))));
  default: {
    return llvm::None;
  }
  }
}

static NestedPattern &vectorTransferPattern() {
  static auto pattern = matcher::Op([](Operation &op) {
    return isa<vector::TransferReadOp, vector::TransferWriteOp>(op);
  });
  return pattern;
}

namespace {

/// Base state for the vectorize pass.
/// Command line arguments are preempted by non-empty pass arguments.
struct Vectorize : public AffineVectorizeBase<Vectorize> {
  Vectorize() = default;
  Vectorize(ArrayRef<int64_t> virtualVectorSize);
  void runOnFunction() override;
};

} // end anonymous namespace

Vectorize::Vectorize(ArrayRef<int64_t> virtualVectorSize) {
  vectorSizes = virtualVectorSize;
}

static void vectorizeLoopIfProfitable(Operation *loop, unsigned depthInPattern,
                                      unsigned patternDepth,
                                      VectorizationStrategy *strategy) {
  assert(patternDepth > depthInPattern &&
         "patternDepth is greater than depthInPattern");
  if (patternDepth - depthInPattern > strategy->vectorSizes.size()) {
    // Don't vectorize this loop
    return;
  }
  strategy->loopToVectorDim[loop] =
      strategy->vectorSizes.size() - (patternDepth - depthInPattern);
}

/// Implements a simple strawman strategy for vectorization.
/// Given a matched pattern `matches` of depth `patternDepth`, this strategy
/// greedily assigns the fastest varying dimension ** of the vector ** to the
/// innermost loop in the pattern.
/// When coupled with a pattern that looks for the fastest varying dimension in
/// load/store MemRefs, this creates a generic vectorization strategy that works
/// for any loop in a hierarchy (outermost, innermost or intermediate).
///
/// TODO: In the future we should additionally increase the power of the
/// profitability analysis along 3 directions:
///   1. account for loop extents (both static and parametric + annotations);
///   2. account for data layout permutations;
///   3. account for impact of vectorization on maximal loop fusion.
/// Then we can quantify the above to build a cost model and search over
/// strategies.
static LogicalResult analyzeProfitability(ArrayRef<NestedMatch> matches,
                                          unsigned depthInPattern,
                                          unsigned patternDepth,
                                          VectorizationStrategy *strategy) {
  for (auto m : matches) {
    if (failed(analyzeProfitability(m.getMatchedChildren(), depthInPattern + 1,
                                    patternDepth, strategy))) {
      return failure();
    }
    vectorizeLoopIfProfitable(m.getMatchedOperation(), depthInPattern,
                              patternDepth, strategy);
  }
  return success();
}

///// end TODO: Hoist to a VectorizationStrategy.cpp when appropriate /////

namespace {

struct VectorizationState {

  VectorizationState(MLIRContext *context) : builder(context) {}

  /// Registers the vector replacement of a scalar operation and its result
  /// values. Both operations must have the same number of results.
  ///
  /// This utility is used to register the replacement for the vast majority of
  /// the vectorized operations.
  ///
  /// Example:
  ///   * 'replaced': %0 = addf %1, %2 : f32
  ///   * 'replacement': %0 = addf %1, %2 : vector<128xf32>
  void registerOpVectorReplacement(Operation *replaced, Operation *replacement);

  /// Registers the vector replacement of a scalar value. The replacement
  /// operation should have a single result, which replaces the scalar value.
  ///
  /// This utility is used to register the vector replacement of block arguments
  /// and operation results which are not directly vectorized (i.e., their
  /// scalar version still exists after vectorization), like uniforms.
  ///
  /// Example:
  ///   * 'replaced': block argument or operation outside of the vectorized
  ///     loop.
  ///   * 'replacement': %0 = vector.broadcast %1 : f32 to vector<128xf32>
  void registerValueVectorReplacement(Value replaced, Operation *replacement);

  /// Registers the scalar replacement of a scalar value. 'replacement' must be
  /// scalar. Both values must be block arguments. Operation results should be
  /// replaced using the 'registerOp*' utilitites.
  ///
  /// This utility is used to register the replacement of block arguments
  /// that are within the loop to be vectorized and will continue being scalar
  /// within the vector loop.
  ///
  /// Example:
  ///   * 'replaced': induction variable of a loop to be vectorized.
  ///   * 'replacement': new induction variable in the new vector loop.
  void registerValueScalarReplacement(BlockArgument replaced,
                                      BlockArgument replacement);

  /// Returns in 'replacedVals' the scalar replacement for values in
  /// 'inputVals'.
  void getScalarValueReplacementsFor(ValueRange inputVals,
                                     SmallVectorImpl<Value> &replacedVals);

  /// Erases the scalar loop nest after its successful vectorization.
  void finishVectorizationPattern(AffineForOp rootLoop);

  // Used to build and insert all the new operations created. The insertion
  // point is preserved and updated along the vectorization process.
  OpBuilder builder;

  // Maps input scalar operations to their vector counterparts.
  DenseMap<Operation *, Operation *> opVectorReplacement;
  // Maps input scalar values to their vector counterparts.
  BlockAndValueMapping valueVectorReplacement;
  // Maps input scalar values to their new scalar counterparts in the vector
  // loop nest.
  BlockAndValueMapping valueScalarReplacement;

  // Maps the newly created vector loops to their vector dimension.
  DenseMap<Operation *, unsigned> vecLoopToVecDim;

  // The strategy drives which loop to vectorize by which amount.
  const VectorizationStrategy *strategy;

private:
  /// Internal implementation to map input scalar values to new vector or scalar
  /// values.
  void registerValueVectorReplacementImpl(Value replaced, Value replacement);
  void registerValueScalarReplacementImpl(Value replaced, Value replacement);
};

} // end namespace

/// Registers the vector replacement of a scalar operation and its result
/// values. Both operations must have the same number of results.
///
/// This utility is used to register the replacement for the vast majority of
/// the vectorized operations.
///
/// Example:
///   * 'replaced': %0 = addf %1, %2 : f32
///   * 'replacement': %0 = addf %1, %2 : vector<128xf32>
void VectorizationState::registerOpVectorReplacement(Operation *replaced,
                                                     Operation *replacement) {
  LLVM_DEBUG(dbgs() << "\n[early-vect]+++++ commit vectorized op:\n");
  LLVM_DEBUG(dbgs() << *replaced << "\n");
  LLVM_DEBUG(dbgs() << "into\n");
  LLVM_DEBUG(dbgs() << *replacement << "\n");

  assert(replaced->getNumResults() <= 1 && "Unsupported multi-result op");
  assert(replaced->getNumResults() == replacement->getNumResults() &&
         "Unexpected replaced and replacement results");
  assert(opVectorReplacement.count(replaced) == 0 && "already registered");
  opVectorReplacement[replaced] = replacement;

  if (replaced->getNumResults() > 0)
    registerValueVectorReplacementImpl(replaced->getResult(0),
                                       replacement->getResult(0));
}

/// Registers the vector replacement of a scalar value. The replacement
/// operation should have a single result, which replaces the scalar value.
///
/// This utility is used to register the vector replacement of block arguments
/// and operation results which are not directly vectorized (i.e., their
/// scalar version still exists after vectorization), like uniforms.
///
/// Example:
///   * 'replaced': block argument or operation outside of the vectorized loop.
///   * 'replacement': %0 = vector.broadcast %1 : f32 to vector<128xf32>
void VectorizationState::registerValueVectorReplacement(
    Value replaced, Operation *replacement) {
  assert(replacement->getNumResults() == 1 &&
         "Expected single-result replacement");
  if (Operation *defOp = replaced.getDefiningOp())
    registerOpVectorReplacement(defOp, replacement);
  else
    registerValueVectorReplacementImpl(replaced, replacement->getResult(0));
}

void VectorizationState::registerValueVectorReplacementImpl(Value replaced,
                                                            Value replacement) {
  assert(!valueVectorReplacement.contains(replaced) &&
         "Vector replacement already registered");
  assert(replacement.getType().isa<VectorType>() &&
         "Expected vector type in vector replacement");
  valueVectorReplacement.map(replaced, replacement);
}

/// Registers the scalar replacement of a scalar value. 'replacement' must be
/// scalar. Both values must be block arguments. Operation results should be
/// replaced using the 'registerOp*' utilitites.
///
/// This utility is used to register the replacement of block arguments
/// that are within the loop to be vectorized and will continue being scalar
/// within the vector loop.
///
/// Example:
///   * 'replaced': induction variable of a loop to be vectorized.
///   * 'replacement': new induction variable in the new vector loop.
void VectorizationState::registerValueScalarReplacement(
    BlockArgument replaced, BlockArgument replacement) {
  registerValueScalarReplacementImpl(replaced, replacement);
}

void VectorizationState::registerValueScalarReplacementImpl(Value replaced,
                                                            Value replacement) {
  assert(!valueScalarReplacement.contains(replaced) &&
         "Scalar value replacement already registered");
  assert(!replacement.getType().isa<VectorType>() &&
         "Expected scalar type in scalar replacement");
  valueScalarReplacement.map(replaced, replacement);
}

/// Returns in 'replacedVals' the scalar replacement for values in 'inputVals'.
void VectorizationState::getScalarValueReplacementsFor(
    ValueRange inputVals, SmallVectorImpl<Value> &replacedVals) {
  for (Value inputVal : inputVals)
    replacedVals.push_back(valueScalarReplacement.lookupOrDefault(inputVal));
}

/// Erases a loop nest, including all its nested operations.
static void eraseLoopNest(AffineForOp forOp) {
  LLVM_DEBUG(dbgs() << "[early-vect]+++++ erasing:\n" << forOp << "\n");
  forOp.erase();
}

/// Erases the scalar loop nest after its successful vectorization.
void VectorizationState::finishVectorizationPattern(AffineForOp rootLoop) {
  LLVM_DEBUG(dbgs() << "\n[early-vect] Finalizing vectorization\n");
  eraseLoopNest(rootLoop);
}

// Apply 'map' with 'mapOperands' returning resulting values in 'results'.
static void computeMemoryOpIndices(Operation *op, AffineMap map,
                                   ValueRange mapOperands,
                                   VectorizationState &state,
                                   SmallVectorImpl<Value> &results) {
  for (auto resultExpr : map.getResults()) {
    auto singleResMap =
        AffineMap::get(map.getNumDims(), map.getNumSymbols(), resultExpr);
    auto afOp = state.builder.create<AffineApplyOp>(op->getLoc(), singleResMap,
                                                    mapOperands);
    results.push_back(afOp);
  }
}

/// Returns a FilterFunctionType that can be used in NestedPattern to match a
/// loop whose underlying load/store accesses are either invariant or all
// varying along the `fastestVaryingMemRefDimension`.
static FilterFunctionType
isVectorizableLoopPtrFactory(const DenseSet<Operation *> &parallelLoops,
                             int fastestVaryingMemRefDimension) {
  return [&parallelLoops, fastestVaryingMemRefDimension](Operation &forOp) {
    auto loop = cast<AffineForOp>(forOp);
    auto parallelIt = parallelLoops.find(loop);
    if (parallelIt == parallelLoops.end())
      return false;
    int memRefDim = -1;
    auto vectorizableBody =
        isVectorizableLoopBody(loop, &memRefDim, vectorTransferPattern());
    if (!vectorizableBody)
      return false;
    return memRefDim == -1 || fastestVaryingMemRefDimension == -1 ||
           memRefDim == fastestVaryingMemRefDimension;
  };
}

/// Returns the vector type resulting from applying the provided vectorization
/// strategy on the scalar type.
static VectorType getVectorType(Type scalarTy,
                                const VectorizationStrategy *strategy) {
  assert(!scalarTy.isa<VectorType>() && "Expected scalar type");
  return VectorType::get(strategy->vectorSizes, scalarTy);
}

/// Tries to transform a scalar constant into a vector constant. Returns the
/// vector constant if the scalar type is valid vector element type. Returns
/// nullptr, otherwise.
static ConstantOp vectorizeConstant(ConstantOp constOp,
                                    VectorizationState &state) {
  Type scalarTy = constOp.getType();
  if (!VectorType::isValidElementType(scalarTy))
    return nullptr;

  auto vecTy = getVectorType(scalarTy, state.strategy);
  auto vecAttr = DenseElementsAttr::get(vecTy, constOp.getValue());
  auto newConstOp = state.builder.create<ConstantOp>(constOp.getLoc(), vecAttr);

  // Register vector replacement for future uses in the scope.
  state.registerOpVectorReplacement(constOp, newConstOp);
  return newConstOp;
}

/// Returns true if the provided value is vector uniform given the vectorization
/// strategy.
// TODO: For now, only values that are invariants to all the loops in the
// vectorization strategy are considered vector uniforms.
static bool isUniformDefinition(Value value,
                                const VectorizationStrategy *strategy) {
  for (auto loopToDim : strategy->loopToVectorDim) {
    auto loop = cast<AffineForOp>(loopToDim.first);
    if (!loop.isDefinedOutsideOfLoop(value))
      return false;
  }
  return true;
}

/// Generates a broadcast op for the provided uniform value using the
/// vectorization strategy in 'state'.
static Operation *vectorizeUniform(Value uniformVal,
                                   VectorizationState &state) {
  OpBuilder::InsertionGuard guard(state.builder);
  state.builder.setInsertionPointAfterValue(uniformVal);

  auto vectorTy = getVectorType(uniformVal.getType(), state.strategy);
  auto bcastOp = state.builder.create<BroadcastOp>(uniformVal.getLoc(),
                                                   vectorTy, uniformVal);
  state.registerValueVectorReplacement(uniformVal, bcastOp);
  return bcastOp;
}

/// Tries to vectorize a given `operand` by applying the following logic:
/// 1. if the defining operation has been already vectorized, `operand` is
///    already in the proper vector form;
/// 2. if the `operand` is a constant, returns the vectorized form of the
///    constant;
/// 3. if the `operand` is uniform, returns a vector broadcast of the `op`;
/// 4. otherwise, the vectorization of `operand` is not supported.
/// Newly created vector operations are registered in `state` as replacement
/// for their scalar counterparts.
/// In particular this logic captures some of the use cases where definitions
/// that are not scoped under the current pattern are needed to vectorize.
/// One such example is top level function constants that need to be splatted.
///
/// Returns an operand that has been vectorized to match `state`'s strategy if
/// vectorization is possible with the above logic. Returns nullptr otherwise.
///
/// TODO: handle more complex cases.
static Value vectorizeOperand(Value operand, VectorizationState &state) {
  LLVM_DEBUG(dbgs() << "\n[early-vect]+++++ vectorize operand: " << operand);
  // If this value is already vectorized, we are done.
  if (Value vecRepl = state.valueVectorReplacement.lookupOrNull(operand)) {
    LLVM_DEBUG(dbgs() << " -> already vectorized: " << vecRepl);
    return vecRepl;
  }

  // An vector operand that is not in the replacement map should never reach
  // this point. Reaching this point could mean that the code was already
  // vectorized and we shouldn't try to vectorize already vectorized code.
  assert(!operand.getType().isa<VectorType>() &&
         "Vector op not found in replacement map");

  // Vectorize constant.
  if (auto constOp = operand.getDefiningOp<ConstantOp>()) {
    ConstantOp vecConstant = vectorizeConstant(constOp, state);
    LLVM_DEBUG(dbgs() << "-> constant: " << vecConstant);
    return vecConstant.getResult();
  }

  // Vectorize uniform values.
  if (isUniformDefinition(operand, state.strategy)) {
    Operation *vecUniform = vectorizeUniform(operand, state);
    LLVM_DEBUG(dbgs() << "-> uniform: " << *vecUniform);
    return vecUniform->getResult(0);
  }

  // Check for unsupported block argument scenarios. A supported block argument
  // should have been vectorized already.
  if (!operand.getDefiningOp())
    LLVM_DEBUG(dbgs() << "-> unsupported block argument\n");
  else
    // Generic unsupported case.
    LLVM_DEBUG(dbgs() << "-> non-vectorizable\n");

  return nullptr;
}

/// Vectorizes an affine load with the vectorization strategy in 'state' by
/// generating a 'vector.transfer_read' op with the proper permutation map
/// inferred from the indices of the load. The new 'vector.transfer_read' is
/// registered as replacement of the scalar load. Returns the newly created
/// 'vector.transfer_read' if vectorization was successful. Returns nullptr,
/// otherwise.
static Operation *vectorizeAffineLoad(AffineLoadOp loadOp,
                                      VectorizationState &state) {
  MemRefType memRefType = loadOp.getMemRefType();
  Type elementType = memRefType.getElementType();
  auto vectorType = VectorType::get(state.strategy->vectorSizes, elementType);

  // Replace map operands with operands from the vector loop nest.
  SmallVector<Value, 8> mapOperands;
  state.getScalarValueReplacementsFor(loadOp.getMapOperands(), mapOperands);

  // Compute indices for the transfer op. AffineApplyOp's may be generated.
  SmallVector<Value, 8> indices;
  indices.reserve(memRefType.getRank());
  if (loadOp.getAffineMap() !=
      state.builder.getMultiDimIdentityMap(memRefType.getRank()))
    computeMemoryOpIndices(loadOp, loadOp.getAffineMap(), mapOperands, state,
                           indices);
  else
    indices.append(mapOperands.begin(), mapOperands.end());

  // Compute permutation map using the information of new vector loops.
  auto permutationMap = makePermutationMap(state.builder.getInsertionBlock(),
                                           indices, state.vecLoopToVecDim);
  if (!permutationMap) {
    LLVM_DEBUG(dbgs() << "\n[early-vect]+++++ can't compute permutationMap\n");
    return nullptr;
  }
  LLVM_DEBUG(dbgs() << "\n[early-vect]+++++ permutationMap: ");
  LLVM_DEBUG(permutationMap.print(dbgs()));

  auto transfer = state.builder.create<vector::TransferReadOp>(
      loadOp.getLoc(), vectorType, loadOp.getMemRef(), indices, permutationMap);

  // Register replacement for future uses in the scope.
  state.registerOpVectorReplacement(loadOp, transfer);
  return transfer;
}

/// Vectorizes an affine store with the vectorization strategy in 'state' by
/// generating a 'vector.transfer_write' op with the proper permutation map
/// inferred from the indices of the store. The new 'vector.transfer_store' is
/// registered as replacement of the scalar load. Returns the newly created
/// 'vector.transfer_write' if vectorization was successful. Returns nullptr,
/// otherwise.
static Operation *vectorizeAffineStore(AffineStoreOp storeOp,
                                       VectorizationState &state) {
  MemRefType memRefType = storeOp.getMemRefType();
  Value vectorValue = vectorizeOperand(storeOp.getValueToStore(), state);
  if (!vectorValue)
    return nullptr;

  // Replace map operands with operands from the vector loop nest.
  SmallVector<Value, 8> mapOperands;
  state.getScalarValueReplacementsFor(storeOp.getMapOperands(), mapOperands);

  // Compute indices for the transfer op. AffineApplyOp's may be generated.
  SmallVector<Value, 8> indices;
  indices.reserve(memRefType.getRank());
  if (storeOp.getAffineMap() !=
      state.builder.getMultiDimIdentityMap(memRefType.getRank()))
    computeMemoryOpIndices(storeOp, storeOp.getAffineMap(), mapOperands, state,
                           indices);
  else
    indices.append(mapOperands.begin(), mapOperands.end());

  // Compute permutation map using the information of new vector loops.
  auto permutationMap = makePermutationMap(state.builder.getInsertionBlock(),
                                           indices, state.vecLoopToVecDim);
  if (!permutationMap)
    return nullptr;
  LLVM_DEBUG(dbgs() << "\n[early-vect]+++++ permutationMap: ");
  LLVM_DEBUG(permutationMap.print(dbgs()));

  auto transfer = state.builder.create<vector::TransferWriteOp>(
      storeOp.getLoc(), vectorValue, storeOp.getMemRef(), indices,
      permutationMap);
  LLVM_DEBUG(dbgs() << "\n[early-vect]+++++ vectorized store: " << transfer);

  // Register replacement for future uses in the scope.
  state.registerOpVectorReplacement(storeOp, transfer);
  return transfer;
}

/// Vectorizes a loop with the vectorization strategy in 'state'. A new loop is
/// created and registered as replacement for the scalar loop. The builder's
/// insertion point is set to the new loop's body so that subsequent vectorized
/// operations are inserted into the new loop. If the loop is a vector
/// dimension, the step of the newly created loop will reflect the vectorization
/// factor used to vectorized that dimension.
// TODO: Add support for 'iter_args'. Related operands and results will be
// vectorized at this point.
static Operation *vectorizeAffineForOp(AffineForOp forOp,
                                       VectorizationState &state) {
  // 'iter_args' not supported yet.
  if (forOp.getNumIterOperands() > 0)
    return nullptr;

  // If we are vectorizing a vector dimension, compute a new step for the new
  // vectorized loop using the vectorization factor for the vector dimension.
  // Otherwise, propagate the step of the scalar loop.
  const VectorizationStrategy &strategy = *state.strategy;
  auto loopToVecDimIt = strategy.loopToVectorDim.find(forOp);
  bool isLoopVecDim = loopToVecDimIt != strategy.loopToVectorDim.end();
  unsigned newStep;
  if (isLoopVecDim) {
    unsigned vectorDim = loopToVecDimIt->second;
    assert(vectorDim < strategy.vectorSizes.size() && "vector dim overflow");
    int64_t forOpVecFactor = strategy.vectorSizes[vectorDim];
    newStep = forOp.getStep() * forOpVecFactor;
  } else {
    newStep = forOp.getStep();
  }

  auto vecForOp = state.builder.create<AffineForOp>(
      forOp.getLoc(), forOp.getLowerBoundOperands(), forOp.getLowerBoundMap(),
      forOp.getUpperBoundOperands(), forOp.getUpperBoundMap(), newStep,
      forOp.getIterOperands(),
      /*bodyBuilder=*/[](OpBuilder &, Location, Value, ValueRange) {
        // Make sure we don't create a default terminator in the loop body as
        // the proper terminator will be added during vectorization.
        return;
      });

  // Register loop-related replacements:
  //   1) The new vectorized loop is registered as vector replacement of the
  //      scalar loop.
  //      TODO: Support reductions along the vector dimension.
  //   2) The new iv of the vectorized loop is registered as scalar replacement
  //      since a scalar copy of the iv will prevail in the vectorized loop.
  //      TODO: A vector replacement will also be added in the future when
  //      vectorization of linear ops is supported.
  //   3) TODO: Support 'iter_args' along non-vector dimensions.
  state.registerOpVectorReplacement(forOp, vecForOp);
  state.registerValueScalarReplacement(forOp.getInductionVar(),
                                       vecForOp.getInductionVar());
  // Map the new vectorized loop to its vector dimension.
  if (isLoopVecDim)
    state.vecLoopToVecDim[vecForOp] = loopToVecDimIt->second;

  // Change insertion point so that upcoming vectorized instructions are
  // inserted into the vectorized loop's body.
  state.builder.setInsertionPointToStart(vecForOp.getBody());
  return vecForOp;
}

/// Vectorizes arbitrary operation by plain widening. We apply generic type
/// widening of all its results and retrieve the vector counterparts for all its
/// operands.
static Operation *widenOp(Operation *op, VectorizationState &state) {
  SmallVector<Type, 8> vectorTypes;
  for (Value result : op->getResults())
    vectorTypes.push_back(
        VectorType::get(state.strategy->vectorSizes, result.getType()));

  SmallVector<Value, 8> vectorOperands;
  for (Value operand : op->getOperands()) {
    Value vecOperand = vectorizeOperand(operand, state);
    if (!vecOperand) {
      LLVM_DEBUG(dbgs() << "\n[early-vect]+++++ an operand failed vectorize\n");
      return nullptr;
    }
    vectorOperands.push_back(vecOperand);
  }

  // Create a clone of the op with the proper operands and return types.
  // TODO: The following assumes there is always an op with a fixed
  // name that works both in scalar mode and vector mode.
  // TODO: Is it worth considering an Operation.clone operation which
  // changes the type so we can promote an Operation with less boilerplate?
  OperationState vecOpState(op->getLoc(), op->getName().getStringRef(),
                            vectorOperands, vectorTypes, op->getAttrs(),
                            /*successors=*/{}, /*regions=*/{});
  Operation *vecOp = state.builder.createOperation(vecOpState);
  state.registerOpVectorReplacement(op, vecOp);
  return vecOp;
}

/// Vectorizes a yield operation by widening its types. The builder's insertion
/// point is set after the vectorized parent op to continue vectorizing the
/// operations after the parent op.
static Operation *vectorizeAffineYieldOp(AffineYieldOp yieldOp,
                                         VectorizationState &state) {
  // 'iter_args' not supported yet.
  if (yieldOp.getNumOperands() > 0)
    return nullptr;

  // Vectorize the yield op and change the insertion point right after the new
  // parent op.
  Operation *newYieldOp = widenOp(yieldOp, state);
  Operation *newParentOp = state.builder.getInsertionBlock()->getParentOp();
  state.builder.setInsertionPointAfter(newParentOp);
  return newYieldOp;
}

/// Encodes Operation-specific behavior for vectorization. In general we
/// assume that all operands of an op must be vectorized but this is not
/// always true. In the future, it would be nice to have a trait that
/// describes how a particular operation vectorizes. For now we implement the
/// case distinction here. Returns a vectorized form of an operation or
/// nullptr if vectorization fails.
// TODO: consider adding a trait to Op to describe how it gets vectorized.
// Maybe some Ops are not vectorizable or require some tricky logic, we cannot
// do one-off logic here; ideally it would be TableGen'd.
static Operation *vectorizeOneOperation(Operation *op,
                                        VectorizationState &state) {
  // Sanity checks.
  assert(!isa<vector::TransferReadOp>(op) &&
         "vector.transfer_read cannot be further vectorized");
  assert(!isa<vector::TransferWriteOp>(op) &&
         "vector.transfer_write cannot be further vectorized");

  if (auto loadOp = dyn_cast<AffineLoadOp>(op))
    return vectorizeAffineLoad(loadOp, state);
  if (auto storeOp = dyn_cast<AffineStoreOp>(op))
    return vectorizeAffineStore(storeOp, state);
  if (auto forOp = dyn_cast<AffineForOp>(op))
    return vectorizeAffineForOp(forOp, state);
  if (auto yieldOp = dyn_cast<AffineYieldOp>(op))
    return vectorizeAffineYieldOp(yieldOp, state);
  if (auto constant = dyn_cast<ConstantOp>(op))
    return vectorizeConstant(constant, state);

  // Other ops with regions are not supported.
  if (op->getNumRegions() != 0)
    return nullptr;

  return widenOp(op, state);
}

/// Recursive implementation to convert all the nested loops in 'match' to a 2D
/// vector container that preserves the relative nesting level of each loop with
/// respect to the others in 'match'. 'currentLevel' is the nesting level that
/// will be assigned to the loop in the current 'match'.
static void
getMatchedAffineLoopsRec(NestedMatch match, unsigned currentLevel,
                         std::vector<SmallVector<AffineForOp, 2>> &loops) {
  // Add a new empty level to the output if it doesn't exist already.
  assert(currentLevel <= loops.size() && "Unexpected currentLevel");
  if (currentLevel == loops.size())
    loops.push_back(SmallVector<AffineForOp, 2>());

  // Add current match and recursively visit its children.
  loops[currentLevel].push_back(cast<AffineForOp>(match.getMatchedOperation()));
  for (auto childMatch : match.getMatchedChildren()) {
    getMatchedAffineLoopsRec(childMatch, currentLevel + 1, loops);
  }
}

/// Converts all the nested loops in 'match' to a 2D vector container that
/// preserves the relative nesting level of each loop with respect to the others
/// in 'match'. This means that every loop in 'loops[i]' will have a parent loop
/// in 'loops[i-1]'. A loop in 'loops[i]' may or may not have a child loop in
/// 'loops[i+1]'.
static void
getMatchedAffineLoops(NestedMatch match,
                      std::vector<SmallVector<AffineForOp, 2>> &loops) {
  getMatchedAffineLoopsRec(match, /*currLoopDepth=*/0, loops);
}

/// Internal implementation to vectorize affine loops from a single loop nest
/// using an n-D vectorization strategy.
static LogicalResult
vectorizeLoopNest(std::vector<SmallVector<AffineForOp, 2>> &loops,
                  const VectorizationStrategy &strategy) {
  assert(loops[0].size() == 1 && "Expected single root loop");
  AffineForOp rootLoop = loops[0][0];
  VectorizationState state(rootLoop.getContext());
  state.builder.setInsertionPointAfter(rootLoop);
  state.strategy = &strategy;

  // Since patterns are recursive, they can very well intersect.
  // Since we do not want a fully greedy strategy in general, we decouple
  // pattern matching, from profitability analysis, from application.
  // As a consequence we must check that each root pattern is still
  // vectorizable. If a pattern is not vectorizable anymore, we just skip it.
  // TODO: implement a non-greedy profitability analysis that keeps only
  // non-intersecting patterns.
  if (!isVectorizableLoopBody(rootLoop, vectorTransferPattern())) {
    LLVM_DEBUG(dbgs() << "\n[early-vect]+++++ loop is not vectorizable");
    return failure();
  }

  //////////////////////////////////////////////////////////////////////////////
  // Vectorize the scalar loop nest following a topological order. A new vector
  // loop nest with the vectorized operations is created along the process. If
  // vectorization succeeds, the scalar loop nest is erased. If vectorization
  // fails, the vector loop nest is erased and the scalar loop nest is not
  // modified.
  //////////////////////////////////////////////////////////////////////////////

  auto opVecResult = rootLoop.walk<WalkOrder::PreOrder>([&](Operation *op) {
    LLVM_DEBUG(dbgs() << "[early-vect]+++++ Vectorizing: " << *op);
    Operation *vectorOp = vectorizeOneOperation(op, state);
    if (!vectorOp)
      return WalkResult::interrupt();

    return WalkResult::advance();
  });

  if (opVecResult.wasInterrupted()) {
    LLVM_DEBUG(dbgs() << "[early-vect]+++++ failed vectorization for: "
                      << rootLoop << "\n");
    // Erase vector loop nest if it was created.
    auto vecRootLoopIt = state.opVectorReplacement.find(rootLoop);
    if (vecRootLoopIt != state.opVectorReplacement.end())
      eraseLoopNest(cast<AffineForOp>(vecRootLoopIt->second));

    return failure();
  }

  assert(state.opVectorReplacement.count(rootLoop) == 1 &&
         "Expected vector replacement for loop nest");
  LLVM_DEBUG(dbgs() << "\n[early-vect]+++++ success vectorizing pattern");
  LLVM_DEBUG(dbgs() << "\n[early-vect]+++++ vectorization result:\n"
                    << *state.opVectorReplacement[rootLoop]);

  // Finish this vectorization pattern.
  state.finishVectorizationPattern(rootLoop);
  return success();
}

/// Extracts the matched loops and vectorizes them following a topological
/// order. A new vector loop nest will be created if vectorization succeeds. The
/// original loop nest won't be modified in any case.
static LogicalResult vectorizeRootMatch(NestedMatch m,
                                        const VectorizationStrategy &strategy) {
  std::vector<SmallVector<AffineForOp, 2>> loopsToVectorize;
  getMatchedAffineLoops(m, loopsToVectorize);
  return vectorizeLoopNest(loopsToVectorize, strategy);
}

/// Traverses all the loop matches and classifies them into intersection
/// buckets. Two matches intersect if any of them encloses the other one. A
/// match intersects with a bucket if the match intersects with the root
/// (outermost) loop in that bucket.
static void computeIntersectionBuckets(
    ArrayRef<NestedMatch> matches,
    std::vector<SmallVector<NestedMatch, 8>> &intersectionBuckets) {
  assert(intersectionBuckets.empty() && "Expected empty output");
  // Keeps track of the root (outermost) loop of each bucket.
  SmallVector<AffineForOp, 8> bucketRoots;

  for (const NestedMatch &match : matches) {
    AffineForOp matchRoot = cast<AffineForOp>(match.getMatchedOperation());
    bool intersects = false;
    for (int i = 0, end = intersectionBuckets.size(); i < end; ++i) {
      AffineForOp bucketRoot = bucketRoots[i];
      // Add match to the bucket if the bucket root encloses the match root.
      if (bucketRoot->isAncestor(matchRoot)) {
        intersectionBuckets[i].push_back(match);
        intersects = true;
        break;
      }
      // Add match to the bucket if the match root encloses the bucket root. The
      // match root becomes the new bucket root.
      if (matchRoot->isAncestor(bucketRoot)) {
        bucketRoots[i] = matchRoot;
        intersectionBuckets[i].push_back(match);
        intersects = true;
        break;
      }
    }

    // Match doesn't intersect with any existing bucket. Create a new bucket for
    // it.
    if (!intersects) {
      bucketRoots.push_back(matchRoot);
      intersectionBuckets.push_back(SmallVector<NestedMatch, 8>());
      intersectionBuckets.back().push_back(match);
    }
  }
}

/// Internal implementation to vectorize affine loops in 'loops' using the n-D
/// vectorization factors in 'vectorSizes'. By default, each vectorization
/// factor is applied inner-to-outer to the loops of each loop nest.
/// 'fastestVaryingPattern' can be optionally used to provide a different loop
/// vectorization order.
static void vectorizeLoops(Operation *parentOp, DenseSet<Operation *> &loops,
                           ArrayRef<int64_t> vectorSizes,
                           ArrayRef<int64_t> fastestVaryingPattern) {
  // Compute 1-D, 2-D or 3-D loop pattern to be matched on the target loops.
  Optional<NestedPattern> pattern =
      makePattern(loops, vectorSizes.size(), fastestVaryingPattern);
  if (!pattern.hasValue()) {
    LLVM_DEBUG(dbgs() << "\n[early-vect] pattern couldn't be computed\n");
    return;
  }

  LLVM_DEBUG(dbgs() << "\n******************************************");
  LLVM_DEBUG(dbgs() << "\n******************************************");
  LLVM_DEBUG(dbgs() << "\n[early-vect] new pattern on parent op\n");
  LLVM_DEBUG(dbgs() << *parentOp << "\n");

  unsigned patternDepth = pattern->getDepth();

  // Compute all the pattern matches and classify them into buckets of
  // intersecting matches.
  SmallVector<NestedMatch, 32> allMatches;
  pattern->match(parentOp, &allMatches);
  std::vector<SmallVector<NestedMatch, 8>> intersectionBuckets;
  computeIntersectionBuckets(allMatches, intersectionBuckets);

  // Iterate over all buckets and vectorize the matches eagerly. We can only
  // vectorize one match from each bucket since all the matches within a bucket
  // intersect.
  for (auto &intersectingMatches : intersectionBuckets) {
    for (NestedMatch &match : intersectingMatches) {
      VectorizationStrategy strategy;
      // TODO: depending on profitability, elect to reduce the vector size.
      strategy.vectorSizes.assign(vectorSizes.begin(), vectorSizes.end());
      if (failed(analyzeProfitability(match.getMatchedChildren(), 1,
                                      patternDepth, &strategy))) {
        continue;
      }
      vectorizeLoopIfProfitable(match.getMatchedOperation(), 0, patternDepth,
                                &strategy);
      // Vectorize match. Skip the rest of intersecting matches in the bucket if
      // vectorization succeeded.
      // TODO: if pattern does not apply, report it; alter the cost/benefit.
      // TODO: some diagnostics if failure to vectorize occurs.
      if (succeeded(vectorizeRootMatch(match, strategy)))
        break;
    }
  }

  LLVM_DEBUG(dbgs() << "\n");
}

std::unique_ptr<OperationPass<FuncOp>>
createSuperVectorizePass(ArrayRef<int64_t> virtualVectorSize) {
  return std::make_unique<Vectorize>(virtualVectorSize);
}
std::unique_ptr<OperationPass<FuncOp>> createSuperVectorizePass() {
  return std::make_unique<Vectorize>();
}

/// Applies vectorization to the current function by searching over a bunch of
/// predetermined patterns.
void Vectorize::runOnFunction() {
  FuncOp f = getFunction();
  if (!fastestVaryingPattern.empty() &&
      fastestVaryingPattern.size() != vectorSizes.size()) {
    f.emitRemark("Fastest varying pattern specified with different size than "
                 "the vector size.");
    return signalPassFailure();
  }

  DenseSet<Operation *> parallelLoops;
  f.walk([&parallelLoops](AffineForOp loop) {
    if (isLoopParallel(loop))
      parallelLoops.insert(loop);
  });

  // Thread-safe RAII local context, BumpPtrAllocator freed on exit.
  NestedPatternContext mlContext;
  vectorizeLoops(f, parallelLoops, vectorSizes, fastestVaryingPattern);
}

/// Verify that affine loops in 'loops' meet the nesting criteria expected by
/// SuperVectorizer:
///   * There must be at least one loop.
///   * There must be a single root loop (nesting level 0).
///   * Each loop at a given nesting level must be nested in a loop from a
///     previous nesting level.
static LogicalResult
verifyLoopNesting(const std::vector<SmallVector<AffineForOp, 2>> &loops) {
  // Expected at least one loop.
  if (loops.empty())
    return failure();

  // Expected only one root loop.
  if (loops[0].size() != 1)
    return failure();

  // Traverse loops outer-to-inner to check some invariants.
  for (int i = 1, end = loops.size(); i < end; ++i) {
    for (AffineForOp loop : loops[i]) {
      //  Check that each loop at this level is nested in one of the loops from
      //  the previous level.
      if (none_of(loops[i - 1], [&](AffineForOp maybeParent) {
            return maybeParent->isProperAncestor(loop);
          }))
        return failure();

      //  Check that each loop at this level is not nested in another loop from
      //  this level.
      for (AffineForOp sibling : loops[i]) {
        if (sibling->isProperAncestor(loop))
          return failure();
      }
    }
  }

  return success();
}

namespace mlir {

/// External utility to vectorize affine loops in 'loops' using the n-D
/// vectorization factors in 'vectorSizes'. By default, each vectorization
/// factor is applied inner-to-outer to the loops of each loop nest.
/// 'fastestVaryingPattern' can be optionally used to provide a different loop
/// vectorization order.
void vectorizeAffineLoops(Operation *parentOp, DenseSet<Operation *> &loops,
                          ArrayRef<int64_t> vectorSizes,
                          ArrayRef<int64_t> fastestVaryingPattern) {
  // Thread-safe RAII local context, BumpPtrAllocator freed on exit.
  NestedPatternContext mlContext;
  vectorizeLoops(parentOp, loops, vectorSizes, fastestVaryingPattern);
}

/// External utility to vectorize affine loops from a single loop nest using an
/// n-D vectorization strategy (see doc in VectorizationStrategy definition).
/// Loops are provided in a 2D vector container. The first dimension represents
/// the nesting level relative to the loops to be vectorized. The second
/// dimension contains the loops. This means that:
///   a) every loop in 'loops[i]' must have a parent loop in 'loops[i-1]',
///   b) a loop in 'loops[i]' may or may not have a child loop in 'loops[i+1]'.
///
/// For example, for the following loop nest:
///
///   func @vec2d(%in0: memref<64x128x512xf32>, %in1: memref<64x128x128xf32>,
///               %out0: memref<64x128x512xf32>,
///               %out1: memref<64x128x128xf32>) {
///     affine.for %i0 = 0 to 64 {
///       affine.for %i1 = 0 to 128 {
///         affine.for %i2 = 0 to 512 {
///           %ld = affine.load %in0[%i0, %i1, %i2] : memref<64x128x512xf32>
///           affine.store %ld, %out0[%i0, %i1, %i2] : memref<64x128x512xf32>
///         }
///         affine.for %i3 = 0 to 128 {
///           %ld = affine.load %in1[%i0, %i1, %i3] : memref<64x128x128xf32>
///           affine.store %ld, %out1[%i0, %i1, %i3] : memref<64x128x128xf32>
///         }
///       }
///     }
///     return
///   }
///
/// loops = {{%i0}, {%i2, %i3}}, to vectorize the outermost and the two
/// innermost loops;
/// loops = {{%i1}, {%i2, %i3}}, to vectorize the middle and the two innermost
/// loops;
/// loops = {{%i2}}, to vectorize only the first innermost loop;
/// loops = {{%i3}}, to vectorize only the second innermost loop;
/// loops = {{%i1}}, to vectorize only the middle loop.
LogicalResult
vectorizeAffineLoopNest(std::vector<SmallVector<AffineForOp, 2>> &loops,
                        const VectorizationStrategy &strategy) {
  // Thread-safe RAII local context, BumpPtrAllocator freed on exit.
  NestedPatternContext mlContext;
  if (failed(verifyLoopNesting(loops)))
    return failure();
  return vectorizeLoopNest(loops, strategy);
}

std::unique_ptr<OperationPass<FuncOp>>
createSuperVectorizePass(ArrayRef<int64_t> virtualVectorSize) {
  return std::make_unique<Vectorize>(virtualVectorSize);
}
std::unique_ptr<OperationPass<FuncOp>> createSuperVectorizePass() {
  return std::make_unique<Vectorize>();
}

} // namespace mlir
