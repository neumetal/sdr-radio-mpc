#pragma once
// Band plan for scanning: which channels each kind of service uses, and how
// to listen to it (wide FM, narrow FM or AM; the receiver picks the matching
// bandwidth). US allocations; other countries differ. No JUCE dependency, so
// the MPC build can share it.
//
// Many police and fire systems have moved to digital or trunked radio, which
// this can't decode; the analog ones (and fireground, EMS, county and
// rural services) still come through.
//
// AM broadcast sits below the R828D tuner's range; the RTL-SDR Blog V4 has
// an upconverter for it that its driver switches in below 28.8 MHz. Older
// dongles hear nothing there.

#include "SdrReceiver.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace bands
{
    struct Range { uint32_t firstHz, lastHz, stepHz; };

    struct Band
    {
        const char* id;          // parameter id
        const char* name;        // full name, for the host
        const char* label;       // button text
        int mode;                // SdrReceiver::Mode
        std::vector<Range> ranges;
    };

    inline const std::vector<Band>& all()
    {
        using R = SdrReceiver;
        static const std::vector<Band> list =
        {
            { "band_fm",     "FM Broadcast",       "FM",      R::FM,  { { 87900000, 107900000, 200000 } } },
            { "band_mw",     "AM Broadcast",       "AM",      R::AM,  { { 530000, 1700000, 10000 } } },
            { "band_air",    "Air (aviation)",     "AIR",     R::AM,  { { 118000000, 136975000, 25000 } } },
            { "band_wx",     "NOAA Weather",       "WEATHER", R::NFM, { { 162400000, 162550000, 25000 } } },
            { "band_marine", "Marine VHF",         "MARINE",  R::NFM, { { 156050000, 157425000, 25000 } } },
            { "band_rail",   "Railroad",           "RAIL",    R::NFM, { { 160215000, 161565000, 15000 } } },
            { "band_psvhf",  "Emergency VHF",      "EMS VHF", R::NFM, { { 150800000, 156250000, 12500 } } },
            { "band_psuhf",  "Emergency UHF",      "EMS UHF", R::NFM, { { 453000000, 460625000, 12500 } } },
            { "band_gmrs",   "GMRS / FRS",         "GMRS",    R::NFM, { { 462562500, 462725000, 25000 }, { 467562500, 467712500, 25000 } } },
            { "band_ham2",   "Ham 2 m repeaters",  "HAM 2M",  R::NFM, { { 145110000, 145490000, 20000 }, { 146520000, 146520000, 1 },
                                                                          { 146610000, 147390000, 15000 } } },
            { "band_ham70",  "Ham 70 cm repeaters","HAM 70CM",R::NFM, { { 442000000, 449975000, 25000 } } },
            { "band_cb",     "CB",                 "CB",      R::AM,  { { 26965000, 27405000, 10000 } } },
            // International broadcast on a 5 kHz grid, split by when the bands
            // are open, plus the WWV/WWVH time signals.
            { "band_swlow",  "Shortwave 120-41 m (best at night)", "SW NIGHT", R::AM,
              { { 2300000, 2495000, 5000 }, { 2500000, 2500000, 1 }, { 3200000, 3400000, 5000 },
                { 3900000, 4000000, 5000 }, { 4750000, 5060000, 5000 }, { 5900000, 6200000, 5000 },
                { 7200000, 7450000, 5000 } } },
            { "band_swhigh", "Shortwave 31-11 m (best by day)",    "SW DAY",   R::AM,
              { { 9400000, 9900000, 5000 }, { 10000000, 10000000, 1 }, { 11600000, 12100000, 5000 },
                { 13570000, 13870000, 5000 }, { 15000000, 15000000, 1 }, { 15100000, 15800000, 5000 },
                { 17480000, 17900000, 5000 }, { 18900000, 19020000, 5000 }, { 20000000, 20000000, 1 },
                { 21450000, 21850000, 5000 }, { 25000000, 25000000, 1 }, { 25670000, 26100000, 5000 } } },
        };
        return list;
    }

    // The mode a frequency is normally heard in: the band it falls in, or
    // else the usual US use of that part of the spectrum (HF and the two
    // airbands are AM, broadcast FM is wide, nearly everything else narrow).
    inline int modeFor (uint32_t hz)
    {
        for (const auto& b : all())
            for (const auto& r : b.ranges)
                if (hz >= r.firstHz && hz <= r.lastHz)
                    return b.mode;

        using R = SdrReceiver;
        if (hz < 30000000)                      return R::AM;
        if (hz >= 87500000 && hz < 108000000)   return R::FM;
        if (hz >= 108000000 && hz < 137000000)  return R::AM;    // civil air
        if (hz >= 225000000 && hz < 400000000)  return R::AM;    // military air
        return R::NFM;
    }

    // Every channel of the selected bands (bit i: all()[i]), low to high,
    // without duplicates where bands overlap.
    inline std::vector<SdrReceiver::Channel> channels (uint32_t selected)
    {
        std::vector<SdrReceiver::Channel> out;
        const auto& list = all();
        for (size_t i = 0; i < list.size(); ++i)
            if ((selected >> i) & 1u)
                for (const auto& r : list[i].ranges)
                    for (uint32_t hz = r.firstHz; hz <= r.lastHz && out.size() < (size_t) SdrReceiver::kMaxChannels; hz += r.stepHz)
                        out.push_back ({ hz, list[i].mode });

        std::sort (out.begin(), out.end(), [] (auto& a, auto& b) { return a.hz < b.hz; });
        out.erase (std::unique (out.begin(), out.end(), [] (auto& a, auto& b) { return a.hz == b.hz; }), out.end());
        return out;
    }
}
