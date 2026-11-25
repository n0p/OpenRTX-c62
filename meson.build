##
## OpenRTX - Modular Open Source Radio Firmware
##
project('OpenRTX', ['c', 'cpp'],
  version : '0.4.5',
  default_options : ['warning_level=3', 'b_staticpic=false'])

##
## Optional defines, common to all souces (e.g. to enable debugging)
##

openrtx_def = {}

##
## UI font configuration, only one uncommented at a time
##

openrtx_def += {'FONT_UBUNTU_REGULAR': ''}
# openrtx_def += {'FONT_FREE_SANS': ''}

##
## Firmware configuration parameters
##

#openrtx_def += {}


##
## ----------------- Platform-independent source files -------------------------
##

##
## OpenRTX core sources
##

openrtx_src = ['openrtx/src/core/state.c',
               'openrtx/src/core/threads.c',
               'openrtx/src/core/battery.c',
               'openrtx/src/core/graphics.c',
               'openrtx/src/core/input.c',
               'openrtx/src/core/utils.c',
               'openrtx/src/core/queue.c',
               'openrtx/src/core/chan.c',
               'openrtx/src/core/gps.c',
               'openrtx/src/core/dsp.cpp',
               'openrtx/src/core/cps.c',
               'openrtx/src/core/crc.c',
               'openrtx/src/core/datetime.c',
               'openrtx/src/core/openrtx.c',
               # Post-mortem recorder: breadcrumbs, liveness beats, the crash
               # record. Portable C, built into the host tests as well as the
               # firmware. See docs/BLACKBOX.md.
               'openrtx/src/core/blackbox.c',
               'openrtx/src/core/audio_codec.c',
               'openrtx/src/core/audio_stream.c',
               'openrtx/src/core/audio_path.cpp',
               'openrtx/src/core/data_conversion.c',
               'openrtx/src/core/memory_profiling.cpp',
               'openrtx/src/core/voicePrompts.c',
               'openrtx/src/core/voicePromptUtils.c',
               'openrtx/src/core/voicePromptData.S',
               'openrtx/src/core/nvmem_access.c',
               'openrtx/src/core/channel_store.c',
               'openrtx/src/core/scanner.c',
               'openrtx/src/core/scan_service.c',
               'openrtx/src/core/vfo_search.c',
               'openrtx/src/core/vfo_search_service.c',
               'openrtx/src/core/spectrum.c',
               'openrtx/src/core/spectrum_service.c',
               'openrtx/src/rtx/ModeReport.c',
               'openrtx/src/core/settings.c',
               'openrtx/src/core/settings_record.c',
               # The accessory pad's PTT rule and the table of the three port
               # modes (docs/ACCESSORY.md). Portable C, no radio in it.
               'openrtx/src/core/port_ptt.c',
               'openrtx/src/core/zones.c',
               'openrtx/src/core/agc.c',
               'openrtx/src/core/am_tx.c',
               'openrtx/src/core/airband.c',
               'openrtx/src/core/wfm.c',
               # NXDN status and FEC: the UI reads the receive status (RAN,
               # sync, the SACCH message) and the host tests need exactly that
               # module linked. They are portable C and do not pull the
               # demodulator or the vocoder in, which is what keeps the UI
               # testable without the radio.
               'openrtx/src/protocols/NXDN/NxdnStatus.c',
               'openrtx/src/protocols/NXDN/NxdnFec.c',
               # The P25 and D-STAR status blocks, for the same reason: the mode
               # screens read them and they are portable C. See docs/UI.md.
               'openrtx/src/protocols/P25/P25Status.c',
               'openrtx/src/protocols/DSTAR/DStarStatus.c',
               'openrtx/src/protocols/DSTAR/DStarSlowData.c',
               'openrtx/src/core/slot_store.c',
               'openrtx/src/core/contact_store.c',
               'openrtx/src/core/msg_service.c',
               'openrtx/src/core/messaging.c',
               'openrtx/src/core/messaging_store_none.c',
               # Binary protocol between the radio and a computer: framing,
               # session, the store handlers and the debug namespace. Portable
               # C, built into the host tests as well as the firmware. See
               # docs/PC_PROTO.md.
               'openrtx/src/core/pc_proto.c',
               'openrtx/src/core/pc_proto_ops.c',
               'openrtx/src/core/pc_proto_dbg.c',
               # The bench namespace: scripted measurements against an
               # external receiver. See docs/BENCH.md.
               'openrtx/src/core/pc_proto_bench.c',
               'openrtx/src/rtx/rtx.cpp',
               'openrtx/src/rtx/ModeArena.cpp',
               'openrtx/src/rtx/TxGuard.c',
               'openrtx/src/rtx/OpMode_FM.cpp',
               'openrtx/src/rtx/OpMode_Airband.cpp',
               'openrtx/src/rtx/OpMode_WFM.cpp',
               'openrtx/src/rtx/OpMode_M17.cpp',
               'openrtx/src/protocols/M17/DSP.cpp',
               'openrtx/src/protocols/M17/Golay.cpp',
               'openrtx/src/protocols/M17/MetaText.cpp',
               'openrtx/src/protocols/M17/Callsign.cpp',
               'openrtx/src/protocols/M17/Modulator.cpp',
               'openrtx/src/protocols/M17/Demodulator.cpp',
               'openrtx/src/protocols/M17/FrameEncoder.cpp',
               'openrtx/src/protocols/M17/FrameDecoder.cpp',
               'openrtx/src/protocols/M17/LinkSetupFrame.cpp']

openrtx_inc = ['openrtx/include', 'platform']

##
## OpenRTX UI sources
##

ui_src_default = ['openrtx/src/ui/default/ui.c',
                  'openrtx/src/ui/default/ui_main.c',
                  'openrtx/src/ui/default/ui_menu.c',
                  'openrtx/src/ui/default/ui_hints.c',
                  'openrtx/src/ui/default/ui_layout.c',
                  'openrtx/src/ui/default/ui_zones.c',
                  'openrtx/src/ui/default/ui_quickmenu.c',
                  'openrtx/src/ui/default/ui_strings.c',
                  'openrtx/src/ui/default/ui_channels.c',
                  'openrtx/src/ui/default/ui_contacts.c',
                  'openrtx/src/ui/default/ui_messages.c',
                  'openrtx/src/ui/default/ui_tools.c',
                  'openrtx/src/ui/default/ui_spectrum.c']

ui_src_module17 = ['openrtx/src/ui/module17/ui.c',
                   'openrtx/src/ui/module17/ui_main.c',
                   'openrtx/src/ui/module17/ui_menu.c']

##
## Selection of main entrypoint
##
if get_option('test') != ''
  main_src     = 'tests/platform/'+get_option('test')+'.c'
  openrtx_def += {'RUNNING_TESTSUITE' : ''}
else
  main_src     = 'openrtx/src/main.c'
endif

##
## External libraries
##

# minmea, a lightweight GPS NMEA 0183 parser library
openrtx_src += ['lib/minmea/minmea.c']
openrtx_inc += ['lib/minmea/include']

# QRCode, QR Code generation library
openrtx_src += ['lib/QRCode/qrcode.c']
openrtx_inc += ['lib/QRCode/include']

# QDEC, a very simple, header only, quadrature decoding library
openrtx_inc += ['lib/qdec/include']

# CODEC2, open source speech codec, compile from source on embedded
# Always download the subproject — the Zephyr/CMake build needs the source
# files at subprojects/codec2/src/*.c regardless of which library is used
# for linking. For native builds, prefer system libraries (pkg-config or
# find_library) so homebrew, Nix, and distro packages work. For cross
# builds (embedded), use the subproject build.
codec2_proj = subproject('codec2')
if not meson.is_cross_build()
  # Try pkg-config first (homebrew, Nix, and other pkg-config-based installs)
  codec2_dep = dependency('codec2', method: 'pkg-config', required: false)
  if not codec2_dep.found()
    # Fall back to compiler's built-in library search (libcodec2-dev on Debian/Ubuntu)
    cpp        = meson.get_compiler('cpp')
    codec2_dep = cpp.find_library('codec2', required: false)
  endif
  if not codec2_dep.found()
    codec2_dep = codec2_proj.get_variable('codec2_dep')
  endif
else
  codec2_dep = codec2_proj.get_variable('codec2_dep')
endif

# Catch2, C++ testing framework (only for native/linux builds)
if not meson.is_cross_build()
  catch2_dep = dependency('catch2-with-main', fallback: ['catch2', 'catch2_with_main_dep'])
else
  catch2_dep = disabler()
endif

# XPowersLib, library for x-powers power management series
xpowerslib_proj = subproject('XPowersLib')

##
## RTOS
##
subdir('lib/miosix-kernel')

##
## Current git commit or tag
##
r = run_command('git', 'describe', '--tags', '--dirty', '--always')
if r.returncode() != 0
  # it failed
endif
git_version  = '"'+r.stdout().strip()+'"'
openrtx_def += {'GIT_VERSION': git_version}

##
## --------------------- Family-dependent source files -------------------------
##

##
## TYT MDx family
##

mdx_src = ['platform/drivers/NVM/W25Qx.c',
           'platform/drivers/NVM/nvmem_settings_MDx.c',
           'platform/drivers/NVM/nvmem_MDx.c',
           'platform/drivers/audio/audio_MDx.cpp',
           'platform/drivers/baseband/HR_Cx000.cpp',
           'platform/drivers/baseband/HR_C6000.cpp',
           'platform/drivers/tones/toneGenerator_MDx.cpp',
           'platform/drivers/SPI/spi_custom.c',
           'platform/drivers/SPI/spi_bitbang.c']

##
## GDx family: Radioddity GD-77 and Baofeng DM-1801
##

gdx_src = ['platform/targets/GDx/platform.c',
           'platform/targets/GDx/hwconfig.c',
           'platform/drivers/NVM/W25Qx.c',
           'platform/drivers/NVM/AT24Cx_GDx.c',
           'platform/drivers/NVM/nvmem_GDx.c',
           'platform/drivers/CPS/cps_io_native_GDx.c',
           'platform/drivers/ADC/ADC0_GDx.c',
           'platform/drivers/backlight/backlight_GDx.c',
           'platform/drivers/baseband/radio_GDx.cpp',
           'platform/drivers/baseband/HR_Cx000.cpp',
           'platform/drivers/baseband/HR_C6000.cpp',
           'platform/drivers/baseband/AT1846S_GDx.cpp',
           'platform/drivers/baseband/HR_C6000_GDx.cpp',
           'platform/drivers/display/UC1701_GDx.c',
           'platform/drivers/keyboard/keyboard_GDx.c',
           'platform/drivers/audio/audio_GDx.c',
           'platform/drivers/SPI/spi_custom.c',
           'platform/drivers/SPI/spi_bitbang.c',
           'platform/drivers/SPI/spi_mk22.c']

gdx_inc = ['platform/targets/GDx']

##
## --------------------- MCU-dependent source files ----------------------------
##

##
## STM32F405
##

stm32f405_src = ['platform/mcu/STM32F4xx/boot/startup.cpp',
                 'platform/mcu/STM32F4xx/boot/bsp.cpp',
                 'platform/mcu/STM32F4xx/boot/libc_integration.cpp',
                 'platform/mcu/STM32F4xx/drivers/usb/usb_bsp.c',
                 'platform/mcu/STM32F4xx/drivers/usb/usb_core.c',
                 'platform/mcu/STM32F4xx/drivers/usb/usb_dcd.c',
                 'platform/mcu/STM32F4xx/drivers/usb/usb_dcd_int.c',
                 'platform/mcu/STM32F4xx/drivers/usb/usbd_desc.c',
                 'platform/mcu/STM32F4xx/drivers/usb/usbd_core.c',
                 'platform/mcu/STM32F4xx/drivers/usb/usbd_ioreq.c',
                 'platform/mcu/STM32F4xx/drivers/usb/usbd_req.c',
                 'platform/mcu/STM32F4xx/drivers/usb/usbd_usr.c',
                 'platform/mcu/STM32F4xx/drivers/usb_vcom.c',
                 'platform/mcu/STM32F4xx/drivers/delays.cpp',
                 'platform/mcu/STM32F4xx/drivers/rtc.c',
                 'platform/mcu/STM32F4xx/drivers/USART3.cpp',
                 'platform/mcu/STM32F4xx/drivers/flash.c',
                 'platform/mcu/STM32F4xx/drivers/rng.c',
                 'platform/mcu/STM32F4xx/drivers/rcc.c',
                 'platform/mcu/STM32F4xx/drivers/i2c_stm32.c',
                 'platform/drivers/ADC/adc_stm32f4.c',
                 'platform/drivers/SPI/spi_stm32f4.c',
                 'platform/drivers/GPIO/gpio_stm32.c',
                 'platform/drivers/NVM/flash_stm32f4.c',
                 'platform/drivers/audio/stm32_dac.cpp',
                 'platform/drivers/audio/stm32_adc.cpp',
                 'platform/drivers/audio/stm32_pwm.cpp',
                 'platform/drivers/keyboard/cap1206.c',
                 'platform/mcu/CMSIS/Device/ST/STM32F4xx/Source/system_stm32f4xx.c']

