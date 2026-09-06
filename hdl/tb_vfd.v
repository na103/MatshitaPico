// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// Testbench: CN26 VFD "8" driven as a LEVEL (run=off, reset=on) + /INAC activity
// LED monostable on rp_subq + unchanged SCOR.
`timescale 1ns/1ps
module tb_vfd;
    reg c16m = 0;
    always #29.5265 c16m = ~c16m;      // 16.9344 MHz

    reg reset_n = 0;

    wire [7:0] db;
    wire [7:0] rp_d;
    wire drq, n_sten, n_stch, n_ihac, n_xaen, dten, db_dir;
    wire c16m_o, adata, alrck, adsck, sbcp, scor, effk, empasis;
    wire cmd_stb, byte_req, chan, bclk_o, lrck_o, fsync;
    wire vfd_sck, vfd_sdata;
    reg  subq = 0;                     // subcode activity stimulus (/INAC test)
    reg  subq_act = 0;                 // 1 = activity generator on
    always begin                       // one edge every 100 us when active
        #100000;
        if (subq_act) subq = ~subq;
    end

    MatshitaPico_top dut (
        .c16m_osc(c16m),
        .cn9_db(db), .cn9_reset(reset_n),
        .cn9_n_hwr(1'b1), .cn9_n_hrd(1'b1), .cn9_n_cmd(1'b1), .cn9_enable(1'b0),
        .cn9_n_xaen(n_xaen), .cn9_dten(dten), .cn9_drq(drq), .cn9_n_sten(n_sten),
        .cn9_n_stch(n_stch), .cn9_n_ihac(n_ihac), .cn9_db_dir(db_dir),
        .cn9_c16m(c16m_o), .cn9_data(adata), .cn9_lrck(alrck), .cn9_dsck(adsck),
        .cn9_scck(1'b0), .cn9_sbcp(sbcp), .cn9_scor(scor), .cn9_effk(effk),
        .cn9_empasis(empasis),
        .cn26_sck(vfd_sck), .cn26_sdata(vfd_sdata),
        .rp_d(rp_d), .rp_cmd_stb(cmd_stb), .rp_byte_req(byte_req), .rp_chan(chan),
        .rp_byte_vld(1'b0), .rp_st_avail(1'b0), .rp_dat_avail(1'b0),
        .rp_stch_req(1'b0),
        .rp_audio_data(1'b0), .rp_subq(subq), .rp_empasis(1'b0),
        .rp_bclk(bclk_o), .rp_lrck(lrck_o), .rp_frame_sync(fsync)
    );

    integer errors = 0;
    real    t_blink;

    // spurious transitions on the CN26 pins in run (must be zero)
    integer cn26_edges = 0;
    always @(vfd_sck or vfd_sdata)
        if (reset_n === 1'b1 && $realtime > 100000) cn26_edges = cn26_edges + 1;

    // SCOR measurement (period and width)
    real t_scor_rise = 0, scor_period = 0, scor_width = 0;
    always @(posedge scor) begin
        if (t_scor_rise > 0) scor_period = $realtime - t_scor_rise;
        t_scor_rise = $realtime;
    end
    always @(negedge scor) if (t_scor_rise > 0) scor_width = $realtime - t_scor_rise;

    task check_near(input real val, input real lo, input real hi, input [255:0] name);
        if (val < lo || val > hi) begin
            errors = errors + 1;
            $display("FAIL %0s: %0.3f ms outside [%0.3f, %0.3f]", name,
                     val/1e6, lo/1e6, hi/1e6);
        end else $display("ok   %0s: %0.3f ms", name, val/1e6);
    endtask

    initial begin
        // reset: SDATA/SCK high ("8" on like the real drive in reset)
        #1000;
        if (vfd_sck !== 1'b1 || vfd_sdata !== 1'b1) begin
            errors = errors + 1;
            $display("FAIL: reset levels SCK=%b SDATA=%b (expected 1/1)",
                     vfd_sck, vfd_sdata);
        end else $display("ok   SCK/SDATA high during reset");
        #1000 reset_n = 1;

        // run: SDATA fixed low ("8" off), SCK high, zero spurious edges
        #200000;
        if (vfd_sck !== 1'b1 || vfd_sdata !== 1'b0) begin
            errors = errors + 1;
            $display("FAIL: run levels SCK=%b SDATA=%b (expected 1/0)",
                     vfd_sck, vfd_sdata);
        end else $display("ok   run: SCK high, SDATA low (the '8' turns off)");
        #400000000;
        if (cn26_edges !== 0) begin
            errors = errors + 1;
            $display("FAIL: %0d spurious transitions on CN26 in run", cn26_edges);
        end else $display("ok   CN26 idle in run (zero transitions)");

        // SCOR: 75 Hz (13.333 ms), pulse 6 lrck = 136 us
        check_near(scor_period, 13.2e6, 13.5e6, "SCOR period");
        check_near(scor_width, 0.130e6, 0.142e6, "SCOR width");

        // /INAC: level monostable on rp_subq activity
        if (n_ihac !== 1'b1) begin errors=errors+1;
            $display("FAIL: /INAC low with no subq activity"); end
        else $display("ok   /INAC high (LED off) with no activity");
        subq_act = 1;
        t_blink = $realtime;
        while (n_ihac !== 1'b0 && $realtime - t_blink < 50e6) #100000;
        if (n_ihac !== 1'b0) begin errors=errors+1;
            $display("FAIL: /INAC not asserted during subq activity"); end
        else $display("ok   /INAC low (LED on) during activity");
        // stays fixed low under continuous activity
        begin : led_solid
            integer k;
            for (k = 0; k < 30; k = k + 1) begin
                if (n_ihac !== 1'b0) begin
                    errors = errors + 1;
                    $display("FAIL: /INAC rose under continuous activity");
                    disable led_solid;
                end
                #10000000;
            end
            $display("ok   /INAC fixed low under continuous activity");
        end
        // released within a few SCOR rollovers after activity ends
        subq_act = 0;
        #100000000;
        begin : led_off_stable
            integer k;
            for (k = 0; k < 20; k = k + 1) begin
                if (n_ihac !== 1'b1) begin
                    errors = errors + 1;
                    $display("FAIL: /INAC still low after the end of activity");
                    disable led_off_stable;
                end
                #10000000;
            end
            $display("ok   /INAC released stable after the end of activity");
        end

        // warm reset: SDATA back high ("8" instantly on), then low on release
        reset_n = 0;
        #1000000;
        if (vfd_sck !== 1'b1 || vfd_sdata !== 1'b1) begin
            errors = errors + 1;
            $display("FAIL: warm reset SCK=%b SDATA=%b (expected 1/1)",
                     vfd_sck, vfd_sdata);
        end else $display("ok   warm reset: SDATA high (the '8' appears)");
        reset_n = 1;
        #1000000;
        if (vfd_sdata !== 1'b0) begin
            errors = errors + 1;
            $display("FAIL: SDATA does not return low on reset release");
        end else $display("ok   reset release: SDATA returns low");

        if (errors == 0) $display("ALL VFD TESTS OK");
        else $display("%0d ERRORS", errors);
        $finish;
    end
endmodule
