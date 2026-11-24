/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/platform.h"
#include "interfaces/cps_io.h"
#include "interfaces/delays.h"
#include <stdio.h>
#include <stdint.h>
#include "ui/ui_default.h"
#include "core/airband.h"
#ifdef CONFIG_APRS
#include "ui/ui_aprs.h"
#endif
#include <string.h>
#include "ui/ui_strings.h"
#include "core/utils.h"
#include "ui/utils.h"
#ifdef UI_MODE_NXDN
#include "protocols/NXDN/NxdnStatus.h"
#endif
#ifdef CONFIG_DMR
#include "protocols/DMR/DmrDefs.h"
#include "protocols/DMR/DmrStatus.h"
#endif
#ifdef CONFIG_TETRA
#include "protocols/TETRA/TetraSds.h"
#include "protocols/TETRA/TetraStatus.h"
#endif

/* An SDS text stays on the main screen this long */
#define TETRA_SDS_SHOW_MS 30000

/*
 * Text beside a symbol keeps this far from where gfx_drawSymbol() says the
 * symbol ends: the symbol bitmaps are wider than their advance (13 against
 * 9 pixels at 8 pt), and a gap keeps the two apart.
 */
#define TETRA_SYMBOL_GAP 6

/**
 * Print, centred on a line, the first of several texts that fits clear of a
 * left margin and the same margin on the right, measured in the font; the
 * last one is printed in any case. gfx_print() does not clip: text wider
 * than the screen wraps onto the line below.
 *
 * @param pos: line position.
 * @param font: font.
 * @param left: left margin, pixels.
 * @param texts: candidate texts, longest first.
 * @param n: number of candidates.
 */
static void _ui_printFit(point_t pos, fontSize_t font, uint16_t left,
                         const char *const *texts, size_t n)
{
    uint16_t avail = 0;
    if (CONFIG_SCREEN_WIDTH > 2 * left)
        avail = CONFIG_SCREEN_WIDTH - 2 * left;

    size_t i = 0;
    while (i + 1 < n && gfx_getTextWidth(font, texts[i]) > avail)
        i++;
    gfx_print(pos, font, TEXT_ALIGN_CENTER, color_white, "%s", texts[i]);
}

/**
 * Draw a symbol at the left of a line and centre a text clear of it.
 */
static void _ui_printWithSymbol(point_t pos, symbolSize_t symSize,
                                symbol_t symbol, fontSize_t font,
                                const char *const *texts, size_t n)
{
    point_t end = gfx_drawSymbol(pos, symSize, TEXT_ALIGN_LEFT, color_white,
                                 symbol);
    _ui_printFit(pos, font, end.x + TETRA_SYMBOL_GAP, texts, n);
}

/**
 * Format an SSI, or a dash if it is not known.
 */
static void _ui_formatSsi(char *buf, size_t size, const char *prefix,
                          uint32_t ssi)
{
    if (ssi != 0)
        snprintf(buf, size, "%s%lu", prefix, (unsigned long)ssi);
    else
        snprintf(buf, size, "%s-", prefix);
}

#ifdef CONFIG_TETRA
/**
 * Check whether a TETRA call takes over the main screen.
 */
static bool _ui_tetraCallShown(const struct tetraStatus *ts)
{
    return ts->sync && ts->call;
}

/**
 * TETRA cell or call information, from line 1 to line 4 during a call,
 * on line 2 otherwise.
 */
