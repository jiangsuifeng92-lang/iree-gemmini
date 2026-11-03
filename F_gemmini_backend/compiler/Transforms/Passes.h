// Copyright 2026 The Gemmini Backend Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef COMPILER_TRANSFORMS_PASSES_H_
#define COMPILER_TRANSFORMS_PASSES_H_

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Pass/Pass.h"

namespace mlir::gemmini_compiler {

// Registers every pass defined in Passes.td with MLIR's global registry, so
// `iree-opt --gemmini-select` works for debugging.
void registerGemminiPasses();

#define GEN_PASS_DECL
#include "compiler/Transforms/Passes.h.inc"

std::unique_ptr<InterfacePass<mlir::FunctionOpInterface>>
createGemminiSelectPass();

std::unique_ptr<InterfacePass<mlir::FunctionOpInterface>>
createGemminiLowerDispatchPass();

std::unique_ptr<InterfacePass<mlir::FunctionOpInterface>>
createGemminiTilePlanningPass();
std::unique_ptr<InterfacePass<mlir::FunctionOpInterface>>
createGemminiLayoutPlanningPass();

std::unique_ptr<OperationPass<mlir::ModuleOp>> createGemminiRestoreConvolutionPass();
std::unique_ptr<OperationPass<mlir::ModuleOp>> createGemminiGraphFusionPass();
}  // namespace mlir::gemmini_compiler

#endif  // COMPILER_TRANSFORMS_PASSES_H_