stm32f405_inc = ['platform/mcu/CMSIS/Include',
                 'platform/mcu/CMSIS/Device/ST/STM32F4xx/Include',
                 'platform/mcu/STM32F4xx',
                 'platform/mcu/STM32F4xx/drivers',
                 'platform/mcu/STM32F4xx/drivers/usb']

stm32f405_def = {'STM32F405xx': '', 'HSE_VALUE':'8000000'}

stm32f405_src += miosix_cm4f_src
stm32f405_inc += miosix_cm4f_inc
stm32f405_def += miosix_cm4f_def

##
## MK22FN512
##

mk22fn512_src = ['platform/mcu/MK22FN512xxx12/boot/startup.cpp',
                 'platform/mcu/MK22FN512xxx12/boot/bsp.cpp',
                 'platform/mcu/MK22FN512xxx12/boot/libc_integration.cpp',
                 'platform/mcu/MK22FN512xxx12/drivers/delays.cpp',
                 'platform/mcu/MK22FN512xxx12/drivers/I2C0.c',
                 'platform/mcu/MK22FN512xxx12/drivers/rng.c',
                 'platform/mcu/MK22FN512xxx12/drivers/usb/usb_device_cdc_acm.c',
                 'platform/mcu/MK22FN512xxx12/drivers/usb/usb_device_ch9.c',
                 'platform/mcu/MK22FN512xxx12/drivers/usb/usb_device_dci.c',
                 'platform/mcu/MK22FN512xxx12/drivers/usb/usb_device_descriptor.c',
                 'platform/mcu/MK22FN512xxx12/drivers/usb/usb_device_khci.c',
                 'platform/mcu/MK22FN512xxx12/drivers/usb/usb_osa_bm.c',
                 'platform/mcu/MK22FN512xxx12/drivers/usb_vcom.c',
                 'platform/drivers/SPI/spi_mk22.c',
                 'platform/drivers/GPIO/gpio_mk22.c',
                 'platform/mcu/CMSIS/Device/NXP/MK22FN512xxx12/Source/system_MK22F51212.c']

mk22fn512_inc = ['platform/mcu/CMSIS/Include',
                 'platform/mcu/CMSIS/Device/NXP/MK22FN512xxx12/Include',
                 'platform/mcu/MK22FN512xxx12',
                 'platform/mcu/MK22FN512xxx12/drivers']

mk22fn512_def = {'MK22FN512xx': ''}

mk22fn512_src += miosix_cm4f_src
mk22fn512_inc += miosix_cm4f_inc
mk22fn512_def += miosix_cm4f_def

##
## STM32H743
##

stm32h743_src = ['platform/mcu/STM32H7xx/boot/startup.cpp',
                 'platform/mcu/STM32H7xx/boot/bsp.cpp',
                 'platform/mcu/STM32H7xx/boot/libc_integration.cpp',
                 'platform/mcu/STM32H7xx/drivers/rcc.cpp',
                 'platform/mcu/STM32H7xx/drivers/delays.cpp',
                 'platform/drivers/GPIO/gpio_stm32.c',
                 'platform/drivers/ADC/adc_stm32h7.c',
                 'platform/drivers/SPI/spi_stm32h7.c',
                 'platform/drivers/GPS/gps_stm32.cpp',
                 'platform/drivers/GPS/nmea_rbuf.c',
                 'platform/drivers/NVM/flash_stm32h7.c',
                 'platform/drivers/audio/stm32_dac.cpp',
                 'platform/drivers/audio/stm32_adc.cpp',
                 'platform/mcu/CMSIS/Device/ST/STM32H7xx/Source/system_stm32h7xx.c']

stm32h743_inc = ['platform/mcu/CMSIS/Include',
                 'platform/mcu/CMSIS/Device/ST/STM32H7xx/Include',
                 'platform/mcu/STM32H7xx',
                 'platform/mcu/STM32H7xx/drivers']

stm32h743_def = {'STM32H743xx': '', 'HSE_VALUE':'25000000'}

stm32h743_src += miosix_cm7_src
stm32h743_inc += miosix_cm7_inc
stm32h743_def += miosix_cm7_def

##
## AT32F423
##

at32f423_src = ['platform/mcu/AT32F423/boot/startup.cpp',
                'platform/mcu/AT32F423/boot/bsp.cpp',
                'platform/mcu/AT32F423/boot/libc_integration.cpp',
                'platform/mcu/AT32F423/drivers/delays.cpp',
                'platform/mcu/AT32F423/drivers/USART6.cpp',
                'platform/drivers/ADC/adc_at32.c',
                'platform/drivers/GPIO/gpio_at32f423.c',
                'platform/mcu/CMSIS/Device/Artery/AT32F423/Source/system_at32f423.c']

at32f423_inc = ['platform/mcu/AT32F423',
                'platform/mcu/AT32F423/drivers',
                'platform/mcu/CMSIS/Include',
                'platform/mcu/CMSIS/Device/Artery/AT32F423/Include']

at32f423_def = {'AT32F423CCT7':'', 'AT32F423xx':''}

at32f423_src += miosix_cm4f_src
at32f423_inc += miosix_cm4f_inc
at32f423_def += miosix_cm4f_def

##
## ----------------------- Platform specializations ----------------------------
##

##
## Linux
##
linux_src = ['platform/targets/linux/emulator/emulator.c',
             'platform/targets/linux/emulator/sdl_engine.c',
             # DMR and TETRA keep their decoded status in a translation unit
             # that includes the demodulator; the host decodes nothing, so it
             # answers the two getters with "no sync" and the screens of both
             # modes still build and are walked by the UI tests.
             'platform/targets/linux/emulator/ui_mode_status.c',
             'platform/drivers/display/display_libSDL.c',
             'platform/drivers/keyboard/keyboard_linux.c',
             'platform/drivers/NVM/nvmem_linux.c',
             'platform/drivers/GPS/gps_linux.c',
             'platform/mcu/x86_64/drivers/delays.c',
             'platform/mcu/x86_64/drivers/rng.cpp',
             'platform/drivers/baseband/radio_linux.cpp',
             'platform/drivers/audio/audio_linux.c',
             'platform/drivers/audio/file_source.c',
             'platform/drivers/audio/audio_sdl.cpp',
             'platform/targets/linux/platform.c',
             'platform/drivers/tones/toneGenerator_sdl.cpp',
             'platform/drivers/CPS/cps_io_libc.c',
             'platform/drivers/NVM/posix_file.c']

linux_inc = ['platform/targets/linux',
             'platform/targets/linux/emulator',
             'platform/drivers/audio',
             'platform/drivers/tones']

linux_def = {'PLATFORM_LINUX': '', 'VP_USE_FILESYSTEM':'',
             # Scan and vigilance run in the host emulator as well, so the
             # integration path of core/state.c is compiled and exercised
             # outside the radio. See docs/SCAN.md.
             'CONFIG_SCAN': '',
             # Same for the VFO carrier search: state_task() polls it and the
             # UI hooks are compiled, so the host build covers the whole path
             # except the radio itself. See docs/VFO_SEARCH.md.
             'CONFIG_VFO_SEARCH': ''}

sdl_dep     = dependency('SDL2',     required: false)
threads_dep = dependency('threads',  required: false)
linux_src  += openrtx_src
linux_inc  += openrtx_inc
linux_def  += openrtx_def
linux_def  += {'sniprintf':'snprintf', 'vsniprintf':'vsnprintf'}

#
# Standard UI
#
linux_default_src = linux_src + ui_src_default
# UI_TEST_NXDN compiles the UI's NXDN screens into the host build. It is not
# CONFIG_NXDN on purpose: that one makes rtx.cpp instantiate OpMode_NXDN, which
# would drag the demodulator and the AMBE+2 vocoder into every test binary. The
# UI only needs the status module (NxdnStatus.c, above), so the screens are
# tested here and the mode itself on the radio.
linux_default_def = linux_def + {'CONFIG_SCREEN_WIDTH': '160', 'CONFIG_SCREEN_HEIGHT': '128', 'CONFIG_PIX_FMT_RGB565': '',
                                 'CONFIG_GPS': '', 'CONFIG_RTC': '', 'UI_TEST_NXDN': '',
                                 'UI_TEST_DPMR': '', 'UI_TEST_P25': '',
                                 'UI_TEST_YSF': '', 'UI_TEST_DSTAR': '',
                                 # M17, TETRA and DMR: their screens, their
                                 # registry entries and their Settings pages,
                                 # without the engines. See ui_modes.h.
                                 'UI_TEST_M17': '', 'UI_TEST_TETRA': '',
                                 'UI_TEST_DMR': '',
                                 # The TETRA page of Settings (AFC) too.
                                 'CONFIG_TETRA_AFC': ''}
linux_small_def   = linux_def + {'CONFIG_SCREEN_WIDTH': '128', 'CONFIG_SCREEN_HEIGHT': '64', 'CONFIG_PIX_FMT_BW': '',
                                 'CONFIG_GPS': '', 'CONFIG_RTC': ''}

#
# Module17 UI
#
linux_mod17_src = linux_src + ui_src_module17
linux_mod17_def = linux_def + {'CONFIG_SCREEN_WIDTH': '128', 'CONFIG_SCREEN_HEIGHT': '64', 'CONFIG_PIX_FMT_BW': ''}

linux_c_args   = ['-ffunction-sections', '-fdata-sections', '-std=gnu17']
linux_cpp_args = ['-ffunction-sections', '-fdata-sections', '-std=c++14']
linux_l_args   = ['-lm', '-lreadline']

# MacOS Clang linker uses -deadstrip instead of --gc-sections
if build_machine.system() == 'darwin'
  linux_l_args += '-Wl,-dead_strip'
else
  linux_l_args += '-Wl,--gc-sections'
endif

# Add AddressSanitizer if required
if get_option('asan')
  linux_c_args   += '-fsanitize=address'
  linux_cpp_args += '-fsanitize=address'
  linux_l_args   += '-fsanitize=address'
endif

# Add Undefined Behaviour Sanitizer if required
if get_option('ubsan')
  linux_c_args   += '-fsanitize=undefined'
  linux_cpp_args += '-fsanitize=undefined'
  linux_l_args   += '-fsanitize=undefined'
endif


##
## TYT MD-3x0 family
##
md3x0_src = ['platform/drivers/CPS/cps_io_native_MD3x0.c',
             'platform/drivers/baseband/SKY72310.c',
             'platform/drivers/baseband/radio_MD3x0.cpp',
             'platform/drivers/baseband/HR_C5000_MDx.cpp',
             'platform/drivers/keyboard/keyboard_MD3x.c',
             'platform/drivers/display/HX8353_MD3x.cpp',
             'platform/drivers/backlight/backlight_MDx.c',
             'platform/drivers/GPS/gps_stm32.cpp',
             'platform/drivers/GPS/nmea_rbuf.c',
             'platform/targets/MD-3x0/platform.c',
             'platform/targets/MD-3x0/hwconfig.c']

md3x0_inc = ['platform/targets/MD-3x0']
md3x0_def = {'PLATFORM_MD3x0': '', 'timegm': 'mktime'}

