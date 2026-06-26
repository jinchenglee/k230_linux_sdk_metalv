#include "display.h"
#include <cstdio>
#include <drm/drm_fourcc.h>
#include <iostream>
#include <pipeline.hpp>
#include <signal.h>

using namespace pipeline;
using namespace std;

Pipeline main_pipe;

void sighandler(int sig) {
    main_pipe.stop();
}

int main(void) {
    auto d = display_init(0);
    if (!d) {
        cerr << "display_init failed: no connected display found" << endl;
        return -1;
    }
    // Use the negotiated CRTC mode dimensions, not a hardcoded 1920x1080
    unsigned width = d->width;
    unsigned height = d->height;
    auto display = Display(d);
    if (!display.createChannel(width, height, DRM_FORMAT_NV12)) {
        cerr << "create display channel error" << endl;
        return -1;
    }
    if (auto vicap = VideoCapture::create(1, width, height, V4L2_PIX_FMT_NV12)) {
        if (!main_pipe.link(*vicap, display)) {
            cerr << "link error" << endl;
            return -1;
        }
        signal(SIGINT, sighandler);
        signal(SIGTERM, sighandler);
        main_pipe.run();
    } else {
        cerr << "VideoCapture::create failed" << endl;
        return -1;
    }
}
