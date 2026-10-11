// ======================================================================
// Formal harness: a back-to-back INITIATOR and TARGET connected by a
// ready/valid link, each with its own FIFO "interface".
//
// This mirrors the rv_proto transactor structure -- an initiator that
// drains an internal FIFO onto a ready/valid link, and a target that fills
// an internal FIFO from the link -- but written as plain synthesizable RTL
// so SymbiYosys/yosys can reason about it formally. (The class-based fw
// model and SV-queue FIFOs are not synthesizable; this is the formal-shaped
// equivalent of the same dataflow.)
//
// SymbiYosys (BMC) proves three things about the connected pair:
//   1. LINK CONTRACT  -- the initiator (ready/valid master) holds valid and
//      data stable while the target stalls (the AXI-stream handshake rule).
//   2. NO OVERFLOW    -- neither FIFO ever overflows; outstanding words in
//      the pipe never exceed the combined capacity.
//   3. DATA INTEGRITY -- every word that enters the initiator leaves the
//      target exactly once, in order, unchanged (the classic anyconst-index
//      FIFO proof: the word at an arbitrary position in == the word at that
//      same position out).
//
// Run:  dfm run fw-hdl.formal.fifo-link
// ======================================================================

`default_nettype none

// ----------------------------------------------------------------------
// Synchronous FIFO, depth = (1<<AW). Count-based full/empty; writes ignored
// when full and reads ignored when empty (so the environment can poke the
// enables freely without an explicit assumption).
// ----------------------------------------------------------------------
module fw_fifo #(
    parameter int DW = 8,
    parameter int AW = 2
) (
    input  wire           clk,
    input  wire           rst,
    input  wire           wr,
    input  wire [DW-1:0]  wdata,
    output wire           full,
    input  wire           rd,
    output wire [DW-1:0]  rdata,
    output wire           empty
);
    localparam int DEPTH = (1 << AW);

    reg [DW-1:0] mem [0:DEPTH-1];
    reg [AW-1:0] wptr, rptr;
    reg [AW:0]   cnt;                 // 0 .. DEPTH

    assign full  = (cnt == DEPTH[AW:0]);
    assign empty = (cnt == 0);
    assign rdata = mem[rptr];

    wire do_wr = wr && !full;
    wire do_rd = rd && !empty;

    always @(posedge clk) begin
        if (rst) begin
            wptr <= '0;
            rptr <= '0;
            cnt  <= '0;
        end else begin
            if (do_wr) begin
                mem[wptr] <= wdata;
                wptr      <= wptr + 1'b1;
            end
            if (do_rd)
                rptr <= rptr + 1'b1;
            cnt <= cnt + (do_wr ? 1 : 0) - (do_rd ? 1 : 0);
        end
    end
endmodule

// ----------------------------------------------------------------------
// Initiator: an API-side producer port (i_we/i_data/i_full) into an internal
// FIFO whose head is presented on the ready/valid link (master). The link
// transfer (m_valid && m_ready) pops the FIFO.
// ----------------------------------------------------------------------
module fw_initiator #(
    parameter int DW = 8,
    parameter int AW = 2
) (
    input  wire           clk,
    input  wire           rst,
    // API side -- producer pushes beats
    input  wire           i_we,
    input  wire [DW-1:0]  i_data,
    output wire           i_full,
    // link side -- ready/valid master
    output wire           m_valid,
    output wire [DW-1:0]  m_data,
    input  wire           m_ready
);
    wire empty;

    fw_fifo #(.DW(DW), .AW(AW)) u_fifo (
        .clk   (clk),
        .rst   (rst),
        .wr    (i_we),
        .wdata (i_data),
        .full  (i_full),
        .rd    (m_valid && m_ready),
        .rdata (m_data),
        .empty (empty)
    );

    assign m_valid = !empty;          // present head whenever we have one
endmodule

// ----------------------------------------------------------------------
// Target: a ready/valid slave on the link that fills an internal FIFO, with
// an API-side consumer port (o_re/o_data/o_empty). m_ready is asserted while
// there is room, so the link never overflows the FIFO.
// ----------------------------------------------------------------------
module fw_target #(
    parameter int DW = 8,
    parameter int AW = 2
) (
    input  wire           clk,
    input  wire           rst,
    // link side -- ready/valid slave
    input  wire           m_valid,
    input  wire [DW-1:0]  m_data,
    output wire           m_ready,
    // API side -- consumer pops beats
    input  wire           o_re,
    output wire [DW-1:0]  o_data,
    output wire           o_empty
);
    wire full;

    fw_fifo #(.DW(DW), .AW(AW)) u_fifo (
        .clk   (clk),
        .rst   (rst),
        .wr    (m_valid && m_ready),
        .wdata (m_data),
        .full  (full),
        .rd    (o_re),
        .rdata (o_data),
        .empty (o_empty)
    );

    assign m_ready = !full;           // accept while there is room
endmodule

// ----------------------------------------------------------------------
// Top harness: initiator + target wired back-to-back over the ready/valid
// link. The API-side enables/data and the consumer read-enable are free
// formal inputs (the FIFOs gate them), so the solver explores every legal
// producer/consumer interleaving. All properties live here.
// ----------------------------------------------------------------------
module fifo_link_fv #(
    parameter int DW = 4,                  // narrow data keeps the SMT state small
    parameter int AW = 2                   // FIFO depth = 4 per side
) (
    input wire           clk,
    input wire           rst,
    // free environment stimulus
    input wire           i_we,
    input wire [DW-1:0]  i_data,
    input wire           o_re
);
    wire           i_full, o_empty;
    wire           m_valid, m_ready;
    wire [DW-1:0]  m_data, o_data;

    fw_initiator #(.DW(DW), .AW(AW)) u_init (
        .clk(clk), .rst(rst),
        .i_we(i_we), .i_data(i_data), .i_full(i_full),
        .m_valid(m_valid), .m_data(m_data), .m_ready(m_ready)
    );

    fw_target #(.DW(DW), .AW(AW)) u_targ (
        .clk(clk), .rst(rst),
        .m_valid(m_valid), .m_data(m_data), .m_ready(m_ready),
        .o_re(o_re), .o_data(o_data), .o_empty(o_empty)
    );

`ifdef FORMAL
    // ------------------------------------------------------------------
    // Formal preamble. f_past_valid masks the first cycle (where $past is
    // undefined); we also require that the design starts in reset.
    // ------------------------------------------------------------------
    reg f_past_valid = 1'b0;
    always @(posedge clk)
        f_past_valid <= 1'b1;

    always @(*)
        if (!f_past_valid)
            assume (rst);

    // Transfer events at the two FIFO endpoints.
    wire i_xfer = i_we && !i_full;    // word accepted into the initiator
    wire o_xfer = o_re && !o_empty;   // word emitted from the target

    // ------------------------------------------------------------------
    // (1) LINK CONTRACT: while the master holds valid and the slave is not
    //     ready, valid stays asserted and data is held stable.
    // ------------------------------------------------------------------
    always @(posedge clk)
        if (f_past_valid && !$past(rst))
            if ($past(m_valid) && !$past(m_ready)) begin
                assert (m_valid);
                assert (m_data == $past(m_data));
            end

    // ------------------------------------------------------------------
    // (2)+(3) DATA INTEGRITY via an arbitrary tracked position f_idx.
    //     i_cnt / o_cnt count words accepted / emitted. We capture the data
    //     of the f_idx-th accepted word, then require the f_idx-th emitted
    //     word to match it -- proving no loss, no duplication, in order, and
    //     no corruption.
    // ------------------------------------------------------------------
    localparam int CW  = 5;                // counter width (no wrap within BMC depth)
    localparam int CAP = 2 * (1 << AW);    // both FIFOs full = max in flight

    (* anyconst *) reg [CW-1:0] f_idx;
    reg [CW-1:0] i_cnt, o_cnt;
    reg [DW-1:0] f_data;
    reg          f_have;

    always @(posedge clk) begin
        if (rst) begin
            i_cnt  <= '0;
            o_cnt  <= '0;
            f_have <= 1'b0;
        end else begin
            if (i_xfer) begin
                if (i_cnt == f_idx) begin
                    f_data <= i_data;
                    f_have <= 1'b1;
                end
                i_cnt <= i_cnt + 1'b1;
            end
            if (o_xfer)
                o_cnt <= o_cnt + 1'b1;
        end
    end

    always @(posedge clk)
        if (!rst) begin
            // The tracked word leaves the target equal to what entered.
            if (o_xfer && (o_cnt == f_idx)) begin
                assert (f_have);                 // it must have entered first
                assert (o_data == f_data);       // ... and be unchanged
            end
            // Outstanding words never exceed the combined FIFO capacity.
            assert ((i_cnt - o_cnt) <= CW'(CAP));
        end

    // ------------------------------------------------------------------
    // Cover (checked in 'cover' mode): the tracked word can actually make it
    // through end to end -- guards against a vacuously-passing proof.
    // ------------------------------------------------------------------
    always @(posedge clk)
        cover (!rst && o_xfer && (o_cnt == f_idx) && f_have);
`endif
endmodule

`default_nettype wire
