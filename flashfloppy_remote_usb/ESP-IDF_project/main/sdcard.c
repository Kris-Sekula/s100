#include "sdcard.h"
#include "board.h"
#include "state.h"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h" /* vTaskDelay for the USB detach settle */
#include "esp_log.h"
#include "esp_err.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h" /* SDMMC_HOST_DEFAULT, sdmmc_slot_config_t,
                                  SDMMC_SLOT_CONFIG_DEFAULT, pullup flag.
                                  In IDF 5.4+ this lives in the separate
                                  esp_driver_sdmmc component. */
#include "tinyusb.h"
#if __has_include("tinyusb_default_config.h")
#include "tinyusb_default_config.h"
#endif
#include "tusb_msc_storage.h"

static const char *TAG = "ff_sdcard";

static sdmmc_card_t *s_card;
static bool s_local; /* card currently mounted locally (USB sees no media) */

/* The storage component reports every mount switch here; keep the
 * shared 'presented' flag truthful from it rather than guessing. */
static void storage_mount_changed_cb(tinyusb_msc_event_t *event)
{
    if (event->type == TINYUSB_MSC_EVENT_MOUNT_CHANGED) {
        g_ff.presented = !event->mount_changed_data.is_mounted;
        ESP_LOGI(TAG, "card %s", event->mount_changed_data.is_mounted
                                     ? "mounted locally (USB: no media)"
                                     : "handed to USB");
    }
}

esp_err_t ff_sdcard_init(void)
{
    /* Initialise the SDMMC card ourselves and hand the card handle to
     * the esp_tinyusb MSC storage component (pinned < 2.0, whose
     * storage API this is). The component owns the tud_msc callbacks
     * from here on — v15 deliberately presents the WHOLE card, the
     * thing the v01 header comment warned against in the container
     * design; the storage-ownership problem it named is now handled
     * by the component's mount switch (temporal ownership) instead
     * of by isolating a container file. */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4; /* 4-bit SDMMC, per the schematic wiring */
    slot_config.clk = BOARD_SD_CLK_GPIO;
    slot_config.cmd = BOARD_SD_CMD_GPIO;
    slot_config.d0 = BOARD_SD_D0_GPIO;
    slot_config.d1 = BOARD_SD_D1_GPIO;
    slot_config.d2 = BOARD_SD_D2_GPIO;
    slot_config.d3 = BOARD_SD_D3_GPIO;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc host init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc slot init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_card = malloc(sizeof(sdmmc_card_t));
    if (!s_card) return ESP_ERR_NO_MEM;
    err = sdmmc_card_init(&host, s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD card init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SD card %s, %llu MB", s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) /
                 (1024 * 1024));

    const tinyusb_msc_sdmmc_config_t storage_cfg = {
        .card = s_card,
        .callback_mount_changed = storage_mount_changed_cb,
        .callback_premount_changed = NULL,
        .mount_config = {
            .format_if_mount_failed = false, /* never format his card */
            .max_files = 8,
            .allocation_unit_size = 16 * 1024,
        },
    };
    err = tinyusb_msc_storage_init_sdmmc(&storage_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MSC storage init failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t ff_usb_install(void)
{
    /* Same driver install as the container design's ff_msc_init
     * (proven v02+): esp_tinyusb supplies the descriptors, and the
     * storage registered above becomes the MSC medium. Installing
     * switches the single internal USB PHY from USB-Serial-JTAG to
     * USB-OTG; logs live on the header UART from here on. */
    esp_err_t err;
#ifdef TINYUSB_DEFAULT_CONFIG
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    err = tinyusb_driver_install(&cfg);
#else
    const tinyusb_config_t cfg = {
        .device_descriptor = NULL,
        .string_descriptor = NULL,
        .external_phy = false,
        .configuration_descriptor = NULL,
    };
    err = tinyusb_driver_install(&cfg);
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB install failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t ff_sdcard_local_begin(void)
{
    if (s_local) return ESP_OK;
    /* Full USB detach first (v21). The storage component's own mount
     * switch does not present a convincing removal to the host:
     * Kris's v20 swap log showed the PC still issuing reads
     * mid-window (sdmmc timeouts logged by the component), and
     * afterwards Windows kept showing its cached IMAGE_A.CFG —
     * "first line never changes" — even though the card provably
     * held the new name, because as far as the host knew the media
     * never left. tud_disconnect() is the electronic unplug: the
     * host stops and drops its caches, and the tud_connect() in
     * local_end is then a fresh insertion that gets re-read from
     * scratch. This is what the hand-rolled MSC code did back when
     * PC swaps were confirmed working, and a physical re-insert is
     * what FlashFloppy itself is built to handle. */
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(250));
    /* In the resolved esp_tinyusb (< 2.0), mount takes the local
     * mount point explicitly (Kris's v15 build error quoted the
     * header declaration); unmount takes no argument. */
    esp_err_t err = tinyusb_msc_storage_mount(FF_SD_MOUNT_POINT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "local mount failed: %s", esp_err_to_name(err));
        tud_connect(); /* do not strand the card invisible */
        return err;
    }
    s_local = true;
    return ESP_OK;
}

esp_err_t ff_sdcard_local_end(void)
{
    if (!s_local) return ESP_OK;
    esp_err_t err = tinyusb_msc_storage_unmount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "local unmount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_local = false;
    tud_connect(); /* fresh insertion: the host re-reads the card */
    return ESP_OK;
}

bool ff_sdcard_local_active(void)
{
    return s_local;
}

sdmmc_card_t *ff_sdcard_handle(void)
{
    return s_card;
}
