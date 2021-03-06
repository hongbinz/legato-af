#*******************************************************************************
# Copyright (C) Sierra Wireless Inc. Use of this work is subject to license.
#
# Contributors:
#     Sierra Wireless - initial API and implementation
#*******************************************************************************

set(TEST_EXEC logTest)

# Configure file

get_target_property(TESTLOG_LOGDAEMON_PATH "logCtrlDaemon" LOCATION)
get_target_property(TESTLOG_SERVICE_DIRECTORY_PATH "serviceDirectory" LOCATION)
set(TESTLOG_LOGTOOL_PATH        ${LEGATO_BINARY_DIR}/bin/log)
set(TESTLOG_STDERR_FILE_PATH    ${CMAKE_CURRENT_BINARY_DIR}/logStdErr.txt)
set(TESTLOG_LOGTEST_PATH        ${EXECUTABLE_OUTPUT_PATH}/${TEST_EXEC})

add_definitions(-DTESTLOG_LOGTOOL_PATH="${TESTLOG_LOGTOOL_PATH}"
                -DTESTLOG_STDERR_FILE_PATH="${TESTLOG_STDERR_FILE_PATH}")

# Executable

set_legato_component(${TEST_EXEC})

add_legato_executable(${TEST_EXEC}
    main.c
    component1.c
    component1Helper.c
    component2.c
)

# Dependencies

add_dependencies(${TEST_EXEC} logDaemon)
add_dependencies(${TEST_EXEC} log)

# Testing

add_test(testFwLog ${CMAKE_CURRENT_SOURCE_DIR}/testFwLog.sh)
set_tests_properties(testFwLog PROPERTIES
    ENVIRONMENT "SERVICE_DIRECTORY_PATH=${TESTLOG_SERVICE_DIRECTORY_PATH};LOGDAEMON_PATH=${TESTLOG_LOGDAEMON_PATH};LOG_STDERR_PATH=${TESTLOG_STDERR_FILE_PATH};LOGTOOL_PATH=${TESTLOG_LOGTOOL_PATH};LOGTEST_PATH=${TESTLOG_LOGTEST_PATH}")
