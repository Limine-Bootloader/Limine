#ifndef LIB__SHIM_H__
#define LIB__SHIM_H__

#if defined (UEFI)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <efi.h>

#ifndef SHIM_LOCK_PROTOCOL_GUID
#define SHIM_LOCK_PROTOCOL_GUID \
	{ 0x605dab50, 0xe046, 0x4300, { 0xab, 0xb6, 0x3d, 0xd8, 0x10, 0xdd, 0x8b, 0x23 } }
#endif

#ifndef EFI_SECURITY_ARCH_PROTOCOL_GUID
#define EFI_SECURITY_ARCH_PROTOCOL_GUID \
	{ 0xA46423E3, 0x4617, 0x49f1, { 0xB9, 0xFF, 0xD1, 0xBF, 0xA9, 0x11, 0x58, 0x39 } }
#endif

#ifndef EFI_SECURITY2_ARCH_PROTOCOL_GUID
#define EFI_SECURITY2_ARCH_PROTOCOL_GUID \
	{ 0x94ab2f58, 0x1438, 0x4ef1, { 0x91, 0x52, 0x18, 0x94, 0x1a, 0x3a, 0x0e, 0x68 } }
#endif

#ifndef __sysv_abi__
#if defined(__x86_64__) || defined(__i386__)
#define __sysv_abi__ __attribute__((sysv_abi))
#else
#define __sysv_abi__
#endif
#endif

struct ShimLock
{
	EFI_STATUS __sysv_abi__ (*shim_verify)(const void *buffer, uint32_t size);
	EFI_STATUS __sysv_abi__ (*generate_hash)(void *data, uint32_t datasize, void *context, uint8_t *sha256hash, uint8_t *sha1hash);
	EFI_STATUS __sysv_abi__ (*read_header)(void *data, uint32_t datasize, void *context);
};

typedef struct ShimLock SHIM_LOCK;
typedef struct ShimLock EFI_SHIM_LOCK_PROTOCOL;
typedef struct ShimLock SHIM_LOCK_PROTOCOL;

typedef struct _EFI_SECURITY_ARCH_PROTOCOL EFI_SECURITY_ARCH_PROTOCOL;
typedef struct _EFI_SECURITY2_ARCH_PROTOCOL EFI_SECURITY2_ARCH_PROTOCOL;

typedef EFI_STATUS (EFI_API *EFI_SECURITY_FILE_AUTHENTICATION_STATE)(
	const EFI_SECURITY_ARCH_PROTOCOL *This,
	uint32_t AuthenticationStatus,
	const EFI_DEVICE_PATH_PROTOCOL *File
);

typedef EFI_STATUS (EFI_API *EFI_SECURITY2_FILE_AUTHENTICATION)(
	const EFI_SECURITY2_ARCH_PROTOCOL *This,
	const EFI_DEVICE_PATH_PROTOCOL *DevicePath,
	void *FileBuffer,
	size_t FileSize,
	bool BootPolicy
);

struct _EFI_SECURITY_ARCH_PROTOCOL
{
	EFI_SECURITY_FILE_AUTHENTICATION_STATE FileAuthenticationState;
};

struct _EFI_SECURITY2_ARCH_PROTOCOL
{
	EFI_SECURITY2_FILE_AUTHENTICATION FileAuthentication;
};

typedef bool (*security_validator_t)(
	const void *ctx,
	const EFI_DEVICE_PATH_PROTOCOL *device_path,
	const void *file_buffer,
	size_t file_size
);

EFI_STATUS shim_init(void);
bool shim_loaded(void);

void install_security_override(void);
void install_security_override_ctx(security_validator_t validator, const void *validator_ctx);
void uninstall_security_override(void);
bool security_override_available(void);
bool shim_last_image_validated_by_shim(void);

EFI_STATUS shim_load_image(
	bool boot_policy,
	EFI_HANDLE parent_image_handle,
	EFI_DEVICE_PATH_PROTOCOL *device_path,
	const void *source_buffer,
	size_t source_size,
	EFI_HANDLE *ret_image_handle
);

#else // !defined(UEFI)

#include <stdbool.h>

static inline bool shim_loaded(void)
{
	return false;
}

static inline bool shim_last_image_validated_by_shim(void)
{
	return false;
}

#endif // UEFI

#endif // LIB__SHIM_H__
