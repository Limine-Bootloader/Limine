AC_PREREQ([2.69])

AC_INIT([Limine], [m4_esyscmd([./version.sh])], [https://github.com/Limine-Bootloader/Limine/issues], [limine], [https://limine-bootloader.org/])

AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])

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

: "${CFLAGS=-g -O2 -pipe}"

AC_LANG([C])
AC_PROG_CC
CC="$(rel2abs "$CC")"

AX_PROG_CC_FOR_BUILD
CC_FOR_BUILD="$(rel2abs "$CC_FOR_BUILD")"

werror_state="no"
AC_ARG_ENABLE([werror],
    [AS_HELP_STRING([--enable-werror], [treat warnings as errors])],
    [werror_state="$enableval"])
AS_IF([test "x$werror_state" = "xyes"],
    [WERROR_FLAG="-Werror"],
    [WERROR_FLAG="-Wno-error"])
AC_SUBST([WERROR_FLAG])

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
AS_IF([test "x$FIND_FOUND" != "xyes"],
    [AC_MSG_ERROR([find not found, please install find before configuring])])

# GET_PROG(UPPERCASEVAR, [default command], ['tool' if toolchain], ['no-err' to warn instead of fail])
# Resolves VAR to an absolute program path, honouring a user-supplied value if
# there is one, and errors out when nothing usable is found.
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
AS_IF([test "x${TOOLCHAIN_FOR_TARGET+set}" = "x"],
    [: "${CC_FOR_TARGET=clang}"],
    [: "${CC_FOR_TARGET=${TOOLCHAIN_FOR_TARGET}gcc}"])

AC_ARG_VAR([LD_FOR_TARGET], [linker command for Limine])
AS_IF([test "x${TOOLCHAIN_FOR_TARGET+set}" = "x"],
    [: "${LD_FOR_TARGET=ld.lld}"],
    [: "${LD_FOR_TARGET=${TOOLCHAIN_FOR_TARGET}ld}"])

: "${TOOLCHAIN_FOR_TARGET=llvm-}"

AC_ARG_VAR([OBJCOPY_FOR_TARGET], [objcopy command for Limine])
: "${OBJCOPY_FOR_TARGET=${TOOLCHAIN_FOR_TARGET}objcopy}"
AC_ARG_VAR([OBJDUMP_FOR_TARGET], [objdump command for Limine])
: "${OBJDUMP_FOR_TARGET=${TOOLCHAIN_FOR_TARGET}objdump}"
AC_ARG_VAR([READELF_FOR_TARGET], [readelf command for Limine])
: "${READELF_FOR_TARGET=${TOOLCHAIN_FOR_TARGET}readelf}"

GET_PROG([CC_FOR_TARGET])
GET_PROG([LD_FOR_TARGET])
GET_PROG([OBJCOPY_FOR_TARGET])
GET_PROG([OBJDUMP_FOR_TARGET])
GET_PROG([READELF_FOR_TARGET])

BUILD_ALL="no"

AC_ARG_ENABLE([all],
    [AS_HELP_STRING([--enable-all], [enable ALL ports and targets])],
    [BUILD_ALL="$enableval"])

# LIMINE_ENABLE(option, VAR, help text)
# Declares --enable-<option>, defaulting to whatever --enable-all selected, and
# leaves the resulting yes/no in BUILD_<VAR>.
AC_DEFUN([LIMINE_ENABLE], [
    BUILD_$2="$BUILD_ALL"
    AC_ARG_ENABLE([$1],
        [AS_HELP_STRING([--enable-$1], [$3])],
        [BUILD_$2="$enableval"])
])

# LIMINE_PORT(VAR, make target, [nasm])
# Rewrites the yes/no in BUILD_<VAR> into the make target that builds the port,
# or the empty string when disabled, and notes whether the port needs nasm.
AC_DEFUN([LIMINE_PORT], [
    AS_IF([test "x$BUILD_$1" = "xno"],
        [BUILD_$1=""],
        [BUILD_$1="$2"
         m4_ifnblank([$3], [NEED_NASM=yes])])
    AC_SUBST([BUILD_$1])
])

