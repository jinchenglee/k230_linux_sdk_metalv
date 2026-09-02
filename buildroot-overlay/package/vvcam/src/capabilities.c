#include <string.h>
#include <vvcam_sensor.h>

extern struct vvcam_sensor vvcam_ov5647;
extern struct vvcam_sensor vvcam_imx335;
extern struct vvcam_sensor vvcam_gc2093;
extern struct vvcam_sensor vvcam_gc2053;
extern struct vvcam_sensor vvcam_bf3238;

/*
 * The closed isp_media_server owns registration through vvcam_sensor_add().
 * This separate DSO intentionally contains no registration entry point: it
 * lets capture clients translate a generic capability into the active
 * driver's opaque mode index without loading the server-facing DSO.
 */
int vvcam_sensor_find_mode(const char *sensor_name, uint16_t width,
                           uint16_t height, uint32_t fps,
                           uint32_t *mode_index)
{
    struct vvcam_sensor *sensors[] = {
        &vvcam_ov5647, &vvcam_imx335, &vvcam_gc2093,
        &vvcam_gc2053, &vvcam_bf3238,
    };
    struct vvcam_sensor_mode mode;
    size_t i;
    uint32_t index;

    if (!sensor_name || !mode_index)
        return -1;
    for (i = 0; i < sizeof(sensors) / sizeof(sensors[0]); ++i) {
        if (strcmp(sensors[i]->name, sensor_name) != 0)
            continue;
        for (index = 0;
             sensors[i]->ctrl.enum_mode(NULL, index, &mode) == 0;
             ++index) {
            if (mode.width == width && mode.height == height &&
                mode.ae_info.cur_fps == fps) {
                *mode_index = index;
                return 0;
            }
        }
        return -1;
    }
    return -1;
}
