cmake_minimum_required(VERSION 3.5)

set(CMAKE_BUILD_TYPE Debug)

project(vvcam)

# Starter profile only: it preserves the board's OV5647 tuning values while
# changing the profile key to 1280x720.  It is intentionally installed as a
# separate asset, so a calibrated export can replace it without disturbing
# the validated 1080p scene.
foreach(profile ov5647.xml ov5647.manual.json ov5647.auto.json)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/configs/${profile}" profile_text)
    string(REPLACE "1920x1080" "1280x720" profile_text "${profile_text}")
    # The XML header carries the selected sensor geometry both as its profile
    # name and as scalar values.  Keep these two representations consistent.
    # These exact bracketed scalars occur only in ov5647.xml's header; do not
    # alter arbitrary calibration-table values such as 1920 or 1080.
    if(profile STREQUAL "ov5647.xml")
        string(REPLACE "[ 1920]" "[ 1280]" profile_text "${profile_text}")
        string(REPLACE "[ 1080]" "[ 720]" profile_text "${profile_text}")
    endif()
    string(REPLACE "ov5647" "ov5647-1280x720" profile_name "${profile}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${profile_name}" "${profile_text}")
    list(APPEND OV5647_720P_STARTER_PROFILES
         "${CMAKE_CURRENT_BINARY_DIR}/${profile_name}")
endforeach()

file(GLOB SOURCES "src/*.c")
add_library(${PROJECT_NAME} SHARED ${SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC include)

set(CAPABILITIES_SOURCES ${SOURCES})
list(REMOVE_ITEM CAPABILITIES_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/lib.c")
add_library(vvcam-capabilities SHARED ${CAPABILITIES_SOURCES})
target_include_directories(vvcam-capabilities PUBLIC include)

add_subdirectory(v4l2-drm)

add_subdirectory(v4l2-drm-scene)

install(TARGETS ${PROJECT_NAME} DESTINATION /usr/lib)
install(TARGETS vvcam-capabilities DESTINATION /usr/lib)
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/isp_media_server DESTINATION /usr/bin)
install(DIRECTORY include/ DESTINATION /usr/include)
install(DIRECTORY configs/ DESTINATION /etc/vvcam)
install(FILES ${OV5647_720P_STARTER_PROFILES} DESTINATION /etc/vvcam)
