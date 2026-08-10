# cli_args_test.cmake
# CLI 参数解析回归测试：直接运行 executor 二进制，断言退出码与输出。
# 覆盖：--pipe 缺失、未知参数、--token-file 不存在/为空（fix-wave 崩溃回归）、
#       传统模式启动警告、RPC_PROXY_TOKEN 环境变量提供 token 时无警告。
# 用法: cmake -DEXECUTOR=<path-to-executor-binary> -P cli_args_test.cmake
# 由 test/CMakeLists.txt 以 ctest 方式调用（$<TARGET_FILE:executor>）。

if(NOT DEFINED EXECUTOR)
  message(FATAL_ERROR "cli_args_test: EXECUTOR not defined")
endif()

set(FAILURES 0)
macro(expect_ok desc)
  if(NOT (${ARGN}))
    message(STATUS "FAIL: ${desc}")
    set(FAILURES 1)
  else()
    message(STATUS "PASS: ${desc}")
  endif()
endmacro()

# --- 1) 缺少 --pipe：exit 1 且打印 Usage ---
execute_process(COMMAND "${EXECUTOR}"
                RESULT_VARIABLE r OUTPUT_VARIABLE out ERROR_VARIABLE err)
expect_ok("missing --pipe exits 1 (got ${r})" r EQUAL 1)
string(FIND "${err}${out}" "Usage:" pos)
expect_ok("missing --pipe prints usage (pos=${pos})" pos GREATER_EQUAL 0)

# --- 2) 未知参数：exit 1 ---
execute_process(COMMAND "${EXECUTOR}" --bogus-flag
                RESULT_VARIABLE r OUTPUT_VARIABLE out ERROR_VARIABLE err)
expect_ok("unknown flag exits 1 (got ${r})" r EQUAL 1)

# --- 3) --token-file 不存在：exit 1 + Cannot open token file（崩溃回归：必须干净报错，绝不 abort）---
execute_process(COMMAND "${EXECUTOR}" --pipe cli_missing_file
                        --token-file "${CMAKE_CURRENT_LIST_DIR}/definitely_missing_token_file_xyz.txt"
                RESULT_VARIABLE r OUTPUT_VARIABLE out ERROR_VARIABLE err)
expect_ok("missing token-file exits 1 (got ${r})" r EQUAL 1)
string(FIND "${err}${out}" "Cannot open token file" pos)
expect_ok("missing token-file prints clean error (pos=${pos})" pos GREATER_EQUAL 0)

# --- 4) --token-file 内容为空：exit 1 + Token file is empty ---
string(RANDOM LENGTH 8 _rnd)
if(WIN32)
  set(_tmpdir "$ENV{TEMP}")
else()
  set(_tmpdir "/tmp")
endif()
set(_empty_file "${_tmpdir}/rpc_cli_empty_token_${_rnd}.txt")
file(WRITE "${_empty_file}" "")
execute_process(COMMAND "${EXECUTOR}" --pipe cli_empty_file --token-file "${_empty_file}"
                RESULT_VARIABLE r OUTPUT_VARIABLE out ERROR_VARIABLE err)
expect_ok("empty token-file exits 1 (got ${r})" r EQUAL 1)
string(FIND "${err}${out}" "Token file is empty" pos)
expect_ok("empty token-file prints clean error (pos=${pos})" pos GREATER_EQUAL 0)
file(REMOVE "${_empty_file}")

# --- 5) 传统模式启动：无 token 时正常启动（阻塞等待连接 → 被超时终止），stderr 含 legacy 警告 ---
set(_pipe1 "cli_legacy_${_rnd}")
if(UNIX)
  file(REMOVE "/tmp/${_pipe1}")
endif()
execute_process(COMMAND "${EXECUTOR}" --pipe "${_pipe1}"
                TIMEOUT 3 RESULT_VARIABLE r OUTPUT_VARIABLE out ERROR_VARIABLE err)
string(FIND "${err}" "legacy mode" pos)
expect_ok("legacy-mode start prints warning (pos=${pos}, result=${r})" pos GREATER_EQUAL 0)
if(UNIX)
  file(REMOVE "/tmp/${_pipe1}")
endif()

# --- 6) RPC_PROXY_TOKEN 提供 token：启动时无 legacy 警告 ---
set(_pipe2 "cli_env_${_rnd}")
if(UNIX)
  file(REMOVE "/tmp/${_pipe2}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "RPC_PROXY_TOKEN=cli-env-token"
                        "${EXECUTOR}" --pipe "${_pipe2}"
                TIMEOUT 3 RESULT_VARIABLE r OUTPUT_VARIABLE out ERROR_VARIABLE err)
string(FIND "${err}" "legacy mode" pos)
expect_ok("env-token start has NO legacy warning (pos=${pos})" pos EQUAL -1)
if(UNIX)
  file(REMOVE "/tmp/${_pipe2}")
endif()

if(FAILURES)
  message(FATAL_ERROR "cli_args_test: ${FAILURES} check(s) failed")
endif()
message(STATUS "cli_args_test: all checks passed")
