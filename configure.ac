AC_INIT([Limine], [m4_esyscmd([./version.sh])], [https://codeberg.org/Limine/Limine/issues], [limine], [https://limine-bootloader.org/])

AC_PREREQ([2.69])

AC_CONFIG_AUX_DIR([build-aux])

SRCDIR="$(cd "$srcdir" && pwd -P)"
BUILDDIR="$(pwd -P)"

AC_SUBST([SRCDIR])
AC_SUBST([BUILDDIR])

. "$SRCDIR"/timestamps

AC_SUBST([REGEN_DATE])
AC_SUBST([SOURCE_DATE_EPOCH])
AC_SUBST([SOURCE_DATE_EPOCH_TOUCH])

AC_CANONICAL_HOST

# Portably convert relative paths into absolute paths.
rel2abs() {
    rel2abs_first=true
    for i in $1; do
        if test $rel2abs_first = true; then
            case "$i" in
                /*)
                    printf "%s" "$i"
                    ;;
                */*)
                    if test -d "$(dirname "$i")"; then
                        printf "%s" "$(cd "$(dirname "$i")" && pwd -P)/$(basename "$i")"
                    else
                        printf "false"
                    fi
                    ;;
                *)
                    printf "%s" "$i"
                    ;;
            esac
            rel2abs_first=false
        else
            printf " %s" "$i"
        fi
    done
    printf "\n"
}

test "x${CFLAGS+set}" = "x" && CFLAGS='-g -O2 -pipe'

AC_LANG([C])
AC_PROG_CC
CC="$(rel2abs "$CC")"

werror_state="no"
AC_ARG_ENABLE([werror],
    [AS_HELP_STRING([--enable-werror], [treat warnings as errors])],
    [werror_state="$enableval"])
if test "$werror_state" = "yes"; then
    AC_SUBST([WERROR_FLAG], [-Werror])
else
    AC_SUBST([WERROR_FLAG], [-Wno-error])
fi

AC_PROG_MKDIR_P
MKDIR_P="$(rel2abs "$MKDIR_P")"
AC_PROG_INSTALL
INSTALL="$(rel2abs "$INSTALL")"
AC_PROG_SED
SED="$(rel2abs "$SED")"
AC_PROG_GREP
GREP="$(rel2abs "$GREP")"
AC_PROG_AWK
AWK="$(rel2abs "$AWK")"

AC_CHECK_PROG([FIND_FOUND], [find], [yes])
if ! test "x$FIND_FOUND" = "xyes"; then
    AC_MSG_ERROR([find not found, please install find before configuring])
fi

