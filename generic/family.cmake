set(VIADUCT_UARCHES "example")
foreach(uarch ${VIADUCT_UARCHES})
    aux_source_directory(${family}/viaduct/${uarch} UARCH_FILES)
    foreach(target ${family_targets})
        target_sources(${target} PRIVATE ${UARCH_FILES})
    endforeach()
endforeach(uarch)
