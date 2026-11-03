// Copyright 2026 The Gemmini Backend Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "compiler/Transforms/Passes.h"
#include "gemmini_plan.h"

#include "compiler/Transforms/GemminiConvMatch.h"
#include "iree/compiler/Codegen/Dialect/Codegen/IR/IREECodegenAttrs.h"
#include "llvm/Support/Debug.h"
#include <algorithm>
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Linalg/IR/LinalgInterfaces.h"
#include "mlir/Interfaces/FunctionInterfaces.h"

#define DEBUG_TYPE "iree-gemmini-select"

namespace mlir::gemmini_compiler {

using namespace mlir::iree_compiler;

#define GEN_PASS_DEF_GEMMINISELECT
#include "compiler/Transforms/Passes.h.inc"

namespace {

// Must match GEMMINI_MAX_N / GEMMINI_MAX_K in kernels/gemmini_matmul.c. M is
// not bounded: the kernel streams it in chunks.
constexpr int64_t kMaxTileN = H_GEMMINI_MAX_N;
constexpr int64_t kMaxTileK = H_GEMMINI_MAX_K;
// Mirrors GEMMINI_MAX_IM2COL in kernels/gemmini_matmul.c.
constexpr int64_t kMaxIm2colBytes = H_GEMMINI_IM2COL_ROWS * H_GEMMINI_MAX_K;

// Attribute key shared between this pass and gemmini-lower-dispatch.
constexpr const char *kSelectAttrName = "gemmini.select";

struct GemminiSelectPass : public impl::GemminiSelectBase<GemminiSelectPass> {
  using impl::GemminiSelectBase<GemminiSelectPass>::GemminiSelectBase;

  void runOnOperation() override {
    FunctionOpInterface funcOp = getOperation();

    // Two forms qualify. A contraction is what 1x1 convolutions and plain
    // matmuls lower to; a 3x3 convolution keeps the sliding window in its
    // indexing maps and does not implement the contraction interface, so it
    // needs the explicit matcher.
    linalg::LinalgOp root;
    ConvShape conv;
    bool isConv = false;
    funcOp->walk([&](linalg::LinalgOp op) {
      if (!root && linalg::isaContractionOpInterface(op)) root = op;
    });
    if (!root) {
      funcOp->walk([&](linalg::LinalgOp op) {
        if (root) return;
        if (auto shape = matchGemminiConv(op)) {
          root = op;
          conv = *shape;
          isConv = true;
        }
      });
    }
    if (!root) return;

    std::string rejectReason =
        isConv ? getConvRejectReason(conv) : getRejectReason(root);
    if (!rejectReason.empty()) {
      LLVM_DEBUG(llvm::dbgs() << "gemmini: skipping dispatch: " << rejectReason
                              << "\n");
      return;
    }

    MLIRContext *context = funcOp->getContext();
    funcOp->setAttr(kSelectAttrName, UnitAttr::get(context));

    // This is the whole point: tell LLVMCPU's lowering pass not to add any
    // compute pipeline for this function. The final lowering-to-LLVM still
    // runs, which is what turns the call we insert later into machine code.
    if (failed(iree_compiler::setTranslationInfo(
            funcOp, IREE::Codegen::TranslationInfoAttr::get(
                        context,
                        IREE::Codegen::DispatchLoweringPassPipeline::None,
                        /*workgroupSize=*/SmallVector<int64_t>{1, 1, 1},
                        /*subgroupSize=*/std::nullopt,
                        /*configuration=*/DictionaryAttr())))) {
      funcOp->emitError("gemmini: failed to set translation info");
      return signalPassFailure();
    }

    // Remove LLVMCPU configuration only. H records its own gemmini.plan next;
    // the kernel will receive explicit hardware tiles from that plan.
    root->removeAttr("compilation_info");
    funcOp->walk([&](Operation *op) {
      op->removeAttr("compilation_info");
      op->removeAttr("lowering_config");
    });

    LLVM_DEBUG(llvm::dbgs() << "gemmini: selected dispatch for "
                            << root->getName() << "\n");
  }

