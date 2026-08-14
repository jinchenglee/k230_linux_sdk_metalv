if(NOT DEFINED PACKAGE_DIR)
    message(FATAL_ERROR "PACKAGE_DIR is required")
endif()

file(READ "${PACKAGE_DIR}/CMakeLists.txt" cmake_source)
file(READ "${PACKAGE_DIR}/apriltag_demo.mk" package_makefile)

if(cmake_source MATCHES "apriltag_sequence_tests")
    message(FATAL_ERROR "sequence tests must not be built for the target")
endif()
if(NOT EXISTS "${PACKAGE_DIR}/tests/run_sequence_host_tests.sh")
    message(FATAL_ERROR "sequence host test runner is missing")
endif()
string(FIND "${package_makefile}"
    "CXX=\"$(HOSTCXX)\" bash $(@D)/tests/run_sequence_host_tests.sh"
    host_runner_position)
if(host_runner_position EQUAL -1)
    message(FATAL_ERROR "Buildroot does not run sequence tests with HOSTCXX")
endif()
string(FIND "${package_makefile}"
    "APRILTAG_DEMO_POST_BUILD_HOOKS += APRILTAG_DEMO_RUN_SEQUENCE_HOST_TESTS"
    post_build_hook_position)
if(post_build_hook_position EQUAL -1)
    message(FATAL_ERROR "sequence host tests are not a post-build hook")
endif()
