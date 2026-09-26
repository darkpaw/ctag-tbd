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

#include "ctagSampleRom.hpp"
//#include "esp_spi_flash.h"
#include <esp_flash.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <cassert>
#include <cinttypes>
#include <cstring>

#ifdef TBD_SIM
#define CONFIG_SAMPLE_ROM_START_ADDRESS 0
// The simulator backs the ROM with the sample-rom.tbd file loaded into a RAM buffer, see
// simulator/fake-idf/esp_spi_flash.c, and has no sdkconfig.h. This is only used to answer
// "how big may a sample ROM be".
#ifndef CONFIG_SAMPLE_ROM_SIZE
#define CONFIG_SAMPLE_ROM_SIZE 0x500000
#endif
#else
#include "sdkconfig.h"
#endif

// Backends:
//  - CONFIG_TBD_SD_ENABLE (4 MiB boards such as the Strampler builds): the ROM is a file on
//    the SD card. The header and the slice table are read from the card, the sample data is
//    preloaded into PSRAM and playback only ever reads PSRAM.
//  - everything else: the ROM is a raw blob at CONFIG_SAMPLE_ROM_START_ADDRESS.
// In both cases Read()/ReadSlice()/ReadSliceAsFloat() are called from the audio task and
// must never block, so the SD card is only ever touched by RefreshDataStructure().
#ifdef CONFIG_TBD_SD_ENABLE
#include "sdcard.hpp"
using CTAG::DRIVERS::SDCard;
#endif

namespace CTAG::SP::HELPERS {
    atomic<uint32_t> ctagSampleRom::nConsumers = 0;
    uint32_t ctagSampleRom::totalSize = 0;
    uint32_t ctagSampleRom::numberSlices = 0;
    uint32_t ctagSampleRom::headerSize = 0;
    uint32_t *ctagSampleRom::sliceSizes = nullptr;
    uint32_t *ctagSampleRom::sliceOffsets = nullptr;
    uint32_t ctagSampleRom::firstNonWtSlice = 0;
    int16_t *ctagSampleRom::ptrSPIRAM = nullptr;
    uint32_t ctagSampleRom::nSlicesBuffered = 0;
    uint32_t ctagSampleRom::preloadedSamples = 0;
    uint32_t ctagSampleRom::sampleBytes = 0;

    ctagSampleRom::ctagSampleRom() {
        //ESP_LOGE("SR", "nConsumers %li", nConsumers.load());
        nConsumers++;
        if(nConsumers == 1)
            RefreshDataStructure();
    }

    uint32_t ctagSampleRom::GetNumberSlices() {
        return numberSlices;
    }

    uint32_t ctagSampleRom::GetSliceSize(const uint32_t slice) {
        if (slice >= numberSlices) return 0;
        return sliceSizes[slice];
    }

    uint32_t ctagSampleRom::GetSliceGroupSize(const uint32_t startSlice, const uint32_t endSlice) {
        if (endSlice <= startSlice) return 0;
        uint32_t totalSize = 0;
        for (uint32_t i = startSlice; i <= endSlice; i++) {
            totalSize += sliceSizes[i];
        }
        return totalSize;
    }

    uint32_t ctagSampleRom::GetSliceOffset(const uint32_t slice) {
        if (slice >= numberSlices) return 0;
        return sliceOffsets[slice];
    }

    // reads words, offset in words not bytes
    void ctagSampleRom::Read(int16_t *dst, uint32_t offset, const uint32_t n_samples) {
        assert(dst != nullptr);
        uint32_t done = 0;
        // Sample data that is resident in PSRAM (SD preload, or BufferInSPIRAM() on the
        // platforms that still keep the ROM in flash) is always taken from there.
        if (ptrSPIRAM != nullptr && offset < preloadedSamples) {
            uint32_t n = preloadedSamples - offset;
            if (n > n_samples) n = n_samples;
            memcpy(dst, &ptrSPIRAM[offset], n * 2);
            done = n;
        }
        if (done == n_samples) return;
#ifdef CONFIG_TBD_SD_ENABLE
        // Never read the SD card here. A FAT read can block for hundreds of milliseconds on
        // a crowded card, which would glitch every audio channel of the module, not just the
        // voice that asked. Whatever is not in PSRAM is silence.
        memset(&dst[done], 0, (n_samples - done) * 2);
#else
        // the remainder comes straight out of the raw flash blob
        uint32_t byteOffset = (offset + done) * 2;
        byteOffset += headerSize; // add header size
        byteOffset += CONFIG_SAMPLE_ROM_START_ADDRESS; // add start offset
        //spi_flash_read(offset, dst, n_samples * 2);
        esp_flash_read(nullptr, &dst[done], byteOffset, (n_samples - done) * 2);
#endif
    }