LIMINE_ENABLE([bios-cd], [BIOS_CD], [enable building the x86 BIOS CD image])
AC_SUBST([BUILD_BIOS_CD])

LIMINE_ENABLE([bios-pxe], [BIOS_PXE], [enable building the x86 BIOS PXE image])
AC_SUBST([BUILD_BIOS_PXE])

LIMINE_ENABLE([bios], [BIOS], [enable building the x86 BIOS port])
# The CD and PXE images are artefacts of the BIOS port, so either one drags it in.
AS_IF([test "x$BUILD_BIOS" = "xno"], [
    AS_IF([test "x$BUILD_BIOS_CD" = "xyes" || test "x$BUILD_BIOS_PXE" = "xyes"],
        [BUILD_BIOS="yes"])
])
LIMINE_PORT([BIOS], [limine-bios], [nasm])

LIMINE_ENABLE([uefi-ia32], [UEFI_IA32], [enable building the IA-32 UEFI port])
LIMINE_PORT([UEFI_IA32], [limine-uefi-ia32], [nasm])

LIMINE_ENABLE([uefi-x86-64], [UEFI_X86_64], [enable building the x86-64 UEFI port])
LIMINE_PORT([UEFI_X86_64], [limine-uefi-x86-64], [nasm])

LIMINE_ENABLE([uefi-aarch64], [UEFI_AARCH64], [enable building the aarch64 UEFI port])
LIMINE_PORT([UEFI_AARCH64], [limine-uefi-aarch64])

LIMINE_ENABLE([uefi-riscv64], [UEFI_RISCV64], [enable building the riscv64 UEFI port])
LIMINE_PORT([UEFI_RISCV64], [limine-uefi-riscv64])

LIMINE_ENABLE([uefi-loongarch64], [UEFI_LOONGARCH64], [enable building the loongarch64 UEFI port])
LIMINE_PORT([UEFI_LOONGARCH64], [limine-uefi-loongarch64])

LIMINE_ENABLE([uefi-cd], [UEFI_CD], [enable building limine-uefi-cd.bin])
AS_IF([test "x$BUILD_UEFI_CD" != "xno"], [
    AC_CHECK_PROG([MTOOLS_FOUND], [mcopy], [yes])
    AS_IF([test "x$MTOOLS_FOUND" != "xyes"], [
        AS_IF([test "x$BUILD_UEFI_CD" = "xyes"],
            [AC_MSG_ERROR([mtools not found, install mtools to build limine-uefi-cd.bin])])
        AC_MSG_WARN([mtools not found, install mtools to build limine-uefi-cd.bin])
        BUILD_UEFI_CD="no"
    ])
])
AC_SUBST([BUILD_UEFI_CD])

AS_IF([test "x$NEED_NASM" = "xyes"], [
    AC_CHECK_PROG([NASM_FOUND], [nasm], [yes])
    AS_IF([test "x$NASM_FOUND" != "xyes"],
        [AC_MSG_ERROR([nasm not found, please install nasm before configuring])])
])


BORROWED_CFLAGS=""
for cflag in $CFLAGS; do
    AS_CASE([$cflag],
        [-O*|-pipe|-g|-f*-prefix-map*], [BORROWED_CFLAGS="$BORROWED_CFLAGS $cflag"])
done

AC_ARG_VAR([CFLAGS_FOR_TARGET], [C flags for Limine])
: "${CFLAGS_FOR_TARGET=$BORROWED_CFLAGS}"

AC_ARG_VAR([CPPFLAGS_FOR_TARGET], [C preprocessor flags for Limine])
: "${CPPFLAGS_FOR_TARGET=}"

AC_ARG_VAR([LDFLAGS_FOR_TARGET], [linker flags for Limine])
: "${LDFLAGS_FOR_TARGET=}"

AC_ARG_VAR([NASMFLAGS_FOR_TARGET], [nasm flags for Limine])
: "${NASMFLAGS_FOR_TARGET=-g}"

LIMINE_COPYRIGHT=$($GREP Copyright "$SRCDIR/COPYING")
AC_SUBST([LIMINE_COPYRIGHT])

AC_CONFIG_FILES([man/man1/limine.1 GNUmakefile config.h])
AC_OUTPUT
