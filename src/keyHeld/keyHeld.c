#include <fcntl.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

// Exit 0 if a key is currently held down, 1 otherwise.
// Unlike reading /dev/input events, the key state (EVIOCGKEY) also reports
// keys pressed before this program started (e.g. held since power on).

#define INPUT_DEVICES_COUNT 4
#define POLL_INTERVAL_MS 50

static bool keyheld_isPressed(int keyCode)
{
    unsigned char keys[KEY_MAX / 8 + 1];
    char path[32];

    for (int i = 0; i < INPUT_DEVICES_COUNT; ++i) {
        sprintf(path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        memset(keys, 0, sizeof(keys));
        int ret = ioctl(fd, EVIOCGKEY(sizeof(keys)), keys);
        close(fd);
        if (ret >= 0 && (keys[keyCode / 8] & (1 << (keyCode % 8)))) {
            return true;
        }
    }
    return false;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s key_code [wait_ms]\n", argv[0]);
        return 2;
    }

    int keyCode = atoi(argv[1]);
    int waitMs = argc > 2 ? atoi(argv[2]) : 0;

    if (keyCode <= 0 || keyCode > KEY_MAX || waitMs < 0) {
        fprintf(stderr, "Invalid arguments\n");
        return 2;
    }

    for (int elapsed = 0;; elapsed += POLL_INTERVAL_MS) {
        if (keyheld_isPressed(keyCode)) {
            return 0;
        }
        if (elapsed >= waitMs) {
            return 1;
        }
        usleep(POLL_INTERVAL_MS * 1000);
    }
}
