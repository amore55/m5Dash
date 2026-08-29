idf_component_register(
    SRCS
        "app_main.cpp"
    INCLUDE_DIRS
        "."
        "${CMAKE_SOURCE_DIR}/config"
    REQUIRES
        tab5_board
        dashboard_core
        dashboard_storage
        dashboard_network
        # One component per dashboard page, from plugins/ (added to EXTRA_COMPONENT_DIRS by the
        # top-level CMakeLists.txt). Removing a plugin from the firmware is a one-line change
        # here plus deleting its directory — which is the property that makes "a failing
        # integration must not take the dashboard down" true at build time as well as run time.
        clock
        weather
        elizabeth_line
        summary
        github
        calendar
        json            # cJSON allocator hooks, pointed at PSRAM at boot
        mdns            # app_main advertises <hostname>.local once the link is up
        nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
