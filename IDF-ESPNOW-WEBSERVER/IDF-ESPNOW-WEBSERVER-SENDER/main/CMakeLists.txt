idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS ".")

target_add_binary_data(${COMPONENT_TARGET} "index.html" TEXT)