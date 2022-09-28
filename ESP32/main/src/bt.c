#include "bt.h"


/* ================================================================================ Private includes */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"


/* ================================================================================ Private defines */
/* Buffer size */
#define BT_TX_BUFFER_SIZE    70

/* Bluetooth names */
#define SPP_SERVER_NAME      "SPP_SERVER"
#define BT_DEVICE_NAME       "ESP_SV"


/* ================================================================================ Private types */


/* ================================================================================ Private variables */
/* Bluetooth SPP variables */
xQueueHandle                message_queue_;
static uint32_t             bt_spp_connection_handle_ = 0;
static const esp_spp_mode_t esp_spp_mode_             = ESP_SPP_MODE_CB;
static const esp_spp_sec_t  spp_sec_mask_             = ESP_SPP_SEC_AUTHENTICATE;
static const esp_spp_role_t spp_role_                 = ESP_SPP_ROLE_SLAVE;


/* ================================================================================ Private functions */
/* Send data via Bluetooth SPP */
static int bt_spp_send(uint8_t* bt_message) {
	if(!bt_spp_connection_handle_ || !bt_message) {
		return -1;
	}
	if(esp_spp_write(bt_spp_connection_handle_, bt_message[1], bt_message) != ESP_OK) {
		return -2;
	}

	return 0;
}


/* Task checking for data in the queue and sending it via Bluetooth SPP */
static void bt_spp_task() {
	while(1) {
		uint8_t bt_message[BT_TX_BUFFER_SIZE];
		if(xQueueReceive(message_queue_, bt_message, portMAX_DELAY) != pdTRUE) {
			continue;
		}
		if(bt_spp_send(bt_message)) {
			continue;
		}
	}
}


/* Bluetooth serial port callback */
static void bt_spp_callback(esp_spp_cb_event_t event, esp_spp_cb_param_t* param) {
	switch(event) {
		case ESP_SPP_INIT_EVT:
			if(esp_bt_dev_set_device_name(BT_DEVICE_NAME) != ESP_OK) { }
			if(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE) != ESP_OK) { }
			if(esp_spp_start_srv(spp_sec_mask_, spp_role_, 0, SPP_SERVER_NAME) != ESP_OK) { }
			break;

		case ESP_SPP_DISCOVERY_COMP_EVT:
			break;

		case ESP_SPP_OPEN_EVT:
			break;

		case ESP_SPP_CLOSE_EVT:
			bt_spp_connection_handle_ = 0;
			bt_disconnected_callback();
			break;

		case ESP_SPP_START_EVT:
			break;

		case ESP_SPP_CL_INIT_EVT:
			break;

		case ESP_SPP_DATA_IND_EVT:
			bt_data_received_callback(param->data_ind.data, param->data_ind.len);
			break;

		case ESP_SPP_CONG_EVT:
			break;

		case ESP_SPP_WRITE_EVT:
			break;

		case ESP_SPP_SRV_OPEN_EVT:
			bt_spp_connection_handle_ = param->srv_open.handle;
			bt_connected_callback();
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
void bt_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
	switch(event) {
		case ESP_BT_GAP_AUTH_CMPL_EVT:
			if(param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) { }
			break;

		case ESP_BT_GAP_PIN_REQ_EVT: {
			if(param->pin_req.min_16_digit) {
				esp_bt_pin_code_t pin_code = { 0 };
				if(esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code) != ESP_OK) { }
			} else {
				esp_bt_pin_code_t pin_code;
				pin_code[0] = '1';
				pin_code[1] = '2';
				pin_code[2] = '3';
				pin_code[3] = '4';
				if(esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code) != ESP_OK) { }
			}
			break;
		}

#if(CONFIG_BT_SSP_ENABLED == true)
		case ESP_BT_GAP_CFM_REQ_EVT:
			if(esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true) != ESP_OK) { }
			break;

		case ESP_BT_GAP_KEY_NOTIF_EVT:
			break;

		case ESP_BT_GAP_KEY_REQ_EVT:
			break;
#endif

		default:
			break;
	}
}


/* Setup Bluetooth serial port */
static int bt_spp_init() {
	esp_err_t ret = nvs_flash_init();
	if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		if(nvs_flash_erase() != ESP_OK) {
			return -1;
		}
		ret = nvs_flash_init();
	}
	if(ret != ESP_OK) {
		return -2;
	}
	if(esp_bt_controller_mem_release(ESP_BT_MODE_BLE) != ESP_OK) {
		return -3;
	}

	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
	if(esp_bt_controller_init(&bt_cfg) != ESP_OK) {
		return -4;
	}
	if(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
		return -5;
	}
	if(esp_bluedroid_init() != ESP_OK) {
		return -6;
	}
	if(esp_bluedroid_enable() != ESP_OK) {
		return -7;
	}
	if(esp_bt_gap_register_callback(bt_gap_callback) != ESP_OK) {
		return -8;
	}
	if(esp_spp_register_callback(bt_spp_callback) != ESP_OK) {
		return -9;
	}
	if(esp_spp_init(esp_spp_mode_) != ESP_OK) {
		return -10;
	}

#if(CONFIG_BT_SSP_ENABLED == true)
	/* Set default parameters for Secure Simple Pairing */
	esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
	esp_bt_io_cap_t   iocap      = ESP_BT_IO_CAP_IO;
	if(esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t)) != ESP_OK) {
		return -11;
	}
#endif

	/* Set default parameters for Legacy Pairing (variable pin) */
	esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
	esp_bt_pin_code_t pin_code;
	if(esp_bt_gap_set_pin(pin_type, 0, pin_code) != ESP_OK) {
		return -12;
	}

	return 0;
}


/* ================================================================================ Public functions */
int bt_init() {
	if(bt_spp_init()) {
		return -1;
	}

	message_queue_ = xQueueCreate(3, BT_TX_BUFFER_SIZE);
	if(!message_queue_) {
		return -2;
	}
	if(xTaskCreate(bt_spp_task, "bt_spp_task", 2048, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
		return -3;
	}

	return 0;
}


/* Send a text message */
int bt_send(bt_content_type_t type, const uint8_t* payload, uint8_t payload_size) {
	if(payload_size > (0xFF - 2)) {
		return -1;
	}

	uint8_t message[BT_TX_BUFFER_SIZE];
	message[0] = type;
	message[1] = payload_size + 2;
	memcpy(&message[2], payload, payload_size);

	if(xQueueSendFromISR(message_queue_, &message, NULL) != pdTRUE) {
		return -2;
	}

	return 0;
}