 private:
  // The conv kernel gathers the input into an im2col matrix of M x PAD16(K)
  // bytes before handing it to the same tiled_matmul_auto the matmul path
  // uses, so the bound is on that buffer, not on the output.
  std::string getConvRejectReason(const ConvShape &shape) const {
    int64_t K = shape.KH * shape.KW * shape.C;
    int64_t M = shape.OH * shape.OW;
    if (K <= 0 || M <= 0 || shape.OC <= 0) return "empty convolution";
    if (shape.OC > kMaxTileN) {
      return "out channels=" + std::to_string(shape.OC) + " exceed the " +
             std::to_string(kMaxTileN) + "-wide tile";
    }
    if (K > kMaxTileK) {
      return "K=" + std::to_string(K) + " exceeds the " +
             std::to_string(kMaxTileK) + "-deep tile";
    }
    int64_t rows = std::min<int64_t>(M, H_GEMMINI_IM2COL_ROWS);
    if (rows * K > kMaxIm2colBytes) {
      return "im2col would need " + std::to_string(rows * K) +
             " bytes, over the " + std::to_string(kMaxIm2colBytes) + " limit";
    }
    return {};
  }

  // Returns an empty string when the contraction is supported, otherwise the
  // reason it is not. Every rejection falls back to LLVMCPU, which is always
  // numerically correct -- just slower.
  std::string getRejectReason(linalg::LinalgOp root) const {
    auto lhs = dyn_cast<ShapedType>(root->getOperand(0).getType());
    auto rhs = dyn_cast<ShapedType>(root->getOperand(1).getType());
    auto out = dyn_cast<ShapedType>(root->getOperand(2).getType());
    if (!lhs || !rhs || !out) return "operand is not a shaped type";

    // Gemmini only has an int8 datapath with int32 accumulation.
    if (!lhs.getElementType().isInteger(8)) return "lhs is not i8";
    if (!rhs.getElementType().isInteger(8)) return "rhs is not i8";
    if (!out.getElementType().isInteger(32)) return "accumulator is not i32";

    if (!lhs.hasStaticShape() || !rhs.hasStaticShape() || !out.hasStaticShape())
      return "dynamic shape";

    // The kernel ABI is MxK times KxN -> MxN. A rank-1 operand is the same
    // memory layout with the corresponding dimension equal to 1, so all three
    // contraction forms map onto it:
    //   matmul (A[M,K], B[K,N], C[M,N])
    //   vecmat (A[K],   B[K,N], C[N])     -- M == 1
    //   matvec (A[M,K], B[K],   C[M])     -- N == 1
    if (rhs.getRank() != 2) return "rhs is not rank-2";
    if (lhs.getRank() < 1 || lhs.getRank() > 2) return "lhs rank is not 1 or 2";
    if (out.getRank() < 1 || out.getRank() > 2) return "result rank is not 1 or 2";

    // The accumulator size does not bound anything: the kernel streams the
    // output in row chunks (SqueezeNet's 1x1 convolutions become 3025-row
    // contractions). What the kernel genuinely cannot handle is a single tile
    // wider or deeper than its staging buffers.
    int64_t N = out.getRank() == 2 ? out.getShape()[1] : out.getShape()[0];
    int64_t K = lhs.getRank() == 2 ? lhs.getShape()[1] : lhs.getShape()[0];
    if (N <= 0 || K <= 0) return "empty contraction";
    if (N > kMaxTileN) {
      return "N=" + std::to_string(N) + " exceeds the " +
             std::to_string(kMaxTileN) + "-wide tile";
    }
    if (K > kMaxTileK) {
      return "K=" + std::to_string(K) + " exceeds the " +
             std::to_string(kMaxTileK) + "-deep tile";
    }
    return {};
  }
};

}  // namespace

std::unique_ptr<InterfacePass<mlir::FunctionOpInterface>>
createGemminiSelectPass() {
  return std::make_unique<GemminiSelectPass>();
}

}  // namespace mlir::gemmini_compiler
