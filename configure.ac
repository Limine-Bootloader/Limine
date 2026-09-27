AC_PREREQ([2.64])

AC_INIT([Limine], [m4_esyscmd([./version.sh])], [https://github.com/Limine-Bootloader/Limine/issues], [limine], [https://limine-bootloader.org/])

AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])

SRCDIR="$(cd "$srcdir" && pwd -P)"
BUILDDIR="$(pwd -P)"

# '&' otherwise yields an empty GNUmakefile with configure still exiting 0, and
# '=' makes make recurse without bound. '*' and '?' work from make 3.82
# onwards, but macOS still ships 3.81.
for bad_char in '"' "'" ':' '`' '#' '$' '&' ';' '|' '='; do
    case "$SRCDIR" in
        *"$bad_char"*)
            AC_MSG_ERROR([the source directory path contains '$bad_char'; move the source tree])
            ;;
    esac
done

for bad_char in "'" '%' ':' '#' '$' '&' ';' '|' '=' '*' '?'; do
    case "$BUILDDIR" in
        *"$bad_char"*)
            AC_MSG_ERROR([the build directory path contains '$bad_char'; use a different build directory])
            ;;
    esac
done

if test "$(printf '%s' "$SRCDIR$BUILDDIR" | wc -l)" -gt 0; then
    AC_MSG_ERROR([the source or build directory path contains a newline; move the tree])
fi

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
                        return 1
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
AC_PROG_EGREP
EGREP="$(rel2abs "$EGREP")"
AC_PROG_AWK
AWK="$(rel2abs "$AWK")"

AC_CHECK_PROG([FIND_FOUND], [find], [yes])
AS_IF([test "x$FIND_FOUND" != "xyes"],
    [AC_MSG_ERROR([find not found, please install find before configuring])])

# The macro only sets the version when it runs the check, not when the result
# comes from a cache file.
AS_UNSET([_cv_gnu_make_command])
AX_CHECK_GNU_MAKE([],
    [AC_MSG_ERROR([GNU make not found, please install GNU make 3.81 or later before configuring])])
AC_MSG_CHECKING([whether $_cv_gnu_make_command is 3.81 or later])
AX_COMPARE_VERSION([$ax_check_gnu_make_version], [ge], [3.81],
    [AC_MSG_RESULT([yes])],
    [AC_MSG_RESULT([no])
     AC_MSG_ERROR([$_cv_gnu_make_command is GNU make $ax_check_gnu_make_version, but 3.81 or later is required; set MAKE to a newer one])])

