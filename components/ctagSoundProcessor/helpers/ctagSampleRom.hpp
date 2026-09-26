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
#include <cstdint>
#include <vector>
#include <atomic>

using namespace std;

namespace CTAG::SP::HELPERS{
    class ctagSampleRom {
    public:
        static void RefreshDataStructure(); // forces refresh of data structure, not thread safe!
        ctagSampleRom();
        ~ctagSampleRom();
        uint32_t GetNumberSlices();
        uint32_t GetFirstNonWaveTableSlice();
        // return slice size in int16 samples i.e. *2 in bytes
        uint32_t GetSliceSize(const uint32_t slice);
        uint32_t GetSliceGroupSize(const uint32_t startSlice, const uint32_t endSlice);
        uint32_t GetSliceOffset(const uint32_t slice);
        bool HasSlice(const uint32_t slice);
        bool HasSliceGroup(const uint32_t startSlice, const uint32_t endSlice);
        void Read(int16_t *dst, uint32_t offset, const uint32_t n_samples);
        void ReadSlice(int16_t *dst, const uint32_t slice, const uint32_t offset, const uint32_t n_samples);
        void ReadSliceAsFloat(float *dst, const uint32_t slice, const uint32_t offset, const uint32_t n_samples);
        void BufferInSPIRAM();
        bool IsBufferedInSPIRAM();
        // Bytes of 16bit mono sample data the medium holds (SD file / raw flash region).
        static uint32_t GetAvailableBytes();
        // Bytes of sample data that can be resident at one time, i.e. the size limit a
        // sample ROM has to respect. With CONFIG_TBD_SD_ENABLE this is what fits into PSRAM,
        // not the capacity of the SD card.
        static uint32_t GetPlayableBytes();
        // Bytes of sample data currently resident and therefore audible.
        static uint32_t GetLoadedBytes();
    private:
        // SD backend only: reads header and slice table from the ROM file and preloads as
        // much sample data into PSRAM as fits. Not declared anywhere else, so builds without
        // CONFIG_TBD_SD_ENABLE simply never reference it.
        static void RefreshFromSDCard();
        static uint32_t totalSize;
        static uint32_t numberSlices;
        static uint32_t headerSize;
        static uint32_t *sliceSizes;
        static uint32_t *sliceOffsets;
        static uint32_t firstNonWtSlice;
        static atomic<uint32_t>  nConsumers;
        static int16_t *ptrSPIRAM;
        // number of int16 samples of the ROM that are resident in ptrSPIRAM. Read() only
        // ever touches samples below this number, which is what keeps an audio task out of a
        // buffer that is being (re)loaded.
        static uint32_t preloadedSamples;
        // bytes of sample data described by the ROM header (excludes header and slice table)
        static uint32_t sampleBytes;
        static uint32_t nSlicesBuffered;
    };
}
