# ============================================================================
# 【文件】CMakeLists.txt（顶层）—— muduo-core 的构建入口，先读它，再读 src/、example/
# 【一句话】定下 C++ 标准/构建类型/产物输出路径/全局 LIBS 变量，再把 src（动态库）与 example（可执行 testserver）两个子目录拉进构建。
# 【Reactor 角色】基础设施（不参与运行期，决定“怎么编出来、编成什么样、产物放哪”）。
# 【如何从零编译本项目】（在仓库根目录 /home/lee/projects/muduo-core 下执行）
#     cmake -S . -B build            # 配置（S=源码目录 B=构建目录；机器上是 CMake 4.x 时须再加 -DCMAKE_POLICY_VERSION_MINIMUM=3.5，原因见下）
#     cmake --build build -j         # 构建（等价于 cd build && make -j）
#     ./example/testserver           # 运行；动态库在 lib/libmuduo_core.so，可执行已烧录 RUNPATH，可直接跑
# 【产物】动态库 lib/libmuduo_core.so（位置由下面 LIBRARY_OUTPUT_PATH 决定）
#         可执行 example/testserver（位置由 example/CMakeLists.txt 的 RUNTIME_OUTPUT_DIRECTORY 决定）
#         build/ 下全是中间文件（.o 与 Makefile），可随时 rm -rf build 重配——这就是 out-of-source（源码外构建）的意义：源码树不被编译器产物污染。
# 【依赖】本文件不依赖任何代码；两个 add_subdirectory 是它全部的“调用图”
# 【谁用它】cmake 命令行工具本身；编辑器/插件也拿它当工程索引
# 【先修知识】编译与链接分离；一条 g++ 命令行里 -I/-std/-l/-Wl,-rpath 各管什么
# 【难度】★★☆☆☆ 【建议耗时】20 分钟
# 【自检】1) set(CMAKE_BUILD_TYPE Debug) 写进缓存了吗？为什么它照样生效？
#         2) 新增 src/Foo.cc 后只跑 cmake --build build -j 为什么编不进去？缺哪一步？
#         3) example 里没人写 -Iinclude，testserver.cc 的 #include "TcpServer.h" 为什么能解析？
# ============================================================================
#设置最低版本和项目名称
# 【语法】cmake_minimum_required(VERSION 3.0)：声明“本脚本按 CMake 3.0 的语义写”，并把所有策略(policy)默认值钉在 3.0；
#          它不是“必须用恰好 3.0 的二进制”，高版本照样解析，低版本直接拒绝配置。
# 【⚠问题】CMake 4.x 起不再兼容声明低于 3.5 的工程：在本机（CMake 4.2）配置会报 Compatibility with CMake < 3.5 has been removed，
#          两个解法——要么把 VERSION 改成 3.5（本教学禁止改代码，别动），要么配置时追加 -DCMAKE_POLICY_VERSION_MINIMUM=3.5；
#          build/CMakeCache.txt 里留有 CMAKE_POLICY_VERSION_MINIMUM:UNINITIALIZED=3.5，说明上次成功配置走的就是后一条路。
cmake_minimum_required(VERSION 3.0)
project(muduo-core) #锁定项目的工作目录
# 【语法】project(muduo-core) 干三件事：1) 结束“探测编译器前”的阶段，默认使能 C/CXX 语言；
#          2) 生成变量 PROJECT_NAME / PROJECT_SOURCE_DIR(=仓库根) / PROJECT_BINARY_DIR(=build 目录)——下面 LIBRARY_OUTPUT_PATH 用的 ${PROJECT_SOURCE_DIR} 就来自这里；
#          3) 项目名是 muduo-core（连字符），记住库目标名是 muduo_core（下划线，src/CMakeLists.txt 的 add_library），别混。
# 【易错】原注释“锁定项目的工作目录”不够准确：它锁的不是 cwd，而是登记源码根与项目元信息；命令行 cmake -S/-B 传的路径才是真正的工作目录来源。

