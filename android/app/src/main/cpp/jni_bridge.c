/* 3DODroid JNI bridge: runs the emulator on its own thread at 60 Hz and
 * copies each 320x240 frame to the game's Surface. */
#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <aaudio/AAudio.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sys3do.h"
#include "xbus.h"
#include "disc.h"
#include "dsp.h"
#include "dbglog.h"

#define TAG "3DODroid"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static pthread_mutex_t win_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *window;
static pthread_t thread;
static volatile int running, paused, reset_req;
static volatile uint32_t buttons;
static volatile float fps;
static Disc *disc;
static char last_error[256];
static char nvram_path[1024];

/* ------------------------------------------------------------ audio -- */
#define AUDIO_TARGET 3072        /* frames buffered between emulation and output (~70 ms) */
static AAudioStream *stream;
static volatile int sound_on = 1;

static aaudio_data_callback_result_t audio_cb(AAudioStream *s, void *user, void *data, int32_t frames) {
    (void)s; (void)user;
    int16_t *out = (int16_t *)data;
    int got = (sound_on && running && !paused) ? dsp_audio_take(out, frames) : 0;
    if (got < frames) memset(out + got * 2, 0, (size_t)(frames - got) * 4);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void audio_error_cb(AAudioStream *s, void *user, aaudio_result_t err);

static void audio_open(void) {
    AAudioStreamBuilder *b;
    if (stream || AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, 44100);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, audio_cb, NULL);
    AAudioStreamBuilder_setErrorCallback(b, audio_error_cb, NULL);
    if (AAudioStreamBuilder_openStream(b, &stream) == AAUDIO_OK) {
        LOGI("audio: %d Hz, burst %d", AAudioStream_getSampleRate(stream), AAudioStream_getFramesPerBurst(stream));
        AAudioStream_requestStart(stream);
    } else stream = NULL;
    AAudioStreamBuilder_delete(b);
}

static void audio_close(void) {
    if (!stream) return;
    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);
    stream = NULL;
}

/* Headphones plugged in or out: the stream is disconnected and must be
 * reopened, but not from the callback's own thread. */
static void *audio_reopen(void *arg) { (void)arg; audio_close(); if (running) audio_open(); return NULL; }
static void audio_error_cb(AAudioStream *s, void *user, aaudio_result_t err) {
    (void)s; (void)user;
    if (err == AAUDIO_ERROR_DISCONNECTED) { pthread_t t; pthread_create(&t, NULL, audio_reopen, NULL); pthread_detach(t); }
}

/* CUE sheets opened through Android's storage framework: track file names
 * mapped to /proc/self/fd paths of descriptors the Java side keeps open. */
static char cue_names[32][256], cue_paths[32][64];
static int cue_count;

static const char *cue_resolve(const char *name) {
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    for (int i = 0; i < cue_count; i++)
        if (!strcasecmp(cue_names[i], base)) return cue_paths[i];
    return NULL;
}

static void nvram_load(void) {
    FILE *f = nvram_path[0] ? fopen(nvram_path, "rb") : NULL;
    if (!f) return;
    if (fread(sys.nvram, 1, NVRAM_SIZE, f) != NVRAM_SIZE) LOGI("NVRAM file was short");
    fclose(f);
    sys.nvram_dirty = 0;
}

