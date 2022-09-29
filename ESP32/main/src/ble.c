#include "ble.h"


/* ================================================================================ Private includes */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "esp_nimble_hci.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"


/* ================================================================================ Private defines */
#define TX_BUFFER_SIZE                  70
#define DEVICE_NAME                     "ESP_SV"

/* 16 Bit SPP Service and Characteristic UUIDs */
#define SPP_SERVICE_UUID16              0xABF0
#define SPP_RX_CHARACTERISTIC_UUID16    0xABF1
#define SPP_TX_CHARACTERISTIC_UUID16    0xABF2


/* ================================================================================ Private types */


/* ================================================================================ Private prototypes */
static int spp_service_gatt_handler(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg);
static int ble_advertise();


/* ================================================================================ Private variables */
xQueueHandle    message_queue_;
static uint16_t tx_value_handle_     = 0;
static uint16_t connection_handle_   = 0;
static bool     connected_           = false;
static bool     notify_enabled_      = false;

/* Define new custom service */
static const struct ble_gatt_svc_def gatt_services_defintion_[] = {
	{
		/* SPP service */
		.type            = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid            = BLE_UUID16_DECLARE(SPP_SERVICE_UUID16),
		.characteristics = (struct ble_gatt_chr_def[]) {
			{
				/* Characteristic to receive data from the remote device */
				.uuid       = BLE_UUID16_DECLARE(SPP_RX_CHARACTERISTIC_UUID16),
				.access_cb  = spp_service_gatt_handler,
				.flags      = BLE_GATT_CHR_F_WRITE_NO_RSP,
			}, {
				/* Characteristic to send data to the remote device */
				.uuid       = BLE_UUID16_DECLARE(SPP_TX_CHARACTERISTIC_UUID16),
				.access_cb  = spp_service_gatt_handler,
				.val_handle = &tx_value_handle_,
				.flags      = BLE_GATT_CHR_F_NOTIFY,
			}, {
				0, /* No more characteristics */
			}
		},
	}, {
		0, /* No more services. */
	},
};


/* ================================================================================ Private functions */
static int ble_spp_send(uint8_t* message) {
	if(!connected_ || !notify_enabled_ || !message) {
		return -1;
	}

	/* Second element contains size */
	struct os_mbuf *om = ble_hs_mbuf_from_flat(message, message[1]);
	if(ble_gattc_notify_custom(connection_handle_, tx_value_handle_, om)) {
		return -2;
	}

	return 0;
}


/* Task checking for data in the queue and sending it via Bluetooth LE SPP */
static void ble_send_task() {
	while(1) {
		uint8_t message[TX_BUFFER_SIZE];
		if(xQueueReceive(message_queue_, message, portMAX_DELAY) != pdTRUE) {
			continue;
		}
		if(ble_spp_send(message)) {
			continue;
		}
	}
}


/* Callback function for custom service */
static int spp_service_gatt_handler(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg) {
	switch(ctxt->op) {
		case BLE_GATT_ACCESS_OP_READ_CHR:
			break;

		case BLE_GATT_ACCESS_OP_WRITE_CHR:
			ble_data_received_callback(ctxt->om->om_data, ctxt->om->om_len);
			break;

		default:
			break;
	}

	return 0;
}


static int ble_gap_event_callback(struct ble_gap_event* event, void* arg) {
	switch(event->type) {
		case BLE_GAP_EVENT_CONNECT:
			/* A new connection was established or a connection attempt failed */
			if(!event->connect.status) {
				connection_handle_ = event->connect.conn_handle;
				connected_         = true;
				notify_enabled_    = false;
				ble_connected_callback();
			} else {
				/* Connection failed, resume advertising */
				connected_      = false;
				notify_enabled_ = false;
				ble_advertise();
			}
			break;

		case BLE_GAP_EVENT_DISCONNECT:
			/* Connection terminated, resume advertising */
			connected_      = false;
			notify_enabled_ = false;
			ble_advertise();
			ble_disconnected_callback();
			break;

		case BLE_GAP_EVENT_ADV_COMPLETE:
			ble_advertise();
			break;

		case BLE_GAP_EVENT_SUBSCRIBE:
			if(event->subscribe.attr_handle == tx_value_handle_) {
				notify_enabled_ = event->subscribe.cur_notify;
				ble_notify_changed_callback(notify_enabled_);
			}
			break;

		case BLE_GAP_EVENT_MTU:
			break;
	}

	return 0;
}


static void ble_sync_callback() {
	/* Begin advertising */
	ble_advertise();
}


static int ble_advertise() {
	struct ble_hs_adv_fields fields = {
		.flags                 = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,    /* General discoverable, BLE-only (BR/EDR unsupported) */
		.tx_pwr_lvl_is_present = 1,                                                   /* Include tx power level */
		.tx_pwr_lvl            = BLE_HS_ADV_TX_PWR_LVL_AUTO,                          /* Let the stack fill it in automatically */
		.name                  = (uint8_t *) DEVICE_NAME,
		.name_len              = strlen(DEVICE_NAME),
		.name_is_complete      = 1
	};
	if(ble_gap_adv_set_fields(&fields)) {
		return -1;
	}

	/* Begin advertising */
	struct ble_gap_adv_params adv_params = {
		.conn_mode = BLE_GAP_CONN_MODE_UND,
		.disc_mode = BLE_GAP_DISC_MODE_GEN
	};
	if(ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event_callback, NULL)) {
		return -2;
	}

	return 0;
}


static void ble_host_task(void* param) {
	/* This function will return only when nimble_port_stop() is executed */
	nimble_port_run();

	nimble_port_freertos_deinit();
}


/* Setup Bluetooth Low Energy stack and service */
static int ble_spp_init() {
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
	if(esp_nimble_hci_and_controller_init() != ESP_OK) {
		return -3;
	}

	nimble_port_init();

	/* Initialize the NimBLE host configuration */
	ble_hs_cfg.sync_cb  = ble_sync_callback;

	ble_svc_gap_init();
	ble_svc_gatt_init();

	if(ble_gatts_count_cfg(gatt_services_defintion_)) {
		return -4;
	}
	if(ble_gatts_add_svcs(gatt_services_defintion_)) {
		return -5;
	}

	if(ble_svc_gap_device_name_set(DEVICE_NAME)) {
		return -6;
	}

	/* Start the task */
	nimble_port_freertos_init(ble_host_task);

	return 0;
}


/* ================================================================================ Public functions */
int ble_init() {
	if(ble_spp_init()) {
		return -1;
	}

	message_queue_ = xQueueCreate(3, TX_BUFFER_SIZE);
	if(!message_queue_) {
		return -2;
	}
	if(xTaskCreate(ble_send_task, "ble_send_task", 2048, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
		return -3;
	}

	return 0;
}


/* Send a message to the remote device */
int ble_send(ble_content_type_t type, const uint8_t* payload, uint8_t payload_size) {
	if(payload_size > (0xFF - 2)) {
		return -1;
	}

	uint8_t message[TX_BUFFER_SIZE];
	message[0] = type;
	message[1] = payload_size + 2;
	memcpy(&message[2], payload, payload_size);

	if(xQueueSendFromISR(message_queue_, &message, NULL) != pdTRUE) {
		return -2;
	}

	return 0;
}