static void _ui_drawTetraInfo(void)
{
    struct tetraStatus ts;
    tetra_getStatus(&ts);
    const uint16_t pad = layout.horizontal_pad;
    char a[32], b[32], c[32];

    if (_ui_tetraCallShown(&ts)) {
        // Talking party
        _ui_formatSsi(a, sizeof(a), "", ts.talker);
        const char *talker[] = { a };
        _ui_printWithSymbol(layout.line1_pos, layout.line1_symbol_size,
                            SYMBOL_CALL_MADE, layout.line1_font, talker, 1);

        // Called group or individual
        _ui_formatSsi(a, sizeof(a), ts.callGroup ? "G " : "", ts.callDst);
        const char *called[] = { a };
        _ui_printWithSymbol(layout.line2_pos, layout.line2_symbol_size,
                            SYMBOL_CALL_RECEIVED, layout.line2_font, called,
                            1);

        // Cell, without a symbol: the text is too wide to go beside one
        snprintf(a, sizeof(a), "%u-%u LA %u", ts.mcc, ts.mnc, ts.la);
        snprintf(b, sizeof(b), "%u-%u", ts.mcc, ts.mnc);
        const char *cell[] = { a, b };
        _ui_printFit(layout.line3_pos, layout.line3_font, pad, cell, 2);

        // Call state
        if (ts.callEncrypted) {
            const char *enc[] = { "Encrypted" };
            _ui_printWithSymbol(layout.line4_pos, layout.line4_symbol_size,
                                SYMBOL_LOCK, layout.line4_font, enc, 1);
        } else {
            const char *voice = ts.voice ? "Voice" : "Idle";
            snprintf(a, sizeof(a), "%s, call %u", voice, ts.callId);
            const char *state[] = { a, voice };
            _ui_printFit(layout.line4_pos, layout.line4_font, pad,
                         ts.callId != 0 ? state : state + 1,
                         ts.callId != 0 ? 2 : 1);
        }
        return;
    }

    if (!ts.sync) {
        const char *searching[] = { "T searching" };
        _ui_printFit(layout.line2_pos, layout.line2_font, pad, searching, 1);
        return;
    }

    /* No symbol: centred, the cell text reaches the left edge (as in M17) */
    if (ts.sysinfo) {
        const char *enc = ts.airEncryption ? " enc" : "";
        snprintf(a, sizeof(a), "T %u-%u LA %u%s", ts.mcc, ts.mnc, ts.la, enc);
        snprintf(b, sizeof(b), "T %u-%u%s", ts.mcc, ts.mnc, enc);
        snprintf(c, sizeof(c), "T %u-%u", ts.mcc, ts.mnc);
        const char *cell[] = { a, b, c };
        _ui_printFit(layout.line2_pos, layout.line2_font, pad, cell, 3);
    } else {
        snprintf(a, sizeof(a), "T %u-%u CC %u", ts.mcc, ts.mnc,
                 ts.colourCode);
        snprintf(b, sizeof(b), "T %u-%u", ts.mcc, ts.mnc);
        const char *cell[] = { a, b };
        _ui_printFit(layout.line2_pos, layout.line2_font, pad, cell, 2);
    }
}

/**
 * TETRA line 1 in VFO mode, when no call is shown: the received SDS messages
 * (paged, scrolling) or the demodulator quality.
 */
static void _ui_drawTetraLine1(ui_state_t *ui_state)
{
    struct tetraStatus ts;
    tetra_getStatus(&ts);
    const uint16_t pad = layout.horizontal_pad;

    if (_ui_tetraCallShown(&ts))
        return;

    long long now = getTick();
    char sds[96];

    /*
     * tetraSds_line() pages through the messages not seen yet and marks them
     * read once it has shown them all, so a burst of short data is not lost
     * behind the newest one. Fresh messages are shown for TETRA_SDS_SHOW_MS.
     */
    if (tetraSds_count() > 0 &&
        (tetraSds_unread() > 0 || now - ts.sdsTime < TETRA_SDS_SHOW_MS)) {
        char text[96];
        char msg[14];
        snprintf(text, sizeof(text), "%s   ",
                 tetraSds_line(sds, sizeof(sds), now));
        scrollTextPeek(text, msg, sizeof(msg),
                       ui_state->m17_meta_text_scroll_position);
        if (now - ui_state->m17_meta_text_last_scroll_tick >= 100) {
            scrollTextAdvance(text, sizeof(msg),
                              &ui_state->m17_meta_text_scroll_position);
            ui_state->m17_meta_text_last_scroll_tick = now;
        }
        /* Wide characters: show fewer of them */
        size_t len = strlen(msg);
        while (len > 1
               && gfx_getTextWidth(layout.line1_font, msg)
                      > CONFIG_SCREEN_WIDTH - 2 * pad)
            msg[--len] = '\0';
        gfx_print(layout.line1_pos, layout.line1_font, TEXT_ALIGN_CENTER,
                  color_white, "%s", msg);
        return;
    }

    // Phase error, carrier offset, lost sample blocks, resyncs
    ui_state->m17_meta_text_scroll_position = 0;
    char a[40], b[40], c[24], d[8];
    snprintf(a, sizeof(a), "e%u%% %+dHz o%u r%u", ts.phaseErr, ts.carrierHz,
             ts.overruns, ts.resyncs);
    snprintf(b, sizeof(b), "e%u%% %+d o%u r%u", ts.phaseErr, ts.carrierHz,
             ts.overruns, ts.resyncs);
    snprintf(c, sizeof(c), "e%u%% %+dHz", ts.phaseErr, ts.carrierHz);
    snprintf(d, sizeof(d), "e%u%%", ts.phaseErr);
    const char *demod[] = { a, b, c, d };
    _ui_printFit(layout.line1_pos, layout.line1_font, pad, demod, 4);
}
#endif