    bool ctagSampleRom::HasSlice(const uint32_t slice) {
        if (slice >= numberSlices) return false;
        return true;
    }

    bool ctagSampleRom::HasSliceGroup(const uint32_t startSlice, const uint32_t endSlice) {
        if (startSlice > numberSlices || endSlice > numberSlices) return false;
        return true;
    }

    void ctagSampleRom::ReadSlice(int16_t *dst, const uint32_t slice, const uint32_t offset, const uint32_t n_samples) {
        if (slice >= numberSlices) {
            memset(dst, 0, n_samples * 2);
            return;
        }
        uint32_t start = sliceOffsets[slice] + offset;
        int32_t len = (int32_t) sliceSizes[slice] - (int32_t) offset;
        if (len < 0) len = 0;
        if ((uint32_t) len > n_samples) len = n_samples;
        if (len > 0) Read(dst, start, len);
        // Reading past the end of a slice used to leave the rest of dst untouched, i.e. the
        // voice kept playing whatever the previous caller had left in the buffer.
        if ((uint32_t) len < n_samples) memset(&dst[len], 0, (n_samples - len) * 2);
    }

    void ctagSampleRom::ReadSliceAsFloat(float *dst, const uint32_t slice, const uint32_t offset,
                                         const uint32_t n_samples) {
        if (slice >= numberSlices) {
            memset(dst, 0, n_samples * 4);
            return;
        }
        uint32_t start = sliceOffsets[slice] + offset;
        int32_t len = (int32_t) sliceSizes[slice] - (int32_t) offset;
        if (len < 0) len = 0;
        if ((uint32_t) len > n_samples) len = n_samples;
        // Small fixed size buffer instead of a VLA over the whole request, converted in
        // chunks so the stack usage of the audio task stays predictable.
        int16_t idst[64];
        const uint32_t chunkMax = sizeof(idst) / sizeof(idst[0]);
        uint32_t done = 0;
        while (done < n_samples) {
            uint32_t chunk = n_samples - done < chunkMax ? n_samples - done : chunkMax;
            uint32_t stillInSlice = (uint32_t) len > done ? (uint32_t) len - done : 0;
            uint32_t todo = stillInSlice < chunk ? stillInSlice : chunk;
            if (todo > 0) Read(idst, start + done, todo);
            if (todo < chunk) memset(&idst[todo], 0, (chunk - todo) * 2);
            for (uint32_t i = 0; i < chunk; i++)
                dst[done + i] = float(idst[i]) * 0.000030518509476f;
            done += chunk;
        }
    }

#ifdef CONFIG_TBD_SD_ENABLE
    // Table allocations prefer internal RAM (the audio task reads them on every block) and
    // fall back to PSRAM, which is what the flash backend has always used.
    static uint32_t *AllocOffsetTable(const uint32_t numberSlices) {
        size_t bytes = (size_t) numberSlices * sizeof(uint32_t);
        uint32_t *p = (uint32_t *) heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p == nullptr)
            p = (uint32_t *) heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return p;
    }

    // Rebuild the data structure and preload the sample data from the SD card file.
    // Called at startup and after an upload. The upload path holds the plugin processing
    // mutex (SoundProcessorManager::DisablePluginProcessing()), so no voice can be inside
    // ReadSlice() while the previous image is released and the new one is built.
    void ctagSampleRom::RefreshFromSDCard() {
        // take the old generation out of use first: readers see a null pointer and stay
        // silent instead of a half rebuilt table, and the memory becomes available again
        // for the new image (which is what lets a ROM use the full PSRAM).
        if (sliceOffsets != nullptr) heap_caps_free(sliceOffsets);
        if (sliceSizes != nullptr) heap_caps_free(sliceSizes);
        if (ptrSPIRAM != nullptr) heap_caps_free(ptrSPIRAM);
        sliceOffsets = nullptr;
        sliceSizes = nullptr;
        ptrSPIRAM = nullptr;
        preloadedSamples = 0;
        nSlicesBuffered = 0;
        totalSize = 0;
        numberSlices = 0;
        headerSize = 0;
        firstNonWtSlice = 0;
        sampleBytes = 0;

        if (!SDCard::IsMounted()) {
            ESP_LOGE("SROM", "Sample ROM unavailable, the SD card is not mounted");
            return;
        }
        size_t fileSize = SDCard::SampleRomSize();
        if (fileSize == 0) {
            ESP_LOGI("SROM", "No sample ROM file at %s", SDCard::SampleRomPath());
            return;
        }
        uint32_t hdr[3];
        if (SDCard::ReadSampleRom(hdr, 0, sizeof(hdr)) != sizeof(hdr)) {
            ESP_LOGE("SROM", "Cannot read the header of %s", SDCard::SampleRomPath());
            return;
        }
        if (hdr[0] != 0xdeadface) {
            ESP_LOGE("SROM", "%s is not a sample ROM (magic number wrong)", SDCard::SampleRomPath());
            return;
        }
        totalSize = hdr[1];
        numberSlices = hdr[2];
        headerSize = 12;
        ESP_LOGI("SROM", "%s: %" PRIu32 " slices, %" PRIu32 " samples",
                 SDCard::SampleRomPath(),
                 numberSlices, totalSize);
        if (numberSlices == 0 || numberSlices > 0x100000) {
            ESP_LOGE("SROM", "Sane number of slices is 0..1048576, file says %" PRIu32,
                     numberSlices);
            numberSlices = 0;
            totalSize = 0;
            return;
        }
        // the slice offset table sits between the fixed header and the sample data
        size_t dataStart = headerSize + (size_t) numberSlices * 4;
        // The ROM header and slice offsets count int16 samples, not bytes. Widen
        // before multiplying so a malformed header cannot wrap the byte count.
        const uint64_t dataBytes = (uint64_t) totalSize * sizeof(int16_t);
        if (dataBytes > UINT32_MAX || dataStart > fileSize || dataBytes > fileSize - dataStart) {
            ESP_LOGE("SROM", "%s has an invalid sample length: %" PRIu64 " bytes announced,"
                             " %u present", SDCard::SampleRomPath(), dataBytes,
                     fileSize > dataStart ? (unsigned) (fileSize - dataStart) : 0u);
            totalSize = 0;
            numberSlices = 0;
            return;
        }
        sampleBytes = (uint32_t) dataBytes;

        uint32_t *offsets = AllocOffsetTable(numberSlices);
        if (offsets == nullptr) {
            ESP_LOGE("SROM", "No memory for the slice table (%" PRIu32 " slices)", numberSlices);
            numberSlices = 0;
            return;
        }
        if (SDCard::ReadSampleRom(offsets, headerSize, 4 * numberSlices) != 4 * numberSlices) {
            ESP_LOGE("SROM", "Cannot read the slice table of %s", SDCard::SampleRomPath());
            heap_caps_free(offsets);
            numberSlices = 0;
            totalSize = 0;
            return;
        }
        uint32_t *sizes = AllocOffsetTable(numberSlices);
        if (sizes == nullptr) {
            ESP_LOGE("SROM", "No memory for the slice sizes (%" PRIu32 " slices)", numberSlices);
            heap_caps_free(offsets);
            numberSlices = 0;
            totalSize = 0;
            return;
        }
        int lastOffset = 0;
        for (uint32_t i = 0; i < numberSlices; i++) {
            sizes[i] = offsets[i] - lastOffset;
            lastOffset = offsets[i];
            offsets[i] -= sizes[i];
            ESP_LOGD("SROM", "Slice size %" PRIu32 ", offset %" PRIu32, sizes[i], offsets[i]);
        }
        // get first non Wt Slice
        for (uint32_t i = 0; i < numberSlices; i++) {
            if (sizes[i] > 256) {
                firstNonWtSlice = i;
                break;
            }
        }
        sliceOffsets = offsets;
        sliceSizes = sizes;
        headerSize = (uint32_t) dataStart;

        // How much of the ROM can be made resident? Everything that does not fit stays
        // silent, which is audible but not fatal, and is reported loudly below.
        size_t reserve = (size_t) CONFIG_TBD_SD_ROM_RESERVE_KB * 1024;
        size_t avail = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        size_t want = sampleBytes;
        size_t take = avail > reserve ? avail - reserve : 0;
        if (take > want) take = want;
        if (take > 0) {
            int16_t *data = (int16_t *) heap_caps_malloc(take, MALLOC_CAP_SPIRAM);
            if (data == nullptr) {
                ESP_LOGE("SROM", "Could not allocate %u bytes of PSRAM for the sample ROM",
                         (unsigned) take);
            } else {
                size_t got = SDCard::ReadSampleRom(data, headerSize, take);
                // publish after the pointer, readers check ptrSPIRAM first
                ptrSPIRAM = data;
                preloadedSamples = got / 2;
            }
        }
        if (preloadedSamples * 2 < sampleBytes) {
            ESP_LOGW("SROM", "Sample ROM is %" PRIu32 " bytes, only %" PRIu32 " bytes fit in PSRAM - slices "
                             "beyond that play silence. Use a smaller ROM or lower "
                             "CONFIG_TBD_SD_ROM_RESERVE_KB.",
                     sampleBytes, preloadedSamples * 2);
        } else {
            ESP_LOGI("SROM", "Sample ROM completely in PSRAM (%" PRIu32 " bytes, %" PRIu32
                             " slices)", sampleBytes, numberSlices);
        }
    }
