#include "nn_profile_plugin.h"

static const uint64_t input_shape[] = {8, 4};
static const uint64_t output_shape[] = {8, 1};
static const nn_profile_tensor_info_v1 inputs[] = {
    {"input", NN_PROFILE_DTYPE_F32, 2, input_shape, 32},
};
static const nn_profile_tensor_info_v1 outputs[] = {
    {"output", NN_PROFILE_DTYPE_F32, 2, output_shape, 8},
};

static int32_t run(const nn_profile_tensor_v1 *in, uint32_t in_count,
                   nn_profile_tensor_v1 *out, uint32_t out_count,
                   nn_profile_error_v1 *error)
{
    (void) error;
    if (in_count != 1 || out_count != 1 || in[0].element_count != 32 ||
        out[0].element_count != 8)
        return 3;
    const float *x = (const float *) in[0].data;
    float *y = (float *) out[0].data;
    for (uint32_t row = 0; row < 8; ++row) {
        float sum = 0.0f;
        for (uint32_t col = 0; col < 4; ++col)
            sum += x[row * 4 + col];
        y[row] = sum;
    }
    return 0;
}

static const nn_profile_model_v1 model = {
    NN_PROFILE_PLUGIN_ABI_VERSION, sizeof(nn_profile_model_v1),
    "mock-batch8", "mock-v1", 8, 1, 1, inputs, outputs, 32, 64, run,
};

const nn_profile_model_v1 *nn_profile_get_model_v1(void)
{
    return &model;
}
