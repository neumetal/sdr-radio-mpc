#include "SdrReceiver.h"
#include <rtl-sdr.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>

namespace
{
    // Power spectrum of 8-bit IQ, averaged over as many 1024-point frames as
    // fit (Hann window, radix-2). 937.5 Hz bins at 960 kHz.
    struct Spectrum
    {
        static constexpr int kN = 1024;
        std::vector<float> window = std::vector<float> (kN), power = std::vector<float> (kN);
        std::vector<std::complex<float>> twiddle = std::vector<std::complex<float>> (kN / 2), x = std::vector<std::complex<float>> (kN);

        Spectrum()
        {
            constexpr double kTwoPi = 6.283185307179586;
            for (int i = 0; i < kN; ++i)
                window[(size_t) i] = 0.5f - 0.5f * (float) std::cos (kTwoPi * i / kN);
            for (int i = 0; i < kN / 2; ++i)
                twiddle[(size_t) i] = std::polar (1.0f, (float) (-kTwoPi * i / kN));
        }

        void fft()                                                  // in place
        {
            for (int i = 1, j = 0; i < kN; ++i)
            {
                int bit = kN >> 1;
                for (; j & bit; bit >>= 1) j ^= bit;
                j ^= bit;
                if (i < j) std::swap (x[(size_t) i], x[(size_t) j]);
            }
            for (int len = 2; len <= kN; len <<= 1)
                for (int i = 0; i < kN; i += len)
                    for (int k = 0; k < len / 2; ++k)
                    {
                        const auto t = x[(size_t) (i + k + len / 2)] * twiddle[(size_t) (k * (kN / len))];
                        x[(size_t) (i + k + len / 2)] = x[(size_t) (i + k)] - t;
                        x[(size_t) (i + k)] += t;
                    }
        }

        // Fills power[] from up to maxFrames frames of data (n bytes).
        void measure (const uint8_t* data, int n, int maxFrames = 1 << 30)
        {
            std::fill (power.begin(), power.end(), 0.0f);
            for (int f = 0; (f + 1) * kN * 2 <= n && f < maxFrames; ++f)
            {
                const uint8_t* p = data + f * kN * 2;
                for (int i = 0; i < kN; ++i)
                    x[(size_t) i] = { (p[2 * i] - 127.5f) * window[(size_t) i], (p[2 * i + 1] - 127.5f) * window[(size_t) i] };
                fft();
                for (int i = 0; i < kN; ++i)
                    power[(size_t) i] += std::norm (x[(size_t) i]);
            }
        }

        // Total power within +-halfWidthHz of offsetHz from the centre.
        double band (double offsetHz, double halfWidthHz) const
        {
            double sum = 0.0;
            for (double hz = offsetHz - halfWidthHz; hz <= offsetHz + halfWidthHz; hz += (double) SdrReceiver::kSdrRate / kN)
                sum += power[(size_t) ((int) std::lround (hz * kN / SdrReceiver::kSdrRate) + kN) % kN];
            return sum;
        }
    };
}

SdrReceiver::~SdrReceiver() { stop(); }

void SdrReceiver::start()
{
    if (running.exchange (true)) return;
    worker = std::thread (&SdrReceiver::run, this);
}

void SdrReceiver::stop()
{
    if (! running.exchange (false)) return;
    if (worker.joinable()) worker.join();
}

void SdrReceiver::setStatus (const std::string& s)
{
    std::lock_guard<std::mutex> lock (statusMutex);
    statusText = s;
}

std::string SdrReceiver::status() const
{
    std::lock_guard<std::mutex> lock (statusMutex);
    return statusText;
}

// ---------------------------------------------------------------- ring buffer

void SdrReceiver::push (float s)
{
    const size_t w = wr.load (std::memory_order_relaxed);
    const size_t r = rd.load (std::memory_order_acquire);
    if (w - r >= kRing) return;                       // full: drop
    ring[w & (kRing - 1)] = s;
    wr.store (w + 1, std::memory_order_release);
}

bool SdrReceiver::popSample (float& out)
{
    const size_t r = rd.load (std::memory_order_relaxed);
    const size_t w = wr.load (std::memory_order_acquire);
    if (r == w) return false;
    out = ring[r & (kRing - 1)];
    rd.store (r + 1, std::memory_order_release);
    return true;
}

size_t SdrReceiver::available() const
{
    return wr.load (std::memory_order_acquire) - rd.load (std::memory_order_acquire);
}