#设置全局的c++标准
# 【设计】CMAKE_BUILD_TYPE 决定往编译命令里追加哪组配置专用 flags：Debug 默认只加 -g（见 build/CMakeCache.txt：CMAKE_CXX_FLAGS_DEBUG:STRING=-g），
#          不含 -O2/-O3（那是 Release 的 -O3 -DNDEBUG）；GCC 没给 -O 时等效 -O0。教学效果：gdb 里每个局部变量都能看、单步与源码逐行对应。
# 【易错】这里的 set 没带 CACHE：它只是目录作用域的普通变量，不写进缓存——所以 build/CMakeCache.txt 里 CMAKE_BUILD_TYPE:STRING= 是空的。
#          CMake 读同名变量时普通变量优先于缓存，因此 Debug 照样生效；但若有人命令行传 -DCMAKE_BUILD_TYPE=Release，会被本行覆盖回 Debug，
#          “我显式传的 Release 去哪了”这种灵异问题的根就在这。规范写法是 set(... CACHE STRING "" ) 或干脆不在脚本里写死（原版如何设置构建类型，以其 CMakeLists 为准）。
# 【⚠问题】仓库里 checked-in 的 build/ 是在本行被加进来之前的旧配置：build/*/flags.make 里连 -g 都没有。
#          用 gdb 断点却“看不见变量/找不到行号”时先想到：产物是旧的，不是断点打错了。改完 CMakeLists 必须重新配置构建。
set (CMAKE_BUILD_TYPE Debug)
# 【语法】CMAKE_CXX_STANDARD 17 是“声明式”标准：CMake 按编译器能力翻译成具体开关；GCC 下得到 -std=gnu++17。
#          注意 gnu 前缀：CMAKE_CXX_EXTENSIONS 默认 ON，GNU 扩展可用——src/EventLoopThreadPool.cc 的 start 里 char buf[name_.size() + 32]（变长数组 VLA）
#          就是靠它才编得过的；set(CMAKE_CXX_EXTENSIONS OFF) 换成严格 -std=c++17 后，那行 VLA 会直接编译失败。
# 【语法】CMAKE_CXX_STANDARD_REQUIRED True：编译器不支持 17 时配置期报错，而不是悄悄退回 11。
# 【⚠问题】A7 冲突条目（全局这一侧）：example/CMakeLists.txt 的 target_compile_options(testserver PRIVATE -std=c++11) 与本行打架。
#          CMake 生成的编译行把标准变量翻译出的 -std=gnu++17 放前、目标自带的裸选项放后：... -std=gnu++17 -std=c++11 -Wall，
#          GCC 遇到多个 -std 取最后一个 → testserver.cc 实际按 C++11 编，src 的 17 个翻译单元按 gnu++17 编。
#          旧产物已留证据：build/example/CMakeFiles/testserver.dir/flags.make 的 CXX_FLAGS 里两个 -std 并排（当时全局还是 11，写作 -std=gnu++11 -std=c++11）。
#          今天恰好两边都只用 C++11 语法所以不炸；一旦在 example 用结构化绑定/std::string_view，报错“只有 C++17 才支持”，病灶在本行+那行 -std=c++11 的相互作用。
set (CMAKE_CXX_STANDARD 17) 
set (CMAKE_CXX_STANDARD_REQUIRED True)

