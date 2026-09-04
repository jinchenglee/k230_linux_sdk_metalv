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
    # The lens-shading mesh is stored as 33x33 samples plus sector tables that
    # give, in pixels, how far apart those samples sit.  The sector tables are
    # therefore a statement about the frame the mesh was calibrated against.
    # Relabelling a 1920x1080 profile as 1280x720 without rescaling them leaves
    # the ISP walking the mesh with a row pitch that overruns the shorter
    # frame, which shows up as severe horizontal banding in luma (the mesh row
    # applied to a given image row is effectively arbitrary).  Rescale both
    # axes by 2/3 so the sectors span the 720p frame: 32 x 60 -> 32 x 40 across
    # 1280 columns, and a 540-row half-height table -> 360 rows.  The mesh
    # samples themselves are untouched and remain 1080p-derived, so corner
    # falloff is only approximately corrected until a real 720p flat-field
    # calibration replaces this starter profile.
    if(profile STREQUAL "ov5647.manual.json")
        string(REPLACE "60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60"
                       "40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40"
               profile_text "${profile_text}")
        string(REPLACE "34, 34, 33, 34, 34, 34, 33, 34, 34, 34, 33, 34, 34, 34, 33, 34"
                       "23, 22, 23, 22, 23, 22, 23, 22, 23, 22, 23, 22, 23, 22, 23, 22"
               profile_text "${profile_text}")
    endif()
    if(profile STREQUAL "ov5647.xml")
        string(REPLACE "[60 60 60 60 60 60 60 60 60 60 60 60 60 60 60 60 60 60 60 60 60 60 60 60 60 60 60 60 60 60 60 60]"
                       "[40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40]"
               profile_text "${profile_text}")
        string(REPLACE "[34 34 33 34 34 34 33 34 34 34 33 34 34 34 33 34]"
                       "[23 22 23 22 23 22 23 22 23 22 23 22 23 22 23 22]"
               profile_text "${profile_text}")
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
set(VVCAM_ISP_MEDIA_SERVER "isp_media_server" CACHE STRING
    "ISA-appropriate prebuilt ISP media server")
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/${VVCAM_ISP_MEDIA_SERVER}
        DESTINATION /usr/bin RENAME isp_media_server)
install(DIRECTORY include/ DESTINATION /usr/include)
install(DIRECTORY configs/ DESTINATION /etc/vvcam)
install(FILES ${OV5647_720P_STARTER_PROFILES} DESTINATION /etc/vvcam)