#endif

    void ctagSampleRom::RefreshDataStructure() {
        if(nConsumers == 0) return;
#ifdef CONFIG_TBD_SD_ENABLE
        RefreshFromSDCard();
        return;
#else
        if (sliceOffsets != nullptr) {
            heap_caps_free(sliceOffsets);
            sliceOffsets = nullptr;
        }
        if (sliceSizes != nullptr) {
            heap_caps_free(sliceSizes);
            sliceSizes = nullptr;
        }
        uint32_t deadface = 0;
        totalSize = 0;
        numberSlices = 0;
        headerSize = 0;
        sampleBytes = 0;
        preloadedSamples = 0;
        //spi_flash_read(CONFIG_SAMPLE_ROM_START_ADDRESS, &deadface, 4);
        esp_flash_read(nullptr, &deadface, CONFIG_SAMPLE_ROM_START_ADDRESS, 4);
        if (deadface != 0xdeadface) {
            ESP_LOGE("SROM", "Magic number wrong!");
            return;
        }
        headerSize += 4;
        //spi_flash_read(CONFIG_SAMPLE_ROM_START_ADDRESS + 4, &totalSize, 4);
        esp_flash_read(nullptr,&totalSize, CONFIG_SAMPLE_ROM_START_ADDRESS + 4, 4);
        headerSize += 4;
        ESP_LOGD("SROM", "Total sample data size %li bytes", totalSize);
        //spi_flash_read(CONFIG_SAMPLE_ROM_START_ADDRESS + 8, &numberSlices, 4);
        esp_flash_read(nullptr, &numberSlices, CONFIG_SAMPLE_ROM_START_ADDRESS + 8, 4);
        headerSize += 4;
        ESP_LOGD("SROM", "Number slices %li", numberSlices);
        sampleBytes = totalSize;
        // alloc memory
        sliceOffsets = (uint32_t *) heap_caps_malloc(numberSlices * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        assert(sliceOffsets != nullptr);
        sliceSizes = (uint32_t *) heap_caps_malloc(numberSlices * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        assert(sliceSizes != nullptr);
        //spi_flash_read(CONFIG_SAMPLE_ROM_START_ADDRESS + 12, &sliceOffsets[0], 4 * numberSlices);
        esp_flash_read(nullptr, &sliceOffsets[0], CONFIG_SAMPLE_ROM_START_ADDRESS + 12, 4 * numberSlices);
        headerSize += 4 * numberSlices;
        int lastOffset = 0;
        for (uint32_t i = 0; i < numberSlices; i++) {
            sliceSizes[i] = sliceOffsets[i] - lastOffset;
            lastOffset = sliceOffsets[i];
            sliceOffsets[i] -= sliceSizes[i];
            ESP_LOGD("SROM", "Slice size %li, offset %li", sliceSizes[i], sliceOffsets[i]);
        }
        // get first non Wt Slice
        for (uint32_t i = 0; i < numberSlices; i++) {
            if (sliceSizes[i] > 256){
                firstNonWtSlice = i;
                break;
            }
        }
#endif
    }

    ctagSampleRom::~ctagSampleRom() {
        //ESP_LOGE("SR", "nConsumers %li", nConsumers.load());
        nConsumers--;

        if (nConsumers > 0) return;
        //ESP_LOGE("SR", "freeing up SR data structure");
        if (sliceOffsets != nullptr) {
            heap_caps_free(sliceOffsets);
        }
        if (sliceSizes != nullptr) {
            heap_caps_free(sliceSizes);
        }
        totalSize = 0;
        numberSlices = 0;
        headerSize = 0;
        sampleBytes = 0;
        preloadedSamples = 0;
        sliceSizes = nullptr;
        sliceOffsets = nullptr;
        firstNonWtSlice = 0;

        if(ptrSPIRAM != nullptr){
            heap_caps_free(ptrSPIRAM);
            ptrSPIRAM = nullptr;
            nSlicesBuffered = 0;
        }
    }

    uint32_t ctagSampleRom::GetFirstNonWaveTableSlice() {
        return firstNonWtSlice;
    }

    bool ctagSampleRom::IsBufferedInSPIRAM() {
        if(ptrSPIRAM == nullptr) return false;
        return true;
    }

    uint32_t ctagSampleRom::GetAvailableBytes() {
        // sample bytes that the medium holds
#ifdef CONFIG_TBD_SD_ENABLE
        return sampleBytes;
#else
        return CONFIG_SAMPLE_ROM_SIZE;
#endif
    }

    uint32_t ctagSampleRom::GetPlayableBytes() {
        // What the web UI should treat as the size limit of a sample ROM: not the size of
        // the storage medium, but the amount of sample data that can be resident at one
        // time. While a ROM is loaded its own memory counts, because an upload erases the
        // ROM before the new one is preloaded.
#ifdef CONFIG_TBD_SD_ENABLE
        size_t reserve = (size_t) CONFIG_TBD_SD_ROM_RESERVE_KB * 1024;
        size_t avail = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        size_t fresh = avail > reserve ? avail - reserve : 0;
        if (preloadedSamples * 2 > fresh) fresh = preloadedSamples * 2;
        return (uint32_t) fresh;
#else
        return CONFIG_SAMPLE_ROM_SIZE;
#endif
    }

    uint32_t ctagSampleRom::GetLoadedBytes() {
        return preloadedSamples * 2;
    }

    void ctagSampleRom::BufferInSPIRAM() {
#ifdef CONFIG_TBD_SD_ENABLE
        // With the SD backend the sample data is already in PSRAM: RefreshDataStructure()
        // preloads as much of the file as fits, there is nothing left to do here.
        return;
#else
        if(ptrSPIRAM != nullptr) return; // already buffered
        size_t maxSizeBytes = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        maxSizeBytes -= 128*1024; // reserve 128k for other stuff
        if(maxSizeBytes < 1024*1024) return; // not enough memory for this to make sense
        ptrSPIRAM = (int16_t *)heap_caps_malloc(maxSizeBytes, MALLOC_CAP_SPIRAM);
        if(ptrSPIRAM == nullptr) return;
        ESP_LOGI("SR", "Buffering %d bytes in SPIRAM", maxSizeBytes);
        // figure out how many slices can be buffered
        uint32_t maxSizeWords = maxSizeBytes / 2;
        nSlicesBuffered = 0;
        uint32_t totalSizeWords = 0;
        for(uint32_t i=0;i<numberSlices;i++){
            if(totalSizeWords + sliceSizes[i] > maxSizeWords) break;
            totalSizeWords += sliceSizes[i];
            nSlicesBuffered++;
        }
        ESP_LOGI("SR", "Buffering %li slices of %li, consuming %li bytes", nSlicesBuffered, numberSlices, totalSizeWords*2);
        preloadedSamples = 0; // keeps readers out of the half filled buffer, see Read()
        Read(ptrSPIRAM, 0, totalSizeWords);
        preloadedSamples = totalSizeWords; // publish the size last
#endif
    }
}
