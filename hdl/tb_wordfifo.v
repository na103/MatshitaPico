// =============================================================================
// MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
// Copyright (c) 2026 Nicola Avanzi (na103)
// Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
// See LICENSE.
// =============================================================================
// Testbench: host_if's 2-byte FIFO against the DMAC word pairs.
`timescale 1ns/1ps
module tb_wordfifo;
    reg c16m = 0, rst_n = 0;
    always #29.5 c16m = ~c16m;           // 16.9344 MHz

    // clkgen ticks (replica of clkgen.v)
    reg [8:0] divcnt = 0;
    reg bclk = 0, lrck = 1;
    always @(posedge c16m) begin
        divcnt <= (divcnt == 9'd383) ? 9'd0 : divcnt + 9'd1;
        bclk   <= divcnt[2];
        lrck   <= (divcnt < 9'd192);
    end

    // CN9 side
    wire [7:0] db;
    reg  [7:0] db_drv = 8'hzz; assign db = db_drv;
    reg  n_hwr = 1, n_hrd = 1, n_cmd = 1, n_enable = 0;
    wire drq, n_sten, n_stch, db_dir;

    // RP side
    wire [7:0] rp_d;
    reg  [7:0] rp_drv = 8'hzz; assign rp_d = rp_drv;
    wire rp_dir, cmd_stb, byte_req, chan;
    reg  byte_vld = 0, st_avail = 0, dat_avail = 0, stch_req = 0;

    host_if dut(.c16m(c16m), .rst_n(rst_n), .bclk(bclk), .lrck(lrck),
                .db(db), .n_hwr(n_hwr), .n_hrd(n_hrd),
                .n_cmd(n_cmd), .n_enable(n_enable), .drq(drq), .n_sten(n_sten),
                .n_stch(n_stch), .db_dir(db_dir), .rp_d(rp_d), .rp_dir(rp_dir),
                .cmd_stb(cmd_stb), .byte_req(byte_req), .chan(chan),
                .byte_vld(byte_vld), .st_avail(st_avail), .dat_avail(dat_avail),
                .stch_req(stch_req));

    integer errors = 0;
    integer req_edges = 0;
    reg byte_req_d;
    always @(byte_req) req_edges = req_edges + 1;   // count every edge (like the Pico IRQ)

    // push a byte from the Pico
    task push(input [7:0] v);
        begin
            rp_drv = v; #200;
            byte_vld = 1; #1000;
            byte_vld = 0; #1000;
            rp_drv = 8'hzz;
        end
    endtask

    // host read: _HRD low 100 ns, sample db at end of cycle
    reg [7:0] got;
    task hread;
        begin
            n_hrd = 0; #98; got = db; #2; n_hrd = 1;
        end
    endtask

    task check(input [7:0] exp, input [127:0] name);
        if (got !== exp) begin
            errors = errors + 1;
            $display("FAIL %0s: read %02h expected %02h (t=%0t)", name, got, exp, $time);
        end else $display("ok   %0s: %02h", name, got);
    endtask

    initial begin
        #200 rst_n = 1; #200;

        // 1) data transfer: push a word (2 bytes), read the DMAC pair
        n_cmd = 1;
        dat_avail = 1; #200;
        push(8'hA1); push(8'hB2);
        #300;
        if (drq !== 1) begin errors=errors+1; $display("FAIL: DRQ low with a complete word"); end
        else $display("ok   DRQ high on a complete word");
        req_edges = 0;
        hread; check(8'hA1, "word byte1");
        #112;
        hread; check(8'hB2, "word byte2 (2nd back-to-back read)");
        #500;
        if (req_edges !== 2) begin errors=errors+1; $display("FAIL: byte_req %0d edges (expected 2)", req_edges); end
        else $display("ok   byte_req: 2 edges for the pair");
        if (drq !== 0) begin errors=errors+1; $display("FAIL: DRQ still high on an empty FIFO"); end
        else $display("ok   DRQ low after the pair");

        // 2) refill: the next pair
        push(8'hC3); push(8'hD4); #300;
        if (drq !== 1) begin errors=errors+1; $display("FAIL: DRQ did not rise again after refill"); end
        hread; check(8'hC3, "word2 byte1");
        #112;
        hread; check(8'hD4, "word2 byte2");
        #500; dat_avail = 0;

        // 3) command in the middle (flush) + status reply (interlude)
        push(8'hE5); push(8'hF6); dat_avail = 1; #300;
        n_cmd = 0;
        db_drv = 8'h81; #100; n_hwr = 0; #440; n_hwr = 1; #50; db_drv = 8'hzz;
        n_cmd = 1; #300;
        if (drq !== 0) begin errors=errors+1; $display("FAIL: DRQ high after a command flush"); end
        else $display("ok   command flush: DRQ down (data suspended)");
        #3500;
        push(8'h69); st_avail = 1; #300;
        if (n_sten !== 0) begin errors=errors+1; $display("FAIL: _STEN not asserted on status"); end
        else $display("ok   _STEN asserted on the post-flush status");
        n_cmd = 0;
        hread; check(8'h69, "status read after flush");
        st_avail = 0; n_cmd = 1; #300;
        push(8'hE5); push(8'hF6); #300;
        if (drq !== 1) begin errors=errors+1; $display("FAIL: DRQ not re-armed after the interlude"); end
        hread; check(8'hE5, "replay byte1");
        #112;
        hread; check(8'hF6, "replay byte2");
        #500; dat_avail = 0;

        // 4) three close consumes: 3 byte_req edges
        dat_avail = 1;
        push(8'h01); push(8'h02); #300; req_edges = 0;
        hread; #112; hread; #500;
        push(8'h03); push(8'h04); #300;
        hread; #500;
        if (req_edges !== 3) begin errors=errors+1; $display("FAIL: byte_req %0d edges (expected 3)", req_edges); end
        else $display("ok   byte_req: 3 edges for 3 consumes");

        // 5) _STEN safety timeout: spontaneous rise between 113 and 136 us
        hread; #500; dat_avail = 0;
        push(8'h11); st_avail = 1; #500;
        if (n_sten !== 0) begin errors=errors+1; $display("FAIL: _STEN not asserted (timeout test)"); end
        #90000;
        if (n_sten !== 0) begin errors=errors+1; $display("FAIL: _STEN rose too early (<113us)"); end
        else $display("ok   _STEN still low at +90us");
        #60000;
        if (n_sten !== 1) begin errors=errors+1; $display("FAIL: _STEN did not rise at timeout (~150us)"); end
        else $display("ok   _STEN safety timeout: risen between 113 and 150 us");
        st_avail = 0;

        if (errors == 0) $display("ALL FIFO TESTS OK");
        else $display("%0d ERRORS", errors);
        $finish;
    end
endmodule
