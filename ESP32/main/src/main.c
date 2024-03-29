/* ================================================================================ Private includes */
#include "esp_system.h"
#include "led.h"
#include "ble.h"
#include "kline.h"


/* ================================================================================ Private defines */


/* ================================================================================ Private types */


/* ================================================================================ Private variables */


/* ================================================================================ Private functions */


/* ================================================================================ Public functions */
/* Application entry point */
void app_main() {
	if(led_init()) {
		esp_restart();
	}
	if(ble_init()) {
		esp_restart();
	}
	if(kline_init()) {
		esp_restart();
	}

	led_on();
}


void ble_connected_callback() {
	led_off();
}


void ble_disconnected_callback() {
	kline_stop();
	led_on();
}


void ble_notify_changed_callback(bool enabled) {
	if(enabled) {
		ble_send(BLE_CONTENT_TEXT, (uint8_t *) "ESP_SV ready", 12);
	}
}


void ble_data_received_callback(const uint8_t* data, uint16_t data_size) {
	if(data_size == 1 && data[0] == 0xFF) {
		kline_stop();
		esp_restart();
	} else if(data_size == 1 && data[0] == 0x01) {
		kline_start();
	} else if(data_size == 1 && data[0] == 0x00) {
		kline_stop();
	}
}


void kline_data_received_callback(const uint8_t* data, uint8_t data_size) {
	ble_send(BLE_CONTENT_DATA, data, data_size);
}


void kline_state_changed_callback(kline_state_t state) {
	switch(state) {
		case KLINE_INIT:          ble_send(BLE_CONTENT_TEXT, (uint8_t *) "INIT",           4); break;
		case KLINE_START_SESSION: ble_send(BLE_CONTENT_TEXT, (uint8_t *) "START_SESSION", 13); break;
		case KLINE_ACTIVE:        ble_send(BLE_CONTENT_TEXT, (uint8_t *) "ACTIVE",         6); break;
		default:                  break;
	}
}
