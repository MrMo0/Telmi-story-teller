#ifndef MP3_READER_
#define MP3_READER_

#ifndef MPG123_NO_LARGENAME
#define MPG123_NO_LARGENAME
#endif

#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "SDL2/SDL.h"
#include "mpg123/mpg123.h"
#include "utils/file.h"
#include "utils/str.h"
#include "./logs_helper.h"
#include "./crypt_helpers.h"

static const int mpeg1Layer3Bitrates[15] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
static const int mpeg2Layer3Bitrates[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};

static int mp3_frame_bitrate(const uint8_t *data, size_t length) {
    int bitrateKbps = -1;
    for (size_t offset = 0; offset + 3 < length && bitrateKbps < 0; ++offset) {
        uint8_t b0 = data[offset];
        uint8_t b1 = data[offset + 1];
        uint8_t b2 = data[offset + 2];
        if (b0 != 0xFF || (b1 & 0xF0) != 0xF0) {
            continue; // pas de sync de frame
        }
        int version = (b1 >> 3) & 0x03; // 3 = MPEG1, 0 = MPEG2, 1 = MPEG2.5, 2 = réservé
        int layer = (b1 >> 1) & 0x03; // 1 = Layer III (MP3)
        int bitRateIndex = (b2 >> 4) & 0x0F;
        int sampleRateIndex = (b2 >> 2) & 0x03;
        if (layer != 1 || bitRateIndex == 0 || bitRateIndex == 15 || sampleRateIndex == 3) {
            continue;
        }
        if (version == 3) { // MPEG1
            bitrateKbps = mpeg1Layer3Bitrates[bitRateIndex];
        } else if (version == 1 || version == 0) { // MPEG2 / MPEG2.5
            bitrateKbps = mpeg2Layer3Bitrates[bitRateIndex];
        }
    }
    return bitrateKbps;
}

static double mp3_duration_estimate(const char *path) {
    double duration = -1.0;
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        struct stat fileStat;
        if (fstat(fd, &fileStat) == 0) {
            off_t fileSize = fileStat.st_size;
            uint8_t buffer[4096];
            ssize_t bytesRead = read(fd, buffer, sizeof(buffer));

            if (bytesRead >= 4 && fileSize >= bytesRead) {
                // Tag ID3v2 en tête : taille syncsafe (4 octets de 7 bits)
                off_t audioStart = 0;
                if (bytesRead >= 10 && buffer[0] == 'I' && buffer[1] == 'D' && buffer[2] == '3') {
                    audioStart = 10
                               + ((off_t)(buffer[6] & 0x7F) << 21)
                               + ((off_t)(buffer[7] & 0x7F) << 14)
                               + ((off_t)(buffer[8] & 0x7F) << 7)
                               + (off_t)(buffer[9] & 0x7F);
                    if ((buffer[5] & 0x10) != 0) {
                        audioStart += 10; // footer ID3v2
                    }
                }

                if (audioStart < fileSize) {
                    // Tag ID3v1 en queue
                    off_t audioEnd = fileSize;
                    if (fileSize >= 128) {
                        uint8_t tail[128];
                        if (lseek(fd, -128, SEEK_END) >= 0
                            && read(fd, tail, sizeof(tail)) == 128
                            && tail[0] == 'T' && tail[1] == 'A' && tail[2] == 'G') {
                            audioEnd -= 128;
                        }
                    }

                    // Première frame : on la lit après le tag si elle dépasse le buffer de 4Ko
                    uint8_t frameBuffer[4096];
                    off_t frameRead = 0;
                    if (audioStart < bytesRead) {
                        frameRead = bytesRead - audioStart;
                        memcpy(frameBuffer, buffer + audioStart, frameRead);
                    } else {
                        if (lseek(fd, audioStart, SEEK_SET) >= 0) {
                            ssize_t frameBytes = read(fd, frameBuffer, sizeof(frameBuffer));
                            if (frameBytes > 0) {
                                frameRead = frameBytes;
                            }
                        }
                    }

                    // Détection VBR : on échantillonne une frame au milieu de la zone audio
                    // et on compare son bitrate au premier frame.
                    off_t middlePosition = audioStart + (audioEnd - audioStart) / 2;
                    uint8_t middleBuffer[4096];
                    ssize_t middleRead = 0;
                    if (lseek(fd, middlePosition, SEEK_SET) >= 0) {
                        middleRead = read(fd, middleBuffer, sizeof(middleBuffer));
                    }

                    int firstBitrateKbps = mp3_frame_bitrate(frameBuffer, (size_t) frameRead);
                    int middleBitrateKbps = (middleRead > 3) ? mp3_frame_bitrate(middleBuffer, (size_t) middleRead) : -1;

                    if (firstBitrateKbps > 0 && middleBitrateKbps == firstBitrateKbps
                        && audioEnd - audioStart >= 4) {
                        duration = ((double)(audioEnd - audioStart) * 8.0) / ((double)firstBitrateKbps * 1000.0);
                        if (duration <= 0.0) {
                            duration = -1.0;
                        }
                    }
                }
            }
        }
        close(fd);
    }
    return duration;
}

