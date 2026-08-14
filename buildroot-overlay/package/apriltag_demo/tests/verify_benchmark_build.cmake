if(NOT DEFINED PRODUCTION OR NOT DEFINED PROFILE OR NOT DEFINED SEQUENCE OR
   NOT DEFINED WORKLOAD OR NOT DEFINED DEMO OR NOT DEFINED C_DEMO OR NOT DEFINED NM OR
   NOT DEFINED STRINGS OR NOT DEFINED PRODUCTION_ID OR NOT DEFINED PROFILE_ID OR
   NOT DEFINED SEQUENCE_ID)
    message(FATAL_ERROR "benchmark build verification arguments are incomplete")
endif()
execute_process(COMMAND "${NM}" -g "${PRODUCTION}" OUTPUT_VARIABLE production_nm
                RESULT_VARIABLE production_nm_result)
execute_process(COMMAND "${NM}" -g "${PROFILE}" OUTPUT_VARIABLE profile_nm
                RESULT_VARIABLE profile_nm_result)
execute_process(COMMAND "${NM}" -g "${SEQUENCE}" OUTPUT_VARIABLE sequence_nm
                RESULT_VARIABLE sequence_nm_result)
execute_process(COMMAND "${NM}" -g "${WORKLOAD}" OUTPUT_VARIABLE workload_nm
                RESULT_VARIABLE workload_nm_result)
execute_process(COMMAND "${NM}" -g "${DEMO}" OUTPUT_VARIABLE demo_nm
                RESULT_VARIABLE demo_nm_result)
execute_process(COMMAND "${NM}" -g "${C_DEMO}" OUTPUT_VARIABLE c_demo_nm
                RESULT_VARIABLE c_demo_nm_result)
if(NOT production_nm_result EQUAL 0 OR NOT profile_nm_result EQUAL 0 OR
   NOT sequence_nm_result EQUAL 0 OR NOT workload_nm_result EQUAL 0 OR
   NOT demo_nm_result EQUAL 0 OR
   NOT c_demo_nm_result EQUAL 0)
    message(FATAL_ERROR "nm failed while verifying benchmark executables")
endif()

function(nm_has_symbol output symbol result)
    string(REPLACE "\r\n" "\n" normalized "${output}")
    string(REPLACE "\r" "\n" normalized "${normalized}")
    string(REPLACE "\n" ";" lines "${normalized}")
    set(found FALSE)
    foreach(line IN LISTS lines)
        string(STRIP "${line}" line)
        if(line MATCHES "(^|[ \t])${symbol}$")
            set(found TRUE)
            break()
        endif()
    endforeach()
    set(${result} "${found}" PARENT_SCOPE)
endfunction()

nm_has_symbol("${production_nm}" apriltag_get_ccl_profile_v1 production_has_profile)
nm_has_symbol("${profile_nm}" apriltag_get_ccl_profile_v1 profile_has_profile)
nm_has_symbol("${sequence_nm}" apriltag_get_ccl_profile_v1 sequence_has_profile)
nm_has_symbol("${production_nm}" apriltag_get_ccl_scratch_v1 production_has_scratch)
nm_has_symbol("${profile_nm}" apriltag_get_ccl_scratch_v1 profile_has_scratch)
nm_has_symbol("${sequence_nm}" apriltag_get_ccl_scratch_v1 sequence_has_scratch)
foreach(output IN ITEMS production_nm profile_nm sequence_nm workload_nm demo_nm c_demo_nm)
    nm_has_symbol("${${output}}" apriltag_get_ccl_pending_profile_v1
                  ${output}_has_pending)
endforeach()
if(production_has_profile)
    message(FATAL_ERROR "production benchmark contains profile getter")
endif()
if(NOT profile_has_profile)
    message(FATAL_ERROR "profile benchmark does not contain profile getter")
endif()
if(production_has_scratch)
    message(FATAL_ERROR "production benchmark contains scratch getter")
endif()
if(NOT profile_has_scratch)
    message(FATAL_ERROR "profile benchmark does not contain scratch getter")
endif()
if(NOT sequence_has_profile OR NOT sequence_has_scratch)
    message(FATAL_ERROR "sequence benchmark does not contain profile/scratch getters")
endif()
if(production_nm_has_pending OR workload_nm_has_pending OR demo_nm_has_pending OR
   c_demo_nm_has_pending OR
   NOT profile_nm_has_pending OR NOT sequence_nm_has_pending)
    message(FATAL_ERROR "pending profile getter is not isolated to profile consumers")
endif()
foreach(symbol IN ITEMS apriltag_get_ccl_grouping_profile_v1 apriltag_set_ccl_grouping_mode_v1)
    foreach(output IN ITEMS production_nm profile_nm sequence_nm workload_nm demo_nm c_demo_nm)
        nm_has_symbol("${${output}}" "${symbol}" has_obsolete_symbol)
        if(has_obsolete_symbol)
            message(FATAL_ERROR "executable contains obsolete grouping symbol ${symbol}")
        endif()
    endforeach()