/**
 * Check whether the current mode shows a call instead of the frequency.
 */
static bool _ui_callShown(void)
{
#ifdef CONFIG_M17
    rtxStatus_t status = rtx_getCurrentStatus();
    if ((status.opMode == OPMODE_M17) && status.lsfOk)
        return true;
#endif
#ifdef CONFIG_TETRA
    if (last_state.channel.mode == OPMODE_TETRA) {
        struct tetraStatus ts;
        tetra_getStatus(&ts);
        return _ui_tetraCallShown(&ts);
    }
#endif
    return false;
}

void _ui_drawMainBackground()
{
    // Print top bar line of hline_h pixel height
    gfx_drawHLine(layout.top_h, layout.hline_h, color_grey);
    // Print bottom bar line of 1 pixel height
    gfx_drawHLine(CONFIG_SCREEN_HEIGHT - layout.bottom_h - 1, layout.hline_h, color_grey);
}

void _ui_drawMainTop(ui_state_t * ui_state)
{
    /*
     * Short mode name on the left, in the one place every main screen shares,
     * so the mode reads the same in every mode's screen. The label is the mode
     * registry's (ui/ui_modes.h) and it is measured before it is drawn: the
     * left quarter of the bar belongs to it and to the keypad lock, and the
     * clock in the middle is never overlapped, whatever a future mode is
     * called.
     */
    char mode[UI_MODE_LABEL_LEN + 1];
    const char *label = ui_mode_label(last_state.channel.mode);

    snprintf(mode, sizeof(mode), "%s", label);
    while ((strlen(mode) > 1)
           && (gfx_getTextWidth(FONT_SIZE_6PT, mode) > UI_MODE_LABEL_MAX_W))
        mode[strlen(mode) - 1] = '\0';

    point_t mode_pos = { layout.horizontal_pad, layout.top_pos.y };
    gfx_print(mode_pos, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, yellow_fab413,
              "%s", mode);

    uint16_t mode_w = gfx_getTextWidth(FONT_SIZE_6PT, mode);

#ifdef CONFIG_RTC
    // Print clock on top bar
    datetime_t local_time = utcToLocalTime(last_state.time,
                                           last_state.settings.utc_timezone);
    gfx_print(layout.top_pos, layout.top_font, TEXT_ALIGN_CENTER,
              color_white, "%02d:%02d:%02d", local_time.hour,
              local_time.minute, local_time.second);
#endif
    // If the radio has no built-in battery, print input voltage
#ifdef CONFIG_BAT_NONE
    gfx_print(layout.top_pos, layout.top_font, TEXT_ALIGN_RIGHT,
              color_white,"%u.%uV", last_state.v_bat/1000, (last_state.v_bat % 1000) /100);
#else
    if(last_state.settings.showBatteryIcon) {
        // print battery icon on top bar, use 4 px padding
        uint16_t bat_width = CONFIG_SCREEN_WIDTH / 9;
        uint16_t bat_height = layout.top_h - (layout.status_v_pad * 2);
        point_t bat_pos = {CONFIG_SCREEN_WIDTH - bat_width - layout.horizontal_pad,
                        layout.status_v_pad};
        gfx_drawBattery(bat_pos, bat_width, bat_height, last_state.charge);
    } else {
        // print the battery percentage
        point_t bat_pos = {layout.top_pos.x, layout.top_pos.y - 2};
        gfx_print(bat_pos , FONT_SIZE_6PT, TEXT_ALIGN_RIGHT,
        color_white,"%d%%", last_state.charge);
    }
#endif
    if (ui_state->input_locked == true)
    {
        point_t lock_pos = { (int16_t)(layout.top_pos.x + mode_w + 3),
                             layout.top_pos.y };

        /* Right of the mode label, so the two never draw over each other */
        if (lock_pos.x > (int16_t)(CONFIG_SCREEN_WIDTH - 16))
            lock_pos.x = layout.top_pos.x;

        gfx_drawSymbol(lock_pos, layout.top_symbol_size, TEXT_ALIGN_LEFT,
                       color_white, SYMBOL_LOCK);
    }
}

