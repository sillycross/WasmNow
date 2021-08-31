# remove -O0 -g flag, and add -O3 flag
# This is very tricky: we are going to compile some crazily compile-time-intensive CPP files and 
# then JIT them to get their symbol-to-code mappings.
# So the total build time is clang's time to compile CPP file + LLVM JIT's time to JIT execute them.
# Despite this is only executed as a build step, so all it impacts is the developer's user experience, 
# we still want to make this step faster so we don't have to wait too long for every build.
#
# -O3 results in better inlining, so less symbols to resolve, so JIT can be faster (significantly). 
# On the other hand, -O3 results in the compiler doing more work, also slowing down compilation significantly 
# if inlining is done too extensively.
#
# So for fastest build time, we always use -O3, but in source code we manually add 'noinline' and 'optnone' 
# to places where inlining actually hurts overall build time, to manually tune for fastest build time.
# 
string(REPLACE " -O0 " " " CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
string(REPLACE " -g " " " CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -fconstexpr-steps=100000000 -fconstexpr-depth=131072 -ftemplate-depth=131072 -mavx2 -fno-math-errno ")

SET(FASTINTERP_TPL_SOURCES
  wasm_int_binary_ops.cpp
  wasm_float_binary_ops.cpp
  wasm_int_unary_ops.cpp
  wasm_float_unary_ops.cpp
  wasm_relational_ops.cpp
  wasm_test_ops.cpp
  wasm_memory_load_ops.cpp
  wasm_memory_store_ops_1.cpp
  wasm_memory_store_ops_2.cpp
  wasm_conversion_ops.cpp
  wasm_bitcast_ops.cpp
  wasm_extend_ops.cpp
  wasm_global_set.cpp
  wasm_global_get.cpp
  wasm_local_store_tee.cpp
  wasm_local_get.cpp
  wasm_select_float_ops.cpp
  wasm_select_int_ops.cpp
  wasm_noop.cpp
  wasm_store_block_simple_result.cpp
  wasm_branch.cpp
  wasm_constant32.cpp
  wasm_constant64.cpp
  wasm_return.cpp
  wasm_return_none.cpp
  fastinterp_tpl_cdecl_interface.cpp
  wasm_call.cpp
  wasm_call_store_int_param.cpp
  wasm_call_store_float_param.cpp
  wasm_call_switch_sf.cpp
  wasm_br_table.cpp
  wasm_call_indirect.cpp
)

SET(FASTINTERP_SOURCES
  fastinterp_codegen_helper.cpp
  x86_64_get_fs_base_helper.cpp
)

add_library(fastinterp_tpl_ir OBJECT
  ${FASTINTERP_TPL_SOURCES}
)
# "-Walloca" is required: LLVM seems to generate buggy assembly in GHC calling convention 
# if variable-sized alloca is used. It failed to recognize that rbp is garbaged by the call.
# 
set_target_properties(fastinterp_tpl_ir PROPERTIES COMPILE_FLAGS " -emit-llvm -fno-exceptions -Walloca ")

set(generated_src_list "")
foreach(cur_src ${FASTINTERP_TPL_SOURCES})
  set(cur_generated_src "${GENERATED_FILES_DIR}/${cur_src}.lib.generated.cpp")
  list(APPEND generated_src_list "${cur_generated_src}")
  set_source_files_properties(${cur_generated_src} PROPERTIES GENERATED true)
endforeach()

add_executable(build_fast_interp_lib
  build_fast_interp_lib.cpp   
  $<TARGET_OBJECTS:fake_symbol_resolver_util>
  $<TARGET_OBJECTS:runtime_lib_builder_util>
) 
# for some reason, get a weird 'typeinfo for ErrorInfoBase does not exist' link error in debug
# compile it in -fno-rtti for now, consider build llvm with rtti if we really need to fix it
#
set_target_properties(build_fast_interp_lib PROPERTIES COMPILE_FLAGS " -fno-rtti ")

target_link_libraries(build_fast_interp_lib PUBLIC
  ${LLVM_EXTRA_LINK_LIBRARIES}
)

set_source_files_properties(${GENERATED_FILES_DIR}/fastinterp_fwd_declarations.generated.h PROPERTIES GENERATED true)
set_source_files_properties(${GENERATED_FILES_DIR}/fastinterp_library.generated.h PROPERTIES GENERATED true)

add_library(fastinterp OBJECT
  ${FASTINTERP_SOURCES}
  ${generated_src_list}
)

add_custom_command(
  OUTPUT ${GENERATED_FILES_DIR}/fastinterp_fwd_declarations.generated.h
  OUTPUT ${GENERATED_FILES_DIR}/fastinterp_library.generated.h
  OUTPUT ${generated_src_list}
  COMMAND ${PROJECT_BINARY_DIR}/fastinterp/build_fast_interp_lib '$<TARGET_OBJECTS:fastinterp_tpl_ir>' "${GENERATED_FILES_DIR}"
  DEPENDS $<TARGET_OBJECTS:fastinterp_tpl_ir> build_fast_interp_lib 
)

