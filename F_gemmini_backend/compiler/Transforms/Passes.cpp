// Copyright 2026 The Gemmini Backend Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "compiler/Transforms/Passes.h"

#include "mlir/Pass/Pass.h"

namespace mlir::gemmini_compiler {

namespace {
#define GEN_PASS_REGISTRATION
#include "compiler/Transforms/Passes.h.inc"
}  // namespace

void registerGemminiPasses() { registerPasses(); }

}  // namespace mlir::gemmini_compiler
