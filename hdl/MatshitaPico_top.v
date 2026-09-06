// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// MatshitaPico_top.v -- Top-level CPLD (ATF1508AS, PLCC84, 5V).
// The only chip facing CN9 (5V TTL). Instantiates the audio clock generator and
// the host-interface FSM, generates the CD+G subcode burst engine (SBCP/SCOR/
// EFFK), the CN26 VFD level drive, and the /INAC activity LED. The RP2350 (3.3V)
// handles the command protocol, SD reads, audio samples and the subcode Q stream.
// Fits at 128/128 macrocells. See docs/host-dma-and-audio.md for the pinout.
// =============================================================================
module MatshitaPico_top (
    input  wire        c16m_osc,   // 16.9344 MHz crystal oscillator (dedicated GCLK1)

    // ================= CN9 (CDTV side, 5V TTL) ===========================
    inout  wire [7:0]  cn9_db,     // DB0..DB7
    input  wire        cn9_reset,  // RESET (active low)
    input  wire        cn9_n_hwr,  // _HWR
    input  wire        cn9_n_hrd,  // _HRD
    input  wire        cn9_n_cmd,  // _CMD
    input  wire        cn9_enable, // _ENABLE (active low)
    output wire        cn9_n_xaen, // _XAEN (drive output, never asserted)
    output wire        cn9_dten,   // _DTEN (drive output, never asserted in DRQ mode)
    output wire        cn9_drq,    // DRQ (active high)
    output wire        cn9_n_sten, // _STEN
    output wire        cn9_n_stch, // _STCH
    output wire        cn9_n_ihac, // /INAC (status/activity LED line)
    output wire        cn9_db_dir, // DIR of the 74LS245 transceiver on DB
    output wire        cn9_c16m,   // C16M master clock to the DAC
    output wire        cn9_data,   // serial audio ("D0" on the schematic, not DB0)
    output wire        cn9_lrck,   // LRCK
    output wire        cn9_dsck,   // BCLK (= DSCK)
    input  wire        cn9_scck,   // SCCK (INPUT: the motherboard generates the burst)
    output wire        cn9_sbcp,   // SBCP (Q/R-W data bits)
    output wire        cn9_scor,   // SCOR (block sync)
    output wire        cn9_effk,   // EFFK
    output wire        cn9_empasis,// EMPHASIS (de-emphasis flag)

    // ============ CN26/J3 -- serial link drive->front panel (VFD) ==========
    output wire        cn26_sck,   // J3 pin 1 -> U62 SCK
    output wire        cn26_sdata, // J3 pin 3 -> U62 SI

    // ================= RP2350 / Pico 2 (internal bus) =====================
    inout  wire [7:0]  rp_d,       // data bus (via SN74CBTD16211, no DIR)
    output wire        rp_cmd_stb, // pulse: command byte ready
    output wire        rp_byte_req,// request for the next output byte
    output wire        rp_chan,    // 0=status, 1=data
    input  wire        rp_byte_vld,// output byte valid on rp_d
    input  wire        rp_st_avail,// status available
    input  wire        rp_dat_avail,// data transfer ready
    input  wire        rp_stch_req,// async notification request (_STCH)
    input  wire        rp_audio_data, // A_DATA serial audio sample (I2S slave)
    input  wire        rp_subq,       // A_SUBQ subcode Q data stream
    input  wire        rp_empasis,    // A_EMPH de-emphasis flag
    output wire        rp_bclk,       // BCLK to the Pico
    output wire        rp_lrck,       // LRCK to the Pico
    output wire        rp_frame_sync  // = SCOR (subcode block sync, 75 Hz)
);
    // ---- internal synchronous reset from CN9 RESET (active low) -----------
    wire reset_n_comb = cn9_reset;
    reg [1:0] rst_sync;
    always @(posedge c16m_osc) rst_sync <= {rst_sync[0], reset_n_comb};
    wire rst_n = rst_sync[1];

    // ---- audio clock generation ------------------------------------------
    wire bclk, lrck, frame_sync;
    clkgen u_clkgen (
        .c16m       (c16m_osc),
        .rst_n      (rst_n),
        .bclk       (bclk),
        .lrck       (lrck),
        .frame_sync (frame_sync)
    );

    // ---- host-interface FSM ----------------------------------------------
    wire rp_dir_int;
    host_if u_host (
        .c16m     (c16m_osc),
        .rst_n    (rst_n),
        .bclk     (bclk),
        .lrck     (lrck),
        .db       (cn9_db),
        .n_hwr    (cn9_n_hwr),
        .n_hrd    (cn9_n_hrd),
        .n_cmd    (cn9_n_cmd),
        .n_enable (cn9_enable),
        .drq      (cn9_drq),
        .n_sten   (cn9_n_sten),
        .n_stch   (cn9_n_stch),
        .db_dir   (cn9_db_dir),
        .rp_d     (rp_d),
        .rp_dir   (rp_dir_int),
        .cmd_stb  (rp_cmd_stb),
        .byte_req (rp_byte_req),
        .chan     (rp_chan),
        .byte_vld (rp_byte_vld),
        .st_avail (rp_st_avail),
        .dat_avail(rp_dat_avail),
        .stch_req (rp_stch_req)
    );

    // ---- clocks toward CN9 and toward the Pico ---------------------------
    assign cn9_c16m = c16m_osc;
    assign cn9_dsck = bclk;
    assign cn9_lrck = lrck;
    assign rp_bclk  = bclk;
    assign rp_lrck  = lrck;

    // ---- audio from the Pico, forwarded to CN9 (registered) --------------
    reg data_r, empasis_r;
    always @(posedge c16m_osc) begin
        data_r    <= rp_audio_data;
        empasis_r <= rp_empasis;
    end
    assign cn9_data    = data_r;
    assign cn9_empasis = empasis_r;

    // ---- subcode block/frame counter -------------------------------------
    // 98 frames of 6 LRCK = 13.333 ms = 75 Hz. SCOR high for the whole S1 frame;
    // EFFK = LRCK/6 square (high for the first 3 LRCK of each frame).
    reg lrck_hb_d;
    always @(posedge c16m_osc) lrck_hb_d <= lrck;
    wire lrck_tick     = lrck & ~lrck_hb_d;  // lrck rising edge (44.1 kHz)
    wire lrck_edge_any = lrck ^ lrck_hb_d;   // both edges (88.2 kHz)

    reg [2:0] f6;                            // LRCK phase within the frame (0..5)
    reg [6:0] sframe;                        // frame within the block (0..97)
    reg       scor_r;
    reg       effk_r;
    always @(posedge c16m_osc) begin
        if (!rst_n) begin
            f6     <= 3'd0;
            sframe <= 7'd0;
            scor_r <= 1'b0;
            effk_r <= 1'b0;
        end else if (lrck_tick) begin
            if (f6 == 3'd5) begin
                f6 <= 3'd0;
                if (sframe == 7'd97) begin
                    sframe <= 7'd0;
                    scor_r <= 1'b1;
                end else begin
                    sframe <= sframe + 7'd1;
                    if (sframe == 7'd0)
                        scor_r <= 1'b0;
                end
            end else
                f6 <= f6 + 3'd1;
            effk_r <= (f6 == 3'd5) || (f6 < 3'd2);
        end
    end

    // ---- A_SUBQ synchronizer (shared by the shift-in and the /INAC LED) ---
    reg [1:0] subq_sync;
    always @(posedge c16m_osc) begin
        if (!rst_n) subq_sync <= 2'b00;
        else        subq_sync <= {subq_sync[0], rp_subq};
    end
    wire subq_edge = subq_sync[0] ^ subq_sync[1];

    // ---- SCCK from the host: sync + rising edge --------------------------
    reg [1:0] scck_sync;
    always @(posedge c16m_osc) begin
        if (!rst_n) scck_sync <= 2'b00;
        else        scck_sync <= {scck_sync[0], cn9_scck};
    end
    wire scck_rise = scck_sync[0] & ~scck_sync[1];

    // ---- CD+G burst engine: shift P..W out on SCCK, load from the Pico ----
    // Content (8 bits/frame P,Q,R,S,T,U,V,W) comes from A_SUBQ, loaded on LRCK
    // edges in the f6 1..4 window; the host's SCCK burst shifts it out on SBCP.
    reg [7:0] pw_sh;
    wire      pw_load_win = (f6 != 3'd0) && (f6 != 3'd5);   // f6 1..4 = 8 edges
    always @(posedge c16m_osc) begin
        if (!rst_n)
            pw_sh <= 8'h00;
        else if (scck_rise)
            pw_sh <= {pw_sh[6:0], 1'b0};
        else if (lrck_edge_any && pw_load_win)
            pw_sh <= {pw_sh[6:0], subq_sync[1]};
    end

    // ---- VFD "8" on CN26: level drive (protocol replay never accepted) ----
    // SDATA low = "8" off (run), high = "8" on (reset); SCK idle high.
    assign cn26_sck   = 1'b1;
    assign cn26_sdata = ~rst_n;

    // ---- /INAC (CN9 pin 19): CD-ROM status/activity LED, active low -------
    // Retriggerable monostable on A_SUBQ activity: an edge reloads led_cnt,
    // which decays on each SCOR block rollover (~40 ms). The firmware gates
    // rp_subq to produce the blink; the CPLD only follows the level.
    reg [1:0] led_cnt;
    always @(posedge c16m_osc) begin
        if (!rst_n) begin
            led_cnt   <= 2'd0;
        end else begin
            if (subq_edge)
                led_cnt <= 2'd3;
            else if (lrck_tick && f6 == 3'd5 && sframe == 7'd97 && led_cnt != 2'd0)
                led_cnt <= led_cnt - 2'd1;
        end
    end
    assign cn9_n_ihac = (led_cnt == 2'd0);

    // ---- subcode + fixed drive outputs toward CN9 ------------------------
    assign cn9_sbcp      = pw_sh[7];   // current bit, advances on the host's SCCK edges
    assign cn9_scor      = scor_r;     // 75 Hz block sync -> 6525 port C bit1
    assign cn9_effk      = effk_r;     // LRCK/6 square: burst reference for the motherboard
    assign cn9_n_xaen    = 1'b1;       // drive output, never asserted
    assign cn9_dten      = 1'b1;       // drive output, never asserted in DRQ mode
    assign rp_frame_sync = scor_r;     // SCOR to the Pico (block sync for the subpw_tx PIO)
endmodule
