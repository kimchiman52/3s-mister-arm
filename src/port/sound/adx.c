#include "port/sound/adx.h"

#include "common.h"
#include "port/io/afs.h"
#include "sf33rd/Source/Game/io/gd3rd.h"

#include <SDL3/SDL.h>

#if defined(ENABLE_FFMPEG_ADX)
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/intreadwrite.h>
#include <libswresample/swresample.h>
#endif

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_SAMPLE_RATE 48000
#define N_CHANNELS 2
#define BYTES_PER_SAMPLE 2
#define MIN_QUEUED_DATA_MS 400
#define TRACKS_MAX 10
#define ADX_SAMPLES_PER_FRAME 32

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#if defined(ENABLE_FFMPEG_ADX)
typedef struct ADXDecoderPipeline {
    AVCodecContext* context;
    AVCodecParserContext* parser_context;
    SwrContext* swr;
    AVPacket* packet;
    AVFrame* frame;
} ADXDecoderPipeline;
#else
typedef struct ADXBuiltinDecoder {
    int encoding_type;
    int frame_size;
    int channels;
    int sample_rate;
    int total_samples;
    int data_offset;
    int data_pos;
    int decoded_samples;
    int coeff1;
    int coeff2;
    int16_t hist1[2];
    int16_t hist2[2];
    bool finished;
} ADXBuiltinDecoder;
#endif

typedef struct ADXLoopInfo {
    bool looping_enabled;
    int start_sample;
    int end_sample;
    uint8_t* data;
    int data_size;
    int position;
} ADXLoopInfo;

typedef struct ADXTrack {
    int size;
    uint8_t* data;
    bool should_free_data_after_use;
    int used_bytes;
    int processed_samples;
    ADXLoopInfo loop_info;
#if defined(ENABLE_FFMPEG_ADX)
    ADXDecoderPipeline pipeline;
#else
    ADXBuiltinDecoder decoder;
#endif
} ADXTrack;

static SDL_AudioStream* stream = NULL;
static ADXTrack tracks[TRACKS_MAX] = { 0 };
static int num_tracks = 0;
static int first_track_index = 0;
static bool has_tracks = false;
static int output_sample_rate = DEFAULT_SAMPLE_RATE;

static uint16_t read_be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int min_queued_data_bytes() {
    return (int)((float)output_sample_rate * MIN_QUEUED_DATA_MS / 1000 * N_CHANNELS * BYTES_PER_SAMPLE);
}

static int stream_data_needed() {
    if (stream == NULL) {
        return 0;
    }

    return min_queued_data_bytes() - SDL_GetAudioStreamQueued(stream);
}

static bool stream_needs_data() {
    return stream_data_needed() > 0;
}

static bool stream_is_empty() {
    if (stream == NULL) {
        return true;
    }

    return SDL_GetAudioStreamQueued(stream) <= 0;
}

static void create_audio_stream(int sample_rate) {
    if (stream != NULL) {
        SDL_DestroyAudioStream(stream);
        stream = NULL;
    }

    output_sample_rate = sample_rate;
    const SDL_AudioSpec spec = { .format = SDL_AUDIO_S16, .channels = N_CHANNELS, .freq = output_sample_rate };
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (stream == NULL) {
        SDL_Log("Couldn't create ADX audio stream: %s", SDL_GetError());
        return;
    }

    SDL_ResumeAudioStreamDevice(stream);
}

#if defined(ENABLE_FFMPEG_ADX)
static void pipeline_init(ADXDecoderPipeline* pipeline) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_ADPCM_ADX);
    pipeline->context = avcodec_alloc_context3(codec);
    avcodec_open2(pipeline->context, codec, NULL);
    pipeline->parser_context = av_parser_init(codec->id);

    const AVChannelLayout ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    swr_alloc_set_opts2(&pipeline->swr,
                        &ch_layout,
                        AV_SAMPLE_FMT_S16,
                        output_sample_rate,
                        &ch_layout,
                        AV_SAMPLE_FMT_S16P,
                        output_sample_rate,
                        0,
                        NULL);
    swr_init(pipeline->swr);

    pipeline->packet = av_packet_alloc();
    pipeline->frame = av_frame_alloc();
}

static void pipeline_destroy(ADXDecoderPipeline* pipeline) {
    av_packet_free(&pipeline->packet);
    av_frame_free(&pipeline->frame);
    swr_free(&pipeline->swr);
    avcodec_free_context(&pipeline->context);
    av_parser_close(pipeline->parser_context);
}

