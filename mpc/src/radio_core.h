#pragma once
// The radio half of both MPC plugins (SDR Radio, and SDR Haze, which adds
// the Haze step effects): tuning, modes, presets, scanning and the band
// survey, as string key/value parameters for mpc-vst-plugins' wrapper
// (wrapper/engine.h). Each plugin's engine file owns a RadioCore and hands
// it the radio keys; the receiver, scanner and band plan are the desktop
// plugin's (Source/SdrReceiver.*, StationScanner.h, Bands.h).
//
// The wrapper never calls two engine functions at once for one instance, so
// plain members are fine; SdrReceiver's own thread talks to us via atomics.
// What the desktop plugin does on its message thread (preset recall, the dial
// following the scanner) happens here directly, and the values the core
// changes itself are reported through takeChanged() (mpc_engine_changed).
//
// Every channel is Saved (known good: KEEP, or found by FIND ACTIVE),
// Blocked (known bad: BLOCK; no scan stops there) or New (the rest of the
// selected bands). Saved and Blocked are shared by both plugins and every
// project (channel_store.h). Scan sources: ALL (saved and new, never
// blocked), SAVED, NEW, PRESETS.

#include "channel_store.h"
#include "SdrReceiver.h"
#include "StationScanner.h"
#include "Bands.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

class RadioCore
{
public:
    static constexpr int kHostRate = 44100;
    static constexpr long long kMinHz = 500000, kMaxHz = 1766000000;   // the V4 tuner's range (the desktop dial stops at 1700)
    static constexpr int kNumPresets = 8;
    static constexpr int kFirstPresetNote = 36;                         // C1..G1 recall presets 1-8, as on the desktop
    static constexpr int kListRows = 24;                                // saved/blocked tiles per page

    RadioCore()
    {
        constexpr float kDefaultPresetMHz[kNumPresets] = { 88.1f, 91.5f, 94.7f, 98.1f, 100.1f, 102.7f, 105.9f, 107.5f };
        for (int i = 0; i < kNumPresets; ++i)
            presets[i] = { (uint32_t) std::lround (kDefaultPresetMHz[i] * 1000.0f) * 1000u, SdrReceiver::FM };
    }

    // dataDir: the plugin's folder (the shared channel file goes next to it), or null.
    void start (const char* dataDir)
    {
        store.open (dataDir);
        storeSeen = store.generation();
        updateScanList();
        sdr.start();                                 // returns at once; the receiver keeps retrying for the dongle
    }
    void stop()  { sdr.stop(); }

    // ---------------------------------------------------------------- parameters