#define MP3_CACHE_DIR        "/mnt/SDCARD/.cache/mp3scan"
#define MP3_CACHE_MAGIC      0x43333350u // "MP3C"
#define MP3_CACHE_VERSION    1
#define MP3_CACHE_HEADER_SIZE (sizeof(uint32_t) + sizeof(int) + sizeof(off_t) + sizeof(double) + sizeof(off_t) + sizeof(size_t))
#define MP3_CACHE_MAX_FILL   200000

static void mp3_scanCachePath(const char *path, const char *suffix, char *out, size_t outSize) {
    char md5[33];
    md5_string(path, md5);
    snprintf(out, outSize, "%s/%s%s", MP3_CACHE_DIR, md5, suffix);
}

// Renvoie la durée du cache si valide, sinon -1.0.
// Si valide : restaure l'index de seek dans `handle` via mpg123_set_index().
static double mp3_scanCache_get(const char *path, mpg123_handle *handle) {
    struct stat fileStat;
    if (stat(path, &fileStat) != 0 || fileStat.st_size <= 0) {
        return -1.0;
    }
    off_t fileSize = fileStat.st_size;

    char cachePath[128];
    mp3_scanCachePath(path, ".cache", cachePath, sizeof(cachePath));
    if (!exists(cachePath)) {
        return -1.0;
    }

    FILE *fp = fopen(cachePath, "rb");
    if (fp == NULL) {
        writeLog("mp3_reader", "scanCache: fopen failed");
        return -1.0;
    }

    uint32_t magic = 0;
    int version = 0;
    off_t cachedFileSize = 0;
    double duration = -1.0;
    off_t step = 0;
    size_t fill = 0;
    bool headerOk = fread(&magic, sizeof(magic), 1, fp) == 1
        && fread(&version, sizeof(version), 1, fp) == 1
        && fread(&cachedFileSize, sizeof(cachedFileSize), 1, fp) == 1
        && fread(&duration, sizeof(duration), 1, fp) == 1
        && fread(&step, sizeof(step), 1, fp) == 1
        && fread(&fill, sizeof(fill), 1, fp) == 1;
    if (!headerOk
        || magic != MP3_CACHE_MAGIC
        || version != MP3_CACHE_VERSION
        || cachedFileSize != fileSize
        || duration <= 0.0
        || fill == 0
        || fill > MP3_CACHE_MAX_FILL) {
        fclose(fp);
        return -1.0; // cache absent/invalide → re-scan
    }

    off_t *offsets = (off_t *) malloc(fill * sizeof(off_t));
    if (offsets == NULL) {
        writeLog("mp3_reader", "scanCache: malloc failed");
        fclose(fp);
        return -1.0;
    }
    if (fseek(fp, (long) MP3_CACHE_HEADER_SIZE, SEEK_SET) != 0
        || fread(offsets, sizeof(off_t), fill, fp) != fill) {
        free(offsets);
        fclose(fp);
        return -1.0;
    }
    fclose(fp);

    if (mpg123_set_index(handle, offsets, step, fill) != MPG123_OK) {
        free(offsets);
        return -1.0;
    }
    free(offsets);
    return duration;
}

