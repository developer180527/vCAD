function(cad_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /EHsc)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic
      -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
      -Wcast-align -Wunused -Wconversion -Wsign-conversion
      -Wnull-dereference -Wdouble-promotion)
  endif()
endfunction()
