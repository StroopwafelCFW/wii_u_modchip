/* Small test design actuating all IO on the iCEBreaker dev board. */

module top (
	input  CLK,

	// UART
	output TX,

	output LED1,
	output LED2,
	output LED3,
	output LED4,
	output LED5,

	input BTN_N,
	input BTN1,
	input BTN2,
	input BTN3,

	output LEDR_N,
	output LEDG_N,

	// Unused for now.
	input P1A1, P1A2, P1A3, P1A4, P1A7, P1A8, P1A9, P1A10,
	input P1B4, P1B8, P1B9, P1B10,

	output P1B1, // reset trigger
	inout P1B2, P1B3, // glitch mosfet trigger, EXI mosfet
	input P1B7, // exi clk
);

	// TODO: wat?
	localparam clk_freq = 12_000_000; // 12MHz
	//localparam clk_freq = 42_000_000; // 42MHz
	//localparam baud = 57600;
	localparam baud = 1000000;

	localparam GLITCH_LEN_ROLLOVER = 16'h101;
	localparam GLITCH_ROLLOVER = 16'h300;
	localparam GLITCH_DELAY = 16'h001; // 10
	localparam GLITCH_LEN = 16'h180;

	// 726 = 0x0 bytes of OTP loaded, but JTAG fuse is unloaded!
	// 720 = 0x0 bytes of OTP loaded, but JTAG fuse is still loaded.
	// 710 = ~0x8 bytes of OTP loaded
	// 6B0 = ~0x40 bytes of OTP loaded
	// 690 = ~0x50 bytes of OTP loaded,
	// 669 = ~0x60 bytes of OTP loaded, 
	// 638 = ~0x80 bytes loaded, 
	// 634 it starts to load 0x80 and fail

	// Magic numbers, emulates my BTN1 bounce
	localparam DELAY_0 = 24'hF0000;
	localparam DELAY_1 = 16'h6F4E;
	localparam DELAY_2 = 16'h726;
	localparam DELAY_1_MAX = 16'h7000;
	localparam DELAY_2_MAX = 16'h727;

	// "NDEV_LED" GPIOs
	wire [7:0] WIIU_DEBUG_LIVE;
	assign WIIU_DEBUG_LIVE = {P1A10, P1A9, P1A8, P1A7, P1A4, P1A3, P1A2, P1A1};
	reg [7:0] WIIU_DEBUG = 8'hAA;
	reg [7:0] LAST_WIIU_DEBUG = 8'h55;

	// UART queue
	reg [1023:0] WIIU_DEBUG_HISTORY;
	reg [7:0] WIIU_DEBUG_HISTORY_LEN;
	reg perma_stop_reset = 0;

	/* instantiate the tx1 module */
	wire clk_12mhz;
	assign clk_12mhz = CLK;
	reg tx1_start;
	reg [7:0] tx1_data;
	reg tx1_busy;
	uart_tx #(clk_freq, baud) utx1 (
		.clk(clk_12mhz),
		.tx_start(tx1_start),
		.tx_data(tx1_data),
		.tx(TX),
		.tx_busy(tx1_busy)
	);

	reg buffer_btn1;
	reg buffer_btn2;
	reg last_buffer_btn1;
	reg last_buffer_btn2;

	// UART state
	reg data_check_busy = 0;
	reg data_flag = 0;
	reg [7:0] data_tx = 0;

	reg reset_trigger = 0;

	// Button bounce emulation
	reg [15:0] button_delay_1_inc = DELAY_1;
	reg [15:0] button_delay_2_inc = DELAY_2;
	reg [23:0] button_delay_0 = 0;
	reg [15:0] button_delay_1 = 0;
	reg [15:0] button_delay_2 = 0;

	// Button bounce logging
	reg [15:0] last_button_bounce_3 = 0;
	reg [15:0] last_button_bounce_2 = 0;
	reg [15:0] last_button_bounce = 0;
	reg [15:0] button_bounce = 0;

	reg [15:0] glitch_len_iter = GLITCH_LEN;
	reg [15:0] reset_counter = 0;
	reg [15:0] glitch_counter = 0;
	reg [15:0] glitch_counter_iter = GLITCH_DELAY + GLITCH_LEN;
	reg [15:0] last_glitch_counter_iter = GLITCH_DELAY + GLITCH_LEN; // 4AC7

	// h3FA5 = 0xD
	// h1f69 = ? weird roadbump

	reg exi_clk = 0;
	reg last_exi_clk = 0;
	reg [15:0] exi_clk_cnt = 0;
	//reg [63:0] exi_dat = 0;
	//reg [127:0] exi_dat_in = 128'h0;
	reg glitch_trigger = 0;
	reg is_serial = 0;
	reg [3:0] serial_bits;
	reg [7:0] serial_byte;
	reg got_bit = 0;
	reg last_bit7 = 0;

	always @(posedge CLK) begin
		buffer_btn1 <= BTN1;
		last_buffer_btn1 <= buffer_btn1;
		buffer_btn2 <= BTN2;
		last_buffer_btn2 <= buffer_btn2;

		WIIU_DEBUG <= WIIU_DEBUG_LIVE;

		button_bounce <= button_bounce + 1;

		// EXI CLK P1B7 TP101
		// ? P1B8 1 CS? TP102
		// ? P1B9 6 MISO? TP176
		// ? P1B10 6 MOSI? TPidk
		exi_clk <= P1B7;
		last_exi_clk <= exi_clk;

		last_glitch_counter_iter <= glitch_counter_iter;

		// UART flushing
		if(data_flag) begin
			// First check if the previous transmission is over
			if(data_check_busy) begin
			  	if(~tx1_busy) begin
			    	data_check_busy <= 0;
			  	end // if(~tx1_busy)

			end else begin // try to send waiting for busy to go high to make sure
			  	if(~tx1_busy) begin
			    	tx1_data <= data_tx;
			    	tx1_start <= 1'b1;
			  	end else begin // Yey we did it!
			    	tx1_start <= 1'b0;
			    	data_flag <= 0;
			  	end
			end
		end

		// UART queue:
		// We either push count info to WIIU_DEBUG_HISTORY,
		// a changed value to WIIU_DEBUG_HISTORY,
		// or we shift one value off.
		if (last_glitch_counter_iter != glitch_counter_iter && !perma_stop_reset) begin
			//WIIU_DEBUG_HISTORY <= (WIIU_DEBUG_HISTORY << 104) | (glitch_counter_iter[15:0] << 72) | (16'h55aa << 88) | (exi_clk_cnt << 8) | 8'h55;
			//WIIU_DEBUG_HISTORY_LEN <= WIIU_DEBUG_HISTORY_LEN + 13;


			//WIIU_DEBUG_HISTORY <= (WIIU_DEBUG_HISTORY << 72) | (16'h55aa << 56) | (glitch_counter_iter[15:0] << 40) | (button_delay_1_inc[15:0] << 24) | (button_delay_2_inc[15:0] << 8) | 8'h55;

			WIIU_DEBUG_HISTORY <= (WIIU_DEBUG_HISTORY << 72) | (16'h55aa << 56) | (last_button_bounce_3[15:0] << 40) | (last_button_bounce_2[15:0] << 24) | (last_button_bounce[15:0] << 8) | 8'h55;

			WIIU_DEBUG_HISTORY_LEN <= WIIU_DEBUG_HISTORY_LEN + 9;
		end
		else if (is_serial && (WIIU_DEBUG != LAST_WIIU_DEBUG) && !BTN3) begin
			if (WIIU_DEBUG[7] && !LAST_WIIU_DEBUG[7] && ((WIIU_DEBUG & 8'h7E) == 8'h00)) begin
				if (serial_bits <= 7)  begin
					serial_byte <= (serial_byte << 1) | WIIU_DEBUG[0];
					serial_bits <= serial_bits + 1;

					WIIU_DEBUG_HISTORY <= WIIU_DEBUG_HISTORY;
					WIIU_DEBUG_HISTORY_LEN <= WIIU_DEBUG_HISTORY_LEN;
					//WIIU_DEBUG_HISTORY <= WIIU_DEBUG_HISTORY | (((serial_byte << 1) | WIIU_DEBUG[0]) << (8*WIIU_DEBUG_HISTORY_LEN));
					//WIIU_DEBUG_HISTORY_LEN <= WIIU_DEBUG_HISTORY_LEN + 1;

					got_bit <= 1;
				end
			end
			else if (WIIU_DEBUG == 8'h8F && serial_bits >= 8) begin
				WIIU_DEBUG_HISTORY <= WIIU_DEBUG_HISTORY | (serial_byte << (8*WIIU_DEBUG_HISTORY_LEN));
				WIIU_DEBUG_HISTORY_LEN <= WIIU_DEBUG_HISTORY_LEN + 1;

				got_bit <= 0;

				serial_byte <= 0;
				serial_bits <= 0;
			end
			else if (WIIU_DEBUG == 8'h8F && serial_bits < 8) begin
				
				got_bit <= 0;

				serial_byte <= 0;
				serial_bits <= 0;
			end
			else begin
				WIIU_DEBUG_HISTORY <= WIIU_DEBUG_HISTORY;
				WIIU_DEBUG_HISTORY_LEN <= WIIU_DEBUG_HISTORY_LEN;
				got_bit <= 0;
			end

			
			LAST_WIIU_DEBUG <= WIIU_DEBUG;

			// We're looking for 0x8F to signal end of serial transmission.
			if (WIIU_DEBUG == 8'h25) begin
				//perma_stop_reset <= 1;
				//is_serial <= 0;
			end
			else if (WIIU_DEBUG == 8'h8F && LAST_WIIU_DEBUG == 8'h0F) begin
				//is_serial <= 0;
			end
			else if (WIIU_DEBUG == 8'hc3
					 || WIIU_DEBUG == 8'hda 
					 || WIIU_DEBUG == 8'he1 
					 || WIIU_DEBUG == 8'h0d 
					 || WIIU_DEBUG == 8'h1d) begin
				//perma_stop_reset <= 0;
				//is_serial <= 0;
			end
		end 
		else if (!is_serial && (WIIU_DEBUG != LAST_WIIU_DEBUG || (buffer_btn2 && !last_buffer_btn2)) && !BTN3) begin
			WIIU_DEBUG_HISTORY <= WIIU_DEBUG_HISTORY | (WIIU_DEBUG << (8*WIIU_DEBUG_HISTORY_LEN));
			WIIU_DEBUG_HISTORY_LEN <= WIIU_DEBUG_HISTORY_LEN + 1;
			LAST_WIIU_DEBUG <= WIIU_DEBUG;

			// 0x25 and 0x88 are our winners.
			// Everything else is just on the off chance that the pins
			// float to 0x88 or 0x25 during reset or booting.
			if (WIIU_DEBUG == 8'h88
				|| WIIU_DEBUG == 8'h25) begin
				perma_stop_reset <= 1;
				is_serial <= 0;
			end
			else if (WIIU_DEBUG == 8'h8F && LAST_WIIU_DEBUG == 8'h0F) begin
				is_serial <= 1; // We're looking for 0x8F to signal start of serial transmission.
			end
			else if (WIIU_DEBUG == 8'hc3
					 || WIIU_DEBUG == 8'hda 
					 || WIIU_DEBUG == 8'he1 
					 || WIIU_DEBUG == 8'h0d 
					 || WIIU_DEBUG == 8'h1d) begin
				perma_stop_reset <= 0;
				is_serial <= 0;
			end
		end else begin
			//LAST_WIIU_DEBUG <= WIIU_DEBUG;
			if (!data_flag && WIIU_DEBUG_HISTORY_LEN) begin
				data_tx <= WIIU_DEBUG_HISTORY[7:0];
				WIIU_DEBUG_HISTORY_LEN <= WIIU_DEBUG_HISTORY_LEN - 1;
			    WIIU_DEBUG_HISTORY <= WIIU_DEBUG_HISTORY >> 8;

				data_flag <= 1;
				data_check_busy <= 1;
			end
		end

		//LAST_WIIU_DEBUG <= WIIU_DEBUG;

		//data_buf <= WIIU_DEBUG;

		// EXI handling, used to read values but for some reason
		// the FPGA pins would drop the voltage too much.
		if (reset_trigger) begin
			exi_clk_cnt <= 0;
			//exi_dat <= 0;
			//exi_dat_in <= 128'hFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF; // TODO why is this shift needed
		end
		else if (exi_clk && !last_exi_clk && !reset_trigger && exi_clk_cnt < 16'h80) begin
			exi_clk_cnt <= exi_clk_cnt + 1;

			//exi_dat_tmp <= exi_dat_in[63];
			//exi_dat_in <= exi_dat_in << 1;
		end
		else if (!exi_clk && last_exi_clk && !reset_trigger && exi_clk_cnt < 16'h80) begin
			//exi_clk_cnt <= exi_clk_cnt + 1;
			//exi_dat <= (exi_dat << 1) | P1B9;

			//exi_dat_tmp <= exi_dat_in[0];
			//exi_dat_in <= exi_dat_in >> 1;
		end

		// Button-instantiated glitch run
		if (buffer_btn1 && !last_buffer_btn1) begin
			last_button_bounce_3 <= last_button_bounce_2;
			last_button_bounce_2 <= last_button_bounce;
			last_button_bounce <= button_bounce;
			button_bounce <= 0;

			//perma_stop_reset <= 0;
			reset_counter <= 16'h200;
			glitch_counter <= glitch_counter_iter;//8'h0; // interesting thresholds: 0x7c -> boot1 fails to start
			if (glitch_counter_iter < GLITCH_ROLLOVER + glitch_len_iter) begin
				glitch_counter_iter <= glitch_counter_iter + 1;
			end
			else begin
				glitch_counter_iter <= GLITCH_DELAY + glitch_len_iter;
				
				if (glitch_len_iter < GLITCH_LEN_ROLLOVER) begin
					glitch_len_iter <= glitch_len_iter + 1;
				end
				else begin
					glitch_len_iter <= GLITCH_LEN;
				end
				
			end
		end

		// Manual reset hold on BTN3, plus reset counter
		if (BTN3) begin
			reset_trigger <= 1;
			is_serial <= 0;
			perma_stop_reset <= 0;
			//glitch_counter <= GLITCH_DELAY + glitch_len_iter;
		end
		else if (reset_counter) begin
			reset_counter <= reset_counter - 1;
			reset_trigger <= 1;
		end 
		else begin
			reset_trigger <= 0;
		end

		// Emulate my button bouncing
		if (button_delay_0) begin
			if (button_delay_0 == 1) begin
				buffer_btn1 <= 1;
			end
			button_delay_0 <= button_delay_0 - 1;
		end
		else if (button_delay_1) begin
			if (button_delay_1 == 1) begin
				buffer_btn1 <= 1;
			end
			button_delay_1 <= button_delay_1 - 1;
		end
		else if (button_delay_2) begin
			if (button_delay_2 == 1) begin
				buffer_btn1 <= 1;
			end
			button_delay_2 <= button_delay_2 - 1;
		end
		else begin
			button_delay_0 <= DELAY_0;
			if (button_delay_1_inc > DELAY_1_MAX) begin
				button_delay_1_inc <= DELAY_1;
				button_delay_2_inc <= button_delay_2_inc + 1;
			end
			else begin
				button_delay_1_inc <= button_delay_1_inc + 1;
			end

			if (button_delay_2_inc > DELAY_2_MAX) begin
				button_delay_2_inc <= DELAY_2;
			end

			button_delay_1 <= button_delay_1_inc;
			button_delay_2 <= button_delay_2_inc;			
		end

	end

	// UART blinkenlights
	assign LEDR_N = ~data_tx[0];
	assign LEDG_N = ~data_tx[1];

	assign {LED1, LED2, LED3, LED4} = WIIU_DEBUG[7:4];
	assign P1B1 = !(reset_trigger && !perma_stop_reset);
	assign {LED5} = buffer_btn1;

	// Force UNSTBL_PWR on boot:
	// My p-channel MOSFET is crusty and slow, so we trigger earlier than we should.
	// Nominally, this would trigger after 64ish clocks, so that the 03030303 check
	// passes and the next read is just FFs.
	assign P1B3 = (exi_clk_cnt > 16'h20 && exi_clk_cnt < 16'h80) ? 1'bz : 1'b1;//exi_dat_tmp; //

endmodule