void _ui_drawBankChannel()
{
    // Print Bank number, channel number and Channel name
    uint16_t b = (last_state.bank_enabled) ? last_state.bank : 0;
    gfx_print(layout.line1_pos, layout.line1_font, TEXT_ALIGN_CENTER,
              color_white, "%01d-%03d: %.12s",
              b, last_state.channel_index + 1, last_state.channel.name);
}

const char* _ui_getToneEnabledString(bool tone_tx_enable, bool tone_rx_enable,
        bool use_abbreviation)
{
    const char *strings[2][4] = {
        {
            currentLanguage->None,
            currentLanguage->Encode,
            currentLanguage->Decode,
            currentLanguage->Both,
        }, {
            "N",
            "T",
            "R",
            "B"
        }
    };


    uint8_t index = (tone_rx_enable << 1) | (tone_tx_enable);
    return strings[use_abbreviation][index];
}

#ifdef CONFIG_DMR
/**
 * DMR colour code, timeslot and call state, on line 2 (replacing the channel
 * mode line) and line 1 (where TETRA puts its demodulator figures).
 */
static void _ui_drawDmrInfo(void)
{
    struct dmrStatus ds;
    dmr_getStatus(&ds);
    const uint16_t pad = layout.horizontal_pad;
    char a[32];

    if (!ds.sync) {
        const char *searching[] = { "DMR searching" };
        _ui_printFit(layout.line2_pos, layout.line2_font, pad, searching, 1);
        return;
    }

    if (ds.colourCode >= 0)
        snprintf(a, sizeof(a), "CC%u TS%d", ds.colourCode,
                 (ds.timeslot > 0) ? ds.timeslot : 0);
    else
        snprintf(a, sizeof(a), "CC- TS%d", (ds.timeslot > 0) ? ds.timeslot : 0);

    const char *info[] = { a };
    _ui_printFit(layout.line2_pos, layout.line2_font, pad, info, 1);
}

/**
 * Line 1 in DMR: what the last burst was, and how open its eye is.
 */
static void _ui_drawDmrLine1(void)
{
    struct dmrStatus ds;
    dmr_getStatus(&ds);
    const uint16_t pad = layout.horizontal_pad;
    char a[40], b[40], c[24];

    if (!ds.sync)
        return;

    const char *state = ds.voice ? "Voice" : "Data";
    if (!ds.voice && (ds.lastType >= 0))
        state = dmr_data_type_name((uint8_t)ds.lastType);

    snprintf(a, sizeof(a), "%s e%u%%", state, (unsigned)(ds.evm / 10));
    snprintf(b, sizeof(b), "e%u%%", (unsigned)(ds.evm / 10));
    snprintf(c, sizeof(c), "%s", state);
    const char *texts[] = { a, b, c };
    _ui_printFit(layout.line1_pos, layout.line1_font, pad, texts, 3);
}
#endif

#ifdef UI_MODE_NXDN
/*
 * NXDN line 2: the RAN the receiver locked to and the demodulator's state, or
 * SRCH while nothing decodes. Line 1 carries the counters that matter when the
 * eye is marginal: EVM and decoded frames.
 */
static void _ui_drawNxdnInfo(void)
{
    struct nxdnStatus st;
    const uint16_t pad = layout.horizontal_pad;
    char a[32], b[32], c[24];

    nxdn_getStatus(&st);

    if (!st.sync) {
        const char *searching[] = { "NXDN SRCH", "SRCH" };
        _ui_printFit(layout.line2_pos, layout.line2_font, pad, searching, 2);
        return;
    }

    if (st.ran >= 0) {
        snprintf(a, sizeof(a), "RAN%u %.2s", (unsigned)st.ran,
                 st.voice ? "VOX" : "");
        snprintf(b, sizeof(b), "RAN%u", (unsigned)st.ran);
    } else {
        snprintf(a, sizeof(a), "SYNC %.2s", st.voice ? "VOX" : "");
        snprintf(b, sizeof(b), "SYNC");
    }
    snprintf(c, sizeof(c), "%s", st.voice ? "VOX" : "SYNC");

    const char *texts[] = { a, b, c };
    _ui_printFit(layout.line2_pos, layout.line2_font, pad, texts, 3);
}

