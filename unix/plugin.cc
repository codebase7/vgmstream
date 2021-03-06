#include <cstdlib>
#include <algorithm>

#include <libaudcore/input.h>
#include <libaudcore/plugin.h>
#include <libaudcore/runtime.h>

extern "C" {
#include "../src/vgmstream.h"
}
#include "version.h"
#include "vfs.h"
#include "settings.h"

VGMSTREAM *vgmstream = NULL;
#define CFG_ID "vgmstream"  //ID for storing in audacious

// //default-values for Settings
#define DEFAULT_LOOP_FOREVER "1"
#define DEFAULT_FADE_LENGTH "3"
#define DEFAULT_FADE_DELAY "3"
#define DEFAULT_LOOP_COUNT "2"

static const char* const defaults[] =
{
    "loop_forever", DEFAULT_LOOP_FOREVER,
    "loop_count",   DEFAULT_LOOP_COUNT,
    "fade_length",  DEFAULT_FADE_LENGTH,
    "fade_delay",   DEFAULT_FADE_DELAY,
    NULL
};

bool vgmstream_init()
{
    debugMessage("init");
    vgmstream_cfg_load();
    debugMessage("after load cfg");
    return true;
}

void vgmstream_cleanup()
{
    debugMessage("cleanup");
    vgmstream_cfg_save();
}

void vgmstream_seek(int seek_value, int& current_sample_pos)
{
    debugMessage("seeking");
    // compute from ms to samples
    int seek_needed_samples = (long long)seek_value * vgmstream->sample_rate / 1000L;
    short buffer[576 * vgmstream->channels];
    int max_buffer_samples = sizeof(buffer) / sizeof(buffer[0]) / vgmstream->channels;

    int samples_to_do = 0;
    if (seek_needed_samples < current_sample_pos)
    {
        // go back in time, reopen file
        debugMessage("reopen file to seek backward");
        reset_vgmstream(vgmstream);
        current_sample_pos = 0;
        samples_to_do = seek_needed_samples;
    }
    else if (current_sample_pos < seek_needed_samples)
    {
        // go forward in time
        samples_to_do = seek_needed_samples - current_sample_pos;
    }

    // do the actual seeking
    if (samples_to_do >= 0)
    {
        debugMessage("render forward");

        //render till seeked sample
        while (samples_to_do >0)
        {
            int seek_samples = std::min(max_buffer_samples, samples_to_do);
            current_sample_pos += seek_samples;
            samples_to_do -= seek_samples;
            render_vgmstream(buffer, seek_samples, vgmstream);
        }
        debugMessage("after render vgmstream");
    }
}

bool vgmstream_play(const char * filename, VFSFile * file)
{
    int current_sample_pos = 0;
    int rate;

    debugMessage("start play");
    STREAMFILE* streamfile = open_vfs(filename);

    if (!streamfile)
    {
        printf("failed opening %s\n", filename);
        return false;
    }

    vgmstream = init_vgmstream_from_STREAMFILE(streamfile);
    close_streamfile(streamfile);

    if (!vgmstream || vgmstream->channels <= 0)
    {
        printf("Error::Channels are zero or couldn't init plugin\n");
        if (vgmstream)
            close_vgmstream(vgmstream);
        vgmstream = NULL;
        return false;
    }

    short buffer[576 * vgmstream->channels];
    int max_buffer_samples = sizeof(buffer) / sizeof(buffer[0]) / vgmstream->channels;

    int stream_samples_amount = get_vgmstream_play_samples(vgmstream_cfg.loop_count, vgmstream_cfg.fade_length, vgmstream_cfg.fade_delay, vgmstream);
    rate = get_vgmstream_average_bitrate(vgmstream);

    aud_input_set_bitrate(rate);

    if (!aud_input_open_audio(FMT_S16_LE, vgmstream->sample_rate, 2))
        return false;

    int fade_samples = vgmstream_cfg.fade_length * vgmstream->sample_rate;
    while (!aud_input_check_stop())
    {
        int toget = max_buffer_samples;

        int seek_value = aud_input_check_seek();
        if (seek_value > 0)
            vgmstream_seek(seek_value, current_sample_pos);

        // If we haven't configured the plugin to play forever
        // or the current song is not loopable.
        if (!vgmstream_cfg.loop_forever || !vgmstream->loop_flag)
        {
            if (current_sample_pos >= stream_samples_amount)
                break;
            if (current_sample_pos + toget > stream_samples_amount)
                toget = stream_samples_amount - current_sample_pos;
        }

        render_vgmstream(buffer, toget, vgmstream);

        if (vgmstream->loop_flag && fade_samples > 0 && !vgmstream_cfg.loop_forever)
        {
            int samples_into_fade = current_sample_pos - (stream_samples_amount - fade_samples);
            if (samples_into_fade + toget > 0)
            {
                for (int j = 0; j < toget; j++, samples_into_fade++)
                {
                    if (samples_into_fade > 0)
                    {
                        double fadedness = (double)(fade_samples - samples_into_fade) / fade_samples;
                        for (int k = 0; k < vgmstream->channels; k++)
                            buffer[j * vgmstream->channels + k] *= fadedness;
                    }
                }
            }
        }

        aud_input_write_audio(buffer, toget * sizeof(short) * vgmstream->channels);
        current_sample_pos += toget;
    }

    debugMessage("finished");
    if (vgmstream)
        close_vgmstream(vgmstream);
    vgmstream = NULL;
    return true;
}

// called every time the user adds a new file to playlist
Tuple vgmstream_probe_for_tuple(const char *filename, VFSFile *file)
{
    debugMessage("probe for tuple");
    Tuple tuple;
    int ms;
    int rate;
    VGMSTREAM* vgmstream  = NULL;
    STREAMFILE* streamfile = NULL;

    streamfile = open_vfs(filename);
    vgmstream = init_vgmstream_from_STREAMFILE(streamfile);

    tuple.set_filename(filename);
    rate = vgmstream->sample_rate * 2 * vgmstream->channels;
    tuple.set_int(FIELD_BITRATE, rate);

    ms = get_vgmstream_play_samples(vgmstream_cfg.loop_count, vgmstream_cfg.fade_length, vgmstream_cfg.fade_delay, vgmstream) * 1000LL / vgmstream->sample_rate;
    tuple.set_int(FIELD_LENGTH, ms);

    close_streamfile(streamfile);
    close_vgmstream(vgmstream);

    return tuple;
}

void vgmstream_cfg_load()
{
    debugMessage("cfg_load called");
    aud_config_set_defaults(CFG_ID, defaults);
    vgmstream_cfg.loop_forever = aud_get_bool(CFG_ID, "loop_forever");
    vgmstream_cfg.loop_count = aud_get_int(CFG_ID, "loop_count");
    vgmstream_cfg.fade_length = aud_get_double(CFG_ID, "fade_length");
    vgmstream_cfg.fade_delay = aud_get_double(CFG_ID, "fade_delay");
}

void vgmstream_cfg_save()
{
    debugMessage("cfg_save called");
    aud_set_bool(CFG_ID, "loop_forever", vgmstream_cfg.loop_forever);
    aud_set_int(CFG_ID, "loop_count", vgmstream_cfg.loop_count);
    aud_set_double(CFG_ID, "fade_length", vgmstream_cfg.fade_length);
    aud_set_double(CFG_ID, "fade_delay", vgmstream_cfg.fade_delay);
}
