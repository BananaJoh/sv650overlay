#include "kline.h"


/* ================================================================================ Private includes */
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/timer.h"


/* ================================================================================ Private defines */
#define KLINE_BAUD               10400          /* K-line baudrate */
#define KLINE_TX_PIN             GPIO_NUM_17    /* K-line Tx GPIO pin (pin 27) */
#define KLINE_RX_PIN             GPIO_NUM_16    /* K-line Rx GPIO pin (pin 25) */
#define T_01_US                  6000000        /* Initial delay in microseconds */
#define T_02_US                  25000          /* Fast init pulse time in microseconds */
#define T_03_US                  500000         /* Watchdog protocol timeout in microseconds */
#define T_04_US                  200000         /* Post frame delay in microseconds */
#define KLINE_ERROR_THRESHOLD    5              /* Threshold for checksum mismatches in a row before trying to restart session */

/* Buffer sizes */
#define DATA_BUFFER_SIZE         70             /* Same as for BLE */
#define KLINE_RX_BUFFER_SIZE     100
#define UART_RX_BUFFER_SIZE      256

/* Hardware timer clock prescaler and counter value, default clock is APB clock with 80 MHz */
#define TIMER_DIVIDER            2
#define TIMER_SCALE_MS           (TIMER_BASE_CLK / TIMER_DIVIDER / 1000)    


/* ================================================================================ Private types */


/* ================================================================================ Private variables */
/* Protocol handling variables */
static kline_state_t kline_state_                          = KLINE_INIT;
static int64_t       time_us_                              = 0;
static int64_t       kline_last_eof_us_                    = 0;
static int64_t       kline_fastinit_start_us_              = 0;
static int64_t       kline_watchdog_us_                    = 0;
static bool          kline_running_                        = false;

/* Counter and buffer variables */
static uint8_t       kline_error_counter_                  = 0;
static uint8_t       kline_out_counter_                    = 0;
static uint8_t       kline_in_counter_                     = 0;
static uint8_t       kline_in_bytes_[KLINE_RX_BUFFER_SIZE] = { 0 };
static uint8_t       kline_checksum_                       = 0;
static uint8_t       kline_size_                           = 0;

/* Buffer to collect data frame */
static uint8_t       data_buffer_[DATA_BUFFER_SIZE]       = { 0 };
static uint8_t       data_buffer_index_                    = 0;

/* K-line commands */
static const char    KLINE_CMD_START_COM_[5]               = { 0x81, 0x12, 0xF1, 0x81, 0x05 };
static const char    KLINE_CMD_READ_ALL_SENS_[7]           = { 0x80, 0x12, 0xF1, 0x02, 0x21, 0x08, 0xAE };


/* ================================================================================ Private functions */
/* Reset variables after receiving a frame */
static void kline_eof_reset() {
	time_us_ = esp_timer_get_time();
	memset(kline_in_bytes_, 0, KLINE_RX_BUFFER_SIZE);
	kline_out_counter_ = 0;
	kline_in_counter_  = 0;
	kline_checksum_    = 0;
	kline_size_        = 0;
	kline_watchdog_us_ = time_us_;
	kline_last_eof_us_ = time_us_;
}