    // A radio key: applies it and returns true; anything else returns false.
    bool setParam (const char* key, const char* val)
    {
        const float v = (float) std::atof (val);
        const int i = (int) std::lround (v);
        auto is = [key] (const char* k) { return std::strcmp (key, k) == 0; };
        const auto& bandList = bands::all();

        // Preset rows: a tap tunes to it, or with STORE lit saves the current
        // station there. A 0 is the host switching the previous row off: ignored.
        if (const int p = rowOf (key, "preset_", kNumPresets); p >= 0)
        {
            if (v < 0.5f) return true;
            if (storeArmed)
            {
                presets[p] = { hz, mode };
                updateScanList();
                storeArmed = false;
                mark (cStore);
            }
            else
                recallPreset (p);
            return true;
        }
        // Band rows: each one is an on/off switch.
        if (const int b = rowOf (key, "band_", (int) bandList.size()); b >= 0)
        {
            const uint32_t bit = 1u << b, before = bandMask;
            bandMask = v >= 0.5f ? (before | bit) : (before & ~bit);
            if (bandMask != before) updateScanList();
            return true;
        }
        // Saved (or blocked) tiles: a tap tunes to it (a 0 is the host's echo, ignored).
        if (const int l = rowOf (key, "list_", kListRows); l >= 0)
        {
            SdrReceiver::Channel c {};
            if (v >= 0.5f && listAt (listPage * kListRows + l, c))
            {
                setMode (c.mode);
                tune (c.hz, false);
            }
            return true;
        }

        // The knobs land on whole kHz (dropping a 12.5 kHz step's half kHz).
        // The knob that was turned already shows its value: only report it
        // back if the limits moved it (below 0.5 MHz, say).
        if (is ("mhz"))
        {
            tune ((long long) std::clamp (i, 0, 1766) * 1000000 + hz / 1000 % 1000 * 1000LL, true);
            if ((int) (hz / 1000000) == i) changed &= ~(1u << cMhz);
        }
        else if (is ("khz"))
        {
            tune ((long long) (hz / 1000000) * 1000000 + std::clamp (i, 0, 999) * 1000LL, true);
            if ((int) (hz / 1000 % 1000) == i) changed &= ~(1u << cKhz);
        }
        else if (is ("tune_down"))   { if (v > 0.5f) step (-1); }
        else if (is ("tune_up"))     { if (v > 0.5f) step (+1); }
        else if (is ("mode"))
        {
            // Picking a mode by hand turns AUTO off, as on the desktop. The
            // host echoing a mode the core set is not a pick.
            if (std::clamp (i, 0, 2) != mode)
            {
                setMode (i);
                if (modeAuto) { modeAuto = false; mark (cAuto); }
            }
        }
        else if (is ("mode_auto"))
        {
            const bool on = i == 1;
            if (on && ! modeAuto) setMode (bands::modeFor (hz));
            modeAuto = on;
        }
        else if (is ("agc"))         agc = i == 1;
        else if (is ("gain"))        gainDb = std::clamp (v, 0.0f, 49.6f);
        else if (is ("level"))       level = std::clamp (v, 0.0f, 2.0f);
        else if (is ("store"))       storeArmed = i == 1;
        else if (is ("scan_on"))     scanOn = i == 1;
        else if (is ("scan_src"))    { const int s = std::clamp (i, 0, 3); if (s != scanSrc) { scanSrc = s; stationPos = -1; updateScanList(); } }
        else if (is ("scan_style"))  scanStyle = i == 1 ? kSquelch : kBeat;
        else if (is ("scan_rate"))   scanRate = std::clamp (i, 0, StationScanner::kNumRates - 1);
        else if (is ("scan_random")) scanRandom = i == 1;
        else if (is ("scan_sql"))    sqlDb = std::clamp (v, 3.0f, 30.0f);
        else if (is ("tempo"))       tempo = std::clamp (v, 40.0f, 240.0f);
        else if (is ("scan_skip"))   { if (v > 0.5f) sdr.skip(); }
        else if (is ("scan_survey"))
        {
            // FIND ACTIVE: every new channel of the selected bands, once; a second tap stops it.
            if (v > 0.5f)
            {
                if (sdr.listSurveyProgress() >= 0.0f) sdr.cancelListSurvey();
                else                                  startSurvey();
            }
        }
        else if (is ("list_block"))   { if (v > 0.5f) blockTuned(); }
        else if (is ("list_add"))     { if (v > 0.5f) { store.keep ({ hz, mode }); listNote = "kept " + freqLabel (hz); } }
        else if (is ("list_view"))    { listView = i == 1 ? 1 : 0; listPage = 0; }
        else if (is ("list_clear"))
        {
            // Everyone's saved (or blocked) channels, in every project: a second tap within 3 s does it.
            if (v > 0.5f)
            {
                if (clearArmedAt >= 0 && frames - clearArmedAt < 3 * kHostRate)
                {
                    if (listView == 1) store.clearBlocked(); else store.clearSaved();
                    listNote = listView == 1 ? "unblocked all" : "cleared all saved";
                    clearArmedAt = -1;
                    listPage = 0;
                }
                else
                {
                    clearArmedAt = frames;
                    listNote = listView == 1 ? "tap CLEAR again to unblock all" : "tap CLEAR again to clear all saved";
                }
            }
        }
        else if (is ("list_prev"))    { if (v > 0.5f) listPage = std::max (0, listPage - 1); }
        else if (is ("list_next"))    { if (v > 0.5f) listPage = std::min (listPages() - 1, listPage + 1); }
        else return false;
        return true;
    }