static void print_av_error(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(errnum, errbuf, sizeof(errbuf));
    fprintf(stderr, "FFmpeg error: %s\n", errbuf);
}
#else
static int clip_int16(int v) {
    if (v < -32768) {
        return -32768;
    }

    if (v > 32767) {
        return 32767;
    }

    return v;
}

static bool decoder_calculate_coefficients(ADXBuiltinDecoder* decoder, int cutoff_frequency) {
    if (cutoff_frequency <= 0 || decoder->sample_rate <= 0) {
        return false;
    }

    const double sqrt2 = sqrt(2.0);
    const double angle = (2.0 * 3.14159265358979323846 * (double)cutoff_frequency) / (double)decoder->sample_rate;
    const double a = sqrt2 - cos(angle);
    const double b = sqrt2 - 1.0;
    const double c = (a - sqrt((a + b) * (a - b))) / b;

    decoder->coeff1 = (int)lround(c * 2.0 * (double)(1 << 12));
    decoder->coeff2 = (int)lround(-(c * c) * (double)(1 << 12));
    return true;
}

static bool decoder_init(ADXBuiltinDecoder* decoder, const uint8_t* data, int size) {
    if (size < 0x40) {
        return false;
    }

    const int header_size = (int)read_be16(data + 2) + 4;
    const int sample_size = data[6];
    const int cutoff_frequency = (int)read_be16(data + 16);

    decoder->encoding_type = data[4];
    decoder->frame_size = data[5];
    decoder->channels = data[7];
    decoder->sample_rate = (int)read_be32(data + 8);
    decoder->total_samples = (int)read_be32(data + 12);
    decoder->data_offset = header_size;
    decoder->data_pos = header_size;
    decoder->decoded_samples = 0;
    decoder->coeff1 = 0;
    decoder->coeff2 = 0;
    decoder->finished = false;
    SDL_zeroa(decoder->hist1);
    SDL_zeroa(decoder->hist2);

    if (decoder->frame_size <= 2) {
        return false;
    }

    if (sample_size != 4) {
        return false;
    }

    if (decoder->channels < 1 || decoder->channels > 2) {
        return false;
    }

    if (decoder->sample_rate <= 0) {
        return false;
    }

    if (decoder->encoding_type != 3 && decoder->encoding_type != 4) {
        return false;
    }

    if (decoder->data_offset >= size) {
        return false;
    }

    return decoder_calculate_coefficients(decoder, cutoff_frequency);
}

static int decode_channel_frame(ADXBuiltinDecoder* decoder, int channel, const uint8_t* frame_data, int16_t* out) {
    static const int predictor_coeffs[4][2] = {
        { 0, 0 },
        { 0x0F00, 0 },
        { 0x1CC0, -0x0D00 },
        { 0x1880, -0x0C00 },
    };

    int coeff1 = decoder->coeff1;
    int coeff2 = decoder->coeff2;
    const int frame_header = (int)read_be16(frame_data);
    int scale = frame_header;

    if ((frame_header & 0x8000) != 0) {
        decoder->finished = true;
        return 0;
    }

    if (decoder->encoding_type == 4) {
        const int predictor = frame_data[0] >> 5;
        scale = ((frame_data[0] & 0x1F) << 8) | frame_data[1];

        if (predictor >= 0 && predictor < 4) {
            coeff1 = predictor_coeffs[predictor][0];
            coeff2 = predictor_coeffs[predictor][1];
        }
    }

    for (int i = 0; i < ADX_SAMPLES_PER_FRAME; i++) {
        const uint8_t b = frame_data[2 + (i / 2)];
        int nibble = (i & 1) == 0 ? ((b >> 4) & 0x0F) : (b & 0x0F);

        if (nibble >= 8) {
            nibble -= 16;
        }

        int sample = (nibble * scale) + (((coeff1 * decoder->hist1[channel]) + (coeff2 * decoder->hist2[channel])) >> 12);
        sample = clip_int16(sample);

        decoder->hist2[channel] = decoder->hist1[channel];
        decoder->hist1[channel] = (int16_t)sample;
        out[i] = (int16_t)sample;
    }

    return ADX_SAMPLES_PER_FRAME;
}

