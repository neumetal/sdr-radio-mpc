#pragma once
// Scanning: picks the strong stations out of an FM band survey
// (SdrReceiver::requestSurvey) and times hops between them to the beat.
// No JUCE dependency and no allocation, so the desktop plugin and the MPC
// build both call it from the audio thread.

#include "SdrReceiver.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>

class StationScanner
{
public:
    // How often to hop, in beats (4/4: 4 beats to the bar).
    static constexpr int kNumRates = 6;                 // 1 beat, 2 beats, 1 bar, 2 bars, 4 bars, 8 bars
    static constexpr double kRateBeats[kNumRates] = { 1, 2, 4, 8, 16, 32 };

    static constexpr float kAboveFloorDb = 10.0f;       // how far over the band's noise floor counts as strong

    // A station is a channel at least kAboveFloorDb over the noise floor
    // (the quietest quarter of the band) and stronger than both neighbours,
    // so a big station's spill onto the next channels doesn't count twice.
    void setSurvey (const float* db)
    {
        constexpr int n = SdrReceiver::kFmChannels;
        float sorted[n];
        std::copy (db, db + n, sorted);
        std::nth_element (sorted, sorted + n / 4, sorted + n);
        const float threshold = sorted[n / 4] + kAboveFloorDb;

        count = 0;
        for (int i = 0; i < n; ++i)
        {
            const bool peak = (i == 0 || db[i] >= db[i - 1]) && (i == n - 1 || db[i] >= db[i + 1]);
            if (db[i] >= threshold && peak)
                stations[count++] = SdrReceiver::kFmFirstKHz + i * SdrReceiver::kFmStepKHz;
        }
    }

    int numStations() const { return count; }

    // The station to hop to from currentKHz: the next one up (wrapping), or
    // a random other one.
    int next (int currentKHz, bool random)
    {
        if (count == 0) return currentKHz;
        if (random && count > 1)
        {
            int pick;
            do { rng = rng * 1664525u + 1013904223u; pick = (int) ((rng >> 8) % (uint32_t) count); }
            while (stations[pick] == currentKHz);
            return stations[pick];
        }
        for (int i = 0; i < count; ++i)
            if (stations[i] > currentKHz)
                return stations[i];
        return stations[0];
    }

    // The next of n slots (the presets) from current: in order, or a random other one.
    int nextIndex (int current, int n, bool random)
    {
        if (n <= 1) return 0;
        if (! random) return (current + 1) % n;
        int pick;
        do { rng = rng * 1664525u + 1013904223u; pick = (int) ((rng >> 8) % (uint32_t) n); }
        while (pick == current);
        return pick;
    }

    // Once per audio block. ppq: the host's song position in beats at the
    // block start (< 0 when there is none). leadSeconds: how long a retune
    // takes to be heard, so the hop is started that much early and lands on
    // the beat. Returns true when it's time to hop.
    bool tick (double ppq, bool playing, double bpm, int frames, double sampleRate, int rate, double leadSeconds)
    {
        const double beatsPerSecond = std::clamp (bpm, 30.0, 300.0) / 60.0;
        const double blockBeats = frames / sampleRate * beatsPerSecond;
        double pos;
        if (playing && ppq >= 0.0)
            pos = ppq + blockBeats + leadSeconds * beatsPerSecond;
        else
            pos = (freeBeats += blockBeats);

        const long long slot = (long long) std::floor (pos / kRateBeats[std::clamp (rate, 0, kNumRates - 1)]);
        if (slot == lastSlot) return false;
        const bool first = lastSlot == LLONG_MIN;
        lastSlot = slot;
        return ! first;
    }

    void resetClock() { lastSlot = LLONG_MIN; freeBeats = 0.0; }

private:
    int stations[SdrReceiver::kFmChannels] {};
    int count = 0;
    double freeBeats = 0.0;
    long long lastSlot = LLONG_MIN;
    uint32_t rng = 0x2545f491u;
};