    // A radio key's value or text: its length + 1, or 0 if it isn't one.
    int getParam (const char* key, char* buf, int len)
    {
        auto is = [key] (const char* k) { return std::strcmp (key, k) == 0; };
        auto num = [buf, len] (double v) { char t[32]; std::snprintf (t, sizeof t, "%.6g", v); return put (buf, len, t); };
        const auto& bandList = bands::all();
        char t[96];

        if (const int p = rowOf (key, "preset_", kNumPresets); p >= 0)
        {
            std::snprintf (t, sizeof t, "%d  %s %s", p + 1, freqLabel (presets[p].hz).c_str(), kModeNames[presets[p].mode]);
            return put (buf, len, t);
        }
        if (const int p = onRowOf (key, "preset_", kNumPresets); p >= 0)
        {
            const auto& sh = snapshot();
            return put (buf, len, tunedTo (p, sh.hz, sh.mode) ? "1" : "0");
        }
        if (const int b = rowOf (key, "band_", (int) bandList.size()); b >= 0)
            return put (buf, len, bandList[(size_t) b].label);
        if (const int b = onRowOf (key, "band_", (int) bandList.size()); b >= 0)
            return put (buf, len, (bandMask >> b) & 1u ? "1" : "0");
        if (const int l = rowOf (key, "list_", kListRows); l >= 0)
        {
            SdrReceiver::Channel c {};
            if (! listAt (listPage * kListRows + l, c)) return put (buf, len, "");
            if (listView == 1) return put (buf, len, freqLabel (c.hz).c_str());
            std::snprintf (t, sizeof t, "%s %s", freqLabel (c.hz).c_str(), kModeNames[c.mode]);
            return put (buf, len, t);
        }
        if (const int l = onRowOf (key, "list_", kListRows); l >= 0)
        {
            SdrReceiver::Channel c {};
            const auto& sh = snapshot();
            return put (buf, len, listAt (listPage * kListRows + l, c) && ChannelStore::near (c.hz, sh.hz) ? "1" : "0");
        }
        if (is ("list_text"))
        {
            std::snprintf (t, sizeof t, "%d saved, %d blocked, page %d/%d%s%s", (int) store.saved().size(), (int) store.blocked().size(),
                           listPage + 1, listPages(), listNote.empty() ? "" : "   ", listNote.c_str());
            return put (buf, len, t);
        }
        if (is ("chan_state"))
        {
            // What the channel on the dial is, for KEEP / BLOCK.
            const uint32_t h = snapshot().hz;
            return put (buf, len, sdr.listSurveyHz() ? "FINDING..." : store.isBlocked (h) ? "BLOCKED" : store.isSaved (h) ? "SAVED" : "NEW");
        }
        if (is ("hint_text"))
        {
            static const char* const hints[4] = { "ALL: saved and new channels, never blocked", "PRESETS: your eight presets",
                                                  "SAVED: only channels you kept", "NEW: only channels not kept or blocked yet" };
            return put (buf, len, hints[scanSrc]);
        }
        if (is ("count_text"))
        {
            const auto& c = counts();
            std::snprintf (t, sizeof t, "Bands: %d channels: %d saved, %d blocked, %d new", c[0], c[1], c[2], c[3]);
            return put (buf, len, t);
        }

        // Readouts: from the snapshot (see snapshot()).
        if (is ("freq_text"))
        {
            const uint32_t h = snapshot().hz;
            if (h % 1000) std::snprintf (t, sizeof t, "%u.%04u", h / 1000000, h % 1000000 / 100);
            else          std::snprintf (t, sizeof t, "%u.%03u", h / 1000000, h / 1000 % 1000);
            return put (buf, len, t);
        }
        if (is ("info"))
        {
            const auto& sh = snapshot();
            const char* band = bandName (sh.hz);
            std::snprintf (t, sizeof t, "%s%s%s%s", kModeNames[sh.mode], sh.modeAuto ? " AUTO" : "", *band ? "  /  " : "", band);
            return put (buf, len, t);
        }
        if (is ("status"))    return put (buf, len, snapshot().status.c_str());
        if (is ("scan_text")) return put (buf, len, snapshot().scan.c_str());
        if (is ("signal"))
        {
            if (snapshot().status != "Connected") { shownSignal = -1000; return put (buf, len, "Signal --"); }
            // A new value only when it moved by 4 dB or more, and 2 s after the last one.
            const int db = (int) std::lround (sdr.signalDb());
            if (shownSignal == -1000 || (std::abs (db - shownSignal) >= 4 && frames - signalShownAt >= kSignalFrames))
            {
                shownSignal = db;
                signalShownAt = frames;
            }
            std::snprintf (t, sizeof t, "Signal %d dB", shownSignal);
            return put (buf, len, t);
        }

        if (is ("mhz"))         return num (hz / 1000000);
        if (is ("khz"))         return num (hz / 1000 % 1000);
        if (is ("mode"))        return num (mode);
        if (is ("mode_auto"))   return num (modeAuto);
        if (is ("agc"))         return num (agc);
        if (is ("gain"))        return num (gainDb);
        if (is ("level"))       return num (level);
        if (is ("store"))       return num (storeArmed);
        if (is ("scan_on"))     return num (scanOn);
        if (is ("scan_src"))    return num (scanSrc);
        if (is ("scan_style"))  return num (scanStyle);
        if (is ("scan_rate"))   return num (scanRate);
        if (is ("scan_random")) return num (scanRandom);
        if (is ("scan_sql"))    return num (sqlDb);
        if (is ("tempo"))       return num (tempo);
        if (is ("list_view"))   return num (listView);
        if (is ("tune_down") || is ("tune_up") || is ("scan_skip") || is ("scan_survey") || is ("list_block") || is ("list_add")
            || is ("list_clear") || is ("list_prev") || is ("list_next")) return num (0);
        return 0;
    }