static void _ui_drawNxdnLine1(void)
{
    struct nxdnStatus st;
    const uint16_t pad = layout.horizontal_pad;
    char a[40], b[40], c[24];

    nxdn_getStatus(&st);

    if (!st.sync)
        return;

    snprintf(a, sizeof(a), "e%u%% f%lu", (unsigned)(st.evm / 10),
             (unsigned long)st.frames);
    snprintf(b, sizeof(b), "e%u%%", (unsigned)(st.evm / 10));
    snprintf(c, sizeof(c), "f%lu", (unsigned long)st.frames);

    const char *texts[] = { a, b, c };
    _ui_printFit(layout.line1_pos, layout.line1_font, pad, texts, 3);
}
#endif

#ifdef UI_MODE_AIRBAND
/*
 * Airband line 2: the mode, the raster the operator chose and the designator of
 * the channel the receiver is on ("118.005" is the channel whose carrier is
 * 118.00833). Line 1 shows the AGC state and whether the carrier squelch is
 * open, which in AM is the only "is anyone there" indication there is.
 */
static void _ui_drawAirbandInfo(void)
{
    const uint16_t pad = layout.horizontal_pad;
    char name[AIRBAND_NAME_LEN] = "";
    char a[40], b[40], c[24];
    const char *raster = last_state.settings.am_step ? "8.33" : "25";

    if (airband_inBand(last_state.channel.rx_frequency))
        airband_channelName(last_state.channel.rx_frequency, name,
                            sizeof(name));

    if (name[0] != '\0')
        snprintf(a, sizeof(a), "AM %s %s", raster, name);
    else
        snprintf(a, sizeof(a), "AM %s", raster);
    snprintf(b, sizeof(b), "%s", name[0] != '\0' ? name : raster);
    snprintf(c, sizeof(c), "AM");

    const char *texts[] = { a, b, c };
    _ui_printFit(layout.line2_pos, layout.line2_font, pad, texts, 3);
}

static void _ui_drawAirbandLine1(void)
{
    const uint16_t pad = layout.horizontal_pad;
    char a[32], b[24], c[16];
    const char *agc = last_state.settings.am_agc ? "AGC" : "AGC-";

    snprintf(a, sizeof(a), "%s sql%u", agc,
             (unsigned)last_state.settings.sqlLevel);
    snprintf(b, sizeof(b), "%s", agc);
    snprintf(c, sizeof(c), "sql%u", (unsigned)last_state.settings.sqlLevel);

    const char *texts[] = { a, b, c };
    _ui_printFit(layout.line1_pos, layout.line1_font, pad, texts, 3);
}
#endif

void _ui_drawModeInfo(ui_state_t* ui_state)
{
    char bw_str[8] = { 0 };

    switch(last_state.channel.mode)
    {
        case OPMODE_FM:
        {
#ifdef CONFIG_APRS
            /*
             * APRS indicator: replaces the bandwidth line while decoding is
             * active, so the operator can see at a glance that the receiver
             * is decoding and whether anything arrived.
             */
            char aprs_line[16] = { 0 };
            ui_aprs_statusLine(aprs_line, sizeof(aprs_line));
            if (aprs_line[0] != '\0')
            {
                gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          yellow_fab413, "%s", aprs_line);
                break;
            }
#endif
            // Get Bandwidth string
            if(last_state.channel.bandwidth == BW_12_5)
                sniprintf(bw_str, 8, "NFM");
            else if(last_state.channel.bandwidth == BW_25)
                sniprintf(bw_str, 8, "FM");

            // Get encdec string
            bool tone_tx_enable = last_state.channel.fm.txToneEn;
            bool tone_rx_enable = last_state.channel.fm.rxToneEn;
            // Print Bandwidth, Tone and encdec info
            if (tone_tx_enable || tone_rx_enable)
            {
                uint16_t tone = ctcss_tone[last_state.channel.fm.txTone];
                gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "%s %d.%d %s", bw_str, (tone / 10),
                          (tone % 10), _ui_getToneEnabledString(tone_tx_enable, tone_rx_enable, true));
            }
            else
            {
                gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "%s", bw_str );
            }
            break;
        }

