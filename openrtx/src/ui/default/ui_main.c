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
#ifdef CONFIG_KISS
#include "ui/ui_packet.h"
#endif
#include <string.h>
#include "ui/ui_mode_state.h"
#include "ui/ui_strings.h"
#include "core/utils.h"
#include "ui/utils.h"
#ifdef UI_MODE_NXDN
#include "protocols/NXDN/NxdnStatus.h"
#endif
#ifdef UI_MODE_P25
#include "protocols/P25/P25Status.h"
#endif
#ifdef UI_MODE_DSTAR
#include "protocols/DSTAR/DStarStatus.h"
#endif
/*
 * The DMR screens read the demodulator's status, which the host builds link as
 * a stub (platform/targets/linux/emulator/ui_mode_status.c), so they are keyed
 * on the UI flag like NXDN's, P25's and D-STAR's above: the simulator draws
 * them, and the firmware and the host build show the same screen.
 */
#ifdef UI_MODE_DMR
#include "protocols/DMR/DmrDefs.h"
#include "protocols/DMR/DmrStatus.h"
#endif
#ifdef UI_MODE_TETRA
#include "protocols/TETRA/TetraStatus.h"
#endif
#ifdef CONFIG_TETRA
/* The SDS texts are kept in the storage partition: a radio-only service */
#include "protocols/TETRA/TetraSds.h"
#endif
#ifdef CONFIG_VFO_SEARCH
#include "core/vfo_search.h"
#include "core/vfo_search_service.h"
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
 * @param color: ink colour; a warning passes its own so that it does not read
 *               as the value it replaces.
 */
static void _ui_printFitColor(point_t pos, fontSize_t font, uint16_t left,
                              const char *const *texts, size_t n, color_t color)
{
    uint16_t avail = 0;
    if (CONFIG_SCREEN_WIDTH > 2 * left)
        avail = CONFIG_SCREEN_WIDTH - 2 * left;

    size_t i = 0;
    while (i + 1 < n && gfx_getTextWidth(font, texts[i]) > avail)
        i++;
    gfx_print(pos, font, TEXT_ALIGN_CENTER, color, "%s", texts[i]);
}

static void _ui_printFit(point_t pos, fontSize_t font, uint16_t left,
                         const char *const *texts, size_t n)
{
    _ui_printFitColor(pos, font, left, texts, n, color_white);
}

/** Print the same, in colour: a warning is not the reading it replaces. */
static void _ui_printFitWarn(point_t pos, fontSize_t font, uint16_t left,
                             const char *const *texts, size_t n)
{
    _ui_printFitColor(pos, font, left, texts, n, yellow_fab413);
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

#ifdef UI_MODE_TETRA
/**
 * Check whether a TETRA call takes over the main screen.
 */
static bool _ui_tetraCallShown(const struct tetraStatus *ts)
{
    return ts->sync && ts->call;
}

/**
 * TETRA call information, from line 1 to line 4 during a call.
 *
 * A call takes the screen over (symbols, talker, called group, cell, call
 * state); when there is none the shared state block draws the cell and the
 * registration (line 2 and line 4) and _ui_drawTetraLine1() keeps line 1 for
 * the demodulator figures, the uplink and the SDS texts.
 */
static void _ui_drawTetraInfo(void)
{
    struct tetraStatus ts;
    tetra_getStatus(&ts);
    const uint16_t pad = layout.horizontal_pad;
    char a[32], b[32];

    if (!_ui_tetraCallShown(&ts))
        return;

    /*
     * Talking party. An uplink call is an MS transmitting, not the network: it
     * is marked as such and its own SSI is the sending one.
     */
    uint32_t ssi = (ts.talker != 0) ? ts.talker : ts.uplinkSs;
    _ui_formatSsi(a, sizeof(a), ts.uplink ? "UL " : "", ssi);
    const char *talker[] = { a };
    _ui_printWithSymbol(layout.line1_pos, layout.line1_symbol_size,
                        SYMBOL_CALL_MADE, layout.line1_font, talker, 1);

    // Called group or individual
    _ui_formatSsi(a, sizeof(a), ts.callGroup ? "G " : "", ts.callDst);
    const char *called[] = { a };
    _ui_printWithSymbol(layout.line2_pos, layout.line2_symbol_size,
                        SYMBOL_CALL_RECEIVED, layout.line2_font, called, 1);

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
        /*
         * The state line, with the timeslot the call is being heard in when
         * there is one: a TETRA frame has four and the call sits in one of
         * them, so the number says which.  Three forms, widest first, because
         * the line is 160 px and the cell above already takes its own.
         */
        const char *voice = ts.voice ? "Voice" : "Idle";
        char tn[16] = "";
        if (ts.tn >= 1 && ts.tn <= 4)
            snprintf(tn, sizeof(tn), " TN%u", (unsigned)ts.tn);

        char full[32], mid[32];
        snprintf(full, sizeof(full), "%s, call %u%s", voice, ts.callId, tn);
        snprintf(mid, sizeof(mid), "%s%s", voice, tn);
        const char *state[] = { full, mid, voice };
        const int n = (ts.callId != 0) ? 3 : 2;
        _ui_printFit(layout.line4_pos, layout.line4_font, pad, state, n);
    }
}

