#ifndef APRILTAG_C_ADAPTER_H
#define APRILTAG_C_ADAPTER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Configure the official C detector before its first detect call.
 *
 * Comparison defaults used by apriltag_c_demo are deliberately conservative:
 * one thread, zero corrected bits, edge refinement off, and decode sharpening
 * off. This keeps its inputs and acceptance policy close to apriltag-rvv while
 * leaving the upstream algorithm unchanged.
 */
int apriltag_c_configure(void* handle,
                         int threads,
                         int bits_corrected,
                         int refine_edges,
                         double decode_sharpening);

#ifdef __cplusplus
}
#endif

#endif /* APRILTAG_C_ADAPTER_H */
