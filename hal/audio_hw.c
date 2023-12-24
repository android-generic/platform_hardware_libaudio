/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio_hw_primary"
/*#define LOG_NDEBUG 0*/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>

#include <cutils/log.h>
#include <cutils/properties.h>
#include <cutils/str_parms.h>

#include <hardware/audio.h>
#include <hardware/hardware.h>

#include <system/audio.h>

#include <linux/ioctl.h>
#include <sound/asound.h>
#include <tinyalsa/asoundlib.h>

#include <audio_utils/resampler.h>
#include <audio_utils/format.h>

#include "audio_route.h"

#define PCM_CARD 0
#define PCM_DEVICE 0
#define PCM_DEVICE_SCO 2
#define PCM_DEVICE_HDMI 3

#define OUT_PERIOD_SIZE 512
#define OUT_SHORT_PERIOD_COUNT 2
#define OUT_LONG_PERIOD_COUNT 8
#define OUT_SAMPLING_RATE 48000

#define IN_PERIOD_SIZE 1024
#define IN_PERIOD_COUNT 4
#define IN_SAMPLING_RATE 48000

#define SCO_PERIOD_SIZE 256
#define SCO_PERIOD_COUNT 4
#define SCO_SAMPLING_RATE 8000

/* minimum sleep time in out_write() when write threshold is not reached */
#define MIN_WRITE_SLEEP_US 2000
#define MAX_WRITE_SLEEP_US ((OUT_PERIOD_SIZE * OUT_SHORT_PERIOD_COUNT * 1000000) \
                                / OUT_SAMPLING_RATE)

enum {
    OUT_BUFFER_TYPE_UNKNOWN,
    OUT_BUFFER_TYPE_SHORT,
    OUT_BUFFER_TYPE_LONG,
};

struct pcm_config pcm_config_out = {
    .channels = 2,
    .rate = OUT_SAMPLING_RATE,
    .period_size = OUT_PERIOD_SIZE,
    .period_count = OUT_LONG_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = OUT_PERIOD_SIZE * OUT_SHORT_PERIOD_COUNT,
};

struct pcm_config pcm_config_in = {
    .channels = 2,
    .rate = IN_SAMPLING_RATE,
    .period_size = IN_PERIOD_SIZE,
    .period_count = IN_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = 1,
    .stop_threshold = (IN_PERIOD_SIZE * IN_PERIOD_COUNT),
};

struct pcm_config pcm_config_sco = {
    .channels = 1,
    .rate = SCO_SAMPLING_RATE,
    .period_size = SCO_PERIOD_SIZE,
    .period_count = SCO_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

struct audio_device {
    struct audio_hw_device hw_device;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    unsigned int out_device;
    unsigned int in_device;
    bool standby;
    bool mic_mute;
    struct audio_route *ar;
    bool screen_off;

    struct stream_out *active_out;
    struct stream_in *active_in;
};

struct stream_out {
    struct audio_stream_out stream;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    struct pcm *pcm;
    struct pcm_config pcm_config;
    bool standby;

    struct resampler_itfe *resampler;
    int16_t *buffer;
    size_t buffer_frames;

    int write_threshold;
    int cur_write_threshold;
    int buffer_type;

    struct audio_device *dev;
};

#define MAX_PREPROCESSORS 3
struct stream_in {
    struct audio_stream_in stream;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    struct pcm *pcm;
    struct pcm_config pcm_config;
    bool standby;

    unsigned int requested_rate;
    struct resampler_itfe *resampler;
    struct resampler_buffer_provider buf_provider;
    int16_t *buffer;
    size_t buffer_size;
    size_t frames_in;
    int read_status;

    struct audio_device *dev;

    effect_handle_t preprocessors[MAX_PREPROCESSORS];
    int num_preprocessors;
    int16_t *proc_buf;
    size_t proc_buf_size;
    size_t proc_frames_in;
    int16_t *proc_out_buf;
    size_t proc_out_frames;
};

static uint32_t out_get_sample_rate(const struct audio_stream *stream);
static size_t out_get_buffer_size(const struct audio_stream *stream);
static audio_format_t out_get_format(const struct audio_stream *stream);
static uint32_t in_get_sample_rate(const struct audio_stream *stream);
static size_t in_get_buffer_size(const struct audio_stream *stream);
static audio_format_t in_get_format(const struct audio_stream *stream);
static int get_next_buffer(struct resampler_buffer_provider *buffer_provider,
                                   struct resampler_buffer* buffer);
static void release_buffer(struct resampler_buffer_provider *buffer_provider,
                                  struct resampler_buffer* buffer);

/*
 * NOTE: when multiple mutexes have to be acquired, always take the
 * audio_device mutex first, followed by the stream_in and/or
 * stream_out mutexes.
 */

/* Helper functions */

struct snd_pcm_info *select_card(unsigned int device, unsigned int flags, unsigned int routing)
{
    static struct snd_pcm_info *cached_info[7];
    struct snd_pcm_info *info;
    int is_input = !!(flags & PCM_IN);
    char e = is_input ? 'c' : 'p';

    unsigned int headphone_on = routing & (AUDIO_DEVICE_OUT_WIRED_HEADSET |
                                    AUDIO_DEVICE_OUT_WIRED_HEADPHONE); // out
    unsigned int speaker_on = routing & AUDIO_DEVICE_OUT_SPEAKER; // out
    unsigned int docked = routing & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET; // out
    unsigned int main_mic_on = routing & AUDIO_DEVICE_IN_BUILTIN_MIC; // in
    unsigned int headset_mic_on = routing & AUDIO_DEVICE_IN_WIRED_HEADSET; // in

    int d = 0;
    // 12L, not 11, not 13, gives a weird state of no route on start when headphone is not plugged in
    if(is_input){
        if(!main_mic_on && !headset_mic_on){
            main_mic_on = 1;
        }
        d = main_mic_on ? 3 : d;
        d = headset_mic_on ? 4 : d;
    }else{
        if(!speaker_on && !headphone_on && !docked){
            speaker_on = 1;
        }
        d = speaker_on ? 0 : d;
        d = headphone_on ? 1 : d;
        d = docked ? 2 : d;
    }


    int want_hdmi = property_get_bool("hal.audio.primary.hdmi", device == PCM_DEVICE_HDMI);

