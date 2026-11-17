# FireWinPE —— PE 端原生部署程序
# 许可证：GPL-3.0-only
#
# 构建要求：Visual Studio 2022 + CMake 3.20 以上（或 VS 自带的 CMake）
# 生成方式：
#   cmake -S pe -B pe\build -G "Visual Studio 17 2022" -A x64
#   cmake --build pe\build --config Release
# 产物：pe\build\bin\FireWinPE.exe
#
# 关键约束：
#   1. 必须静态链接 CRT（/MT），否则裸 WinPE 里缺少 VCRUNTIME140.dll 无法启动；
#   2. 不链接 wimlib / 7z 的导入库，二者一律以子进程方式调用；
#   3. 单一 exe，无外部依赖。

cmake_minimum_required(VERSION 3.20)

project(FireWinPE LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "PE 端必须使用 MSVC 编译（Win32 API + 静态 CRT）。")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "PE 端只支持 x64，请使用 -A x64 生成。")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 静态 CRT：MultiThreaded = /MT，MultiThreadedDebug = /MTd
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(FireWinPE main.cpp res/app.rc res/icon.rc)

target_compile_definitions(FireWinPE PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_compile_options(FireWinPE PRIVATE
    /W4
    /permissive-
    /utf-8
    /EHsc
)

# comdlg32：「安装系统」的文件选择对话框（GetOpenFileNameW）
# msimg32：背景图 / 图标的 AlphaBlend 半透明绘制
# shell32：桌面模式用 ExtractIconExW 提取工具图标
target_link_libraries(FireWinPE PRIVATE comdlg32 msimg32 shell32)

# 控制台子系统：startnet.cmd 中直接运行可复用 conhost 输出，双击运行也会弹出控制台
target_link_options(FireWinPE PRIVATE /SUBSYSTEM:CONSOLE)

set_target_properties(FireWinPE PROPERTIES
    OUTPUT_NAME "FireWinPE"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    # 多配置生成器（VS）下把 Release/ 也放在 bin 里，脚本默认路径才好找
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)

# ---------------------------------------------------------------------------
# FireBoot —— 独立引导工具（写入 PE 引导 / 修复系统引导 / 查看 BCD）
# GUI 子系统：双击直接开窗，输出走窗口内日志框；同样静态 CRT。
# ---------------------------------------------------------------------------
add_executable(FireBoot boottool.cpp res/icon.rc)

target_compile_definitions(FireBoot PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_compile_options(FireBoot PRIVATE
    /W4
    /permissive-
    /utf-8
    /EHsc
)

target_link_options(FireBoot PRIVATE /SUBSYSTEM:WINDOWS)

set_target_properties(FireBoot PROPERTIES
    OUTPUT_NAME "FireBoot"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)

# 构建后自检：确认没有动态 CRT 依赖（microsoft 官方 PE 无 VCRUNTIME140.dll）
add_custom_command(TARGET FireWinPE POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "输出：$<TARGET_FILE:FireWinPE>"
    COMMAND ${CMAKE_COMMAND} -E echo "请用 dumpbin /dependents 确认只依赖 KERNEL32/USER32 等系统 DLL"
)