void SdrReceiver::discardTo (size_t keepFrames)
{
    const size_t w = wr.load (std::memory_order_acquire);
    const size_t r = rd.load (std::memory_order_relaxed);
    if (w - r > keepFrames)
        rd.store (w - keepFrames, std::memory_order_release);
}

// ---------------------------------------------------------------- worker thread

void SdrReceiver::run()
{
    std::vector<uint8_t> buf (65536);

    while (running.load())
    {
        rtlsdr_dev_t* dev = nullptr;

        if (rtlsdr_get_device_count() == 0 || rtlsdr_open (&dev, 0) < 0)
        {
            setStatus ("No RTL-SDR found (or it is in use by another program)");
            for (int i = 0; i < 10 && running.load(); ++i)
                std::this_thread::sleep_for (std::chrono::milliseconds (100));
            continue;
        }

        rtlsdr_set_sample_rate (dev, kSdrRate);
        appliedFreq = 0; appliedGain = -1; appliedAgc = -1;
        rtlsdr_reset_buffer (dev);
        setStatus ("Connected");

        while (running.load())
        {
            if (surveyRequest.exchange (false))
                runSurvey (dev);
            if (listSurveyRequest.exchange (false))
                runListSurvey (dev);

            if (sqlScan.load())
            {
                if (! squelchStep (dev, buf))
                    break;                              // unplugged or error
                continue;
            }
            if (sqlState.exchange (kScanOff) != kScanOff)
            {
                parked = false;
                appliedFreq = 0;                        // back to the dial
            }

            applySettings (dev, freqHz.load());
            int got = 0;
            if (rtlsdr_read_sync (dev, buf.data(), (int) buf.size(), &got) < 0)
                break;                                  // unplugged or error
            const int demod = mode.load();
            snrDb.store (measureSnr (buf.data(), got, demod), std::memory_order_relaxed);
            process (buf.data(), got, demod);
        }

        rtlsdr_close (dev);
        sigDb.store (-100.0f);
        setStatus ("Disconnected");
    }
}

void SdrReceiver::applySettings (void* d, uint32_t f)
{
    auto* dev = static_cast<rtlsdr_dev_t*> (d);

    if (f != appliedFreq)
    {
        rtlsdr_set_center_freq (dev, f);
        appliedFreq = f;
        fadeIn = 0.0f;
    }

    const int agc = autoGain.load() ? 1 : 0;
    if (agc != appliedAgc)
    {
        rtlsdr_set_tuner_gain_mode (dev, agc ? 0 : 1);   // 0 = automatic, 1 = manual
        appliedAgc = agc;
        appliedGain = -1;
    }

    const int g = gain.load();
    if (! agc && g != appliedGain)
    {
        rtlsdr_set_tuner_gain (dev, g);                  // tenths of a dB
        appliedGain = g;
    }
}

