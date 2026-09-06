// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// clkgen.v -- Audio clock generation from the C16M master clock.
// Derives BCLK (C16M/8) and LRCK (C16M/384, 50% duty) for the LC7883M DAC,
// phase-coherent with C16M. 48 BCLK per frame; the RP2350 is the I2S slave.
// =============================================================================
module clkgen (
    input  wire c16m,        // 16.9344 MHz master clock
    input  wire rst_n,       // active-low synchronous reset
    output reg  bclk,        // audio bit clock (= C16M/8)
    output reg  lrck,        // L/R clock (= fs, high = L)
    output wire frame_sync   // 1-c16m pulse at the start of each half-frame
);
    reg [8:0] cnt;           // 0..383 counter over the full LRCK period

    // free-running divider: BCLK = bit 2 (period 8), LRCK high in the first half
    always @(posedge c16m) begin
        if (!rst_n) begin
            cnt  <= 9'd0;
            bclk <= 1'b0;
            lrck <= 1'b1;
        end else begin
            if (cnt == 9'd383)
                cnt <= 9'd0;
            else
                cnt <= cnt + 9'd1;
            bclk <= cnt[2];
            lrck <= (cnt < 9'd192) ? 1'b1 : 1'b0;
        end
    end

    // pulse at the start of each half-frame (count 0 = L, count 192 = R)
    assign frame_sync = (cnt == 9'd0) || (cnt == 9'd192);
endmodule
