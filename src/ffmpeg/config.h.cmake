/* gathered then modified with grep -hor . --exclude="config.h" -E -e "[a-zA-Z0-9_#]*(ARCH_|CONFIG_|HAVE_|VERSION_)[a-zA-Z0-9_#]*" | sort | uniq | sed 's/.*/#cmakedefine & 1/' */

#cmakedefine ARCH_AARCH64 1
#cmakedefine ARCH_ARM 1
#cmakedefine ARCH_AVR32 1
#cmakedefine ARCH_MIPS 1
#cmakedefine ARCH_PPC 1
#cmakedefine ARCH_X86 1
#cmakedefine ARCH_X86_32 1
#cmakedefine ARCH_X86_64 1
#cmakedefine AVCONV_DATADIR "@AVCONV_DATADIR@"
#cmakedefine CC_IDENT "@CC_IDENT@"
#cmakedefine01 CONFIG_AVCODEC
#cmakedefine01 CONFIG_AVDEVICE
#cmakedefine01 CONFIG_AVFILTER
#cmakedefine01 CONFIG_AVFORMAT
#cmakedefine01 CONFIG_AVRESAMPLE
#cmakedefine01 CONFIG_AVUTIL
#cmakedefine CONFIG_GPL 1
#cmakedefine CONFIG_GPLV3 1
#cmakedefine CONFIG_LGPLV3 1
#cmakedefine CONFIG_NETWORK 1
#cmakedefine CONFIG_NONFREE 1
#cmakedefine CONFIG_OPENCL 1
#cmakedefine01 CONFIG_POSTPROC
#cmakedefine CONFIG_SHARED 1
#cmakedefine CONFIG_SMALL 0
#cmakedefine01 CONFIG_SWRESAMPLE
#cmakedefine01 CONFIG_SWSCALE
#cmakedefine CONFIG_THIS_YEAR @CONFIG_THIS_YEAR@
#cmakedefine CONFIG_VDA 1
#cmakedefine CONFIG_VIDEOTOOLBOX 1
#cmakedefine FFMPEG_CONFIGURATION "@FFMPEG_CONFIGURATION@"
#cmakedefine FFMPEG_DATADIR "@FFMPEG_DATADIR@"
#cmakedefine HAVE_ARPA_INET_H 1
#cmakedefine HAVE_ATAN2F 1
#cmakedefine HAVE_ATANF 1
#cmakedefine HAVE_AV_CONFIG_H 1
#cmakedefine HAVE_BIGENDIAN 1
#cmakedefine HAVE_CBRT 1
#cmakedefine HAVE_CBRTF 1
#cmakedefine HAVE_CLOSESOCKET 1
#cmakedefine HAVE_COMMANDLINETOARGVW 1
#cmakedefine HAVE_COPYSIGN 1
#cmakedefine HAVE_COSF 1
#cmakedefine HAVE_DIRECT_H 1
#cmakedefine HAVE_DOS_PATHS 1
#cmakedefine HAVE_DXVA2_LIB 1
#cmakedefine HAVE_EBP_AVAILABLE 1
#cmakedefine HAVE_EBX_AVAILABLE 1
#cmakedefine HAVE_ERF 1
#cmakedefine HAVE_EXP2 1
#cmakedefine HAVE_EXP2F 1
#cmakedefine HAVE_EXPF 1
#cmakedefine HAVE_GETADDRINFO 1
#cmakedefine HAVE_GETHRTIME 1
#cmakedefine HAVE_GETPROCESSMEMORYINFO 1
#cmakedefine HAVE_GETPROCESSTIMES 1
#cmakedefine HAVE_GETRUSAGE 1
#cmakedefine HAVE_GMTIME_R 1
#cmakedefine HAVE_HYPOT 1
#cmakedefine HAVE_I686 1
#cmakedefine HAVE_INLINE_ASM 1
#cmakedefine HAVE_INLINE_ASM_DIRECT_SYMBOL_REFS 1
#cmakedefine HAVE_IO_H 1
#cmakedefine HAVE_ISATTY 1
#cmakedefine HAVE_ISFINITE 1
#cmakedefine HAVE_ISINF 1
#cmakedefine HAVE_ISNAN 1
#cmakedefine HAVE_KBHIT 1
#cmakedefine HAVE_LDEXPF 1
#cmakedefine HAVE_LIBC_MSVCRT 1
#cmakedefine HAVE_LLRINT 1
#cmakedefine HAVE_LLRINTF 1
#cmakedefine HAVE_LOCAL_ALIGNED_16 1
#cmakedefine HAVE_LOCAL_ALIGNED_32 1
#cmakedefine HAVE_LOCAL_ALIGNED_8 1
#cmakedefine HAVE_LOCALTIME_R 1
#cmakedefine HAVE_LOG10F 1
#cmakedefine HAVE_LOG2 1
#cmakedefine HAVE_LOG2F 1
#cmakedefine HAVE_LRINT 1
#cmakedefine HAVE_LRINTF 1
#cmakedefine HAVE_MACH_ABSOLUTE_TIME 1
#cmakedefine HAVE_MACH_MACH_TIME_H 1
#cmakedefine HAVE_MIPSFPU 1
#cmakedefine HAVE_MM_EMPTY 1
#cmakedefine HAVE_MMX 1
#cmakedefine HAVE_MMX_EXTERNAL 1
#cmakedefine HAVE_MMX_INLINE 1
#cmakedefine HAVE_PEEKNAMEDPIPE 1
#cmakedefine HAVE_POLL_H 1
#cmakedefine HAVE_POWF 1
#cmakedefine HAVE_PRAGMA_DEPRECATED 1
#cmakedefine HAVE_PTHREADS 1
#cmakedefine HAVE_RDTSC 1
#cmakedefine HAVE_RINT 1
#cmakedefine HAVE_ROUND 1
#cmakedefine HAVE_ROUNDF 1
#cmakedefine HAVE_SETCONSOLECTRLHANDLER 1
#cmakedefine HAVE_SETDLLDIRECTORY 1
#cmakedefine HAVE_SETRLIMIT 1
#cmakedefine HAVE_SINF 1
#cmakedefine HAVE_SOCKLEN_T 1
#cmakedefine HAVE_STRUCT_ADDRINFO 1
#cmakedefine HAVE_STRUCT_POLLFD 1
#cmakedefine HAVE_STRUCT_RUSAGE_RU_MAXRSS 1
#cmakedefine HAVE_STRUCT_SOCKADDR_IN6 1
#cmakedefine HAVE_STRUCT_SOCKADDR_SA_LEN 1
#cmakedefine HAVE_STRUCT_SOCKADDR_STORAGE 1
#cmakedefine HAVE_SYMVER_ASM_LABEL 1
#cmakedefine HAVE_SYMVER_GNU_ASM 1
#cmakedefine HAVE_SYS_RESOURCE_H 1
#cmakedefine HAVE_SYS_SELECT_H 1
#cmakedefine HAVE_TERMIOS_H 1
#cmakedefine HAVE_THREADS 1
#cmakedefine HAVE_TRUNC 1
#cmakedefine HAVE_TRUNCF 1
#cmakedefine HAVE_UNISTD_H 1
#cmakedefine HAVE_VDPAU_X11 1
#cmakedefine HAVE_WINSOCK2_H 1
#cmakedefine HAVE_XMM_CLOBBERS 1