#ifdef CONFIG_DMR
        case OPMODE_DMR:
            _ui_drawDmrInfo();
            break;
#endif

        #ifdef CONFIG_M17
        case OPMODE_M17:
        {
            // Print M17 Destination ID on line 3 of 3
            rtxStatus_t rtxStatus = rtx_getCurrentStatus();

            if(rtxStatus.lsfOk)
            {
                // Destination address
                gfx_drawSymbol(layout.line2_pos, layout.line2_symbol_size, TEXT_ALIGN_LEFT,
                               color_white, SYMBOL_CALL_RECEIVED);

                gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "%s", rtxStatus.M17_dst);

                // Source address
                gfx_drawSymbol(layout.line1_pos, layout.line1_symbol_size, TEXT_ALIGN_LEFT,
                               color_white, SYMBOL_CALL_MADE);

                gfx_print(layout.line1_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "%s", rtxStatus.M17_src);

                // RF link (if present)
                if(rtxStatus.M17_link[0] != '\0')
                {
                    gfx_drawSymbol(layout.line4_pos, layout.line3_symbol_size, TEXT_ALIGN_LEFT,
                                   color_white, SYMBOL_ACCESS_POINT);

                    gfx_print(layout.line4_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                              color_white, "%s", rtxStatus.M17_link);
                }

                // Meta text (if present)
                if(rtxStatus.M17_meta_text[0] != '\0')
                {
                    char msg[14];
                    
                    // Always display current position
                    scrollTextPeek(rtxStatus.M17_meta_text, msg, sizeof(msg),
                                   ui_state->m17_meta_text_scroll_position);
                    // Only advance scroll every 100ms
                    long long now = getTick();
                    if(now - ui_state->m17_meta_text_last_scroll_tick >= 100)
                    {
                        scrollTextAdvance(rtxStatus.M17_meta_text, sizeof(msg),
                                          &ui_state->m17_meta_text_scroll_position);
                        ui_state->m17_meta_text_last_scroll_tick = now;
                    }
                    gfx_print(layout.line3_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                              color_white, "%s", msg);
                }
                // Reflector (if present)
                else if(rtxStatus.M17_refl[0] != '\0')
                {                    
                    gfx_drawSymbol(layout.line3_pos, layout.line4_symbol_size, TEXT_ALIGN_LEFT,
                                   color_white, SYMBOL_NETWORK);

                    gfx_print(layout.line3_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                              color_white, "%s", rtxStatus.M17_refl);
                }
                
                // Reset scroll position when meta text becomes empty
                if(rtxStatus.M17_meta_text[0] == '\0')
                {
                    ui_state->m17_meta_text_scroll_position = 0;
                }
            }
            else
            {
                const char *dst = NULL;
                if(ui_state->edit_mode)
                {
                    dst = ui_state->new_callsign;
                }
                else
                {
                    if(strnlen(rtxStatus.destination_address, 10) == 0)
                        dst = currentLanguage->broadcast;
                    else
                        dst = rtxStatus.destination_address;
                }

                gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "M17 #%s", dst);
            }
            break;
        }
        #endif

        #ifdef CONFIG_TETRA
        case OPMODE_TETRA:
            (void) ui_state;
            _ui_drawTetraInfo();
            break;
        #endif

        #ifdef UI_MODE_NXDN
        case OPMODE_NXDN:
            (void) ui_state;
            _ui_drawNxdnInfo();
            break;
        #endif

        #ifdef UI_MODE_AIRBAND
        case OPMODE_AIRBAND:
            (void) ui_state;
            _ui_drawAirbandInfo();
            break;
        #endif

        default:
            /*
             * A mode this build has but no line for: say its name instead of
             * leaving the line blank, so the operator is never looking at an
             * empty screen wondering whether the radio is in a mode at all.
             */
            gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                      color_white, "%s",
                      ui_mode_title(last_state.channel.mode));
            break;
    }
}

