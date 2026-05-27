#ifndef __IR_FILTER_CONTROLLER_H__
#define __IR_FILTER_CONTROLLER_H__

#include <driver/gpio.h>


class IrFilterController {
private:
    bool enable_ = false;
    gpio_num_t gpio_num_;

public:
    IrFilterController(gpio_num_t gpio_num) : gpio_num_(gpio_num) {
        gpio_config_t config = {
            .pin_bit_mask = (1ULL << gpio_num_),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&config));
        gpio_set_level(gpio_num_, 0);

    }
};


#endif // __IR_FILTER_CONTROLLER_H__
