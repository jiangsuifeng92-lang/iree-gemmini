// Copyright 2026 The Gemmini Backend Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef GEMMINI_COMPILER_TRANSFORMS_GEMMINICONVMATCH_H_
#define GEMMINI_COMPILER_TRANSFORMS_GEMMINICONVMATCH_H_

#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Linalg/IR/LinalgInterfaces.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/BuiltinTypes.h"

#include <optional>

namespace mlir::gemmini_compiler {

// The shape of a convolution the Gemmini kernel can run, in the NHWC layout
// the exporter produces.
struct ConvShape {
  int64_t H, W, C;    // input
  int64_t OH, OW, OC; // output
  int64_t KH, KW;     // kernel
  int64_t stride;
  bool singleChannel = false;
};

// Recognizes the convolution form `linalg::isaContractionOpInterface` misses:
// a generic whose indexing maps carry the sliding window explicitly.
//
//   input  map: (d0, d1, d2, d3, d4, d5) -> (d0*s + d3, d1*s + d4, d5)
//   kernel map:                             (d3, d4, d5, d2)
//   output map:                             (d0, d1, d2)
//   iterators : parallel x3, reduction x3
//
// which is exactly how IREE lowers `stablehlo.convolution` with a 3x3 kernel
// and no padding (the exporter pads explicitly, so `padding` is always zero
// here). Anything that does not match this shape is left to LLVMCPU.
// Verify the arithmetic as well as the indexing maps. A matching window
// alone does not prove that an arbitrary linalg.generic computes convolution.
inline bool hasGemminiConvBody(linalg::LinalgOp op) {
  Block &body=op->getRegion(0).front();
  if(body.getNumArguments()!=3)return false;
  for (Operation &inner : body.without_terminator())
    if (!isa<arith::ExtSIOp, arith::MulIOp, arith::AddIOp>(inner))
      return false;
  auto yield=dyn_cast<linalg::YieldOp>(body.getTerminator());
  if(!yield || yield.getNumOperands()!=1)return false;
  auto sum=yield.getOperand(0).getDefiningOp<arith::AddIOp>();
  if(!sum)return false;
  Value product;
  if(sum.getLhs()==body.getArgument(2))product=sum.getRhs();
  else if(sum.getRhs()==body.getArgument(2))product=sum.getLhs();
  else return false;
  auto mul=product.getDefiningOp<arith::MulIOp>();
  if(!mul || !mul.getType().isInteger(32))return false;
  auto lhs=mul.getLhs().getDefiningOp<arith::ExtSIOp>();
  auto rhs=mul.getRhs().getDefiningOp<arith::ExtSIOp>();
  if(!lhs || !rhs)return false;
  return (lhs.getIn()==body.getArgument(0) && rhs.getIn()==body.getArgument(1)) ||
         (lhs.getIn()==body.getArgument(1) && rhs.getIn()==body.getArgument(0));
}

inline std::optional<ConvShape> matchGemminiConv(linalg::LinalgOp op) {
  if (op.getNumDpsInputs() != 2 || op.getNumDpsInits() != 1)
    return std::nullopt;

  auto inType = dyn_cast<ShapedType>(op.getDpsInputOperand(0)->get().getType());
  auto kType = dyn_cast<ShapedType>(op.getDpsInputOperand(1)->get().getType());
  auto oType = dyn_cast<ShapedType>(op.getDpsInitOperand(0)->get().getType());
  if (!inType || !kType || !oType) return std::nullopt;

  if(!hasGemminiConvBody(op))return std::nullopt;
  // IREE folds a unit input-channel dimension out of the first MNIST conv.
  // The physical memory layout is unchanged; handle its five-loop form.
  if(inType.getRank()==2 && kType.getRank()==3 && oType.getRank()==3 &&
      inType.hasStaticShape() && kType.hasStaticShape() && oType.hasStaticShape() &&
      inType.getElementType().isInteger(8) && kType.getElementType().isInteger(8) &&
      oType.getElementType().isInteger(32)) {
    auto ctx=op.getContext();auto maps=op.getIndexingMapsArray();
    auto iters=op.getIteratorTypesArray();
    if(maps.size()!=3 || iters.size()!=5)return std::nullopt;
    for(int i=0;i<5;++i)
      if(iters[i]!=(i<3?utils::IteratorType::parallel:utils::IteratorType::reduction))return std::nullopt;
    AffineExpr d0=getAffineDimExpr(0,ctx),d1=getAffineDimExpr(1,ctx),d2=getAffineDimExpr(2,ctx),
               d3=getAffineDimExpr(3,ctx),d4=getAffineDimExpr(4,ctx);
    if(maps[1]!=AffineMap::get(5,0,{d3,d4,d2},ctx) || maps[2]!=AffineMap::get(5,0,{d0,d1,d2},ctx))return std::nullopt;
    for(int64_t stride:{int64_t{1},int64_t{2}}) {
      auto spatial=[&](AffineExpr out,AffineExpr k){return stride==1?out+k:out*stride+k;};
      if(maps[0]!=AffineMap::get(5,0,{spatial(d0,d3),spatial(d1,d4)},ctx))continue;
      ConvShape shape;
      shape.H=inType.getShape()[0];shape.W=inType.getShape()[1];shape.C=1;
      shape.KH=kType.getShape()[0];shape.KW=kType.getShape()[1];shape.OC=kType.getShape()[2];
      shape.OH=oType.getShape()[0];shape.OW=oType.getShape()[1];shape.stride=stride;shape.singleChannel=true;
      if(oType.getShape()[2]!=shape.OC || shape.OH!=(shape.H-shape.KH)/stride+1 ||
         shape.OW!=(shape.W-shape.KW)/stride+1)return std::nullopt;
      return shape;
    }
    return std::nullopt;
  }
  if (inType.getRank() != 3 || kType.getRank() != 4 || oType.getRank() != 3)
    return std::nullopt;
  if (!inType.hasStaticShape() || !kType.hasStaticShape() ||
      !oType.hasStaticShape())
    return std::nullopt;

  // Gemmini's datapath is int8 in, int32 accumulate.
  if (!inType.getElementType().isInteger(8)) return std::nullopt;
  if (!kType.getElementType().isInteger(8)) return std::nullopt;
  if (!oType.getElementType().isInteger(32)) return std::nullopt;

  SmallVector<AffineMap> maps = op.getIndexingMapsArray();
  if (maps.size() != 3 || maps[0].getNumDims() != 6) return std::nullopt;

  MLIRContext *ctx = op->getContext();
  AffineExpr d0 = getAffineDimExpr(0, ctx), d1 = getAffineDimExpr(1, ctx),
             d2 = getAffineDimExpr(2, ctx), d3 = getAffineDimExpr(3, ctx),
             d4 = getAffineDimExpr(4, ctx), d5 = getAffineDimExpr(5, ctx);

  if (maps[2] != AffineMap::get(6, 0, {d0, d1, d2}, ctx)) return std::nullopt;
  if (maps[1] != AffineMap::get(6, 0, {d3, d4, d5, d2}, ctx))
    return std::nullopt;

  SmallVector<utils::IteratorType> iters = op.getIteratorTypesArray();
  if (iters.size() != 6) return std::nullopt;
  for (int i = 0; i < 3; ++i)
    if (iters[i] != utils::IteratorType::parallel) return std::nullopt;
  for (int i = 3; i < 6; ++i)
    if (iters[i] != utils::IteratorType::reduction) return std::nullopt;

  // Built per stride rather than simplifying: neither form is canonicalized by
  // AffineMap::get, and the IR spells stride 1 as `d0 + d3` (no `* 1`).
  auto spatial = [&](AffineExpr out, AffineExpr k, int64_t s) {
    return s == 1 ? AffineExpr(out + k) : AffineExpr(out * s + k);
  };
  int64_t stride = 0;
  for (int64_t s : {int64_t{1}, int64_t{2}}) {
    if (maps[0] == AffineMap::get(
                      6, 0, {spatial(d0, d3, s), spatial(d1, d4, s), d5}, ctx)) {
      stride = s;
      break;
    }
  }
  if (stride == 0) return std::nullopt;

  ConvShape shape;
  shape.H = inType.getShape()[0];
  shape.W = inType.getShape()[1];
  shape.C = inType.getShape()[2];
  shape.KH = kType.getShape()[0];
  shape.KW = kType.getShape()[1];
  shape.OC = kType.getShape()[3];
  shape.OH = oType.getShape()[0];
  shape.OW = oType.getShape()[1];
  shape.stride = stride;

  // The channels and the kernel's input-channel dimension have to agree, and
  // the output spatial size has to follow from the input one.
  if (kType.getShape()[2] != shape.C) return std::nullopt;
  if (oType.getShape()[2] != shape.OC) return std::nullopt;
  if (shape.OH != (shape.H - shape.KH) / stride + 1) return std::nullopt;
  if (shape.OW != (shape.W - shape.KW) / stride + 1) return std::nullopt;

  return shape;
}

}  // namespace mlir::gemmini_compiler

#endif  // GEMMINI_COMPILER_TRANSFORMS_GEMMINICONVMATCH_H_
