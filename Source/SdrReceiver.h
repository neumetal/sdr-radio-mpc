#pragma once
// Portable RTL-SDR receiver core. No JUCE dependency, so it can be reused
// in other hosts/platforms. Reads IQ on its own thread, demodulates to mono
// 48 kHz audio, and hands it over through a lock-free ring buffer.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class SdrReceiver
{
public:
    // FM: broadcast (200 kHz wide). AM: broadcast, shortwave, aviation, CB
    // (~10 kHz). NFM: narrow FM, two-way radio (12.5 kHz).
    enum Mode { FM = 0, AM = 1, NFM = 2 };

    // AM below CB is broadcast (MW and shortwave): it gets the full ~5.5 kHz
    // of audio. Above, AM is voice (aviation, CB) and stops at ~3.4 kHz.
    static constexpr bool isBroadcastAm (uint32_t hz) { return hz < 26500000; }

    static constexpr int kSdrRate   = 960000;  // IQ sample rate (valid for RTL-SDR)
    static constexpr int kAudioRate = 48000;   // output audio rate (960000 / 20)

    SdrReceiver() = default;
    ~SdrReceiver();

    // start() returns immediately; the worker thread opens the dongle and
    // keeps retrying if it is missing or gets unplugged.
    void start();
    void stop();

    // Safe to call from any thread (they only set atomics).
    void setFrequencyHz (uint32_t hz)      { freqHz.store (hz); }
    void setGainTenthsDb (int tenthsDb)    { gain.store (tenthsDb); }
    void setAutoGain (bool on)             { autoGain.store (on); }
    void setMode (Mode m)                  { mode.store ((int) m); }

    // Audio-thread side (single consumer). Never blocks.
    bool popSample (float& out);
    size_t available() const;
    void   discardTo (size_t keepFrames);  // drop old audio to bound latency

    std::string status() const;

    // Smoothed IQ power after the first decimation stage, in dBFS.
    float signalDb() const { return sigDb.load (std::memory_order_relaxed); }

    // ---- FM band survey, for scanning: measures every US FM channel
    // (87.9 to 107.9 MHz) at a fixed gain. The audio goes silent for the
    // ~2 s it takes, then the receiver returns to its settings.
    static constexpr int kFmChannels = 101;
    static constexpr int kFmFirstKHz = 87900, kFmStepKHz = 200;

    void  requestSurvey()          { surveyRequest.store (true); }
    float surveyProgress() const   { return surveyProg.load (std::memory_order_relaxed); }   // 0..1, or -1 when idle
    int   surveyGeneration() const { return surveyGen.load (std::memory_order_acquire); }   // bumps when one finishes
    void  copySurvey (float* dbOut) const;                  // kFmChannels channel powers, in dB

    // ---- Squelch scan: steps through a list of channels and stops on the
    // first one with a signal, like a scanner. While it runs it tunes the
    // dongle itself (the frequency and mode set above are ignored). After
    // kHangSeconds without a signal it moves on.
    struct Channel { uint32_t hz; int mode; };
    static constexpr int kMaxChannels = 4096;
    static constexpr float kHangSeconds = 2.0f;

    void setScanList (const Channel* list, int n);              // message thread
    void setSquelchScan (bool on)  { sqlScan.store (on); }
    void setSquelchDb (float db)   { sqlDb.store (db); }        // over the noise floor
    void skip()                    { skipRequest.store (true); }
    void setScanHold (bool hold)   { sqlHold.store (hold); }    // stay until skip(), even when quiet

    enum ScanState { kScanOff, kSeeking, kOpen, kWaiting };
    int scanState() const          { return sqlState.load (std::memory_order_relaxed); }
    int scanKHz() const            { return (int) ((sqlHz.load (std::memory_order_relaxed) + 500u) / 1000u); }   // where the scan is now
    uint32_t scanHz() const        { return sqlHz.load (std::memory_order_relaxed); }
    int scanMode() const           { return sqlMode.load (std::memory_order_relaxed); }

    // How far the tuned channel stands over the noise floor, in dB.
    float channelSnrDb() const     { return snrDb.load (std::memory_order_relaxed); }

    // ---- List survey: measures every channel of a list once, ~25 ms each,
    // at a fixed gain, and keeps the ones at least minSnrDb over the noise
    // floor (a station's spill onto the channels next to it doesn't count
    // twice). Silent while it runs, like the FM survey; listSurveyHz()
    // follows the channel being measured, so a UI can show it working.
    void  requestListSurvey (const Channel* list, int n, float minSnrDb);   // message thread
    void  cancelListSurvey()        { listSurveyCancel.store (true); }
    float listSurveyProgress() const { return listSurveyProg.load (std::memory_order_relaxed); }   // 0..1, or -1 when idle
    uint32_t listSurveyHz() const   { return listSurveyAt.load (std::memory_order_relaxed); }     // 0 when idle
    int   listSurveyGeneration() const { return listSurveyGen.load (std::memory_order_acquire); } // bumps when one finishes
    std::vector<Channel> listSurveyStations() const;          // the last survey's finds, low to high

private:
    void run();
    bool squelchStep (void* dev, std::vector<uint8_t>& buf);
    void applySettings (void* dev, uint32_t hz);
    void process (const uint8_t* data, int n, int demod);
    float measureSnr (const uint8_t* data, int n, int demod);
    void push (float s);
    void runSurvey (void* dev);
    void runListSurvey (void* dev);
    void setStatus (const std::string& s);

    std::thread worker;
    std::atomic<bool> running { false };

    std::atomic<uint32_t> freqHz { 100000000 };
    std::atomic<int>  gain { 300 };
    std::atomic<bool> autoGain { true };
    std::atomic<int>  mode { FM };

    uint32_t appliedFreq = 0;
    int  appliedGain = -1;
    int  appliedAgc = -1;

    // DSP state (worker thread only)
    float accI = 0, accQ = 0, prevI = 0, prevQ = 0;
    float accA = 0, deemph = 0, amDc = 0;
    // Narrow modes: a 4-pole low-pass on IQ at 240 kHz picks out the channel,
    // then it's decimated to 48 kHz and demodulated there.
    struct Biquad { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; float run (float x); };
    Biquad chanI[2], chanQ[2];
    int chanMode = -1;
    float nPrevI = 0, nPrevQ = 0, audioLp = 0, audioDc = 0;
    std::atomic<float> sigDb { -100.0f };
    int cnt1 = 0, cnt2 = 0;
    float fadeIn = 1.0f;          // ramps up after a retune, so changes don't click

    std::atomic<bool> surveyRequest { false };
    std::atomic<float> surveyProg { -1.0f };
    std::atomic<int> surveyGen { 0 };
    std::atomic<float> surveyDb[kFmChannels] {};

    std::atomic<bool> listSurveyRequest { false }, listSurveyCancel { false };
    std::atomic<float> listSurveyMin { 8.0f }, listSurveyProg { -1.0f };
    std::atomic<uint32_t> listSurveyAt { 0 };
    std::atomic<int> listSurveyGen { 0 };
    mutable std::mutex stationsMutex;
    std::vector<Channel> stations, surveyList;

    std::atomic<bool> sqlScan { false }, skipRequest { false }, sqlHold { false };
    std::atomic<float> sqlDb { 8.0f }, snrDb { 0.0f };
    std::atomic<int> sqlState { kScanOff }, sqlMode { FM };
    std::atomic<uint32_t> sqlHz { 0 };
    std::mutex listMutex;
    std::vector<Channel> pendingList;
    std::atomic<int> listGen { 0 };
    // worker thread only
    std::vector<Channel> scanList;
    int listSeen = -1, scanPos = -1;
    bool parked = false;
    std::chrono::steady_clock::time_point lastSignal;
    float squelchGain = 0.0f;

    // Single-producer / single-consumer ring buffer of audio samples
    static constexpr size_t kRing = 1 << 16;
    std::vector<float> ring = std::vector<float> (kRing, 0.0f);
    std::atomic<size_t> wr { 0 }, rd { 0 };

    mutable std::mutex statusMutex;
    std::string statusText = "Idle";
};