/* Perform a K-line transmission */
static int8_t kline_transmit(const char* command, size_t len) {
	time_us_ = esp_timer_get_time();

	/* Wait between frames */
	if((kline_out_counter_ < len) && (time_us_ >= kline_last_eof_us_ + T_04_US)) {
		/* Send next bytes */
		int ret = uart_write_bytes(UART_NUM_1, command + kline_out_counter_, len - kline_out_counter_);
		if(ret < 0) {
			return -1;
		}
		kline_out_counter_ += ret;
	}

	size_t rx_bytes = 0;
	if(uart_get_buffered_data_len(UART_NUM_1, &rx_bytes) != ESP_OK) {
		return -2;
	}

	if(rx_bytes > 0) {
		/* Feed protocol watchdog and receive */
		kline_watchdog_us_ = time_us_;
		int rx_size       = uart_read_bytes(UART_NUM_1, kline_in_bytes_, KLINE_RX_BUFFER_SIZE, 5 / portTICK_RATE_MS);

		for(int i = 0; i < rx_size; i++) {
			kline_in_counter_++;
			/* (kline_in_counter_ <= kline_out_counter_) -> ACK bytes of command being sent */
			if(kline_in_counter_ > kline_out_counter_) {
				if(kline_in_counter_ == kline_out_counter_ + 1) {
					/* Reset buffer index */
					data_buffer_index_ = 0;
				} else if(kline_in_counter_ == kline_out_counter_ + 4) {
					/* Retrieve message size field */
					kline_size_ = kline_in_bytes_[i];
				}

				/* Insert byte into data frame buffer  */
				data_buffer_[data_buffer_index_++] = kline_in_bytes_[i];

				if(kline_in_counter_ < kline_out_counter_ + kline_size_ + 5) {
					/* Collect checksum data */
					kline_checksum_ = kline_checksum_ + kline_in_bytes_[i];
				} else if(kline_in_counter_ == kline_out_counter_ + kline_size_ + 5) {
					/* End of frame -> check if checksums match, return -3 (continue) if faulty */
					int8_t ret = -3;
					if(kline_checksum_ == kline_in_bytes_[i]) {
						/* Return 0 (success) if ok */
						ret = 0;
					}
					kline_eof_reset();
					return ret;
				}
			}
		}
	} else {
		/* Nothing to receive */
		if(time_us_ > kline_watchdog_us_ + T_03_US) {
			/* Watchdog protocol timeout */
			kline_eof_reset();
			return -4;
		}
	}

	return 1;
}


/* Perform K-line fast init */
static int8_t kline_fastinit() {
	if(kline_fastinit_start_us_ == 0) {
		kline_fastinit_start_us_ = esp_timer_get_time();
	}
	time_us_ = esp_timer_get_time();

	if(time_us_ < kline_fastinit_start_us_ + T_01_US) {
		/* T_01: Tx high (initial delay) */
		if(uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_INV_DISABLE) != ESP_OK) {
			return -1;
		}
	} else if(time_us_ < kline_fastinit_start_us_ + T_01_US + T_02_US) {
		/* T_02 Tx low (25 ms pulse) */
		if(uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_TXD_INV) != ESP_OK) {
			return -2;
		}
	} else if(time_us_ < kline_fastinit_start_us_ + T_01_US + T_02_US + T_02_US) {
		/* T_02 Tx high (25 ms pulse) */
		if(uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_INV_DISABLE) != ESP_OK) {
			return -3;
		}
	} else {
		/* Fast init sequence finished, start serial communication */
		kline_eof_reset();
		kline_fastinit_start_us_ = 0;
		/* Skip post frame delay */
		kline_last_eof_us_ = 0;
		if(uart_flush_input(UART_NUM_1) != ESP_OK) {
			return -4;
		}

		return 0;
	}

	return 1;
}


/* Millisecond timer callback handling K-line protocol */
static void IRAM_ATTR timer0_isr() {
	/* Enter critical section */
	if(timer_spinlock_take(TIMER_GROUP_1) != ESP_OK) {
		return;
	}

	int8_t ret;
	switch(kline_state_) {
		case KLINE_INIT:
			/* Perform K-line fast init */
			if(kline_fastinit() == 0) {
				/* Go to START_SESSION state on success */
				kline_state_ = KLINE_START_SESSION;
				kline_state_changed_callback(kline_state_);
			}
			break;

		case KLINE_START_SESSION:
			/* Send K-line start sequence */
			ret = kline_transmit(KLINE_CMD_START_COM_, 5);
			if(ret == 0) {
				/* Go to ACTIVE state on success */
				kline_state_ = KLINE_ACTIVE;
				kline_state_changed_callback(kline_state_);
			} else if(ret < 0) {
				/* Go back to INIT state on error */
				kline_state_ = KLINE_INIT;
				kline_state_changed_callback(kline_state_);
			}
			break;

		case KLINE_ACTIVE:
			/* Send sensor data request and process response */
			ret = kline_transmit(KLINE_CMD_READ_ALL_SENS_, 7);
			if(ret == 0) {
				/* Notify via callback on success */
				kline_error_counter_ = 0;
				kline_data_received_callback(data_buffer_, data_buffer_index_);
			} else if(ret == -3) {
				/* Count checksum mismatches and go back to START_SESSION state after KLINE_ERROR_THRESHOLD in a row */
				kline_error_counter_++;
				if(kline_error_counter_ >= KLINE_ERROR_THRESHOLD) {
					kline_state_ = KLINE_START_SESSION;
					kline_state_changed_callback(kline_state_);
				}
			} else if(ret < 0) {
				/* Go back to START_SESSION state on other error (e. g. receive timeout) */
				kline_state_ = KLINE_START_SESSION;
				kline_state_changed_callback(kline_state_);
			}
			break;

		default:
			kline_state_ = KLINE_INIT;
			kline_state_changed_callback(kline_state_);
	}

	/* Clear the timer interrupt */
	timer_group_clr_intr_status_in_isr(TIMER_GROUP_1, TIMER_0);

	/* Reactivate timer alarm */
	timer_group_enable_alarm_in_isr(TIMER_GROUP_1, TIMER_0);

	/* Exit critical section */
	if(timer_spinlock_give(TIMER_GROUP_1) != ESP_OK) {
		return;
	}
}