static void nvram_save(void) {
    if (!sys.nvram_dirty || !nvram_path[0]) return;
    char tmp[1040];
    snprintf(tmp, sizeof tmp, "%s.tmp", nvram_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    int ok = fwrite(sys.nvram, 1, NVRAM_SIZE, f) == NVRAM_SIZE;
    ok = (fclose(f) == 0) && ok;
    if (ok && rename(tmp, nvram_path) == 0) sys.nvram_dirty = 0;
}

static void present(void) {
    pthread_mutex_lock(&win_lock);
    if (window) {
        ANativeWindow_Buffer b;
        if (ANativeWindow_lock(window, &b, NULL) == 0) {
            if (b.width >= 320 && b.height >= 240) {
                for (int y = 0; y < 240; y++) {
                    const uint32_t *src = &sys.fb[y * 320];
                    uint32_t *dst = (uint32_t *)b.bits + (size_t)y * b.stride;
                    for (int x = 0; x < 320; x++) {          /* 0x00RRGGBB -> RGBX bytes */
                        uint32_t c = src[x];
                        dst[x] = 0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
                    }
                }
            }
            ANativeWindow_unlockAndPost(window);
        }
    }
    pthread_mutex_unlock(&win_lock);
}

static int64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void *run(void *arg) {
    (void)arg;
    const int64_t frame_ns = 1000000000LL / 60;
    int64_t next = now_ns(), fps_t0 = next;
    int fps_frames = 0, since_save = 0;
    while (running) {
        if (paused) {
            nvram_save();
            usleep(20000);
            next = fps_t0 = now_ns(); fps_frames = 0;
            continue;
        }
        if (reset_req) {
            reset_req = 0;
            nvram_save();
            sys_init();
            nvram_load();
            if (disc) cdrom_insert(disc);
        }
        pad_buttons[0] = buttons;
        sys_run_frame();
        present();
        if (++since_save >= 120) { since_save = 0; nvram_save(); }

        fps_frames++;
        int64_t t = now_ns();
        if (t - fps_t0 >= 1000000000LL) { fps = fps_frames * 1e9f / (float)(t - fps_t0); fps_t0 = t; fps_frames = 0; }
        /* Keep the audio buffer near its target by running the emulation
         * up to 1% fast or slow, so sound neither runs dry nor lags. */
        int64_t step = frame_ns;
        if (stream && sound_on) {
            int fill = dsp_audio_available();
            if (fill > AUDIO_TARGET + 1024) step = frame_ns * 101 / 100;
            else if (fill < AUDIO_TARGET - 1024) step = frame_ns * 99 / 100;
        } else {
            int16_t junk[1024];
            while (dsp_audio_take(junk, 512) > 0) {}             /* sound off: discard */
        }
        next += step;
        if (next > t) {
            struct timespec ts = { (time_t)(next / 1000000000LL), (long)(next % 1000000000LL) };
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        } else if (t - next > frame_ns * 4) {
            next = t;                                      /* running slow: don't try to catch up */
        }
    }
    nvram_save();
    return NULL;
}

static void stop_thread(void) {
    if (!running) return;
    running = 0;
    pthread_join(thread, NULL);
    audio_close();
}

#define FN(name) Java_com_retroportlab_threedodroid_NativeLib_##name

JNIEXPORT void JNICALL FN(setNvramPath)(JNIEnv *env, jclass c, jstring path) {
    (void)c;
    const char *p = (*env)->GetStringUTFChars(env, path, NULL);
    snprintf(nvram_path, sizeof nvram_path, "%s", p);
    (*env)->ReleaseStringUTFChars(env, path, p);
}

JNIEXPORT void JNICALL FN(setCueFiles)(JNIEnv *env, jclass c, jobjectArray names, jintArray fds) {
    (void)c;
    cue_count = 0;
    if (!names) return;
    int n = (*env)->GetArrayLength(env, names);
    jint *f = (*env)->GetIntArrayElements(env, fds, NULL);
    for (int i = 0; i < n && i < 32; i++) {
        jstring s = (jstring)(*env)->GetObjectArrayElement(env, names, i);
        const char *u = (*env)->GetStringUTFChars(env, s, NULL);
        snprintf(cue_names[cue_count], sizeof cue_names[0], "%s", u);
        snprintf(cue_paths[cue_count], sizeof cue_paths[0], "/proc/self/fd/%d", f[i]);
        cue_count++;
        (*env)->ReleaseStringUTFChars(env, s, u);
        (*env)->DeleteLocalRef(env, s);
    }
    (*env)->ReleaseIntArrayElements(env, fds, f, JNI_ABORT);
}

/* bios: the console's 1MB ROM; fd: the disc image, opened by the caller and
 * kept open while the game runs; type: "chd", "cue", "iso" or "bin". */
JNIEXPORT jboolean JNICALL FN(loadGame)(JNIEnv *env, jclass c, jbyteArray bios, jint fd, jstring type) {
    (void)c;
    stop_thread();
    last_error[0] = 0;
    jsize len = (*env)->GetArrayLength(env, bios);
    jbyte *b = (*env)->GetByteArrayElements(env, bios, NULL);
    sys_init();
    int bad = sys_load_bios((const uint8_t *)b, (size_t)len);
    (*env)->ReleaseByteArrayElements(env, bios, b, JNI_ABORT);
    if (bad) { snprintf(last_error, sizeof last_error, "The BIOS file should be 1 MB (this one is %d bytes)", (int)len); return JNI_FALSE; }
    nvram_load();

    if (disc) { cdrom_insert(NULL); disc_close(disc); disc = NULL; }
    if (fd >= 0) {
        char path[64], err[200];
        snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
        const char *t = (*env)->GetStringUTFChars(env, type, NULL);
        disc_cue_resolver = cue_resolve;
        disc = disc_open_as(path, t, err, sizeof err);
        disc_cue_resolver = NULL;
        char tname[8]; snprintf(tname, sizeof tname, "%s", t);
        (*env)->ReleaseStringUTFChars(env, type, t);
        if (!disc) { snprintf(last_error, sizeof last_error, "Couldn't open the disc: %s", err); return JNI_FALSE; }
        LOGI("disc: %d tracks, %u sectors", disc_track_count(disc), disc_total_sectors(disc));
        dbg_clear();
        dbg("disc: %s image, %d track(s), %u sectors", tname, disc_track_count(disc), disc_total_sectors(disc));
        for (int i = 0; i < disc_track_count(disc) && i < 99; i++) {
            const DiscTrack *tr = disc_track(disc, i);
            dbg("disc: track %d %s from sector %u", tr->number, tr->type == TRK_AUDIO ? "audio" : "data", tr->start);
        }
        cdrom_insert(disc);
    }
    return JNI_TRUE;
}

JNIEXPORT jstring JNICALL FN(lastError)(JNIEnv *env, jclass c) { (void)c; return (*env)->NewStringUTF(env, last_error); }

JNIEXPORT void JNICALL FN(setSurface)(JNIEnv *env, jclass c, jobject surface) {
    (void)c;
    pthread_mutex_lock(&win_lock);
    if (window) { ANativeWindow_release(window); window = NULL; }
    if (surface) {
        window = ANativeWindow_fromSurface(env, surface);
        if (window) ANativeWindow_setBuffersGeometry(window, 320, 240, WINDOW_FORMAT_RGBX_8888);
    }
    pthread_mutex_unlock(&win_lock);
}

JNIEXPORT void JNICALL FN(start)(JNIEnv *env, jclass c) {
    (void)env; (void)c;
    if (running) return;
    paused = 0;
    running = 1;
    { int16_t junk[1024]; while (dsp_audio_take(junk, 512) > 0) {} }
    audio_open();
    pthread_create(&thread, NULL, run, NULL);
}

JNIEXPORT void JNICALL FN(stop)(JNIEnv *env, jclass c) {
    (void)env; (void)c;
    stop_thread();
    if (disc) { cdrom_insert(NULL); disc_close(disc); disc = NULL; }
}

JNIEXPORT void JNICALL FN(reset)(JNIEnv *env, jclass c) { (void)env; (void)c; reset_req = 1; }
JNIEXPORT void JNICALL FN(setPaused)(JNIEnv *env, jclass c, jboolean p) { (void)env; (void)c; paused = p; }
JNIEXPORT void JNICALL FN(setButtons)(JNIEnv *env, jclass c, jint mask) { (void)env; (void)c; buttons = (uint32_t)mask; }
JNIEXPORT jfloat JNICALL FN(getFps)(JNIEnv *env, jclass c) { (void)env; (void)c; return fps; }
JNIEXPORT void JNICALL FN(setSound)(JNIEnv *env, jclass c, jboolean on) { (void)env; (void)c; sound_on = on; }

JNIEXPORT jstring JNICALL FN(debugLog)(JNIEnv *env, jclass c) {
    (void)c;
    size_t max = 600 * 1024;
    char *buf = malloc(max);
    if (!buf) return (*env)->NewStringUTF(env, "");
    dbg_dump(buf, max);
    for (char *p = buf; *p; p++) if ((unsigned char)*p >= 0x80) *p = '?';
    jstring r = (*env)->NewStringUTF(env, buf);
    free(buf);
    return r;
}
