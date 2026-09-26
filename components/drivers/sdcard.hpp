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

#pragma once

#include <cstddef>
#include <cstdint>

namespace CTAG {
    namespace DRIVERS {
        // 1 bit SDMMC SD card, FAT file system, mounted on CONFIG_TBD_SD_MOUNT_POINT.
        //
        // This is the bulk store for the sample ROM on hardware whose system flash is
        // too small to keep a raw sample ROM region in it (Strampler / 4 MiB WROVER).
        // Sample data is meant to be transferred from here into PSRAM in bulk, see
        // CTAG::SP::HELPERS::ctagSampleRom. Nothing on the audio path may call into
        // this class: FAT + SD access has millisecond scale worst case latency.
        class SDCard {
        public:
            // Mount the card. Called once from app_main() before the sound processor
            // is started. Returns false if no card is present or it does not mount -
            // the module stays usable, it just has no sample data.
            // The Strampler socket has no card detect line wired (SD.sch pulls CD up
            // locally only) and no power switch, so a card has to be inserted before
            // the module is reset; hot plugging is not noticed.
            static bool InitSDCard();

            static bool IsMounted();

            // "/sdcard/sample-rom.tbd" i.e. mount point + file name, as one path.
            static const char *SampleRomPath();

            // Size of the sample ROM file in bytes, 0 if the card is not mounted or
            // the file does not exist.
            static size_t SampleRomSize();

            // Copy n_bytes out of the sample ROM file starting at byte_offset.
            // dst may be in PSRAM: SDMMC on ESP32 cannot DMA into PSRAM, so the data
            // is pulled through an internal RAM bounce buffer.
            // Only to be used outside of the audio task (boot, sample ROM upload).
            // Returns the number of bytes actually read.
            static size_t ReadSampleRom(void *dst, size_t byte_offset, size_t n_bytes);

            // Streaming write side, used by the sample ROM upload handlers.
            // CreateSampleRom() truncates an existing file.
            static bool CreateSampleRom();
            static size_t WriteSampleRom(const void *src, size_t n_bytes);
            static bool CloseSampleRom();

            // Bytes still free on the card, 0 if not mounted.
            static size_t FreeBytes();
        };
    }
}