void _ui_drawFrequency()
{
    freq_t freq = platform_getPttStatus() ? last_state.channel.tx_frequency
                                          : last_state.channel.rx_frequency;

    char freq_str[16] = {0};
    sniprintf(freq_str, sizeof(freq_str), "%03lu.%05lu",
              (freq / 1000000lu), (freq % 1000000lu) / 10);

    size_t len = strlen(freq_str);
    char main_str[16] = {0};
    char small_str[3] = {0};
    strncpy(main_str, freq_str, len - 2);
    strncpy(small_str, freq_str + len - 2, 2);

    fontSize_t small_font = FONT_SIZE_5PT;
    if (layout.line3_large_font > FONT_SIZE_6PT)
    {
        small_font = (fontSize_t)(layout.line3_large_font - 2);
    }

    uint16_t main_width = gfx_getTextWidth(layout.line3_large_font, main_str);
    uint16_t small_width = gfx_getTextWidth(small_font, small_str);
    uint16_t total_width = main_width + small_width;
    int16_t start_x = (CONFIG_SCREEN_WIDTH - total_width) / 2;

    point_t main_pos = { (uint16_t)start_x, layout.line3_large_pos.y };
    gfx_print(main_pos, layout.line3_large_font, TEXT_ALIGN_LEFT,
              color_white, "%s", main_str);

    point_t small_pos = { (uint16_t)(start_x + main_width), layout.line3_large_pos.y };
    gfx_print(small_pos, small_font, TEXT_ALIGN_LEFT,
              color_white, "%s", small_str);
}

void _ui_drawVFOMiddleInput(ui_state_t* ui_state)
{
    /*
     * Eight digits, 10 Hz resolution: "Rx:446.00625". The dot sits after the
     * third digit, so the insertion point skips it.
     */
    uint8_t insert_pos = ui_state->input_position + 3;
    if(ui_state->input_position > 3) insert_pos += 1;
    char input_char = ui_state->input_number + '0';
    bool rxOnly = (opmode_canTransmit(last_state.channel.mode) == false);

    if(rxOnly)
    {
        /*
         * Receive-only mode: one frequency, no TX field. Drawing it alone in
         * the middle of the screen also tells the user that ENTER (or the
         * eighth digit) is all it takes.
         */
        if(ui_state->input_position == 0)
        {
            gfx_print(layout.line3_large_pos, layout.input_font,
                      TEXT_ALIGN_CENTER, color_white, ">Rx:%03lu.%05lu",
                      (unsigned long)ui_state->new_rx_frequency/1000000,
                      (unsigned long)(ui_state->new_rx_frequency%1000000)/10);
        }
        else
        {
            if(ui_state->input_position == 1)
                strcpy(ui_state->new_rx_freq_buf, ">Rx:___._____");
            ui_state->new_rx_freq_buf[insert_pos] = input_char;
            gfx_print(layout.line3_large_pos, layout.input_font,
                      TEXT_ALIGN_CENTER, color_white, "%s",
                      ui_state->new_rx_freq_buf);
        }
        return;
    }

    if(ui_state->input_set == SET_RX)
    {
        if(ui_state->input_position == 0)
        {
            gfx_print(layout.line2_pos, layout.input_font, TEXT_ALIGN_CENTER,
                      color_white, ">Rx:%03lu.%05lu",
                      (unsigned long)ui_state->new_rx_frequency/1000000,
                      (unsigned long)(ui_state->new_rx_frequency%1000000)/10);
        }
        else
        {
            // Replace Rx frequency with underscorses
            if(ui_state->input_position == 1)
                strcpy(ui_state->new_rx_freq_buf, ">Rx:___._____");
            ui_state->new_rx_freq_buf[insert_pos] = input_char;
            gfx_print(layout.line2_pos, layout.input_font, TEXT_ALIGN_CENTER,
                      color_white, "%s", ui_state->new_rx_freq_buf);
        }
        gfx_print(layout.line3_large_pos, layout.input_font, TEXT_ALIGN_CENTER,
                  color_white, " Tx:%03lu.%05lu",
                  (unsigned long)last_state.channel.tx_frequency/1000000,
                  (unsigned long)(last_state.channel.tx_frequency%1000000)/10);
    }
    else if(ui_state->input_set == SET_TX)
    {
        gfx_print(layout.line2_pos, layout.input_font, TEXT_ALIGN_CENTER,
                  color_white, " Rx:%03lu.%05lu",
                  (unsigned long)ui_state->new_rx_frequency/1000000,
                  (unsigned long)(ui_state->new_rx_frequency%1000000)/10);
        // Replace Rx frequency with underscorses
        if(ui_state->input_position == 0)
        {
            gfx_print(layout.line3_large_pos, layout.input_font, TEXT_ALIGN_CENTER,
                      color_white, ">Tx:%03lu.%05lu",
                      (unsigned long)ui_state->new_rx_frequency/1000000,
                      (unsigned long)(ui_state->new_rx_frequency%1000000)/10);
        }
        else
        {
            if(ui_state->input_position == 1)
                strcpy(ui_state->new_tx_freq_buf, ">Tx:___._____");
            ui_state->new_tx_freq_buf[insert_pos] = input_char;
            gfx_print(layout.line3_large_pos, layout.input_font, TEXT_ALIGN_CENTER,
                      color_white, "%s", ui_state->new_tx_freq_buf);
        }
    }
}

