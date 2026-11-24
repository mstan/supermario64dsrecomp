# recomp-ui's runtime menu uses the theme resolver. The pinned runner's
# source list predates that dependency.
function(sm64ds_add_runner_theme)
    get_target_property(runner_sources nds_runner SOURCES)
    set(theme_source "${NDS_RECOMP_UI_ROOT}/src/common/launcher_theme_overrides.c")
    if(NOT theme_source IN_LIST runner_sources)
        target_sources(nds_runner PRIVATE "${theme_source}")
    endif()
endfunction()

cmake_language(DEFER CALL sm64ds_add_runner_theme)