endforeach()
nm_has_symbol("${production_nm}" apriltag_set_ccl_scratch_mode_v1 production_has_scratch_setter)
nm_has_symbol("${profile_nm}" apriltag_set_ccl_scratch_mode_v1 profile_has_scratch_setter)
nm_has_symbol("${sequence_nm}" apriltag_set_ccl_scratch_mode_v1 sequence_has_scratch_setter)
nm_has_symbol("${demo_nm}" apriltag_set_ccl_scratch_mode_v1 demo_has_scratch_setter)
nm_has_symbol("${c_demo_nm}" apriltag_set_ccl_scratch_mode_v1 c_demo_has_scratch_setter)
if(NOT production_has_scratch_setter OR NOT profile_has_scratch_setter OR
   NOT sequence_has_scratch_setter)
    message(FATAL_ERROR "a Rust benchmark does not contain the production scratch setter")
endif()
if(NOT demo_has_scratch_setter)
    message(FATAL_ERROR "Rust demo does not contain scratch mode setter")
endif()
if(c_demo_has_scratch_setter)
    message(FATAL_ERROR "C demo contains Rust scratch mode setter")
endif()
execute_process(COMMAND "${STRINGS}" "${PRODUCTION}" OUTPUT_VARIABLE production_strings
                RESULT_VARIABLE production_strings_result)
execute_process(COMMAND "${STRINGS}" "${PROFILE}" OUTPUT_VARIABLE profile_strings
                RESULT_VARIABLE profile_strings_result)
execute_process(COMMAND "${STRINGS}" "${SEQUENCE}" OUTPUT_VARIABLE sequence_strings
                RESULT_VARIABLE sequence_strings_result)
execute_process(COMMAND "${STRINGS}" "${DEMO}" OUTPUT_VARIABLE demo_strings
                RESULT_VARIABLE demo_strings_result)
execute_process(COMMAND "${STRINGS}" "${C_DEMO}" OUTPUT_VARIABLE c_demo_strings
                RESULT_VARIABLE c_demo_strings_result)
if(NOT production_strings_result EQUAL 0 OR NOT profile_strings_result EQUAL 0 OR
   NOT sequence_strings_result EQUAL 0 OR NOT demo_strings_result EQUAL 0 OR
   NOT c_demo_strings_result EQUAL 0)
    message(FATAL_ERROR "strings failed while verifying benchmark executables")
endif()
if(NOT demo_strings MATCHES "--local-ccl-scratch" OR
   NOT demo_strings MATCHES "ccl_scratch=" OR
   NOT c_demo_strings MATCHES "--local-ccl-scratch is only valid for apriltag_demo")
    message(FATAL_ERROR "demo scratch CLI/startup strings are missing")
endif()
string(FIND "${production_strings}" "${PRODUCTION_ID}" production_id_at)
string(FIND "${profile_strings}" "${PROFILE_ID}" profile_id_at)
string(FIND "${production_strings}" "${PROFILE_ID}" profile_id_in_production)
string(FIND "${profile_strings}" "${PRODUCTION_ID}" production_id_in_profile)
string(FIND "${sequence_strings}" "${SEQUENCE_ID}" sequence_id_at)
string(FIND "${profile_strings}" "${SEQUENCE_ID}" sequence_id_in_profile)
string(FIND "${sequence_strings}" "${PROFILE_ID}" profile_id_in_sequence)
if(NOT PRODUCTION_ID MATCHES "_kernelabi-[0-9a-f]+_" OR
   PRODUCTION_ID MATCHES "profileabi" OR PRODUCTION_ID MATCHES "pendingabi" OR
   PRODUCTION_ID MATCHES "_profile-" OR
   NOT PROFILE_ID MATCHES "_profile-[0-9a-f]+_" OR
     NOT PROFILE_ID MATCHES "_profileabi-[0-9a-f]+_" OR
     NOT PROFILE_ID MATCHES "_pendingabi-[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]_" OR
    NOT PROFILE_ID MATCHES "_scratchabi-[0-9a-f]+_" OR
   PROFILE_ID MATCHES "groupingabi" OR
   NOT PROFILE_ID MATCHES "_bufferabi-[0-9a-f]+_" OR
   NOT PROFILE_ID MATCHES "_kernelabi-[0-9a-f]+_" OR
   NOT SEQUENCE_ID MATCHES "^sequence-rvv-" OR
   NOT SEQUENCE_ID MATCHES "_sequence-[0-9a-f]+_" OR
    NOT SEQUENCE_ID MATCHES "_profileabi-[0-9a-f]+_" OR
    NOT SEQUENCE_ID MATCHES "_pendingabi-[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]_" OR
   NOT SEQUENCE_ID MATCHES "_scratchabi-[0-9a-f]+_" OR
   SEQUENCE_ID MATCHES "groupingabi" OR
   NOT SEQUENCE_ID MATCHES "_bufferabi-[0-9a-f]+_" OR
   NOT SEQUENCE_ID MATCHES "_kernelabi-[0-9a-f]+_" OR
   SEQUENCE_ID STREQUAL PROFILE_ID OR
   PROFILE_ID MATCHES "_clib-" OR
   production_id_at EQUAL -1 OR profile_id_at EQUAL -1 OR
   NOT profile_id_in_production EQUAL -1 OR NOT production_id_in_profile EQUAL -1 OR
   sequence_id_at EQUAL -1 OR NOT sequence_id_in_profile EQUAL -1 OR
   NOT profile_id_in_sequence EQUAL -1)
    message(FATAL_ERROR "benchmark executable build IDs are missing or crossed")
endif()
