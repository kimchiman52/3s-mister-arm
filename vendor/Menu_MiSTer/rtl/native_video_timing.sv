//============================================================================
//
//  Native Video Timing Generator
//
//  384x224 active area @ ~59.63 Hz (500x262 total)
//  CLK_VIDEO: 31.25 MHz, CE_PIXEL: divide-by-4 (7.8125 MHz effective)
//
//  H: 384 active + 26 FP + 38 sync + 52 BP = 500 total
//  V: 224 active + 14 FP +  3 sync + 21 BP = 262 total
//
//  Copyright (C) 2026 3SX Project
//  Licensed under GNU General Public License v2+
//
//============================================================================

module native_video_timing (
    input  wire        clk,        // pixel clock (~8.06 MHz)
    input  wire        ce_pix,     // pixel clock enable (normally 1)
    input  wire        reset,      // synchronous reset

    output reg         hsync,      // active low
    output reg         vsync,      // active low
    output reg         hblank,
    output reg         vblank,
    output reg         de,         // data enable = ~(hblank | vblank)
    output reg  [9:0]  hcount,     // 0..511
    output reg  [8:0]  vcount,     // 0..263
    output reg         new_frame,  // single-cycle pulse at vblank start
    output reg         new_line    // single-cycle pulse at hblank start
);

// Timing constants
//
// Image centering notes:
// The CRT positions the image based on sync-to-active timing.
// Larger H_BP shifts image RIGHT, larger V_BP shifts image DOWN.
// Previous values (H_BP=80, V_BP=34) shifted the image ~1.5" right
// and ~1" down on a standard NTSC CRT.
//
// Standard NTSC horizontal blanking at 15.734 kHz:
//   FP ~1.5us, Sync ~4.7us, BP ~4.5us (incl. colorburst)
// At 8.057 MHz pixel clock: FP~12px, Sync~38px, BP~36px
//
// H_TOTAL=500, V_TOTAL=262: with PLL C2=32 (CLK_VIDEO=31.25 MHz, CE_DIV=4):
// pixel_clock = 7.8125 MHz, H_freq = 15,625 Hz, refresh = 59.63 Hz.
// Reduced blanking fills the CRT screen better than H_TOTAL=512.
localparam H_ACTIVE = 384;
localparam H_FP     = 26;
localparam H_SYNC   = 38;
localparam H_BP     = 52;
localparam H_TOTAL  = 500;   // 384+26+38+52

localparam V_ACTIVE = 224;
localparam V_FP     = 14;
localparam V_SYNC   = 3;
localparam V_BP     = 21;
localparam V_TOTAL  = 262;   // 224+14+3+21

// Derived boundaries
localparam H_SYNC_START = H_ACTIVE + H_FP;        // 410
localparam H_SYNC_END   = H_SYNC_START + H_SYNC;  // 448
localparam V_SYNC_START = V_ACTIVE + V_FP;         // 238
localparam V_SYNC_END   = V_SYNC_START + V_SYNC;   // 241

always @(posedge clk) begin
    if (reset) begin
        hcount    <= 10'd0;
        vcount    <= 9'd0;
        hsync     <= 1'b1;  // inactive (active low)
        vsync     <= 1'b1;  // inactive (active low)
        hblank    <= 1'b0;
        vblank    <= 1'b0;
        de        <= 1'b1;  // first pixel is visible
        new_frame <= 1'b0;
        new_line  <= 1'b0;
    end
    else if (ce_pix) begin
        // Default: clear single-cycle pulses
        new_frame <= 1'b0;
        new_line  <= 1'b0;

        // Horizontal counter
        if (hcount == H_TOTAL - 1) begin
            hcount <= 10'd0;

            // Vertical counter (advances at end of each line)
            if (vcount == V_TOTAL - 1)
                vcount <= 9'd0;
            else
                vcount <= vcount + 9'd1;
        end
        else begin
            hcount <= hcount + 10'd1;
        end

        // --- Horizontal blanking ---
        // hblank asserts when hcount reaches H_ACTIVE (entering front porch)
        // hblank deasserts when hcount wraps to 0 (entering active)
        if (hcount == H_ACTIVE - 1)
            hblank <= 1'b1;
        else if (hcount == H_TOTAL - 1)
            hblank <= 1'b0;

        // --- Horizontal sync (active low) ---
        if (hcount == H_SYNC_START - 1)
            hsync <= 1'b0;  // assert
        else if (hcount == H_SYNC_END - 1)
            hsync <= 1'b1;  // deassert

        // --- Vertical blanking ---
        // Transitions at the start of a new line (when hcount wraps)
        if (hcount == H_TOTAL - 1) begin
            if (vcount == V_ACTIVE - 1)
                vblank <= 1'b1;
            else if (vcount == V_TOTAL - 1)
                vblank <= 1'b0;
        end

        // --- Vertical sync (active low) ---
        if (hcount == H_TOTAL - 1) begin
            if (vcount == V_SYNC_START - 1)
                vsync <= 1'b0;  // assert
            else if (vcount == V_SYNC_END - 1)
                vsync <= 1'b1;  // deassert
        end

        // --- New line pulse ---
        // Fires when entering horizontal blanking
        if (hcount == H_ACTIVE - 1)
            new_line <= 1'b1;

        // --- New frame pulse ---
        // Fires at start of vblank
        if (hcount == H_TOTAL - 1 && vcount == V_ACTIVE - 1)
            new_frame <= 1'b1;

        // --- Data enable ---
        // Registered output: active when next pixel will be in visible region.
        // We compute based on what hblank/vblank will be next cycle.
        // Simplest: derive from the blanking signals we just computed.
        // Since hblank and vblank are updated in this same cycle, de follows
        // them with one cycle latency. To keep all signals aligned, compute
        // de from the same conditions:
        begin
            reg next_hblank, next_vblank;

            // Will hblank be set next cycle?
            if (hcount == H_ACTIVE - 1)
                next_hblank = 1'b1;
            else if (hcount == H_TOTAL - 1)
                next_hblank = 1'b0;
            else
                next_hblank = hblank;

            // Will vblank be set next cycle?
            if (hcount == H_TOTAL - 1) begin
                if (vcount == V_ACTIVE - 1)
                    next_vblank = 1'b1;
                else if (vcount == V_TOTAL - 1)
                    next_vblank = 1'b0;
                else
                    next_vblank = vblank;
            end
            else
                next_vblank = vblank;

            de <= ~next_hblank & ~next_vblank;
        end
    end
end

endmodule
