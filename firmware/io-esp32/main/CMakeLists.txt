set(srcs
        "main.c"
        "sketch.cpp"
        "haltz_selftest.cpp")

set(requires "bluepad32" "bluepad32_arduino" "arduino" "btstack" "haltz_proto")

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS "."
                    REQUIRES "${requires}")
