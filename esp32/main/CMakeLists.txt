set(repo ${CMAKE_CURRENT_LIST_DIR}/../..)
idf_component_register(
    SRCS main.cpp linktest.cpp world_app.cpp
         ../selftest/selftest.cpp
         ${repo}/link/link.cpp ${repo}/link/messages.cpp ${repo}/render/render_stream.cpp
         ${repo}/core/world.cpp ${repo}/core/checkpoint.cpp ${repo}/core/nests.cpp ${repo}/core/hunting.cpp ${repo}/core/brain.cpp
         ${repo}/render/render_state.cpp ${repo}/render/life.cpp ${repo}/render/panel.cpp ${repo}/render/observation.cpp
    INCLUDE_DIRS . ../selftest ${repo}/core ${repo}/render ${repo}/link
    PRIV_REQUIRES esp_timer bt nvs_flash esp_driver_spi esp_driver_gpio hal)
# The board world is 128 by 64, matching the observatory firmware.
target_compile_definitions(${COMPONENT_LIB} PRIVATE PETRI_WIDE_WORLD=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
