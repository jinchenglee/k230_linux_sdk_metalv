#include "benchmark.h"

#include <iostream>

int main(int argc, char* argv[])
{
    return apriltag_bench::benchmark_main(
        argc, const_cast<const char* const*>(argv), std::cout, std::cerr);
}