static int decode_samples(ADXBuiltinDecoder* decoder, const uint8_t* data, int data_size, int16_t* out, int max_samples) {
    if (decoder->finished || max_samples <= 0) {
        return 0;
    }

    int samples_written = 0;

    while (samples_written < max_samples) {
        const int frame_block_size = decoder->frame_size * decoder->channels;

        if (decoder->data_pos + frame_block_size > data_size) {
            decoder->finished = true;
            break;
        }

        int16_t channel_samples[2][ADX_SAMPLES_PER_FRAME] = { 0 };
        bool reached_eof = false;

        for (int channel = 0; channel < decoder->channels; channel++) {
            const uint8_t* frame_data = data + decoder->data_pos + (channel * decoder->frame_size);
            if (decode_channel_frame(decoder, channel, frame_data, channel_samples[channel]) <= 0) {
                reached_eof = true;
                break;
            }
        }

        if (reached_eof) {
            break;
        }

        int samples_to_emit = ADX_SAMPLES_PER_FRAME;

        if (decoder->total_samples > 0) {
            const int remaining_samples = decoder->total_samples - decoder->decoded_samples;
            samples_to_emit = MIN(samples_to_emit, remaining_samples);
        }

        samples_to_emit = MIN(samples_to_emit, max_samples - samples_written);

        for (int i = 0; i < samples_to_emit; i++) {
            const int dst = (samples_written + i) * N_CHANNELS;
            out[dst] = channel_samples[0][i];
            out[dst + 1] = decoder->channels == 2 ? channel_samples[1][i] : channel_samples[0][i];
        }

        samples_written += samples_to_emit;
        decoder->decoded_samples += samples_to_emit;
        decoder->data_pos += frame_block_size;

        if (decoder->total_samples > 0 && decoder->decoded_samples >= decoder->total_samples) {
            decoder->finished = true;
            break;
        }
    }

    return samples_written;
}
#endif

static void* load_file(int file_id, int* size) {
    const unsigned int file_size = fsGetFileSize(file_id);
    *size = (int)file_size;
    const size_t buff_size = (file_size + 2048 - 1) & ~(2048 - 1);
    void* buff = malloc(buff_size);

    AFSHandle handle = AFS_Open(file_id);
    AFS_ReadSync(handle, fsCalSectorSize(file_size), buff);
    AFS_Close(handle);

    return buff;
}

static bool track_reached_eof(ADXTrack* track) {
#if defined(ENABLE_FFMPEG_ADX)
    return (track->size - track->used_bytes) <= 0;
#else
    return track->decoder.finished;
#endif
}

static bool track_loop_filled(ADXTrack* track) {
    if (track->loop_info.looping_enabled) {
        return track->processed_samples >= track->loop_info.end_sample;
    }

    return false;
}

static bool track_needs_decoding(ADXTrack* track) {
    if (track->loop_info.looping_enabled) {
        return !track_loop_filled(track);
    }

    return !track_reached_eof(track);
}

static bool track_exhausted(ADXTrack* track) {
    if (track->loop_info.looping_enabled) {
        return false;
    }

    return track_reached_eof(track);
}

static int track_add_samples_to_loop(ADXTrack* track, uint8_t* buf, int num_samples) {
    ADXLoopInfo* loop_info = &track->loop_info;

    if (!loop_info->looping_enabled) {
        return 0;
    }

    const int buf_sample_start = MAX(loop_info->start_sample - track->processed_samples, 0);
    const int buf_sample_end = MIN(loop_info->end_sample - track->processed_samples, num_samples);

    if (buf_sample_end > buf_sample_start) {
        const int buf_start = buf_sample_start * N_CHANNELS * BYTES_PER_SAMPLE;
        const int buf_end = buf_sample_end * N_CHANNELS * BYTES_PER_SAMPLE;
        const int buf_len = buf_end - buf_start;
        memcpy(loop_info->data + loop_info->position, buf + buf_start, buf_len);
        loop_info->position += buf_len;

        if (loop_info->position == loop_info->data_size) {
            loop_info->position = 0;
        }
    }

    const int overflow = MAX(track->processed_samples + num_samples - loop_info->end_sample, 0);
    track->processed_samples += num_samples;
    return overflow;
}

static void loop_info_init(ADXLoopInfo* info, const uint8_t* data) {
    const uint8_t version = data[0x12];
    const int header_size = (int)read_be16(data + 2) + 4;

    switch (version) {
    case 3: {
        if (header_size < 0x28) {
            break;
        }
        const uint16_t loop_enabled_16 = read_be16(data + 0x16);

        if (loop_enabled_16 == 1) {
            info->looping_enabled = true;
            info->start_sample = (int)read_be32(data + 0x1C);
            info->end_sample = (int)read_be32(data + 0x24);
        }

        break;
    }

    case 4: {
        if (header_size < 0x34) {
            break;
        }
        const uint32_t loop_enabled_32 = read_be32(data + 0x24);

        if (loop_enabled_32 == 1) {
            info->looping_enabled = true;
            info->start_sample = (int)read_be32(data + 0x28);
            info->end_sample = (int)read_be32(data + 0x30);
        }

        break;
    }

    default:
        fatal_error("Unhandled ADX version: %d", version);
        break;
    }

    if (info->looping_enabled) {
        info->data_size = (info->end_sample - info->start_sample) * BYTES_PER_SAMPLE * N_CHANNELS;
        info->data = malloc(info->data_size);
        info->position = 0;
    }
}

