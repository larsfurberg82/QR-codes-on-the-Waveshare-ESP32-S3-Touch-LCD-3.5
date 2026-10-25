idf_component_register(SRCS "qrcode.c" INCLUDE_DIRS ".")

# Map Kconfig -> the build-time constants qrcode.h expects.
if(CONFIG_QR_ECC_LEVEL_L)
    set(QR_ECC_NUM 0)
elseif(CONFIG_QR_ECC_LEVEL_M)
    set(QR_ECC_NUM 1)
elseif(CONFIG_QR_ECC_LEVEL_Q)
    set(QR_ECC_NUM 2)
else()
    set(QR_ECC_NUM 3)
endif()

# PUBLIC so `main` sees the same QR_VERSION and therefore the same QR_SIZE.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    QR_VERSION=${CONFIG_QR_VERSION}
    QR_ECC_LEVEL=${QR_ECC_NUM})
if(CONFIG_QR_FIXED_MASK_ENABLE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC QR_FIXED_MASK=${CONFIG_QR_FIXED_MASK})
endif()
