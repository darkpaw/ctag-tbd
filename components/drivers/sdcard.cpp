/***************
CTAG TBD >>to be determined<< is an open source eurorack synthesizer module.

A project conceived within the Creative Technologies Arbeitsgruppe of
Kiel University of Applied Sciences: https://www.creative-technologies.de

(c) 2020 by Robert Manzke. All rights reserved.

The CTAG TBD software is licensed under the GNU General Public License
(GPL 3.0), available here: https://www.gnu.org/licenses/gpl-3.0.txt

The CTAG TBD hardware design is released under the Creative Commons
Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0).
Details here: https://creativecommons.org/licenses/by-nc-sa/4.0/

CTAG TBD is provided "as is" without any express or implied warranties.

License and copyright details for specific submodules are included in their
respective component folders / files if different from this license.
***************/

#include "sdcard.hpp"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"

#define TAG "SD"

using namespace CTAG::DRIVERS;

static sdmmc_card_t *card = nullptr;
static bool isMounted = false;
static char sampleRomPath[300] = {0};
static uint8_t *bounceBuf = nullptr;
static FILE *writeFile = nullptr;

// SDMMC on ESP32 does DMA, and DMA can not reach PSRAM. Everything that ends up in
// PSRAM (the sample ROM image) is therefore staged through this internal RAM buffer.
static uint8_t *GetBounceBuf() {
    if (bounceBuf == nullptr) {
        bounceBuf = (uint8_t *) heap_caps_malloc(CONFIG_TBD_SD_BOUNCE_BUF_SIZE,
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (bounceBuf == nullptr)
            ESP_LOGE(TAG, "Cannot get %i bytes of internal DMA memory for the SD bounce buffer!",
                     CONFIG_TBD_SD_BOUNCE_BUF_SIZE);
    }
    return bounceBuf;
}

bool SDCard::InitSDCard() {
    if (isMounted) return true;

#if CONFIG_TBD_SD_POWER_PIN >= 0
    // power cycle the card so that it always starts from a known state
    gpio_config_t io_conf;
    memset(&io_conf, 0, sizeof(io_conf));
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << CONFIG_TBD_SD_POWER_PIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level((gpio_num_t) CONFIG_TBD_SD_POWER_PIN, 0);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    gpio_set_level((gpio_num_t) CONFIG_TBD_SD_POWER_PIN, 1);
    vTaskDelay(100 / portTICK_PERIOD_MS);
#endif

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = CONFIG_TBD_SD_FREQ_KHZ;
    // 1 bit only. In 4 bit mode D2/D3 would be GPIO9/GPIO10, which on a WROVER module
    // are the flash SPI HOLD/WP pins.
#ifdef SDMMC_HOST_FLAG_1BIT
    // older style switch, kept for compatibility, width below is the modern one
    host.flags |= SDMMC_HOST_FLAG_1BIT;
#endif
#ifdef SDMMC_HOST_FLAG_BUSWIDTH_4
    host.flags &= ~SDMMC_HOST_FLAG_BUSWIDTH_4;
#endif
#ifdef SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF
    // let the driver build its own aligned buffer where a caller buffer cannot be
    // used for DMA directly
    host.flags |= SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = (gpio_num_t) CONFIG_TBD_SD_CLK_PIN;
    slot_config.cmd = (gpio_num_t) CONFIG_TBD_SD_CMD_PIN;
    slot_config.d0 = (gpio_num_t) CONFIG_TBD_SD_D0_PIN;
    slot_config.d1 = GPIO_NUM_NC;
    slot_config.d2 = GPIO_NUM_NC;
    slot_config.d3 = GPIO_NUM_NC;
    slot_config.cd = GPIO_NUM_NC;
    slot_config.wp = GPIO_NUM_NC;
#if defined(SDMMC_SLOT_FLAG_INTERNAL_PULLUP)
#if CONFIG_TBD_SD_INTERNAL_PULLUP
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#endif
#endif

    esp_vfs_fat_sdmmc_mount_config_t mount_config;
    memset(&mount_config, 0, sizeof(mount_config));
    // deliberately never format: formatting would silently destroy the sample ROM
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = CONFIG_TBD_SD_MAX_FILES;
    mount_config.allocation_unit_size = 8192;

    ESP_LOGI(TAG, "Mounting SD card on %s (clk=%i cmd=%i d0=%i, %i kHz, 1 bit)",
             CONFIG_TBD_SD_MOUNT_POINT, CONFIG_TBD_SD_CLK_PIN, CONFIG_TBD_SD_CMD_PIN,
             CONFIG_TBD_SD_D0_PIN, CONFIG_TBD_SD_FREQ_KHZ);

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(CONFIG_TBD_SD_MOUNT_POINT, &host, &slot_config,
                                            &mount_config, &card);
    if (ret != ESP_OK) {
        card = nullptr;
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Card present but the file system did not mount (not FAT? corrupted?)");
        } else {
            ESP_LOGE(TAG, "SD card init failed: %s", esp_err_to_name(ret));
            ESP_LOGE(TAG, "Check the card is in the socket and that clk/cmd/d0 are the ESP32 "
                          "SDMMC IOMUX pins (14/15/2). Other pins need "
                          "CONFIG_SDMMC_USE_GPIO_MATRIX=y, which limits the clock to 20MHz");
        }
        return false;
    }

    isMounted = true;
    sdmmc_card_print_info(stdout, card);
    snprintf(sampleRomPath, sizeof(sampleRomPath), "%s/%s",
             CONFIG_TBD_SD_MOUNT_POINT, CONFIG_TBD_SD_SAMPLE_ROM_FILE);
    return true;
}