    if (!cached_info[d] || (!cached_info[is_input + 5] && want_hdmi)) {
        struct dirent **namelist;
        char path[PATH_MAX] = "/dev/snd/";
        char prop[PROPERTY_VALUE_MAX];
        int n;
        if (want_hdmi && property_get("hal.audio.out.hdmi", prop, NULL)) {
            ALOGI("using hdmi specific card %s from property", prop);
            namelist = malloc(sizeof(struct dirent *));
            namelist[0] = calloc(1, sizeof(struct dirent));
            strncpy(namelist[0]->d_name, prop, sizeof(namelist[0]->d_name) - 1);
            n = 1;
        } else if (!is_input && headphone_on && property_get("hal.audio.out.headphone", prop, NULL)) {
            ALOGI("using headphone specific card %s from property", prop);
            namelist = malloc(sizeof(struct dirent *));
            namelist[0] = calloc(1, sizeof(struct dirent));
            strncpy(namelist[0]->d_name, prop, sizeof(namelist[0]->d_name) - 1);
            n = 1;
        } else if (!is_input && speaker_on && property_get("hal.audio.out.speaker", prop, NULL)) {
            ALOGI("using speaker specific card %s from property", prop);
            namelist = malloc(sizeof(struct dirent *));
            namelist[0] = calloc(1, sizeof(struct dirent));
            strncpy(namelist[0]->d_name, prop, sizeof(namelist[0]->d_name) - 1);
            n = 1;
        } else if (!is_input && docked && property_get("hal.audio.out.dock", prop, NULL)) {
            ALOGI("using dock specific card %s from property", prop);
            namelist = malloc(sizeof(struct dirent *));
            namelist[0] = calloc(1, sizeof(struct dirent));
            strncpy(namelist[0]->d_name, prop, sizeof(namelist[0]->d_name) - 1);
            n = 1;
        } else if (is_input && main_mic_on && property_get("hal.audio.in.mic", prop, NULL)) {
            ALOGI("using mic specific card %s from property", prop);
            namelist = malloc(sizeof(struct dirent *));
            namelist[0] = calloc(1, sizeof(struct dirent));
            strncpy(namelist[0]->d_name, prop, sizeof(namelist[0]->d_name) - 1);
            n = 1;
        } else if (is_input && headset_mic_on && property_get("hal.audio.in.headset", prop, NULL)) {
            ALOGI("using headset mic specific card %s from property", prop);
            namelist = malloc(sizeof(struct dirent *));
            namelist[0] = calloc(1, sizeof(struct dirent));
            strncpy(namelist[0]->d_name, prop, sizeof(namelist[0]->d_name) - 1);
            n = 1;
        } else if (property_get(is_input ? "hal.audio.in" : "hal.audio.out", prop, NULL)) {
            ALOGI("using %s from property", prop);
            namelist = malloc(sizeof(struct dirent *));
            namelist[0] = calloc(1, sizeof(struct dirent));
            strncpy(namelist[0]->d_name, prop, sizeof(namelist[0]->d_name) - 1);
            n = 1;
        } else {
            n = scandir(path, &namelist, NULL, alphasort);
        }
        if (n >= 0) {
            int i, fd;
            for (i = 0; i < n; i++) {
                struct dirent *de = namelist[i];
                if (!strncmp(de->d_name, "pcmC", 4) && de->d_name[strlen(de->d_name) - 1] == e) {
                    strcpy(path + 9, de->d_name);
                    if ((fd = open(path, O_RDWR)) >= 0) {
                        info = malloc(sizeof(*info));
                        if (!ioctl(fd, SNDRV_PCM_IOCTL_INFO, info)) {
                            if (info->stream == is_input && /* ignore IntelHDMI */
                                    !strstr((const char *)info->id, "IntelHDMI")) {
                                ALOGD("found audio %s at %s\ncard: %d/%d id: %s\nname: %s\nsubname: %s\nstream: %d",
                                        is_input ? "in" : "out", path,
                                        info->card, info->device, info->id,
                                        info->name, info->subname, info->stream);
                                int hdmi = (!!strcasestr((const char *)info->id, "HDMI")) * 2;
                                if (cached_info[hdmi ? 5 + is_input : d]) {
                                    ALOGD("ignore %s", de->d_name);
                                    free(info);
                                } else {
                                    cached_info[hdmi ? 5 + is_input : d] = info;
                                }
                            }
                        } else {
                            ALOGV("can't get info of %s", path);
                            free(info);
                        }
                        close(fd);
                    }
                }
                free(de);
            }
            free(namelist);
        }
    }
    if (want_hdmi && cached_info[5 + is_input]) {
        info = cached_info[5 + is_input];
    } else {
        info = cached_info[d] ? cached_info[d] : cached_info[d + 2];
    }
    ALOGI_IF(info, "chose pcmC%dD%d%c for %d on cache slot %d", info->card, info->device, is_input ? 'c' : 'p', device, d);
    return info;
}

int get_format_from_prop(const char *prop){
    int format = property_get_int32(prop, PCM_FORMAT_S16_LE);
    if(format != PCM_FORMAT_S16_LE && format != PCM_FORMAT_S32_LE && format != PCM_FORMAT_S8){
        ALOGW("format %d from %s is ignored", format, prop);
        format = PCM_FORMAT_S16_LE;
    }
    return format;
}

pthread_mutex_t prop_command_lock;
int run_prop_command(const char *command){
    pthread_mutex_lock(&prop_command_lock);
    int ret = system(command);
    pthread_mutex_unlock(&prop_command_lock);
    return ret;
}

void last_ditch_card_and_format_adjustments(unsigned int routing, struct pcm_config *config, int is_input){
    int want_hdmi = property_get_bool("hal.audio.primary.hdmi", !!(routing & AUDIO_DEVICE_OUT_AUX_DIGITAL));
    unsigned int headphone_on = routing & (AUDIO_DEVICE_OUT_WIRED_HEADSET |
                                    AUDIO_DEVICE_OUT_WIRED_HEADPHONE); // out
    unsigned int speaker_on = routing & AUDIO_DEVICE_OUT_SPEAKER; // out
    unsigned int docked = routing & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET; // out
    unsigned int main_mic_on = routing & AUDIO_DEVICE_IN_BUILTIN_MIC; // in
    unsigned int headset_mic_on = routing & AUDIO_DEVICE_IN_WIRED_HEADSET; // in
    char prop[PROPERTY_VALUE_MAX];

    // 12L, not 11, not 13, gives a weird state of no route on start when headphone is not plugged in
    if(is_input){
        if(!main_mic_on && !headset_mic_on){
            main_mic_on = 1;
        }
    }else{
        if(!headphone_on && !speaker_on && !docked){
            speaker_on = 1;
        }
    }

    if(want_hdmi){
        const char *command_key = is_input ? "hal.audio.in.hdmi.command" : "hal.audio.out.hdmi.command";
        const char *format_key = is_input ? "hal.audio.in.hdmi.format" : "hal.audio.out.hdmi.format";
        if (property_get(command_key, prop, NULL)) {
            ALOGI("running bringup command {%s} for hdmi", prop);
            run_prop_command(prop);
        }
        config->format = get_format_from_prop(format_key);
    }
    if (!is_input && headphone_on) {
        if (property_get("hal.audio.out.headphone.command", prop, NULL)) {
            ALOGI("running bringup command {%s} for headphone", prop);
            run_prop_command(prop);
        }
        config->format = get_format_from_prop("hal.audio.out.headphone.format");
    }
    if (!is_input && speaker_on) {
        if (property_get("hal.audio.out.speaker.command", prop, NULL)) {
            ALOGI("running bringup command {%s} for speaker", prop);
            run_prop_command(prop);
        }
        config->format = get_format_from_prop("hal.audio.out.speaker.format");
    }
    if (!is_input && docked) {
        if (property_get("hal.audio.out.dock.command", prop, NULL)) {
            ALOGI("running bringup command {%s} for dock", prop);
            run_prop_command(prop);
        }
        config->format = get_format_from_prop("hal.audio.out.dock.format");
    }
    if (is_input && main_mic_on) {
        if (property_get("hal.audio.in.mic.command", prop, NULL)) {
            ALOGI("running bringup command {%s} for mic", prop);
            run_prop_command(prop);
        }
        config->format = get_format_from_prop("hal.audio.in.mic.format");
    }
    if (is_input && headset_mic_on) {
        if (property_get("hal.audio.in.headset.command", prop, NULL)) {
            ALOGI("running bringup command {%s} for headset mic", prop);
            run_prop_command(prop);
        }
        config->format = get_format_from_prop("hal.audio.in.headset.format");
    }
}

struct pcm *my_pcm_open(unsigned int device, unsigned int flags, struct pcm_config *config, unsigned int routing)
{
    struct snd_pcm_info *info = select_card(device, flags, routing);
    if (!info) {
        ALOGE("unable to find a sound card");
        return NULL;
    }

