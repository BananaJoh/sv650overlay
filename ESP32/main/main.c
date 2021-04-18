#include <string.h>
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/timer.h"


/* LED GPIO pin */
#define LED_GPIO                   2
/* K-Line baudrate */
#define KLINE_BAUD             10400
/* K-Line Tx GPIO pin (pin 27) */
#define KLINE_TX_PIN          GPIO_NUM_17
/* K-Line Rx GPIO pin (pin 25) */
#define KLINE_RX_PIN          GPIO_NUM_16
/* Initial delay in microseconds */
#define T_01_US              6000000
/* Fast init pulse time in microseconds */
#define T_02_US                25000
/* Watchdog protocol timeout in microseconds */
#define T_03_US               500000
/* Post frame delay in microseconds */
#define T_04_US               200000
/* Threshold for checksum mismatches in a row before trying to restart session */
#define KLINE_ERROR_THRESHOLD      5
/* Buffer sizes */
#define BT_TX_BUFFER_SIZE         70
#define KLINE_RX_BUFFER_SIZE     100
#define UART_RX_BUFFER_SIZE      256
/* Bluetooth names */
#define SPP_SERVER_NAME       "SPP_SERVER"
#define BT_DEVICE_NAME        "ESP_SV"
/* Hardware timer clock prescaler, default clock is APB clock with 80 MHz */
#define TIMER_DIVIDER              2
/* Convert counter value to milliseconds */
#define TIMER_SCALE_MS        (TIMER_BASE_CLK / TIMER_DIVIDER / 1000)


/* Message content types */
enum {
	CONTENT_TYPE_DATA = 0x01,
	CONTENT_TYPE_TEXT = 0x02
};


/* K-Line states */
enum kline_states {
	INIT,
	START_SESSION,
	ACTIVE
} static          kline_state                          = INIT;
/* Protocol handling variables */
static int64_t    time_us                              = 0;
static int64_t    kline_last_eof_us                    = 0;
static int64_t    kline_fastinit_start_us              = 0;
static int64_t    kline_watchdog_us                    = 0;
static bool       kline_running                        = false;
/* Counter and buffer variables */
static uint8_t    kline_err_cntr                       = 0;
static uint8_t    kline_out_cntr                       = 0;
static uint8_t    kline_in_cntr                        = 0;
static uint8_t    kline_in_bytes[KLINE_RX_BUFFER_SIZE] = { 0 };
static uint8_t    kline_chksm                          = 0;
static uint8_t    kline_size                           = 0;
/* Buffer for data frame to send via Bluetooth */
static uint8_t    data_message[BT_TX_BUFFER_SIZE]      = { 0 };
static uint8_t    data_message_index                   = 2;
/* K-Line start sequence */
static const char KLINE_CMD_START_COM[5]               = { 0x81, 0x12, 0xF1, 0x81, 0x05 };
/* K-Line sensor data request */
static const char KLINE_CMD_READ_ALL_SENS[7]           = { 0x80, 0x12, 0xF1, 0x02, 0x21, 0x08, 0xAE };
/* Bluetooth SPP variables */
xQueueHandle      message_queue;
static uint32_t   bt_spp_conn_handle                   = 0;
static const      esp_spp_mode_t esp_spp_mode          = ESP_SPP_MODE_CB;
static const      esp_spp_sec_t  sec_mask              = ESP_SPP_SEC_AUTHENTICATE;
static const      esp_spp_role_t role_slave            = ESP_SPP_ROLE_SLAVE;


