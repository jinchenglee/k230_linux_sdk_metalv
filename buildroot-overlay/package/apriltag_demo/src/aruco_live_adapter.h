#ifndef ARUCO_LIVE_ADAPTER_H
#define ARUCO_LIVE_ADAPTER_H

#ifdef __cplusplus
extern "C" {
#endif

enum aruco_live_backend {
    ARUCO_LIVE_BACKEND_NANO = 0,
    ARUCO_LIVE_BACKEND_ARUCO2 = 1,
};

/* Select the implementation and project-defined acceptance preset used by
 * later detect calls. Strict requires exact payload and border bits. Tolerant
 * enables each implementation's maximum dictionary correction and border-error
 * allowance; it is a permissive experimental setting, not the default.
 */
int aruco_live_configure(void* handle, int backend, int tolerant);

#ifdef __cplusplus
}
#endif

#endif /* ARUCO_LIVE_ADAPTER_H */
