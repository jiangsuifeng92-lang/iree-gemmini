package garm

import chisel3._
import chisel3.util.HasBlackBoxPath
import chisel3.experimental.StringParam
import org.chipsalliance.cde.config.Config
import chipyard.harness.{HarnessBinder, HasHarnessInstantiators}
import chipyard.iobinders.AXI4InPort
import testchipip.soc.BankedScratchpadKey
import freechips.rocketchip.subsystem.SBUS

class ArmHostIO extends Bundle {
  val clk = Input(Clock())
  val rst = Input(Bool())
  val irq = Input(Bool())
  val debug_status = Output(UInt(32.W))
  val m_axi_awaddr = Output(UInt(32.W))
  val m_axi_awvalid = Output(Bool())
  val m_axi_awready = Input(Bool())
  val m_axi_wdata = Output(UInt(32.W))
  val m_axi_wstrb = Output(UInt(4.W))
  val m_axi_wlast = Output(Bool())
  val m_axi_wvalid = Output(Bool())
  val m_axi_wready = Input(Bool())
  val m_axi_bresp = Input(UInt(2.W))
  val m_axi_bvalid = Input(Bool())
  val m_axi_bready = Output(Bool())
  val m_axi_araddr = Output(UInt(32.W))
  val m_axi_arlen = Output(UInt(8.W))
  val m_axi_arvalid = Output(Bool())
  val m_axi_arready = Input(Bool())
  val m_axi_rdata = Input(UInt(32.W))
  val m_axi_rresp = Input(UInt(2.W))
  val m_axi_rlast = Input(Bool())
  val m_axi_rvalid = Input(Bool())
  val m_axi_rready = Output(Bool())
}

class ArmHostBlackBox(root: String) extends BlackBox(Map(
    "HOST_MEM_HEX" -> StringParam(sys.env.getOrElse("G_HOST_MEM_HEX",
      s"$root/build/iree-firmware/matmul/host/host.hex"))))
    with HasBlackBoxPath {
  override def desiredName = "arm_host_axi"
  val io = IO(new ArmHostIO)
  addPath(s"$root/third_party/arm9/arm9_compatiable_code.v")
  addPath(s"$root/rtl/axi_single_master.sv")
  addPath(s"$root/rtl/arm_host_axi.sv")
}

// Host is in TestHarness for the first full-system simulation. It is synthesizable
// RTL, but packaging it inside ChipTop is a later integration step.
class WithArmHost extends HarnessBinder({
  case (th: HasHarnessInstantiators, port: AXI4InPort, chipId: Int) => {
    require(chipId == 0, "this bring-up config supports one NPU subsystem")
    require(port.params.beatBytes == 4, "ARM adapter requires 32-bit AXI")
    val root = sys.env.getOrElse("G_HOME", "/home/aiden/iree_rocket_gemmini/G_arm_gemmini")
    val host = Module(new ArmHostBlackBox(root))
    host.io.clk := port.io.clock
    host.io.rst := th.harnessBinderReset.asBool
    host.io.irq := false.B // completion polling; submission uses CLINT MSIP
    val axi = port.io.bits
    axi.aw.bits := 0.U.asTypeOf(axi.aw.bits)
    axi.aw.bits.addr := host.io.m_axi_awaddr
    axi.aw.bits.size := 2.U
    axi.aw.bits.burst := 1.U
    axi.aw.valid := host.io.m_axi_awvalid
    host.io.m_axi_awready := axi.aw.ready
    axi.w.bits := 0.U.asTypeOf(axi.w.bits)
    axi.w.bits.data := host.io.m_axi_wdata
    axi.w.bits.strb := host.io.m_axi_wstrb
    axi.w.bits.last := host.io.m_axi_wlast
    axi.w.valid := host.io.m_axi_wvalid
    host.io.m_axi_wready := axi.w.ready
    host.io.m_axi_bresp := axi.b.bits.resp
    host.io.m_axi_bvalid := axi.b.valid
    axi.b.ready := host.io.m_axi_bready
    axi.ar.bits := 0.U.asTypeOf(axi.ar.bits)
    axi.ar.bits.addr := host.io.m_axi_araddr
    axi.ar.bits.len := host.io.m_axi_arlen
    axi.ar.bits.size := 2.U
    axi.ar.bits.burst := 1.U
    axi.ar.valid := host.io.m_axi_arvalid
    host.io.m_axi_arready := axi.ar.ready
    host.io.m_axi_rdata := axi.r.bits.data
    host.io.m_axi_rresp := axi.r.bits.resp
    host.io.m_axi_rlast := axi.r.bits.last
    host.io.m_axi_rvalid := axi.r.valid
    axi.r.ready := host.io.m_axi_rready
    withClockAndReset(port.io.clock, th.harnessBinderReset) {
      assert(host.io.debug_status =/= 2.U, "ARM host firmware reported failure")
    }
    when(host.io.debug_status === 1.U) { th.success := true.B }
  }
})

// Move AbstractConfig's 64 KiB SRAM above L2 so mailbox polling is uncached.
// Keeping it on MBUS would place it behind the inclusive cache.
class WithHostMailboxSRAM extends Config((site, here, up) => {
  case BankedScratchpadKey =>
    val memories = up(BankedScratchpadKey)
    require(memories.exists(_.base == BigInt("08000000", 16)),
      "expected AbstractConfig's SRAM at 0x08000000")
    memories.map { memory =>
      if (memory.base == BigInt("08000000", 16))
        memory.copy(busWhere = SBUS, name = "host-npu-mailbox")
      else memory
    }
})

class ArmGemminiConfig extends Config(
  new WithArmHost ++
  new WithHostMailboxSRAM ++
  new freechips.rocketchip.subsystem.WithCustomSlavePort(data_width = 32, id_bits = 1) ++
  new freechips.rocketchip.subsystem.WithExtMemSize(BigInt(256) << 20) ++
  new chipyard.CustomGemminiSoCConfig
)
