idf_component_register(SRCS "src/main.c"
                    INCLUDE_DIRS "../components/common/include"
                    REQUIRES 
                        wifi_module 
                        mqtt_module
                        uart_module
                        common
)
