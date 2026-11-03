// Copyright 2026 The Gemmini Backend Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "compiler/Transforms/Passes.h"
#include "compiler/Transforms/GemminiConvMatch.h"
#include "gemmini_plan.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Linalg/IR/LinalgInterfaces.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/IR/SymbolTable.h"
#include <cstdlib>
#include <fstream>
#include <mutex>

namespace mlir::gemmini_compiler {
#define GEN_PASS_DEF_GEMMINITILEPLANNING
#define GEN_PASS_DEF_GEMMINILAYOUTPLANNING
#include "compiler/Transforms/Passes.h.inc"
namespace {

static void report(FunctionOpInterface f, const char *event, int64_t m,
                   int64_t n, int64_t k, const h_gemmini_plan_t &p,
                   int forwarded = 0) {
  const char *path = std::getenv("H_GEMMINI_PLAN_REPORT");
  if (!path || !*path) return;
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  std::ofstream out(path, std::ios::app);
  // Compiler-generated names are identifiers; no user strings are serialized.
  out << "{\"event\":\"" << event << "\",\"function\":\""
      << f->getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName()).getValue().str() << "\",\"M\":" << m << ",\"N\":" << n
      << ",\"K\":" << k << ",\"abi\":" << p.abi
      << ",\"tile_i\":" << p.tile_i << ",\"tile_j\":" << p.tile_j
      << ",\"tile_k\":" << p.tile_k << ",\"block_m\":" << p.block_m
      << ",\"layout_flags\":" << p.layout
      << ",\"forwarded_copies\":" << forwarded << "}\n";
}

static linalg::LinalgOp findRoot(FunctionOpInterface f) {
  linalg::LinalgOp root;
  f->walk([&](linalg::LinalgOp op) {
    if (!root && linalg::isaContractionOpInterface(op)) root = op;
  });
  if (!root) f->walk([&](linalg::LinalgOp op) {
    if (!root && matchGemminiConv(op)) root = op;
  });
  return root;
}

static void storePlan(FunctionOpInterface f, const h_gemmini_plan_t &p) {
  Builder b(f->getContext());
  SmallVector<NamedAttribute> fields;
  for (auto entry : {std::pair<const char *, size_t>{"abi", p.abi},
       {"tile_i", p.tile_i}, {"tile_j", p.tile_j}, {"tile_k", p.tile_k},
       {"block_m", p.block_m}, {"layout_flags", p.layout}})
    fields.emplace_back(b.getStringAttr(entry.first), b.getI64IntegerAttr(entry.second));
  f->setAttr("gemmini.plan", b.getDictionaryAttr(fields));
}
static h_gemmini_plan_t loadPlan(FunctionOpInterface f) {
  h_gemmini_plan_t p = {};
  auto attr = f->getAttrOfType<DictionaryAttr>("gemmini.plan");
  if (!attr) return p;
  auto read = [&](const char *key) -> size_t {
    auto v = attr.getAs<IntegerAttr>(key); return v ? v.getInt() : 0;
  };
  p.abi = read("abi"); p.tile_i = read("tile_i");
  p.tile_j = read("tile_j"); p.tile_k = read("tile_k");
  p.block_m = read("block_m"); p.layout = read("layout_flags");
  return p;
}

struct GemminiTilePlanningPass
    : impl::GemminiTilePlanningBase<GemminiTilePlanningPass> {
  void runOnOperation() override {
    auto f = getOperation();
    if (!f->hasAttr("gemmini.select")) return;
    auto root = findRoot(f);
    if (!root) { f->emitError("H: selected dispatch has no root"); return signalPassFailure(); }
    int64_t m, n, k;
    auto conv = matchGemminiConv(root);
    if (conv) {
      m = conv->OH * conv->OW; n = conv->OC; k = conv->KH * conv->KW * conv->C;
    } else {
      auto a = cast<ShapedType>(root->getOperand(0).getType());
      auto c = cast<ShapedType>(root->getOperand(2).getType());
      m = c.getRank() == 2 ? c.getShape()[0] : 1;
      n = c.getRank() == 2 ? c.getShape()[1] : c.getShape()[0];
      k = a.getRank() == 2 ? a.getShape()[1] : a.getShape()[0];
    }
    auto p = h_gemmini_choose_plan(m, n, k, conv.has_value());
    if (!h_gemmini_plan_valid(&p, m, n, k)) {
      f->emitError("H: no legal WS tile plan for copied hardware");
      return signalPassFailure();
    }
    storePlan(f, p);
    report(f, "tile", m, n, k, p);
  }
};

/* Local producer/consumer optimization only. Forward an allocation's input
 * copy when the contraction is its only reader and no side effects intervene.
 * Never reuse a source that aliases the output or another root operand. */
static bool forwardInputCopy(linalg::LinalgOp root, unsigned operand) {
  Value temporary = root->getOperand(operand);
  if (!temporary.getDefiningOp<memref::AllocOp>() &&
      !temporary.getDefiningOp<memref::AllocaOp>()) return false;
  memref::CopyOp copy;
  for (OpOperand &use : temporary.getUses()) {
    Operation *user = use.getOwner();
    if (user == root.getOperation() && use.getOperandNumber() == operand) continue;
    if (isa<memref::DeallocOp>(user)) continue;
    auto candidate = dyn_cast<memref::CopyOp>(user);
    if (!candidate || candidate.getTarget() != temporary || copy) return false;
    copy = candidate;
  }
  if (!copy || copy->getBlock() != root->getBlock() ||
      !copy->isBeforeInBlock(root.getOperation())) return false;
  Value source = copy.getSource();
  auto type = dyn_cast<MemRefType>(source.getType());
  auto oldType = cast<MemRefType>(temporary.getType());
  if (!type || type.getShape() != oldType.getShape() ||
      type.getElementType() != oldType.getElementType()) return false;
  // An arbitrary view can alias other buffers; V1 only forwards independent
  // allocations, which provides a simple explicit alias proof.
  if (!source.getDefiningOp<memref::AllocOp>() &&
      !source.getDefiningOp<memref::AllocaOp>()) return false;
  auto allocationBase = [](Value value) -> Value {
    while (Operation *definition = value.getDefiningOp()) {
      if (isa<memref::AllocOp, memref::AllocaOp>(definition)) return value;
      if (auto op = dyn_cast<memref::SubViewOp>(definition)) value = op.getSource();
      else if (auto op = dyn_cast<memref::CastOp>(definition)) value = op.getSource();
      else if (auto op = dyn_cast<memref::ReinterpretCastOp>(definition)) value = op.getSource();
      else if (auto op = dyn_cast<memref::MemorySpaceCastOp>(definition)) value = op.getSource();
      else return {};
    }
    return {};
  };
  for (unsigned i = 0; i < root->getNumOperands(); ++i) {
    if (i == operand) continue;
    Value base = allocationBase(root->getOperand(i));
    if (!base || base == source) return false;
  }
  for (Operation *op = copy->getNextNode(); op && op != root.getOperation();
       op = op->getNextNode())
    if (!isMemoryEffectFree(op)) return false;
  root->setOperand(operand, source);
  copy.erase();
  return true;
}

struct GemminiLayoutPlanningPass
    : impl::GemminiLayoutPlanningBase<GemminiLayoutPlanningPass> {
  void runOnOperation() override {
    auto f = getOperation();
    if (!f->hasAttr("gemmini.select")) return;
    auto root = findRoot(f);
    if (!root) return;
    auto p = loadPlan(f);
    if (!p.abi) { f->emitError("H: missing compile-time plan"); return signalPassFailure(); }
    auto conv = matchGemminiConv(root);
    int64_t m, n, k;
    if (conv) {
      m = conv->OH * conv->OW; n = conv->OC; k = conv->KH * conv->KW * conv->C;
      // im2col is produced densely by the tiled convolution kernel.
      auto outType = cast<MemRefType>(root->getOperand(2).getType());
      SmallVector<int64_t> strides; int64_t offset;
      bool directOut = succeeded(outType.getStridesAndOffset(strides, offset)) &&
                       strides.size() == 3 && strides[1] == n;
      p.layout = H_GEMMINI_DIRECT_A | H_GEMMINI_DIRECT_B |
                 (directOut ? H_GEMMINI_DIRECT_C : H_GEMMINI_PACK_C);
      storePlan(f, p); report(f, "layout", m, n, k, p); return;
    }
    int forwarded = forwardInputCopy(root, 0) + forwardInputCopy(root, 1);
    auto a = cast<MemRefType>(root->getOperand(0).getType());
    auto c = cast<MemRefType>(root->getOperand(2).getType());
    m = c.getRank() == 2 ? c.getShape()[0] : 1;
    n = c.getRank() == 2 ? c.getShape()[1] : c.getShape()[0];
    k = a.getRank() == 2 ? a.getShape()[1] : a.getShape()[0];
    const unsigned direct[] = {H_GEMMINI_DIRECT_A, H_GEMMINI_DIRECT_B, H_GEMMINI_DIRECT_C};
    const unsigned pack[] = {H_GEMMINI_PACK_A, H_GEMMINI_PACK_B, H_GEMMINI_PACK_C};
    const int64_t width[] = {k, n, n};
    p.layout = 0;
    for (unsigned i = 0; i < 3; ++i) {
      auto type = cast<MemRefType>(root->getOperand(i).getType());
      SmallVector<int64_t> strides; int64_t offset;
      if (failed(type.getStridesAndOffset(strides, offset)) || strides.back() != 1) {
        root->emitError("H: kernel requires unit inner stride"); return signalPassFailure();
      }
      // Rank-one inputs/outputs contain one matrix row; normalize their row stride.
      int64_t row = type.getRank() == 1 ? width[i] : strides.front();
      if (!ShapedType::isDynamic(row) && row < width[i]) {
        root->emitError("H: overlapping matrix rows are unsupported"); return signalPassFailure();
      }
      p.layout |= row == width[i] ? direct[i] : pack[i];
    }
    storePlan(f, p); report(f, "layout", m, n, k, p, forwarded);
  }
};
} // namespace
std::unique_ptr<InterfacePass<FunctionOpInterface>> createGemminiTilePlanningPass() {
  return std::make_unique<GemminiTilePlanningPass>();
}
std::unique_ptr<InterfacePass<FunctionOpInterface>> createGemminiLayoutPlanningPass() {
  return std::make_unique<GemminiLayoutPlanningPass>();
}
} // namespace mlir::gemmini_compiler