md3x0_src += openrtx_src + stm32f405_src + ui_src_default + mdx_src
md3x0_inc += openrtx_inc + stm32f405_inc
md3x0_def += openrtx_def + stm32f405_def

##
## TYT MD-UV380
##
mduv3x0_src = ['platform/drivers/CPS/cps_io_native_MDUV3x0.c',
               'platform/targets/MD-UV3x0/platform.c',
               'platform/targets/MD-UV3x0/hwconfig.c',
               'platform/drivers/GPS/gps_stm32.cpp',
               'platform/drivers/GPS/nmea_rbuf.c',
               'platform/drivers/keyboard/keyboard_MD3x.c',
               'platform/drivers/display/HX8353_MD3x.cpp',
               'platform/drivers/backlight/backlight_MDx.c',
               'platform/drivers/chSelector/chSelector_UV3x0.c',
               'platform/drivers/audio/Cx000_dac.cpp',
               'platform/drivers/baseband/radio_UV3x0.cpp',
               'platform/drivers/baseband/AT1846S_UV3x0.cpp',
               'platform/drivers/baseband/HR_C6000_UV3x0.cpp']

mduv3x0_inc = ['platform/targets/MD-UV3x0']
mduv3x0_def =  {'PLATFORM_MDUV3x0': '', 'timegm': 'mktime'}

mduv3x0_src += openrtx_src + stm32f405_src + ui_src_default + mdx_src
mduv3x0_inc += openrtx_inc + stm32f405_inc
mduv3x0_def += openrtx_def + stm32f405_def

##
## TYT MD-9600
##
md9600_src = ['platform/targets/MD-9600/platform.c',
              'platform/targets/MD-9600/hwconfig.c',
              'platform/drivers/GPS/gps_stm32.cpp',
              'platform/drivers/GPS/nmea_rbuf.c',
              'platform/drivers/display/ST7567_MD9600.c',
              'platform/drivers/keyboard/keyboard_MD9600.c',
              'platform/drivers/backlight/backlight_MDx.c',
              'platform/drivers/chSelector/chSelector_MD9600.c',
              'platform/drivers/baseband/radio_MD9600.cpp',
              'platform/drivers/CPS/cps_io_native_MD9600.c']

md9600_inc = ['platform/targets/MD-9600']
md9600_def = {'PLATFORM_MD9600': ''}

md9600_src += openrtx_src + stm32f405_src + ui_src_default + mdx_src
md9600_inc += openrtx_inc + stm32f405_inc
md9600_def += openrtx_def + stm32f405_def

##
## Radioddity GD-77
##
gd77_src = []
gd77_inc = []
gd77_def = {'PLATFORM_GD77': ''}

gd77_src += openrtx_src + mk22fn512_src + ui_src_default + gdx_src
gd77_inc += openrtx_inc + mk22fn512_inc + gdx_inc
gd77_def += openrtx_def + mk22fn512_def

##
## Baofeng DM-1801
##
dm1801_src = []
dm1801_inc = []
dm1801_def = {'PLATFORM_DM1801': ''}

dm1801_src += openrtx_src + mk22fn512_src + ui_src_default + gdx_src
dm1801_inc += openrtx_inc + mk22fn512_inc + gdx_inc
dm1801_def += openrtx_def + mk22fn512_def

##
## Module 17
##
mod17_src = ['platform/targets/Module17/platform.c',
             'platform/drivers/display/display_Mod17.c',
             'platform/drivers/display/SH110x_Mod17.c',
             'platform/drivers/display/SSD1309_Mod17.c',
             'platform/drivers/keyboard/keyboard_Mod17.c',
             'platform/drivers/NVM/nvmem_Mod17.c',
             'platform/drivers/CPS/cps_io_native_Mod17.c',
             'platform/drivers/baseband/radio_Mod17.cpp',
             'platform/drivers/audio/audio_Mod17.c',
             'platform/drivers/audio/MAX9814_Mod17.cpp',
             'platform/drivers/baseband/MCP4551.c']

mod17_inc = ['platform/targets/Module17']
mod17_def = {'PLATFORM_MOD17': ''}

mod17_src += openrtx_src + stm32f405_src + ui_src_module17
mod17_inc += openrtx_inc + stm32f405_inc
mod17_def += openrtx_def + stm32f405_def

##
## Connect Systems CS70000
##
cs7000_src = ['platform/drivers/NVM/nvmem_CS7000.c',
             'platform/drivers/NVM/W25Qx.c',
             'platform/drivers/NVM/eeep.c',
             'platform/drivers/stubs/cps_io_stub.c',
             'platform/drivers/baseband/AK2365A.c',
             'platform/drivers/baseband/SKY72310.c',
             'platform/drivers/baseband/HR_C6000.cpp',
             'platform/drivers/baseband/HR_Cx000.cpp',
             'platform/drivers/baseband/HR_C6000_CS7000.cpp',
             'platform/drivers/baseband/radio_CS7000.cpp',
             'platform/drivers/keyboard/keyboard_CS7000.c',
             'platform/drivers/display/ST7735R_CS7000.c',
             'platform/drivers/backlight/backlight_CS7000.c',
             'platform/drivers/audio/Cx000_dac.cpp',
             'platform/drivers/audio/audio_CS7000.cpp',
             'platform/drivers/GPIO/gpio_shiftReg.c',
             'platform/drivers/SPI/spi_custom.c',
             'platform/drivers/SPI/spi_bitbang.c',
             'platform/drivers/GPS/gps_stm32.cpp',
             'platform/drivers/GPS/nmea_rbuf.c',
             'platform/targets/CS7000/hwconfig.c',
             'platform/targets/CS7000/platform.c']

cs7000_inc = ['platform/targets/CS7000']
cs7000_def = {'PLATFORM_CS7000': '', 'timegm': 'mktime'}

cs7000_src += openrtx_src + stm32f405_src + ui_src_default
cs7000_inc += openrtx_inc + stm32f405_inc
cs7000_def += openrtx_def + stm32f405_def

##
## Connect Systems CS70000-PLUS
##
cs7000p_src = ['platform/drivers/NVM/nvmem_CS7000.c',
               'platform/drivers/NVM/W25Qx.c',
               'platform/drivers/NVM/eeep.c',
               'platform/drivers/baseband/HR_C6000.cpp',
               'platform/drivers/baseband/HR_Cx000.cpp',
               'platform/drivers/baseband/HR_C6000_CS7000.cpp',
               'platform/drivers/baseband/radio_CS7000.cpp',
               'platform/drivers/baseband/AK2365A.c',
               'platform/drivers/baseband/SKY72310.c',
               'platform/drivers/audio/Cx000_dac.cpp',
               'platform/drivers/audio/audio_CS7000.cpp',
               'platform/drivers/stubs/cps_io_stub.c',
               'platform/drivers/display/ST7735R_CS7000.c',
               'platform/drivers/keyboard/keyboard_CS7000.c',
               'platform/drivers/backlight/backlight_CS7000.c',
               'platform/drivers/GPIO/gpio_shiftReg.c',
               'platform/drivers/SPI/spi_custom.c',
               'platform/drivers/SPI/spi_bitbang.c',
               'platform/targets/CS7000-PLUS/hwconfig.c',
               'platform/targets/CS7000-PLUS/platform.c']

cs7000p_inc = ['platform/targets/CS7000-PLUS']
cs7000p_def = {'PLATFORM_CS7000P': '', 'timegm': 'mktime'}

cs7000p_src += openrtx_src + stm32h743_src + miosix_cm7_src + ui_src_default
cs7000p_inc += openrtx_inc + stm32h743_inc + miosix_cm7_inc
cs7000p_def += openrtx_def + stm32h743_def + miosix_cm7_def

##
## Baofeng DM-1701
##
dm1701_src = ['platform/targets/DM-1701/platform.c',
              'platform/targets/DM-1701/hwconfig.c',
              'platform/drivers/keyboard/keyboard_DM1701.c',
              'platform/drivers/CPS/cps_io_native_MDUV3x0.c',
              'platform/drivers/display/HX8353_MD3x.cpp',
              'platform/drivers/backlight/backlight_MDx.c',
              'platform/drivers/chSelector/chSelector_UV3x0.c',
              'platform/drivers/audio/Cx000_dac.cpp',
              'platform/drivers/baseband/radio_UV3x0.cpp',
              'platform/drivers/baseband/AT1846S_UV3x0.cpp',
              'platform/drivers/baseband/HR_C6000_UV3x0.cpp']

dm1701_inc = ['platform/targets/DM-1701']
dm1701_def = {'PLATFORM_DM1701': '', 'timegm': 'mktime'}

dm1701_src += openrtx_src + stm32f405_src + ui_src_default + mdx_src
dm1701_inc += openrtx_inc + stm32f405_inc
dm1701_def += openrtx_def + stm32f405_def

##
## Radtel RT-4D
##

rt4d_src = ['platform/drivers/NVM/nvmem_rt4d.c',
            'platform/drivers/NVM/W25Qx.c',
            'platform/targets/RT-4D/platform.c',
            'platform/drivers/backlight/backlight_RT4D.c',
            'platform/drivers/stubs/audio_stub.c',
            'platform/drivers/CPS/cps_io_native_RT4D.c',
            'platform/drivers/display/display_rt4d.c',
            'platform/drivers/keyboard/keyboard_rt4d.c',
            'platform/drivers/stubs/radio_stub.c',
            'platform/drivers/SPI/spi_custom.c',
            'platform/drivers/SPI/spi_bitbang.c',
            'platform/targets/RT-4D/hwconfig.c']
rt4d_inc = ['platform/targets/RT-4D']
rt4d_def = {'PLATFORM_RT4D': ''}

rt4d_src += openrtx_src + at32f423_src + ui_src_default
rt4d_inc += openrtx_inc + at32f423_inc
rt4d_def += openrtx_def + at32f423_def

##
## -------------------------- Compilation arguments ----------------------------
##

linux_c_args         = linux_c_args
linux_cpp_args       = linux_cpp_args
linux_small_c_args   = linux_c_args
linux_small_cpp_args = linux_cpp_args
linux_mod17_c_args   = linux_c_args
linux_mod17_cpp_args = linux_cpp_args

foreach k, v : linux_default_def
  if v == ''
    linux_c_args   += '-D@0@'.format(k)
    linux_cpp_args += '-D@0@'.format(k)
  else
    linux_c_args   += '-D@0@=@1@'.format(k, v)
    linux_cpp_args += '-D@0@=@1@'.format(k, v)
  endif
endforeach

foreach k, v : linux_small_def
  if v == ''
    linux_small_c_args   += '-D@0@'.format(k)
    linux_small_cpp_args += '-D@0@'.format(k)
  else
    linux_small_c_args   += '-D@0@=@1@'.format(k, v)
    linux_small_cpp_args += '-D@0@=@1@'.format(k, v)
  endif
endforeach

foreach k, v : linux_mod17_def
  if v == ''
    linux_mod17_c_args   += '-D@0@'.format(k)
    linux_mod17_cpp_args += '-D@0@'.format(k)
  else
    linux_mod17_c_args   += '-D@0@=@1@'.format(k, v)
    linux_mod17_cpp_args += '-D@0@=@1@'.format(k, v)
  endif
endforeach

md3x0_args = []
foreach k, v : md3x0_def
  if v == ''
    md3x0_args += '-D@0@'.format(k)
  else
    md3x0_args += '-D@0@=@1@'.format(k, v)
  endif
endforeach

mduv3x0_args = []
foreach k, v : mduv3x0_def
  if v == ''
    mduv3x0_args += '-D@0@'.format(k)
  else
    mduv3x0_args += '-D@0@=@1@'.format(k, v)
  endif
endforeach

gd77_args = []
foreach k, v : gd77_def
  if v == ''
    gd77_args += '-D@0@'.format(k)
  else
    gd77_args += '-D@0@=@1@'.format(k, v)
  endif
endforeach

dm1801_args = []
foreach k, v : dm1801_def
  if v == ''
    dm1801_args += '-D@0@'.format(k)
  else
    dm1801_args += '-D@0@=@1@'.format(k, v)
  endif
