if(NOT DEFINED PRODUCTION OR NOT DEFINED PROFILE OR NOT DEFINED NM OR
   NOT DEFINED STRINGS OR NOT DEFINED PRODUCTION_ID OR NOT DEFINED PROFILE_ID)
    message(FATAL_ERROR "benchmark build verification arguments are incomplete")
endif()
execute_process(COMMAND "${NM}" -g "${PRODUCTION}" OUTPUT_VARIABLE production_nm
                RESULT_VARIABLE production_nm_result)
execute_process(COMMAND "${NM}" -g "${PROFILE}" OUTPUT_VARIABLE profile_nm
                RESULT_VARIABLE profile_nm_result)
if(NOT production_nm_result EQUAL 0 OR NOT profile_nm_result EQUAL 0)
    message(FATAL_ERROR "nm failed while verifying benchmark executables")
endif()
if(production_nm MATCHES "apriltag_get_ccl_profile_v1")
    message(FATAL_ERROR "production benchmark contains profile getter")
endif()
if(NOT profile_nm MATCHES "apriltag_get_ccl_profile_v1")
    message(FATAL_ERROR "profile benchmark does not contain profile getter")
endif()
execute_process(COMMAND "${STRINGS}" "${PRODUCTION}" OUTPUT_VARIABLE production_strings
                RESULT_VARIABLE production_strings_result)
execute_process(COMMAND "${STRINGS}" "${PROFILE}" OUTPUT_VARIABLE profile_strings
                RESULT_VARIABLE profile_strings_result)
if(NOT production_strings_result EQUAL 0 OR NOT profile_strings_result EQUAL 0)
    message(FATAL_ERROR "strings failed while verifying benchmark executables")
endif()
string(FIND "${production_strings}" "${PRODUCTION_ID}" production_id_at)
string(FIND "${profile_strings}" "${PROFILE_ID}" profile_id_at)
string(FIND "${production_strings}" "${PROFILE_ID}" profile_id_in_production)
string(FIND "${profile_strings}" "${PRODUCTION_ID}" production_id_in_profile)
if(NOT PRODUCTION_ID MATCHES "_kernelabi-[0-9a-f]+_" OR
   PRODUCTION_ID MATCHES "profileabi" OR PRODUCTION_ID MATCHES "_profile-" OR
   NOT PROFILE_ID MATCHES "_profile-[0-9a-f]+_" OR
   NOT PROFILE_ID MATCHES "_profileabi-[0-9a-f]+_" OR
   NOT PROFILE_ID MATCHES "_kernelabi-[0-9a-f]+_" OR
   PROFILE_ID MATCHES "_clib-" OR
   production_id_at EQUAL -1 OR profile_id_at EQUAL -1 OR
   NOT profile_id_in_production EQUAL -1 OR NOT production_id_in_profile EQUAL -1)
    message(FATAL_ERROR "benchmark executable build IDs are missing or crossed")
endif()