/* Setup UART1 for K-Line communication */
static void init_uart() {
	uart_config_t uart_config = {
		.baud_rate  = KLINE_BAUD,
		.data_bits  = UART_DATA_8_BITS,
		.parity	    = UART_PARITY_DISABLE,
		.stop_bits  = UART_STOP_BITS_1,
		.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_APB,
	};
	uart_param_config(UART_NUM_1, &uart_config);
	uart_set_pin(UART_NUM_1, KLINE_TX_PIN, KLINE_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
	uart_driver_install(UART_NUM_1, UART_RX_BUFFER_SIZE, 0, 0, NULL, 0);
}


/* Send a text message  */
static void send_text_message(const char* text) {
	size_t length = strlen(text) + 2;
	if(length > 0xFF) {
		return;
	}

	uint8_t text_message[BT_TX_BUFFER_SIZE];
	/* Set the content type byte to 2 for text */
	text_message[0] = CONTENT_TYPE_TEXT;
	text_message[1] = length;
	memcpy(&text_message[2], text, length - 2);

	xQueueSendFromISR(message_queue, &text_message, NULL);
}


/* Start K-Line communication */
static void start_kline() {
	if(!kline_running) {
		kline_running           = true;

		/* Reset protocol state machine */
		kline_state             = INIT;
		kline_fastinit_start_us = 0;
		send_text_message("INIT");

		/* Init UART and start timer */
		init_uart();
		timer_set_counter_value(TIMER_GROUP_1, TIMER_0, 0x00000000ULL);
		timer_start(TIMER_GROUP_1, TIMER_0);
	}
}


/* Stop K-Line communication */
static void stop_kline() {
	/* Stop timer and deinit UART */
	timer_pause(TIMER_GROUP_1, TIMER_0);
	uart_driver_delete(UART_NUM_1);

	gpio_config_t uart_pin_reset_config = {
		.pin_bit_mask = (1ULL << KLINE_TX_PIN) | (1ULL << KLINE_RX_PIN),
		.mode         = GPIO_MODE_DISABLE,
		.pull_up_en   = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type    = GPIO_INTR_DISABLE
	};
	gpio_config(&uart_pin_reset_config);

	kline_running = false;
}


/* Reset variables after receiving a frame */
static void kline_eof_reset() {
	time_us = esp_timer_get_time();
	memset(kline_in_bytes, 0, KLINE_RX_BUFFER_SIZE);
	kline_out_cntr    = 0;
	kline_in_cntr     = 0;
	kline_chksm       = 0;
	kline_size        = 0;
	kline_watchdog_us = time_us;
	kline_last_eof_us = time_us;
}


/* Perform a K-Line transmission */
static int8_t kline_transmit(const char* command, size_t len) {
	time_us = esp_timer_get_time();

	/* Wait between frames */
	if((kline_out_cntr < len) && (time_us >= kline_last_eof_us + T_04_US)) {
		/* Send next bytes */
		kline_out_cntr += uart_write_bytes(UART_NUM_1, command + kline_out_cntr, len - kline_out_cntr);
	}

	size_t rx_bytes = 0;
	ESP_ERROR_CHECK(uart_get_buffered_data_len(UART_NUM_1, &rx_bytes));

	if(rx_bytes > 0) {
		/* Feed protocol watchdog and receive */
		kline_watchdog_us = time_us;
		int rx_size       = uart_read_bytes(UART_NUM_1, kline_in_bytes, KLINE_RX_BUFFER_SIZE, 5 / portTICK_RATE_MS);

		for(int i = 0; i < rx_size; i++) {
			kline_in_cntr++;
			/* (kline_in_cntr <= kline_out_cntr) -> ACK bytes of command being sent */
			if(kline_in_cntr > kline_out_cntr) {
				if(kline_in_cntr == kline_out_cntr + 1) {
					/* Reset content type byte to 1 == binary and message length to 0 */
					data_message[0]    = CONTENT_TYPE_DATA;
					data_message[1]    = 0;
					/* Start at index 2 because index 1 is filled with the size byte later */
					data_message_index = 2;
				} else if(kline_in_cntr == kline_out_cntr + 4) {
					/* Retrieve message size field */
					kline_size = kline_in_bytes[i];
				}

				/* Insert byte into data frame buffer  */
				data_message[data_message_index++] = kline_in_bytes[i];

				if(kline_in_cntr < kline_out_cntr + kline_size + 5) {
					/* Collect checksum data */
					kline_chksm = kline_chksm + kline_in_bytes[i];
				} else if(kline_in_cntr == kline_out_cntr + kline_size + 5) {
					/* End of frame -> check if checksums match, return -1 (continue) if faulty */
					int8_t ret = -1;
					if(kline_chksm == kline_in_bytes[i]) {
						/* Set message length and return 0 (success) if ok */
						data_message[1] = data_message_index;
						ret             = 0;
					}
					kline_eof_reset();
					return ret;
				}
			}
		}
	} else {
		/* Nothing to receive */
		if(time_us > kline_watchdog_us + T_03_US) {
			/* Watchdog protocol timeout */
			kline_eof_reset();
			return -2;
		}
	}
	return 1;
}


/* Perform K-Line fast init */
static int8_t kline_fastinit() {
	if(kline_fastinit_start_us == 0) {
		kline_fastinit_start_us = esp_timer_get_time();
	}
	time_us = esp_timer_get_time();

	if(time_us < kline_fastinit_start_us + T_01_US) {
		/* T_01: Tx high (initial delay) */
		uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_INV_DISABLE);
	} else if(time_us < kline_fastinit_start_us + T_01_US + T_02_US) {
		/* T_02 Tx low (25 ms pulse) */
		uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_TXD_INV);
	} else if(time_us < kline_fastinit_start_us + T_01_US + T_02_US + T_02_US) {
		/* T_02 Tx high (25 ms pulse) */
		uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_INV_DISABLE);
	} else {
		/* Fast init sequence finished, start serial communication */
		kline_eof_reset();
		kline_fastinit_start_us = 0;
		/* Skip post frame delay */
		kline_last_eof_us = 0;
		ESP_ERROR_CHECK(uart_flush_input(UART_NUM_1));
		return 0;
	}
	return 1;
}