    last_ditch_card_and_format_adjustments(routing, config, flags & PCM_IN);

    struct pcm *pcm = pcm_open(info->card, info->device, flags, config);
    if (pcm && !pcm_is_ready(pcm)) {
        ALOGE("my_pcm_open(%d) failed: %s", flags, pcm_get_error(pcm));
        pcm_close(pcm);
        ALOGI("my_pcm_open: re-try 44100 on card %d/%d", info->card, info->device);
        config->rate = 44100;
        pcm = pcm_open(info->card, info->device, flags, config);
    }
    return pcm;
}

static void select_devices(struct audio_device *adev)
{
    int headphone_on;
    int speaker_on;
    int docked;
    int main_mic_on;
    int headset_mic_on;

    headphone_on = adev->out_device & (AUDIO_DEVICE_OUT_WIRED_HEADSET |
                                    AUDIO_DEVICE_OUT_WIRED_HEADPHONE);
    speaker_on = adev->out_device & AUDIO_DEVICE_OUT_SPEAKER;
    docked = adev->out_device & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET;
    main_mic_on = adev->in_device & AUDIO_DEVICE_IN_BUILTIN_MIC;
    headset_mic_on = adev->in_device & AUDIO_DEVICE_IN_WIRED_HEADSET;

    reset_mixer_state(adev->ar);

    if (speaker_on)
        audio_route_apply_path(adev->ar, "speaker");
    if (headphone_on)
        audio_route_apply_path(adev->ar, "headphone");
    if (docked)
        audio_route_apply_path(adev->ar, "dock");
    if (main_mic_on)
        audio_route_apply_path(adev->ar, "main-mic");
    if (headset_mic_on)
        audio_route_apply_path(adev->ar, "headset-mic");

    update_mixer_state(adev->ar);

    ALOGV("hp=%c speaker=%c dock=%c main-mic=%c headset-mic=%c", headphone_on ? 'y' : 'n',
          speaker_on ? 'y' : 'n', docked ? 'y' : 'n', main_mic_on ? 'y' : 'n', headset_mic_on ? 'y' : 'n' );
}

/* must be called with hw device and output stream mutexes locked */
static void do_out_standby(struct stream_out *out)
{
    struct audio_device *adev = out->dev;

    if (!out->standby) {
        pcm_close(out->pcm);
        out->pcm = NULL;
        adev->active_out = NULL;
        if (out->resampler) {
            release_resampler(out->resampler);
            out->resampler = NULL;
        }
        if (out->buffer) {
            free(out->buffer);
            out->buffer = NULL;
        }
        out->standby = true;
    }
}

/* must be called with hw device and input stream mutexes locked */
static void do_in_standby(struct stream_in *in)
{
    struct audio_device *adev = in->dev;

    if (!in->standby) {
        pcm_close(in->pcm);
        in->pcm = NULL;
        adev->active_in = NULL;
        if (in->resampler) {
            release_resampler(in->resampler);
            in->resampler = NULL;
        }
        if (in->buffer) {
            free(in->buffer);
            in->buffer = NULL;
        }
        if (in->proc_buf) {
            free(in->proc_buf);
            in->proc_buf = NULL;
            in->proc_buf_size = 0;
        }
        if (in->proc_out_buf) {
            free(in->proc_out_buf);
            in->proc_out_buf = NULL;
            in->proc_out_frames = 0;
        }
        in->standby = true;
    }
}

/* must be called with hw device and output stream mutexes locked */
static int start_output_stream(struct stream_out *out)
{
    struct audio_device *adev = out->dev;
    unsigned int device;
    int ret;

    /*
     * Due to the lack of sample rate converters in the SoC,
     * it greatly simplifies things to have only the main
     * (speaker/headphone) PCM or the BC SCO PCM open at
     * the same time.
     */
    if (adev->out_device & AUDIO_DEVICE_OUT_ALL_SCO) {
        device = PCM_DEVICE_SCO;
        out->pcm_config = pcm_config_sco;
    } else {
        device = (adev->out_device & AUDIO_DEVICE_OUT_AUX_DIGITAL) ? PCM_DEVICE_HDMI : PCM_DEVICE;
        out->pcm_config = pcm_config_out;
        out->buffer_type = OUT_BUFFER_TYPE_UNKNOWN;
    }

    /*
     * All open PCMs can only use a single group of rates at once:
     * Group 1: 11.025, 22.05, 44.1
     * Group 2: 8, 16, 32, 48
     * Group 1 is used for digital audio playback since 44.1 is
     * the most common rate, but group 2 is required for SCO.
     */
    if (adev->active_in) {
        struct stream_in *in = adev->active_in;
        pthread_mutex_lock(&in->lock);
        if (((out->pcm_config.rate % 8000 == 0) &&
                 (in->pcm_config.rate % 8000) != 0) ||
                 ((out->pcm_config.rate % 11025 == 0) &&
                 (in->pcm_config.rate % 11025) != 0))
            do_in_standby(in);
        pthread_mutex_unlock(&in->lock);
    }

    out->pcm = my_pcm_open(device, PCM_OUT | PCM_NORESTART, &(out->pcm_config), adev->out_device);
    if (!out->pcm) {
        return -ENODEV;
    } else if (!pcm_is_ready(out->pcm)) {
        ALOGE("pcm_open(out) failed: %s", pcm_get_error(out->pcm));
        pcm_close(out->pcm);
        return -ENOMEM;
    }

    /*
     * If the stream rate differs from the PCM rate, we need to
     * create a resampler.
     */
    if (out_get_sample_rate(&out->stream.common) != out->pcm_config.rate) {
        ret = create_resampler(out_get_sample_rate(&out->stream.common),
                               out->pcm_config.rate,
                               out->pcm_config.channels,
                               RESAMPLER_QUALITY_DEFAULT,
                               NULL,
                               &out->resampler);
        out->buffer_frames = (pcm_config_out.period_size * out->pcm_config.rate) /
                out_get_sample_rate(&out->stream.common) + 1;

        out->buffer = malloc(pcm_frames_to_bytes(out->pcm, out->buffer_frames));
    }

    adev->active_out = out;

    return 0;
}

/* must be called with hw device and input stream mutexes locked */
static int start_input_stream(struct stream_in *in)
{
    struct audio_device *adev = in->dev;
    unsigned int device;
    int ret;

    /*
     * Due to the lack of sample rate converters in the SoC,
     * it greatly simplifies things to have only the main
     * mic PCM or the BC SCO PCM open at the same time.
     */
    if (adev->in_device & AUDIO_DEVICE_IN_ALL_SCO) {
        device = PCM_DEVICE_SCO;
        in->pcm_config = pcm_config_sco;
    } else {
        device = PCM_DEVICE;
        in->pcm_config = pcm_config_in;
    }

    /*
     * All open PCMs can only use a single group of rates at once:
     * Group 1: 11.025, 22.05, 44.1
     * Group 2: 8, 16, 32, 48
     * Group 1 is used for digital audio playback since 44.1 is
     * the most common rate, but group 2 is required for SCO.
     */
    if (adev->active_out) {
        struct stream_out *out = adev->active_out;
        pthread_mutex_lock(&out->lock);
        if (((in->pcm_config.rate % 8000 == 0) &&
                 (out->pcm_config.rate % 8000) != 0) ||
                 ((in->pcm_config.rate % 11025 == 0) &&
                 (out->pcm_config.rate % 11025) != 0))
            do_out_standby(out);
        pthread_mutex_unlock(&out->lock);
    }

    in->pcm = my_pcm_open(device, PCM_IN, &(in->pcm_config), adev->in_device);
    if (!in->pcm) {
        return -ENODEV;
    } else if (!pcm_is_ready(in->pcm)) {
        ALOGE("pcm_open(in) failed: %s", pcm_get_error(in->pcm));
        pcm_close(in->pcm);
        return -ENOMEM;
    }

    /*
     * If the stream rate differs from the PCM rate, we need to
     * create a resampler.
     */
    if (in_get_sample_rate(&in->stream.common) != in->pcm_config.rate) {
        in->buf_provider.get_next_buffer = get_next_buffer;
        in->buf_provider.release_buffer = release_buffer;

        ret = create_resampler(in->pcm_config.rate,
                               in_get_sample_rate(&in->stream.common),
                               1,
                               RESAMPLER_QUALITY_DEFAULT,
                               &in->buf_provider,
                               &in->resampler);
    }
    in->buffer_size = pcm_frames_to_bytes(in->pcm,
                                          in->pcm_config.period_size);
    if(in->pcm_config.format == PCM_FORMAT_S8){
        // make space for growing it to 16bit format
        in->buffer_size = in->buffer_size * 2;
    }
    in->buffer = malloc(in->buffer_size);
    in->frames_in = 0;

    adev->active_in = in;

    return 0;
}

static int get_next_buffer(struct resampler_buffer_provider *buffer_provider,
                                   struct resampler_buffer* buffer)
{
    struct stream_in *in;