    // MIDI: notes 36-43 recall presets 1-8. Returns true if it used the message.
    bool midi (const uint8_t* msg, int len)
    {
        if (len < 3) return false;
        const int index = (msg[1] & 0x7f) - kFirstPresetNote;
        if ((msg[0] & 0xf0) == 0x90 && (msg[2] & 0x7f) > 0 && index >= 0 && index < kNumPresets)
        {
            recallPreset (index);
            return true;
        }
        return index >= 0 && index < kNumPresets && ((msg[0] & 0xf0) == 0x80 || (msg[0] & 0xf0) == 0x90);
    }

    void songPos (double newPpq, double bpm, int isPlaying)
    {
        hostBpm = bpm > 0.0 ? (float) bpm : 0.0f;
        ppq = newPpq;
        playing = isPlaying != 0;
    }

    float bpm() const          { return hostBpm > 0.0f ? hostBpm : tempo; }
    double songPpq() const     { return ppq; }
    bool isPlaying() const     { return playing; }

    // The values the core changed itself, one key per call (mpc_engine_changed).
    // While a scan runs, its hops' MHz, kHz and mode are held back: those knobs
    // sit on Q-Links, and each report can bring MPC's Q-Link panel back up.
    // The frequency display shows every hop; the knobs catch up when it stops.
    int takeChanged (char* key, int len)
    {
        const uint32_t held = scanOn && ! reportAll ? (1u << cMhz) | (1u << cKhz) | (1u << cMode) : 0u;
        for (int c = 0; c < cNum; ++c)
            if (changed & ~held & (1u << c))
            {
                changed &= ~(1u << c);
                return put (key, len, kChangedKeys[c]);
            }
        reportAll = false;
        return 0;
    }

    // ---------------------------------------------------------------- project state
    // "key=value;" pairs, as part of the plugin's chunk.

    std::string saveState() const
    {
        char t[96];
        std::snprintf (t, sizeof t, "hz=%u;", hz);
        std::string s = t;
        auto add = [&] (const char* k, double v) { std::snprintf (t, sizeof t, "%s=%.6g;", k, v); s += t; };
        add ("mode", mode); add ("auto", modeAuto); add ("agc", agc);
        add ("gain", gainDb); add ("level", level);
        add ("scan", scanOn); add ("src", scanSrc); add ("style", scanStyle); add ("rate", scanRate);
        add ("rnd", scanRandom); add ("sql", sqlDb); add ("tempo", tempo); add ("bands", bandMask);
        for (int i = 0; i < kNumPresets; ++i)
        {
            std::snprintf (t, sizeof t, "p%d=%u/%d;", i, presets[i].hz, presets[i].mode);
            s += t;
        }
        return s;                              // Saved and Blocked live in the shared channel file, not the project
    }

    // One "key=value" of a saved state: true if it was a radio key.
    bool loadKey (const std::string& k, const char* v)
    {
        const double d = std::atof (v);
        if      (k == "hz")    hz = (uint32_t) std::clamp ((long long) d, kMinHz, kMaxHz);
        else if (k == "mode")  mode = std::clamp ((int) d, 0, 2);
        else if (k == "auto")  modeAuto = d != 0;
        else if (k == "agc")   agc = d != 0;
        else if (k == "gain")  gainDb = std::clamp ((float) d, 0.0f, 49.6f);
        else if (k == "level") level = std::clamp ((float) d, 0.0f, 2.0f);
        else if (k == "scan")  scanOn = d != 0;
        else if (k == "src")   scanSrc = std::clamp ((int) d, 0, 3);
        else if (k == "style") scanStyle = d != 0 ? kSquelch : kBeat;
        else if (k == "rate")  scanRate = std::clamp ((int) d, 0, StationScanner::kNumRates - 1);
        else if (k == "rnd")   scanRandom = d != 0;
        else if (k == "sql")   sqlDb = std::clamp ((float) d, 3.0f, 30.0f);
        else if (k == "tempo") tempo = std::clamp ((float) d, 40.0f, 240.0f);
        else if (k == "bands") bandMask = (uint32_t) d & ((1u << bands::all().size()) - 1u);
        else if (k == "list" || k == "blocked")
        {
            // A project from before the shared channel file kept its own list ("16255375 2," = hz/100 then
            // the mode digit) and blocks ("16255375,"): merge them into the shared one.
            for (const char* c = v; *c; )
            {
                char* end = nullptr;
                const unsigned long long n = std::strtoull (c, &end, 10);
                if (end == c) break;
                if (k == "list") store.addFound ({ (uint32_t) std::clamp ((long long) (n / 10) * 100, kMinHz, kMaxHz), std::clamp ((int) (n % 10), 0, 2) });
                else             store.block ((uint32_t) std::clamp ((long long) n * 100, kMinHz, kMaxHz));
                c = *end == ',' ? end + 1 : end;
            }
        }
        else if (k.size() == 2 && k[0] == 'p' && k[1] >= '0' && k[1] < '0' + kNumPresets)
        {
            unsigned h = 0; int m = 0;
            if (std::sscanf (v, "%u/%d", &h, &m) == 2)
                presets[k[1] - '0'] = { (uint32_t) std::clamp ((long long) h, kMinHz, kMaxHz), std::clamp (m, 0, 2) };
        }
        else return false;
        return true;
    }

