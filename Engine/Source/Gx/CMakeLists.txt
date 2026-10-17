file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

if (WOWDECOMP_PLATFORM_WEB)
    # The OpenGL device runs on WebGL2 through CGxDeviceOpenGL/Web/GlShim; only its WGL extension loader is dropped.
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/CGxDevice(D3d|D3d9Ex)/|/CGxDeviceOpenGL/GlExt[.]cpp$")
elseif (NOT WIN32)
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/CGxDevice(D3d|D3d9Ex|OpenGL)/")
endif ()

add_library(Gx STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Gx
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

if (WIN32)
    target_link_libraries(Gx
        PRIVATE
            d3d9.lib
            nvapi
            opengl32.lib
    )
endif ()
