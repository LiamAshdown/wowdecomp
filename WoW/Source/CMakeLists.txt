file(GLOB PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

if (WOWDECOMP_PLATFORM_WEB)
    file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS "Web/*.cpp")
elseif (WOWDECOMP_ARCH_64)
    file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS "Win64/*.cpp")
else ()
    file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS "Win32/*.cpp")
endif ()

add_executable(Wow WIN32 ${PRIVATE_SOURCES} ${PLATFORM_SOURCES})

if (WOWDECOMP_PLATFORM_WEB)
    set(WOW_OUTPUT_NAME Wow)
    set(WOWDECOMP_WEB_BIN_DIR ${CMAKE_SOURCE_DIR}/bin/Web CACHE PATH "Where the browser build writes Wow.js/Wow.wasm")
    set(WOW_BIN_DIR ${WOWDECOMP_WEB_BIN_DIR})
elseif (WOWDECOMP_ARCH_64)
    set(WOW_OUTPUT_NAME Wow64)
else ()
    set(WOW_OUTPUT_NAME Wow32)
endif ()
if (NOT WOW_BIN_DIR)
    set(WOW_BIN_DIR ${CMAKE_SOURCE_DIR}/bin/Win)
endif ()

set_target_properties(Wow PROPERTIES
    OUTPUT_NAME ${WOW_OUTPUT_NAME}
    RUNTIME_OUTPUT_DIRECTORY ${WOW_BIN_DIR}
)

foreach (config ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER ${config} config)
    set_target_properties(Wow PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} ${WOW_BIN_DIR})
endforeach ()

target_include_directories(Wow
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

if (WOWDECOMP_PLATFORM_WEB)
    # The game runs on a pthread so its blocking loop, waits and synchronous file reads stay valid. Memory starts small
    # and grows, because phones refuse a large first allocation; pre.js lowers the maximum when the page asks.
    target_link_options(Wow PRIVATE
        -sPROXY_TO_PTHREAD
        -sALLOW_MEMORY_GROWTH
        -sINITIAL_MEMORY=384MB
        -sMAXIMUM_MEMORY=4GB
        -sSTACK_SIZE=8MB
        -sPTHREAD_POOL_SIZE=24
        -sEXIT_RUNTIME=0
        -lwebsocket.js
        -sEXPORTED_FUNCTIONS=_main,_malloc,_free
        -sOFFSCREENCANVAS_SUPPORT
        -sMIN_WEBGL_VERSION=2
        -sMAX_WEBGL_VERSION=2
        "--pre-js=${CMAKE_SOURCE_DIR}/Engine/Source/Os/Web/pre.js"
        -Wl,--error-limit=0
    )
else ()
    target_link_options(Wow PRIVATE "/INCREMENTAL:NO" "/ENTRY:start")
endif ()

target_link_libraries(Wow
    PRIVATE
        BLPFile
        Base
        BlizzardCore
        ComSat
        ConsoleBase
        Event
        FFX
        Frame
        FrameScript
        lua
        FrameXML
        Gx
        Gxu
        Images
        Model2
        Os
        Scrn
        Services
        ShaderEffect
        Tempest
        SoundEngine
        SteelseriesMouse
        Storm
        WoWCommon
        Battlenet
        Console
        ConsoleDevice
        DB
        Game
        Glue
        LiquidSystem
        Magic
        Minigame
        Net
        Object
        ObjectAlloc
        ObjectMgrClient
        SoundInterface
        UIUtil
        Ui
        WorldClient
        WowServices
        WowSvcs
)