#链接必要的库，比如刚刚我们写好的在src文件Cmakelists中muduo-core_lib静态库，还有全局链接库
# 【易错】对上一行原注释的两处更正（原注释保留不改）：并不叫 muduo-core_lib，也不存在静态库——
#          src/CMakeLists.txt 的目标名是 muduo_core，用 add_library(muduo_core SHARED ...) 创建，是动态库；这里说的“全局链接库”指下面 set(LIBS ...)，
#          而真正的链接动作在 example/CMakeLists.txt 的 target_link_libraries 里，顶层自己不链接任何东西。
# 【设计】LIBRARY_OUTPUT_PATH：老式全局变量，统一设定所有 SHARED/MODULE 目标的输出目录 → lib/libmuduo_core.so 出自这里；不写则默认落在 build/src/。
#          新式等价物是 CMAKE_LIBRARY_OUTPUT_DIRECTORY（目录作用域）或逐目标的 LIBRARY_OUTPUT_DIRECTORY 属性。
# 【易错】A7 产物路径变量三件套，Linux 下按“产物种类”对号入座，极易混：
#          .so 动态库 = LIBRARY（本行）；.a 静态库 = ARCHIVE（下一行被注释掉的 CMAKE_ARCHIVE_OUTPUT_DIRECTORY 就是它）；
#          可执行文件 = RUNTIME（example/CMakeLists.txt 用的 target 属性 RUNTIME_OUTPUT_DIRECTORY）。写错类别 = 设了等于没设。
# 【设计】为什么把 .so 放进源码树的 lib/：链接可执行时 CMake 把 RUNPATH=/home/lee/projects/muduo-core/lib 烧进 testserver（证据：readelf -d example/testserver 的 RUNPATH 条目），
#          于是 ./example/testserver 直接可跑，不需要 export LD_LIBRARY_PATH。代价：产物落在源码树里被 git 盯上（git status 里 lib/libmuduo_core.so、example/testserver 的改动就是它），
#          原版的产物据称留在构建目录、不污染工作区（以原版 CMakeLists 为准）。
set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)
#示例静态库的输出
#set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/lib) # 设置静态库输出路径
#设置全局链接库
# 【设计】LIBS 只是个普通变量，内容是字符串列表（这里只有 pthread）。example/CMakeLists.txt 里 target_link_libraries(testserver muduo_core ${LIBS})
#          配置期纯文本展开成 pthread → 链接命令末尾多一个 -lpthread（证据：build/example/CMakeFiles/testserver.dir/link.txt）。
# 【系统调用】pthread 为什么必须显式链接：src/Thread.cc 的 Thread::start 用 std::thread 并调 sem_init/sem_wait，src/EventLoop.cc 的 mutex_ 与 std::cout
#          底层最终都是 pthread_mutex_lock 等符号。glibc < 2.34 时它们在独立的 libpthread.so.0 里，可执行文件链接阶段不加 -lpthread 就报
#          undefined reference to 'pthread_create'。本机 glibc 2.43 已把 libpthread 并入 libc.so.6（证据：nm -D lib/libmuduo_core.so 的未定义符号带
#          sem_*@GLIBC_2.34、pthread_mutex_lock@GLIBC_2.2.5 版本标记），此时 -lpthread 近乎空转——保留它是为了兼容老 glibc，别按“本机不写也能过”下结论。
# 【对比原版】规范做法是 find_package(Threads REQUIRED) 再链接 Threads::Threads 目标：自动带 -pthread 编译宏（_REENTRANT）、跨平台；原版具体是否用 Threads 包，以原版 CMakeLists 为准。
#          裸变量写法没有传递性：谁想用 pthread 谁自己引用 ${LIBS}，不引用就没有。
set(LIBS
    pthread
)

#添加子目录
# 【设计】add_subdirectory(src)：就地解析 src/CMakeLists.txt，生成目标 muduo_core；下一行同理生成 testserver。
# 【易错】A7 顺序依赖条目的正确理解：目标名（muduo_core/testserver）要到生成期才解析，所以“example 在 src 之前”也能引用后定义的目标，前向引用合法；
#          真正有先后硬依赖的是普通变量——子目录继承的是“执行到 add_subdirectory 那一刻”的父作用域变量快照：
#          example 用的 ${LIBS} 依赖本文件先 set(LIBS)。因此：交换下面两行 add_subdirectory 不会报错；
#          把 add_subdirectory(example) 挪到 set(LIBS) 之前才会炸——${LIBS} 展开为空，老 glibc 上 pthread_create 链接失败。
#          本文件保持“先全部 set，再 src，最后 example”的规范顺序。
# 【对比原版】原版据称用 option()/if() 开关控制是否构建 example 等子目录（变量名以原版 CMakeLists 为准），这里是无条件的教学简化。
add_subdirectory(src)
add_subdirectory(example)