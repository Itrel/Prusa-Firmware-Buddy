#include "usb_host.h"
#include "usbh_core.h"
#include "usbh_msc.h"

#include "fatfs.h"
#include "media.h"
#include <device/board.h>
#include "common/timing.h"

#include <atomic>

USBH_HandleTypeDef hUsbHostHS;
ApplicationTypeDef Appli_state = APPLICATION_IDLE;

static uint32_t one_click_print_timeout { 0 };
static std::atomic<bool> connected_at_startup { false };

static void USBH_UserProcess(USBH_HandleTypeDef *phost, uint8_t id);

void MX_USB_HOST_Init(void) {
#if (BOARD_IS_XBUDDY || BOARD_IS_XLBUDDY)
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_8, GPIO_PIN_SET);
    HAL_Delay(200);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_8, GPIO_PIN_RESET);
#else
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_5, GPIO_PIN_SET);
    HAL_Delay(200);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_5, GPIO_PIN_RESET);
#endif
    // A delay of 3000ms for detecting USB device (flash drive) was present at start
    one_click_print_timeout = ticks_ms() + 3000;

    if (USBH_Init(&hUsbHostHS, USBH_UserProcess, HOST_HS) != USBH_OK) {
        Error_Handler();
    }
    if (USBH_RegisterClass(&hUsbHostHS, USBH_MSC_CLASS) != USBH_OK) {
        Error_Handler();
    }
    if (USBH_Start(&hUsbHostHS) != USBH_OK) {
        Error_Handler();
    }
}

static void USBH_UserProcess([[maybe_unused]] USBH_HandleTypeDef *phost, uint8_t id) {
    // don't detect device at startup when ticks_ms() overflows (every ~50 hours)
    if (one_click_print_timeout > 0 && ticks_ms() >= one_click_print_timeout) {
        one_click_print_timeout = 0;
    }

    switch (id) {
    case HOST_USER_SELECT_CONFIGURATION:
        break;

    case HOST_USER_DISCONNECTION:
        Appli_state = APPLICATION_DISCONNECT;
        media_set_removed();
        f_mount(0, (TCHAR const *)USBHPath, 1); // umount
        connected_at_startup = false;
        break;

    case HOST_USER_CLASS_ACTIVE: {
        Appli_state = APPLICATION_READY;
        FRESULT result = f_mount(&USBHFatFS, (TCHAR const *)USBHPath, 0);
        if (result == FR_OK) {
            if (one_click_print_timeout > 0 && ticks_ms() < one_click_print_timeout) {
                connected_at_startup = true;
            }
            media_set_inserted();
        } else
            media_set_error(media_error_MOUNT);
        break;
    }
    case HOST_USER_CONNECTION:
        Appli_state = APPLICATION_START;
        break;

    default:
        break;
    }
}

bool device_connected_at_startup() {
    return connected_at_startup;
}