static void loop_info_destroy(ADXLoopInfo* info) {
    if (info->looping_enabled) {
        free(info->data);
    }

    SDL_zerop(info);
}

static void process_track(ADXTrack* track) {
#if defined(ENABLE_FFMPEG_ADX)
    ADXDecoderPipeline* pipeline = &track->pipeline;
#endif

    while (stream_needs_data() && track_needs_decoding(track)) {
#if defined(ENABLE_FFMPEG_ADX)
        int ret = av_parser_parse2(pipeline->parser_context,
                                   pipeline->context,
                                   &pipeline->packet->data,
                                   &pipeline->packet->size,
                                   track->data + track->used_bytes,
                                   track->size - track->used_bytes,
                                   AV_NOPTS_VALUE,
                                   AV_NOPTS_VALUE,
                                   0);

        if (ret < 0) {
            print_av_error(ret);
            break;
        }

        track->used_bytes += ret;

        if (pipeline->packet->size > 0) {
            ret = avcodec_send_packet(pipeline->context, pipeline->packet);

            if (ret < 0) {
                print_av_error(ret);
                break;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(pipeline->context, pipeline->frame);

                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    print_av_error(ret);
                    break;
                }

                const int out_channels = pipeline->frame->ch_layout.nb_channels;
                const int out_samples = pipeline->frame->nb_samples;
                uint8_t* out_buf = NULL;
                int out_linesize = 0;

                av_samples_alloc(&out_buf, &out_linesize, out_channels, out_samples, AV_SAMPLE_FMT_S16, 0);

                const int samples_converted = swr_convert(
                    pipeline->swr, &out_buf, out_samples, (const uint8_t**)pipeline->frame->data, out_samples);

                const int overflow = track_add_samples_to_loop(track, out_buf, samples_converted);
                const int samples_to_queue = samples_converted - overflow;

                if (samples_to_queue > 0) {
                    const int out_size = av_samples_get_buffer_size(
                        &out_linesize, out_channels, samples_to_queue, AV_SAMPLE_FMT_S16, 1);
                    SDL_PutAudioStreamData(stream, out_buf, out_size);
                }

                av_freep(&out_buf);
            }
        }
#else
        const int needed_samples = stream_data_needed() / (N_CHANNELS * BYTES_PER_SAMPLE);
        const int decode_samples_target = MAX(1, MIN(needed_samples, ADX_SAMPLES_PER_FRAME * 8));
        int16_t out_pcm[ADX_SAMPLES_PER_FRAME * 8 * N_CHANNELS] = { 0 };

        const int samples_decoded = decode_samples(&track->decoder, track->data, track->size, out_pcm, decode_samples_target);

        track->used_bytes = track->decoder.data_pos;

        if (samples_decoded <= 0) {
            track->decoder.finished = true;
            break;
        }

        const int overflow = track_add_samples_to_loop(track, (uint8_t*)out_pcm, samples_decoded);
        const int samples_to_queue = samples_decoded - overflow;

        if (samples_to_queue > 0) {
            SDL_PutAudioStreamData(stream, out_pcm, samples_to_queue * N_CHANNELS * BYTES_PER_SAMPLE);
        }
#endif
    }

    while (track_loop_filled(track) && stream_needs_data()) {
        const int available_data = track->loop_info.data_size - track->loop_info.position;
        const int data_to_queue = MIN(stream_data_needed(), available_data);
        SDL_PutAudioStreamData(stream, track->loop_info.data + track->loop_info.position, data_to_queue);
        track->loop_info.position += data_to_queue;

        if (track->loop_info.position == track->loop_info.data_size) {
            track->loop_info.position = 0;
        }
    }
}

