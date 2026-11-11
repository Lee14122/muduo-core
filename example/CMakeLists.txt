# ============================================================================
# 【文件】example/CMakeLists.txt —— 把 testserver.cc（EchoServer，全链路 ①→⑫ 的 main()）连成可执行文件
# 【一句话】GLOB 收集本目录 .cc → 链接 muduo_core 与 ${LIBS} → 产物放回源码旁的 example/testserver。
# 【Reactor 角色】基础设施（构建脚本）。
# 【产物】example/testserver（RUNPATH 已烧录指向 lib/，可直接运行）
# 【先修知识】先读 src/CMakeLists.txt 关于 PUBLIC 传播的注释，否则看不懂这里为什么没有 -I
# 【难度】★☆☆☆☆ 【建议耗时】10 分钟
# 【自检】1) testserver 的编译行里，两个 -std 哪个最终生效？
#         2) ${LIBS} 从哪来？删掉它最先在哪一步炸？
# ============================================================================
# 获取当前目录下的所有源文件
# 【易错】GLOB 的坑与 src/CMakeLists.txt 同款：配置期定死文件清单，新增 .cc 必须重新配置才可见。
#          本文件还多一个坑：EXAMPLE_SRCS 全塞进同一个目标 testserver——一旦新增第二个带 main() 的文件（比如 docs/experiments.md 练习 6 的聊天室 chatserver.cc），
#          链接期直接 multiple definition of 'main'。正确做法是 add_executable(chatserver chatserver.cc) 单独建目标（链接设置照抄本文件），或替换掉 testserver.cc 本身。
file(GLOB EXAMPLE_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)

# 创建可执行文件
add_executable(testserver ${EXAMPLE_SRCS})

# 链接必要的库，比如刚刚我们写好的在 src 文件 CMakeLists 中 muduo-core_lib 静态库，还有全局链接库
# 【设计】传递性在这里分成两半看：muduo_core 是“目标”，它的使用要求（include 路径；若它还 PUBLIC 链过别的库）沿依赖边传染给 testserver；
#          而 ${LIBS} 只是普通变量的文本展开——pthread 并不是 muduo_core 的依赖（src/CMakeLists.txt 里没有任何 target_link_libraries），
#          所以必须在这一行手动补上。换句话说：删掉 ${LIBS}，编译全过、链接才报 pthread_create/sem_init 未定义（glibc<2.34；新 glibc 已并入 libc 侥幸能过）。
# 【易错】上一行原注释说“muduo-core_lib 静态库”不准确（保留原文）：目标名是 muduo_core，类型 SHARED 动态库。
# 【对比原版】原版 muduo 的示例据称每个 demo 各 add_executable 并链 glog 等第三方库（具体以原版源码/CMakeLists 为准）；这里只有一个裸 pthread。
target_link_libraries(testserver muduo_core ${LIBS})

# 设置编译选项
# 【⚠问题】A7 冲突条目的另一半：这里的裸 -std=c++11 会“反杀”顶层 set(CMAKE_CXX_STANDARD 17)。
#          最终编译行：-std=gnu++17（来自全局标准变量）+ 本行追加的 -std=c++11 -Wall，GCC 的 -std 后写覆盖先写 → testserver.cc 这个翻译单元按 C++11 编。
#          症状：你在顶层把标准升到 17/20、或在 example 写 C++17 语法，它报“... only available with C++17”——而 src 里同样的写法好好的，因为两边用的标准不同。
#          修法方向（只提示、不动代码）：从本行删去 -std=c++11 只留 -Wall，标准交给 CMAKE_CXX_STANDARD/target_compile_features 统一管理。
# 【语法】-Wall 不是“打开全部警告”（那是 -Wextra/-Wpedantic 一族），只是编译器建议的常用组；-Wall 不会把警告升级为错误，想要严格是 -Werror（注释期别开，避免噪音）。
target_compile_options(testserver PRIVATE -std=c++11 -Wall)

# 设置 testserver 可执行文件输出目录
# 【设计】RUNTIME_OUTPUT_DIRECTORY 是逐目标属性（set_target_properties），只管 testserver 自己 → 产物直接放源码旁的 example/；
#          它属于“RUNTIME”类（Linux 上可执行文件归此类），与 .so 的 LIBRARY 类（顶层 LIBRARY_OUTPUT_PATH）、.a 的 ARCHIVE 类不互通——A7 的产物路径全景到此集齐。
# 【易错】同名全局变量 CMAKE_RUNTIME_OUTPUT_DIRECTORY 也存在，但逐 target 属性优先级更高；多配置生成器（VS/Xcode）要带配置后缀
#          （RUNTIME_OUTPUT_DIRECTORY_DEBUG 等）才吃这个属性，Unix Makefiles 单配置无此烦恼。
# 【设计】产物放源码旁还能跑：链接时把 muduo_core 的实际输出目录烧进了 RUNPATH（readelf -d example/testserver 可见 /…/muduo-core/lib），
#          这正是顶层 LIBRARY_OUTPUT_PATH 挑大梁的地方。若把 testserver 拷去别处跑，报 error while loading shared libraries: libmuduo_core.so，
#          临时补救 LD_LIBRARY_PATH=/home/lee/projects/muduo-core/lib ./testserver。
set_target_properties(testserver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
