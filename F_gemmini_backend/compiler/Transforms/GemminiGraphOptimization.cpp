// Copyright 2026 The Gemmini Backend Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "compiler/Transforms/Passes.h"
#include "gemmini_config.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/AsmParser/AsmParser.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include <cstdlib>
#include <fstream>

namespace mlir::gemmini_compiler {
#define GEN_PASS_DEF_GEMMINIRESTORECONVOLUTION
#define GEN_PASS_DEF_GEMMINIGRAPHFUSION
#include "compiler/Transforms/Passes.h.inc"
namespace {
static bool named(Operation *op, StringRef name) {
  return op && op->getName().getStringRef() == name;
}
static void record(Operation *op, const char *kind, int64_t count) {
  if (const char *path = std::getenv("H_GEMMINI_GRAPH_REPORT")) {
    std::ofstream out(path, std::ios::app);
    out << "{\"event\":\"" << kind << "\",\"count\":" << count << "}\n";
  }
}

/* Reverse the exporter's explicit im2col chain before dispatch formation.
 * This is a structural proof, not a pattern on names/dispatch ordinals.
 * Only static batch-one NHWC, complete ordered windows, and ordinary signed
 * i8 x i8 -> i32 dot products qualify. Shared producers remain intact. */
struct RestoreIm2col : RewritePattern {
  RestoreIm2col(MLIRContext *ctx) : RewritePattern("stablehlo.dot_general", 1, ctx) {}
  LogicalResult matchAndRewrite(Operation *dot, PatternRewriter &r) const override {
    if (dot->getNumOperands() != 2 || dot->getNumResults() != 1) return failure();
    auto lt = dyn_cast<RankedTensorType>(dot->getOperand(0).getType());
    auto wt = dyn_cast<RankedTensorType>(dot->getOperand(1).getType());
    auto ot = dyn_cast<RankedTensorType>(dot->getResult(0).getType());
    if (!lt || !wt || !ot || lt.getRank()!=2 || wt.getRank()!=2 || ot.getRank()!=2 ||
        !lt.hasStaticShape() || !wt.hasStaticShape() || !ot.hasStaticShape() ||
        !lt.getElementType().isInteger(8) || !wt.getElementType().isInteger(8) ||
        !ot.getElementType().isInteger(32)) return failure();
    auto expected = parseAttribute("#stablehlo.dot<lhs_batching_dimensions = [], rhs_batching_dimensions = [], lhs_contracting_dimensions = [1], rhs_contracting_dimensions = [0]>", r.getContext());
    if (dot->getAttr("dot_dimension_numbers") != expected) return failure();
    if (dot->hasAttr("algorithm")) return failure();
    Operation *concat = dot->getOperand(0).getDefiningOp();
    if (!named(concat, "stablehlo.concatenate")) return failure();
    auto dim = concat->getAttrOfType<IntegerAttr>("dimension");
    if (!dim || dim.getInt()!=1 || !concat->getNumOperands()) return failure();
    Value source;
    int64_t oh=0, ow=0, channels=0, stride=0, kh=0, kw=0;
    SmallVector<std::pair<int64_t,int64_t>> offsets;
    for (Value v : concat->getOperands()) {
      Operation *reshape = v.getDefiningOp();
      if (!named(reshape,"stablehlo.reshape") || reshape->getNumOperands()!=1) return failure();
      Operation *slice = reshape->getOperand(0).getDefiningOp();
      if (!named(slice,"stablehlo.slice") || slice->getNumOperands()!=1) return failure();
      auto st = dyn_cast<RankedTensorType>(slice->getResult(0).getType());
      auto rt = dyn_cast<RankedTensorType>(v.getType());
      auto start = slice->getAttrOfType<DenseI64ArrayAttr>("start_indices");
      auto limit = slice->getAttrOfType<DenseI64ArrayAttr>("limit_indices");
      auto steps = slice->getAttrOfType<DenseI64ArrayAttr>("strides");
      if (!st || !rt || st.getRank()!=4 || rt.getRank()!=2 || !st.hasStaticShape() ||
          st.getShape()[0]!=1 || !start || !limit || !steps ||
          start.size()!=4 || limit.size()!=4 || steps.size()!=4) return failure();
      Value input = slice->getOperand(0);
      auto it = dyn_cast<RankedTensorType>(input.getType());
      if (!it || it.getRank()!=4 || !it.hasStaticShape() || it.getShape()[0]!=1) return failure();
      if (!source) {
        source=input;oh=st.getShape()[1];ow=st.getShape()[2];channels=st.getShape()[3];stride=steps[1];
      }
      if (input!=source || !oh || !ow || !channels || channels!=it.getShape()[3] ||
          st.getShape()[1]!=oh || st.getShape()[2]!=ow || st.getShape()[3]!=channels ||
          rt.getShape()[0]!=oh*ow || rt.getShape()[1]!=channels ||
          steps[0]!=1 || steps[3]!=1 || steps[1]!=stride || steps[2]!=stride ||
          (stride!=1 && stride!=2) || start[0]!=0 || start[3]!=0 ||
          limit[0]!=1 || limit[3]!=channels || start[1]<0 || start[2]<0 ||
          limit[1]>it.getShape()[1] || limit[2]>it.getShape()[2] ||
          (limit[1]-start[1]+stride-1)/stride!=oh ||
          (limit[2]-start[2]+stride-1)/stride!=ow) return failure();
      offsets.emplace_back(start[1],start[2]);
      kh=std::max(kh,start[1]+1);kw=std::max(kw,start[2]+1);
    }
    auto it=cast<RankedTensorType>(source.getType());
    int64_t k=kh*kw*channels, n=wt.getShape()[1];
    if (k<=0 || k>H_GEMMINI_MAX_K || n<=0 || n>H_GEMMINI_MAX_N ||
        (int64_t)offsets.size()!=kh*kw || lt.getShape()[0]!=oh*ow || lt.getShape()[1]!=k ||
        wt.getShape()[0]!=k || ot.getShape()[0]!=oh*ow || ot.getShape()[1]!=n ||
        oh!=(it.getShape()[1]-kh)/stride+1 || ow!=(it.getShape()[2]-kw)/stride+1)
      return failure();
    for (int64_t i=0;i<kh*kw;++i)
      if (offsets[i]!=std::make_pair(i/kw,i%kw)) return failure();

    Location loc=dot->getLoc();
    auto input3=RankedTensorType::get({it.getShape()[1],it.getShape()[2],channels},it.getElementType());
    auto weight4=RankedTensorType::get({kh,kw,channels,n},wt.getElementType());
    auto output3=RankedTensorType::get({oh,ow,n},ot.getElementType());
    Value a=r.create<tensor::CollapseShapeOp>(loc,input3,source,
               ArrayRef<ReassociationIndices>{{0,1},{2},{3}});
    Value b=r.create<tensor::ExpandShapeOp>(loc,weight4,dot->getOperand(1),
               ArrayRef<ReassociationIndices>{{0,1,2},{3}});
    Value empty=r.create<tensor::EmptyOp>(loc,output3.getShape(),output3.getElementType());
    Value zero=r.create<arith::ConstantIntOp>(loc,0,32);
    Value init=r.create<linalg::FillOp>(loc,ValueRange{zero},ValueRange{empty}).getResult(0);
    SmallVector<AffineExpr> d;
    for (int i=0;i<6;++i)d.push_back(getAffineDimExpr(i,r.getContext()));
    auto spatial=[&](AffineExpr o,AffineExpr window){return stride==1?o+window:o*stride+window;};
    SmallVector<AffineMap> maps{
      AffineMap::get(6,0,{spatial(d[0],d[3]),spatial(d[1],d[4]),d[5]},r.getContext()),
      AffineMap::get(6,0,{d[3],d[4],d[5],d[2]},r.getContext()),
      AffineMap::get(6,0,{d[0],d[1],d[2]},r.getContext())};
    SmallVector<utils::IteratorType> loops{utils::IteratorType::parallel,utils::IteratorType::parallel,
      utils::IteratorType::parallel,utils::IteratorType::reduction,utils::IteratorType::reduction,utils::IteratorType::reduction};
    auto conv=r.create<linalg::GenericOp>(loc,TypeRange{output3},ValueRange{a,b},ValueRange{init},maps,loops,
      [&](OpBuilder &builder,Location l,ValueRange args){
        Value x=builder.create<arith::ExtSIOp>(l,builder.getI32Type(),args[0]);
        Value y=builder.create<arith::ExtSIOp>(l,builder.getI32Type(),args[1]);
        Value product=builder.create<arith::MulIOp>(l,x,y);
        Value sum=builder.create<arith::AddIOp>(l,product,args[2]);
        builder.create<linalg::YieldOp>(l,sum);
      });
    Value flat=r.create<tensor::CollapseShapeOp>(loc,ot,conv.getResult(0),
               ArrayRef<ReassociationIndices>{{0,1},{2}});
    record(dot,"restore_im2col_convolution",kh*kw);
    r.replaceOp(dot,flat);
    return success();
  }
};
struct GemminiRestoreConvolutionPass : impl::GemminiRestoreConvolutionBase<GemminiRestoreConvolutionPass> {
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<arith::ArithDialect,linalg::LinalgDialect,tensor::TensorDialect>();
  }
  void runOnOperation() override {
    if (std::getenv("H_GEMMINI_NO_GRAPH_OPT")) return;
    RewritePatternSet patterns(&getContext()); patterns.add<RestoreIm2col>(&getContext());
    if (failed(applyPatternsGreedily(getOperation(),std::move(patterns)))) signalPassFailure();
  }
};
struct GemminiGraphFusionPass : impl::GemminiGraphFusionBase<GemminiGraphFusionPass> {
  void runOnOperation() override {
    if (std::getenv("H_GEMMINI_NO_GRAPH_OPT")) return;
    RewritePatternSet patterns(&getContext());
    linalg::populateElementwiseOpsFusionPatterns(patterns,[](OpOperand *operand){
      auto producer=operand->get().getDefiningOp<linalg::GenericOp>();
      auto consumer=dyn_cast<linalg::GenericOp>(operand->getOwner());
      if (!producer || !consumer || !producer.hasPureTensorSemantics() || !consumer.hasPureTensorSemantics() ||
          !operand->get().hasOneUse()) return false;
      auto allParallel=[](linalg::GenericOp op){return llvm::all_of(op.getIteratorTypesArray(),
        [](utils::IteratorType t){return t==utils::IteratorType::parallel;});};
      if (!allParallel(producer) || !allParallel(consumer)) return false;
      // Keep integer epilogues compact; generic contraction/convolution roots
      // are preserved for the device-selection pass later in the pipeline.
      if (producer.getNumDpsInits()!=1 || consumer.getNumDpsInits()!=1) return false;
      auto type=cast<ShapedType>(consumer.getDpsInitOperand(0)->get().getType());
      return type.getElementType().isInteger();
    });
    if (failed(applyPatternsGreedily(getOperation(),std::move(patterns)))) signalPassFailure();
  }
};
} // namespace
std::unique_ptr<OperationPass<ModuleOp>> createGemminiRestoreConvolutionPass(){return std::make_unique<GemminiRestoreConvolutionPass>();}
std::unique_ptr<OperationPass<ModuleOp>> createGemminiGraphFusionPass(){return std::make_unique<GemminiGraphFusionPass>();}
} // namespace mlir::gemmini_compiler