/* Millisecond timer callback handling K-Line protocol */
static void IRAM_ATTR timer0_isr() {
	/* Enter critical section */
	timer_spinlock_take(TIMER_GROUP_1);

	int8_t ret;
	switch(kline_state) {
		case INIT:
			/* Perform K-Line fast init */
			if(kline_fastinit() == 0) {
				/* Go to START_SESSION state on success */
				kline_state = START_SESSION;
				send_text_message("START_SESSION");
			}
			break;

		case START_SESSION:
			/* Send K-Line start sequence */
			ret = kline_transmit(KLINE_CMD_START_COM, 5);
			if(ret == 0) {
				/* Go to ACTIVE state on success */
				kline_state = ACTIVE;
				send_text_message("ACTIVE");
			} else if(ret < 0) {
				/* Go back to INIT state on error */
				kline_state = INIT;
				send_text_message("INIT");
			}
			break;

		case ACTIVE:
			/* Send sensor data request and process response */
			ret = kline_transmit(KLINE_CMD_READ_ALL_SENS, 7);
			if(ret == 0) {
				/* Send data via Bluetooth on success */
				kline_err_cntr = 0;
				xQueueSendFromISR(message_queue, &data_message, NULL);
			} else if(ret == -1) {
				/* Count checksum mismatches and go back to START_SESSION state after KLINE_ERROR_THRESHOLD in a row */
				kline_err_cntr++;
				if(kline_err_cntr >= KLINE_ERROR_THRESHOLD) {
					kline_state = START_SESSION;
					send_text_message("START_SESSION");
				}
			} else if(ret == -2) {
				/* Go back to START_SESSION state on error (receive timeout) */
				kline_state = START_SESSION;
				send_text_message("START_SESSION");
			}
			break;

		default:
			kline_state = INIT;
			send_text_message("INIT");
	}

	/* Clear the timer interrupt */
	timer_group_clr_intr_status_in_isr(TIMER_GROUP_1, TIMER_0);

	/* Reactivate timer alarm */
	timer_group_enable_alarm_in_isr(TIMER_GROUP_1, TIMER_0);

	/* Exit critical section */
	timer_spinlock_give(TIMER_GROUP_1);
}


/* Setup millisecond timer */
static void init_timer() {
	timer_config_t timer_config = {
		.divider     = TIMER_DIVIDER,
		.counter_dir = TIMER_COUNT_UP,
		.counter_en  = TIMER_PAUSE,
		.alarm_en    = TIMER_ALARM_EN,
		.auto_reload = TIMER_AUTORELOAD_EN
	};
	timer_init(TIMER_GROUP_1, TIMER_0, &timer_config);
	timer_set_counter_value(TIMER_GROUP_1, TIMER_0, 0x00000000ULL);

	/* Set timer interrupt every millisecond */
	timer_set_alarm_value(TIMER_GROUP_1, TIMER_0, 1 * TIMER_SCALE_MS);
	timer_enable_intr(TIMER_GROUP_1, TIMER_0);
	timer_isr_register(TIMER_GROUP_1, TIMER_0, timer0_isr, NULL, ESP_INTR_FLAG_IRAM, NULL);
}


/* Send data via Bluetooth SPP */
static void bt_spp_send(uint8_t* bt_message) {
	if(bt_spp_conn_handle && bt_message) {
		esp_spp_write(bt_spp_conn_handle, bt_message[1], bt_message);
	}
}


/* Task checking for data in the queue and sending it via Bluetooth SPP */
static void bt_spp_task() {
	while(1) {
		uint8_t bt_message[BT_TX_BUFFER_SIZE];
		xQueueReceive(message_queue, bt_message, portMAX_DELAY);
		bt_spp_send(bt_message);
	}
}


