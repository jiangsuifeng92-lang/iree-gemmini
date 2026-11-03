// Copyright 2026 The Gemmini Backend Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "compiler/Transforms/Passes.h"

#include "compiler/Transforms/GemminiConvMatch.h"
#include "iree/compiler/Codegen/Common/CPU/Passes.h"
#include "iree/compiler/Codegen/Dialect/Codegen/IR/IREECodegenOps.h"
#include "llvm/Support/Debug.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Linalg/IR/LinalgInterfaces.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Pass/PassManager.h"

#include <optional>
#include <fstream>
#include <mutex>
#include "gemmini_epilogue.h"
#include <tuple>

#define DEBUG_TYPE "iree-gemmini-lower"

namespace mlir::gemmini_compiler {

using namespace mlir::iree_compiler;

#define GEN_PASS_DEF_GEMMINILOWERDISPATCH
#include "compiler/Transforms/Passes.h.inc"

namespace {

// Must match the `func.func private` declaration in
// phase0/gemmini_matmul.mlir and the C definition in
// kernels/gemmini_matmul.c.
constexpr const char *kMatmulSymbol = "gemmini_matmul_i8_planned";
constexpr const char *kMatmulRequantSymbol = "gemmini_matmul_i8_requant_planned";
constexpr const char *kConvSymbol = "gemmini_conv2d_i8_planned";
constexpr const char *kConvRequantSymbol = "gemmini_conv2d_i8_requant_planned";

struct GemminiLowerDispatchPass
    : public impl::GemminiLowerDispatchBase<GemminiLowerDispatchPass> {
  using impl::GemminiLowerDispatchBase<
      GemminiLowerDispatchPass>::GemminiLowerDispatchBase;

  void runOnOperation() override {
    FunctionOpInterface funcOp = getOperation();
    if (!funcOp->hasAttr("gemmini.select")) return;

    // The dispatch is still tensor-based; the kernel takes pointers, so run
    // the same bufferization the CPU pipeline uses. Scoped to this function
    // so unselected dispatches keep their tensor form for LLVMCPU.
    {
      OpPassManager funcPassManager(funcOp->getName().getStringRef());
      addCPUBufferizePasses(funcPassManager);
      funcPassManager.addPass(createGemminiLayoutPlanningPass());
      if (failed(runPipeline(funcPassManager, funcOp.getOperation()))) {
        funcOp->emitError("gemmini: bufferization of the dispatch failed");
        return signalPassFailure();
      }
    }

    // Bufferization in the stock CPU pipeline runs after tile-and-distribute,
    // so its temporaries are small and land on the stack. We replace the
    // contraction before any tiling, which leaves the whole int32 accumulator
    // live: 3025x16 is 193 KB, well past IREE's 32 KB stack-check limit. Move
    // anything that large into the dispatch's transient buffer instead.

    linalg::LinalgOp root;
    ConvShape convShape;
    bool isConv = false;
    funcOp->walk([&](linalg::LinalgOp op) {
      if (!root && linalg::isaContractionOpInterface(op)) root = op;
    });
    if (!root) {
      // A 3x3 convolution keeps the sliding window in its indexing maps, so it
      // never matches the contraction interface. Same matcher as in
      // gemmini-select, re-run here because the operands are now memrefs.
      funcOp->walk([&](linalg::LinalgOp op) {
        if (root) return;
        if (auto shape = matchGemminiConv(op)) {
          root = op;
          convShape = *shape;
          isConv = true;
        }
      });
    }
    if (!root) {
      funcOp->emitError("gemmini: selected dispatch has no contraction left");
      return signalPassFailure();
    }

    auto plan = funcOp->getAttrOfType<DictionaryAttr>("gemmini.plan");
    if (!plan) { funcOp->emitError("H: missing compile-time Gemmini plan"); return signalPassFailure(); }
    for (const char *key : {"abi", "tile_i", "tile_j", "tile_k", "block_m", "layout_flags"}) {
      if (!plan.getAs<IntegerAttr>(key)) {
        funcOp->emitError("H: incomplete Gemmini plan"); return signalPassFailure();
      }
    }

    Value a = root->getOperand(0);
    Value b = root->getOperand(1);
    Value c = root->getOperand(2);
    auto aType = dyn_cast<MemRefType>(a.getType());
    auto bType = dyn_cast<MemRefType>(b.getType());
    auto cType = dyn_cast<MemRefType>(c.getType());
    if (!aType || !bType || !cType) {
      root->emitError("gemmini: contraction operands did not become memrefs");
      return signalPassFailure();
    }

    Location loc = root->getLoc();
    OpBuilder builder(root);
    auto planArgs = [&]() -> SmallVector<Value> {
      SmallVector<Value> args;
      for (const char *key : {"abi", "tile_i", "tile_j", "tile_k", "block_m", "layout_flags"})
        args.push_back(builder.create<arith::ConstantIndexOp>(
            loc, plan.getAs<IntegerAttr>(key).getInt()));
      return args;
    };
    auto plannedCall = [&](func::FuncOp callee, ValueRange values) {
      SmallVector<Value> args(values.begin(), values.end());
      auto extra = planArgs(); args.append(extra.begin(), extra.end());
      return builder.create<func::CallOp>(loc, callee, args);
    };

    // Base pointer, element offset and per-dimension strides of a buffer.
    struct StridedView {
      Value base;
      Value offset;
      SmallVector<Value> strides;
    };
    // Result types are spelled out rather than inferred: the inference path
    // drops the HAL descriptor memory space off the base buffer, which then
    // fails to match the declaration's element memref type.
    auto view = [&](Value memref) -> StridedView {
      auto type = cast<MemRefType>(memref.getType());
      SmallVector<Type> resultTypes{
          MemRefType::get({}, type.getElementType()), builder.getIndexType()};
      for (int64_t i = 0; i < type.getRank(); ++i)
        resultTypes.push_back(builder.getIndexType());  // sizes
      for (int64_t i = 0; i < type.getRank(); ++i)
        resultTypes.push_back(builder.getIndexType());  // strides
      // The metadata base keeps the HAL descriptor memory space. The external
      // declaration cannot: EraseHALDescriptorTypeFromMemRef rewrites SSA
      // values but never external function signatures, so a descriptor-typed
      // parameter would stop matching its argument later in the pipeline.
      resultTypes[0] = MemRefType::get({}, type.getElementType(),
                                       MemRefLayoutAttrInterface(),
                                       type.getMemorySpace());
      auto metadata = builder.create<IREE::Codegen::ExtractStridedMetadataOp>(
          loc, resultTypes, ValueRange{memref});
      Value base = metadata.getBaseBuffer();
      if (cast<MemRefType>(base.getType()).getMemorySpace() != Attribute()) {
        base = builder.create<memref::MemorySpaceCastOp>(
            loc, MemRefType::get({}, type.getElementType()), base);
      }
      SmallVector<Value> strides(metadata.getStrides().begin(),
                                 metadata.getStrides().end());
      return {base, metadata.getOffset(), strides};
    };

    if (isConv) {
      auto callee = getOrCreateConvDecl(funcOp, builder, aType, bType, cType);
      if (!callee) {
        root->emitError("gemmini: could not declare the conv kernel symbol");
        return signalPassFailure();
      }
      StridedView in = view(a), w = view(b), out = view(c);
      auto cst = [&](int64_t value) {
        return builder.create<arith::ConstantIndexOp>(loc, value).getResult();
      };
      // in is [H, W, C], w is [KH, KW, C, OC], out is [OH, OW, OC].
      //
      // The kernel walks k as (kh*KW + kw)*C + c and treats the output as a
      // flat [OH*OW, OC] matrix, so both flattenings must be linear in memory.
      // These are compile-time (static) strides; if a future pipeline hands us
      // a genuinely strided buffer the right move is to fail loudly, because
      // guessing here writes roughly 350 MB past the accumulator.
      SmallVector<int64_t> inStrides, wStrides, outStrides;
      int64_t inOff, wOff, outOff;
      auto stridesOk = [](MemRefType type, SmallVectorImpl<int64_t> &out,
                          int64_t &off) {
        if (failed(type.getStridesAndOffset(out, off))) return false;
        for (int64_t s : out)
          if (ShapedType::isDynamic(s)) return false;
        return true;
      };
      if (!stridesOk(aType, inStrides, inOff) ||
          !stridesOk(bType, wStrides, wOff) ||
          !stridesOk(cType, outStrides, outOff)) {
        root->emitError("gemmini: conv operands have non-static strides");
        return signalPassFailure();
      }
      Value weightRowStride = w.strides[convShape.singleChannel ? 1 : 2];
      bool weightsOk = convShape.singleChannel
          ? wStrides[2]==1 && wStrides[1]==convShape.OC && wStrides[0]==convShape.KW*wStrides[1]
          : wStrides[3]==1 && wStrides[2]==convShape.OC &&
            wStrides[1]==convShape.C*wStrides[2] && wStrides[0]==convShape.KW*wStrides[1];
      bool layoutsOk = inStrides.back()==1 && weightsOk && outStrides[2]==1 &&
                       outStrides[0]==convShape.OW*outStrides[1];
      if (!layoutsOk) {
        root->emitError("gemmini: conv operands are not densely packed");
        return signalPassFailure();
      }

      if (!std::getenv("GEMMINI_NO_FUSE_EPILOGUE")) {
        if (auto fused = matchFusedEpilogue(c, root)) {
          auto fusedType = cast<MemRefType>(fused->out.getType());
          SmallVector<int64_t> fs; int64_t offset;
          bool dense = fusedType.hasStaticShape() && fusedType.getRank() >= 2 &&
                       fusedType.getNumElements() == convShape.OH * convShape.OW * convShape.OC &&
                       fusedType.getShape().back() == convShape.OC &&
                       succeeded(fusedType.getStridesAndOffset(fs, offset));
          if (dense) {
            dense = fs.back() == 1 && fs[fs.size()-2] >= convShape.OC;
            for (int64_t stride : fs) dense &= !ShapedType::isDynamic(stride);
            for (unsigned i=0; i+2<fs.size(); ++i)
              dense &= fs[i] == fusedType.getShape()[i+1] * fs[i+1];
          }
          if (dense) {
            auto fusedCallee = getOrCreateConvRequantDecl(funcOp, builder, aType, bType, fusedType);
            StridedView dest = view(fused->out), bias = view(fused->bias);
            auto constant = [&](Type type, int64_t v) -> Value {
              return builder.create<arith::ConstantOp>(loc, IntegerAttr::get(type,v));
            };
            plannedCall(fusedCallee,
              ValueRange{in.base,in.offset,in.strides[0],in.strides[1],
                w.base,w.offset,weightRowStride,dest.base,dest.offset,dest.strides[fs.size()-2],
                cst(convShape.H),cst(convShape.W),cst(convShape.C),cst(convShape.OH),cst(convShape.OW),cst(convShape.OC),
                cst(convShape.KH),cst(convShape.KW),cst(convShape.stride),bias.base,bias.offset,
                constant(builder.getI64Type(),fused->mult),constant(builder.getI64Type(),fused->round),
                constant(builder.getI64Type(),fused->shift),constant(builder.getI32Type(),fused->minV),
                constant(builder.getI32Type(),fused->maxV)});
            recordEpilogue(funcOp,"convolution",*fused);
            fused->op->erase(); root->erase(); cleanupAccumulator(c); return;
          }
        }
      }

      plannedCall(
          callee,
          ValueRange{in.base, in.offset, in.strides[0], in.strides[1],
                     w.base, w.offset, weightRowStride,
                     out.base, out.offset, out.strides[1],
                     cst(convShape.H), cst(convShape.W), cst(convShape.C),
                     cst(convShape.OH), cst(convShape.OW), cst(convShape.OC),
                     cst(convShape.KH), cst(convShape.KW),
                     cst(convShape.stride)});
      root->erase();
      LLVM_DEBUG(llvm::dbgs()
                 << "gemmini: lowered conv " << convShape.OH << "x"
                 << convShape.OW << "x" << convShape.OC << " k" << convShape.KH
                 << " s" << convShape.stride << " to " << kConvSymbol << "\n");
      return;
    }

    // See gemmini-select: a rank-1 operand means that dimension is 1, so the
    // kernel's MxK times KxN contract covers matmul, vecmat and matvec.
    if (bType.getRank() != 2) {
      root->emitError("gemmini: rhs must be rank-2");
      return signalPassFailure();
    }

    int64_t M = cType.getRank() == 2 ? cType.getShape()[0] : 1;
    int64_t N = cType.getRank() == 2 ? cType.getShape()[1] : cType.getShape()[0];
    int64_t K = aType.getRank() == 2 ? aType.getShape()[1] : aType.getShape()[0];

    auto callee = getOrCreateKernelDecl(funcOp, builder, aType, bType, cType);
    if (!callee) {
      root->emitError("gemmini: could not declare the kernel symbol");
      return signalPassFailure();
    }

    // The matmul operands are frequently strided subviews (e.g. a row of a
    // padded image), so the kernel cannot assume row-major packing; the row
    // stride travels with the pointer.
    StridedView aView = view(a), bView = view(b), cView = view(c);
    Value aBase = aView.base, aOffset = aView.offset, aStride = aView.strides.front();
    Value bBase = bView.base, bOffset = bView.offset, bStride = bView.strides.front();
    Value cBase = cView.base, cOffset = cView.offset, cStride = cView.strides.front();

    Value m = builder.create<arith::ConstantIndexOp>(loc, M);
    Value n = builder.create<arith::ConstantIndexOp>(loc, N);
    Value k = builder.create<arith::ConstantIndexOp>(loc, K);
    if (aType.getRank() == 1) aStride = k;
    if (cType.getRank() == 1) cStride = n;

    // Fold the fixed-point requantize epilogue into the kernel so the int32
    // separate full-size epilogue is removed. g_cp still lives in DDR. The copied baseline was verified on
    // MNIST and SqueezeNet (error 0 on both); set GEMMINI_NO_FUSE_EPILOGUE=1
    // to get the old two-pass behaviour back.
    static const bool fuseEpilogue = ::getenv("GEMMINI_NO_FUSE_EPILOGUE") == nullptr;
    // Bisect aid: GEMMINI_FUSE_K=<K> fuses only the dispatch with that
    // contraction depth, so a single layer can be validated in isolation
    // without rebuilding.
    static const char *fuseKEnv = ::getenv("GEMMINI_FUSE_K");
    static const int64_t fuseK = fuseKEnv ? ::atoll(fuseKEnv) : -1;
    if (fuseEpilogue && (fuseK < 0 || K == fuseK)) {
      if (auto fused = matchFusedEpilogue(c, root)) {
        // The epilogue's init buffer is frequently a temporary that is then
        // copied into the real dispatch output. Writing to the temporary and
        // erasing the epilogue would leave that copy reading an uninitialised
        // buffer, so follow it and target the real destination instead.
        Value outBuf = fused->out;
        // Keep later copies: the kernel initializes the original output.

        auto outType = cast<MemRefType>(outBuf.getType());
        // The kernel writes the result as a flat [M, N] matrix, so the step
        // between consecutive rows is the stride of the second-to-last
        // dimension. It used to pass strides.front(), which for a rank-3 or
        // rank-4 output is the stride of the whole plane: the kernel would
        // then scatter each of the M rows a plane apart, scribbling over
        // every neighbouring buffer. That was the fusion bug. Only fuse when
        // the two innermost dimensions are actually dense.
        SmallVector<int64_t> outStatic;
        int64_t outStaticOff = 0;
        bool layoutOk = outType.getRank() >= 1 &&
                        succeeded(outType.getStridesAndOffset(outStatic,
                                                              outStaticOff));
        for (int64_t s : outStatic)
          if (ShapedType::isDynamic(s)) layoutOk = false;
        if (layoutOk) {
          unsigned rank = outStatic.size();
          layoutOk = outStatic[rank-1]==1 && (rank==1 ? M==1 : outStatic[rank-2]==N);
        }
        if (layoutOk) {
          if (auto requantCallee = getOrCreateRequantDecl(
                  funcOp, builder, aType, bType, outType)) {
            StridedView biasView = view(fused->bias);
            StridedView fusedOut = view(outBuf);
            Value biasBase = biasView.base, biasOffset = biasView.offset;
            Value outBase = fusedOut.base, outOffset = fusedOut.offset;
            Value outRowStride = outStatic.size()==1 ? n : fusedOut.strides[outStatic.size()-2];
            (void)biasView.strides;
            auto constant = [&](Type type, int64_t value) -> Value {
              return builder.create<arith::ConstantOp>(
                  loc, IntegerAttr::get(type, value));
            };
            plannedCall(
                requantCallee,
                ValueRange{aBase, aOffset, aStride, bBase, bOffset, bStride,
                           outBase, outOffset, outRowStride, m, n, k, biasBase,
                           biasOffset,
                           constant(builder.getI64Type(), fused->mult),
                           constant(builder.getI64Type(), fused->round),
                           constant(builder.getI64Type(), fused->shift),
                           constant(builder.getI32Type(), fused->minV),
                           constant(builder.getI32Type(), fused->maxV)});
            fused->op->erase();

            recordEpilogue(funcOp, "matmul", *fused);
            root->erase();
            cleanupAccumulator(c);
            LLVM_DEBUG(llvm::dbgs() << "gemmini: fused the epilogue\n");
            return;
          }
        }
      }
    }

    // Inserted at the contraction's position: after the zero-fill of C and
    // before the epilogue that reads it. The kernel overwrites C entirely, so
    // the fill becomes redundant but harmless.
    plannedCall(
        callee,
        ValueRange{aBase, aOffset, aStride, bBase, bOffset, bStride,
                   cBase, cOffset, cStride, m, n, k});

    // The epilogue consumes C through its buffer, not through a result value,
    // so dropping the op cannot leave a dangling use.
    root->erase();

    // NOTE: no alloca promotion here. Creating a memref.alloc in this pass
    // makes the compiler turn the kernel call into a dynamic import (bisected:
    // with it disabled the library has no import table and the call stays an
    // undefined symbol). Large accumulators are instead handled by raising
    // --gemmini max stack allocation and enlarging the bare-metal stack.

    LLVM_DEBUG(llvm::dbgs() << "gemmini: lowered " << M << "x" << N << "x" << K
                            << " to " << kMatmulSymbol << "\n");
  }

 private:
  struct FusedEpilogue {
    linalg::GenericOp op;
    Value bias;
    Value out;
    int64_t mult, round, shift, minV, maxV;
  };

  // Recognizes the fixed-point requantize epilogue that the int8 exporter
  // emits after every contraction:
  //
  //   addi(acc, bias) -> extsi -> muli(mult) -> addi(round) -> shrsi(shift)
  //     -> trunci -> maxsi(min) -> minsi(max) -> trunci -> yield
  //
  // Anything that does not match exactly is left alone; the caller then falls
  // back to the unfused path, which is always correct.
  static std::optional<FusedEpilogue> matchFusedEpilogue(Value acc, linalg::LinalgOp root) {
    // A HAL-bound accumulator can be observed by another dispatch even if
    // this function has only one reader. Fuse only private temporary storage.
    Value storage=acc;
    while (Operation *definition=storage.getDefiningOp()) {
      if (isa<memref::CollapseShapeOp,memref::ExpandShapeOp,memref::CastOp,
              memref::MemorySpaceCastOp>(definition)) storage=definition->getOperand(0);
      else break;
    }
    if (!storage.getDefiningOp<memref::AllocOp>() &&
        !storage.getDefiningOp<memref::AllocaOp>()) return std::nullopt;
    SmallVector<Value> aliases{storage};
    auto rootType=cast<MemRefType>(acc.getType());
    for (size_t i=0;i<aliases.size();++i) {
      for (Operation *owner : aliases[i].getUsers()) {
        if (!isa<memref::CollapseShapeOp,memref::ExpandShapeOp,memref::CastOp,
                 memref::MemorySpaceCastOp>(owner) || owner->getNumResults()!=1) continue;
        auto type=dyn_cast<MemRefType>(owner->getResult(0).getType());
        if (type && type.hasStaticShape() && rootType.hasStaticShape() && type.getRank()>0 &&
            type.getNumElements()==rootType.getNumElements() &&
            type.getShape().back()==rootType.getShape().back() &&
            !llvm::is_contained(aliases,owner->getResult(0))) aliases.push_back(owner->getResult(0));
      }
    }
    for (Value candidate : aliases) for (Operation *user : candidate.getUsers()) {
      auto generic = dyn_cast<linalg::GenericOp>(user);
      if (!generic || generic.getNumDpsInputs()!=2 || generic.getNumDpsInits()!=1 ||
          generic.getDpsInputOperand(0)->get()!=candidate) continue;
      bool unique = true;
      for (Value alias : aliases) for (OpOperand &use : alias.getUses()) {
        Operation *owner = use.getOwner();
        if (owner==generic.getOperation()) {
          if (use.getOperandNumber()!=0) unique=false;
        } else if (owner==root.getOperation()) {
          if (use.getOperandNumber()!=2) unique=false;
        } else if (auto fill=dyn_cast<linalg::FillOp>(owner)) {
          if (fill.getDpsInitOperand(0)->get()!=alias) unique=false;
        } else if (isa<memref::CollapseShapeOp,memref::ExpandShapeOp,memref::CastOp,
                       memref::MemorySpaceCastOp>(owner)) {
          if (owner->getNumResults()!=1 || !llvm::is_contained(aliases,owner->getResult(0))) unique=false;
        } else if (!isa<memref::DeallocOp,IREE::Codegen::ExtractStridedMetadataOp>(owner)) unique=false;
      }
      if (!unique || !llvm::all_of(generic.getIteratorTypesArray(),
          [](utils::IteratorType t){return t==utils::IteratorType::parallel;})) continue;
      auto outType=dyn_cast<MemRefType>(generic.getDpsInitOperand(0)->get().getType());
      auto biasType=dyn_cast<MemRefType>(generic.getDpsInputOperand(1)->get().getType());
      auto accType=dyn_cast<MemRefType>(candidate.getType());
      if (!outType || !biasType || !accType || !outType.getElementType().isInteger(8) ||
          !accType.getElementType().isInteger(32) || !biasType.getElementType().isInteger(32) ||
          !outType.hasStaticShape() || !biasType.hasStaticShape() ||
          biasType.getRank()<1 || outType.getRank()<1) continue;
      SmallVector<int64_t> bs;int64_t bo;
      if (failed(biasType.getStridesAndOffset(bs,bo)) || bs.back()!=1 ||
          biasType.getShape().back()!=outType.getShape().back()) continue;
      bool broadcast=true;
      for (int i=0;i+1<biasType.getRank();++i) broadcast &= biasType.getShape()[i]==1;
      auto maps=generic.getIndexingMapsArray();
      if (!broadcast || maps.size()!=3 || maps[0]!=maps[2] ||
          maps[1].getNumResults()!=(unsigned)biasType.getRank()) continue;
      for (unsigned i=0;i+1<maps[1].getNumResults();++i)
        broadcast &= maps[1].getResult(i)==getAffineConstantExpr(0,generic.getContext());
      broadcast &= maps[1].getResult(maps[1].getNumResults()-1)==maps[2].getResult(maps[2].getNumResults()-1);
      if (!broadcast) continue;
      Block &body=generic.getRegion().front();
      auto yield=dyn_cast<linalg::YieldOp>(body.getTerminator());
      if (!yield || yield.getNumOperands()!=1) continue;
      auto cst=[](Value value)->std::optional<int64_t>{
        auto op=value.getDefiningOp<arith::ConstantOp>();
        if (!op) return std::nullopt;
        if (auto attr=dyn_cast<IntegerAttr>(op.getValue())) return attr.getInt();
        return std::nullopt;
      };
      Value v=yield.getOperand(0);
      int64_t postMin=-128,postMax=127;
      while(v.getType().isInteger(8)) {
        if(auto post=v.getDefiningOp<arith::MaxSIOp>()) {
          auto bound=cst(post.getRhs());if(!bound)break;
          postMin=std::max(postMin,*bound);v=post.getLhs();
        } else if(auto post=v.getDefiningOp<arith::MinSIOp>()) {
          auto bound=cst(post.getRhs());if(!bound)break;
          postMax=std::min(postMax,*bound);v=post.getLhs();
        } else break;
      }
      if(postMin>postMax)continue;
      auto narrow=v.getDefiningOp<arith::TruncIOp>();
      if (!narrow || !narrow.getIn().getType().isInteger(32)) continue;
      auto mini=narrow.getIn().getDefiningOp<arith::MinSIOp>();
      if (!mini) continue;
      auto maxi=mini.getLhs().getDefiningOp<arith::MaxSIOp>();
      auto maxV=cst(mini.getRhs());
      if (!maxi || !maxV) continue;
      auto minV=cst(maxi.getRhs());
      if (!minV || *minV < -128 || *maxV>127 || *minV>*maxV) continue;
      FusedEpilogue f;f.mult=1;f.round=0;f.shift=0;
      Value combined=maxi.getLhs();
      if (auto trunc=combined.getDefiningOp<arith::TruncIOp>()) {
        auto shr=trunc.getIn().getDefiningOp<arith::ShRSIOp>();
        if (!shr || !shr.getType().isInteger(64)) continue;
        auto roundAdd=shr.getLhs().getDefiningOp<arith::AddIOp>();
        if (!roundAdd) continue;
        auto mul=roundAdd.getLhs().getDefiningOp<arith::MulIOp>();
        if (!mul) continue;
        auto extend=mul.getLhs().getDefiningOp<arith::ExtSIOp>();
        auto mult=cst(mul.getRhs()), rounding=cst(roundAdd.getRhs()), shift=cst(shr.getRhs());
        if (!extend || !extend.getIn().getType().isInteger(32) ||
            !mult || !rounding || !shift || *shift<0 || *shift>=64) continue;
        combined=extend.getIn();f.mult=*mult;f.round=*rounding;f.shift=*shift;
      }
      auto addBias=combined.getDefiningOp<arith::AddIOp>();
      if (!addBias || !addBias.getType().isInteger(32) ||
          !((addBias.getLhs()==body.getArgument(0) && addBias.getRhs()==body.getArgument(1)) ||
            (addBias.getRhs()==body.getArgument(0) && addBias.getLhs()==body.getArgument(1)))) continue;
      // Entire expression must be pure; removing it cannot remove side effects.
      if (!llvm::all_of(body.without_terminator(),[](Operation &op){return isa<arith::ConstantOp,
          arith::AddIOp,arith::MulIOp,arith::ExtSIOp,arith::TruncIOp,arith::ShRSIOp,
          arith::MaxSIOp,arith::MinSIOp>(op);})) continue;
      f.op=generic;f.bias=generic.getDpsInputOperand(1)->get();
      f.out=generic.getDpsInitOperand(0)->get();f.minV=std::max(*minV,postMin);f.maxV=std::min(*maxV,postMax);
      if(f.minV>f.maxV)continue;
      return f;
    }
    return std::nullopt;
  }


  static void cleanupAccumulator(Value acc) {
    SmallVector<Value> aliases{acc};
    for (size_t i=0;i<aliases.size();++i) for(Operation *user:aliases[i].getUsers()) {
      if(isa<memref::CollapseShapeOp,memref::ExpandShapeOp,memref::CastOp,
             memref::MemorySpaceCastOp>(user) && user->getNumResults()==1 &&
         !llvm::is_contained(aliases,user->getResult(0))) aliases.push_back(user->getResult(0));
    }
    for(Value value:aliases) {
      SmallVector<Operation *> fills;
      for(Operation *user:value.getUsers())
        if(auto fill=dyn_cast<linalg::FillOp>(user))
          if(fill.getDpsInitOperand(0)->get()==value)fills.push_back(user);
      for(Operation *fill:fills)fill->erase();
    }
    for(Value value:llvm::reverse(aliases)) {
      SmallVector<Operation *> unusedMetadata;
      for(Operation *user:value.getUsers())
        if(isa<IREE::Codegen::ExtractStridedMetadataOp>(user) && user->use_empty())unusedMetadata.push_back(user);
      for(Operation *op:unusedMetadata)op->erase();
      if(value.use_empty()) if(Operation *definition=value.getDefiningOp())
        if(isa<memref::AllocOp,memref::AllocaOp,memref::CollapseShapeOp,memref::ExpandShapeOp,
               memref::CastOp,memref::MemorySpaceCastOp>(definition)) definition->erase();
    }
  }
  static void recordEpilogue(FunctionOpInterface fn,const char *kind,const FusedEpilogue &f) {
    if (const char *path=std::getenv("H_GEMMINI_EPILOGUE_REPORT")) {
      static std::mutex lock;std::lock_guard<std::mutex> guard(lock);
      std::ofstream out(path,std::ios::app);
      bool hw=h_gemmini_identity_epilogue(f.mult,f.round,f.shift,f.minV,f.maxV);
      out << "{\"kind\":\"" << kind << "\",\"mult\":" << f.mult
          << ",\"round\":" << f.round << ",\"shift\":" << f.shift
          << ",\"min\":" << f.minV << ",\"max\":" << f.maxV
          << ",\"path\":\"" << (hw?"identity_accumulator_mvout":"exact_integer_ddr_tile") << "\"}\n";
    }
  }

  // Keeps the dispatch under LLVMCPUCheckIRBeforeLLVMConversion's stack limit.
  // Only statically shaped allocas are considered, which is every temporary
  // our dispatches produce.
  static void promoteLargeAllocas(FunctionOpInterface funcOp) {
    constexpr int64_t kMaxStackBytes = 16 * 1024;
    SmallVector<memref::AllocaOp> candidates;
    funcOp->walk([&](memref::AllocaOp allocaOp) {
      auto type = dyn_cast<MemRefType>(allocaOp.getType());
      if (!type || !type.hasStaticShape()) return;
      int64_t bytes = type.getNumElements() *
                      type.getElementTypeBitWidth() / 8;
      if (bytes > kMaxStackBytes) candidates.push_back(allocaOp);
    });
    for (auto allocaOp : candidates) {
      OpBuilder builder(allocaOp);
      auto allocOp = builder.create<memref::AllocOp>(
          allocaOp.getLoc(), allocaOp.getType(), allocaOp.getDynamicSizes());
      allocaOp.getResult().replaceAllUsesWith(allocOp.getResult());
      allocaOp.erase();
    }
  }

  // Declaration for the fused epilogue variant: same leading parameters plus
  // the requantize constants, and an int8 output.
  func::FuncOp getOrCreateRequantDecl(FunctionOpInterface funcOp,
                                      OpBuilder &builder, MemRefType aType,
                                      MemRefType bType, MemRefType outType) {
    ModuleOp module = funcOp->getParentOfType<ModuleOp>();
    if (!module) return nullptr;
    if (auto existing = module.lookupSymbol<func::FuncOp>(kMatmulRequantSymbol))
      return existing;

    MLIRContext *context = module.getContext();
    Type indexType = builder.getIndexType();
    Type i64 = builder.getI64Type();
    Type i32 = builder.getI32Type();
    auto elem = [](Type t) { return MemRefType::get({}, t); };
    FunctionType fnType = builder.getFunctionType(
        {elem(aType.getElementType()), indexType, indexType,
         elem(bType.getElementType()), indexType, indexType,
         elem(outType.getElementType()), indexType, indexType,
         indexType, indexType, indexType,
         elem(i32), indexType,
         i64, i64, i64, i32, i32},
        {});

    SmallVector<Type> plannedInputs(fnType.getInputs().begin(), fnType.getInputs().end());
    for (int i = 0; i < 6; ++i) plannedInputs.push_back(builder.getIndexType());
    fnType = builder.getFunctionType(plannedInputs, fnType.getResults());
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    auto callee = func::FuncOp::create(moduleBuilder, module.getLoc(),
                                       kMatmulRequantSymbol, fnType);
    callee.setPrivate();
    callee->setAttr("hal.import.static", UnitAttr::get(context));
    callee->setAttr("llvm.bareptr", UnitAttr::get(context));
    return callee;
  }

  // One declaration per module, shared by every dispatch that calls it. It is
  // public so the linking pass keeps the name as-is instead of uniquing it to
  // gemmini_matmul_i8_0 across merged executables.
  func::FuncOp getOrCreateKernelDecl(FunctionOpInterface funcOp,
                                     OpBuilder &builder, MemRefType aType,
                                     MemRefType bType, MemRefType cType) {
    ModuleOp module = funcOp->getParentOfType<ModuleOp>();
    if (!module) return nullptr;

    // The bare buffer types must match the operands exactly, including the HAL
    // descriptor memory space; otherwise func.call fails to verify. Deriving
    // them from the operands also keeps this correct if the runtime ever moves
    // to a different memory space.
    auto elementMemRef = [](MemRefType type) {
      // Deliberately the default memory space: LLVMCPU's
      // EraseHALDescriptorTypeFromMemRef rewrites SSA values but not external
      // declarations, so a descriptor-typed parameter would mismatch the
      // value it is given.
      return MemRefType::get({}, type.getElementType());
    };
    Type aElem = elementMemRef(aType);
    Type bElem = elementMemRef(bType);
    Type cElem = elementMemRef(cType);

    if (auto existing = module.lookupSymbol<func::FuncOp>(kMatmulSymbol)) {
      FunctionType existingType = existing.getFunctionType();
      if (existingType.getInput(0) != aElem ||
          existingType.getInput(3) != bElem ||
          existingType.getInput(6) != cElem) {
        existing->emitError()
            << "gemmini: existing " << kMatmulSymbol
            << " declaration does not match this dispatch's buffer types";
        return nullptr;
      }
      return existing;
    }

    MLIRContext *context = module.getContext();
    Type indexType = builder.getIndexType();
    FunctionType fnType = builder.getFunctionType(
        {aElem, indexType, indexType, bElem, indexType, indexType,
         cElem, indexType, indexType, indexType, indexType, indexType},
        {});

    SmallVector<Type> plannedInputs(fnType.getInputs().begin(), fnType.getInputs().end());
    for (int i = 0; i < 6; ++i) plannedInputs.push_back(builder.getIndexType());
    fnType = builder.getFunctionType(plannedInputs, fnType.getResults());
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    auto callee = func::FuncOp::create(moduleBuilder, module.getLoc(),
                                       kMatmulSymbol, fnType);
    // Declarations cannot be public; see the import note in the pass
    // description.
    callee.setPrivate();
    callee->setAttr("hal.import.static", UnitAttr::get(context));
    callee->setAttr("llvm.bareptr", UnitAttr::get(context));
    return callee;
  }

  // gemmini_conv2d_i8(in, in_off, in_rs, in_ws, w, w_off, w_rs, out, out_off,
  //                   out_rs, H, W, C, OH, OW, OC, KH, KW, stride)
  func::FuncOp getOrCreateConvDecl(FunctionOpInterface funcOp,
                                   OpBuilder &builder, MemRefType inType,
                                   MemRefType wType, MemRefType outType) {
    ModuleOp module = funcOp->getParentOfType<ModuleOp>();
    if (!module) return nullptr;

    // Same memory-space note as getOrCreateKernelDecl: the declaration must
    // use the default space even though the value may still carry a HAL
    // descriptor type at this point.
    auto elementMemRef = [](MemRefType type) {
      return MemRefType::get({}, type.getElementType());
    };
    Type inElem = elementMemRef(inType);
    Type wElem = elementMemRef(wType);
    Type outElem = elementMemRef(outType);

    if (auto existing = module.lookupSymbol<func::FuncOp>(kConvSymbol)) {
      FunctionType existingType = existing.getFunctionType();
      if (existingType.getInput(0) != inElem ||
          existingType.getInput(4) != wElem ||
          existingType.getInput(7) != outElem) {
        existing->emitError()
            << "gemmini: existing " << kConvSymbol
            << " declaration does not match this dispatch's buffer types";
        return nullptr;
      }
      return existing;
    }

    MLIRContext *context = module.getContext();
    Type indexType = builder.getIndexType();
    SmallVector<Type> inputs{inElem, indexType, indexType, indexType,
                             wElem, indexType, indexType,
                             outElem, indexType, indexType};
    for (int i = 0; i < 9; ++i) inputs.push_back(indexType);

    for (int i = 0; i < 6; ++i) inputs.push_back(indexType);
    OpBuilder moduleBuilder = OpBuilder::atBlockEnd(module.getBody());
    auto callee = func::FuncOp::create(
        moduleBuilder, module.getLoc(), kConvSymbol,
        builder.getFunctionType(inputs, {}));
    callee.setPrivate();
    callee->setAttr("hal.import.static", UnitAttr::get(context));
    callee->setAttr("llvm.bareptr", UnitAttr::get(context));
    return callee;
  }
  func::FuncOp getOrCreateConvRequantDecl(FunctionOpInterface function,OpBuilder &builder,
                                         MemRefType in,MemRefType weight,MemRefType out) {
    ModuleOp module=function->getParentOfType<ModuleOp>();
    if (auto existing=module.lookupSymbol<func::FuncOp>(kConvRequantSymbol)) return existing;
    Type idx=builder.getIndexType(),i32=builder.getI32Type(),i64=builder.getI64Type();
    auto elem=[](Type type){return MemRefType::get({},type);};
    SmallVector<Type> inputs{elem(in.getElementType()),idx,idx,idx,
        elem(weight.getElementType()),idx,idx,elem(out.getElementType()),idx,idx};
    for(int i=0;i<9;++i)inputs.push_back(idx);
    inputs.append({elem(i32),idx,i64,i64,i64,i32,i32});
    for(int i=0;i<6;++i)inputs.push_back(idx);
    OpBuilder atEnd=OpBuilder::atBlockEnd(module.getBody());
    auto callee=func::FuncOp::create(atEnd,module.getLoc(),kConvRequantSymbol,builder.getFunctionType(inputs,{}));
    callee.setPrivate();callee->setAttr("hal.import.static",builder.getUnitAttr());
    callee->setAttr("llvm.bareptr",builder.getUnitAttr());return callee;
  }

};

}  // namespace

std::unique_ptr<InterfacePass<mlir::FunctionOpInterface>>
createGemminiLowerDispatchPass() {
  return std::make_unique<GemminiLowerDispatchPass>();
}

}  // namespace mlir::gemmini_compiler
