/* Compiled against the actual patched Android SDL update function. */
#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef uint64_t Uint64;
typedef struct { int64_t timestamp; float data[16]; } ASensorEvent;
struct sensor_hwdata { void *eventqueue; };
typedef struct { struct sensor_hwdata *hwdata; } SDL_Sensor;
struct android_poll_source { int unused; };
#define LOOPER_ID_USER 3
#define SDL_zero(value) memset(&(value), 0, sizeof(value))
#define SDL_arraysize(array) (sizeof(array) / sizeof((array)[0]))

static int queued, received;
static Uint64 timestamps[2];
static float axes[2][3];

static int ALooper_pollOnce(int timeout, void *fd, int *events, void **source)
{
    (void)fd; (void)events; (void)source;
    assert(timeout == 0);
    return LOOPER_ID_USER;
}
static int ASensorEventQueue_getEvents(void *queue, ASensorEvent *event, int count)
{
    assert(queue != NULL && count == 1);
    if (queued >= 2) return 0;
    event->timestamp = 2500123000LL + queued * 1000000LL;
    event->data[0] = 12.5f + queued;
    event->data[1] = -31.0f;
    event->data[2] = 8.25f;
    ++queued;
    return 1;
}
static void SDL_PrivateSensorUpdate(SDL_Sensor *sensor, Uint64 timestamp, float *data, size_t count)
{
    assert(sensor != NULL && count == 16 && received < 2);
    timestamps[received] = timestamp;
    memcpy(axes[received], data, sizeof(axes[received]));
    ++received;
}

#include "sdl_sensor_fragment.h"

int main(void)
{
    int queue = 1;
    struct sensor_hwdata hwdata = {&queue};
    SDL_Sensor sensor = {&hwdata};
    SDL_ANDROID_SensorUpdate(&sensor);
    assert(received == 2);
    assert(timestamps[0] == 2500123 && timestamps[1] == 2501123);
    assert(axes[0][0] == 12.5f && axes[1][0] == 13.5f);
    assert(axes[0][1] == -31.0f && axes[0][2] == 8.25f);
    return 0;
}