    if (buffer_provider == NULL || buffer == NULL)
        return -EINVAL;

    in = (struct stream_in *)((char *)buffer_provider -
                                   offsetof(struct stream_in, buf_provider));

    if (in->pcm == NULL) {
        buffer->raw = NULL;
        buffer->frame_count = 0;
        in->read_status = -ENODEV;
        return -ENODEV;
    }

    if (in->frames_in == 0) {
        unsigned int read_size = in->buffer_size;
        if(in->pcm_config.format == PCM_FORMAT_S8){
            read_size = read_size / 2;
        }
        in->read_status = pcm_read(in->pcm,
                                   (void*)in->buffer,
                                   read_size);
        if (in->read_status != 0) {
            ALOGE("get_next_buffer() pcm_read error %d", in->read_status);
            buffer->raw = NULL;
            buffer->frame_count = 0;
            return in->read_status;
        }

        // if not 16bit, make it 16bit
        if(in->pcm_config.format == PCM_FORMAT_S32_LE){
            memcpy_by_audio_format((void*)in->buffer, AUDIO_FORMAT_PCM_16_BIT, (void*)in->buffer, AUDIO_FORMAT_PCM_32_BIT, in->pcm_config.period_size * in->pcm_config.channels);
        }
        if(in->pcm_config.format == PCM_FORMAT_S8){
            memcpy_by_audio_format((void*)in->buffer, AUDIO_FORMAT_PCM_16_BIT, (void*)in->buffer, AUDIO_FORMAT_PCM_8_BIT, in->pcm_config.period_size * in->pcm_config.channels);
        }

        in->frames_in = in->pcm_config.period_size;
        if (in->pcm_config.channels == 2) {
            unsigned int i;

            /* Discard right channel */
            for (i = 1; i < in->frames_in; i++)
                in->buffer[i] = in->buffer[i * 2];
        }
    }

