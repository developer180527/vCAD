# core/log — the logging façade. No dependencies at all, deliberately: every other core module may
# use it, and it must compile for the iPad target where there is no toolkit.
cad_core_library(cad_log
  src/Log.cpp
)
target_include_directories(cad_log PUBLIC include)