    void finishLoad()
    {
        storeArmed = false;
        listPage = 0;
        listNote.clear();
        clearArmedAt = -1;
        updateScanList();
        changed = (1u << cNum) - 1u;           // tell the host every value again, scanning or not
        reportAll = true;
    }

    // Splits "k=v;k=v;" and hands each pair to load (a std::string key, a C string value).
    template <typename Fn>
    static void forEachPair (const char* s, Fn load)
    {
        for (const char* p = s; *p; )
        {
            const char* end = std::strchr (p, ';');
            if (! end) end = p + std::strlen (p);
            std::string kv (p, end);
            p = *end ? end + 1 : end;
            const auto eq = kv.find ('=');
            if (eq != std::string::npos)
                load (kv.substr (0, eq), kv.c_str() + eq + 1);
        }
    }

    // ---------------------------------------------------------------- audio

    // One host block of radio, mono, at 44.1 kHz with the level applied.
    void render (float* out, int n)
    {
        scanTick (n);

        sdr.setFrequencyHz (hz);
        sdr.setMode ((SdrReceiver::Mode) mode);
        sdr.setAutoGain (agc);
        sdr.setGainTenthsDb ((int) std::lround (gainDb * 10.0f));

        frames += n;
        sdr.discardTo (4800);    // keep latency near 100 ms if the dongle's clock runs fast

        const double stepSize = (double) SdrReceiver::kAudioRate / kHostRate;
        for (int i = 0; i < n; ++i)
        {
            while (phase >= 1.0)
            {
                s0 = s1;
                if (! sdr.popSample (s1)) s1 = 0.0f;
                phase -= 1.0;
            }
            out[i] = (s0 + (s1 - s0) * (float) phase) * level;
            phase += stepSize;
        }
    }

    long long renderedFrames() const { return frames; }

    // "1", "12": like the preset rows, for other lists of the plugin.
    static int rowOf (const char* key, const char* prefix, int count)
    {
        const size_t n = std::strlen (prefix);
        if (std::strncmp (key, prefix, n) != 0) return -1;
        const char* rest = key + n;
        if (! *rest) return -1;
        for (const char* c = rest; *c; ++c)
            if (*c < '0' || *c > '9') return -1;
        const int i = std::atoi (rest) - 1;
        return i >= 0 && i < count ? i : -1;
    }

    // The row of a "<prefix>N_on" key, or -1.
    static int onRowOf (const char* key, const char* prefix, int count)
    {
        const char* on = std::strstr (key, "_on");
        if (! on || on[3] != 0 || on - key > 30) return -1;
        char row[32];
        std::snprintf (row, sizeof row, "%.*s", (int) (on - key), key);
        return rowOf (row, prefix, count);
    }

    static int put (char* buf, int len, const char* s)
    {
        if (len <= 0) return 0;
        std::snprintf (buf, (size_t) len, "%s", s);
        return (int) std::strlen (buf) + 1;
    }

private:
    enum ScanSource { kAll, kPresets, kSaved, kNew };   // new sources go at the end (saved in projects by index)
    enum ScanStyle  { kBeat, kSquelch };
    enum Changed { cMhz, cKhz, cMode, cAuto, cAgc, cGain, cLevel, cStore, cScan, cSrc, cStyle, cRate, cRandom, cSql, cTempo, cNum };
    static constexpr const char* kChangedKeys[cNum] = { "mhz", "khz", "mode", "mode_auto", "agc", "gain", "level", "store",
                                                         "scan_on", "scan_src", "scan_style", "scan_rate", "scan_random", "scan_sql", "tempo" };
    static constexpr const char* kModeNames[3] = { "FM", "AM", "NFM" };