endforeach

md9600_args = []
foreach k, v : md9600_def
  if v == ''
    md9600_args += '-D@0@'.format(k)
  else
    md9600_args += '-D@0@=@1@'.format(k, v)
  endif
endforeach

mod17_args = []
foreach k, v : mod17_def
  if v == ''
    mod17_args += '-D@0@'.format(k)
  else
    mod17_args += '-D@0@=@1@'.format(k, v)
  endif
endforeach

cs7000_args = []
foreach k, v : cs7000_def
  if v == ''
    cs7000_args += '-D@0@'.format(k)
  else
    cs7000_args += '-D@0@=@1@'.format(k, v)
  endif
endforeach

cs7000p_args = []
foreach k, v : cs7000p_def
  if v == ''
    cs7000p_args += '-D@0@'.format(k)
  else
    cs7000p_args += '-D@0@=@1@'.format(k, v)
  endif
endforeach

dm1701_args = []
foreach k, v : dm1701_def
  if v == ''
    dm1701_args += '-D@0@'.format(k)
  else
    dm1701_args += '-D@0@=@1@'.format(k, v)
  endif
endforeach

rt4d_args = []
foreach k, v : rt4d_def
  if v == ''
    rt4d_args += '-D@0@'.format(k)
  else
    rt4d_args += '-D@0@=@1@'.format(k, v)
  endif
endforeach

linux_opts = {
  'sources'            : linux_default_src,
  'include_directories': linux_inc,
  'dependencies'       : [sdl_dep, threads_dep, codec2_dep],
  'c_args'             : linux_c_args,
  'cpp_args'           : linux_cpp_args,
  'link_args'          : linux_l_args
}

linux_small_opts = {
    'sources'            : linux_default_src,
    'include_directories': linux_inc,
    'dependencies'       : [sdl_dep, threads_dep, codec2_dep],
    'c_args'             : linux_small_c_args,
    'cpp_args'           : linux_small_cpp_args,
    'link_args'          : linux_l_args
}

linux_mod17_opts = {
    'sources'            : linux_mod17_src,
    'include_directories': linux_inc,
    'dependencies'       : [sdl_dep, threads_dep, codec2_dep],
    'c_args'             : linux_mod17_c_args,
    'cpp_args'           : linux_mod17_cpp_args,
    'link_args'          : linux_l_args
}

##
## The C62 simulator
##
## The host emulator, built with the radio's own UI configuration instead of the
## generic Linux one: every mode's screens, the quick menu rows that only exist
## when the platform defines the feature (the keypad backlight, the fine RX
## tuning), APRS and the packet modem page. It adds the frame recorder
## (core/gfx_capture.c) and the text dump of the screen (ui/ui_dump.c), and the
## scriptable control channel that replaces the readline shell
## (platform/targets/linux/emulator/sim_control.c). docs/SIMULATOR.md.
##
## It is a host-only target: the firmware builds of the radio get the same
## screens from its Kconfig options, and none of this code is compiled into an
## image.
##
zlib_dep = dependency('zlib', required : false)

sim_src = linux_default_src + [
  'openrtx/src/core/gfx_capture.c',
  'openrtx/src/core/aprs_service.c',
  'openrtx/src/core/messaging_aprs.c',
  'openrtx/src/protocols/APRS/ax25.c',
  'openrtx/src/protocols/APRS/hdlc.c',
  'openrtx/src/protocols/APRS/afsk.c',
  'openrtx/src/protocols/APRS/aprs.c',
  'openrtx/src/protocols/APRS/aprs_msg.c',
  # The packet modem's protocol layer is freestanding: the host bench
  # (tests/host/kiss_loop) runs the same three files. The service on top of it
  # (core/packet_service.c) needs the audio streams and a thread of its own, so
  # it stays the double in platform/targets/linux/emulator/ui_mode_status.c.
  'openrtx/src/protocols/KISS/kiss.c',
  'openrtx/src/protocols/KISS/afsk9600.c',
  'openrtx/src/protocols/KISS/packet_modem.c',
  'openrtx/src/ui/default/ui_aprs.c',
  'openrtx/src/ui/default/ui_packet.c',
  'openrtx/src/ui/default/ui_dump.c',
  'platform/targets/linux/emulator/sim_control.c',
  'platform/targets/linux/emulator/sim_png.c',
  # The channel bank and the contact book, in a file instead of a partition
  'platform/targets/linux/emulator/user_store_host.c',
]

# The radio's own feature flags, as -D on top of linux_default_def, which
# already turns on the screens of the modes that need an engine
# (UI_TEST_M17/NXDN/DPMR/P25/YSF/DSTAR/DMR/TETRA). CONFIG_APRS and CONFIG_KISS
# are the two the C62's Kconfig builds (platform/targets/c62/Kconfig.board).
sim_def = ['-DPLATFORM_LINUX_SIM', '-DUI_CAPTURE',
           '-DCONFIG_APRS', '-DCONFIG_KISS']

sim_c_args   = linux_c_args + sim_def
sim_cpp_args = linux_cpp_args + sim_def

if zlib_dep.found()
  openrtx_c62_sim = executable('openrtx_c62_sim',
                               sources             : sim_src + main_src,
                               include_directories : linux_inc,
                               dependencies        : [sdl_dep, threads_dep,
                                                      codec2_dep, zlib_dep],
                               c_args              : sim_c_args,
                               cpp_args            : sim_cpp_args,
                               link_args           : linux_l_args)
  alias_target('simulator', openrtx_c62_sim)
endif


md3x0_opts = {
  'sources'            : md3x0_src,
  'include_directories': md3x0_inc,
  'dependencies'       : [codec2_dep],
  'c_args'             : md3x0_args,
  'cpp_args'           : md3x0_args,
  'link_args'          : ['-Wl,-T../platform/mcu/STM32F4xx/linker_script_MDx.ld']
}

mduv3x0_opts = {
  'sources'            : mduv3x0_src,
  'include_directories': mduv3x0_inc,
  'dependencies'       : [codec2_dep],
  'c_args'             : mduv3x0_args,
  'cpp_args'           : mduv3x0_args,
  'link_args'          : ['-Wl,-T../platform/mcu/STM32F4xx/linker_script_MDx.ld']
}

gd77_opts = {
  'sources'            : gd77_src,
  'include_directories': gd77_inc,
  'dependencies'       : [codec2_dep],
  'c_args'             : gd77_args,
  'cpp_args'           : gd77_args,
  'link_args'          : ['-Wl,-T../platform/mcu/MK22FN512xxx12/linker_script.ld']
}

dm1801_opts = {
  'sources'            : dm1801_src,
  'include_directories': dm1801_inc,
  'dependencies'       : [codec2_dep],
  'c_args'             : dm1801_args,
  'cpp_args'           : dm1801_args,
  'link_args'          : ['-Wl,-T../platform/mcu/MK22FN512xxx12/linker_script.ld']
}

md9600_opts = {
  'sources'            : md9600_src,
  'include_directories': md9600_inc,
  'dependencies'       : [codec2_dep],
  'c_args'             : md9600_args,
  'cpp_args'           : md9600_args,
  'link_args'          : ['-Wl,-T../platform/mcu/STM32F4xx/linker_script_MDx.ld']
}

mod17_opts = {
  'sources'            : mod17_src,
  'include_directories': mod17_inc,
  'dependencies'       : [codec2_dep],
  'c_args'             : mod17_args,
  'cpp_args'           : mod17_args,
  'link_args'          : ['-Wl,-T../platform/mcu/STM32F4xx/stm32_1m+192k_rom.ld']
}

rt4d_opts = {
  'sources'            : rt4d_src,
  'include_directories': rt4d_inc,
  'dependencies'       : [codec2_dep],
  'c_args'             : rt4d_args,
  'cpp_args'           : rt4d_args,
  'link_args'          : ['-Wl,-T../platform/mcu/AT32F423/linker_script.ld',
                          '-Wl,--print-memory-usage'],
}

ttwrplus_opts = {
  'sources'            : ['CMakeLists.txt'],
  'include_directories': [],
  'dependencies'       : [codec2_dep],
  'c_args'             : [],
  'cpp_args'           : [],
  'link_args'          : [],
}

c62_opts = {
  'sources'            : ['CMakeLists.txt'],
  'include_directories': [],
  'dependencies'       : [codec2_dep],
  'c_args'             : [],
  'cpp_args'           : [],
  'link_args'          : [],
}

cs7000_opts = {
  'sources'            : cs7000_src,
  'include_directories': cs7000_inc,
  'dependencies'       : [codec2_dep],
  'c_args'             : cs7000_args,
  'cpp_args'           : cs7000_args,
  'link_args'          : ['-Wl,-T../platform/mcu/STM32F4xx/stm32_1m+192k_rom.ld']
}

cs7000p_opts = {
  'sources'            : cs7000p_src,
  'include_directories': cs7000p_inc,
  'dependencies'       : [codec2_dep],
  'c_args'             : cs7000p_args,
  'cpp_args'           : cs7000p_args,
  'link_args'          : ['-Wl,-T../platform/mcu/STM32H7xx/linker_script_cs7000p.ld',
                          '-Wl,--print-memory-usage']
}

dm1701_opts = {
  'sources'            : dm1701_src,
  'include_directories': dm1701_inc,
  'dependencies'       : [codec2_dep],
  'c_args'             : dm1701_args,
  'cpp_args'           : dm1701_args,
  'link_args'          : ['-Wl,-T../platform/mcu/STM32F4xx/linker_script_MDx.ld']
}

##
## ---------------------------- Compilation targets ----------------------------
##

targets = [
  {
    'name'     : 'linux',
    'opts'     : linux_opts,
    'flashable': false,
    'cpu'      : 'linux'
  },
  {
    'name'     : 'linux_smallscreen',
    'opts'     : linux_small_opts,
    'flashable': false,
    'cpu'      : 'linux'
  },
  {
    'name'     : 'linux_mod17',
    'opts'     : linux_mod17_opts,
    'flashable': false,
    'cpu'      : 'linux'
  },
  {
    'name'     : 'md3x0',
    'opts'     : md3x0_opts,
    'flashable': true,
    'wrap'     : 'MD380',
    'load_addr': '0x0800C000',
    'cpu'      : 'cm4'
  },
  {
    'name'     : 'mduv3x0',
    'opts'     : mduv3x0_opts,
    'flashable': true,
    'wrap'     : 'UV3X0',
    'load_addr': '0x0800C000',
    'cpu'      : 'cm4'
  },
  {
    'name'     : 'gd77',
    'opts'     : gd77_opts,
    'flashable': true,
    'wrap'     : 'GD-77',
    'load_addr': '0x0800C000',
    'cpu'      : 'cm4'
  },
  {
    'name'     : 'dm1801',
    'opts'     : dm1801_opts,
    'flashable': true,
    'wrap'     : 'DM-1801',
    'load_addr': '0x0800C000',
    'cpu'      : 'cm4'
  },
  {
    'name'     : 'md9600',
    'opts'     : md9600_opts,
    'flashable': true,
    'wrap'     : 'MD9600',
    'load_addr': '0x0800C000',
    'cpu'      : 'cm4'
  },
  {
    'name'     : 'mod17',
    'opts'     : mod17_opts,
    'flashable': true,
    'wrap'     : ' ',
    'load_addr': ' ',
    'cpu'      : 'cm4'
  },
  {
    'name'     : 'rt4d',
    'opts'     : rt4d_opts,
    'flashable': true,
    'wrap'     : ' ',
    'load_addr': ' ',
    'cpu'      : 'cm4'
  },
  {
    'name'     : 'ttwrplus',
    'opts'     : ttwrplus_opts,
    'flashable': true,
    'wrap'     : ' ',
    'load_addr': ' ',
    'cpu'      : 'lx7'
  },
  {
    'name'     : 'c62',
    'opts'     : c62_opts,
    'flashable': true,
    'wrap'     : ' ',
    'load_addr': ' '
  },
  {
    'name'     : 'cs7000',
    'opts'     : cs7000_opts,
    'flashable': true,
    'wrap'     : 'cs7000',
    'load_addr': '0x08000000',
    'cpu'      : 'cm4'
  },
  {
    'name'     : 'cs7000p',
    'opts'     : cs7000p_opts,
    'flashable': true,
    'wrap'     : ' ',
    'load_addr': '0x08100000',
    'cpu'      : 'cm7'
  },
  {
    'name'     : 'dm1701',
    'opts'     : dm1701_opts,
    'flashable': true,
    'wrap'     : 'DM1701',
    'load_addr': '0x0800C000',
    'cpu'      : 'cm4'
  },
]

