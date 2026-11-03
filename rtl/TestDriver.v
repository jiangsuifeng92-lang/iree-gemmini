// See LICENSE.SiFive for license details.
//VCS coverage exclude_file
`ifndef RESET_DELAY
 `define RESET_DELAY 777.7
`endif
`ifndef MODEL
 `define MODEL TestHarness
`endif

module TestDriver;

  reg clock = 1'b0;
  reg reset = 1'b1;

  always #(`CLOCK_PERIOD/2.0) clock = ~clock;
  initial #(`RESET_DELAY) reset = 0;

  // Read input arguments and initialize
  reg verbose = 1'b0;
  wire printf_cond = verbose && !reset;
  reg [63:0] max_cycles = 0;
  reg [63:0] dump_start = 0;
  reg [63:0] trace_count = 0;
  reg [2047:0] fsdbfile = 0;
  reg [2047:0] vcdplusfile = 0;
  reg [2047:0] vcdfile = 0;
  int unsigned rand_value;
  initial
  begin
    void'($value$plusargs("max-cycles=%d", max_cycles));
    void'($value$plusargs("dump-start=%d", dump_start));
    verbose = $test$plusargs("verbose");

    // do not delete the lines below.
    // $random function needs to be called with the seed once to affect all
    // the downstream $random functions within the Chisel-generated Verilog
    // code.
    // $urandom is seeded via cmdline (+ntb_random_seed in VCS) but that
    // doesn't seed $random.
    rand_value = $urandom;
    rand_value = $random(rand_value);
    if (verbose) begin
`ifdef VCS
      $fdisplay(stderr, "testing $random %0x seed %d", rand_value, unsigned'($get_initial_random_seed));
`else
      $fdisplay(stderr, "testing $random %0x", rand_value);
`endif
    end

`ifdef DEBUG

    if ($value$plusargs("vcdplusfile=%s", vcdplusfile))
    begin
`ifdef VCS
      $vcdplusfile(vcdplusfile);
`else
      $fdisplay(stderr, "Error: +vcdplusfile is VCS-only; use +vcdfile instead or recompile with VCS=1");
      $fatal;
`endif
    end

    if ($value$plusargs("fsdbfile=%s", fsdbfile))
    begin
`ifdef FSDB
      $fsdbDumpfile(fsdbfile);
      $fsdbDumpvars("+all");
      //$fsdbDumpSVA;
`else
      $fdisplay(stderr, "Error: +fsdbfile is FSDB-only; use +vcdfile/+vcdplus instead or recompile with FSDB=1");
      $fatal;
`endif
    end

    if ($value$plusargs("vcdfile=%s", vcdfile))
    begin
      $dumpfile(vcdfile);
      if ($test$plusargs("profile-wave"))
        $dumpvars(0, profileSignals);
      else
        $dumpvars(0, testHarness);
    end

`ifdef FSDB
`define VCDPLUSON $fsdbDumpon;
`define VCDPLUSCLOSE $fsdbDumpoff;
`elsif VCS
`define VCDPLUSON $vcdpluson(0); $vcdplusmemon(0);
`define VCDPLUSCLOSE $vcdplusclose; $dumpoff;
`else
`define VCDPLUSON $dumpon;
`define VCDPLUSCLOSE $dumpoff;
`endif
`else
  // No +define+DEBUG
`define VCDPLUSON
`define VCDPLUSCLOSE

    if ($test$plusargs("vcdplusfile=") || $test$plusargs("vcdfile=") || $test$plusargs("fsdbfile="))
    begin
      $fdisplay(stderr, "Error: +vcdfile, +vcdplusfile, or +fsdbfile requested but compile did not have +define+DEBUG enabled");
      $fatal;
    end

`endif

    if (dump_start == 0)
    begin
      // Start dumping before first clock edge to capture reset sequence in waveform
      `VCDPLUSON
    end
  end

`ifdef TESTBENCH_IN_UVM
  // UVM library has its own way to manage end-of-simulation.
  // A UVM-based testbench will raise an objection, watch this signal until this goes 1, then drop the objection.
  reg finish_request = 1'b0;
`endif
  reg [255:0] reason = "";
  reg failure = 1'b0;
  wire success;
  integer stderr = 32'h80000002;
  always @(posedge clock)
  begin
`ifdef GATE_LEVEL
    if (verbose)
    begin
      $fdisplay(stderr, "C: %10d", trace_count);
    end