#endif /* UI_MODE_TETRA */

#ifdef CONFIG_TETRA
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
     * An MS heard on the uplink: the sending SSI, for the few seconds the
     * status keeps the flag (TetraStatus.c, UPLINK_TIMEOUT_MS). It goes first
     * because it is the event the operator is listening for.
     */
    if (ts.uplink) {
        char ul[32];

        _ui_formatSsi(ul, sizeof(ul), "UL ", ts.uplinkSs);
        gfx_print(layout.line1_pos, layout.line1_font, TEXT_ALIGN_CENTER,
                  color_white, "%s", ul);
        return;
    }

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
#ifdef UI_MODE_TETRA
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
    gfx_print(gfx_right_edge(layout.top_pos), layout.top_font,
              TEXT_ALIGN_RIGHT, color_white,
              "%u.%uV", last_state.v_bat/1000, (last_state.v_bat % 1000) /100);
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
        gfx_print(gfx_right_edge(bat_pos), FONT_SIZE_6PT, TEXT_ALIGN_RIGHT,
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

/*
 * ---------------------------------------------------------------------------
 * The mode's decoded state, in one place
 * ---------------------------------------------------------------------------
 *
 * Every receive mode used to publish what it had decoded as read-only rows of
 * its quick menu - which is also that mode's Settings page - so the operator
 * had to open a menu to see whether the radio had locked, and the page was a
 * list of rows that could not be actuated. The state is now drawn here, on the
 * main screen, from one block (ui_mode_state.h) that every mode fills the same
 * way, so it reads the same in all of them and there is a single place to fix
 * when a protocol changes what it publishes.
 *
 * The layout is two lines of the existing bands, and only those:
 *
 *   line 1  the call and the quality      "2317>9 LC98%", "e4% f1234"
 *   line 2  the identifiers, left          "CC1 TS2", "RAN12 VOX", "T 228-8889
 *           and the secondary state, right LA 1" ... "ACC G3 NB4 L2"
 *
 * Line 2 is a status line in the usual sense - what the receiver is locked to
 * on the left, what else it has learnt on the right - because the C62 has no
 * free row below the frequency: the 16 pt digits of line 3 reach down to y=93
 * and `layout.line4_pos` is y=97, so a fourth line of the state block would be
 * drawn over the frequency (seen in the simulator, docs/UI.md). Both halves are
 * measured before they are drawn: the note is dropped, never shortened over the
 * identifiers, when the two would collide.
 *
 * The frequency keeps the big 16 pt row and the bottom row stays the S-meter
 * (and the carrier-search message), so nothing moves between modes: the same
 * baselines, from the same layout_t, in all of them.
 */

/**
 * Draw one line from a primary and a secondary text.
 *
 * The full "primary secondary", then the primary alone, then the secondary
 * alone: the first that fits the panel is printed, and if none does the
 * primary is shortened by _ui_printFit (which picks the shortest candidate it
 * was given). gfx_print() does not clip, so a text that is not measured first
 * wraps onto the line below and draws over the frequency.
 */
static void _ui_drawStateLine(point_t pos, fontSize_t font, const char *first,
                              const char *second)
{
    char both[(2 * UI_MODE_STATE_LEN) + 2];
    const char *texts[3];
    size_t n = 0;

    if ((first != NULL) && (first[0] != '\0') && (second != NULL)
        && (second[0] != '\0')) {
        snprintf(both, sizeof(both), "%s %s", first, second);
        texts[n++] = both;
    }
    if ((first != NULL) && (first[0] != '\0'))
        texts[n++] = first;
    if ((second != NULL) && (second[0] != '\0'))
        texts[n++] = second;

    if (n == 0)
        return;

    _ui_printFit(pos, font, layout.horizontal_pad, texts, n);
}

/**
 * The state block of the current mode: line 1 (call and quality) and line 2
 * (identifiers on the left, secondary state on the right).
 */
static void _ui_drawModeState(void)
{
    const int16_t left = (int16_t)layout.horizontal_pad;
    const int16_t right = (int16_t)(CONFIG_SCREEN_WIDTH - layout.horizontal_pad);
    /* Air between the identifiers and the note, so the two read as two */
    const int16_t gap = 6;
    ui_mode_state_t st;

    ui_mode_state_read(&st);

    _ui_drawStateLine(layout.line1_pos, layout.line1_font, st.callTxt, st.qual);

    if (st.id[0] == '\0')
        return;

    gfx_print((point_t){ left, layout.line2_pos.y }, layout.line2_font,
              TEXT_ALIGN_LEFT, color_white, "%s", st.id);

    const int16_t id_end = (int16_t)(left
                                     + gfx_getTextWidth(layout.line2_font,
                                                        st.id));

    /*
     * The note is secondary: it takes the short form when the long one does not
     * fit, and gives up its place when neither does. The identifiers never do.
     */
    const char *note = (st.note[0] != '\0') ? st.note : st.noteShort;

    if ((note == NULL) || (note[0] == '\0'))
        return;

    if ((id_end + gap
         + (int16_t)gfx_getTextWidth(layout.line2_font, note)) > right) {
        note = st.noteShort;

        if ((note == NULL) || (note[0] == '\0'))
            return;

        if ((id_end + gap
             + (int16_t)gfx_getTextWidth(layout.line2_font, note)) > right)
            return;
    }

    gfx_print((point_t){ right, layout.line2_pos.y }, layout.line2_font,
              TEXT_ALIGN_RIGHT, color_white, "%s", note);
}

#ifdef UI_MODE_DMR
/**
 * DMR: without an own DMR ID the transmitter does not key (rtx/DmrKey.h) and
 * the main screen says so instead of the colour code. That warning is state
 * too, and the most important one in the mode: a radio that silently refuses
 * the PTT is what docs/TX_SAFETY.md exists to prevent. The row that fixes it is
 * Settings -> Identity.
 */
static void _ui_drawDmrWarning(void)
{
    if (settings_dmrIdValid(state.settings.dmr_id))
        return;

    const char *noTx[] = { "NO TX: DMR ID 0", "NO TX: NO ID", "NO TX" };
    _ui_printFitWarn(layout.line2_pos, layout.line2_font,
                     layout.horizontal_pad, noTx, 3);
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
    char a[40], b[28], c[20];
    const char *agc = last_state.settings.am_agc ? "AGC" : "AGC-";

#ifdef CONFIG_C62_AIR_TX
    /*
     * The transmit state of the band is not a menu away: it is on the main
     * screen, next to the squelch, for as long as the mode is selected. LOCK
     * means the first PTT raises the warning; ARM means PTT transmits.
     */
    const char *tx = (airband_txState() == AIRBAND_TX_ARMED) ? "TX-ARM" : "TX-LOCK";

    snprintf(a, sizeof(a), "%s %s sql%u", tx, agc,
             (unsigned)last_state.settings.sqlLevel);
    snprintf(b, sizeof(b), "%s %s", tx, agc);
    snprintf(c, sizeof(c), "%s", tx);
#else
    snprintf(a, sizeof(a), "%s sql%u", agc,
             (unsigned)last_state.settings.sqlLevel);
    snprintf(b, sizeof(b), "%s", agc);
    snprintf(c, sizeof(c), "sql%u", (unsigned)last_state.settings.sqlLevel);
#endif

    const char *texts[] = { a, b, c };
    _ui_printFit(layout.line1_pos, layout.line1_font, pad, texts, 3);
}

#ifdef CONFIG_C62_AIR_TX
/**
 * The warning that stands between the operator and a transmission in the
 * aeronautical band.
 *
 * It covers the main screen while the mode waits for the confirmation: the
 * first press of the PTT opened it and keyed nothing, a second press keys, and
 * if nobody presses anything the mode locks itself again when the prompt
 * expires. The text says what the band is and who answers for a transmission in
 * it, because that is the whole point of the gate.
 */
static void _ui_drawAirbandTxPrompt(void)
{
    const color_t warn = { 0xFF, 0x30, 0x30, 0xFF };
    const uint16_t boxX = 6;
    const uint16_t boxY = (uint16_t)(layout.line1_pos.y - 26);
    const uint16_t boxW = (uint16_t)(CONFIG_SCREEN_WIDTH - 2 * boxX);
    const uint16_t boxH = 84;
    const point_t box = { boxX, boxY };
    point_t p = { (uint16_t)(CONFIG_SCREEN_WIDTH / 2), (uint16_t)(boxY + 4) };

    if (airband_txState() != AIRBAND_TX_PROMPT)
        return;

    gfx_drawRect(box, boxW, boxH, color_black, true);
    gfx_drawRect(box, boxW, boxH, warn, false);

    gfx_print(p, FONT_SIZE_8PT, TEXT_ALIGN_CENTER, warn, "AIRBAND TX");
    p.y += 14;
    gfx_print(p, FONT_SIZE_8PT, TEXT_ALIGN_CENTER, color_white,
              "118-137 MHz: licensed");
    p.y += 12;
    gfx_print(p, FONT_SIZE_8PT, TEXT_ALIGN_CENTER, color_white,
              "band, you are the one");
    p.y += 12;
    gfx_print(p, FONT_SIZE_8PT, TEXT_ALIGN_CENTER, color_white,
              "in command");
    p.y += 16;
    gfx_print(p, FONT_SIZE_8PT, TEXT_ALIGN_CENTER, yellow_fab413,
              "PTT again = transmit");
    p.y += 12;
    gfx_print(p, FONT_SIZE_8PT, TEXT_ALIGN_CENTER, color_white,
              "wait = locked");
}
#endif /* CONFIG_C62_AIR_TX */
#endif

#ifdef UI_MODE_DPMR
/*
 * dPMR line 2: whether a call is being followed. The mode's squelch opens with
 * the frame grid and the CCH CRC, so it is the only "is anyone there" flag the
 * engine has; dPMR has no per-frame counters published to the UI yet.
 */
#endif

#ifdef UI_MODE_P25
#endif

#ifdef UI_MODE_YSF
/*
 * System Fusion line 2: the frame layer publishes only "a carrier is being
 * tracked", so that is what the line says.
 */
#endif

#ifdef UI_MODE_DSTAR
#endif

#ifdef UI_MODE_WFM
/*
 * Broadcast FM line 2: the stereo state of the tuner and the channel designator
 * ("101.1"). Line 1 carries the level in dBuV and the chip's own SNR estimate.
 * There is no RDS on this hardware, so nothing else can be shown.
 */
static void _ui_drawWfmInfo(void)
{
    const uint16_t pad = layout.horizontal_pad;
    char label[16] = "";
    char a[40], b[40], c[24];
    const char *mode;

    if (!last_state.wfm.tuned)
        mode = "SRCH";
    else if (last_state.wfm.stereoSync)
        mode = "STEREO";
    else if (last_state.wfm.stereo)
        mode = "PILOT";
    else
        mode = "MONO";

    wfm_formatFreq(last_state.channel.rx_frequency, label, sizeof(label));

    snprintf(a, sizeof(a), "%s %s", mode, label);
    snprintf(b, sizeof(b), "%s", mode);
    snprintf(c, sizeof(c), "%s", label);

    const char *texts[] = { a, b, c };
    _ui_printFit(layout.line2_pos, layout.line2_font, pad, texts, 3);
}

static void _ui_drawWfmLine1(void)
{
    const uint16_t pad = layout.horizontal_pad;
    char a[32], b[24], c[16];

    if (!last_state.wfm.tuned)
        return;

    snprintf(a, sizeof(a), "%udBuV snr%u", (unsigned)last_state.wfm.rssi,
             (unsigned)last_state.wfm.snr);
    snprintf(b, sizeof(b), "%udBuV", (unsigned)last_state.wfm.rssi);
    snprintf(c, sizeof(c), "snr%u", (unsigned)last_state.wfm.snr);

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
#ifdef CONFIG_KISS
            /*
             * Packet modem: the same place, the same colour as the APRS
             * indicator, so "the receiver is decoding something" reads the
             * same whichever of the two is running.
             */
            char pkt_line[16] = { 0 };
            ui_packet_statusLine(pkt_line, sizeof(pkt_line));
            if (pkt_line[0] != '\0')
            {
                gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          yellow_fab413, "%s", pkt_line);
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
                /*
                 * The index is a 7 bit field and the table has 50 entries: a
                 * record from another codeplug, or this union read in a mode
                 * that is not FM, can hold anything up to 127.  The helper
                 * wraps, which is what the channel store has always done, so
                 * the screen never shows a tone that does not exist.
                 */
                uint16_t tone = ctcssFreqFromIndex(tone_tx_enable
                                                  ? last_state.channel.fm.txTone
                                                  : last_state.channel.fm.rxTone);
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

#ifdef UI_MODE_DMR
        case OPMODE_DMR:
            /*
             * Without a DMR ID the warning takes line 2 and there is nothing
             * to decode yet, so the block is not drawn under it. With an ID,
             * the block shows sync, colour code, timeslot, call and quality.
             */
            _ui_drawDmrWarning();
            if (settings_dmrIdValid(state.settings.dmr_id))
                _ui_drawModeState();
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

        #ifdef UI_MODE_TETRA
        case OPMODE_TETRA:
            (void) ui_state;
            /* A call owns lines 1..4; otherwise the block draws the cell and
               the registration and _ui_drawTetraLine1() keeps line 1 */
            _ui_drawTetraInfo();
            if (!_ui_callShown())
                _ui_drawModeState();
            break;
        #endif

        /*
         * The digital receive modes and DMR: everything they decode goes
         * through the one state block (see _ui_drawModeState), so the sync
         * state, the identifiers, the call and the quality read the same in
         * all of them and no mode can grow a private line again.
         */
        case OPMODE_NXDN:
        case OPMODE_DPMR:
        case OPMODE_P25:
        case OPMODE_YSF:
        case OPMODE_DSTAR:
            (void) ui_state;
            _ui_drawModeState();
            break;

        #ifdef UI_MODE_AIRBAND
        case OPMODE_AIRBAND:
            (void) ui_state;
            _ui_drawAirbandInfo();
            break;
        #endif

        #ifdef UI_MODE_WFM
        case OPMODE_WFM_BC:
            (void) ui_state;
            _ui_drawWfmInfo();
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

#ifdef CONFIG_VFO_SEARCH
/**
 * The carrier search, on the bottom row of the main screen.
 *
 * While it sweeps, the row says where it is going and how much of the band has
 * been covered; for a couple of seconds after it stops it says why, which is
 * the only feedback the operator gets that the sweep found a carrier, a
 * synchronism, a frequency the network corrected, nothing, or was cancelled.
 *
 * The S-meter gives up the row while this is drawn: the search is what the
 * operator is watching, and the meter comes back the moment the message
 * expires.  The row is the one the hints use on the menu screens, so nothing
 * else moves.
 *
 * @return true when the row was taken over.
 */
static bool _ui_drawSearchLine(void)
{
    struct vfoSearchStatus st;
    if (!vfoSearchService_status(&st))
        return false;

    if (!st.active) {
        if ((st.result == VFOS_RESULT_NONE) || (st.age >= VFO_SEARCH_MESSAGE_MS))
            return false;
    }

    char freq[16];
    char line[32];

    snprintf(freq, sizeof(freq), "%03lu.%05lu",
             (unsigned long)(st.frequency / 1000000UL),
             (unsigned long)((st.frequency % 1000000UL) / 10UL));

    if (st.active) {
        const char *arrow = (st.direction == VFOS_UP) ? "up" : "dn";
        snprintf(line, sizeof(line), "%s %s %u%%", arrow, freq,
                 (unsigned)(st.progress / 10U));
    }
    else {
        const char *why = vfoSearchService_reasonName(st.result, st.corrected);
        if (st.corrected) {
            long off = (long)(st.offsetHz / 10);   /* 10 Hz units, like the VFO */
            snprintf(line, sizeof(line), "%s %s %+ld", why, freq, off);
        }
        else {
            snprintf(line, sizeof(line), "%s %s", why, freq);
        }
    }

    gfx_print(layout.bottom_pos, FONT_SIZE_5PT, TEXT_ALIGN_CENTER, color_white,
              "%s", line);

    return true;
}
#endif /* CONFIG_VFO_SEARCH */

void _ui_drawMainBottom()
{
#ifdef CONFIG_VFO_SEARCH
    /* A running search, or the reason the last one stopped, owns the row */
    if (_ui_drawSearchLine())
        return;
#endif

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

/*
 * Line 1 of the digital modes belongs to the state block, drawn above with
 * line 2 and line 4 (ui_mode_state.h). The three modes that keep a line of
 * their own are the ones whose line 1 is not a decoded state: TETRA's SDS and
 * demodulator figures, broadcast FM's level and SNR, and the aeronautical
 * band's AGC, squelch and TX state.
 */
    #ifdef CONFIG_TETRA
    if(last_state.channel.mode == OPMODE_TETRA)
        _ui_drawTetraLine1(ui_state);
    #endif

    #ifdef UI_MODE_WFM
    if(last_state.channel.mode == OPMODE_WFM_BC)
        _ui_drawWfmLine1();
    #endif

    #ifdef UI_MODE_AIRBAND
    if(last_state.channel.mode == OPMODE_AIRBAND)
        _ui_drawAirbandLine1();
    #endif

    _ui_drawMainBottom();

    #ifdef CONFIG_C62_AIR_TX
    if(last_state.channel.mode == OPMODE_AIRBAND)
        _ui_drawAirbandTxPrompt();
    #endif
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

    _ui_drawMainBottom();
}
