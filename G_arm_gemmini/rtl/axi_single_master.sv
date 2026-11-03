// AXI4, 32-bit, one outstanding transaction. Writes and data reads are single
// beat; instruction-cache fills may request a four-beat incrementing read.
module axi_single_master (
  input logic clk, rst,
  input logic req_valid, output logic req_ready,
  input logic req_write, input logic [31:0] req_addr, req_wdata,
  input logic [3:0] req_wstrb, input logic [7:0] req_len,
  output logic rsp_valid, input logic rsp_ready,
  output logic [31:0] rsp_rdata, output logic rsp_error,
  output logic [31:0] m_axi_awaddr, output logic m_axi_awvalid,
  input logic m_axi_awready,
  output logic [31:0] m_axi_wdata, output logic [3:0] m_axi_wstrb,
  output logic m_axi_wlast, m_axi_wvalid, input logic m_axi_wready,
  input logic [1:0] m_axi_bresp, input logic m_axi_bvalid,
  output logic m_axi_bready,
  output logic [31:0] m_axi_araddr, output logic [7:0] m_axi_arlen,
  output logic m_axi_arvalid,
  input logic m_axi_arready,
  input logic [31:0] m_axi_rdata, input logic [1:0] m_axi_rresp,
  input logic m_axi_rlast, m_axi_rvalid, output logic m_axi_rready
);
  typedef enum logic [1:0] {IDLE, WRITE, READ, RESPONSE} state_t;
  state_t state;
  logic aw_sent, w_sent, ar_sent;
  logic [31:0] address, data;
  logic [3:0] strobe;
  logic [7:0] read_beats_left, burst_len;

  assign req_ready = state == IDLE;
  assign rsp_valid = state == RESPONSE ||
                     (state == READ && ar_sent && m_axi_rvalid);
  assign m_axi_awaddr = address;
  assign m_axi_awvalid = state == WRITE && !aw_sent;
  assign m_axi_wdata = data;
  assign m_axi_wstrb = strobe;
  assign m_axi_wlast = 1'b1;
  assign m_axi_wvalid = state == WRITE && !w_sent;
  assign m_axi_bready = state == WRITE && aw_sent && w_sent;
  assign m_axi_araddr = address;
  assign m_axi_arlen = burst_len;
  assign m_axi_arvalid = state == READ && !ar_sent;
  assign m_axi_rready = state == READ && ar_sent && rsp_ready;
  always_comb begin
    if (state == READ) begin
      rsp_rdata = m_axi_rdata;
      rsp_error = (m_axi_rresp != 0) || (m_axi_rlast != (read_beats_left == 0));
    end else begin
      rsp_rdata = response_rdata;
      rsp_error = response_error;
    end
  end

  logic [31:0] response_rdata;
  logic response_error;

  always_ff @(posedge clk or posedge rst) begin
    if (rst) begin
      state <= IDLE;
      address <= 0; data <= 0; strobe <= 0;
      aw_sent <= 0; w_sent <= 0; ar_sent <= 0;
      read_beats_left <= 0; burst_len <= 0;
      response_rdata <= 0; response_error <= 0;
    end else begin
      case (state)
        IDLE: if (req_valid) begin
          address <= req_addr;
          data <= req_wdata;
          strobe <= req_wstrb;
          burst_len <= req_len;
          read_beats_left <= req_len;
          aw_sent <= 0; w_sent <= 0; ar_sent <= 0;
          state <= req_write ? WRITE : READ;
        end
        WRITE: begin
          if (m_axi_awvalid && m_axi_awready) aw_sent <= 1;
          if (m_axi_wvalid && m_axi_wready) w_sent <= 1;
          if (m_axi_bvalid && m_axi_bready) begin
            response_rdata <= 0;
            response_error <= m_axi_bresp != 0;
            state <= RESPONSE;
          end
        end
        READ: begin
          if (m_axi_arvalid && m_axi_arready) ar_sent <= 1;
          if (m_axi_rvalid && m_axi_rready) begin
            if (read_beats_left != 0) read_beats_left <= read_beats_left - 1'b1;
            if (m_axi_rlast) begin
              ar_sent <= 0;
              state <= IDLE;
            end
          end
        end
        RESPONSE: if (rsp_ready) state <= IDLE;
        default: state <= IDLE;
      endcase
    end
  end
endmodule
