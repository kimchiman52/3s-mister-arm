// Adapted from jtframe_hsize by Jose Tejada Gomez (GPL v3)
// Applies horizontal scaling to an analogue signal via line buffer

module hsize #(parameter COLORW = 8) (
    input              clk,
    input              pxl_cen,
    input              pxl2_cen,
    input        [3:0] scale,
    input              enable,
    input [COLORW-1:0] r_in, g_in, b_in,
    input              HS_in, VS_in, HB_in, VB_in,
    output reg         HS_out, VS_out, HB_out, VB_out,
    output reg [COLORW-1:0] r_out, g_out, b_out
);

localparam VW = 9;
localparam SW = 8;

wire [VW+SW-2:0] summand;
wire [COLORW*3-1:0] rgb_out, rgb_in;
reg [VW:0] rdcnt_l=0, rdcnt=0, rgbcnt_i=0;
reg [SW-2:0] rdfrac=0, rgbfrac=0;
reg [VW-1:0] hb0=0, hb1=0;
reg [VW-1:0] wrcnt=0, rgbcnt=0, hmax=0;
reg VSl=0, HSl=0, LHBl=0, LHBll=0, VBl=0, pass=0, over=0;

assign rgb_in = {r_in, g_in, b_in};
assign summand = {{VW{1'b0}}, ~scale[3], {SW-6{scale[3]}}, scale};

always @(posedge clk) if(pxl_cen) begin
    HSl <= HS_in;
    LHBl <= ~HB_in;
    LHBll <= LHBl;
    HB_out <= HB_in;
    HS_out <= HS_in;
    VB_out <= VB_in;
    if( ~HB_in & ~LHBl ) {VB_out, VBl} <= {VBl, VB_in};
    if( HS_in & ~HSl ) begin
        wrcnt <= 0;
        hmax <= wrcnt;
        VSl <= VS_in;
        VS_out <= VSl;
    end else begin
        wrcnt <= wrcnt + 1'd1;
    end
    if(!enable) begin
        VB_out <= VB_in;
        VS_out <= VS_in;
    end
    if( LHBl & ~LHBll ) hb1 <= wrcnt;
    if( HB_in & LHBl ) hb0 <= wrcnt;
    {r_out, g_out, b_out} <= enable ? (!HB_out && pass ? rgb_out : {3*COLORW{1'b0}}) : rgb_in;
end

always @(posedge clk) if(pxl2_cen) begin
    {rdcnt, rdfrac} <= {rdcnt, rdfrac} + {1'b0, summand};
    pass <= (rgbcnt <= hb0) && (rgbcnt >= hb1);
    if( ~HS_in & HSl ) over <= 1;
    if( HS_in & ~HSl & over ) begin
        {rdcnt, rdfrac} <= { {VW+1{1'b0}}, {SW-1{1'b0}} };
        rdcnt_l <= rdcnt;
        rgbcnt <= rgbcnt_i[VW-1:0];
        rgbfrac <= 0;
        rgbcnt_i <= ({1'b0, hmax} - rdcnt_l) >> 1;
        over <= 0;
    end else begin
        {rgbcnt, rgbfrac} <= {rgbcnt, rgbfrac} + summand;
    end
end

// Ping-pong line buffer — write to one bank, read from the other.
// Bank toggle flips on each HS rising edge (same event that resets wrcnt),
// so the write side fills the new bank while the read side drains the old one.
// This prevents the read pointer from crossing the write pointer mid-line,
// which would cause a one-scanline vertical split artifact.
reg bank = 0;
always @(posedge clk) if(pxl_cen && HS_in && !HSl) bank <= ~bank;

reg [3*COLORW-1:0] lb [0:1023]; // 2 x 512-entry banks
always @(posedge clk) if(pxl_cen) lb[{bank, wrcnt}] <= rgb_in;
reg [3*COLORW-1:0] lb_rd;
always @(posedge clk) lb_rd <= lb[{~bank, rgbcnt}];
assign rgb_out = lb_rd;

endmodule
