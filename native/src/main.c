#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif

#define TARGET_SAMPLE_RATE 16000U
#define MAX_RECORD_SECONDS 3600U
#define MAX_SYNTH_WORDS 64U
#define WORD_PAUSE_SAMPLES 320U

typedef struct {
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t block_align;
    uint32_t data_size;
    long data_offset;
} wav_info;

static uint16_t read_u16(const unsigned char *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t read_u32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0]
        | ((uint32_t)bytes[1] << 8)
        | ((uint32_t)bytes[2] << 16)
        | ((uint32_t)bytes[3] << 24);
}

static int read_exact(FILE *file, void *buffer, size_t size)
{
    return fread(buffer, 1, size, file) == size;
}

static int seek_to(FILE *file, long offset)
{
    return offset >= 0 && fseek(file, offset, SEEK_SET) == 0;
}

static int parse_wav(FILE *file, wav_info *info)
{
    unsigned char header[12];
    unsigned char chunk_header[8];
    unsigned char format[16];
    long file_size;
    uint32_t riff_size;
    long format_offset = 0;
    int found_format = 0;
    int found_data = 0;

    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 12
        || !seek_to(file, 0) || !read_exact(file, header, sizeof(header))) {
        return 0;
    }
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        return 0;
    }

    riff_size = read_u32(header + 4);
    if (riff_size < 4 || (uint64_t)riff_size + 8U > (uint64_t)file_size) {
        return 0;
    }

    while (ftell(file) >= 0 && (uint64_t)ftell(file) + sizeof(chunk_header) <= (uint64_t)riff_size + 8U) {
        uint32_t chunk_size;
        long chunk_offset;
        long next_offset;

        if (!read_exact(file, chunk_header, sizeof(chunk_header))) {
            return 0;
        }
        chunk_size = read_u32(chunk_header + 4);
        chunk_offset = ftell(file);
        if (chunk_offset < 0 || (uint64_t)chunk_offset + chunk_size > (uint64_t)riff_size + 8U) {
            return 0;
        }
        next_offset = chunk_offset + (long)chunk_size + (long)(chunk_size & 1U);
        if ((uint64_t)next_offset > (uint64_t)riff_size + 8U) {
            return 0;
        }

        if (memcmp(chunk_header, "fmt ", 4) == 0 && !found_format) {
            if (chunk_size < sizeof(format)) {
                return 0;
            }
            format_offset = chunk_offset;
            found_format = 1;
        } else if (memcmp(chunk_header, "data", 4) == 0 && !found_data) {
            info->data_offset = chunk_offset;
            info->data_size = chunk_size;
            found_data = 1;
        }

        if (!seek_to(file, next_offset)) {
            return 0;
        }
    }

    if (!found_format || !found_data || !seek_to(file, format_offset)
        || !read_exact(file, format, sizeof(format))) {
        return 0;
    }

    info->channels = read_u16(format + 2);
    info->sample_rate = read_u32(format + 4);
    info->block_align = read_u16(format + 12);

    return read_u16(format) == 1
        && (info->channels == 1 || info->channels == 2)
        && read_u16(format + 14) == 16
        && info->sample_rate > 0
        && info->block_align == info->channels * 2U
        && (uint64_t)read_u32(format + 8) == (uint64_t)info->sample_rate * info->block_align
        && info->data_size > 0
        && info->data_size % info->block_align == 0;
}

static int32_t decode_sample(const unsigned char *bytes)
{
    uint16_t sample = read_u16(bytes);
    return sample < 32768U ? sample : (int32_t)sample - 65536;
}

