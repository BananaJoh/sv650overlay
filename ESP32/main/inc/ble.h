#ifndef BLE_H
#define BLE_H


/* ================================================================================ Public includes */
#include <stdbool.h>
#include <stdint.h>


/* ================================================================================ Public defines */


/* ================================================================================ Public types */
typedef enum {
	BLE_CONTENT_DATA = 0x01,
	BLE_CONTENT_TEXT = 0x02
} ble_content_type_t;


/* ================================================================================ Public functions */
extern int  ble_init();
extern int  ble_send(ble_content_type_t type, const uint8_t* payload, uint8_t payload_size);
extern void ble_connected_callback();
extern void ble_disconnected_callback();
extern void ble_notify_changed_callback(bool enabled);
extern void ble_data_received_callback(const uint8_t* data, uint16_t data_size);


#endif /* BLE_H */