static void track_init(ADXTrack* track, int file_id, void* buf, size_t buf_size, bool looping_allowed) {
    if (file_id == -1 && buf == NULL) {
        fatal_error("One of file_id or buf must be valid.");
    }

    if (file_id != -1) {
        track->data = load_file(file_id, &track->size);
        track->should_free_data_after_use = true;
    } else {
        track->data = buf;
        track->size = (int)buf_size;
        track->should_free_data_after_use = false;
    }

    track->used_bytes = 0;

#if defined(ENABLE_FFMPEG_ADX)
    pipeline_init(&track->pipeline);
#else
    if (!decoder_init(&track->decoder, track->data, track->size)) {
        fatal_error("Failed to initialize in-tree ADX decoder.");
    }

    if (num_tracks == 1 && track->decoder.sample_rate != output_sample_rate) {
        create_audio_stream(track->decoder.sample_rate);
    }
#endif

    if (looping_allowed) {
        loop_info_init(&track->loop_info, track->data);
    }

    process_track(track);
}

static void track_destroy(ADXTrack* track) {
#if defined(ENABLE_FFMPEG_ADX)
    pipeline_destroy(&track->pipeline);
#endif
    loop_info_destroy(&track->loop_info);

    if (track->should_free_data_after_use) {
        free(track->data);
    }

    SDL_zerop(track);
}

static ADXTrack* alloc_track() {
    if (num_tracks == TRACKS_MAX) {
        ADXTrack* old_track = &tracks[first_track_index];
        track_destroy(old_track);
        first_track_index = (first_track_index + 1) % TRACKS_MAX;
        num_tracks -= 1;
    }

    const int index = (first_track_index + num_tracks) % TRACKS_MAX;
    num_tracks += 1;
    has_tracks = true;
    return &tracks[index];
}

void ADX_ProcessTracks() {
    if (num_tracks == 0) {
        return;
    }

    if (!stream_needs_data() && !track_exhausted(&tracks[first_track_index])) {
        return;
    }

    const int first_track_index_old = first_track_index;
    const int num_tracks_old = num_tracks;

    for (int i = 0; i < num_tracks_old; i++) {
        const int j = (first_track_index_old + i) % TRACKS_MAX;
        ADXTrack* track = &tracks[j];
        process_track(track);

        if (!track_exhausted(track)) {
            break;
        }

        track_destroy(track);
        num_tracks -= 1;

        if (num_tracks > 0) {
            first_track_index += 1;
        } else {
            first_track_index = 0;
        }
    }
}

void ADX_Init() {
    create_audio_stream(DEFAULT_SAMPLE_RATE);
}

void ADX_Exit() {
    ADX_Stop();

    if (stream != NULL) {
        SDL_DestroyAudioStream(stream);
        stream = NULL;
    }
}

void ADX_Stop() {
    if (stream != NULL) {
        ADX_Pause(true);
        SDL_ClearAudioStream(stream);
    }

    for (int i = 0; i < num_tracks; i++) {
        const int j = (first_track_index + i) % TRACKS_MAX;
        track_destroy(&tracks[j]);
    }

    num_tracks = 0;
    first_track_index = 0;
    has_tracks = false;
}

int ADX_IsPaused() {
    if (stream == NULL) {
        return 1;
    }

    return SDL_AudioStreamDevicePaused(stream);
}

void ADX_Pause(int pause) {
    if (stream == NULL) {
        return;
    }

    if (pause) {
        SDL_PauseAudioStreamDevice(stream);
    } else {
        SDL_ResumeAudioStreamDevice(stream);
    }
}

void ADX_StartMem(void* buf, size_t size) {
    ADX_Stop();

    ADXTrack* track = alloc_track();
    track_init(track, -1, buf, size, true);
    ADX_Pause(false);
}

int ADX_GetNumFiles() {
    return num_tracks;
}

void ADX_EntryAfs(int file_id) {
    ADXTrack* track = alloc_track();
    track_init(track, file_id, NULL, 0, false);
}

void ADX_StartSeamless() {
    ADX_Pause(false);
}

void ADX_ResetEntry() {
}

void ADX_StartAfs(int file_id) {
    ADX_Stop();

    ADXTrack* track = alloc_track();
    track_init(track, file_id, NULL, 0, true);
    ADX_Pause(false);
}

void ADX_SetOutVol(int volume) {
    const float gain = powf(10.0f, volume / 200.0f);

    if (stream != NULL) {
        SDL_SetAudioStreamGain(stream, gain);
    }
}

void ADX_SetMono(bool mono) {
    (void)mono;
}

ADXState ADX_GetState() {
    if (!has_tracks) {
        return ADX_STATE_STOP;
    }

    if (stream_is_empty()) {
        return ADX_STATE_PLAYEND;
    }

    if (ADX_IsPaused()) {
        return ADX_STATE_STOP;
    }

    return ADX_STATE_PLAYING;
}