bool SDCard::IsMounted() {
    return isMounted;
}

const char *SDCard::SampleRomPath() {
    if (sampleRomPath[0] == 0)
        snprintf(sampleRomPath, sizeof(sampleRomPath), "%s/%s",
                 CONFIG_TBD_SD_MOUNT_POINT, CONFIG_TBD_SD_SAMPLE_ROM_FILE);
    return sampleRomPath;
}

size_t SDCard::SampleRomSize() {
    if (!isMounted) return 0;
    struct stat st;
    if (stat(SampleRomPath(), &st) != 0) return 0;
    return (size_t) st.st_size;
}

size_t SDCard::ReadSampleRom(void *dst, size_t byte_offset, size_t n_bytes) {
    if (!isMounted || dst == nullptr || n_bytes == 0) return 0;
    uint8_t *buf = GetBounceBuf();
    if (buf == nullptr) return 0;
    FILE *f = fopen(SampleRomPath(), "rb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "Cannot open %s", SampleRomPath());
        return 0;
    }
    if (fseek(f, (long) byte_offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Cannot seek to %u in %s", (unsigned) byte_offset, SampleRomPath());
        fclose(f);
        return 0;
    }
    size_t done = 0;
    while (done < n_bytes) {
        size_t want = n_bytes - done;
        if (want > (size_t) CONFIG_TBD_SD_BOUNCE_BUF_SIZE) want = (size_t) CONFIG_TBD_SD_BOUNCE_BUF_SIZE;
        size_t got = fread(buf, 1, want, f);
        if (got == 0) break; // end of file or read error
        memcpy((uint8_t *) dst + done, buf, got);
        done += got;
    }
    fclose(f);
    if (done != n_bytes)
        ESP_LOGE(TAG, "Read %u of %u bytes from %s", (unsigned) done, (unsigned) n_bytes,
                 SampleRomPath());
    return done;
}

bool SDCard::CreateSampleRom() {
    if (!isMounted) return false;
    if (writeFile != nullptr) {
        fclose(writeFile);
        writeFile = nullptr;
    }
    writeFile = fopen(SampleRomPath(), "wb"); // truncates an existing file
    if (writeFile == nullptr) {
        ESP_LOGE(TAG, "Cannot create %s (card full?)", SampleRomPath());
        return false;
    }
    return true;
}

size_t SDCard::WriteSampleRom(const void *src, size_t n_bytes) {
    if (writeFile == nullptr || src == nullptr || n_bytes == 0) return 0;
    size_t written = fwrite(src, 1, n_bytes, writeFile);
    if (written != n_bytes) ESP_LOGE(TAG, "SD card write failed / card full!");
    return written;
}

bool SDCard::CloseSampleRom() {
    if (writeFile == nullptr) return false;
    fflush(writeFile);
    int res = fclose(writeFile);
    writeFile = nullptr;
    return res == 0;
}

size_t SDCard::FreeBytes() {
    if (!isMounted) return 0;
    // newlib has no statvfs() on ESP-IDF, ask the FATFS/VFS glue layer directly
    uint64_t total = 0;
    uint64_t freeBytes = 0;
    if (esp_vfs_fat_info(CONFIG_TBD_SD_MOUNT_POINT, &total, &freeBytes) != ESP_OK) return 0;
    return (size_t) freeBytes;
}
