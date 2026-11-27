#include "epd_board_specific.h"
#include "boards/common/display/epdiy/epdiy.h"

void epd_powerdown_lilygo_t5_47(void) {
    epd_poweroff();
}

void epd_powerdown(void) {
    epd_poweroff();
}