radio_tool  = find_program('radio_tool')
bin2sgl     = find_program('scripts/bin2sgl.py',               required:false, disabler:true)
objcopy     = find_program('objcopy',                          required:false, disabler:true)
gd77_loader = find_program('scripts/gd-77_firmware_loader.py', required:false, disabler:true)
dfu_util    = find_program('dfu-util',                         required:false, disabler:true)
west        = find_program('west',                             required:false, disabler:true)
uf2conv     = find_program('scripts/uf2conv.py',               required:false, disabler:true)
dfu_convert = find_program('scripts/dfu-convert.py',           required:false, disabler:true)
cs7000_wrap = find_program('scripts/cs7000_wrap.py',           required:false, disabler:true)

cm4_targets = []
cm7_targets = []
linux_targets = []

foreach t : targets

  name = 'openrtx_'+t['name']

  # ttwrplus is a Zephyr target, we compile it using west and package it in uf2 format
  if name == 'openrtx_ttwrplus'

      if build_machine.system() == 'windows'

          txt = custom_target('Copy CMakeLists.txt',
                              input            : 'CMakeLists.txt',
                              output           : 'CMakeLists.txt',
                              command          : ['xcopy', '@INPUT@', '@OUTPUT@'],
                              install          : false,
                              build_by_default : true)
      else  # Unix-like OS that has cp

          txt = custom_target('Copy CMakeLists.txt',
                              input            : 'CMakeLists.txt',
                              output           : 'CMakeLists.txt',
                              command          : ['cp', '@INPUT@', '@OUTPUT@'],
                              install          : false,
                              build_by_default : true)
      endif

      deps = custom_target(name+'_deps',
                          output  : name+'_deps.stamp',
                          capture : true,
                          command : [west,
                                    'update',
                                    '--group-filter', '+esp32'])

      bin = custom_target(name,
                          input   : txt,
                          output  : name+'_bin',
                          command : [west,
                                    'build',
                                    '-b', 'ttwrplus',
                                    '-d', '.', '..'])

      uf2 = custom_target(name+'_uf2',
                          input   : bin,
                          output  : name+'.uf2',
                          command : [uf2conv,
                                    'zephyr/zephyr.bin',
                                    '-c',
                                    '-f', 'ESP32S3',
                                    '-b', '0x0',
                                    '-o', '@OUTPUT@'])

      custom_target(name+'_flash',
                    input   : uf2,
                    output  : name+'_flash',
                    command : [uf2conv, '-D', '@INPUT@'])

  # C62 is a Zephyr target, we compile it using west and package it in uf2 format
  elif name == 'openrtx_c62'
      c62_modules = custom_target('c62_west_update',
                                  output  : 'c62_west_update.stamp',
                                  command : ['sh', '-c', 'west update --group-filter +csk6 && touch @OUTPUT@'])

      bin = custom_target(name,
                          input   : [txt, c62_modules],
                          output  : name+'_bin',
                          command : [west,
                                    'build',
                                    '-b', 'c62',
                                    '-d', '.', '..'])
  else

    target_opts = t['opts']
    exe = executable(name,
                     sources             : target_opts['sources'] + main_src,
                     include_directories : target_opts['include_directories'],
                     dependencies        : target_opts['dependencies'],
                     c_args              : target_opts['c_args'],
                     cpp_args            : target_opts['cpp_args'],
                     link_args           : target_opts['link_args'])
    if t['flashable']

      bin = custom_target(name+'_bin',
                          output  : name+'_bin',
                          input   : exe,
                          command : [objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@'])

      # Handle GD77 custom wrapping and flashing tools
      if name == 'openrtx_gd77' or name == 'openrtx_dm1801'

        wrap = custom_target(name+'_wrap',
                              output  : name+'_bin.sgl',
                              input   : bin,
                              command : [bin2sgl,
                                        '--in', '@INPUT@',
                                        '--out', '@OUTPUT@',
                                        '--mode', t['wrap']])

        custom_target(name+'_flash',
                      input   : wrap,
                      output  : name+'_flash',
                      command : [gd77_loader, '-f', '@INPUT@', '-m', t['wrap']])

      # Module17 uses dfu-tool for flashing
      elif name == 'openrtx_mod17'

        custom_target(name+'_flash',
                      input   : bin,
                      output  : name+'_flash',
                      command : [dfu_util,
                                 '-d', '0483:df11',
                                 '-a', '0',
                                 '-D', '@INPUT@',
                                 '-s', '0x08000000'])

      elif name == 'openrtx_cs7000' or name == 'openrtx_cs7000p'

        custom_target(name+'_dfu',
                      output  : name+'_dfu',
                      input   : bin,
                      command : [dfu_convert, '-b', t['load_addr']+':@INPUT@', '@OUTPUT@'])

        custom_target(name+'_wrap',
                      output  : name+'_wrap',
                      input   : bin,
                      command : [cs7000_wrap, '@INPUT@', '@OUTPUT@'])

        custom_target(name+'_flash',
                      input   : bin,
                      output  : name+'_flash',
                      command : [dfu_util,
                                 '-d', '0483:df11',
                                 '-a', '0',
                                 '-D', '@INPUT@',
                                 '-s', t['load_addr']])
      else

        wrap = custom_target(name+'_wrap',
                             output  : name+'_wrap',
                             input   : bin,
                             command : [radio_tool,
                                        '--wrap',
                                        '-o', '@OUTPUT@',
                                        '-r', t['wrap'],
                                        '-s', t['load_addr']+':@INPUT@'])

        custom_target(name+'_flash',
                      input   : wrap,
                      output  : name+'_flash',
                      command : [radio_tool, '-d', '0', '-f', '-i', '@INPUT@'])

      endif

    endif

    if t['cpu'] == 'cm4'
      cm4_targets += exe
    elif t['cpu'] == 'cm7'
      cm7_targets += exe
    elif t['cpu'] == 'linux'
      linux_targets += exe
    endif

  endif
endforeach

cm4 = alias_target('cm4', cm4_targets)
cm7 = alias_target('cm7', cm7_targets)
linux = alias_target('linux', linux_targets)

##
## ----------------------------------- Unit Tests ------------------------------
##

unit_test_opts = {'c_args'             : linux_c_args,
                  'cpp_args'           : linux_cpp_args,
                  'include_directories': linux_inc,
                  'dependencies'       : [sdl_dep, threads_dep, codec2_dep, catch2_dep],
                  'link_args'          : linux_l_args}

unit_test_src = linux_src + ui_src_default

m17_golay_test = executable('m17_golay_test',
                            sources : unit_test_src + ['tests/unit/M17_golay.cpp'],
                            kwargs  : unit_test_opts)

# P25 receive PHY.  The sources are added to the test on their own: nothing else
# in the Linux build pulls them in, and the test is the only consumer.
p25_phy_src = ['openrtx/src/protocols/P25/P25Phy.c',
               'openrtx/src/protocols/P25/P25Fec.c',
               'openrtx/src/protocols/P25/P25Defs.c',
               'openrtx/src/protocols/P25/P25Tables.c']

p25_phy_test = executable('p25_phy_test',
                          sources : unit_test_src + p25_phy_src +
                                    ['tests/unit/p25_phy.cpp'],
                          kwargs  : unit_test_opts)

m17_viterbi_test = executable('m17_viterbi_test',
                               sources : unit_test_src + ['tests/unit/M17_viterbi.cpp'],
                               kwargs  : unit_test_opts)

m17_callsign_test = executable('m17_callsign_test',
                      sources : unit_test_src + ['tests/unit/M17_callsign.cpp'],
                      kwargs  : unit_test_opts)

m17_metatext_test = executable('m17_metatext_test',
                      sources : unit_test_src + ['tests/unit/M17_metatext.cpp'],
                      kwargs  : unit_test_opts)
                 
# Test signals shared by the M17 tests (tests/platform/M17Signal.hpp)
m17_signal_src = ['tests/platform/NoisyChannel.cpp', 'tests/platform/M17Signal.cpp']
m17_signal_inc = linux_inc + ['tests/platform']

m17_demodulator_test = executable('m17_demodulator_test',
                            sources: unit_test_src + m17_signal_src + ['tests/unit/M17_demodulator.cpp'],
                            kwargs: unit_test_opts + {'include_directories' : m17_signal_inc})

m17_rrc_test = executable('m17_rrc_test',
                          sources: unit_test_src + ['tests/unit/M17_rrc.cpp'],
                          kwargs: unit_test_opts)

cps_test = executable('cps_test',
                      sources : unit_test_src + ['tests/unit/cps.cpp'],
                      kwargs  : unit_test_opts)

linux_inputStream_test = executable('linux_inputStream_test',
                                    sources : unit_test_src + ['tests/unit/linux_inputStream_test.cpp'],
                                    kwargs  : unit_test_opts)

minmea_conversion_test = executable('minmea_conversion_test',
                                     sources : unit_test_src + ['tests/unit/convert_minmea_coord.cpp'],
                                     kwargs  : unit_test_opts)

ui_check_standby_test = executable('ui_check_standby_test',
                                    sources : unit_test_src + ['tests/unit/ui_check_standby.cpp'],
                                    kwargs  : unit_test_opts)

ui_vfo_input_test = executable('ui_vfo_input_test',
                               sources : unit_test_src + ['tests/unit/ui_vfo_input.cpp'],
                               kwargs : unit_test_opts)

# The VFO carrier search through the real UI FSM: the three second hold, the
# arming, and the keys that cancel it.  It sleeps for the hold, so it is slow
# on purpose: the timing is the feature (docs/VFO_SEARCH.md).
ui_vfo_search_test = executable('ui_vfo_search_test',
                                sources : unit_test_src + ['tests/unit/ui_vfo_search.cpp'],
                                kwargs : unit_test_opts)

# Regresiones de la interfaz, por sintoma: el despacho de teclas se prueba a
# traves del FSM real (ui_updateFSM), con eventos como los que encola el teclado.
#
# Se compila dos veces. La primera, con CONFIG_APRS y las fuentes de APRS, es la
# misma configuracion de despacho que lleva la radio (la build mega tiene APRS).
# La segunda, sin APRS, es la que demuestra que las pantallas de mensajes,
# contactos y zonas siguen despachandose: su bloque vivia dentro de
# `#ifdef CONFIG_APRS`, asi que en esa configuracion no habia menu de mensajes.
ui_regressions_src = ['tests/unit/ui_regressions.cpp']
ui_regressions_aprs_src = ['openrtx/src/ui/default/ui_aprs.c',
                           'openrtx/src/core/aprs_service.c',
                           'openrtx/src/core/messaging_aprs.c',
                           'openrtx/src/protocols/APRS/ax25.c',
                           'openrtx/src/protocols/APRS/hdlc.c',
                           'openrtx/src/protocols/APRS/afsk.c',
                           'openrtx/src/protocols/APRS/aprs.c',
                           'openrtx/src/protocols/APRS/aprs_msg.c']

ui_regressions_opts = unit_test_opts + {
    'c_args'   : linux_c_args + ['-DCONFIG_APRS'],
    'cpp_args' : linux_cpp_args + ['-DCONFIG_APRS'],
}

ui_regressions_test = executable('ui_regressions_test',
                                 sources : unit_test_src + ui_regressions_aprs_src +
                                           ui_regressions_src,
                                 kwargs  : ui_regressions_opts)

ui_regressions_noaprs_test = executable('ui_regressions_noaprs_test',
                                        sources : unit_test_src + ui_regressions_src,
                                        kwargs  : unit_test_opts)

# El recorrido de los menús (docs/UI.md): cada pantalla de cada modo compilado,
# fila a fila y tecla a tecla, a través del FSM real. Se compila dos veces, como
# las regresiones: con APRS y el módem de paquetes (la configuración de la
# radio) y sin ellos, que es la build en la que las pantallas de mensajes,
# contactos y zonas se quedaban fuera por un #ifdef mal cerrado.
ui_menu_audit_src = ['tests/unit/ui_menu_audit.cpp']
# El módem de paquetes entra con su pantalla: core/packet_service.c pertenece al
# kernel (hilo propio, streams de audio, PSRAM), así que el host usa el doble de
# platform/targets/linux/emulator/ui_mode_status.c, como con los estados de DMR
# y TETRA.
ui_menu_audit_aprs_src = ui_regressions_aprs_src + ['openrtx/src/ui/default/ui_packet.c']

ui_menu_audit_opts = unit_test_opts + {
    'c_args'   : linux_c_args + ['-DCONFIG_APRS', '-DCONFIG_KISS'],
    'cpp_args' : linux_cpp_args + ['-DCONFIG_APRS', '-DCONFIG_KISS'],
}

ui_menu_audit_test = executable('ui_menu_audit_test',
                                sources : unit_test_src + ui_menu_audit_aprs_src +
                                          ui_menu_audit_src,
                                kwargs  : ui_menu_audit_opts)

ui_menu_audit_plain_test = executable('ui_menu_audit_plain_test',
                                      sources : unit_test_src + ui_menu_audit_src,
                                      kwargs  : unit_test_opts)

ui_quickmenu_test = executable('ui_quickmenu_test',
                               sources : unit_test_src + ['tests/unit/ui_quickmenu.cpp'],
                               kwargs  : unit_test_opts)

# P2 held on the FM main screen has to reach the APRS screens: the keypad
# reports the press and, 700 ms later, the long press, so the popup is already
# open when the long press arrives and used to eat it. The test presses the two
# events in that order (tests/unit/ui_aprs_entry.cpp).
ui_aprs_entry_test = executable('ui_aprs_entry_test',
                                sources : unit_test_src + ui_regressions_aprs_src +
                                          ['tests/unit/ui_aprs_entry.cpp'],
                                kwargs  : ui_regressions_opts)

# Green on a bank entry loads it as a memory channel: MAIN_MEM, tuner_mode CH,
# and the VFO kept for the way back (tests/unit/ui_bank_tune.cpp).
ui_bank_tune_test = executable('ui_bank_tune_test',
                               sources : unit_test_src + ['tests/unit/ui_bank_tune.cpp'],
                               kwargs  : unit_test_opts)

m17_packet_test = executable('m17_packet_test',
                              sources : unit_test_src + ['tests/unit/M17_packet.cpp'],
                              kwargs  : unit_test_opts)

m17_decoder_test = executable('m17_decoder_test',
                              sources : unit_test_src + ['tests/unit/M17_decoder.cpp'],
                              kwargs  : unit_test_opts)
ui_messages_test = executable('ui_messages_test',
                              sources : unit_test_src + ['tests/unit/ui_messages.cpp'],
                              kwargs : unit_test_opts)

ui_layout_test = executable('ui_layout_test',
                            sources : unit_test_src + ['tests/unit/ui_layout.cpp'],
                            kwargs : unit_test_opts)

ui_zones_test = executable('ui_zones_test',
                           sources : unit_test_src + ['tests/unit/ui_zones.cpp'],
                           kwargs : unit_test_opts)

# El menú de herramientas (P1 largo): abrir y cerrar, recorrer las filas, que
# cada una arranque el servicio que dice, el submenú de modos y el TX apagado
# mientras está abierto. Por el FSM real, con el teclado como lo alimenta la
# matriz (tests/unit/ui_tools.cpp).
ui_tools_test = executable('ui_tools_test',
                           sources : unit_test_src + ['tests/unit/ui_tools.cpp'],
                           kwargs : unit_test_opts)

# Registro de modos: la lista única de la que salen el editor de canales, la
# tecla lateral, el menú rápido y las páginas de ajustes de cada modo.
ui_modes_test = executable('ui_modes_test',
                           sources : unit_test_src + ['tests/unit/ui_modes.cpp'],
                           kwargs : unit_test_opts)

user_stores_test = executable('user_stores_test',
                                sources : unit_test_src + ['tests/unit/user_stores.cpp'],
                                kwargs : unit_test_opts)

channel_store_test = executable('channel_store_test',
                                sources : unit_test_src + ['tests/unit/channel_store.cpp'],
                                kwargs  : unit_test_opts)

# Protocolo binario con el PC. Se compila dos veces: la primera con el espacio
# de nombres de depuracion, la segunda sin el, para que "una build de produccion
# responde NOT_ALLOWED y no filtra nada" sea un test y no una promesa.
tx_power_test = executable('tx_power_test',
                           sources : unit_test_src + ['tests/unit/tx_power.cpp'],
                           kwargs  : unit_test_opts)

pc_proto_test = executable('pc_proto_test',
                           sources : unit_test_src + ['tests/unit/pc_proto.cpp'],
                           kwargs  : unit_test_opts)

pc_proto_opts_dbg = {'c_args' : linux_c_args + ['-DCONFIG_C62_DEBUG_PROTO'],
                     'cpp_args' : linux_cpp_args + ['-DCONFIG_C62_DEBUG_PROTO'],
                     'include_directories' : linux_inc,
                     'dependencies' : [sdl_dep, threads_dep, codec2_dep,
                                       catch2_dep],
                     'link_args' : linux_l_args}

pc_proto_dbg_test = executable('pc_proto_dbg_test',
                               sources : unit_test_src + ['tests/unit/pc_proto.cpp'],
                               kwargs  : pc_proto_opts_dbg)

# El banco de medidas (docs/BENCH.md). Igual: dos veces, con el espacio de
# nombres del banco y sin el, para que "una build de produccion no mueve la
# radio" sea un test y no una promesa.
pc_bench_opts = {'c_args' : linux_c_args + ['-DCONFIG_C62_DEBUG_PROTO',
                                            '-DCONFIG_C62_BENCH_PROTO'],
                 'cpp_args' : linux_cpp_args + ['-DCONFIG_C62_DEBUG_PROTO',
                                                '-DCONFIG_C62_BENCH_PROTO'],
                 'include_directories' : linux_inc,
                 'dependencies' : [sdl_dep, threads_dep, codec2_dep,
                                   catch2_dep],
                 'link_args' : linux_l_args}

pc_bench_test = executable('pc_bench_test',
                           sources : unit_test_src + ['tests/unit/pc_proto_bench.cpp'],
                           kwargs  : pc_bench_opts)

pc_bench_nobench_test = executable('pc_bench_nobench_test',
                                   sources : unit_test_src + ['tests/unit/pc_proto_bench.cpp'],
                                   kwargs  : unit_test_opts)

# Zonas: los planes de canales como datos (repetidoras R0-R7 y U0-U100, PMR446)
zones_test = executable('zones_test',
                        sources : unit_test_src + ['tests/unit/zones.cpp'],
                        kwargs  : unit_test_opts)

# Airband: los dos rasters de canales y los designadores de 8,33 kHz
airband_test = executable('airband_test',
                          sources : unit_test_src + ['tests/unit/airband.cpp'],
                          kwargs  : unit_test_opts)

# AGC de la banda aerea: ataque, liberacion y limitador
agc_test = executable('agc_test',
                      sources : unit_test_src + ['tests/unit/agc.cpp'],
                      kwargs  : unit_test_opts)

# Analizador de espectro, desde el teclado: la pantalla, sus teclas y su menu
ui_spectrum_test = executable('ui_spectrum_test',
                              sources : unit_test_src + ['tests/unit/ui_spectrum.cpp'],
                              kwargs  : unit_test_opts)

# Analizador de espectro: la rejilla del barrido, la traza contra un receptor
# sintetico, el pico, la retencion de maximos y el mapeo de nivel a pixel
spectrum_test = executable('spectrum_test',
                           sources : unit_test_src + ['tests/unit/spectrum.cpp'],
                           kwargs  : unit_test_opts)

# El coste del barrido visto por el servicio (tests/unit/spectrum_settle.cpp): la
# tabla de espera que recorre la fila ESPERA, el indice por defecto de la build,
# el estado que leen la pantalla y el menu, y el span de 96 MHz que el barrido
# rapido hizo posible.
spectrum_settle_test = executable('spectrum_settle_test',
                                  sources : unit_test_src + ['tests/unit/spectrum_settle.cpp'],
                                  kwargs  : unit_test_opts)

# Vocoder IMBE de P25 (lib/imbe_vocoder, docs/IMBE.md): el decodificador que
# lleva el modo P25 por defecto. El primero compara 64 tramas de la captura real
# contra su propia renderizacion comprometida (regresion bit a bit); el segundo
# barre errores de un bit y parametros aleatorios, que es lo que la cadena
# entrega en un canal con ruido. Los dos necesitan la ruta de los corpora y la
# del asset como rutas absolutas, para no depender del directorio de ejecucion.
imbe_defs = {
    'IMBE_CORPUS_DIR' : meson.project_source_root() / 'lib/imbe_vocoder/tests/data',
    'IMBE_ASSET_DIR'  : meson.project_source_root() / 'tests/unit/assets'
}
imbe_inc = linux_inc + ['lib/imbe_vocoder', 'lib/dmr_vocoder']
imbe_src = ['lib/imbe_vocoder/imbe_vocoder.c',
            'lib/imbe_vocoder/imbe_decoder.c',
            'lib/imbe_vocoder/imbe_fft.c']
imbe_test_defs = []
foreach k, v : imbe_defs
    imbe_test_defs += '-D@0@="@1@"'.format(k, v)
endforeach

imbe_vocoder_test = executable('imbe_vocoder_test',
                               sources : imbe_src + ['tests/unit/imbe_vocoder.cpp'],
                               kwargs  : {'include_directories' : imbe_inc,
                                          'c_args' : linux_c_args + ['-ffp-contract=off'],
                                          'cpp_args' : linux_cpp_args + imbe_test_defs,
                                          'dependencies' : [catch2_dep],
                                          'link_args' : linux_l_args})

imbe_robust_test = executable('imbe_robust_test',
                              sources : imbe_src + ['tests/unit/imbe_robust.cpp'],
                              kwargs  : {'include_directories' : imbe_inc,
                                         'c_args' : linux_c_args + ['-ffp-contract=off'],
                                         'cpp_args' : linux_cpp_args + imbe_test_defs,
                                         'dependencies' : [catch2_dep],
                                         'link_args' : linux_l_args})

# Modulador AM de la banda aerea: banda de audio, profundidad, portadora y rampas
am_tx_test = executable('am_tx_test',
                        sources : unit_test_src + ['tests/unit/am_tx.cpp'],
                        kwargs  : unit_test_opts)

# FM de radiodifusion: la banda, su rejilla de 100/50 kHz y el nombre de canal
wfm_test = executable('wfm_test',
                      sources : unit_test_src + ['tests/unit/wfm.cpp'],
                      kwargs  : unit_test_opts)

# Registro de ajustes: version, CRC y descarte limpio de un registro viejo
settings_record_test = executable('settings_record_test',
                                  sources : unit_test_src + ['tests/unit/settings_record.cpp'],
                                  kwargs : unit_test_opts)

# El puerto de accesorio: un pad, tres modos (docs/ACCESSORY.md). El
# discriminador de PTT contra el byte 0x00 y el break, la convivencia con las
# tramas HELLO intercaladas y la tabla de los tres modos, sobre el parser real.
port_mode_test = executable('port_mode_test',
                            sources : unit_test_src + ['tests/unit/port_mode.cpp'],
                            kwargs  : unit_test_opts)

# Caja negra (docs/BLACKBOX.md): el detector de atasco con un hilo que deja de
# latir y un reloj de mentira, el contenido del registro (campos, orden de las
# migajas, CRC, version) y la lectura al arrancar sobre una flash falsa con
# semantica NOR.
blackbox_test = executable('blackbox_test',
                           sources : unit_test_src + ['tests/unit/blackbox.cpp'],
                           kwargs  : unit_test_opts)

# APRS: validacion de indicativo, SSID y path antes de emitir
aprs_config_test = executable('aprs_config_test',
                              sources : unit_test_src + [
                                  'openrtx/src/protocols/APRS/ax25.c',
                                  'openrtx/src/protocols/APRS/aprs.c',
                                  'tests/unit/aprs_config.cpp'],
                              kwargs  : unit_test_opts)

# The package's own files include their headers without a prefix (dmr_fec.h,
# dmr_sms.h), so a target that compiles them needs the DMR directories too.
dmr_inc = ['openrtx/include/protocols/DMR',
           'openrtx/include/protocols/DMR/trunk']
dmr_test_opts = unit_test_opts + {'include_directories' : linux_inc + dmr_inc}

# Cabecera de llamada de DMR: el destino configurado llega al LC del paquete
# (dmr_tx_setup) y se lee de vuelta de la rafaga, como lo hace un receptor.
dmr_lc_dest_test = executable('dmr_lc_dest_test',
                              sources : unit_test_src + [
                                  'openrtx/src/protocols/DMR/dmr_fec.c',
                                  'openrtx/src/protocols/DMR/dmr_trellis.c',
                                  'openrtx/src/protocols/DMR/dmr_tx.c',
                                  'tests/unit/dmr_lc_dest.cpp'],
                              kwargs  : dmr_test_opts)

# DMR SMS: formato de payload elegido en los ajustes -> las tres formas del
# paquete, y las rafagas que salen por el aire para cada una.
dmr_sms_format_test = executable('dmr_sms_format_test',
                                 sources : unit_test_src + [
                                     'openrtx/src/protocols/DMR/dmr_fec.c',
                                     'openrtx/src/protocols/DMR/dmr_trellis.c',
                                     'openrtx/src/protocols/DMR/dmr_data.c',
                                     'openrtx/src/protocols/DMR/dmr_lrrp.c',
                                     'openrtx/src/protocols/DMR/dmr_sms.c',
                                     'openrtx/src/protocols/DMR/dmr_glue.c',
                                     'tests/unit/dmr_sms_format.cpp'],
                                 kwargs  : dmr_test_opts)

# Nuestro glue: los records que lee el paquete, el retardo de aire y su
# pre-roll.
dmr_glue_test = executable('dmr_glue_test',
                           sources : unit_test_src + [
                               'openrtx/src/protocols/DMR/dmr_glue.c',
                               'tests/unit/dmr_glue.cpp'],
                           kwargs  : dmr_test_opts)

# dPMR (TS 102 490): capa de codificacion, reparto AMBE+2 contra el camino DMR,
# datos cortos y una captura real por el demodulador completo.  El vocoder es
# el mismo lib/dmr_vocoder que usa el modo DMR.
# NXDN transmit: las mitades de codificacion (LICH, SACCH, VCH) cerradas
# contra sus decodificadores, y la cadena entera -- NxdnTx.c, el modulador 4FSK
# y nxdnPhy_push() -- exigiendo los 182 dibits, los cuatro cuadros AMBE+2 y el
# mensaje de la SACCH bit a bit, en tres configuraciones y dos niveles.
nxdn_tx_test = executable('nxdn_tx_test',
                          sources : ['tests/unit/nxdn_tx.cpp',
                                     'openrtx/src/protocols/Fsk4Tx.c',
                                     'openrtx/src/protocols/NXDN/NxdnDefs.c',
                                     'openrtx/src/protocols/NXDN/NxdnFec.c',
                                     'openrtx/src/protocols/NXDN/NxdnPhy.c',
                                     'openrtx/src/protocols/NXDN/NxdnTx.c'],
                          kwargs : {'include_directories' : linux_inc,
                                    'cpp_args' : linux_cpp_args + ['-O2'],
                                    'c_args' : linux_c_args + ['-O2',
                                                               '-ffp-contract=off'],
                                    'dependencies' : [catch2_dep],
                                    'link_args' : linux_l_args})

# dPMR transmit: las mitades de codificacion (CCH, HI, END, TCH) cerradas
# contra sus decodificadores y la cadena completa -- cabecera, supertramas y
# trama END -- bit a bit por dpmrPhy_push(), en dos llamadas y dos niveles.
dpmr_tx_test = executable('dpmr_tx_test',
                          sources : ['tests/unit/dpmr_tx.cpp',
                                     'openrtx/src/protocols/Fsk4Tx.c',
                                     'openrtx/src/protocols/DPMR/DpmrDefs.c',
                                     'openrtx/src/protocols/DPMR/DpmrPhy.c',
                                     'openrtx/src/protocols/DPMR/DpmrTx.c'],
                          kwargs : {'include_directories' : linux_inc,
                                    'cpp_args' : linux_cpp_args + ['-O2'],
                                    'c_args' : linux_c_args + ['-O2',
                                                               '-ffp-contract=off'],
                                    'dependencies' : [catch2_dep],
                                    'link_args' : linux_l_args})

dpmr_rx_test = executable('dpmr_rx_test',
                          sources : unit_test_src + [
                              'openrtx/src/protocols/DPMR/DpmrDefs.c',
                              'openrtx/src/protocols/DPMR/DpmrData.c',
                              'openrtx/src/protocols/DPMR/DpmrPhy.c',
                              'lib/dmr_vocoder/dmr_vocoder.c',
                              'lib/dmr_vocoder/dmr_decoder.c',
                              'lib/dmr_vocoder/dmr_fft.c',
                              'tests/unit/dpmr_rx.cpp'],
                          kwargs : unit_test_opts + {
                              'include_directories' : linux_inc + ['lib/dmr_vocoder'],
                              # El demodulador decide por margenes de EVM y de
                              # correlacion: -O2 y -ffp-contract=off son las
                              # opciones del firmware, y con ellas las capturas
                              # dan el mismo numero de tramas en host y en el
                              # C62.  Con -O0 el mismo fichero da 111 en vez de
                              # 121: la sensibilidad esta medida y documentada
                              # en docs/RX_DPMR.md.
                              'c_args' : linux_c_args + ['-O2', '-ffp-contract=off'],
                              'cpp_args' : linux_cpp_args + ['-O2']})

# The dPMR receive chain also has the two harnesses of tests/platform: the host
# one runs the firmware's own demodulator over a capture and prints what it
# decodes, and the bare-metal one measures its cost on a Cortex-M33 under QEMU.
# The host harness is built here like any other test and run by `meson test` on
# the seven seconds of the real capture kept in tests/unit/assets; the cost
# harness needs arm-none-eabi-gcc and qemu-system-arm, and its runner skips
# (exit 77) where they are missing.
dpmr_host_exe = executable('c62_dpmr_host',
                           sources : ['tests/platform/c62_dpmr_host.c',
                                      'openrtx/src/protocols/DPMR/DpmrDefs.c',
                                      'openrtx/src/protocols/DPMR/DpmrData.c',
                                      'openrtx/src/protocols/DPMR/DpmrPhy.c'],
                           kwargs  : {'include_directories' : linux_inc,
                                      'c_args' : linux_c_args +
                                                 ['-O2', '-ffp-contract=off'],
                                      'link_args' : linux_l_args})
dpmr_phy_cost_run = find_program('tests/platform/run_dpmr_phy_cost.sh')
dpmr_capture = meson.project_source_root() / 'tests/unit/assets/dpmr_air_frame.dis'

c62_calib_test = executable('c62_calib_test',
                            sources : ['tests/unit/C62_calib.cpp',
                                       'platform/targets/c62/calib_C62.c'],
                            kwargs  : unit_test_opts)

dsp_oversampling_test = executable('dsp_oversampling_test',
                                    sources : unit_test_src + ['tests/unit/dsp_oversampling.cpp'],
                                    kwargs  : unit_test_opts)

dsp_dcblock_test = executable('dsp_dcblock_test',
                              sources : unit_test_src + ['tests/unit/dsp_dcblock.cpp'],
                              kwargs  : unit_test_opts)

gfx_text_test = executable('gfx_text_test',
                            sources : unit_test_src + ['tests/unit/gfx_text.cpp'],
                            kwargs  : unit_test_opts)

slevel_test = executable('slevel_test',
                         sources : unit_test_src + ['tests/unit/slevel.cpp'],
                         kwargs  : unit_test_opts)

audio_codec_test = executable('audio_codec_test',
                              sources : unit_test_src + ['tests/unit/audio_codec.cpp'],
                              kwargs  : unit_test_opts)

# Transmit lifecycle: phases, deadlines and trace. TxGuard.c has no dependency
# on the OS at all (the time is passed in), so this links the guard alone.
tx_guard_test = executable('tx_guard_test',
                           sources : ['tests/unit/tx_guard.cpp',
                                      'openrtx/src/rtx/TxGuard.c'],
                           kwargs  : {'c_args'             : linux_c_args,
                                      'cpp_args'           : linux_cpp_args,
                                      'include_directories': linux_inc +
                                           ['openrtx/include'],
                                      'dependencies'       : [catch2_dep],
                                      'link_args'          : linux_l_args})

# The modes share one block of memory, the size of the largest of them
# (rtx/ModeArena.hpp, docs/MEMORY.md). The arena takes its storage and its
# registry as arguments, so this test needs no radio, no mode implementation
# and no protocol package: it drives the eleven sizes measured on the C62 image
# and sweeps all 121 ordered pairs of modes. Built against OpMode.hpp for the
# stub objects only.
mode_arena_test = executable('mode_arena_test',
                             sources : ['tests/unit/mode_arena.cpp',
                                        'openrtx/src/rtx/ModeArena.cpp',
                                        # OpMode::update() sleeps: the vtable
                                        # of the stubs pulls it in
                                        'platform/mcu/x86_64/drivers/delays.c'],
                             kwargs  : {'c_args'             : linux_c_args,
                                        'cpp_args'           : linux_cpp_args,
                                        'include_directories': linux_inc +
                                             ['openrtx/include'],
                                        'dependencies'       : [catch2_dep],
                                        'link_args'          : linux_l_args})

# The keying rule on its own (rtx/KeyGate.h): a mode change vetoes keying, the
# PTT has to be released once after it and a press has to last. Header-only, so
# the test needs nothing but Catch2.
# The radio thread's stack against the deepest chain of the build
# (rtx/DmrTxPlan.h's encoder path).  The failing combination -- TETRA and
# DMR_TX in the same image -- is compiled on purpose.
rtx_stack_test = executable('rtx_stack_budget_test',
                            sources : ['tests/unit/rtx_stack_budget.cpp'],
                            kwargs  : {'c_args'             : linux_c_args,
                                       'cpp_args'           : linux_cpp_args,
                                       'include_directories': linux_inc +
                                            ['openrtx/include'],
                                       'dependencies'       : [catch2_dep],
                                       'link_args'          : linux_l_args})
test('RTX Stack Budget Test', rtx_stack_test)

key_gate_test = executable('key_gate_test',
                           sources : ['tests/unit/key_gate.cpp'],
                           kwargs  : {'c_args'             : linux_c_args,
                                      'cpp_args'           : linux_cpp_args,
                                      'include_directories': linux_inc +
                                           ['openrtx/include'],
                                      'dependencies'       : [catch2_dep],
                                      'link_args'          : linux_l_args})

# The absolute transmit clock (rtx/TxWatch.h): air counted since the last
# release, the 90 s cut, the rekey, the unattended chain and the driver's
# margin. Header-only, so the test needs nothing but Catch2.
tx_watch_test = executable('tx_watch_test',
                           sources : ['tests/unit/tx_watch.cpp'],
                           kwargs  : {'c_args'             : linux_c_args,
                                      'cpp_args'           : linux_cpp_args,
                                      'include_directories': linux_inc +
                                           ['openrtx/include'],
                                      'dependencies'       : [catch2_dep],
                                      'link_args'          : linux_l_args})

# The mode switch through the firmware's own entry points (rtx.cpp): the real
# modes of the Linux build, every ordered pair of them, on the real arena. The
# radio driver, the audio paths and the UI come along in unit_test_src, which
# is what the emulator runs too.
mode_switch_test = executable('mode_switch_test',
                              sources : unit_test_src +
                                        ['tests/unit/mode_switch.cpp'],
                              kwargs  : unit_test_opts)

# The audio path table on its own: the priorities, the suspend/resume, and the
# property a change of mode depends on -- that the RTX thread and the UI thread
# can use it at the same time. The table had no lock, and the loop this test runs
# is the one that corrupted it (see tests/unit/audio_path.cpp).
audio_path_test = executable('audio_path_test',
                             sources : unit_test_src +
                                       ['tests/unit/audio_path.cpp'],
                             kwargs  : unit_test_opts)

# FM -> AM (airband) -> FM through rtx_configure()/rtx_task(), with the UI
# thread's share of a mode change on the same audio path table, and with a
# driver that refuses to receive. This is the reported freeze, modelled.
mode_switch_am_test = executable('mode_switch_am_test',
                                 sources : unit_test_src +
                                           ['tests/unit/mode_switch_am.cpp'],
                                 kwargs  : unit_test_opts)

# Scan and vigilance: the state machine (core/scanner.c) is pure — no clock, no
# radio — so this test drives it with a simulated receiver (synthetic squelch,
# a bank with SKIP flags, a priority channel) and measures its cost.  settings.c
# comes along for the step table it owns.
scanner_test = executable('scanner_test',
                          sources : ['tests/unit/scanner.cpp',
                                     'openrtx/src/core/scanner.c',
                                     'openrtx/src/core/settings.c'],
                          kwargs  : {'c_args'             : linux_c_args,
                                     'cpp_args'           : linux_cpp_args,
                                     'include_directories': linux_inc +
                                          ['openrtx/include'],
                                     'dependencies'       : [catch2_dep],
                                     'link_args'          : linux_l_args})

# Baseband replay (tests/platform/M17Replay.hpp): a command line tool and a
# unit test replaying a committed recording through the same class. The test
# gets the asset directory as an absolute path so it does not depend on the
# directory it is run from.
m17_replay_src = ['tests/platform/M17Replay.cpp']
m17_replay_inc = linux_inc + ['tests/platform']
m17_replay_tool = executable('m17_replay_tool',
                             sources             : unit_test_src + m17_replay_src + ['tests/platform/m17_replay_tool.cpp'],
                             c_args              : linux_c_args,
                             cpp_args            : linux_cpp_args,
                             include_directories : m17_replay_inc,
                             dependencies        : [sdl_dep, threads_dep, codec2_dep],
                             link_args           : linux_l_args)

m17_replay_test_opts = unit_test_opts + {'include_directories' : m17_replay_inc,
                                         'cpp_args' : linux_cpp_args +
      ['-DREPLAY_ASSET_DIR="' + meson.project_source_root() / 'tests/unit/assets' + '"']}
m17_replay_test = executable('m17_replay_test',
                             sources : unit_test_src + m17_replay_src + m17_signal_src + ['tests/unit/M17_replay.cpp'],
                             kwargs  : m17_replay_test_opts)

test('M17 Golay Unit Test',   m17_golay_test)
test('P25 PHY Test',           p25_phy_test)
test('M17 Viterbi Unit Test', m17_viterbi_test)
test('M17 Demodulator Test',  m17_demodulator_test)
test('M17 RRC Test',          m17_rrc_test)
test('M17 Callsign Unit Test',          m17_callsign_test)
test('M17 Meta Text Unit Test',         m17_metatext_test)
test('Codeplug Test',         cps_test)
## test('Linux InputStream Test', linux_inputStream_test)
test('minmea conversion Test', minmea_conversion_test)
test('UI Check Standby Test', ui_check_standby_test)
test('UI VFO Input Test', ui_vfo_input_test)
test('UI VFO Search Test', ui_vfo_search_test, timeout : 120)
test('UI Regressions Test', ui_regressions_test, timeout : 120)
test('UI Regressions Test (no APRS)', ui_regressions_noaprs_test, timeout : 120)
test('UI Quick Menu Test', ui_quickmenu_test)
test('UI APRS Entry Test',   ui_aprs_entry_test)
test('UI Bank Tune Test',    ui_bank_tune_test)

# El mapa del menu despues de cinco pasadas (tests/unit/ui_menu_map.cpp): el
# registro de modos y el menu rapido son la misma lista, cada fila es pintable,
# UI_MODE_TX y opmode_canTransmit() dicen lo mismo, y el analizador es
# alcanzable desde los dos modos analogicos con su fila ESPERA.
ui_menu_map_test = executable('ui_menu_map_test',
                              sources : unit_test_src + ['tests/unit/ui_menu_map.cpp'],
                              kwargs  : unit_test_opts)
test('UI Menu Map Test', ui_menu_map_test)
test('M17 Packet Frame Test', m17_packet_test)
test('M17 Frame Decoder Test', m17_decoder_test)
test('C62 Calibration Test', c62_calib_test)
test('Channel Store Test',   channel_store_test)
test('TX Power Test',        tx_power_test)
test('PC Protocol Test',     pc_proto_test)
test('PC Protocol Debug Test', pc_proto_dbg_test)
test('PC Bench Test',        pc_bench_test)
test('PC Bench Production Test', pc_bench_nobench_test)
test('Zones Test',           zones_test)
test('AGC Test',             agc_test)
test('AM TX Test',           am_tx_test)
test('Airband Test',         airband_test)
test('Broadcast FM Test',    wfm_test)

test('Spectrum Test',        spectrum_test)
test('Spectrum Settle Test',  spectrum_settle_test)
test('UI Spectrum Test',     ui_spectrum_test)
test('Settings Record Test', settings_record_test)
test('Accessory Port Test',  port_mode_test)
test('Blackbox Test',        blackbox_test)
test('P25 IMBE Vocoder Test', imbe_vocoder_test)
test('P25 IMBE Robustness Test', imbe_robust_test)
test('APRS Config Test',     aprs_config_test)
test('DMR SMS Format Test',  dmr_sms_format_test)
test('DMR LC Destination Test', dmr_lc_dest_test)
test('NXDN TX Test',          nxdn_tx_test)
test('dPMR TX Test',          dpmr_tx_test)
test('dPMR RX Test',          dpmr_rx_test)
test('dPMR Host Harness',     dpmr_host_exe, args : [dpmr_capture])
test('dPMR PHY Cost',         dpmr_phy_cost_run, args : [dpmr_capture])
test('DMR Glue Test',        dmr_glue_test)
test('User Stores Test',     user_stores_test)
test('UI Messages Test',     ui_messages_test)
test('UI Layout Test',       ui_layout_test)
test('UI Zones Test',        ui_zones_test)
test('UI Tools Test',        ui_tools_test)
test('UI Modes Test',        ui_modes_test)
test('DSP Oversampling Test', dsp_oversampling_test)
test('DSP DC Block Test',    dsp_dcblock_test)
test('GFX Text Test',        gfx_text_test)
test('S-Level Test', slevel_test)
test('Audio Codec Test',     audio_codec_test)
test('TX Guard Test',        tx_guard_test)
test('Key Gate Test',        key_gate_test)
test('TX Watch Test',        tx_watch_test)
test('Mode Arena Test',      mode_arena_test)
test('Mode Switch Test',     mode_switch_test, timeout : 120)
test('Audio Path Test',      audio_path_test, timeout : 120)
test('Mode Switch AM Test',  mode_switch_am_test, timeout : 120)

# Los once modos del enum, ida y vuelta, sobre el bloque compartido, y el lazo
# TX->RX despues del recorrido (tests/unit/mode_walk.cpp). El host tiene cuatro
# de los once, asi que los otros ocho comprueban que pedirlos deja la radio
# inactiva sin romper el cambio.
mode_walk_test = executable('mode_walk_test',
                            sources : unit_test_src + ['tests/unit/mode_walk.cpp'],
                            kwargs  : unit_test_opts)
test('Mode Walk Test',       mode_walk_test)

# Per-mode statistics (core/mode_stats.h). The layer takes everything it
# measures as an argument and its cycle counter is a weak symbol, so this links
# mode_stats.c alone and drives the counter from the test. Built with the
# option on and one cycle per microsecond, so the cost figures in the test are
# the cycle counts it sets.
mode_stats_test = executable('mode_stats_test',
                             sources : ['tests/unit/mode_stats.cpp',
                                        'openrtx/src/core/mode_stats.c'],
                             kwargs  : {'c_args'             : linux_c_args +
                                           ['-DCONFIG_C62_MODE_STATS',
                                            '-DMODE_STATS_CPU_MHZ=1'],
                                        'cpp_args'           : linux_cpp_args +
                                           ['-DCONFIG_C62_MODE_STATS'],
                                        'include_directories': linux_inc +
                                           ['openrtx/include'],
                                        'dependencies'       : [catch2_dep],
                                        'link_args'          : linux_l_args})
test('Mode Stats Test', mode_stats_test)

# And with the option off: the hooks must compile to nothing at all.
mode_stats_off_script = find_program('tests/mode_stats_off.sh')
test('Mode Stats Off Test', mode_stats_off_script,
     args : [meson.project_source_root(), ''])

test('Scanner Test',         scanner_test)

# VFO carrier search: the machine (core/vfo_search.c) is pure — the time and a
# sample of the receiver are arguments — so this test sweeps a band with a
# synthetic squelch, frames and a published frequency, and measures its cost.
vfo_search_test = executable('vfo_search_test',
                             sources : ['tests/unit/vfo_search.cpp',
                                        'openrtx/src/core/vfo_search.c'],
                             kwargs  : {'c_args'             : linux_c_args,
                                        'cpp_args'           : linux_cpp_args,
                                        'include_directories': linux_inc +
                                             ['openrtx/include'],
                                        'dependencies'       : [catch2_dep],
                                        'link_args'          : linux_l_args})
test('VFO Search Test',      vfo_search_test)
test('M17 Baseband Replay Test', m17_replay_test, timeout : 120) # ~40 s in CI with coverage

# The packet modem bench (tests/host/kiss_loop): the KISS codec, AFSK 1200 and
# G3RUH 9600 in loopback and their BER against noise. It builds itself with cc
# in its own directory, the way the operator runs it; the direwolf cross-check
# is optional and the script skips it when the tools are not installed.
kiss_loop_script = find_program('tests/host/kiss_loop/run.sh')
test('KISS Packet Modem Loop', kiss_loop_script, timeout : 300)

# The DMR transmitter on the host (tests/host/dmr_tx_host): the mode's own slot
# machine (rtx/DmrTxPlan.h), its burst builders and its encoder, walked through
# TUNE(DMR) -> PTT -> three seconds of bursts -> unkey, and the baseband decoded
# by the repository's reference receiver (work/dmr-rx).  It also runs the same
# loop against a sink that stops consuming and fails if the thread does not
# come back.
dmr_tx_host_script = find_program('tests/host/dmr_tx_host/run.sh')
test('DMR TX Host Sequence', dmr_tx_host_script, timeout : 300)

# The menu tour of the simulator (docs/SIMULATOR.md): every screen of the C62's
# interface, walked with keys through the control channel, with a PNG and a
# text dump per screen. It fails on a blank screen, on a row that cannot be
# selected, on a value that does not change and on a key that no block of the
# UI claims. `ninja -C build_sim test` builds the simulator first through the
# dependency.
python3 = import('python').find_installation('python3')
sim_tour_py = files('tools/simulator/tour.py')
if python3.found() and zlib_dep.found()
  test('Simulator Menu Tour',
       python3,
       args : [sim_tour_py, '--quiet', '--out', meson.current_build_dir() / 'sim-tour'],
       depends : openrtx_c62_sim,
       env : ['OPENRTX_SIM_BIN=' + openrtx_c62_sim.full_path()],
       timeout : 600)
endif

# The PC protocol's own harness (tests/host/pc_proto_host): three device builds
# -- protocol, protocol+debug, protocol+debug+bench -- driven through a pty by
# the Python clients that ship in tools/cps and tools/bench, including the bench
# end to end against a simulated radio. It builds itself with cc in its own
# directory, the way the operator runs it.
pc_proto_host_script = find_program('tests/host/pc_proto_host/run.sh')
test('PC Protocol Host Harness', pc_proto_host_script, timeout : 300)
