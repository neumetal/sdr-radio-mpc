// SDR Radio for MPC OS standalone: the radio (radio_core.h, which carries
// the desktop plugin's receiver, scanner and band plan) behind the
// mpc-vst-plugins engine interface (wrapper/engine.h: 44.1 kHz, interleaved
// int16 stereo, 128-frame blocks, string key/value parameters).

#include "radio_core.h"

#include <new>

extern "C" {
#include "engine.h"
}

namespace
{
    struct Radio
    {
        RadioCore core;
        float mono[128] {};
    };

    void* create (const char* dataDir)
    {
        auto* r = new (std::nothrow) Radio();
        if (! r) return nullptr;
        r->core.start (dataDir);                // the shared Saved/Blocked file goes next to the plugin folder
        return r;
    }

    void destroy (void* inst)
    {
        auto* r = static_cast<Radio*> (inst);
        r->core.stop();
        delete r;
    }

    void midi (void* inst, const uint8_t* msg, int len)
    {
        static_cast<Radio*> (inst)->core.midi (msg, len);
    }

    void set_param (void* inst, const char* key, const char* val)
    {
        auto* r = static_cast<Radio*> (inst);
        if (! key || ! val) return;
        if (std::strcmp (key, "state") == 0)
        {
            RadioCore::forEachPair (val, [r] (const std::string& k, const char* v) { r->core.loadKey (k, v); });
            r->core.finishLoad();
            return;
        }
        r->core.setParam (key, val);
    }

    int get_param (void* inst, const char* key, char* buf, int len)
    {
        auto* r = static_cast<Radio*> (inst);
        if (! key) return 0;
        if (std::strcmp (key, "state") == 0)
            return RadioCore::put (buf, len, ("v=1;" + r->core.saveState()).c_str());
        return r->core.getParam (key, buf, len);
    }

    void render (void* inst, int16_t* out, int frames)
    {
        auto* r = static_cast<Radio*> (inst);
        for (int done = 0; done < frames; )
        {
            const int n = std::min (frames - done, 128);
            r->core.render (r->mono, n);
            for (int i = 0; i < n; ++i)
            {
                const int16_t s = (int16_t) std::lround (std::clamp (r->mono[i], -1.0f, 1.0f) * 32767.0f);
                out[2 * (done + i)] = out[2 * (done + i) + 1] = s;
            }
            done += n;
        }
    }

    // The MPC's tempo and song position, for beat scanning (vst.json: HAS_SONG_POS).
    void song_pos (void* inst, double ppq, double bpm, int playing, int)
    {
        static_cast<Radio*> (inst)->core.songPos (ppq, bpm, playing);
    }

    const mpc_engine_t kEngine = { create, destroy, midi, set_param, get_param, render, nullptr, song_pos };
}

extern "C" const mpc_engine_t* mpc_engine (void) { return &kEngine; }

// Values the radio changed itself (step buttons, presets, AUTO, the scanner,
// a project load), so the screen and Q-Links follow (engine.h: mpc_engine_changed).
extern "C" int mpc_engine_changed (void* inst, char* key, int len)
{
    return static_cast<Radio*> (inst)->core.takeChanged (key, len);
}
