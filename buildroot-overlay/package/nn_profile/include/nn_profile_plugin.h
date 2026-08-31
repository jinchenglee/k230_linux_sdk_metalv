#ifndef NN_PROFILE_PLUGIN_H
#define NN_PROFILE_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NN_PROFILE_PLUGIN_ABI_VERSION 1u
#define NN_PROFILE_GETTER_SYMBOL "nn_profile_get_model_v1"

typedef enum {
    NN_PROFILE_DTYPE_INVALID = 0,
    NN_PROFILE_DTYPE_F32 = 1,
    NN_PROFILE_DTYPE_F16 = 2,
    NN_PROFILE_DTYPE_I8 = 3,
    NN_PROFILE_DTYPE_U8 = 4,
    NN_PROFILE_DTYPE_I32 = 5,
    NN_PROFILE_DTYPE_I64 = 6,
} nn_profile_dtype_v1;

typedef struct {
    const char *name;
    uint32_t dtype;
    uint32_t rank;
    const uint64_t *shape;
    uint64_t element_count;
} nn_profile_tensor_info_v1;

typedef struct {
    void *data;
    uint64_t element_count;
} nn_profile_tensor_v1;

typedef struct {
    uint32_t node_id;
    uint32_t op_kind;
    uint32_t phase;
    int32_t detail_status;
} nn_profile_error_v1;

typedef int32_t (*nn_profile_run_v1)(
    const nn_profile_tensor_v1 *inputs, uint32_t input_count,
    nn_profile_tensor_v1 *outputs, uint32_t output_count,
    nn_profile_error_v1 *error);

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *model_name;
    const char *build_id;
    uint64_t static_batch_size;
    uint32_t input_count;
    uint32_t output_count;
    const nn_profile_tensor_info_v1 *inputs;
    const nn_profile_tensor_info_v1 *outputs;
    uint64_t macs_per_inference;
    uint64_t flops_per_inference;
    nn_profile_run_v1 run;
} nn_profile_model_v1;

typedef const nn_profile_model_v1 *
    (*nn_profile_get_model_v1_fn)(void);

const nn_profile_model_v1 *nn_profile_get_model_v1(void);

#ifdef __cplusplus
}
#endif

#endif
