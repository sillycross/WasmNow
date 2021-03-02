add_library(pochivm 
  codegen_context.cpp
  $<TARGET_OBJECTS:fastinterp>
)

add_dependencies(pochivm fastinterp)

