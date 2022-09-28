#ifndef BT_H
#define BT_H


/* ================================================================================ Public includes */
#include <stdint.h>


/* ================================================================================ Public defines */


/* ================================================================================ Public types */
typedef enum {
	BT_CONTENT_DATA = 0x01,
	BT_CONTENT_TEXT = 0x02
} bt_content_type_t;


/* ================================================================================ Public functions */
extern int  bt_init();
extern int  bt_send(bt_content_type_t type, const uint8_t* payload, uint8_t payload_size);
extern void bt_connected_callback();
extern void bt_disconnected_callback();
extern void bt_data_received_callback(const uint8_t* data, uint16_t data_size);


#endif /* BT_H */
