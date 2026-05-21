module counter_assert #(parameter N = 4) (
    input  logic clk,
    input  logic rst_n
);
    logic [N-1:0] count;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            count <= '0;
        else
            count <= count + 1'b1;
    end

    // Property: count is always within N-bit range (trivially true)
    always @(posedge clk) begin
        if (rst_n) begin
            assert (count < (1 << N));
        end
    end

    // Property: count reaches max value (cover target for reachability)
    always @(posedge clk) begin
        if (rst_n) begin
            cover (count == {N{1'b1}});
        end
    end
endmodule