# $1 - UPPERCASEVAR, $2 - default command, $3 - 'tool' if toolchain, $4 - 'no-err' to ignore errors
AC_DEFUN([GET_PROG], [
    if test "x${$1+set}" = "xset"; then
        first_elem="$(rel2abs $(echo "$$1" | cut -f 1 -d " "))"
        case "$first_elem" in
            /*)
                AC_MSG_CHECKING([for $first_elem])
                if test -f "$first_elem" && test -x "$first_elem"; then
                    $1="$(rel2abs "$$1")"
                    $1_FOUND=yes
                else
                    $1_FOUND=no
                fi
                AC_MSG_RESULT([$$1_FOUND])
                ;;
            *)
                AC_CHECK_PROG([$1_FOUND], [$first_elem], [yes])
                ;;
        esac
    else
        if ! test -z "$2"; then
            if test "x$3" = "xtool"; then
                AC_CHECK_TOOL([$1_FOUND], [$2], [no])
                if ! test "x$$1_FOUND" = "xno"; then
                    $1="$(rel2abs "$$1_FOUND")"
                    $1_FOUND=yes
                fi
            else
                first_elem="$(rel2abs $(echo "$2" | cut -f 1 -d " "))"
                AC_CHECK_PROG([$1_FOUND], [$first_elem], [yes])
                if test "x$$1_FOUND" = "xyes"; then
                    $1="$(rel2abs "$2")"
                fi
            fi
        else
            $1_FOUND=no
        fi
    fi

    if ! test "x$$1_FOUND" = "xyes"; then
        if test "x$4" = "xno-err"; then
            AC_MSG_WARN([$first_elem invalid, set $1 to a valid program])
        else
            AC_MSG_ERROR([$first_elem invalid, set $1 to a valid program])
        fi
    fi
])

AC_ARG_VAR([STRIP], [strip command])
GET_PROG([STRIP], [strip], [tool])

AC_CHECK_HEADERS([stdio.h stdlib.h stdint.h stddef.h stdbool.h stdarg.h string.h errno.h inttypes.h limits.h time.h],
    [], [AC_MSG_ERROR([required header not found])])

AC_ARG_VAR([TOOLCHAIN_FOR_TARGET], [alternative toolchain prefix for Limine])

AC_ARG_VAR([CC_FOR_TARGET], [C compiler command for Limine])
if test "x${CC_FOR_TARGET+set}" = "x"; then
    if test "x${TOOLCHAIN_FOR_TARGET+set}" = "x"; then
        CC_FOR_TARGET="clang"
    else
        CC_FOR_TARGET="${TOOLCHAIN_FOR_TARGET}gcc"
    fi
fi
AC_ARG_VAR([LD_FOR_TARGET], [linker command for Limine])
if test "x${LD_FOR_TARGET+set}" = "x"; then
    if test "x${TOOLCHAIN_FOR_TARGET+set}" = "x"; then
        LD_FOR_TARGET="ld.lld"
    else
        LD_FOR_TARGET="${TOOLCHAIN_FOR_TARGET}ld"
    fi
fi

test "x${TOOLCHAIN_FOR_TARGET+set}" = "x" && TOOLCHAIN_FOR_TARGET=llvm-

AC_ARG_VAR([OBJCOPY_FOR_TARGET], [objcopy command for Limine])
test "x${OBJCOPY_FOR_TARGET+set}" = "x" && OBJCOPY_FOR_TARGET="${TOOLCHAIN_FOR_TARGET}objcopy"
AC_ARG_VAR([OBJDUMP_FOR_TARGET], [objdump command for Limine])
test "x${OBJDUMP_FOR_TARGET+set}" = "x" && OBJDUMP_FOR_TARGET="${TOOLCHAIN_FOR_TARGET}objdump"
AC_ARG_VAR([READELF_FOR_TARGET], [readelf command for Limine])
test "x${READELF_FOR_TARGET+set}" = "x" && READELF_FOR_TARGET="${TOOLCHAIN_FOR_TARGET}readelf"

GET_PROG([CC_FOR_TARGET])
GET_PROG([LD_FOR_TARGET])
GET_PROG([OBJCOPY_FOR_TARGET])
GET_PROG([OBJDUMP_FOR_TARGET])
GET_PROG([READELF_FOR_TARGET])

BUILD_ALL="no"

AC_ARG_ENABLE([all],
    [AS_HELP_STRING([--enable-all], [enable ALL ports and targets])],
    [BUILD_ALL="$enableval"])

BUILD_BIOS_CD="$BUILD_ALL"

AC_ARG_ENABLE([bios-cd],
    [AS_HELP_STRING([--enable-bios-cd], [enable building the x86 BIOS CD image])],
    [BUILD_BIOS_CD="$enableval"])

AC_SUBST([BUILD_BIOS_CD])

BUILD_BIOS_PXE="$BUILD_ALL"

AC_ARG_ENABLE([bios-pxe],
    [AS_HELP_STRING([--enable-bios-pxe], [enable building the x86 BIOS PXE image])],
    [BUILD_BIOS_PXE="$enableval"])

AC_SUBST([BUILD_BIOS_PXE])

BUILD_BIOS="$BUILD_ALL"

AC_ARG_ENABLE([bios],
    [AS_HELP_STRING([--enable-bios], [enable building the x86 BIOS port])],
    [BUILD_BIOS="$enableval"])

if test "x$BUILD_BIOS" = "xno"; then
    if test "x$BUILD_BIOS_CD" = "xyes"; then
        BUILD_BIOS="yes"
    fi
    if test "x$BUILD_BIOS_PXE" = "xyes"; then
        BUILD_BIOS="yes"
    fi
fi

if test "x$BUILD_BIOS" = "xno"; then
    BUILD_BIOS=""
else
    BUILD_BIOS="limine-bios"
    NEED_NASM=yes
    NEED_GZIP=yes
fi

AC_SUBST([BUILD_BIOS])

BUILD_UEFI_IA32="$BUILD_ALL"

AC_ARG_ENABLE([uefi-ia32],
    [AS_HELP_STRING([--enable-uefi-ia32], [enable building the IA-32 UEFI port])],
    [BUILD_UEFI_IA32="$enableval"])

if test "x$BUILD_UEFI_IA32" = "xno"; then
    BUILD_UEFI_IA32=""
else
    BUILD_UEFI_IA32="limine-uefi-ia32"
    NEED_NASM=yes
fi

AC_SUBST([BUILD_UEFI_IA32])

BUILD_UEFI_X86_64="$BUILD_ALL"

AC_ARG_ENABLE([uefi-x86-64],
    [AS_HELP_STRING([--enable-uefi-x86-64], [enable building the x86-64 UEFI port])],
    [BUILD_UEFI_X86_64="$enableval"])

if test "x$BUILD_UEFI_X86_64" = "xno"; then
    BUILD_UEFI_X86_64=""
else
    BUILD_UEFI_X86_64="limine-uefi-x86-64"
    NEED_NASM=yes
fi

AC_SUBST([BUILD_UEFI_X86_64])

BUILD_UEFI_AARCH64="$BUILD_ALL"

AC_ARG_ENABLE([uefi-aarch64],
    [AS_HELP_STRING([--enable-uefi-aarch64], [enable building the aarch64 UEFI port])],
    [BUILD_UEFI_AARCH64="$enableval"])

if test "x$BUILD_UEFI_AARCH64" = "xno"; then
    BUILD_UEFI_AARCH64=""
else
    BUILD_UEFI_AARCH64="limine-uefi-aarch64"
fi

AC_SUBST([BUILD_UEFI_AARCH64])

BUILD_UEFI_RISCV64="$BUILD_ALL"

AC_ARG_ENABLE([uefi-riscv64],
    [AS_HELP_STRING([--enable-uefi-riscv64], [enable building the riscv64 UEFI port])],
    [BUILD_UEFI_RISCV64="$enableval"])

if test "x$BUILD_UEFI_RISCV64" = "xno"; then
    BUILD_UEFI_RISCV64=""
else
    BUILD_UEFI_RISCV64="limine-uefi-riscv64"
fi

AC_SUBST([BUILD_UEFI_RISCV64])

BUILD_UEFI_LOONGARCH64="$BUILD_ALL"

AC_ARG_ENABLE([uefi-loongarch64],
    [AS_HELP_STRING([--enable-uefi-loongarch64], [enable building the loongarch64 UEFI port])],
    [BUILD_UEFI_LOONGARCH64="$enableval"])

if test "x$BUILD_UEFI_LOONGARCH64" = "xno"; then
    BUILD_UEFI_LOONGARCH64=""
else
    BUILD_UEFI_LOONGARCH64="limine-uefi-loongarch64"
fi

AC_SUBST([BUILD_UEFI_LOONGARCH64])

BUILD_UEFI_CD="$BUILD_ALL"

AC_ARG_ENABLE([uefi-cd],
    [AS_HELP_STRING([--enable-uefi-cd], [enable building limine-uefi-cd.bin])],
    [BUILD_UEFI_CD="$enableval"])

if ! test "x$BUILD_UEFI_CD" = "xno"; then
    AC_CHECK_PROG([MTOOLS_FOUND], [mcopy], [yes])
    if ! test "x$MTOOLS_FOUND" = "xyes"; then
        if test "x$BUILD_UEFI_CD" = "xyes"; then
            AC_MSG_ERROR([mtools not found, install mtools to build limine-uefi-cd.bin])
        fi
        AC_MSG_WARN([mtools not found, install mtools to build limine-uefi-cd.bin])
        BUILD_UEFI_CD="no"
    fi
fi

AC_SUBST([BUILD_UEFI_CD])

if test "x$NEED_NASM" = "xyes"; then
    AC_CHECK_PROG([NASM_FOUND], [nasm], [yes])
    if ! test "x$NASM_FOUND" = "xyes"; then
        AC_MSG_ERROR([nasm not found, please install nasm before configuring])
    fi
fi

if test "x$NEED_GZIP" = "xyes"; then
    AC_CHECK_PROG([GZIP_FOUND], [gzip], [yes])
    if ! test "x$GZIP_FOUND" = "xyes"; then
        AC_MSG_ERROR([gzip not found, please install gzip before configuring])
    fi
fi

BORROWED_CFLAGS=""
for cflag in $CFLAGS; do
    case $cflag in
        -O*|-pipe|-g|-f*-prefix-map*)
            BORROWED_CFLAGS="$BORROWED_CFLAGS $cflag"
            ;;
    esac
done

AC_ARG_VAR([CFLAGS_FOR_TARGET], [C flags for Limine])
test "x${CFLAGS_FOR_TARGET+set}" = "x" && CFLAGS_FOR_TARGET="$BORROWED_CFLAGS"

AC_ARG_VAR([CPPFLAGS_FOR_TARGET], [C preprocessor flags for Limine])
test "x${CPPFLAGS_FOR_TARGET+set}" = "x" && CPPFLAGS_FOR_TARGET=""

AC_ARG_VAR([LDFLAGS_FOR_TARGET], [linker flags for Limine])
test "x${LDFLAGS_FOR_TARGET+set}" = "x" && LDFLAGS_FOR_TARGET=""

AC_ARG_VAR([NASMFLAGS_FOR_TARGET], [nasm flags for Limine])
test "x${NASMFLAGS_FOR_TARGET+set}" = "x" && NASMFLAGS_FOR_TARGET="-g"

LIMINE_COPYRIGHT=$($GREP Copyright "$SRCDIR/COPYING")
AC_SUBST([LIMINE_COPYRIGHT])

AC_PREFIX_DEFAULT([/usr/local])

AC_CONFIG_FILES([man/man1/limine.1 GNUmakefile config.h])
AC_OUTPUT
