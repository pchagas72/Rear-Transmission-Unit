idf_component_register(SRCS "ebyte_e32.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board driver esp_driver_gpio esp_driver_uart freertos log)

# Aplica regras mais rígidas na hora de compilar APENAS este componente (PRIVATE).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