    struct Preset { uint32_t hz; int mode; };

    SdrReceiver sdr;

    uint32_t hz = 100100000;
    int mode = SdrReceiver::FM;
    bool modeAuto = true, agc = true;
    float gainDb = 30.0f, level = 1.0f;

    Preset presets[kNumPresets];
    bool storeArmed = false;

    // Scanning, as in the desktop plugin, with the band survey in place of its FM survey.
    StationScanner scanner;
    bool scanOn = false, scanRandom = false;
    int scanSrc = kAll, scanStyle = kBeat, scanRate = 2, presetPos = -1, stationPos = -1;
    float sqlDb = 8.0f, tempo = 120.0f;
    uint32_t bandMask = 1;                     // bit i: bands::all()[i]; FM broadcast
    uint32_t lastParkedHz = 0;
    int surveySeen = 0;

    // Saved and Blocked, shared (channel_store.h); its generation, to notice changes from the file.
    ChannelStore store;
    int storeSeen = 0;
    long long storePolledAt = 0;
    int listView = 0, listPage = 0;            // the SAVED page shows the saved (0) or the blocked (1)
    long long clearArmedAt = -1;               // CLEAR's first tap, waiting for the second
    std::string listNote;                      // what the last FIND ACTIVE, KEEP or BLOCK did
    uint32_t countsKey = 0xffffffffu; int countsGen = -1, countsCache[4] {};
    float hostBpm = 0.0f;                      // from songPos; 0 when the MPC sends none
    double ppq = -1.0;
    bool playing = false;

    // 48 kHz -> 44.1 kHz linear interpolation, as in the desktop plugin.
    double phase = 1.0;
    float s0 = 0, s1 = 0;

    uint32_t changed = 0;                      // bit per Changed
    bool reportAll = false;                    // after a project load: report even what a scan holds back

    // Every change of a readout's text (or of a lit preset) makes MPC
    // rebuild the whole page (the wrapper polls them every 10-100 ms),
    // which is slow enough to starve the touchscreen and keep the Q-Link
    // panel up when it happens all the time. So the readouts show a
    // snapshot taken at most every 0.4 s (a turning knob shows its own
    // value meanwhile), and the signal moves only on a real change, at
    // most every 2 s.
    static constexpr long long kSnapshotFrames = kHostRate * 2 / 5, kSignalFrames = kHostRate * 2;
    long long frames = 0;                      // rendered so far, a clock
    struct Shown { uint32_t hz = 0; int mode = 0; bool modeAuto = false; std::string status, scan; } shown;
    long long shownAt = -1;                    // -1: no snapshot yet
    int shownSignal = -1000;                   // dB on screen; -1000: not shown yet
    long long signalShownAt = 0;

    void mark (Changed c) { changed |= 1u << c; }

    const Shown& snapshot()
    {
        if (shownAt >= 0 && frames - shownAt < kSnapshotFrames) return shown;
        shownAt = frames;
        // During a survey the display follows the channel being measured.
        const uint32_t surveying = sdr.listSurveyHz();
        shown.hz = surveying ? surveying : hz;
        shown.mode = surveying ? bands::modeFor (surveying) : mode;
        shown.modeAuto = modeAuto && ! surveying;
        const std::string s = sdr.status();
        shown.status = s == "Connected" ? "Connected" : s.rfind ("No RTL-SDR", 0) == 0 ? "No RTL-SDR found" : s;
        shown.scan = scanText();
        return shown;
    }

    // A frequency the user (or a preset, or the scanner) picked. With
    // AUTO on, a new frequency also brings the mode it's normally heard in.
    void tune (long long newHz, bool fromDial)
    {
        newHz = std::clamp (newHz, kMinHz, kMaxHz);
        if ((uint32_t) newHz == hz) return;
        const uint32_t before = hz;
        hz = (uint32_t) newHz;
        if (hz / 1000000 != before / 1000000) mark (cMhz);
        if (hz / 1000 % 1000 != before / 1000 % 1000) mark (cKhz);
        if (fromDial && modeAuto) setMode (bands::modeFor (hz));
    }

    void setMode (int m)
    {
        m = std::clamp (m, 0, 2);
        if (m != mode) { mode = m; mark (cMode); }
    }

    // Step to the next channel on the grid for the current mode: US FM
    // 200 kHz, narrow FM 12.5 kHz, AM airband 25 kHz, AM broadcast
    // 10 kHz, shortwave 5 kHz (as the desktop's arrows).
    void step (int direction)
    {
        const long long h = hz;
        const bool am = mode == SdrReceiver::AM;
        const long long grid = am && h < 2000000 ? 10000 : am && h < 26500000 ? 5000 : am ? 25000
                             : mode == SdrReceiver::NFM ? 12500 : 200000;
        tune ((direction > 0 ? h / grid + 1 : (h + grid - 1) / grid - 1) * grid, true);
    }

