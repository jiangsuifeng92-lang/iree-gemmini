// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "compiler/Target/GemminiTargetBackend.h"
#include "compiler/Transforms/Passes.h"

// IREE headers
#include "iree/compiler/Dialect/HAL/IR/HALTypes.h"
#include "iree/compiler/Dialect/HAL/Target/TargetRegistry.h"
#include "iree/compiler/Dialect/Util/IR/UtilOps.h"
#include "iree/compiler/PluginAPI/Client.h"

// MLIR headers
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Pass/Pass.h"

// LLVM headers
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "gemmini-target"

using namespace mlir;
using namespace mlir::iree_compiler;

namespace mlir::gemmini_compiler {

namespace {

struct GemminiTargetDevice final : public IREE::HAL::TargetDevice {
  GemminiTargetDevice(const GemminiOptions & /*options*/) {}

  IREE::HAL::DeviceTargetAttr getDefaultDeviceTarget(
      MLIRContext *context,
      const IREE::HAL::TargetRegistry &targetRegistry) const override {
    Builder b(context);
    auto configAttr = b.getDictionaryAttr({});

    SmallVector<IREE::HAL::ExecutableTargetAttr> executableTargetAttrs;
    targetRegistry.getTargetBackend("gemmini")
        ->getDefaultExecutableTargets(context, "gemmini", configAttr,
                                      executableTargetAttrs);

    return IREE::HAL::DeviceTargetAttr::get(context,
                                            b.getStringAttr("gemmini"),
                                            configAttr, executableTargetAttrs);
  }
};

struct GemminiSession
    : public PluginSession<GemminiSession, GemminiOptions,
                           PluginActivationPolicy::DefaultActivated> {
  static void registerPasses() { registerGemminiPasses(); }

  void populateHALTargetDevices(IREE::HAL::TargetDeviceList &targets) override {
    targets.add("gemmini", [=]() {
      return std::make_shared<GemminiTargetDevice>(options);
    });
  }

  virtual void populateHALTargetBackends(
      IREE::HAL::TargetBackendList &targets) override {
    targets.add("gemmini", [=]() {
      return std::make_shared<GemminiTargetBackend>(options);
    });
  }

  virtual void onRegisterDialects(DialectRegistry &registry) override {
    // TODO:
  }

  LogicalResult onActivate() override {
    LLVM_DEBUG(llvm::dbgs() << "Gemmini plugin activated\n");
    if (failed(options.validate(context))) {
      return failure();
    }
    return success();
  }

  // Gemmini dispatch placement happens per-executable in the backend's
  // configuration pipeline, not as a module-level preprocessing pass, so there
  // is nothing to add here yet.
  void extendInputConversionPreprocessingPassPipeline(
      OpPassManager &passManager, InputDialectOptions::Type inputType) override {
    passManager.addPass(createGemminiRestoreConvolutionPass());
  }
  void extendPreprocessingPassPipeline(OpPassManager &passManager) override {
    passManager.addPass(createGemminiGraphFusionPass());
  }
};

}  // namespace
}  // namespace mlir::gemmini_compiler

IREE_DEFINE_COMPILER_OPTION_FLAGS(mlir::gemmini_compiler::GemminiOptions);

extern "C" bool iree_register_compiler_plugin_gemmini(
    mlir::iree_compiler::PluginRegistrar *registrar) {
  registrar->registerPlugin<mlir::gemmini_compiler::GemminiSession>(
      "gemmini");
  return true;
}