static void mp3_scanCache_set(const char *path, mpg123_handle *handle, double duration) {
    if (duration <= 0.0) {
        return;
    }
    struct stat fileStat;
    if (stat(path, &fileStat) != 0 || fileStat.st_size <= 0) {
        return;
    }
    off_t fileSize = fileStat.st_size;

    // mpg123_index renvoie un pointeur vers l'index interne de mpg123
    // (pas une copie) : on ne doit PAS le free.
    off_t *offsets = NULL;
    off_t step = 0;
    size_t fill = 0;
    if (mpg123_index(handle, &offsets, &step, &fill) != MPG123_OK
        || offsets == NULL
        || fill == 0
        || fill > MP3_CACHE_MAX_FILL) {
        return;
    }

    mkdirs(MP3_CACHE_DIR);
    char cachePath[128];
    char tempPath[128];
    mp3_scanCachePath(path, ".cache", cachePath, sizeof(cachePath));
    mp3_scanCachePath(path, ".cache.tmp", tempPath, sizeof(tempPath));

    FILE *fp = fopen(tempPath, "wb");
    if (fp == NULL) {
        writeLog("mp3_reader", "scanCache: fopen tmp failed");
        return;
    }
    uint32_t magic = MP3_CACHE_MAGIC;
    int version = MP3_CACHE_VERSION;
    bool ok = fwrite(&magic, sizeof(magic), 1, fp) == 1
        && fwrite(&version, sizeof(version), 1, fp) == 1
        && fwrite(&fileSize, sizeof(fileSize), 1, fp) == 1
        && fwrite(&duration, sizeof(duration), 1, fp) == 1
        && fwrite(&step, sizeof(step), 1, fp) == 1
        && fwrite(&fill, sizeof(fill), 1, fp) == 1
        && fwrite(offsets, sizeof(off_t), fill, fp) == fill;
    if (ok) {
        fflush(fp);
        fsync(fileno(fp));
    }
    if (fclose(fp) != 0 || !ok) {
        writeLog("mp3_reader", "scanCache: write failed");
        remove(tempPath);
        return;
    }
    if (rename(tempPath, cachePath) != 0) {
        writeLog("mp3_reader", "scanCache: rename failed");
        remove(tempPath);
    }
}

#define MP3_AUDIO_SAMPLE_RATE 44100
#define MP3_AUDIO_CHANNELS 2
#define MP3_AUDIO_BUFFER_SAMPLES 4096

static mpg123_handle *mpg = NULL;
static int mpgFd = -1;
static pthread_mutex_t audioMutex = PTHREAD_MUTEX_INITIALIZER;
static double musicDuration = 0.0;
static double currentPosition = 0.0;
static bool isPlaying = false;
static bool isPaused = false;
static bool isFinished = true;
static bool seekRequested = false;
static double seekTarget = 0.0;

#define AUDIO_DURATION_CACHE_SIZE 128

typedef struct {
    uint64_t hash;
    double duration;
} audioDurationCacheEntry;

static audioDurationCacheEntry audioDurationCache[AUDIO_DURATION_CACHE_SIZE];

static off_t mp3_frameForTime(mpg123_handle *handle, double time) {
    off_t frame = mpg123_timeframe(handle, time);
    if (frame < 0) {
        int rate = MP3_AUDIO_SAMPLE_RATE;
        struct mpg123_frameinfo frameInfo;
        if (mpg123_info(handle, &frameInfo) == MPG123_OK && frameInfo.rate > 0) {
            rate = (int) frameInfo.rate;
        }
        int samplesPerFrame = mpg123_spf(handle);
        if (samplesPerFrame <= 0) {
            samplesPerFrame = 1152;
        }
        frame = (off_t) ((time * (double) rate) / (double) samplesPerFrame);
    }
    return frame;
}

