# fake_symbol_resolver class inherits a LLVM class,
# so we must compile it with -fno-rtti
#
add_library(fake_symbol_resolver_util OBJECT
  fake_symbol_resolver.cpp
)
set_target_properties(fake_symbol_resolver_util PROPERTIES COMPILE_FLAGS " -fno-rtti ")

add_library(runtime_lib_builder_util OBJECT
  symbol_list_util.cpp
  sha1.cpp
)

