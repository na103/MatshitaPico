// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// host_if.v -- "LC8951-style" host-interface front-end, drive side (DRQ mode).
// Emulates the host side of the LC8951 as seen from CN9: command capture
// (host->drive), status/data replies (drive->host), DMA in DRQ mode with a
// 2-byte output FIFO (the CDTV DMAC reads 16-bit words), and the _STCH async
// notification. Everything is synchronous to C16M except the command latch,
// which is clocked by _HWR itself. See docs/timing-host-interface.md and
// docs/protocol-cdtv.md.
// =============================================================================
module host_if (
    input  wire        c16m,
    input  wire        rst_n,
    // clkgen ticks reused as slow time bases (macrocell diet)
    input  wire        bclk,      // C16M/8   (~472 ns per period)
    input  wire        lrck,      // C16M/384 (~22.7 us per period)

    // ---- CN9 side (5V domain) --------------------------------------------
    inout  wire [7:0]  db,        // host data bus (bidirectional)
    input  wire        n_hwr,     // host write strobe        (active low)
    input  wire        n_hrd,     // host read strobe         (active low)
    input  wire        n_cmd,     // command/data select      (low=command/status, high=data)
    input  wire        n_enable,  // host interface enable    (active low)
    output reg         drq,       // data request (drive->host, active high)
    output reg         n_sten,    // status enable (drive->host, active low)
    output reg         n_stch,    // status change / IRQ (drive->host, active low pin, active-high wire)
    output wire        db_dir,    // DIR of the 74LS245 transceiver on DB

    // ---- RP2350 side (internal bus, 3.3V via translator) ------------------
    inout  wire [7:0]  rp_d,
    output reg         rp_dir,
    output reg         cmd_stb,
    output reg         byte_req,
    output reg         chan,
    input  wire        byte_vld,
    input  wire        st_avail,
    input  wire        dat_avail,
    input  wire        stch_req
);
    localparam [2:0] CMD_WIN    = 3'd6;   // cmd_stb/rp_d window: 6 bclk ticks (~2.4-2.8 us)
    localparam [2:0] STEN_TICKS = 3'd6;   // _STEN safety-timeout width: 6 lrck ticks (~113-136 us)

    // ---- 2-FF synchronizers for the asynchronous inputs -------------------
    reg [1:0] hrd_s, cmd_s, en_s;
    reg [1:0] vld_s, sta_s, dta_s, stc_s;
    reg [1:0] tgl_s;                          // command-capture toggle (_HWR domain -> C16M)
    reg [1:0] qual_s;                         // _CMD latched at the _HWR edge
    always @(posedge c16m) begin
        hrd_s <= {hrd_s[0], n_hrd};
        cmd_s <= {cmd_s[0], n_cmd};
        en_s  <= {en_s[0],  n_enable};
        vld_s <= {vld_s[0], byte_vld};
        sta_s <= {sta_s[0], st_avail};
        dta_s <= {dta_s[0], dat_avail};
        stc_s <= {stc_s[0], stch_req};
        tgl_s <= {tgl_s[0], cmd_tgl};
        qual_s <= {qual_s[0], cmd_qual};
    end
    wire hrd_sync = hrd_s[1];
    wire cmd_sync = cmd_s[1];        // 1 = DATA channel; 0 = command/status channel
    wire en_active = ~en_s[1];
    wire vld_sync  = vld_s[1];
    wire sta_sync  = sta_s[1];
    wire dta_sync  = dta_s[1];
    wire stc_sync  = stc_s[1];
    wire cmd_new   = tgl_s[1] ^ tgl_s[0];   // edge = a new latched command
    wire cmd_qual_sync = qual_s[1];         // qualifier latched with the byte

    // ---- edge detection ---------------------------------------------------
    reg hrd_d, vld_d;
    reg bclk_d, lrck_d;
    always @(posedge c16m) begin
        hrd_d  <= hrd_sync;
        vld_d  <= vld_sync;
        bclk_d <= bclk;
        lrck_d <= lrck;
    end
    wire hrd_rise = ( hrd_sync & ~hrd_d); // _HRD deasserted: end of read, byte consumed
    wire hrd_fall = (~hrd_sync &  hrd_d); // _HRD asserted: host starts reading
    wire vld_rise = ( vld_sync & ~vld_d); // new output byte from the Pico
    wire tick8    = ( bclk & ~bclk_d);    // cmd_win time base (~472 ns)
    wire tick384  = ( lrck & ~lrck_d);    // sten_win time base (~22.7 us)

    wire data_phase = cmd_sync;           // _DTEN/_XAEN never asserted: _CMD alone selects data

    // ---- data registers ---------------------------------------------------
    reg [7:0] cmd_lat;    // command byte latched from DB (async on the _HWR edge)
    reg       cmd_qual;   // _CMD & _ENABLE latched on the _HWR edge with the byte
    reg       cmd_tgl;    // flips on every async capture (_HWR domain)
    reg [7:0] out_r0;     // 2-slot alternating output FIFO (DMAC word engine)
    reg [7:0] out_r1;
    reg       wr_sel;     // slot for the Pico's next push (toggles on vld_rise)
    reg       rd_sel;     // slot presented to the host (toggles async at end of read)
    reg [1:0] fill;       // bytes in FIFO not yet consumed (0..2)
    reg       cmd_flush;  // 1-clk pulse: command captured -> flush FIFO
    reg [2:0]  cmd_win;
    reg [2:0]  sten_win;
    reg        status_ready_d;

    // ---- COMMAND CAPTURE: asynchronous latch clocked by _HWR (LC8951-style)
    // Short command strobes (~100 ns) are shorter than the 2-FF sync latency, so
    // the byte must be latched by the _HWR rising edge itself; _CMD/_ENABLE are
    // latched here too as a consistent per-byte qualifier.
    always @(posedge n_hwr or negedge rst_n) begin
        if (!rst_n) begin
            cmd_lat  <= 8'h00;
            cmd_qual <= 1'b0;
            cmd_tgl  <= 1'b0;
        end else begin
            cmd_lat  <= db;
            cmd_qual <= ~n_cmd & ~n_enable;
            cmd_tgl  <= ~cmd_tgl;
        end
    end

    // ---- _STEN load: re-pulse on every new status byte --------------------
    wire status_ready      = sta_sync & (fill != 2'd0);
    wire status_ready_rise = status_ready & ~status_ready_d;
    wire sten_load = status_ready_rise | (vld_rise & sta_sync);

    // ---- DB bus: driven combinationally only when the host reads from us ----
    wire db_oe = en_active & ~n_hrd;
    wire [7:0] out_cur = rd_sel ? out_r1 : out_r0;
    assign db   = db_oe ? out_cur : 8'bz;

    // ---- rd_sel: read slot, _HRD domain (toggles async at end of read) ----
    wire rdsel_clr = ~rst_n | cmd_flush;
    always @(posedge n_hrd or posedge rdsel_clr) begin
        if (rdsel_clr)
            rd_sel <= 1'b0;
        else if (!n_enable)
            rd_sel <= ~rd_sel;
    end

    // ---- DIR of the external 74LS245 transceiver on DB (rev.1: A=CPLD, B=CN9)
    assign db_dir = db_oe;

    // ---- RP bus: the CPLD drives only when it sends a command -------------
    assign rp_d = rp_dir ? cmd_lat : 8'bz;

    always @(posedge c16m) begin
        if (!rst_n) begin
            out_r0    <= 8'd0;
            out_r1    <= 8'd0;
            wr_sel    <= 1'b0;
            fill      <= 2'd0;
            cmd_flush <= 1'b0;
            cmd_win   <= 3'd0;
            sten_win  <= 3'd0;
            status_ready_d <= 1'b0;
            rp_dir    <= 1'b0;
            cmd_stb   <= 1'b0;
            byte_req  <= 1'b0;
            chan      <= 1'b0;
            drq       <= 1'b0;
            n_sten    <= 1'b1;
            n_stch    <= 1'b0;
        end else begin
            // COMMAND CHANNEL: open the cmd_stb / rp_d window on a new latch
            if (cmd_new)
                cmd_win <= CMD_WIN;
            else if (hrd_rise)
                cmd_win <= 3'd0;
            else if (tick8 && cmd_win != 3'd0)
                cmd_win <= cmd_win - 3'd1;
            rp_dir  <= (cmd_win != 3'd0);
            cmd_stb <= (cmd_win != 3'd0);

            // 2-SLOT FIFO: flush on every command byte, else push from the Pico
            // and keep fill balanced against host consumes
            cmd_flush <= cmd_new;
            if (cmd_new) begin
                fill   <= 2'd0;
                wr_sel <= 1'b0;
            end else begin
                if (vld_rise && !rp_dir) begin
                    if (wr_sel) out_r1 <= rp_d;
                    else        out_r0 <= rp_d;
                    wr_sel <= ~wr_sel;
                end
                case ({(vld_rise && !rp_dir && fill != 2'd2),
                       (hrd_rise && en_active && fill != 2'd0)})
                    2'b10:   fill <= fill + 2'd1;
                    2'b01:   fill <= fill - 2'd1;
                    default: ;
                endcase
            end

            // byte_req = TOGGLE: one edge per byte consumed by the host
            if (hrd_rise && en_active) begin
                chan     <= data_phase ? 1'b1 : 1'b0;
                byte_req <= ~byte_req;
            end

            // _STEN: one falling edge per status byte (assert on sten_load,
            // deassert on _HRD fall; the tick counter is only a safety timeout)
            status_ready_d <= status_ready;
            if (sten_load)
                sten_win <= STEN_TICKS;
            else if (hrd_fall && en_active)
                sten_win <= 3'd0;
            else if (tick384 && sten_win != 3'd0)
                sten_win <= sten_win - 3'd1;
            n_sten <= ~(sten_win != 3'd0);

            // DRQ (active high): assert only when a COMPLETE word is in the FIFO
            drq    <= dta_sync & (fill == 2'd2);

            // STCH: async notification, active-high wire (idle low, assert high)
            n_stch <= stc_sync;
        end
    end
endmodule
