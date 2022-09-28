#include "led.h"


/* ================================================================================ Private includes */
#include "driver/gpio.h"


/* ================================================================================ Private defines */
#define LED_GPIO_PIN    2


/* ================================================================================ Private types */


/* ================================================================================ Private variables */


/* ================================================================================ Private functions */


/* ================================================================================ Public functions */
int led_init() {
	gpio_config_t io_conf = {
		.intr_type    = GPIO_PIN_INTR_DISABLE,
		.mode         = GPIO_MODE_OUTPUT,
		.pin_bit_mask = 1ULL << LED_GPIO_PIN,
		.pull_down_en = 0,
		.pull_up_en   = 0
	};
	if(gpio_config(&io_conf) != ESP_OK) {
		return -1;
	}
	if(led_off()) {
		return -2;
	}

	return 0;
}


int led_on() {
	if(gpio_set_level(LED_GPIO_PIN, 1) != ESP_OK) {
		return -1;
	}

	return 0;
}


int led_off() {
	if(gpio_set_level(LED_GPIO_PIN, 0) != ESP_OK) {
		return -1;
	}

	return 0;
}