    buffer->frame_count = (buffer->frame_count > in->frames_in) ?
                                in->frames_in : buffer->frame_count;
    buffer->i16 = in->buffer + (in->pcm_config.period_size - in->frames_in);

    return in->read_status;

}

static void release_buffer(struct resampler_buffer_provider *buffer_provider,
                                  struct resampler_buffer* buffer)
{
    struct stream_in *in;

    if (buffer_provider == NULL || buffer == NULL)
        return;

    in = (struct stream_in *)((char *)buffer_provider -
                                   offsetof(struct stream_in, buf_provider));

    in->frames_in -= buffer->frame_count;
}

/* read_frames() reads frames from kernel driver, down samples to capture rate
 * if necessary and output the number of frames requested to the buffer specified */
static ssize_t read_frames(struct stream_in *in, void *buffer, ssize_t frames)
{
    ssize_t frames_wr = 0;

    while (frames_wr < frames) {
        size_t frames_rd = frames - frames_wr;
        if (in->resampler != NULL) {
            in->resampler->resample_from_provider(in->resampler,
                    (int16_t *)((char *)buffer +
                    frames_wr * audio_stream_in_frame_size(&in->stream)),
                    &frames_rd);
        } else {
            struct resampler_buffer buf = {
                    { .raw = NULL, },
                    .frame_count = frames_rd,
            };
            get_next_buffer(&in->buf_provider, &buf);
            if (buf.raw != NULL) {
                memcpy((char *)buffer +
                        frames_wr * audio_stream_in_frame_size(&in->stream),
                        buf.raw,
                        buf.frame_count * audio_stream_in_frame_size(&in->stream));
                frames_rd = buf.frame_count;
            }
            release_buffer(&in->buf_provider, &buf);
        }
        /* in->read_status is updated by getNextBuffer() also called by
         * in->resampler->resample_from_provider() */
        if (in->read_status != 0)
            return in->read_status;

        frames_wr += frames_rd;
    }
    return frames_wr;
}

static ssize_t process_frames(struct stream_in *in, void* buffer, ssize_t frames)
{
    ssize_t frames_wr = 0;
    audio_buffer_t in_buf;
    audio_buffer_t out_buf;
    int i;

    /* PreProcessing library can only operates on 10ms chunks.
     * FIXME: Sampling rate that are not multiple of 100 should probably be forbidden... */
    size_t proc_frames_count = in_get_sample_rate(&in->stream.common) / 100;
    /* Number of required input frames, must be a multiple of proc_frames_count. */
    size_t frames_rq = (((size_t)frames + (proc_frames_count - 1)) / proc_frames_count) * proc_frames_count;

    /* Use frames from previous run, if any.
     * NOTE: This should never be more than wanted number of frames. */
    if (in->proc_out_frames) {
        memcpy(buffer,
               in->proc_out_buf,
               in->proc_out_frames * sizeof(int16_t));
        frames_wr = in->proc_out_frames;
        in->proc_out_frames = 0;
    }

    while (frames_wr < frames) {
        /* first reload enough frames at the end of process input buffer */
        if (in->proc_frames_in < frames_rq) {
            ssize_t frames_rd;

            if (in->proc_buf_size < frames_rq) {
                in->proc_buf_size = frames_rq;
                in->proc_buf = (int16_t *)realloc(in->proc_buf, in->proc_buf_size * sizeof(int16_t));
                ALOGV("process_frames(): in->proc_buf %p size extended to %zu frames",
                      in->proc_buf, in->proc_buf_size);
            }
            frames_rd = read_frames(in,
                                    in->proc_buf + in->proc_frames_in,
                                    frames_rq - in->proc_frames_in);
            if (frames_rd < 0) {
                frames_wr = frames_rd;
                break;
            }

            in->proc_frames_in += frames_rd;
        }

        if (!in->proc_out_buf) {
            in->proc_out_buf = (int16_t*)malloc(proc_frames_count * sizeof(int16_t));
        }

        /* in_buf.frameCount and out_buf.frameCount indicate respectively
         * the maximum number of frames to be consumed and produced by process(),
         * must be proc_frames_count */
        in_buf.frameCount = proc_frames_count;
        in_buf.s16 = in->proc_buf;
        out_buf.frameCount = proc_frames_count;
        out_buf.s16 = in->proc_out_buf;

        /* FIXME: this works because of current pre processing library implementation that
         * does the actual process only when the last enabled effect process is called.
         * The generic solution is to have an output buffer for each effect and pass it as
         * input to the next. */
        for (i = 0; i < in->num_preprocessors; i++) {
            (*in->preprocessors[i])->process(in->preprocessors[i], &in_buf, &out_buf);
        }

        /* process() has updated the number of frames consumed and produced in
         * in_buf.frameCount and out_buf.frameCount respectively
         * move remaining frames to the beginning of in->proc_buf_in */
        in->proc_frames_in -= in_buf.frameCount;
        if (in->proc_frames_in) {
            memmove(in->proc_buf,
                    in->proc_buf + in_buf.frameCount,
                    in->proc_frames_in * sizeof(int16_t));
        }

        /* if not enough frames were passed to process(), read more and retry. */
        if (out_buf.frameCount == 0)
            continue;

        if ((frames_wr + (ssize_t)out_buf.frameCount) <= frames) {
            memcpy((int16_t *)buffer + frames_wr,
                   out_buf.s16,
                   out_buf.frameCount * sizeof(int16_t));
            frames_wr += out_buf.frameCount;
        } else {
            memcpy((int16_t *)buffer + frames_wr,
                   out_buf.s16,
                   (frames - frames_wr) * sizeof(int16_t));
            in->proc_out_frames = out_buf.frameCount - (frames - frames_wr);
            memmove(in->proc_out_buf,
                    in->proc_out_buf + (frames - frames_wr),
                    in->proc_out_frames * sizeof(int16_t));
            frames_wr = frames;
        }
    }

    return frames_wr;
}

/* API functions */

static uint32_t out_get_sample_rate(const struct audio_stream *stream __unused)
{
    return pcm_config_out.rate;
}

static int out_set_sample_rate(struct audio_stream *stream __unused, uint32_t rate __unused)
{
    return -ENOSYS;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    return pcm_config_out.period_size *
               audio_stream_out_frame_size((struct audio_stream_out *)stream);
}

static uint32_t out_get_channels(const struct audio_stream *stream __unused)
{
    return AUDIO_CHANNEL_OUT_STEREO;
}

static audio_format_t out_get_format(const struct audio_stream *stream __unused)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int out_set_format(struct audio_stream *stream __unused, audio_format_t format __unused)
{
    return -ENOSYS;
}

static int out_standby(struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);
    do_out_standby(out);
    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);

    return 0;
}