void _ui_drawMainBottom()
{
    // Squelch bar
    rssi_t   rssi = last_state.rssi;
    uint8_t  squelch = last_state.settings.sqlLevel;
    uint8_t  volume = last_state.volume;
    uint16_t meter_width = CONFIG_SCREEN_WIDTH - 2 * layout.horizontal_pad;
    uint16_t meter_height = layout.bottom_h;
    point_t meter_pos = { layout.horizontal_pad,
                          CONFIG_SCREEN_HEIGHT - meter_height - layout.bottom_pad};
    uint8_t mic_level = platform_getMicLevel();
    switch(last_state.channel.mode)
    {
        case OPMODE_FM:
            gfx_drawSmeter(meter_pos,
                           meter_width,
                           meter_height,
                           rssi,
                           squelch,
                           volume,
                           true,
                           yellow_fab413);
            break;
        default:
            /*
             * Every other mode gets the level meter: FM is the only one whose
             * bar also shows the squelch threshold. A mode that was never
             * listed here used to draw nothing at all, which is what NXDN and
             * AM looked like before they were registered.
             */
            gfx_drawSmeterLevel(meter_pos,
                                meter_width,
                                meter_height,
                                rssi,
                                mic_level,
                                volume,
                                true);
            break;
    }
}

void _ui_drawMainVFO(ui_state_t* ui_state)
{
    gfx_clearScreen();
    _ui_drawMainTop(ui_state);
    _ui_drawModeInfo(ui_state);

    // Show VFO frequency unless the mode shows a call (M17 LSF, TETRA call)
    if(_ui_callShown() == false)
        _ui_drawFrequency();

#ifdef CONFIG_DMR
    if(last_state.channel.mode == OPMODE_DMR)
        _ui_drawDmrLine1();
#endif

    #ifdef CONFIG_TETRA
    if(last_state.channel.mode == OPMODE_TETRA)
        _ui_drawTetraLine1(ui_state);
    #endif

    #ifdef UI_MODE_NXDN
    if(last_state.channel.mode == OPMODE_NXDN)
        _ui_drawNxdnLine1();
    #endif

    #ifdef UI_MODE_AIRBAND
    if(last_state.channel.mode == OPMODE_AIRBAND)
        _ui_drawAirbandLine1();
    #endif

    _ui_drawMainBottom();
}

void _ui_drawMainVFOInput(ui_state_t* ui_state)
{
    gfx_clearScreen();
    _ui_drawMainTop(ui_state);
    _ui_drawVFOMiddleInput(ui_state);
    _ui_drawMainBottom();
}

void _ui_drawMainMEM(ui_state_t* ui_state)
{
    gfx_clearScreen();
    _ui_drawMainTop(ui_state);
    _ui_drawModeInfo(ui_state);

    // Show channel data unless the mode shows a call (M17 LSF, TETRA call)
    if(_ui_callShown() == false)
    {
        _ui_drawBankChannel();
        _ui_drawFrequency();
    }

    #ifdef UI_MODE_NXDN
    if(last_state.channel.mode == OPMODE_NXDN)
        _ui_drawNxdnLine1();
    #endif

    _ui_drawMainBottom();
}