float SdrReceiver::Biquad::run (float x)
{
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

// IQ (8-bit unsigned) @ 960 kHz -> /4 -> 240 kHz, then
//   FM:      discriminator -> /5 -> 48 kHz audio, 75 us de-emphasis
//   AM, NFM: channel filter -> /5 -> 48 kHz IQ -> envelope or discriminator
void SdrReceiver::process (const uint8_t* data, int n, int demod)
{
    // Squelch (while scanning): mute when the channel goes quiet.
    const float squelchTarget = sqlState.load (std::memory_order_relaxed) == kWaiting ? 0.0f : 1.0f;
    const bool wide = demod == FM;

    const bool broadcast = demod == AM && isBroadcastAm (appliedFreq);
    const int chan = broadcast ? 3 : demod;
    if (! wide && chan != chanMode)
    {
        // 4-pole Butterworth (two biquads, Q 0.54 and 1.31) at the channel's
        // half-width: 5 kHz for voice AM, 5.5 kHz for broadcast AM, 6 kHz for NFM.
        const float fc = broadcast ? 5500.0f : demod == AM ? 5000.0f : 6000.0f;
        const float k = std::tan (3.14159265f * fc / 240000.0f);
        const float qs[2] = { 0.5412f, 1.3066f };
        for (int s = 0; s < 2; ++s)
        {
            const float norm = 1.0f / (1.0f + k / qs[s] + k * k);
            Biquad b;
            b.b0 = k * k * norm; b.b1 = 2.0f * b.b0; b.b2 = b.b0;
            b.a1 = 2.0f * (k * k - 1.0f) * norm;
            b.a2 = (1.0f - k / qs[s] + k * k) * norm;
            chanI[s] = chanQ[s] = b;
        }
        chanMode = chan;
    }
    // Audio low-pass (one pole): ~3.4 kHz for voice, ~6 kHz for broadcast.
    const float audioCoef = broadcast ? 0.54f : 0.36f;

    constexpr float kInvPi = 0.31830988f;
    float powerSum = 0.0f;
    int powerCount = 0;

    for (int k = 0; k + 1 < n; k += 2)
    {
        accI += (data[k]     - 127.5f) * (1.0f / 127.5f);
        accQ += (data[k + 1] - 127.5f) * (1.0f / 127.5f);

        if (++cnt1 < 4) continue;
        cnt1 = 0;

        float ci = accI * 0.25f, cq = accQ * 0.25f;
        accI = accQ = 0;
        powerSum += ci * ci + cq * cq;
        ++powerCount;

        float a;
        if (wide)
        {
            // quadrature FM discriminator at 240 kHz
            const float re = ci * prevI + cq * prevQ;
            const float im = cq * prevI - ci * prevQ;
            prevI = ci; prevQ = cq;
            accA += std::atan2 (im, re) * kInvPi * 1.5f;
            if (++cnt2 < 5) continue;
            cnt2 = 0;

            a = accA * 0.2f;
            accA = 0;
            deemph += 0.2425f * (a - deemph);          // 75 us de-emphasis (US broadcast)
            a = deemph;
        }
        else
        {
            ci = chanI[1].run (chanI[0].run (ci));
            cq = chanQ[1].run (chanQ[0].run (cq));
            if (++cnt2 < 5) continue;
            cnt2 = 0;

            if (demod == NFM)
            {
                // Discriminator at 48 kHz, scaled so 5 kHz deviation is about full level.
                const float re = ci * nPrevI + cq * nPrevQ;
                const float im = cq * nPrevI - ci * nPrevQ;
                nPrevI = ci; nPrevQ = cq;
                a = std::atan2 (im, re) * (48000.0f / (6.2831853f * 5000.0f)) * 0.7f;
            }
            else
            {
                // Envelope over the carrier level, so weak and strong stations
                // come out about as loud.
                const float m = std::sqrt (ci * ci + cq * cq);
                amDc += 0.0005f * (m - amDc);
                a = (m - amDc) / (amDc + 1.0e-4f) * 0.7f;
            }

            // Lose the DC, and everything over the audio width.
            audioDc += 0.002f * (a - audioDc);
            audioLp += audioCoef * (a - audioDc - audioLp);
            a = std::clamp (audioLp, -1.5f, 1.5f);
        }

        if (fadeIn < 1.0f)
        {
            a *= fadeIn * fadeIn;
            fadeIn = std::min (1.0f, fadeIn + 1.0f / 960.0f);   // 20 ms
        }
        squelchGain += (squelchTarget - squelchGain) * (1.0f / 240.0f);    // 5 ms
        push (a * squelchGain);
    }

    if (powerCount > 0)
    {
        const float db = 10.0f * std::log10 (powerSum / (float) powerCount + 1.0e-12f);
        const float prev = sigDb.load (std::memory_order_relaxed);
        sigDb.store (prev < -90.0f ? db : prev + 0.3f * (db - prev), std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------- band survey

void SdrReceiver::copySurvey (float* dbOut) const
{
    for (int i = 0; i < kFmChannels; ++i)
        dbOut[i] = surveyDb[i].load (std::memory_order_relaxed);
}

// One tuning covers four channels: the dongle is centred 300 kHz above the
// first, so they sit at -300, -100, +100 and +300 kHz. That keeps them clear
// of the DC spike at the centre and the roll-off at the band edges. Each
// channel's power is the spectrum summed over +-80 kHz around it.
void SdrReceiver::runSurvey (void* d)
{
    auto* dev = static_cast<rtlsdr_dev_t*> (d);
    constexpr int kPerTune = 4;
    constexpr int kTunings = (kFmChannels + kPerTune - 1) / kPerTune;
    Spectrum spectrum;

    // Fixed gain, so channels compare fairly (auto gain would even them out).
    rtlsdr_set_tuner_gain_mode (dev, 1);
    rtlsdr_set_tuner_gain (dev, 297);

    std::vector<uint8_t> raw (65536);
    float results[kFmChannels];
    std::fill (std::begin (results), std::end (results), -200.0f);

    for (int t = 0; t < kTunings && running.load(); ++t)
    {
        surveyProg.store ((float) t / kTunings, std::memory_order_relaxed);
        const int first = t * kPerTune;
        rtlsdr_set_center_freq (dev, (uint32_t) (kFmFirstKHz + first * kFmStepKHz + 300) * 1000u);
        rtlsdr_reset_buffer (dev);

        int got = 0;
        if (rtlsdr_read_sync (dev, raw.data(), 16384, &got) < 0)       // let the tuner settle
            break;
        if (rtlsdr_read_sync (dev, raw.data(), (int) raw.size(), &got) < 0)
            break;

        spectrum.measure (raw.data(), got);
        for (int j = 0; j < kPerTune && first + j < kFmChannels; ++j)
            results[first + j] = (float) (10.0 * std::log10 (spectrum.band (-300000.0 + 200000.0 * j, 80000.0) + 1.0e-9));
    }

    for (int i = 0; i < kFmChannels; ++i)
        surveyDb[i].store (results[i], std::memory_order_relaxed);
    surveyGen.fetch_add (1, std::memory_order_release);
    surveyProg.store (-1.0f, std::memory_order_relaxed);

    // Back to the listener's frequency and gain.
    appliedFreq = 0; appliedGain = -1; appliedAgc = -1;
    rtlsdr_reset_buffer (dev);
}

// ---------------------------------------------------------------- list survey

void SdrReceiver::runListSurvey (void* d)
{
    auto* dev = static_cast<rtlsdr_dev_t*> (d);
    listSurveyCancel.store (false);

    std::vector<Channel> list;
    {
        std::lock_guard<std::mutex> lock (stationsMutex);
        list = surveyList;
    }

    // Fixed gain, so channels compare fairly (auto gain would even them out).
    rtlsdr_set_tuner_gain_mode (dev, 1);
    rtlsdr_set_tuner_gain (dev, 297);

    std::vector<uint8_t> raw (16384);
    std::vector<float> snr (list.size(), -100.0f);
    bool done = true;
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (! running.load() || listSurveyCancel.load()) { done = false; break; }
        listSurveyProg.store ((float) i / (float) list.size(), std::memory_order_relaxed);
        listSurveyAt.store (list[i].hz, std::memory_order_relaxed);
        rtlsdr_set_center_freq (dev, list[i].hz);
        rtlsdr_reset_buffer (dev);
        int got = 0;
        if (rtlsdr_read_sync (dev, raw.data(), 8192, &got) < 0) { done = false; break; }     // let the tuner settle
        if (rtlsdr_read_sync (dev, raw.data(), (int) raw.size(), &got) < 0) { done = false; break; }
        snr[i] = measureSnr (raw.data(), got, list[i].mode);
    }

    if (done)
    {
        // A find is over the threshold and at least as strong as the
        // channels either side of it (when they're its neighbours on the dial).
        const float minSnr = listSurveyMin.load();
        std::vector<Channel> found;
        for (size_t i = 0; i < list.size(); ++i)
        {
            auto beats = [&] (size_t j)
            {
                const uint32_t gap = list[i].hz > list[j].hz ? list[i].hz - list[j].hz : list[j].hz - list[i].hz;
                return gap > 250000u || snr[i] >= snr[j];
            };
            if (snr[i] >= minSnr && (i == 0 || beats (i - 1)) && (i + 1 == list.size() || beats (i + 1)))
                found.push_back (list[i]);
        }
        {
            std::lock_guard<std::mutex> lock (stationsMutex);
            stations = std::move (found);
        }
        listSurveyGen.fetch_add (1, std::memory_order_release);
    }
    listSurveyAt.store (0, std::memory_order_relaxed);
    listSurveyProg.store (-1.0f, std::memory_order_relaxed);

    // Back to the listener's frequency and gain.
    appliedFreq = 0; appliedGain = -1; appliedAgc = -1;
    rtlsdr_reset_buffer (dev);
}

void SdrReceiver::requestListSurvey (const Channel* list, int n, float minSnrDb)
{
    {
        std::lock_guard<std::mutex> lock (stationsMutex);
        surveyList.assign (list, list + std::clamp (n, 0, kMaxChannels));
    }
    listSurveyMin.store (minSnrDb);
    listSurveyRequest.store (true);
}

std::vector<SdrReceiver::Channel> SdrReceiver::listSurveyStations() const
{
    std::lock_guard<std::mutex> lock (stationsMutex);
    return stations;
}

// ---------------------------------------------------------------- squelch scan

void SdrReceiver::setScanList (const Channel* list, int n)
{
    std::lock_guard<std::mutex> lock (listMutex);
    pendingList.assign (list, list + std::clamp (n, 0, kMaxChannels));
    listGen.fetch_add (1);
}

// The channel's power over the noise floor (the median bin of the whole
// 960 kHz spectrum), so it reads the same whatever the gain is.
float SdrReceiver::measureSnr (const uint8_t* data, int n, int demod)
{
    thread_local Spectrum spectrum;
    spectrum.measure (data, n, 8);

    const double halfWidth = demod == FM ? 80000.0 : demod == NFM ? 6000.0 : 5000.0;
    const int bins = (int) (2.0 * halfWidth * Spectrum::kN / kSdrRate) + 1;
    const double channel = spectrum.band (0.0, halfWidth) / bins;

    auto sorted = spectrum.power;
    std::nth_element (sorted.begin(), sorted.begin() + Spectrum::kN / 2, sorted.end());
    const double floor = sorted[Spectrum::kN / 2];
    return (float) (10.0 * std::log10 ((channel + 1.0e-9) / (floor + 1.0e-9)));
}

// One step of the scan: tries the next channel while seeking (~25 ms each),
// or listens to the one it stopped on. Returns false if the dongle is gone.
bool SdrReceiver::squelchStep (void* d, std::vector<uint8_t>& buf)
{
    auto* dev = static_cast<rtlsdr_dev_t*> (d);

    if (listGen.load() != listSeen)
    {
        std::lock_guard<std::mutex> lock (listMutex);
        listSeen = listGen.load();
        scanList = pendingList;
        parked = false;
        // Carry on from where it was rather than from the top.
        const uint32_t here = sqlHz.load() > 0 ? sqlHz.load() : freqHz.load();
        scanPos = -1;
        for (int i = 0; i < (int) scanList.size(); ++i)
            if (scanList[(size_t) i].hz < here) scanPos = i;
    }
    if (scanList.empty())
    {
        sqlState.store (kSeeking);
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
        return true;
    }

    if (skipRequest.exchange (false))
        parked = false;

    int got = 0;
    if (! parked)
    {
        // Seeking: tune the next channel, let it settle, then measure.
        sqlState.store (kSeeking, std::memory_order_relaxed);
        scanPos = (scanPos + 1) % (int) scanList.size();
        const auto ch = scanList[(size_t) scanPos];
        sqlHz.store (ch.hz, std::memory_order_relaxed);
        sqlMode.store (ch.mode, std::memory_order_relaxed);
        applySettings (dev, ch.hz);
        rtlsdr_reset_buffer (dev);

        if (rtlsdr_read_sync (dev, buf.data(), 8192, &got) < 0) return false;      // settle
        if (rtlsdr_read_sync (dev, buf.data(), 16384, &got) < 0) return false;
        const float snr = measureSnr (buf.data(), got, ch.mode);
        snrDb.store (snr, std::memory_order_relaxed);
        if (snr >= sqlDb.load())
        {
            parked = true;
            lastSignal = std::chrono::steady_clock::now();
            sqlState.store (kOpen, std::memory_order_relaxed);
            fadeIn = 0.0f;
        }
        return true;
    }

    // Parked: listen, and keep checking the signal.
    const auto ch = scanList[(size_t) scanPos];
    applySettings (dev, ch.hz);
    if (rtlsdr_read_sync (dev, buf.data(), (int) buf.size(), &got) < 0) return false;
    const float snr = measureSnr (buf.data(), got, ch.mode);
    snrDb.store (snr, std::memory_order_relaxed);

    const auto now = std::chrono::steady_clock::now();
    if (snr >= sqlDb.load() - 1.5f)                     // a little hysteresis
    {
        lastSignal = now;
        sqlState.store (kOpen, std::memory_order_relaxed);
    }
    else if (! sqlHold.load() && now - lastSignal > std::chrono::duration<float> (kHangSeconds))
    {
        parked = false;                                 // gone quiet: move on
        return true;
    }
    else
        sqlState.store (kWaiting, std::memory_order_relaxed);

    process (buf.data(), got, ch.mode);
    return true;
}