static int out_dump(const struct audio_stream *stream __unused, int fd __unused)
{
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    struct str_parms *parms;
    char value[32];
    int len;
    unsigned int val;

    parms = str_parms_create_str(kvpairs);

    len = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING,
                            value, sizeof(value));
    pthread_mutex_lock(&adev->lock);
    if (len >= 0) {
        val = atoi(value);
        if ((adev->out_device != val) && (val != 0)) {
            /*
             * If SCO is turned on/off, we need to put audio into standby
             * because SCO uses a different PCM.
             */
            if ((val & AUDIO_DEVICE_OUT_ALL_SCO) ^
                    (adev->out_device & AUDIO_DEVICE_OUT_ALL_SCO)) {
                pthread_mutex_lock(&out->lock);
                do_out_standby(out);
                pthread_mutex_unlock(&out->lock);
            }

            adev->out_device = val;
            select_devices(adev);
            // go into standby in case the route is on another card
            pthread_mutex_lock(&out->lock);
            if(!out->standby){
                do_out_standby(out);
            }
            pthread_mutex_unlock(&out->lock);
        }
    }
    pthread_mutex_unlock(&adev->lock);

    str_parms_destroy(parms);
    return 0;
}

static char *out_get_parameters(const struct audio_stream *stream __unused, const char *keys __unused)
{
    return strdup("");
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    size_t period_count;

    pthread_mutex_lock(&adev->lock);

    if (adev->screen_off && !adev->active_in && !(adev->out_device & AUDIO_DEVICE_OUT_ALL_SCO))
        period_count = OUT_LONG_PERIOD_COUNT;
    else
        period_count = OUT_SHORT_PERIOD_COUNT;

    pthread_mutex_unlock(&adev->lock);

    return (pcm_config_out.period_size * period_count * 1000) / pcm_config_out.rate;
}

static int out_set_volume(struct audio_stream_out *stream __unused, float left __unused,
                          float right __unused)
{
    return -ENOSYS;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
                         size_t bytes)
{
    int ret = 0;
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    size_t frame_size = audio_stream_out_frame_size(stream);
    int16_t *in_buffer = (int16_t *)buffer;
    size_t in_frames = bytes / frame_size;
    size_t out_frames;
    int buffer_type;
    int kernel_frames;
    bool sco_on;

    /*
     * acquiring hw device mutex systematically is useful if a low
     * priority thread is waiting on the output stream mutex - e.g.
     * executing out_set_parameters() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&out->lock);
    if (out->standby) {
        ret = start_output_stream(out);
        if (ret != 0) {
            pthread_mutex_unlock(&adev->lock);
            goto exit;
        }
        out->standby = false;
    }
    buffer_type = (adev->screen_off && !adev->active_in) ?
            OUT_BUFFER_TYPE_LONG : OUT_BUFFER_TYPE_SHORT;
    sco_on = (adev->out_device & AUDIO_DEVICE_OUT_ALL_SCO);
    pthread_mutex_unlock(&adev->lock);

    /* detect changes in screen ON/OFF state and adapt buffer size
     * if needed. Do not change buffer size when routed to SCO device. */
    if (!sco_on && (buffer_type != out->buffer_type)) {
        size_t period_count;

        if (buffer_type == OUT_BUFFER_TYPE_LONG)
            period_count = OUT_LONG_PERIOD_COUNT;
        else
            period_count = OUT_SHORT_PERIOD_COUNT;

        out->write_threshold = out->pcm_config.period_size * period_count;
        /* reset current threshold if exiting standby */
        if (out->buffer_type == OUT_BUFFER_TYPE_UNKNOWN)
            out->cur_write_threshold = out->write_threshold;
        out->buffer_type = buffer_type;
    }

    /* Reduce number of channels, if necessary */
    if (popcount(out_get_channels(&stream->common)) >
                 (int)out->pcm_config.channels) {
        unsigned int i;

        /* Discard right channel */
        for (i = 1; i < in_frames; i++)
            in_buffer[i] = in_buffer[i * 2];

        /* The frame size is now half */
        frame_size /= 2;
    }

    /* Change sample rate, if necessary */
    if (out_get_sample_rate(&stream->common) != out->pcm_config.rate) {
        out_frames = out->buffer_frames;
        out->resampler->resample_from_input(out->resampler,
                                            in_buffer, &in_frames,
                                            out->buffer, &out_frames);
        in_buffer = out->buffer;
    } else {
        out_frames = in_frames;
    }

    if (!sco_on) {
        int total_sleep_time_us = 0;
        size_t period_size = out->pcm_config.period_size;

        /* do not allow more than out->cur_write_threshold frames in kernel
         * pcm driver buffer */
        do {
            struct timespec time_stamp;
            if (pcm_get_htimestamp(out->pcm,
                                   (unsigned int *)&kernel_frames,
                                   &time_stamp) < 0) {
                kernel_frames = 0; /* assume no space is available */
                break;
            }
            kernel_frames = pcm_get_buffer_size(out->pcm) - kernel_frames;

            if (kernel_frames > out->cur_write_threshold) {
                int sleep_time_us =
                    (int)(((int64_t)(kernel_frames - out->cur_write_threshold)
                                    * 1000000) / out->pcm_config.rate);
                if (sleep_time_us < MIN_WRITE_SLEEP_US)
                    break;
                total_sleep_time_us += sleep_time_us;
                if (total_sleep_time_us > MAX_WRITE_SLEEP_US) {
                    ALOGV("out_write() limiting sleep time %d to %d",
                          total_sleep_time_us, MAX_WRITE_SLEEP_US);
                    sleep_time_us = MAX_WRITE_SLEEP_US -
                                        (total_sleep_time_us - sleep_time_us);
                }
                usleep(sleep_time_us);
            }

        } while ((kernel_frames > out->cur_write_threshold) &&
                (total_sleep_time_us <= MAX_WRITE_SLEEP_US));

        /* do not allow abrupt changes on buffer size. Increasing/decreasing
         * the threshold by steps of 1/4th of the buffer size keeps the write
         * time within a reasonable range during transitions.
         * Also reset current threshold just above current filling status when
         * kernel buffer is really depleted to allow for smooth catching up with
         * target threshold.
         */
        if (out->cur_write_threshold > out->write_threshold) {
            out->cur_write_threshold -= period_size / 4;
            if (out->cur_write_threshold < out->write_threshold) {
                out->cur_write_threshold = out->write_threshold;
            }
        } else if (out->cur_write_threshold < out->write_threshold) {
            out->cur_write_threshold += period_size / 4;
            if (out->cur_write_threshold > out->write_threshold) {
                out->cur_write_threshold = out->write_threshold;
            }
        } else if ((kernel_frames < out->write_threshold) &&
            ((out->write_threshold - kernel_frames) >
                (int)(period_size * OUT_SHORT_PERIOD_COUNT))) {
            out->cur_write_threshold = (kernel_frames / period_size + 1) * period_size;
            out->cur_write_threshold += period_size / 4;
        }
    }

    if (out->pcm_config.format == PCM_FORMAT_S32_LE) {
        unsigned int new_buffer_size = out_frames * frame_size * 2;
        uint8_t resize_buffer[new_buffer_size];
        memcpy_by_audio_format((void*)resize_buffer, AUDIO_FORMAT_PCM_32_BIT, (void*)in_buffer, AUDIO_FORMAT_PCM_16_BIT, out_frames * frame_size / 2);
        ret = pcm_write(out->pcm, resize_buffer, new_buffer_size);
    } else if(out->pcm_config.format == PCM_FORMAT_S8) {
        unsigned int new_buffer_size = out_frames * frame_size / 2;
        uint8_t resize_buffer[new_buffer_size];
        memcpy_by_audio_format((void*)resize_buffer, AUDIO_FORMAT_PCM_8_BIT, (void*)in_buffer, AUDIO_FORMAT_PCM_16_BIT, out_frames * frame_size / 2);
        ret = pcm_write(out->pcm, resize_buffer, new_buffer_size);
    } else {
        ret = pcm_write(out->pcm, in_buffer, out_frames * frame_size);
    }
    if (ret == -EPIPE) {
        /* In case of underrun, don't sleep since we want to catch up asap */
        pthread_mutex_unlock(&out->lock);
        ALOGW("out_write underrun: %d", ret);
        return ret;
    }

exit:
    pthread_mutex_unlock(&out->lock);

    if (ret != 0) {
        ALOGW("out_write error: %d, sleeping...", ret);
        usleep(bytes * 1000000 / audio_stream_out_frame_size(stream) /
               out_get_sample_rate(&stream->common));
    }

    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream __unused,
                                   uint32_t *dsp_frames __unused)
{
    return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream __unused, effect_handle_t effect __unused)
{
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream __unused, effect_handle_t effect __unused)
{
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream __unused,
                                        int64_t *timestamp __unused)
{
    return -EINVAL;
}

