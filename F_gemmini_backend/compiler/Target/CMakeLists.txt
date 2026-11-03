# The Gemmini target backend. Most of this is the LLVMCPU serialization path
# with the target triple, ABI and output format changed; see the file headers
# for what is inherited and what is ours.

iree_cc_library(
  NAME
    Utils
  HDRS
    "Utils.h"
  SRCS
    "Utils.cpp"
  INCLUDES
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/../include"
  DEPS
    iree::compiler::Dialect::HAL::Target
    MLIRIR
  PUBLIC
)

iree_cc_library(
  NAME
    GemminiTarget
  HDRS
    "GemminiLinkerTool.h"
    "GemminiTargetBackend.h"
    "RegisterAllocationReportCollector.h"
    "RegisterUsageTrackerPass.h"
  SRCS
    "GemminiLinkerTool.cpp"
    "GemminiTarget.cpp"
    "GemminiTargetBackend.cpp"
    "RegisterAllocationReportCollector.cpp"
    "RegisterUsageTrackerPass.cpp"
  INCLUDES
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/../include"
  DEPS
    gemmini_compiler::Target::Utils
    gemmini_compiler::Transforms
    iree::compiler::plugins::target::LLVMCPU::LLVMCPU
    iree::compiler::plugins::target::LLVMCPU::LLVMIRPasses
    iree::compiler::plugins::target::LLVMCPU::LLVMTargetOptions
    iree::compiler::plugins::target::LLVMCPU::LinkerTool
    iree::compiler::plugins::target::LLVMCPU::StaticLibraryGenerator
    iree::compiler::plugins::target::LLVMCPU::Builtins
    iree::compiler::Codegen::Dialect::CPU::IR::IREECPUDialect
    iree::compiler::Codegen::Dialect::Codegen::IR::IREECodegenDialect
    iree::compiler::Codegen::Dialect::VectorExt::IR::IREEVectorExtDialect
    iree::compiler::Codegen::LLVMCPU
    iree::compiler::Codegen::Utils
    iree::compiler::Dialect::Encoding::IR
    iree::compiler::Dialect::HAL::IR
    iree::compiler::Dialect::HAL::Target
    iree::compiler::Dialect::HAL::Utils::LLVMLinkerUtils
    iree::compiler::Dialect::LinalgExt::IR
    iree::compiler::Dialect::Util::IR
    iree::compiler::PluginAPI
    iree::compiler::Utils
    LLVMBitWriter
    LLVMCodeGen
    LLVMCore
    LLVMLinker
    LLVMSupport
    LLVMTarget
    LLVMTargetParser
    MLIRBuiltinToLLVMIRTranslation
    MLIRIR
    MLIRLLVMDialect
    MLIRLLVMToLLVMIRTranslation
    MLIRLinalgDialect
    MLIRPDLDialect
    MLIRPDLInterpDialect
    MLIRPass
    MLIRTargetLLVMIRExport
    MLIRTransformDialect
    MLIRTransforms
  PUBLIC
)

# Registers the plugin: IREE generates StaticLinkedPlugins.inc from this, and
# every compiler binary built here statically links the plugin.
iree_compiler_register_plugin(
  PLUGIN_ID
    gemmini
  TARGET
    ::GemminiTarget
)
