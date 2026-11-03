// Actual ARM RTL diagnostics, stopping BEFORE any remote NPU execution.
// No Rocket, Gemmini, native executable implementation or reference inference.
#include "Varm_host_axi.h"
#include "Varm_host_axi___024root.h"
#include "verilated.h"
#include "queue_abi.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <unordered_map>
#include <vector>

static std::unordered_map<uint32_t, uint32_t> memory;
static uint32_t load32(uint32_t a) { return memory[a & ~3u]; }
static void store32(uint32_t a, uint32_t value, uint8_t mask = 15) {
  uint32_t &word = memory[a & ~3u];
  for (unsigned lane = 0; lane < 4; ++lane)
    if (mask & (1u << lane)) {
      unsigned shift = lane * 8;
      word = (word & ~(255u << shift)) | (value & (255u << shift));
    }
}
static void check_address(uint32_t a) {
  if ((a >= 0x80000000u && a < 0x90000000u) ||
      (a >= G_MAILBOX_BASE && a < G_MAILBOX_BASE + 65536u) ||
      a == G_DOORBELL_ADDR) return;
  std::fprintf(stderr, "Unmapped address %08x\n", a);
  throw std::runtime_error("unmapped ARM AXI request");
}
static void load_elf(const char *path) {
  std::ifstream input(path, std::ios::binary);
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(input)), {});
  auto u16 = [&](size_t at) { uint16_t v; if (at + 2 > b.size()) throw std::runtime_error("ELF bounds"); std::memcpy(&v, &b[at], 2); return v; };
  auto u32 = [&](size_t at) { uint32_t v; if (at + 4 > b.size()) throw std::runtime_error("ELF bounds"); std::memcpy(&v, &b[at], 4); return v; };
  if (b.size() < 52 || std::memcmp(b.data(), "\177ELF", 4) || b[4] != 1 || b[5] != 1)
    throw std::runtime_error("expected ARM ELF32 little endian");
  for (unsigned i = 0; i < u16(44); ++i) {
    size_t ph = u32(28) + i * u16(42);
    if (u32(ph) != 1) continue;
    uint32_t offset = u32(ph + 4), address = u32(ph + 12), bytes = u32(ph + 16);
    if (uint64_t(offset) + bytes > b.size()) throw std::runtime_error("ELF segment bounds");
    if (address < 0x80000000u) continue; // Private boot RAM uses runtime hex.
    for (uint32_t j = 0; j < bytes; ++j) {
      uint32_t a = address + j; check_address(a);
      store32(a, uint32_t(b[offset + j]) << ((a & 3u) * 8), 1u << (a & 3u));
    }
  }
}
int main(int argc, char **argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: host-debug host.elf +g_host_mem_hex=...\n"); return 2; }
  try {
    load_elf(argv[1]);
    Verilated::commandArgs(argc, argv);
    Varm_host_axi dut;
    dut.irq = 0; dut.rst = 1;
    for (unsigned i = 0; i < 8; ++i) { dut.clk = 0; dut.eval(); dut.clk = 1; dut.eval(); }
    dut.rst = 0;
    store32(G_QUEUE_BASE, G_ABI_MAGIC);
    store32(G_QUEUE_BASE + 4, G_QUEUE_VERSION);
    store32(G_QUEUE_BASE + 8, 1);
    store32(G_QUEUE_BASE + 12, 1);
    std::puts("ARM-only RTL diagnostic: no NPU execution; stop at first submission.");
    bool have_aw = false, have_w = false, bp = false, rp = false;
    uint32_t aw = 0, wd = 0, r_addr = 0; uint8_t ws = 0, r_beats_left = 0;
    uint64_t bd = 0, r_due = 0, reads = 0, writes = 0;
    const char *watch_text = std::getenv("G_ARM_WATCH_PC");
    uint32_t watch_pc = watch_text ? std::strtoul(watch_text, nullptr, 0) : 0;
    for (uint64_t cycle = 0; cycle < 20000000; ++cycle) {
      dut.clk = 0;
      dut.m_axi_awready = !have_aw && !bp && cycle % 3 != 0;
      dut.m_axi_wready = !have_w && !bp && cycle % 5 == 0;
      dut.m_axi_arready = !rp && cycle % 4 != 0;
      dut.m_axi_bresp = 0; dut.m_axi_bvalid = bp && cycle >= bd;
      dut.m_axi_rdata = load32(r_addr); dut.m_axi_rresp = 0;
      dut.m_axi_rlast = r_beats_left == 1;
      dut.m_axi_rvalid = rp && cycle >= r_due; dut.eval();
      bool ah = dut.m_axi_awvalid && dut.m_axi_awready;
      bool wh = dut.m_axi_wvalid && dut.m_axi_wready;
      bool bh = dut.m_axi_bvalid && dut.m_axi_bready;
      bool rh = dut.m_axi_rvalid && dut.m_axi_rready;
      bool ar = dut.m_axi_arvalid && dut.m_axi_arready;
      uint32_t aa = dut.m_axi_awaddr, wv = dut.m_axi_wdata, ra = dut.m_axi_araddr;
      uint8_t arlen = dut.m_axi_arlen;
      uint8_t wm = dut.m_axi_wstrb;
      dut.clk = 1; dut.eval();
      const auto *r = dut.rootp;
      if (std::getenv("G_ARM_TRACE") && cycle > 10000 &&
          r->arm_host_axi__DOT__state == 1 &&
          r->arm_host_axi__DOT__data_addr < 65536 &&
          r->arm_host_axi__DOT__fetch_addr >= 0x88000000u) {
        std::fprintf(stderr, "ARM private access cycle=%llu pc=%08x addr=%08x r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x\n",
          (unsigned long long)cycle, r->arm_host_axi__DOT__fetch_addr,
          r->arm_host_axi__DOT__data_addr,
          r->arm_host_axi__DOT__core__DOT__r0, r->arm_host_axi__DOT__core__DOT__r1,
          r->arm_host_axi__DOT__core__DOT__r2, r->arm_host_axi__DOT__core__DOT__r3,
          r->arm_host_axi__DOT__core__DOT__r4, r->arm_host_axi__DOT__core__DOT__r5,
          r->arm_host_axi__DOT__core__DOT__r6, r->arm_host_axi__DOT__core__DOT__r7);
      }
      if (bh) bp = false;
      if (rh) {
        ++reads;
        if (r_beats_left > 1) {
          --r_beats_left;
          r_addr += 4;
          r_due = cycle + 1;
        } else {
          rp = false;
          r_beats_left = 0;
        }
      }
      if (ah) { aw = aa; have_aw = true; }
      if (wh) { wd = wv; ws = wm; have_w = true; }
      if (have_aw && have_w && !bp) {
        check_address(aw); store32(aw, wd, ws); ++writes;
        if (aw == G_DOORBELL_ADDR && wd) {
          std::printf("ARM reached first NPU submission after %llu cycles; diagnostic STOP, no inference result.\n", (unsigned long long)cycle);
          dut.final(); return 0;
        }
        have_aw = have_w = false; bp = true; bd = cycle + 7;
      }
      if (ar) {
        check_address(ra); r_addr = ra; r_beats_left = arlen + 1;
        rp = true; r_due = cycle + 5;
        if (watch_pc && r->arm_host_axi__DOT__fetch_addr >= watch_pc &&
            r->arm_host_axi__DOT__fetch_addr < watch_pc + 64)
          std::fprintf(stderr, "ARM watched read pc=%08x addr=%08x value=%08x r1=%08x\n",
            r->arm_host_axi__DOT__fetch_addr, ra, load32(ra), r->arm_host_axi__DOT__core__DOT__r1);
      }
      if (dut.debug_status) {
        std::fprintf(stderr, "ARM diagnostic terminal status=%u cycles=%llu reads=%llu writes=%llu\n",
                     dut.debug_status, (unsigned long long)cycle,
                     (unsigned long long)reads, (unsigned long long)writes);
        dut.final(); return 1;
      }
    }
    throw std::runtime_error("ARM diagnostic timed out");
  } catch (const std::exception &e) { std::fprintf(stderr, "ARM diagnostic: %s\n", e.what()); return 1; }
}