/** audio_stream_in implementation **/
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    return in->requested_rate;
}

static int in_set_sample_rate(struct audio_stream *stream __unused, uint32_t rate __unused)
{
    return 0;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;
    size_t size;

    /*
     * take resampling into account and return the closest majoring
     * multiple of 16 frames, as audioflinger expects audio buffers to
     * be a multiple of 16 frames
     */
    size = (in->pcm_config.period_size * in_get_sample_rate(stream)) /
            in->pcm_config.rate;
    size = ((size + 15) / 16) * 16;

    return size * audio_stream_in_frame_size(&in->stream);
}

static uint32_t in_get_channels(const struct audio_stream *stream __unused)
{
    return AUDIO_CHANNEL_IN_MONO;
}

static audio_format_t in_get_format(const struct audio_stream *stream __unused)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream __unused, audio_format_t format __unused)
{
    return -ENOSYS;
}

static int in_standby(struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);
    do_in_standby(in);
    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);

    return 0;
}

static int in_dump(const struct audio_stream *stream __unused, int fd __unused)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    struct str_parms *parms;
    char value[32];
    int len;
    unsigned int val;

    parms = str_parms_create_str(kvpairs);

    len = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING,
                            value, sizeof(value));
    pthread_mutex_lock(&adev->lock);
    if (len >= 0) {
        val = atoi(value) & ~AUDIO_DEVICE_BIT_IN;
        if ((adev->in_device != val) && (val != 0)) {
            /*
             * If SCO is turned on/off, we need to put audio into standby
             * because SCO uses a different PCM.
             */
            if ((val & AUDIO_DEVICE_IN_ALL_SCO) ^
                    (adev->in_device & AUDIO_DEVICE_IN_ALL_SCO)) {
                pthread_mutex_lock(&in->lock);
                do_in_standby(in);
                pthread_mutex_unlock(&in->lock);
            }

            adev->in_device = val;
            select_devices(adev);
            // go into standby in case the route is on another card
            pthread_mutex_lock(&in->lock);
            if(!in->standby){
                do_in_standby(in);
            }
            pthread_mutex_unlock(&in->lock);
        }
    }
    pthread_mutex_unlock(&adev->lock);

    str_parms_destroy(parms);
    return 0;
}

static char * in_get_parameters(const struct audio_stream *stream __unused,
                                const char *keys __unused)
{
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream __unused, float gain __unused)
{
    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
                       size_t bytes)
{
    int ret = 0;
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    size_t frames_rq = bytes / audio_stream_in_frame_size(stream);

    /*
     * acquiring hw device mutex systematically is useful if a low
     * priority thread is waiting on the input stream mutex - e.g.
     * executing in_set_parameters() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&in->lock);
    if (in->standby) {
        ret = start_input_stream(in);
        if (ret == 0)
            in->standby = 0;
    }
    pthread_mutex_unlock(&adev->lock);

    if (ret < 0)
        goto exit;

    if (in->num_preprocessors != 0) {
        ret = process_frames(in, buffer, frames_rq);
    } else {
        ret = read_frames(in, buffer, frames_rq);
    }

    if (ret > 0)
        ret = 0;

    /*
     * Instead of writing zeroes here, we could trust the hardware
     * to always provide zeroes when muted.
     */
    if (ret == 0 && adev->mic_mute)
        memset(buffer, 0, bytes);

exit:
    if (ret < 0)
        usleep(bytes * 1000000 / audio_stream_in_frame_size(stream) /
               in_get_sample_rate(&stream->common));

    pthread_mutex_unlock(&in->lock);
    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream __unused)
{
    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream,
                               effect_handle_t effect)
{
    struct stream_in *in = (struct stream_in *)stream;
    int status = 0;

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);
    if (in->num_preprocessors >= MAX_PREPROCESSORS) {
        status = -ENOSYS;
        goto exit;
    }

    in->preprocessors[in->num_preprocessors++] = effect;

