#ifndef KLINE_H
#define KLINE_H


/* ================================================================================ Public includes */
#include <stdint.h>


/* ================================================================================ Public defines */


/* ================================================================================ Public types */
typedef enum {
	KLINE_INIT,
	KLINE_START_SESSION,
	KLINE_ACTIVE
} kline_state_t;


/* ================================================================================ Public functions */
extern int  kline_init();
extern int  kline_start();
extern int  kline_stop();
extern void kline_data_received_callback(const uint8_t* data, uint8_t data_size);
extern void kline_state_changed_callback(kline_state_t state);


#endif /* KLINE_H */
