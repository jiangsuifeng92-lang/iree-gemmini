// Actual ARM RTL and AXI backpressure; NPU is a behavioral endpoint here.
// This executable intentionally does not claim to simulate Rocket/Gemmini RTL.
#include "Varm_host_axi.h"
#include "verilated.h"
#include "device_abi.h"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>

static std::unordered_map<uint32_t, uint32_t> memory;
static uint32_t load32(uint32_t address) { return memory[address & ~3u]; }
static int8_t load8(uint32_t address) {
  return static_cast<int8_t>(load32(address) >> ((address & 3u) * 8));
}
static void store32(uint32_t address, uint32_t value, uint8_t mask = 15) {
  uint32_t &word = memory[address & ~3u];
  for (unsigned i = 0; i < 4; ++i)
    if (mask & (1u << i))
      word = (word & ~(255u << (i * 8))) | (value & (255u << (i * 8)));
}
static void check_address(uint32_t address) {
  bool mailbox = address >= G_MAILBOX_BASE && address < G_MAILBOX_BASE + 65536;
  if (!mailbox && !g_range_in_io(address, 4))
    throw std::runtime_error("ARM issued an unmapped AXI address");
}

static uint32_t behavioral_npu(uint32_t sequence) {
  uint32_t op = load32(G_DESCRIPTOR_BASE);
  uint32_t status = G_STATUS_OK;
  if (op == G_OP_MATMUL_I8) {
    auto field = [](unsigned word) { return load32(G_DESCRIPTOR_BASE + 4 * word); };
    uint32_t a = field(4), b = field(6), c = field(8);
    uint32_t m = field(10), n = field(11), k = field(12);
    if (m != G_DEMO_DIM || n != G_DEMO_DIM || k != G_DEMO_DIM ||
        !g_range_in_io(a, m * k) || !g_range_in_io(b, k * n) ||
        !g_range_in_io(c, m * n * 4))
      throw std::runtime_error("ARM published an invalid matmul descriptor");
    for (uint32_t i = 0; i < m; ++i)
      for (uint32_t j = 0; j < n; ++j) {
        int32_t sum = 0;
        for (uint32_t t = 0; t < k; ++t)
          sum += static_cast<int32_t>(load8(a + i * k + t)) * load8(b + t * n + j);
        store32(c + (i * n + j) * 4, static_cast<uint32_t>(sum));
      }
  } else if (op != G_OP_PING) status = G_STATUS_UNSUPPORTED;
  store32(G_MAILBOX_BASE + offsetof(g_mailbox_t, status), status);
  store32(G_MAILBOX_BASE + offsetof(g_mailbox_t, complete_seq), sequence);
  return op;
}

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  Varm_host_axi dut;
  dut.irq = 0;
  dut.rst = 1;
  for (unsigned i = 0; i < 8; ++i) {
    dut.clk = 0; dut.eval(); dut.clk = 1; dut.eval();
  }
  dut.rst = 0;
  store32(G_MAILBOX_BASE, G_ABI_MAGIC);
  store32(G_MAILBOX_BASE + 4, G_ABI_VERSION);
  store32(G_MAILBOX_BASE + 8, 1);
  store32(G_MAILBOX_BASE + 12, 1);

  bool have_aw = false, have_w = false, b_pending = false, r_pending = false;
  uint32_t aw = 0, wd = 0, rd = 0;
  uint8_t ws = 0;
  uint64_t b_due = 0, r_due = 0, job_due = 0;
  uint32_t job_seq = 0, jobs = 0, reads = 0, writes = 0;
  try {
    for (uint64_t cycle = 0; cycle < 2000000; ++cycle) {
      dut.clk = 0;
      // Different AW/W schedules exercise independent write channels.
      dut.m_axi_awready = !have_aw && !b_pending && cycle % 3 != 0;
      dut.m_axi_wready = !have_w && !b_pending && cycle % 5 == 0;
      dut.m_axi_arready = !r_pending && cycle % 4 != 0;
      dut.m_axi_bresp = 0;
      dut.m_axi_bvalid = b_pending && cycle >= b_due;
      dut.m_axi_rdata = rd;
      dut.m_axi_rresp = 0;
      dut.m_axi_rlast = 1;
      dut.m_axi_rvalid = r_pending && cycle >= r_due;
      dut.eval();
      bool aw_hs = dut.m_axi_awvalid && dut.m_axi_awready;
      bool w_hs = dut.m_axi_wvalid && dut.m_axi_wready;
      bool b_hs = dut.m_axi_bvalid && dut.m_axi_bready;
      bool ar_hs = dut.m_axi_arvalid && dut.m_axi_arready;
      bool r_hs = dut.m_axi_rvalid && dut.m_axi_rready;
      uint32_t aw_address = dut.m_axi_awaddr, w_value = dut.m_axi_wdata;
      uint8_t w_mask = dut.m_axi_wstrb;
      uint32_t ar_address = dut.m_axi_araddr;
      dut.clk = 1; dut.eval();
      if (b_hs) b_pending = false;
      if (r_hs) r_pending = false;
      if (aw_hs) { aw = aw_address; have_aw = true; }
      if (w_hs) { wd = w_value; ws = w_mask; have_w = true; }
      if (have_aw && have_w && !b_pending) {
        check_address(aw); store32(aw, wd, ws); ++writes;
        have_aw = have_w = false; b_pending = true; b_due = cycle + 7;
      }
      if (ar_hs) {
        check_address(ar_address); rd = load32(ar_address); ++reads;
        r_pending = true; r_due = cycle + 5;
      }
      uint32_t seq = load32(G_MAILBOX_BASE + offsetof(g_mailbox_t, request_seq));
      uint32_t done = load32(G_MAILBOX_BASE + offsetof(g_mailbox_t, complete_seq));
      if (!job_seq && seq != done) { job_seq = seq; job_due = cycle + 80; }
      if (job_seq && cycle >= job_due) {
        behavioral_npu(job_seq); ++jobs; job_seq = 0;
      }
      if (dut.debug_status) {
        if (dut.debug_status != 1 || jobs != 2)
          throw std::runtime_error("ARM firmware reported failure");
        std::printf("PASS: actual ARM RTL -> AXI -> shared memory -> behavioral NPU\n"
                    "cycles=%llu jobs=%u AXI_reads=%u AXI_writes=%u\n"
                    "Rocket/Gemmini RTL is not present in this host smoke run.\n",
                    static_cast<unsigned long long>(cycle), jobs, reads, writes);
        dut.final();
        return 0;
      }
    }
    throw std::runtime_error("ARM firmware timed out");
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL: %s (jobs=%u reads=%u writes=%u)\n",
                 e.what(), jobs, reads, writes);
    return 1;
  }
}