    void recallPreset (int i)
    {
        setMode (presets[i].mode);
        tune (presets[i].hz, false);
    }

    bool tunedTo (int i, uint32_t atHz, int atMode) const
    {
        return std::llabs ((long long) presets[i].hz - (long long) atHz) < 500 && presets[i].mode == atMode;
    }

    // The channels the receiver's squelch scan steps through, by source.
    void updateScanList()
    {
        std::vector<SdrReceiver::Channel> list;
        if (scanSrc == kPresets)
            for (const auto& p : presets)
                list.push_back ({ p.hz, p.mode });
        else if (scanSrc == kSaved)
            list = store.saved();
        else if (scanSrc == kNew)
            list = newChannels();
        else
        {
            // ALL: the saved ones (in the bands or not) and the bands' new ones, low to high.
            list = newChannels();
            list.insert (list.end(), store.saved().begin(), store.saved().end());
            std::sort (list.begin(), list.end(), [] (const SdrReceiver::Channel& a, const SdrReceiver::Channel& b) { return a.hz < b.hz; });
        }
        sdr.setScanList (list.data(), (int) list.size());
        scanList = list;
    }

    // The selected bands' channels that are neither saved nor blocked.
    std::vector<SdrReceiver::Channel> newChannels() const
    {
        auto list = bands::channels (bandMask);
        list.erase (std::remove_if (list.begin(), list.end(), [this] (const SdrReceiver::Channel& c)
                                    { return store.isBlocked (c.hz) || store.isSaved (c.hz); }), list.end());
        return list;
    }

    // [channels, saved, blocked, new] in the selected bands, kept until the bands or the store change.
    const int* counts()
    {
        if (countsKey == bandMask && countsGen == store.generation()) return countsCache;
        countsKey = bandMask; countsGen = store.generation();
        const auto all = bands::channels (bandMask);
        int s = 0, b = 0;
        for (const auto& c : all) { if (store.isBlocked (c.hz)) ++b; else if (store.isSaved (c.hz)) ++s; }
        countsCache[0] = (int) all.size(); countsCache[1] = s; countsCache[2] = b; countsCache[3] = (int) all.size() - s - b;
        return countsCache;
    }

    // FIND ACTIVE: listen to every new channel of the selected bands once; the active ones are saved.
    void startSurvey()
    {
        const auto list = newChannels();
        if (! list.empty()) sdr.requestListSurvey (list.data(), (int) list.size(), sqlDb);
        else listNote = "no new channels in these bands";
    }

    // Tile n of the SAVED page: a saved channel, or (BLOCKED view) a blocked one.
    bool listAt (int n, SdrReceiver::Channel& out) const
    {
        if (n < 0) return false;
        if (listView == 1)
        {
            if ((size_t) n >= store.blocked().size()) return false;
            out = { store.blocked()[(size_t) n], bands::modeFor (store.blocked()[(size_t) n]) };
            return true;
        }
        if ((size_t) n >= store.saved().size()) return false;
        out = store.saved()[(size_t) n];
        return true;
    }

    int listPages() const
    {
        const size_t n = listView == 1 ? store.blocked().size() : store.saved().size();
        return std::max (1, ((int) n + kListRows - 1) / kListRows);
    }

    // BLOCK: not saved any more, and no scan stops there again; a scan sitting on it moves on.
    void blockTuned()
    {
        store.block (hz);
        listNote = "blocked " + freqLabel (hz);
        if (scanOn) sdr.skip();
    }

    std::vector<SdrReceiver::Channel> scanList;   // what the receiver scans now (for Beat hopping)

    static const char* bandName (uint32_t atHz)
    {
        for (const auto& b : bands::all())
            for (const auto& r : b.ranges)
                if (atHz >= r.firstHz && atHz <= r.lastHz)
                    return b.name;
        return "";
    }