/* Process data received from Bluetooth serial port */
static void bt_spp_process_rxdata(uint8_t* data, uint16_t len) {
	if(len == 1 && data[0] == 0xFF) {
		stop_kline();
		esp_restart();
	} else if(len == 1 && data[0] == 0x01) {
		start_kline();
	} else if(len == 1 && data[0] == 0x00) {
		stop_kline();
	}
}


/* Bluetooth serial port callback */
static void bt_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t* param) {
	switch(event) {
		case ESP_SPP_INIT_EVT:
			esp_bt_dev_set_device_name(BT_DEVICE_NAME);
			esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
			esp_spp_start_srv(sec_mask, role_slave, 0, SPP_SERVER_NAME);
			break;
		case ESP_SPP_DISCOVERY_COMP_EVT:
			break;
		case ESP_SPP_OPEN_EVT:
			break;
		case ESP_SPP_CLOSE_EVT:
			bt_spp_conn_handle = 0;
			stop_kline();
			gpio_set_level(LED_GPIO, 1);
			break;
		case ESP_SPP_START_EVT:
			gpio_set_level(LED_GPIO, 1);
			break;
		case ESP_SPP_CL_INIT_EVT:
			break;
		case ESP_SPP_DATA_IND_EVT:
			bt_spp_process_rxdata(param->data_ind.data, param->data_ind.len);
			break;
		case ESP_SPP_CONG_EVT:
			break;
		case ESP_SPP_WRITE_EVT:
			break;
		case ESP_SPP_SRV_OPEN_EVT:
			bt_spp_conn_handle = param->srv_open.handle;
			gpio_set_level(LED_GPIO, 0);
			send_text_message("ESP_SV ready");
			break;
		case ESP_SPP_SRV_STOP_EVT:
			break;
		case ESP_SPP_UNINIT_EVT:
			break;
		default:
			break;
	}
}


/* Bluetooth generic access callback */
void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
	switch(event) {
		case ESP_BT_GAP_AUTH_CMPL_EVT:{
			if(param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
			}
			break;
		}
		case ESP_BT_GAP_PIN_REQ_EVT:{
			if(param->pin_req.min_16_digit) {
				esp_bt_pin_code_t pin_code = {0};
				esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
			} else {
				esp_bt_pin_code_t pin_code;
				pin_code[0] = '1';
				pin_code[1] = '2';
				pin_code[2] = '3';
				pin_code[3] = '4';
				esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
			}
			break;
		}
#if(CONFIG_BT_SSP_ENABLED == true)
		case ESP_BT_GAP_CFM_REQ_EVT:
			esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
			break;
		case ESP_BT_GAP_KEY_NOTIF_EVT:
			break;
		case ESP_BT_GAP_KEY_REQ_EVT:
			break;
#endif
		default: {
			break;
		}
	}
	return;
}


/* Setup Bluetooth serial port */
static void init_bt_spp() {
	esp_err_t ret = nvs_flash_init();
	if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
	if((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
		return;
	}

	if((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
		return;
	}

	if((ret = esp_bluedroid_init()) != ESP_OK) {
		return;
	}

	if((ret = esp_bluedroid_enable()) != ESP_OK) {
		return;
	}

	if((ret = esp_bt_gap_register_callback(bt_gap_cb)) != ESP_OK) {
		return;
	}

	if((ret = esp_spp_register_callback(bt_spp_cb)) != ESP_OK) {
		return;
	}

	if((ret = esp_spp_init(esp_spp_mode)) != ESP_OK) {
		return;
	}

#if(CONFIG_BT_SSP_ENABLED == true)
	/* Set default parameters for Secure Simple Pairing */
	esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
	esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
	esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
#endif

	/* Set default parameters for Legacy Pairing (variable pin) */
	esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
	esp_bt_pin_code_t pin_code;
	esp_bt_gap_set_pin(pin_type, 0, pin_code);
}


/* Setup LED gpio */
static void init_led() {
	gpio_config_t io_conf;
	io_conf.intr_type    = GPIO_PIN_INTR_DISABLE;
	io_conf.mode         = GPIO_MODE_OUTPUT;
	io_conf.pin_bit_mask = 1ULL << LED_GPIO;
	io_conf.pull_down_en = 0;
	io_conf.pull_up_en   = 0;
	gpio_config(&io_conf);

	gpio_set_level(LED_GPIO, 0);
}


/* Entry point */
void app_main() {
	message_queue = xQueueCreate(3, BT_TX_BUFFER_SIZE);
	init_led();
	init_bt_spp();
	init_timer();
	xTaskCreate(bt_spp_task, "bt_spp_task", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
}