static int write_u16(FILE *file, uint16_t value)
{
    unsigned char bytes[2] = {(unsigned char)value, (unsigned char)(value >> 8)};
    return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

static int write_u32(FILE *file, uint32_t value)
{
    unsigned char bytes[4] = {
        (unsigned char)value,
        (unsigned char)(value >> 8),
        (unsigned char)(value >> 16),
        (unsigned char)(value >> 24)
    };
    return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

static int write_wav_header(FILE *file, uint32_t sample_count)
{
    uint32_t data_size = sample_count * 2U;

    return fwrite("RIFF", 1, 4, file) == 4
        && write_u32(file, data_size + 36U)
        && fwrite("WAVEfmt ", 1, 8, file) == 8
        && write_u32(file, 16)
        && write_u16(file, 1)
        && write_u16(file, 1)
        && write_u32(file, TARGET_SAMPLE_RATE)
        && write_u32(file, TARGET_SAMPLE_RATE * 2U)
        && write_u16(file, 2)
        && write_u16(file, 16)
        && fwrite("data", 1, 4, file) == 4
        && write_u32(file, data_size);
}

static int prepare_audio(const char *input_path, const char *output_path)
{
    FILE *input = NULL;
    FILE *output = NULL;
    unsigned char *source_bytes = NULL;
    int32_t *mono_samples = NULL;
    wav_info info = {0};
    uint64_t frame_count;
    uint64_t output_count;
    uint64_t output_index;
    uint64_t output_bytes;
    int success = 0;
    int output_opened = 0;

    input = fopen(input_path, "rb");
    if (input == NULL) {
        fprintf(stderr, "Could not open input WAV file.\n");
        goto cleanup;
    }
    if (!parse_wav(input, &info)) {
        fprintf(stderr, "Input must be a valid 16-bit PCM WAV with one or two channels.\n");
        goto cleanup;
    }

    frame_count = info.data_size / info.block_align;
    output_count = (frame_count * TARGET_SAMPLE_RATE + info.sample_rate / 2U) / info.sample_rate;
    if (output_count == 0) {
        output_count = 1;
    }
    output_bytes = output_count * 2U;
    if (output_bytes > UINT32_MAX - 36U
        || frame_count > SIZE_MAX / sizeof(*mono_samples)
        || output_count > SIZE_MAX / sizeof(*mono_samples)) {
        fprintf(stderr, "Audio is too long for a standard WAV file.\n");
        goto cleanup;
    }

    source_bytes = (unsigned char *)malloc(info.data_size);
    mono_samples = (int32_t *)malloc((size_t)frame_count * sizeof(*mono_samples));
    if (source_bytes == NULL || mono_samples == NULL) {
        fprintf(stderr, "Not enough memory to prepare this audio file.\n");
        goto cleanup;
    }
    if (!seek_to(input, info.data_offset) || !read_exact(input, source_bytes, info.data_size)) {
        fprintf(stderr, "Could not read audio samples.\n");
        goto cleanup;
    }

    for (uint64_t frame = 0; frame < frame_count; ++frame) {
        const unsigned char *sample = source_bytes + frame * info.block_align;
        int32_t left = decode_sample(sample);
        mono_samples[frame] = info.channels == 1
            ? left
            : (left + decode_sample(sample + 2)) / 2;
    }

    output = fopen(output_path, "wb");
    if (output == NULL) {
        fprintf(stderr, "Could not open output WAV file.\n");
        goto cleanup;
    }
    output_opened = 1;
    if (!write_wav_header(output, (uint32_t)output_count)) {
        fprintf(stderr, "Could not write output WAV header.\n");
        goto cleanup;
    }

    for (output_index = 0; output_index < output_count; ++output_index) {
        uint64_t source_position = output_index * info.sample_rate;
        uint64_t source_index = source_position / TARGET_SAMPLE_RATE;
        uint64_t remainder = source_position % TARGET_SAMPLE_RATE;
        int32_t first;
        int32_t second;
        int64_t numerator;
        int32_t converted;

        if (source_index >= frame_count) {
            source_index = frame_count - 1;
        }
        first = mono_samples[source_index];
        second = source_index + 1 < frame_count ? mono_samples[source_index + 1] : first;
        numerator = (int64_t)first * (TARGET_SAMPLE_RATE - remainder)
            + (int64_t)second * remainder;
        converted = numerator >= 0
            ? (int32_t)((numerator + TARGET_SAMPLE_RATE / 2U) / TARGET_SAMPLE_RATE)
            : (int32_t)((numerator - TARGET_SAMPLE_RATE / 2U) / TARGET_SAMPLE_RATE);
        if (!write_u16(output, (uint16_t)(int16_t)converted)) {
            fprintf(stderr, "Could not write output audio samples.\n");
            goto cleanup;
        }
    }

    if (fflush(output) != 0) {
        fprintf(stderr, "Could not finish writing output WAV file.\n");
        goto cleanup;
    }
    success = 1;

cleanup:
    if (output != NULL && fclose(output) != 0) {
        success = 0;
    }
    if (input != NULL) {
        fclose(input);
    }
    free(mono_samples);
    free(source_bytes);
    if (!success && output_opened) {
        remove(output_path);
    }
    return success;
}

#ifdef _WIN32
static void report_wave_error(const char *operation, MMRESULT result)
{
    char message[MAXERRORLENGTH];

    if (waveInGetErrorTextA(result, message, MAXERRORLENGTH) == MMSYSERR_NOERROR) {
        fprintf(stderr, "%s: %s\n", operation, message);
    } else {
        fprintf(stderr, "%s failed (Windows audio error %u).\n", operation, result);
    }
}

static int record_microphone(uint32_t seconds, const char *output_path)
{
    const DWORD buffer_size = TARGET_SAMPLE_RATE * 2U;
    const uint32_t sample_count = TARGET_SAMPLE_RATE * seconds;
    const uint64_t total_bytes = (uint64_t)sample_count * 2U;
    WAVEFORMATEX format = {0};
    WAVEHDR headers[2] = {{0}};
    HANDLE event = NULL;
    HWAVEIN input = NULL;
    FILE *output = NULL;
    unsigned char *buffers[2] = {NULL, NULL};
    int prepared[2] = {0, 0};
    uint64_t remaining_bytes = total_bytes;
    int success = 0;
    MMRESULT result;

    event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (event == NULL) {
        fprintf(stderr, "Could not create microphone capture event.\n");
        goto cleanup;
    }

    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = TARGET_SAMPLE_RATE;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = TARGET_SAMPLE_RATE * format.nBlockAlign;
    format.cbSize = 0;
    result = waveInOpen(&input, WAVE_MAPPER, &format, (DWORD_PTR)event, 0, CALLBACK_EVENT);
    if (result != MMSYSERR_NOERROR) {
        report_wave_error("Could not open a 16 kHz microphone input", result);
        goto cleanup;
    }

    output = fopen(output_path, "wb");
    if (output == NULL) {
        fprintf(stderr, "Could not open output WAV file.\n");
        goto cleanup;
    }
    if (!write_wav_header(output, sample_count)) {
        fprintf(stderr, "Could not write output WAV header.\n");
        goto cleanup;
    }

    for (unsigned int index = 0; index < 2; ++index) {
        buffers[index] = (unsigned char *)malloc(buffer_size);
        if (buffers[index] == NULL) {
            fprintf(stderr, "Not enough memory to record audio.\n");
            goto cleanup;
        }
        headers[index].lpData = (LPSTR)buffers[index];
        headers[index].dwBufferLength = buffer_size;
        result = waveInPrepareHeader(input, &headers[index], sizeof(headers[index]));
        if (result != MMSYSERR_NOERROR) {
            report_wave_error("Could not prepare microphone capture buffer", result);
            goto cleanup;
        }
        prepared[index] = 1;
        result = waveInAddBuffer(input, &headers[index], sizeof(headers[index]));
        if (result != MMSYSERR_NOERROR) {
            report_wave_error("Could not queue microphone capture buffer", result);
            goto cleanup;
        }
    }

    result = waveInStart(input);
    if (result != MMSYSERR_NOERROR) {
        report_wave_error("Could not start microphone capture", result);
        goto cleanup;
    }

    while (remaining_bytes > 0) {
        DWORD wait_result = WaitForSingleObject(event, INFINITE);
        int completed = 0;

        if (wait_result != WAIT_OBJECT_0) {
            fprintf(stderr, "Waiting for microphone audio failed.\n");
            goto cleanup;
        }

        for (unsigned int index = 0; index < 2 && remaining_bytes > 0; ++index) {
            DWORD bytes_to_write;

            if ((headers[index].dwFlags & WHDR_DONE) == 0) {
                continue;
            }
            completed = 1;
            if (headers[index].dwBytesRecorded > headers[index].dwBufferLength
                || headers[index].dwBytesRecorded % format.nBlockAlign != 0) {
                fprintf(stderr, "Microphone returned an invalid audio buffer.\n");
                goto cleanup;
            }

            bytes_to_write = headers[index].dwBytesRecorded;
            if ((uint64_t)bytes_to_write > remaining_bytes) {
                bytes_to_write = (DWORD)remaining_bytes;
            }
            if (bytes_to_write > 0
                && fwrite(headers[index].lpData, 1, bytes_to_write, output) != bytes_to_write) {
                fprintf(stderr, "Could not write recorded audio.\n");
                goto cleanup;
            }
            remaining_bytes -= bytes_to_write;

            if (remaining_bytes > 0) {
                headers[index].dwBytesRecorded = 0;
                result = waveInAddBuffer(input, &headers[index], sizeof(headers[index]));
                if (result != MMSYSERR_NOERROR) {
                    report_wave_error("Could not requeue microphone capture buffer", result);
                    goto cleanup;
                }
            }
        }
        if (!completed) {
            fprintf(stderr, "Microphone signaled without a completed audio buffer.\n");
            goto cleanup;
        }
    }

    if (fflush(output) != 0) {
        fprintf(stderr, "Could not finish writing recorded audio.\n");
        goto cleanup;
    }
    success = 1;

cleanup:
    if (input != NULL) {
        waveInReset(input);
        for (unsigned int index = 0; index < 2; ++index) {
            if (prepared[index]) {
                waveInUnprepareHeader(input, &headers[index], sizeof(headers[index]));
            }
        }
        waveInClose(input);
    }
    if (output != NULL && fclose(output) != 0) {
        success = 0;
    }
    if (!success && output != NULL) {
        remove(output_path);
    }
    for (unsigned int index = 0; index < 2; ++index) {
        free(buffers[index]);
    }
    if (event != NULL) {
        CloseHandle(event);
    }
    return success;
}
#else
static int record_microphone(uint32_t seconds, const char *output_path)
{
    (void)seconds;
    (void)output_path;
    fprintf(stderr, "Microphone recording is currently supported only on Windows.\n");
    return 0;
}
#endif

static int wav_is_synthesis_format(const wav_info *info)
{
    return info->channels == 1
        && info->sample_rate == TARGET_SAMPLE_RATE
        && info->block_align == 2;
}

static int synthesize_words(const char *output_path, int word_count, char **word_paths)
{
    FILE *output = NULL;
    uint64_t total_samples = word_count > 0 ? (uint64_t)(word_count - 1) * WORD_PAUSE_SAMPLES : 0;
    int success = 0;
    int output_opened = 0;

    if (word_count < 1 || word_count > (int)MAX_SYNTH_WORDS) {
        fprintf(stderr, "Speech requires between 1 and %u recorded words.\n", MAX_SYNTH_WORDS);
        return 0;
    }

    for (int index = 0; index < word_count; ++index) {
        FILE *input = fopen(word_paths[index], "rb");
        wav_info info = {0};
        uint64_t samples;

        if (input == NULL || !parse_wav(input, &info) || !wav_is_synthesis_format(&info)) {
            fprintf(stderr, "Word audio must be valid mono 16-bit 16 kHz PCM WAV: %s\n",
                    word_paths[index]);
            if (input != NULL) {
                fclose(input);
            }
            return 0;
        }
        fclose(input);
        samples = info.data_size / info.block_align;
        if (UINT32_MAX / 2U < total_samples + samples) {
            fprintf(stderr, "Synthesized audio is too long for a standard WAV file.\n");
            return 0;
        }
        total_samples += samples;
    }

    output = fopen(output_path, "wb");
    if (output == NULL) {
        fprintf(stderr, "Could not open synthesized output WAV file.\n");
        goto cleanup;
    }
    output_opened = 1;
    if (!write_wav_header(output, (uint32_t)total_samples)) {
        fprintf(stderr, "Could not write synthesized WAV header.\n");
        goto cleanup;
    }

    for (int index = 0; index < word_count; ++index) {
        FILE *input = fopen(word_paths[index], "rb");
        wav_info info = {0};
        unsigned char buffer[8192];
        uint32_t remaining;

        if (input == NULL || !parse_wav(input, &info) || !wav_is_synthesis_format(&info)
            || !seek_to(input, info.data_offset)) {
            fprintf(stderr, "Could not read word audio: %s\n", word_paths[index]);
            if (input != NULL) {
                fclose(input);
            }
            goto cleanup;
        }

        remaining = info.data_size;
        while (remaining > 0) {
            size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
            if (!read_exact(input, buffer, chunk) || fwrite(buffer, 1, chunk, output) != chunk) {
                fprintf(stderr, "Could not copy word audio: %s\n", word_paths[index]);
                fclose(input);
                goto cleanup;
            }
            remaining -= (uint32_t)chunk;
        }
        fclose(input);

        if (index + 1 < word_count) {
            for (uint32_t pause = 0; pause < WORD_PAUSE_SAMPLES; ++pause) {
                if (!write_u16(output, 0)) {
                    fprintf(stderr, "Could not write speech pause.\n");
                    goto cleanup;
                }
            }
        }
    }

    if (fflush(output) != 0) {
        fprintf(stderr, "Could not finish synthesized WAV file.\n");
        goto cleanup;
    }
    success = 1;

cleanup:
    if (output != NULL && fclose(output) != 0) {
        success = 0;
    }
    if (!success && output_opened) {
        remove(output_path);
    }
    return success;
}

static int play_audio(const char *input_path)
{
    FILE *input = fopen(input_path, "rb");
    wav_info info = {0};

    if (input == NULL) {
        fprintf(stderr, "Could not open input WAV file.\n");
        return 0;
    }
    if (!parse_wav(input, &info)) {
        fprintf(stderr, "Input must be a valid 16-bit PCM WAV with one or two channels.\n");
        fclose(input);
        return 0;
    }
    fclose(input);

#ifdef _WIN32
    if (!PlaySoundA(input_path, NULL, SND_FILENAME | SND_SYNC)) {
        fprintf(stderr, "Windows could not play this WAV file.\n");
        return 0;
    }
    return 1;
#else
    fprintf(stderr, "WAV playback is currently supported only on Windows.\n");
    return 0;
#endif
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "prepare") == 0) {
        return prepare_audio(argv[2], argv[3]) ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "play") == 0) {
        return play_audio(argv[2]) ? 0 : 1;
    }
    if (argc >= 4 && strcmp(argv[1], "synthesize") == 0) {
        return synthesize_words(argv[2], argc - 3, argv + 3) ? 0 : 1;
    }
    if (argc == 4 && strcmp(argv[1], "record") == 0) {
        char *end = NULL;
        unsigned long seconds;

        if (argv[2][0] == '\0' || argv[2][0] == '-') {
            fprintf(stderr, "Recording duration must be between 1 and %u seconds.\n",
                    MAX_RECORD_SECONDS);
            return 2;
        }
        seconds = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || seconds == 0 || seconds > MAX_RECORD_SECONDS) {
            fprintf(stderr, "Recording duration must be between 1 and %u seconds.\n",
                    MAX_RECORD_SECONDS);
            return 2;
        }
        return record_microphone((uint32_t)seconds, argv[3]) ? 0 : 1;
    }
    if (argc == 3) {
        fprintf(stderr, "Usage: symvoice-audio prepare <input.wav> <output.wav>\n");
    } else {
        fprintf(stderr, "Usage: symvoice-audio record <seconds> <output.wav>\n");
        fprintf(stderr, "       symvoice-audio prepare <input.wav> <output.wav>\n");
        fprintf(stderr, "       symvoice-audio play <input.wav>\n");
        fprintf(stderr, "       symvoice-audio synthesize <output.wav> <word.wav> [word.wav ...]\n");
    }
    return 2;
}
