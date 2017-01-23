include (CheckCCompilerFlag)
include (CheckFunctionExists)
include (CheckIncludeFile)
include (CheckIncludeFiles)
include (CheckLibraryExists)
include (CheckStructHasMember)
include (CheckSymbolExists)
include (CheckTypeSize)
include (CMakeDependentOption)
include (TestBigEndian)

set(CMAKE_REQUIRED_INCLUDES math.h)
set(CMAKE_REQUIRED_LIBRARIES m)

if(NOT CMAKE_INSTALL_DATADIR)
	set(CMAKE_INSTALL_DATADIR "share" CACHE PATH "read-only architecture-independent data (DATAROOTDIR)/root")
endif(NOT CMAKE_INSTALL_DATADIR)

set(CONFIG_TESTS_DIR config-tests)
set(AVCONV_DATADIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/ffmpeg")
set(FFMPEG_DATADIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/ffmpeg")
set(FFMPEG_CONFIGURATION "zm-avengine")
set(CC_IDENT ${CMAKE_C_COMPILER_ID} ${CMAKE_SYSTEM})
set(CONFIG_THIS_YEAR 2017) #TODO

include_directories("${CMAKE_BINARY_DIR}")

if ("${CMAKE_SYSTEM_PROCESSOR}" MATCHES "^(AMD64|amd64|x86_64)$")
	set(ARCH_X86_64 1)
	set(HAVE_I686 1)
elseif("${CMAKE_SYSTEM_PROCESSOR}" MATCHES "^(i386|i686|x86)$")
	set(ARCH_X86_32 1)
	set(HAVE_I686 1)
elseif("${CMAKE_SYSTEM_PROCESSOR}" MATCHES "^(arm|armv6|armv7-a)$")
	set(ARCH_ARM 1)
elseif("${CMAKE_SYSTEM_PROCESSOR}" MATCHES "^(aarch64)$")
	set(ARCH_AARCH64 1)
endif()

check_c_compiler_flag(-mavx HAVE_AVX)

check_function_exists(atanf    HAVE_ATANF)
check_function_exists(atan2f   HAVE_ATAN2F)
check_function_exists(cbrt     HAVE_CBRT)
check_function_exists(cbrtf    HAVE_CBRTF)
check_function_exists(copysign HAVE_COPYSIGN)
check_function_exists(erf      HAVE_ERF)
check_function_exists(exp2     HAVE_EXP2)
check_function_exists(exp2f    HAVE_EXP2F)
check_function_exists(expf     HAVE_EXPF)
check_function_exists(hypot    HAVE_HYPOT)
check_function_exists(isfinite HAVE_ISFINITE)
check_function_exists(isinf    HAVE_ISINF)
check_function_exists(isnan    HAVE_ISNAN)
check_function_exists(ldexpf   HAVE_LDEXPF)
check_function_exists(llrint   HAVE_LLRINT)
check_function_exists(llrintf  HAVE_LLRINTF)
check_function_exists(log2     HAVE_LOG2)
check_function_exists(log2f    HAVE_LOG2F)
check_function_exists(log10f   HAVE_LOG10F)
check_function_exists(lrint    HAVE_LRINT)
check_function_exists(lrintf   HAVE_LRINTF)
check_function_exists(powf     HAVE_POWF)
check_function_exists(rint     HAVE_RINT)
check_function_exists(round    HAVE_ROUND)
check_function_exists(roundf   HAVE_ROUNDF)
check_function_exists(sinf     HAVE_SINF)
check_function_exists(trunc    HAVE_TRUNC)
check_function_exists(truncf   HAVE_TRUNCF)

if (NOT HAVE_ISFINITE)
	MESSAGE("-- Looking for isfinite")
	TRY_COMPILE(HAVE_ISFINITE
		${CMAKE_CURRENT_BINARY_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/${CONFIG_TESTS_DIR}/isfinite.c
		OUTPUT_VARIABLE SLT)
	IF(HAVE_ISFINITE)
		MESSAGE("-- Looking for isfinite - found")
	ELSE(HAVE_ISFINITE)
		MESSAGE("-- Looking for isfinite - not found:\n${SLT}")
	ENDIF(HAVE_ISFINITE)
endif (NOT HAVE_ISFINITE)

CHECK_SYMBOL_EXISTS(closesocket winsock2.h HAVE_CLOSESOCKET)
CHECK_SYMBOL_EXISTS(CommandLineToArgvW Shellapi.h HAVE_COMMANDLINETOARGVW)


check_include_files(arpa/inet.h HAVE_ARPA_INET_H)
check_include_file(byteswap.h HAVE_BYTESWAP_H)
check_include_file(direct.h HAVE_DIRECT_H)
#TODO HAVE_DOS_PATHS
#TODO HAVE_DXVA2_LIB
#TODO HAVE_EBP_AVAILABLE
#TODO HAVE_EBX_AVAILABLE
check_function_exists(getaddrinfo HAVE_GETADDRINFO)
check_function_exists(gethrtime HAVE_GETHRTIME)
#TODO HAVE_GETPROCESSMEMORYINFO
#TODO HAVE_GETPROCESSTIMES
check_symbol_exists(getrusage sys/resource.h HAVE_GETRUSAGE)
check_function_exists(gmtime_r HAVE_GMTIME_R)
#TODO HAVE_INLINE_ASM
#TODO HAVE_INLINE_ASM_DIRECT_SYMBOL_REFS
check_include_file(io.h HAVE_IO_H)
check_function_exists(isatty HAVE_ISATTY)

check_include_file(inttypes.h HAVE_INTTYPES_H)
#TODO HAVE_KBHIT
#TODO HAVE_LIBC_MSVCRT
#TODO HAVE_LOCAL_ALIGNED_8
#TODO HAVE_LOCAL_ALIGNED_16
#TODO HAVE_LOCAL_ALIGNED_32
check_function_exists (localtime_r HAVE_LOCALTIME_R)
#TODO HAVE_MACH_ABSOLUTE_TIME
#TODO HAVE_MACH_MACH_TIME_H
#TODO HAVE_MIPSFPU
#TODO HAVE_MM_EMPTY
check_c_compiler_flag(-mmmx HAVE_MMX)
#TODO HAVE_MMX_EXTERNAL
#TODO HAVE_MMX_INLINE
#TODO HAVE_PEEKNAMEDPIPE
check_include_file(poll.h HAVE_POLL_H)
#TODO HAVE_PRAGMA_DEPRECATED
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	set(HAVE_PRAGMA_DEPRECATED 1)
endif()
#check_include_file(pthread.h HAVE_PTHREADS)
#TODO HAVE_RDTSC
#TODO HAVE_SETCONSOLECTRLHANDLER
#TODO HAVE_SETDLLDIRECTORY
check_function_exists(setrlimit HAVE_SETRLIMIT)

#CHECK_SYMBOL_EXISTS(socklen_t "unistd.h;sys/types.h;sys/socket.h" HAVE_SOCKLEN_T)
check_type_size(socklen_t HAVE_SOCKLEN_T)
# https://cmake.org/pipermail/cmake/2007-February/013009.html
if (NOT HAVE_SOCKLEN_T)
	MESSAGE("-- Looking for socklen_t")
	TRY_COMPILE(HAVE_SOCKLEN_T
		${CMAKE_CURRENT_BINARY_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/${CONFIG_TESTS_DIR}/socklen_t.c
		OUTPUT_VARIABLE SLT)
	IF(HAVE_SOCKLEN_T)
		MESSAGE("-- Looking for socklen_t - found")
	ELSE(HAVE_SOCKLEN_T)
		MESSAGE("-- Looking for socklen_t - not found:\n${SLT}")
	ENDIF(HAVE_SOCKLEN_T)
endif (NOT HAVE_SOCKLEN_T)

# check for existing datatypes
set(CMAKE_EXTRA_INCLUDE_FILES "sys/socket.h;netdb.h")
check_type_size("struct addrinfo" HAVE_STRUCT_ADDRINFO)
check_type_size("struct sockaddr_in6" HAVE_STRUCT_SOCKADDR_IN6)
check_type_size("struct sockaddr_storage" HAVE_STRUCT_SOCKADDR_STORAGE)
set(CMAKE_EXTRA_INCLUDE_FILES)  #reset CMAKE_EXTRA_INCLUDE_FILES

#TODO HAVE_STRUCT_POLLFD
check_symbol_exists("struct pollfd" sys/poll.h HAVE_STRUCT_POLLFD)
#TODO HAVE_STRUCT_RUSAGE_RU_MAXRSS
check_struct_has_member("struct sockaddr" sa_len  "sys/types.h;sys/socket.h" HAVE_STRUCT_SOCKADDR_SA_LEN)

#TODO HAVE_SYMVER_ASM_LABEL
#TODO HAVE_SYMVER_GNU_ASM
check_include_file(sys/resource.h HAVE_SYS_RESOURCE_H)
check_include_file(sys/select.h HAVE_SYS_SELECT_H)
check_include_files(termios.h HAVE_TERMIOS_H)

option(FEATURE_THREADS "Enable threads support" ON)
if(FEATURE_THREADS)
    set(CMAKE_THREAD_PREFER_PTHREAD 1)
    find_package(Threads)
    if(Threads_FOUND)
        set(HAVE_PTHREAD 1)
		set(HAVE_PTHREADS 1)
        list(APPEND NO_LIBRARIES ${CMAKE_THREAD_LIBS_INIT})
    endif(Threads_FOUND)
endif(FEATURE_THREADS)
cmake_dependent_option(HAVE_THREADS "Enable thread support" ON "FEATURE_THREADS;Threads_FOUND" OFF)
mark_as_advanced(HAVE_THREADS)

#TODO HAVE_VDPAU_X11
#TODO HAVE_WINSOCK2_H
#TODO HAVE_XMM_CLOBBERS



check_c_compiler_flag(-msse HAVE_SSE)
check_c_compiler_flag(-msse2 HAVE_SSE2)
check_c_compiler_flag(-msse3 HAVE_SSE3)
check_c_compiler_flag(-msse4.1 HAVE_SSE4_1)
check_c_compiler_flag(-msse4.2 HAVE_SSE4_2)
check_c_compiler_flag(-mssse3 HAVE_SSSE3)

check_include_file(stdint.h HAVE_STDINT_H)
check_include_files(unistd.h HAVE_UNISTD_H)


test_big_endian(HAVE_BIGENDIAN)


set (CONFIG_AVCODEC on)
set (CONFIG_AVDEVICE on)
set (CONFIG_AVFILTER on)
set (CONFIG_AVFORMAT on)
set (CONFIG_AVRESAMPLE off)
set (CONFIG_AVUTIL on)

set (CONFIG_NETWORK on)
set (CONFIG_SHARED off)
set (CONFIG_SMALL off)
set (CONFIG_SWRESAMPLE on)
set (CONFIG_SWSCALE on)
set (CONFIG_POSTPROC off)

set (CONFIG_VDA off)
set (CONFIG_VIDEOTOOLBOX off)


