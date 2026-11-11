# ============================================================================
# 【文件】src/CMakeLists.txt —— 把本目录所有 .cc 编成一个动态库
# 【一句话】file(GLOB) 收集源文件 → add_library(SHARED) 产出 libmuduo_core.so → PUBLIC 把 include/ 路径传播给使用方。
# 【Reactor 角色】基础设施（构建脚本）。
# 【产物】lib/libmuduo_core.so（输出位置由顶层 LIBRARY_OUTPUT_PATH 决定；没有那行则落在 build/src/）
# 【先修知识】动态库与静态库的区别；“未定义符号”的含义（见 docs/prerequisites.md 第 5 节智能指针之外，这里补链接视角）
# 【难度】★☆☆☆☆ 【建议耗时】10 分钟
# 【自检】1) 这里一行 target_link_libraries 都没写，为什么整个工程还能链接成功？
#         2) 新增 src/Foo.cc 后要做哪一步才会进构建？
# ============================================================================
#获取当前目录下的所有源文件
# 【语法】file(GLOB ...) 只在【配置期】扫一次目录，把结果固化进 build/ 下的 Makefile，构建期不再重扫。
# 【易错】A7 GLOB 条目：新增 src/Foo.cc 后只跑 cmake --build build -j 不会把它编进去（GLOB 没重跑），必须重新配置：cmake -S . -B build（或 cd build && cmake ..）；
#          删除/改名同理——旧 Makefile 还引用不存在的文件，报 No rule to make target '.../Foo.cc'。
#          做 docs/experiments.md 练习 3（新增 TimerQueue.cc）时，你踩到的第一个坑就是它。
# 【对比原版】原版 muduo 据称显式列出每个 .cc（以原版 CMakeLists 为准），CMake 官方文档也不推荐 GLOB；折中方案 file(GLOB ... CONFIGURE_DEPENDS)（CMake>=3.12）
#          让每次构建自动重扫目录、GLOB “活”起来，但本文件声明的最低版本只有 3.0，没开它，请保持“改完源文件就重配”的习惯。
file(GLOB SRC_FILES ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)

#创建静态库或共享库
# 【设计】SHARED → 产出 libmuduo_core.so。链接命令（build/src/CMakeFiles/muduo_core.dir/link.txt）里有 -fPIC -shared -Wl,-soname,libmuduo_core.so：
#          -fPIC 是位置无关代码（.so 可被加载到任意地址），-soname 是运行期自我声明的名字（可执行文件里记录的 NEEDED 就是它）。
# 【系统调用】动态库的关键性质：链接期允许留未定义符号。本库对 pthread_create/sem_init/pthread_mutex_lock 的引用在 .so 里全是 nm -D 可见的 U 符号，
#          合法地“欠着”，等最终可执行文件链接（example/CMakeLists.txt 的 ${LIBS}）或运行期由 ld.so 凑齐——这就是本文件不写 target_link_libraries 也没炸的原因。
#          .a 静态库只是 .o 的归档，符号欠到最终链接才清算，行为不同，别混着试。
# 【易错】目标名 muduo_core：实际文件名是 CMake 自动加的 lib 前缀 + muduo_core + .so 后缀；使用方 target_link_libraries 写目标名即可，不用手写 -L/-l，
#          也不用拼 libmuduo_core.so 路径——这是“目标基”写法相对“裸字符串传编译器参数”的最大红利。
add_library(muduo_core SHARED ${SRC_FILES})

#设置头文件的路径
# 【设计】PUBLIC 让这个 include 路径身兼两职：本目标编译自己的 .cc 要用它 + 凡链接 muduo_core 的目标自动继承它。
#          这就是“example 里没人写 -I 而 testserver.cc 的 #include "TcpServer.h" 能解析”的全部答案：
#          build/example/CMakeFiles/testserver.dir/flags.make 里 CXX_INCLUDES = -I<仓库>/include 是从 muduo_core 的 PUBLIC 传播来的。
# 【易错】A7 PUBLIC 条目：关键字三档选错各有症状——PRIVATE 只给自己用（写成它，example 立刻找不到头文件）；INTERFACE 只传播给使用方（适合 header-only 库，
#          本目标自己反而丢 include）；PUBLIC 两者都要。路径写法用的是 ${CMAKE_SOURCE_DIR}/include（顶层源码根）而非 ${CMAKE_CURRENT_SOURCE_DIR}/../include：
#          二者结果相同，但前者隐含“src 必须直接挂在顶层下”的假设，目录挪深或本目录被别的项目复用时就错。
# 【对比原版】有安装规则的工程还会配 INSTALL_INTERFACE（原版情况以原版 CMakeLists 为准）；本工程没有 install 规则，不涉及。
target_include_directories(muduo_core PUBLIC ${CMAKE_SOURCE_DIR}/include)