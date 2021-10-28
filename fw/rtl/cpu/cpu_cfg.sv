module cpu_cfg (
    if_system.sys sys,
    if_cpu_bus bus,
    if_config.cpu cfg
);

    logic skip_bootloader;
    logic trigger_reconfiguration;

    typedef enum bit [2:0] { 
        R_SCR,
        R_DD_OFFSET,
        R_SAVE_OFFSET,
        R_COMMAND,
        R_DATA_0,
        R_DATA_1,
        R_VERSION,
        R_RECONFIGURE
    } e_reg_id;

    const logic [31:0] RECONFIGURE_MAGIC = 32'h52535446;

    always_ff @(posedge sys.clk) begin
        bus.ack <= 1'b0;
        if (bus.request) begin
            bus.ack <= 1'b1;
        end
    end

    always_comb begin
        bus.rdata = 32'd0;
        if (bus.ack) begin
            case (bus.address[4:2])
                R_SCR: bus.rdata = {
                    cfg.cpu_ready,
                    cfg.cpu_busy,
                    1'b0,
                    cfg.cmd_error,
                    21'd0,
                    skip_bootloader,
                    cfg.flashram_enabled,
                    cfg.sram_banked,
                    cfg.sram_enabled,
                    cfg.dd_enabled,
                    cfg.sdram_writable,
                    cfg.sdram_switch
                };
                R_DD_OFFSET: bus.rdata = {6'd0, cfg.dd_offset};
                R_SAVE_OFFSET: bus.rdata = {6'd0, cfg.save_offset};
                R_COMMAND: bus.rdata = {24'd0, cfg.cmd};
                R_DATA_0: bus.rdata = cfg.data[0];
                R_DATA_1: bus.rdata = cfg.data[1];
                R_VERSION: bus.rdata = sc64::SC64_VER;
                R_RECONFIGURE: bus.rdata = {31'd0, trigger_reconfiguration};
                default: bus.rdata = 32'd0;
            endcase
        end
    end

    always_comb begin
        cfg.wdata = bus.wdata;
        cfg.data_write = 2'b00;
        if (bus.request && (&bus.wmask)) begin
            cfg.data_write[0] = bus.address[4:2] == R_DATA_0;
            cfg.data_write[1] = bus.address[4:2] == R_DATA_1;
        end
    end

    always_ff @(posedge sys.clk) begin
        if (sys.reset) begin
            cfg.cpu_ready <= 1'b0;
            cfg.cpu_busy <= 1'b0;
            cfg.cmd_error <= 1'b0;
            cfg.sdram_switch <= 1'b0;
            cfg.sdram_writable <= 1'b0;
            cfg.dd_enabled <= 1'b0;
            cfg.sram_enabled <= 1'b0;
            cfg.sram_banked <= 1'b0;
            cfg.flashram_enabled <= 1'b0;
            cfg.dd_offset <= 26'h3BE_0000;
            cfg.save_offset <= 26'h3FE_0000;
            skip_bootloader <= 1'b0;
            trigger_reconfiguration <= 1'b0;
        end else begin
            if (sys.n64_soft_reset) begin
                cfg.sdram_switch <= skip_bootloader;
                cfg.sdram_writable <= 1'b0;
            end
            if (cfg.cmd_request) begin
                cfg.cpu_busy <= 1'b1;
            end
            if (bus.request) begin
                case (bus.address[4:2])
                    R_SCR: begin
                        if (bus.wmask[3]) begin
                            {
                                cfg.cpu_ready,
                                cfg.cpu_busy,
                                cfg.cmd_error
                            } <= {bus.wdata[31:30], bus.wdata[28]};
                        end
                        if (bus.wmask[0]) begin
                            {
                                skip_bootloader,
                                cfg.flashram_enabled,
                                cfg.sram_banked,
                                cfg.sram_enabled,
                                cfg.dd_enabled,
                                cfg.sdram_writable,
                                cfg.sdram_switch
                            } <= bus.wdata[6:0];
                        end
                    end

                    R_DD_OFFSET: begin
                        if (&bus.wmask) begin
                            cfg.dd_offset <= bus.wdata[25:0];
                        end
                    end

                    R_SAVE_OFFSET: begin
                        if (&bus.wmask) begin
                            cfg.save_offset <= bus.wdata[25:0];
                        end
                    end

                    R_RECONFIGURE: begin
                        if (&bus.wmask && bus.wdata == RECONFIGURE_MAGIC) begin
                            trigger_reconfiguration <= 1'b1;
                        end
                    end
                endcase
            end
        end
    end

    logic reconfig_clk;
    logic reconfig_write;
    logic [31:0] reconfig_rdata;
    logic reconfig_write_done;

    const logic [31:0] TRIGGER_RECONFIGURATION = 32'h00000001;

    always_ff @(posedge sys.clk) begin
        if (sys.reset) begin
            reconfig_clk <= 1'b0;
            reconfig_write <= 1'b0;
            reconfig_write_done <= 1'b0;
        end else begin
            reconfig_clk <= ~reconfig_clk;

            if (!reconfig_clk) begin
                reconfig_write <= 1'b0;

                if (trigger_reconfiguration && !reconfig_write_done) begin
                    reconfig_write <= 1'b1;
                    reconfig_write_done <= 1'b1;
                end
            end
        end
    end

    intel_config intel_config_inst (
        .clk(reconfig_clk),
        .nreset(~sys.reset),
        .avmm_rcv_address(3'd0),
        .avmm_rcv_read(1'b0),
        .avmm_rcv_writedata(TRIGGER_RECONFIGURATION),
        .avmm_rcv_write(reconfig_write),
        .avmm_rcv_readdata(reconfig_rdata)
    );

endmodule