    // "88.1", "162.55", "462.5625": MHz with no trailing zeros; below 30 MHz in kHz.
    static std::string freqLabel (uint32_t h)
    {
        char t[32];
        if (h < 30000000)
        {
            if (h % 1000) std::snprintf (t, sizeof t, "%u.%u kHz", h / 1000, h % 1000 / 100);
            else          std::snprintf (t, sizeof t, "%u kHz", h / 1000);
            return t;
        }
        std::snprintf (t, sizeof t, "%u.%06u", h / 1000000, h % 1000000);
        std::string s = t;
        while (s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
        return s;
    }

    // Like every readout, it only changes on a real change of state: no live numbers.
    std::string scanText() const
    {
        char t[64];
        const float progress = sdr.listSurveyProgress();
        if (progress >= 0.0f)
        {
            std::snprintf (t, sizeof t, "Surveying %d%%  (tap again to stop)", (int) (progress * 100.0f));
            return t;
        }
        if (! scanOn) return listNote.rfind ("found", 0) == 0 ? listNote : "Scan off";
        if ((scanSrc == kAll || scanSrc == kNew) && bandMask == 0) return "Pick a band below";
        if (scanSrc == kSaved && store.saved().empty()) return "Nothing saved yet: FIND ACTIVE, or KEEP";
        if (scanSrc == kNew && scanList.empty()) return "No new channels left in these bands";
        const int state = sdr.scanState();
        if (state == SdrReceiver::kSeeking) return "Searching...";
        if (state == SdrReceiver::kOpen)    return "Stopped on a signal";
        if (scanStyle == kBeat && scanSrc == kSaved && stationPos >= 0)
        {
            std::snprintf (t, sizeof t, "Saved: %d of %d", stationPos + 1, (int) scanList.size());
            return t;
        }
        if (scanSrc == kPresets && presetPos >= 0)
        {
            std::snprintf (t, sizeof t, "Preset %d", presetPos + 1);
            return t;
        }
        return scanStyle == kBeat ? "On the beat" : "Scanning";
    }

    // Once per host block: the scanner, as in the desktop plugin's processBlock().
    void scanTick (int n)
    {
        if (sdr.listSurveyGeneration() != surveySeen)
        {
            // FIND ACTIVE finished: the active channels are saved.
            surveySeen = sdr.listSurveyGeneration();
            const auto found = sdr.listSurveyStations();
            int added = 0;
            for (const auto& c : found) added += store.addFound (c);
            char t[64];
            std::snprintf (t, sizeof t, "found %d active, %d saved", (int) found.size(), added);
            listNote = t;
            stationPos = -1;
        }
        if (frames - storePolledAt >= 2 * kHostRate)   // another plugin or instance changed the shared file? (a stat)
        {
            storePolledAt = frames;
            store.poll();
        }
        if (store.generation() != storeSeen)       // KEEP, BLOCK, a find, or the other plugin: rescan the new set
        {
            storeSeen = store.generation();
            listPage = std::min (listPage, listPages() - 1);
            updateScanList();
        }

        const bool squelch = scanStyle == kSquelch;
        const bool surveying = sdr.listSurveyProgress() >= 0.0f;

        // Beat through ALL or NEW: the receiver seeks the next channel with a
        // signal on each beat. Beat through SAVED or the presets: hop
        // straight to the next one, right on the beat.
        const bool beatSeek = scanOn && ! squelch && (scanSrc == kAll || scanSrc == kNew);
        const bool beatHop = scanOn && ! squelch && scanSrc == kSaved && ! scanList.empty();
        const bool beatPresets = scanOn && ! squelch && scanSrc == kPresets;

        sdr.setSquelchDb (sqlDb);
        sdr.setSquelchScan ((scanOn && squelch) || beatSeek);
        sdr.setScanHold (beatSeek);

        if ((beatHop || beatSeek || beatPresets) && ! surveying)
        {
            const double lead = ((double) sdr.available() + 820.0) / SdrReceiver::kAudioRate;
            if (scanner.tick (ppq, playing, bpm(), n, kHostRate, scanRate, lead))
            {
                if (beatSeek)
                    sdr.skip();
                else if (beatHop)
                {
                    stationPos = scanner.nextIndex (std::min (stationPos, (int) scanList.size() - 1), (int) scanList.size(), scanRandom);
                    setMode (scanList[(size_t) stationPos].mode);
                    tune (scanList[(size_t) stationPos].hz, false);
                }
                else
                {
                    presetPos = scanner.nextIndex (presetPos, kNumPresets, scanRandom);
                    recallPreset (presetPos);
                }
            }
        }
        else
            scanner.resetClock();

        // Squelch (and seek): when the receiver stops on a channel, put that on the dial.
        const int state = sdr.scanState();
        if (state == SdrReceiver::kOpen && sdr.scanHz() != lastParkedHz)
        {
            lastParkedHz = sdr.scanHz();
            setMode (sdr.scanMode());
            tune (lastParkedHz, false);
        }
        else if (state == SdrReceiver::kSeeking)
            lastParkedHz = 0;
    }
};
