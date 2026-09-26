if(CONFIG_TBD_SD_ENABLE)
    set(sd_src sdcard.cpp)
endif()

# sdmmc/fatfs are listed as dependencies unconditionally, not only when the SD card is
# enabled. Component dependencies are resolved before sdkconfig.h exists, so a dependency
# that is itself guarded by a CONFIG_ symbol only shows up one configuration pass late -
# the FATFS options taken from sdkconfig.defaults.strampler would silently be dropped in
# the first pass otherwise. Unused code is discarded by the linker anyway.

if(CONFIG_TBD_PLATFORM_MK2)
    idf_component_register(SRCS fs.cpp led_rgb.cpp codec.cpp mk2.cpp ssd1306.c ssd1306_i2c.c ${sd_src}
            INCLUDE_DIRS "."
            REQUIRES driver sdmmc fatfs )
elseif (CONFIG_TBD_PLATFORM_BBA)
    if(CONFIG_TBD_BBA_CODEC_ES8388)
        set(codec_src es8388.cpp)
    else()
        set(codec_src aic3254.cpp)
    endif()

    idf_component_register(SRCS fs.cpp led_rgb_bba.cpp codec_bba.cpp ${codec_src} midiuart.cpp tusbmidi.cpp ssd1306.c ssd1306_i2c.c ${sd_src}
            INCLUDE_DIRS "."
            REQUIRES driver esp-dsp esp-tinyusb sdmmc fatfs)
else()
    idf_component_register(SRCS gpio.cpp adc.cpp fs.cpp led_rgb.cpp codec.cpp ssd1306.c ssd1306_i2c.c ${sd_src}
            INCLUDE_DIRS "."
            REQUIRES ulp driver sdmmc fatfs )

    set(ulp_app_name ulp_${COMPONENT_NAME})

    if(CONFIG_TBD_PLATFORM_STR)
        set(ulp_s_sources ulp/mcp3208.S)
    else()
        set(ulp_s_sources ulp/adc.S)
    endif()

    set(ulp_exp_dep_srcs "adc.cpp")

    ulp_embed_binary(${ulp_app_name} ${ulp_s_sources} ${ulp_exp_dep_srcs})
endif()