# GET_PROG(UPPERCASEVAR, [default command], ['tool' if toolchain], ['no-err' to warn instead of fail])
# Resolves VAR to an absolute program path, honouring a user-supplied value if
# there is one, and errors out when nothing usable is found.
AC_DEFUN([GET_PROG], [
    if test "x${$1+set}" = "xset"; then
        first_elem="$(rel2abs $(printf '%s\n' "$$1" | cut -f 1 -d " "))"
        case "$first_elem" in
            "")
                first_elem="$$1"
                $1_FOUND=no
                ;;
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
                first_elem="$(rel2abs $(printf '%s\n' "$2" | cut -f 1 -d " "))"
                if test -z "$first_elem"; then
                    first_elem="$2"
                    $1_FOUND=no
                else
                    AC_CHECK_PROG([$1_FOUND], [$first_elem], [yes])
                fi
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

# LIMINE_ENABLE(option, VAR, help text, [default])
# Declares --enable-<option>, defaulting to whatever --enable-all selected unless
# a default is given, and leaves the resulting yes/no in BUILD_<VAR>.
AC_DEFUN([LIMINE_ENABLE], [
    BUILD_$2=m4_default([$4], ["$BUILD_ALL"])
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

LIMINE_ENABLE([bios], [BIOS], [enable building the x86 BIOS port])

LIMINE_ENABLE([bios-cd], [BIOS_CD], [enable building the x86 BIOS CD image],
    ["$BUILD_BIOS"])
AC_SUBST([BUILD_BIOS_CD])

LIMINE_ENABLE([bios-pxe], [BIOS_PXE], [enable building the x86 BIOS PXE image],
    ["$BUILD_BIOS"])
AC_SUBST([BUILD_BIOS_PXE])

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

# limine-uefi-cd.bin is a container for the UEFI ports' binaries, so without one
# it would just be an empty filesystem.
BUILD_ANY_UEFI="no"
AS_IF([test -n "$BUILD_UEFI_IA32$BUILD_UEFI_X86_64$BUILD_UEFI_AARCH64$BUILD_UEFI_RISCV64$BUILD_UEFI_LOONGARCH64"],
    [BUILD_ANY_UEFI="yes"])

UEFI_CD_DEFAULT="$BUILD_ALL"
AS_IF([test "x$BUILD_ANY_UEFI" = "xno"], [UEFI_CD_DEFAULT="no"])

LIMINE_ENABLE([uefi-cd], [UEFI_CD], [enable building limine-uefi-cd.bin],
    ["$UEFI_CD_DEFAULT"])
AS_IF([test "x$BUILD_UEFI_CD" = "xyes" && test "x$BUILD_ANY_UEFI" = "xno"],
    [AC_MSG_ERROR([limine-uefi-cd.bin needs at least one UEFI port enabled])])
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

AC_MSG_CHECKING([whether $CC_FOR_TARGET is Clang])
AS_IF([$CC_FOR_TARGET --version 2>/dev/null | $GREP -q '^Target: '],
    [CC_FOR_TARGET_IS_CLANG="yes"],
    [CC_FOR_TARGET_IS_CLANG="no"])
AC_MSG_RESULT([$CC_FOR_TARGET_IS_CLANG])

# Help lines name a flag with one dash or two, and may follow it with a comma,
# an "=" or a "[" for its value.
limine_ld_lists_flag() {
    LC_ALL=C $LD_FOR_TARGET --help 2>/dev/null | $EGREP -q '(^|[[[:space:]]])-?'"$1"'([[[:space:],=@<:@]]|$)'
}

# Run CC_FOR_TARGET with the Clang target of the port being checked and
# CFLAGS_FOR_TARGET, as the build does, keeping its diagnostics, untranslated,
# in limine_diagnostics.
limine_cc_run() {
    printf '%s\n' "$CC_FOR_TARGET $limine_target $CFLAGS_FOR_TARGET $*" >&AS_MESSAGE_LOG_FD
    limine_diagnostics="$(LC_ALL=C $CC_FOR_TARGET $limine_target $CFLAGS_FOR_TARGET "$@" 2>&1 >/dev/null)"
    limine_status=$?
    test -z "$limine_diagnostics" || printf '%s\n' "$limine_diagnostics" >&AS_MESSAGE_LOG_FD
    return $limine_status
}

# Print an extended regular expression for the name of a flag, up to any "=",
# with the dashes after the first made optional, as compilers leave the value
# out or spell the flag differently.
limine_flag_name() {
    printf '%s\n' "$1" | $SED 's/=.*//; s/^-//; s/-/-?/g; s/^/-/'
}

# Succeed if limine_diagnostics name a flag, or its other spelling if one is
# given. The name has to stand on its own, as "-pie" would otherwise be found
# inside a complaint about "-no-pie".
limine_flag_is_named() {
    limine_names="$(limine_flag_name "$1")"
    if test -n "$2"; then
        limine_names="$limine_names|$(limine_flag_name "$2")"
    fi
    printf '%s\n' "$limine_diagnostics" | $EGREP -q '(error|warning|note):.*[[^[:alnum:]]]('"$limine_names"')([[^-[:alnum:]]]|$)'
}

# LIMINE_PORT_FLAGS(VAR, port, Clang triple architecture, C flags, linker flags,
#                   checks)
# Sets <VAR>_CFLAGS and <VAR>_LDFLAGS for a port if BUILD_<VAR> enables it,
# placing CFLAGS_FOR_TARGET after the Clang target and before the C flags, and
# LDFLAGS_FOR_TARGET before the linker flags. The checks see all the flags so
# far, placed as in the build. CC_FOR_TARGET is first checked to work with
# them, as the checks would otherwise blame a missing feature.
AC_DEFUN([LIMINE_PORT_FLAGS], [
    $1_CFLAGS=""
    $1_LDFLAGS=""
    AS_IF([test -n "$BUILD_$1"], [
        limine_port="$2"
        limine_target=""
        AS_IF([test "x$CC_FOR_TARGET_IS_CLANG" = "xyes"],
            [limine_target="-target $3-unknown-none-elf"])
        $1_CFLAGS="$4"
        $1_LDFLAGS="$5"
        AC_MSG_CHECKING([whether $CC_FOR_TARGET works for $2])
        AS_IF([limine_cc_run $$1_CFLAGS -c -x c /dev/null -o /dev/null],
            [AC_MSG_RESULT([yes])],
            [AC_MSG_RESULT([no])
             limine_error="$(printf '%s\n' "$limine_diagnostics" | $EGREP -i 'error|sorry' | head -n 1)"
             AC_MSG_ERROR([$CC_FOR_TARGET does not work for $2: $limine_error])])
        # Go by the macro for ELF targets, as GNU as cannot write an object to
        # standard output. Clang is given an ELF target, and before version 17
        # did not define the macro for all of them.
        AS_IF([test "x$CC_FOR_TARGET_IS_CLANG" = "xno"], [
            AC_MSG_CHECKING([whether $CC_FOR_TARGET emits ELF objects for $2])
            AS_IF([$CC_FOR_TARGET $CFLAGS_FOR_TARGET $$1_CFLAGS -dM -E -x c /dev/null 2>/dev/null | $GREP -q 'define __ELF__ 1'],
                [AC_MSG_RESULT([yes])],
                [AC_MSG_RESULT([no])
                 AC_MSG_ERROR([$CC_FOR_TARGET does not emit ELF objects for $2])])
        ])
        $6
        $1_CFLAGS="${limine_target:+$limine_target }\$(CFLAGS_FOR_TARGET) $$1_CFLAGS"
        $1_LDFLAGS="\$(LDFLAGS_FOR_TARGET) $$1_LDFLAGS"
    ])
    AC_SUBST([$1_CFLAGS])
    AC_SUBST([$1_LDFLAGS])
])

# LIMINE_CHECK_CFLAG(VAR, flag, [other spelling], [action if not taken])
# Adds a flag to <VAR>_CFLAGS unless a diagnostic of the compiler names it, as
# some compilers only warn about the flags they do not take.
AC_DEFUN([LIMINE_CHECK_CFLAG], [
    AC_MSG_CHECKING([whether $CC_FOR_TARGET accepts $2 for $limine_port])
    limine_cc_run $$1_CFLAGS $2 -c -x c /dev/null -o /dev/null
    AS_IF([limine_flag_is_named '$2' '$3'],
        [AC_MSG_RESULT([no])
         $4],
        [AC_MSG_RESULT([yes])
         $1_CFLAGS="$$1_CFLAGS $2"])
])

# LIMINE_CHECK_LDFLAG(VAR, flag, [action if not taken])
# Adds a flag to <VAR>_LDFLAGS if the help of LD_FOR_TARGET lists it, as older
# GNU ld take unknown flags for different ones (-no-pie for -n -o -pie).
AC_DEFUN([LIMINE_CHECK_LDFLAG], [
    AC_MSG_CHECKING([whether $LD_FOR_TARGET accepts $2])
    AS_IF([limine_ld_lists_flag '$2'],
        [AC_MSG_RESULT([yes])
         $1_LDFLAGS="$$1_LDFLAGS $2"],
        [AC_MSG_RESULT([no])
         $3])
])

# LIMINE_X86_CHECKS(VAR)
AC_DEFUN([LIMINE_X86_CHECKS], [
    LIMINE_CHECK_CFLAG([$1], [-fno-stack-clash-protection])
    LIMINE_CHECK_CFLAG([$1], [-fcf-protection=none])
    # GCC needs this flag for interrupt handlers and Clang below 3.9 has no
    # name for it, where only a long double could reach the x87.
    LIMINE_CHECK_CFLAG([$1], [-mno-80387])
])

limine_cflags="m4_normalize([
    -g -Wall -Wextra -Wshadow -Wvla $WERROR_FLAG -std=gnu11 -ffreestanding
    -fno-common -ffunction-sections -fdata-sections -fstack-protector-strong
    -fno-stack-check -fno-omit-frame-pointer -fno-strict-aliasing -fno-lto
])"
limine_ldflags="-nostdlib -z max-page-size=0x1000 -z noexecstack --gc-sections"

# GCC puts the x86 canary in the TLS block by default, and there is no TLS
# here. Clang already uses the global on the bare metal targets.
limine_x86_mflags=""
AS_IF([test "x$CC_FOR_TARGET_IS_CLANG" = "xno"],
    [limine_x86_mflags="-mstack-protector-guard=global"])

LIMINE_PORT_FLAGS([BIOS], [the x86 BIOS port], [i686],
    [$limine_cflags -fno-PIC -m32 -march=i686 -mno-mmx${limine_x86_mflags:+ $limine_x86_mflags}],
    [$limine_ldflags -m elf_i386 -static --build-id=sha1], [
    LIMINE_X86_CHECKS([BIOS])
    # Some linkers produce a PIE by default, which the BIOS port cannot be
    # linked as.
    LIMINE_CHECK_LDFLAG([BIOS], [-no-pie])
])

LIMINE_PORT_FLAGS([UEFI_IA32], [the IA-32 UEFI port], [i686],
    [$limine_cflags -fPIE -fshort-wchar -m32 -march=i686 -mno-mmx -malign-double${limine_x86_mflags:+ $limine_x86_mflags}],
    [$limine_ldflags -m elf_i386 -pie -z text --hash-style=gnu], [
    LIMINE_X86_CHECKS([UEFI_IA32])
    # Clang below 9 takes -malign-double and ignores it, which lays out every
    # firmware structure with a 64 bit member wrong, so check the alignment
    # rather than the flag.
    AC_MSG_CHECKING([whether $CC_FOR_TARGET aligns 64 bit types to 8 bytes for $limine_port])
    AS_IF([printf 'struct s { char c; long long x; };\n_Static_assert(sizeof(struct s) == 16, "");\n' |
           limine_cc_run $UEFI_IA32_CFLAGS -c -x c - -o /dev/null],
        [AC_MSG_RESULT([yes])],
        [AC_MSG_RESULT([no])
         AC_MSG_ERROR([$CC_FOR_TARGET does not align 64 bit types to 8 bytes])])
])

LIMINE_PORT_FLAGS([UEFI_X86_64], [the x86-64 UEFI port], [x86_64],
    [$limine_cflags -fPIE -fshort-wchar -m64 -march=x86-64 -mno-mmx -mno-sse -mno-red-zone${limine_x86_mflags:+ $limine_x86_mflags}],
    [$limine_ldflags -m elf_x86_64 -pie -z text --hash-style=gnu],
    [LIMINE_X86_CHECKS([UEFI_X86_64])])

LIMINE_PORT_FLAGS([UEFI_AARCH64], [the aarch64 UEFI port], [aarch64],
    [$limine_cflags -fPIE -fshort-wchar -mcpu=generic -march=armv8-a+nofp+nosimd -mcmodel=small],
    [$limine_ldflags -m aarch64elf -pie -z text --hash-style=gnu], [
    LIMINE_CHECK_CFLAG([UEFI_AARCH64], [-fno-stack-clash-protection])
    LIMINE_CHECK_CFLAG([UEFI_AARCH64], [-mno-outline-atomics])
    LIMINE_CHECK_CFLAG([UEFI_AARCH64], [-mbranch-protection=none])
    # Only the linker can work around Cortex-A53 erratum 843419, so require one
    # that can.
    LIMINE_CHECK_LDFLAG([UEFI_AARCH64], [--fix-cortex-a53-843419],
        [AC_MSG_ERROR([$LD_FOR_TARGET cannot work around Cortex-A53 erratum 843419])])
])

# The ABI is given first, as the instruction set is checked against it.
LIMINE_PORT_FLAGS([UEFI_RISCV64], [the riscv64 UEFI port], [riscv64],
    [$limine_cflags -fPIE -fshort-wchar -mabi=lp64],
    [$limine_ldflags -m elf64lriscv --no-relax -pie -z text --hash-style=gnu], [
    LIMINE_CHECK_CFLAG([UEFI_RISCV64], [-fno-stack-clash-protection])
    LIMINE_CHECK_CFLAG([UEFI_RISCV64], [-fcf-protection=none])
    # Name Zicsr and Zifencei only if the compiler knows them, as older ones
    # have them in the base ISA. The whole compile is checked, as the flag is
    # not always named when an instruction set is rejected.
    AC_MSG_CHECKING([whether $CC_FOR_TARGET accepts -march=rv64imac_zicsr_zifencei for $limine_port])
    AS_IF([limine_cc_run $UEFI_RISCV64_CFLAGS -march=rv64imac_zicsr_zifencei -c -x c /dev/null -o /dev/null],
        [AC_MSG_RESULT([yes])
         UEFI_RISCV64_CFLAGS="$UEFI_RISCV64_CFLAGS -march=rv64imac_zicsr_zifencei"],
        [AC_MSG_RESULT([no])
         UEFI_RISCV64_CFLAGS="$UEFI_RISCV64_CFLAGS -march=rv64imac"])
    # Clang called this code model "small" before the ISA's own name.
    LIMINE_CHECK_CFLAG([UEFI_RISCV64], [-mcmodel=medlow], [-mcode-model],
        [LIMINE_CHECK_CFLAG([UEFI_RISCV64], [-mcmodel=small], [-mcode-model],
            [AC_MSG_ERROR([$CC_FOR_TARGET has no name for the low code model])])])
    LIMINE_CHECK_CFLAG([UEFI_RISCV64], [-mno-relax])
])

# -msoft-float, unlike -mfpu=none, also overrides a -mdouble-float or
# -msingle-float in CFLAGS_FOR_TARGET, which take effect wherever they appear.
LIMINE_PORT_FLAGS([UEFI_LOONGARCH64], [the loongarch64 UEFI port], [loongarch64],
    [$limine_cflags -fPIE -fshort-wchar -mabi=lp64s -march=loongarch64 -msoft-float],
    [$limine_ldflags -m elf64loongarch --no-relax -pie -z text --hash-style=gnu], [
    LIMINE_CHECK_CFLAG([UEFI_LOONGARCH64], [-fno-stack-clash-protection])
    # Clang 16 takes the soft float flags but still records the double float
    # ABI, which no linker mixes with soft float objects, so read the ABI out
    # of the ELF header it writes. BSD od pads its output line with spaces.
    AS_IF([test "x$CC_FOR_TARGET_IS_CLANG" = "xyes"], [
        AC_MSG_CHECKING([whether $CC_FOR_TARGET records the soft float ABI for $limine_port])
        limine_float_abi="$($CC_FOR_TARGET $limine_target $CFLAGS_FOR_TARGET $UEFI_LOONGARCH64_CFLAGS -c -x c /dev/null -o - 2>/dev/null | od -A n -t x1 -j 48 -N 1 | tr -d ' ')"
        AS_CASE([$limine_float_abi],
            [*1], [AC_MSG_RESULT([yes])],
            [AC_MSG_RESULT([no])
             AC_MSG_ERROR([$CC_FOR_TARGET does not record the soft float ABI])])
    ])
    # Do not go through the GOT for external symbols. Only GCC is checked, as
    # Clang had its flag before the architecture existed.
    AS_IF([test "x$CC_FOR_TARGET_IS_CLANG" = "xyes"],
        [UEFI_LOONGARCH64_CFLAGS="$UEFI_LOONGARCH64_CFLAGS -fdirect-access-external-data"],
        [LIMINE_CHECK_CFLAG([UEFI_LOONGARCH64], [-mdirect-extern-access])])
    # Clang called this code model "small" before the ISA's own name.
    LIMINE_CHECK_CFLAG([UEFI_LOONGARCH64], [-mcmodel=normal], [-mcode-model],
        [LIMINE_CHECK_CFLAG([UEFI_LOONGARCH64], [-mcmodel=small], [-mcode-model],
            [AC_MSG_ERROR([$CC_FOR_TARGET has no name for the normal code model])])])
    LIMINE_CHECK_CFLAG([UEFI_LOONGARCH64], [-mno-relax])
])

LIMINE_COPYRIGHT=$($GREP Copyright "$SRCDIR/COPYING")
AC_SUBST([LIMINE_COPYRIGHT])

AC_CONFIG_FILES([man/man1/limine.1 GNUmakefile config.h])
AC_OUTPUT
