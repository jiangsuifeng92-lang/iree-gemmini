// Copyright 2026 The Gemmini Backend Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

include "mlir/Pass/PassBase.td"

#ifndef IREE_GEMMINI_PASSES
#define IREE_GEMMINI_PASSES

def GemminiSelect : InterfacePass<"gemmini-select", "mlir::FunctionOpInterface"> {
  let summary = "Mark dispatches that should run on the Gemmini systolic array";

  let description = [{
    Runs in the HAL configuration pipeline, after LLVMCPU's own configuration
    pipeline, and answers one question per dispatch: can Gemmini run this?

    A dispatch is selected when it is a plain integer contraction
    (i8 x i8 -> i32), has fully static shape, and its accumulator fits the
    scratch bound that the backend advertises. The selection is recorded in
    two attributes:

      gemmini.select           unit attribute; consumed by gemmini-lower-dispatch
      translation_info         set to `None` so LLVMCPU's lowering pass skips
                               the function entirely and only the final
                               lowering-to-LLVM step runs over it

    Anything not selected keeps its LLVMCPU configuration untouched and is
    compiled to scalar Rocket code exactly as before, so a partial selection is
    always safe: it degrades to the status quo rather than to wrong results.
  }];

  let constructor = [{
    ::mlir::gemmini_compiler::createGemminiSelectPass()
  }];
}

def GemminiLowerDispatch : InterfacePass<"gemmini-lower-dispatch", "mlir::FunctionOpInterface"> {
  let summary = "Rewrite selected contractions into calls to the Gemmini kernels";

  let description = [{
    Runs at the start of the HAL translation pipeline, before LLVMCPU's
    translation pipeline. For a function marked by gemmini-select it
    bufferizes the dispatch, replaces the root contraction with a call to the
    external runtime kernel, and fuses supported bias/requantize/clamp epilogues into the planned
    kernel call. Unsupported epilogues remain normal Rocket code.

    LLVMCPU's lowering then sees `translation_info = None`, adds no compute
    pipeline, and only runs the final lowering-to-LLVM conversion. Because the
    callee is declared with `hal.import.static`, the call survives into the
    object file as an undefined symbol that the final bare-metal link resolves
    against kernels/gemmini_matmul.c.
  }];

  let constructor = [{
    ::mlir::gemmini_compiler::createGemminiLowerDispatchPass()
  }];
}

def GemminiTilePlanning : InterfacePass<"gemmini-tile-planning", "mlir::FunctionOpInterface"> {
  let summary = "Choose bounded WS hardware tiles and reusable row workspace at compile time";
  let constructor = [{ ::mlir::gemmini_compiler::createGemminiTilePlanningPass() }];
}
def GemminiLayoutPlanning : InterfacePass<"gemmini-layout-planning", "mlir::FunctionOpInterface"> {
  let summary = "Classify strided matrix layouts and forward provably redundant local copies";
  let constructor = [{ ::mlir::gemmini_compiler::createGemminiLayoutPlanningPass() }];
}
def GemminiRestoreConvolution : Pass<"gemmini-restore-convolution", "mlir::ModuleOp"> {
 let summary = "Restore static NHWC convolution from ordered StableHLO im2col producers";
 let constructor = [{ ::mlir::gemmini_compiler::createGemminiRestoreConvolutionPass() }];
}
def GemminiGraphFusion : Pass<"gemmini-graph-fusion", "mlir::ModuleOp"> {
 let summary = "Fuse single-consumer integer elementwise producers before dispatch creation";
 let constructor = [{ ::mlir::gemmini_compiler::createGemminiGraphFusionPass() }];
}
#endif  // IREE_GEMMINI_PASSES