static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void) userdata;
    pthread_mutex_lock(&audioMutex);
    if (mpg != NULL && seekRequested) {
        seekRequested = false;
        off_t frame = mp3_frameForTime(mpg, seekTarget);
        if (mpg123_seek_frame(mpg, frame, SEEK_SET) >= 0) {
            currentPosition = seekTarget;
        }
    }
    if (mpg == NULL) {
        memset(stream, 0, len);
        pthread_mutex_unlock(&audioMutex);
        return;
    }
    if (isPaused || isFinished) {
        memset(stream, 0, len);
        pthread_mutex_unlock(&audioMutex);
        return;
    }
    size_t done = 0;
    int result = mpg123_decode(mpg, NULL, 0, stream, (size_t) len, &done);
    if (result == MPG123_OK || result == MPG123_NEW_FORMAT) {
        if (done > 0) {
            if (done < (size_t) len) {
                memset(stream + done, 0, len - (int) done);
            }
            currentPosition += (double) done / (2.0 * (double) MP3_AUDIO_CHANNELS * (double) MP3_AUDIO_SAMPLE_RATE);
        } else {
            memset(stream, 0, len);
        }
    } else {
        isFinished = true;
        isPlaying = false;
        memset(stream, 0, len);
    }
    pthread_mutex_unlock(&audioMutex);
}

static void audio_closeLocked(void) {
    if (mpg != NULL) {
        mpg123_close(mpg);
        mpg123_delete(mpg);
        mpg = NULL;
    }
    if (mpgFd >= 0) {
        close(mpgFd);
        mpgFd = -1;
    }
    isPlaying = false;
    isPaused = false;
    isFinished = true;
    seekRequested = false;
    seekTarget = 0.0;
    currentPosition = 0.0;
}

double audio_duration_cache_get(const char *path) {
    uint64_t hash = string_hash(path);
    double duration = -1.0;
    int i = 0;
    while (i < AUDIO_DURATION_CACHE_SIZE && audioDurationCache[i].hash != hash) {
        ++i;
    }
    if (i < AUDIO_DURATION_CACHE_SIZE) {
        audioDurationCacheEntry entry = audioDurationCache[i];
        memmove(&audioDurationCache[1], &audioDurationCache[0], i * sizeof(audioDurationCache[0]));
        audioDurationCache[0] = entry;
        duration = entry.duration;
    }
    return duration;
}

void audio_duration_cache_set(const char *path, double duration) {
    if (duration <= 0.0) {
        return;
    }
    uint64_t hash = string_hash(path);
    memmove(&audioDurationCache[1], &audioDurationCache[0], (AUDIO_DURATION_CACHE_SIZE - 1) * sizeof(audioDurationCache[0]));
    audioDurationCache[0].hash = hash;
    audioDurationCache[0].duration = duration;
}

bool audio_isFinished(void) {
    pthread_mutex_lock(&audioMutex);
    bool finished = isFinished;
    pthread_mutex_unlock(&audioMutex);
    return finished;
}

bool audio_isPlaying(void) {
    pthread_mutex_lock(&audioMutex);
    bool playing = isPlaying && !isFinished;
    pthread_mutex_unlock(&audioMutex);
    return playing;
}

bool audio_isPaused(void) {
    pthread_mutex_lock(&audioMutex);
    bool paused = isPaused;
    pthread_mutex_unlock(&audioMutex);
    return paused;
}

void audio_pause(void) {
    pthread_mutex_lock(&audioMutex);
    if (isPlaying && !isFinished) {
        isPaused = true;
    }
    pthread_mutex_unlock(&audioMutex);
}

void audio_resume(void) {
    pthread_mutex_lock(&audioMutex);
    isPaused = false;
    pthread_mutex_unlock(&audioMutex);
}

void audio_free_music(void) {
    pthread_mutex_lock(&audioMutex);
    audio_closeLocked();
    musicDuration = 0.0;
    pthread_mutex_unlock(&audioMutex);
}

double audio_getDuration(void) {
    return musicDuration;
}

double audio_getPosition(void) {
    pthread_mutex_lock(&audioMutex);
    double position = currentPosition;
    pthread_mutex_unlock(&audioMutex);
    return position;
}

void audio_setPosition(double position) {
    pthread_mutex_lock(&audioMutex);
    if (mpg != NULL && !isFinished) {
        if (position < 0.0) {
            position = 0.0;
        }
        if (musicDuration > 0.0 && position > musicDuration) {
            position = musicDuration;
        }
        seekTarget = position;
        seekRequested = true;
    }
    pthread_mutex_unlock(&audioMutex);
}

