#ifndef STORYTELLER_HEADPHONE__
#define STORYTELLER_HEADPHONE__

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "system/device_model.h"
#include "system/settings.h"
#include "./logs_helper.h"

//
// Miyoo Mini Flip (MIYOO285) headphone jack handling.
// The Flip has no 3.5mm jack: the headset plugs into USB-C.
// Audio routing is a hardware switch driven by GPIO:
//   GPIO 45 (input)  : jack detection - 1 = headset plugged in
//   GPIO 44 (output) : audio switch   - 1 = headphones (speaker cut), 0 = speaker
// Reference: Onion-OS keymon.c (initHeadphoneJack / checkHeadphoneJack)
//

#define HEADPHONE_DETECT_GPIO "/sys/class/gpio/gpio45/value"
#define AUDIO_SWITCH_GPIO "/sys/class/gpio/gpio44/value"
#define GPIO_EXPORT_PATH "/sys/class/gpio/export"
#define GPIO45_DIRECTION "/sys/class/gpio/gpio45/direction"
#define GPIO44_DIRECTION "/sys/class/gpio/gpio44/direction"

#define HEADPHONE_CHECK_INTERVAL_MS 500

static bool headphone_available = false;
static int headphone_last_state = -1;
static long headphone_last_check_ms = 0;

static int headphone_gpioReadInt(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int val = -1;
    if (fscanf(f, "%d", &val) != 1) val = -1;
    fclose(f);
    return val;
}

static void headphone_gpioWriteInt(const char *path, int value) {
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d", value);
        fclose(f);
    }
}

static void headphone_gpioExport(int number) {
    char valuePath[64];
    snprintf(valuePath, sizeof(valuePath), "/sys/class/gpio/gpio%d/value", number);
    // Ne pas ré-exporter un GPIO déjà exporté (l'écriture échouerait)
    if (access(valuePath, F_OK) != 0) {
        FILE *f = fopen(GPIO_EXPORT_PATH, "w");
        if (f) {
            fprintf(f, "%d", number);
            fclose(f);
        }
        usleep(50000); // attendre que le nœud sysfs apparaisse
    }
}

static void headphone_gpioSetDirection(const char *path, const char *dir) {
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s", dir);
        fclose(f);
    }
}

static long headphone_nowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void headphone_init(void) {
    if (DEVICE_ID != MIYOO285) {
        headphone_available = false;
        return;
    }

    headphone_gpioExport(45);
    headphone_gpioExport(44);

    headphone_gpioSetDirection(GPIO45_DIRECTION, "in");
    headphone_gpioSetDirection(GPIO44_DIRECTION, "out");

    headphone_available = (access(HEADPHONE_DETECT_GPIO, F_OK) == 0);

    // Synchronisation du routage initial (casque peut déjà être branché au boot)
    if (headphone_available) {
        headphone_last_state = headphone_gpioReadInt(HEADPHONE_DETECT_GPIO);
        if (headphone_last_state != -1) {
            headphone_gpioWriteInt(AUDIO_SWITCH_GPIO, headphone_last_state);
        }
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "headphone init: available=%d state=%d",
             headphone_available, headphone_last_state);
    writeLog("headphone", msg);
}

void headphone_check(void) {
    if (!headphone_available) return;

    long now_ms = headphone_nowMs();
    if (now_ms - headphone_last_check_ms < HEADPHONE_CHECK_INTERVAL_MS) return;
    headphone_last_check_ms = now_ms;

    int current = headphone_gpioReadInt(HEADPHONE_DETECT_GPIO);
    if (current == -1) return;

    if (current != headphone_last_state) {
        headphone_last_state = current;
        // Commute le routage audio : 1 = casque (HP coupé), 0 = HP
        headphone_gpioWriteInt(AUDIO_SWITCH_GPIO, current);

        if (current == 1) {
            // Restauration du volume à la connexion (comportement keymon)
            settings_setVolume(settings.volume, true);
        }

        char msg[64];
        snprintf(msg, sizeof(msg), "headphone %s",
                 current ? "connected" : "disconnected");
        writeLog("headphone", msg);
    }
}

#endif // STORYTELLER_HEADPHONE__