`endif

    trace_count = trace_count + 1;

    if (trace_count == dump_start)
    begin
      `VCDPLUSON
    end

    if (!reset)
    begin
      if (max_cycles > 0 && trace_count > max_cycles)
      begin
        reason = " (timeout)";
        failure = 1'b1;
      end

      if (failure)
      begin
        $fdisplay(stderr, "*** FAILED ***%s after %d simulation cycles", reason, trace_count);
        `VCDPLUSCLOSE
        $fatal;
      end

      if (success)
      begin
        if (verbose)
          $fdisplay(stderr, "*** PASSED *** Completed after %d simulation cycles", trace_count);
        `VCDPLUSCLOSE
`ifdef TESTBENCH_IN_UVM
        finish_request = 1;
`else
        $finish;
`endif
      end
    end
  end

  `MODEL testHarness(
    .clock(clock),
    .reset(reset),
    .io_success(success)
  );

  ProfileWaveSignals profileSignals (
    .reset(reset),
    .simulation_clock(clock),
    .arm_cpu_en(testHarness.host.cpu_en),
    .arm_axi_awvalid(testHarness.host.m_axi_awvalid),
    .arm_axi_awready(testHarness.host.m_axi_awready),
    .arm_axi_awaddr(testHarness.host.m_axi_awaddr),
    .arm_axi_wvalid(testHarness.host.m_axi_wvalid),
    .arm_axi_wready(testHarness.host.m_axi_wready),
    .arm_axi_wdata(testHarness.host.m_axi_wdata),
    .arm_axi_arvalid(testHarness.host.m_axi_arvalid),
    .arm_axi_arready(testHarness.host.m_axi_arready),
    .arm_axi_araddr(testHarness.host.m_axi_araddr),
    .mailbox_a_valid(testHarness.chiptop0.system.sbus._system_bus_xbar_auto_anon_out_2_a_valid),
    .mailbox_a_ready(testHarness.chiptop0.system._bank_auto_xbar_anon_in_a_ready),
    .mailbox_a_address(testHarness.chiptop0.system.sbus._system_bus_xbar_auto_anon_out_2_a_bits_address),
    .mailbox_a_opcode(testHarness.chiptop0.system.sbus._system_bus_xbar_auto_anon_out_2_a_bits_opcode),
    .mailbox_a_source(testHarness.chiptop0.system.sbus._system_bus_xbar_auto_anon_out_2_a_bits_source),
    .mailbox_a_mask(testHarness.chiptop0.system.sbus._system_bus_xbar_auto_anon_out_2_a_bits_mask),
    .mailbox_a_data(testHarness.chiptop0.system.sbus._system_bus_xbar_auto_anon_out_2_a_bits_data),
    .mailbox_d_valid(testHarness.chiptop0.system._bank_auto_xbar_anon_in_d_valid),
    .mailbox_d_data(testHarness.chiptop0.system._bank_auto_xbar_anon_in_d_bits_data),
    .gemmini_busy(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile.gemmini.io_busy),
    .uart_tx(testHarness.chiptop0.uart_0_txd),
    .icache_a_valid(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._frontend_auto_icache_master_out_a_valid),
    .icache_a_ready(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._tlMasterXbar_auto_anon_in_1_a_ready),
    .icache_a_address(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._frontend_auto_icache_master_out_a_bits_address),
    .icache_d_valid(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._tlMasterXbar_auto_anon_in_1_d_valid),
    .icache_d_data(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._tlMasterXbar_auto_anon_in_1_d_bits_data),
    .rocket_retire_valid(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile.core._csr_io_trace_0_valid),
    .rocket_retire_pc(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile.core._csr_io_trace_0_iaddr),
    .rocket_retire_insn(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile.core._csr_io_trace_0_insn),
    .rocc_cmd_valid(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._core_io_rocc_cmd_valid),
    .rocc_cmd_ready(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._cmdRouter_io_in_ready),
    .rocc_opcode(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._core_io_rocc_cmd_bits_inst_opcode),
    .rocc_funct(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._core_io_rocc_cmd_bits_inst_funct),
    .rocc_rs1(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._core_io_rocc_cmd_bits_rs1),
    .rocc_rs2(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._core_io_rocc_cmd_bits_rs2),
    .gemmini_cmd_valid(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._cmdRouter_io_out_0_valid),
    .gemmini_cmd_ready(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._gemmini_io_cmd_ready),
    .gemmini_cmd_opcode(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._cmdRouter_io_out_0_bits_inst_opcode),
    .gemmini_cmd_funct(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._cmdRouter_io_out_0_bits_inst_funct),
    .gemmini_cmd_rs1(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._cmdRouter_io_out_0_bits_rs1),
    .gemmini_cmd_rs2(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._cmdRouter_io_out_0_bits_rs2),
    .gemmini_load_busy(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile.gemmini._load_controller_io_busy),
    .gemmini_execute_busy(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile.gemmini._ex_controller_io_busy),
    .gemmini_store_busy(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile.gemmini._store_controller_io_busy),
    .gemmini_load_dma_valid(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile.gemmini._load_controller_io_dma_req_valid),
    .gemmini_load_dma_vaddr(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile.gemmini._load_controller_io_dma_req_bits_vaddr),
    .gemmini_store_dma_valid(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile.gemmini._store_controller_io_dma_req_valid),
    .gemmini_store_dma_vaddr(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile.gemmini._store_controller_io_dma_req_bits_vaddr),
    .dcache_a_valid(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._dcache_auto_out_a_valid),
    .dcache_a_address(testHarness.chiptop0.system.tile_prci_domain.element_reset_domain_rockettile._dcache_auto_out_a_bits_address),
    .ddr_ar_valid(testHarness._chiptop0_axi4_mem_0_bits_ar_valid),
    .ddr_ar_ready(testHarness._simdram_axi_ar_ready),
    .ddr_ar_address(testHarness._chiptop0_axi4_mem_0_bits_ar_bits_addr),
    .ddr_ar_len(testHarness._chiptop0_axi4_mem_0_bits_ar_bits_len),
    .ddr_ar_id(testHarness._chiptop0_axi4_mem_0_bits_ar_bits_id),
    .ddr_r_valid(testHarness._simdram_axi_r_valid),
    .ddr_r_ready(testHarness._chiptop0_axi4_mem_0_bits_r_ready),
    .ddr_r_data(testHarness._simdram_axi_r_bits_data),
    .ddr_r_last(testHarness._simdram_axi_r_bits_last),
    .ddr_r_id(testHarness._simdram_axi_r_bits_id)
  );