exit:

    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);
    return status;
}

static int in_remove_audio_effect(const struct audio_stream *stream,
                                  effect_handle_t effect)
{
    struct stream_in *in = (struct stream_in *)stream;
    int status = -EINVAL;
    int i;

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);
    if (in->num_preprocessors <= 0) {
        status = -ENOSYS;
        goto exit;
    }

    for (i = 0; i < in->num_preprocessors; i++) {
        if (status == 0) {
            in->preprocessors[i - 1] = in->preprocessors[i];
            continue;
        }
        if (in->preprocessors[i] == effect) {
            in->preprocessors[i] = NULL;
            status = 0;
        }
    }

    if (status != 0)
        goto exit;

    in->num_preprocessors--;

exit:

    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);
    return status;
}

static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle __unused,
                                   audio_devices_t devices __unused,
                                   audio_output_flags_t flags __unused,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out,
                                   const char *address __unused)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_out *out;
    int ret;

    if (!select_card(0, PCM_OUT, adev->out_device))
        return -ENODEV;

    out = (struct stream_out *)calloc(1, sizeof(struct stream_out));
    if (!out)
        return -ENOMEM;

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;

    out->dev = adev;

    config->format = out_get_format(&out->stream.common);
    config->channel_mask = out_get_channels(&out->stream.common);
    config->sample_rate = out_get_sample_rate(&out->stream.common);

    out->standby = true;

    int res = pthread_mutex_init(&(out->lock), NULL);
    if(res != 0){
        free(out);
        return -ENOMEM;
    }

    *stream_out = &out->stream;
    return 0;

    free(out);
    *stream_out = NULL;
    return ret;
}

static void adev_close_output_stream(struct audio_hw_device *dev __unused,
                                     struct audio_stream_out *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    out_standby(&stream->common);

    pthread_mutex_destroy(&(out->lock));

    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct str_parms *parms;
    char *str;
    char value[32];
    int ret;

    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, "screen_state", value, sizeof(value));
    if (ret >= 0) {
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0)
            adev->screen_off = false;
        else
            adev->screen_off = true;
    }

    str_parms_destroy(parms);
    return ret;
}

static char * adev_get_parameters(const struct audio_hw_device *dev __unused,
                                  const char *keys __unused)
{
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev __unused)
{
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev __unused, float volume __unused)
{
    return -ENOSYS;
}

static int adev_set_master_volume(struct audio_hw_device *dev __unused, float volume __unused)
{
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev __unused, audio_mode_t mode __unused)
{
    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    struct audio_device *adev = (struct audio_device *)dev;

    adev->mic_mute = state;

    return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    struct audio_device *adev = (struct audio_device *)dev;

    *state = adev->mic_mute;

    return 0;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev __unused,
                                         const struct audio_config *config)
{
    size_t size;

    /*
     * take resampling into account and return the closest majoring
     * multiple of 16 frames, as audioflinger expects audio buffers to
     * be a multiple of 16 frames
     */
    size = (pcm_config_in.period_size * config->sample_rate) / pcm_config_in.rate;
    size = ((size + 15) / 16) * 16;

    return (size * popcount(config->channel_mask) *
                audio_bytes_per_sample(config->format));
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle __unused,
                                  audio_devices_t devices __unused,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in,
                                  audio_input_flags_t flags __unused,
                                  const char *address __unused,
                                  audio_source_t source __unused)

{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_in *in;
    int ret;

    *stream_in = NULL;

    /* Respond with a request for mono if a different format is given. */
    if (config->channel_mask != AUDIO_CHANNEL_IN_MONO) {
        config->channel_mask = AUDIO_CHANNEL_IN_MONO;
        return -EINVAL;
    }

    in = (struct stream_in *)calloc(1, sizeof(struct stream_in));
    if (!in)
        return -ENOMEM;

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;

    in->dev = adev;
    in->standby = true;
    in->requested_rate = config->sample_rate;
    in->pcm_config = pcm_config_in; /* default PCM config */

    int res = pthread_mutex_init(&(in->lock), NULL);
    if(res != 0){
        free(in);
        return -ENOMEM;
    }

    *stream_in = &in->stream;
    return 0;
}

static void adev_close_input_stream(struct audio_hw_device *dev __unused,
                                   struct audio_stream_in *stream)
{
    struct stream_in *in = (struct stream_in *)stream;
    in_standby(&stream->common);

    pthread_mutex_destroy(&(in->lock));

    free(stream);
}

static int adev_dump(const audio_hw_device_t *device __unused, int fd __unused)
{
    return 0;
}

static int adev_close(hw_device_t *device)
{
    struct audio_device *adev = (struct audio_device *)device;

    audio_route_free(adev->ar);

    pthread_mutex_destroy(&(adev->lock));
    pthread_mutex_destroy(&prop_command_lock);

    free(device);
    return 0;
}

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    struct audio_device *adev;
    int ret;

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(struct audio_device));
    if (!adev)
        return -ENOMEM;

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->hw_device.common.module = (struct hw_module_t *) module;
    adev->hw_device.common.close = adev_close;

    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;

    adev->ar = audio_route_init();

    adev->out_device = AUDIO_DEVICE_OUT_SPEAKER;
    adev->in_device = AUDIO_DEVICE_IN_BUILTIN_MIC & ~AUDIO_DEVICE_BIT_IN;

    int res = pthread_mutex_init(&(adev->lock), NULL);
    if(res != 0){
        free(adev);
        return -ENOMEM;
    }
    res = pthread_mutex_init(&prop_command_lock, NULL);
    if(res != 0){
        free(adev);
        return -ENOMEM;
    }

    *device = &adev->hw_device.common;

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Grouper audio HW HAL",
        .author = "The Android Open Source Project",
        .methods = &hal_module_methods,
    },
};