void audio_play_path(char *soundPath, double position, bool askDuration) {
    pthread_mutex_lock(&audioMutex);
    audio_closeLocked();
    musicDuration = 0.0;
    if (position < 0.0) {
        position = 0.0;
    }
    int fd = open(soundPath, O_RDONLY);
    if (fd >= 0) {
        int error = 0;
        mpg123_handle *handle = mpg123_new(NULL, &error);
        if (handle != NULL) {
            if (mpg123_open_fd(handle, fd) == MPG123_OK) {
                mpg123_format_none(handle);
                mpg123_format(handle, MP3_AUDIO_SAMPLE_RATE, MP3_AUDIO_CHANNELS, MPG123_ENC_SIGNED_16);
                musicDuration = -1.0;
                if (askDuration) {
                    double cachedDuration = audio_duration_cache_get(soundPath);
                    if (cachedDuration >= 0.0) {
                        musicDuration = cachedDuration;
                    } else {
                        musicDuration = mp3_duration_estimate(soundPath);
                        if (musicDuration < 0.0) { // → VBR
                            double scanCacheDuration = mp3_scanCache_get(soundPath, handle);
                            if (scanCacheDuration >= 0.0) {
                                musicDuration = scanCacheDuration; // cache fichier hit → pas de scan
                            } else if (mpg123_scan(handle) == MPG123_OK) {
                                off_t length = mpg123_length(handle);
                                if (length > 0) {
                                    double sourceRate = (double) MP3_AUDIO_SAMPLE_RATE;
                                    struct mpg123_frameinfo frameInfo;
                                    if (mpg123_info(handle, &frameInfo) == MPG123_OK && frameInfo.rate > 0) {
                                        sourceRate = (double) frameInfo.rate;
                                    }
                                    musicDuration = (double) length / sourceRate;
                                }
                                mp3_scanCache_set(soundPath, handle, musicDuration);
                            }
                        }
                        audio_duration_cache_set(soundPath, musicDuration);
                    }
                }
                if (musicDuration > 0.0 && position > musicDuration) {
                    position = musicDuration;
                }
                if (position > 0.0) {
                    unsigned char frameBuffer[8192];
                    size_t frameDone = 0;
                    mpg123_decode(handle, NULL, 0, frameBuffer, sizeof(frameBuffer), &frameDone);
                    off_t frame = mp3_frameForTime(handle, position);
                    if (mpg123_seek_frame(handle, frame, SEEK_SET) >= 0) {
                        currentPosition = position;
                    }
                }
                mpg = handle;
                mpgFd = fd;
                isPlaying = true;
                isPaused = false;
                isFinished = false;
                seekRequested = false;
            } else {
                writeLog("mp3_reader", "mpg123_open_fd failed");
                mpg123_delete(handle);
                close(fd);
            }
        } else {
            char errMsg[64];
            sprintf(errMsg, "mpg123_new failed (error=%d)", error);
            writeLog("mp3_reader", errMsg);
            close(fd);
        }
    } else {
        writeLog("mp3_reader", "open failed");
    }
    pthread_mutex_unlock(&audioMutex);
}

void audio_play(const char *dir, const char *name, double position, bool askDuration) {
    char soundPath[STR_MAX * 2];
    snprintf(soundPath, sizeof(soundPath), "%s%s", dir, name);
    audio_play_path(soundPath, position, askDuration);
}

void mp3_reader_init(void) {
    if (mpg123_init() != MPG123_OK) {
        writeLog("mp3_reader", "mpg123_init failed");
    }
    SDL_AudioSpec want = {0};
    want.freq = MP3_AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = MP3_AUDIO_CHANNELS;
    want.samples = MP3_AUDIO_BUFFER_SAMPLES;
    want.callback = audio_callback;
    want.userdata = NULL;
    if (SDL_OpenAudio(&want, NULL) < 0) {
        writeLog("mp3_reader", "SDL_OpenAudio failed");
    } else {
        SDL_PauseAudio(0);
    }
}

void mp3_reader_quit(void) {
    pthread_mutex_lock(&audioMutex);
    audio_closeLocked();
    pthread_mutex_unlock(&audioMutex);
    SDL_CloseAudio();
    mpg123_exit();
}

#endif // MP3_READER_