endmodule


module ProfileWaveSignals(
  input wire reset,
  input wire simulation_clock,
  input wire arm_cpu_en,
  input wire arm_axi_awvalid,
  input wire arm_axi_awready,
  input wire [31:0] arm_axi_awaddr,
  input wire arm_axi_wvalid,
  input wire arm_axi_wready,
  input wire [31:0] arm_axi_wdata,
  input wire arm_axi_arvalid,
  input wire arm_axi_arready,
  input wire [31:0] arm_axi_araddr,
  input wire mailbox_a_valid,
  input wire mailbox_a_ready,
  input wire [27:0] mailbox_a_address,
  input wire [2:0] mailbox_a_opcode,
  input wire [6:0] mailbox_a_source,
  input wire [15:0] mailbox_a_mask,
  input wire [127:0] mailbox_a_data,
  input wire mailbox_d_valid,
  input wire [127:0] mailbox_d_data,
  input wire gemmini_busy,
  input wire uart_tx,
  input wire icache_a_valid,
  input wire icache_a_ready,
  input wire [31:0] icache_a_address,
  input wire icache_d_valid,
  input wire [127:0] icache_d_data,
  input wire rocket_retire_valid,
  input wire [39:0] rocket_retire_pc,
  input wire [31:0] rocket_retire_insn,
  input wire rocc_cmd_valid,
  input wire rocc_cmd_ready,
  input wire [6:0] rocc_opcode,
  input wire [6:0] rocc_funct,
  input wire [63:0] rocc_rs1,
  input wire [63:0] rocc_rs2,
  input wire gemmini_cmd_valid,
  input wire gemmini_cmd_ready,
  input wire [6:0] gemmini_cmd_opcode,
  input wire [6:0] gemmini_cmd_funct,
  input wire [63:0] gemmini_cmd_rs1,
  input wire [63:0] gemmini_cmd_rs2,
  input wire gemmini_load_busy,
  input wire gemmini_execute_busy,
  input wire gemmini_store_busy,
  input wire gemmini_load_dma_valid,
  input wire [39:0] gemmini_load_dma_vaddr,
  input wire gemmini_store_dma_valid,
  input wire [39:0] gemmini_store_dma_vaddr,
  input wire dcache_a_valid,
  input wire [31:0] dcache_a_address,
  input wire ddr_ar_valid,
  input wire ddr_ar_ready,
  input wire [31:0] ddr_ar_address,
  input wire [7:0] ddr_ar_len,
  input wire [3:0] ddr_ar_id,
  input wire ddr_r_valid,
  input wire ddr_r_ready,
  input wire [63:0] ddr_r_data,
  input wire ddr_r_last,
  input wire [3:0] ddr_r_id
);
endmodule