/* Setup UART1 for K-line communication */
static int uart_init() {
	uart_config_t uart_config = {
		.baud_rate  = KLINE_BAUD,
		.data_bits  = UART_DATA_8_BITS,
		.parity	    = UART_PARITY_DISABLE,
		.stop_bits  = UART_STOP_BITS_1,
		.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_APB,
	};
	if(uart_param_config(UART_NUM_1, &uart_config) != ESP_OK) {
		return -1;
	}
	if(uart_set_pin(UART_NUM_1, KLINE_TX_PIN, KLINE_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
		return -2;
	}
	if(uart_driver_install(UART_NUM_1, UART_RX_BUFFER_SIZE, 0, 0, NULL, 0) != ESP_OK) {
		return -3;
	}

	return 0;
}


/* Setup millisecond timer */
static int protocol_timer_init() {
	timer_config_t timer_config = {
		.divider     = TIMER_DIVIDER,
		.counter_dir = TIMER_COUNT_UP,
		.counter_en  = TIMER_PAUSE,
		.alarm_en    = TIMER_ALARM_EN,
		.auto_reload = TIMER_AUTORELOAD_EN
	};
	if(timer_init(TIMER_GROUP_1, TIMER_0, &timer_config) != ESP_OK) {
		return -1;
	}
	if(timer_set_counter_value(TIMER_GROUP_1, TIMER_0, 0x00000000ULL) != ESP_OK) {
		return -2;
	}

	/* Set timer interrupt every millisecond */
	if(timer_set_alarm_value(TIMER_GROUP_1, TIMER_0, 1 * TIMER_SCALE_MS) != ESP_OK) {
		return -3;
	}
	if(timer_enable_intr(TIMER_GROUP_1, TIMER_0) != ESP_OK) {
		return -4;
	}
	if(timer_isr_register(TIMER_GROUP_1, TIMER_0, timer0_isr, NULL, ESP_INTR_FLAG_IRAM, NULL) != ESP_OK) {
		return -5;
	}

	return 0;
}


/* ================================================================================ Public functions */
int kline_init() {
	if(protocol_timer_init()) {
		return -1;
	}

	return 0;
}


/* Start K-line communication */
int kline_start() {
	if(!kline_running_) {
		kline_running_ = true;

		/* Reset protocol state machine */
		kline_state_             = KLINE_INIT;
		kline_fastinit_start_us_ = 0;
		kline_state_changed_callback(kline_state_);

		/* Init UART and start timer */
		if(uart_init()) {
			return -1;
		}
		if(timer_set_counter_value(TIMER_GROUP_1, TIMER_0, 0x00000000ULL) != ESP_OK) {
			return -2;
		}
		if(timer_start(TIMER_GROUP_1, TIMER_0) != ESP_OK) {
			return -3;
		}
	}

	return 0;
}


/* Stop K-line communication */
int kline_stop() {
	int ret = 0;

	/* Stop timer and deinit UART */
	if(timer_pause(TIMER_GROUP_1, TIMER_0) != ESP_OK) {
		ret = -1;
	}
	if(uart_driver_delete(UART_NUM_1) != ESP_OK) {
		ret = -2;
	}

	gpio_config_t uart_pin_reset_config = {
		.pin_bit_mask = (1ULL << KLINE_TX_PIN) | (1ULL << KLINE_RX_PIN),
		.mode         = GPIO_MODE_DISABLE,
		.pull_up_en   = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type    = GPIO_INTR_DISABLE
	};
	if(gpio_config(&uart_pin_reset_config) != ESP_OK) {
		ret = -3;
	}

	kline_running_ = false;

	return ret;
}
