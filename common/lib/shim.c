#if defined (UEFI)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <efi.h>
#include <lib/shim.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <lib/libc.h>

static struct ShimLock *shim_lock = NULL;
static bool shim_initialized = false;

static struct SecurityOverride
{
	EFI_SECURITY_ARCH_PROTOCOL *security;
	EFI_SECURITY2_ARCH_PROTOCOL *security2;
	EFI_SECURITY_FILE_AUTHENTICATION_STATE original_hook;
	EFI_SECURITY2_FILE_AUTHENTICATION original_hook2;

	security_validator_t validator;
	const void *validator_ctx;

	const void *override_buffer;
	size_t override_size;

	bool validated_by_shim;
	bool installed;
} security_override;

static bool last_image_validated_by_shim = false;

bool shim_last_image_validated_by_shim(void)
{
	return last_image_validated_by_shim;
}

EFI_STATUS shim_init(void)
{
	EFI_STATUS status;
	EFI_GUID shim_lock_guid = SHIM_LOCK_PROTOCOL_GUID;

	status = gBS->LocateProtocol(&shim_lock_guid, NULL, (void **)&shim_lock);
	if (status != EFI_SUCCESS)
	{
		shim_lock = NULL;
	}

	shim_initialized = true;
	return EFI_SUCCESS;
}

bool shim_loaded(void)
{
	if (!shim_initialized)
	{
		shim_init();
	}

	return shim_lock != NULL;
}

static bool default_shim_validate(
	const EFI_DEVICE_PATH_PROTOCOL *device_path,
	const void *file_buffer,
	size_t file_size
)
{
	(void)device_path;

	if (!shim_loaded())
	{
		return false;
	}

	if (file_buffer != NULL && file_size > 0)
	{
		EFI_STATUS status = shim_lock->shim_verify(file_buffer, (uint32_t)file_size);
		return status == EFI_SUCCESS;
	}

	if (security_override.override_buffer != NULL && security_override.override_size > 0)
	{
		EFI_STATUS status = shim_lock->shim_verify(
			security_override.override_buffer,
			(uint32_t)security_override.override_size
		);
		return status == EFI_SUCCESS;
	}

	return false;
}

static EFI_STATUS EFI_API security2_hook(
	const EFI_SECURITY2_ARCH_PROTOCOL *This,
	const EFI_DEVICE_PATH_PROTOCOL *DevicePath,
	void *FileBuffer,
	size_t FileSize,
	bool BootPolicy
)
{
	(void)This;

	if (security_override.original_hook2 != NULL)
	{
		EFI_STATUS status = security_override.original_hook2(
			security_override.security2,
			DevicePath,
			FileBuffer,
			FileSize,
			BootPolicy
		);
		if (status == EFI_SUCCESS)
		{
			security_override.validated_by_shim = false;
			return EFI_SUCCESS;
		}
	}

	if (security_override.validator != NULL)
	{
		if (security_override.validator(
			security_override.validator_ctx,
			DevicePath,
			FileBuffer,
			FileSize
		))
		{
			security_override.validated_by_shim = true;
			return EFI_SUCCESS;
		}
	}
	else
	{
		if (default_shim_validate(DevicePath, FileBuffer, FileSize))
		{
			security_override.validated_by_shim = true;
			return EFI_SUCCESS;
		}
	}

	return EFI_SECURITY_VIOLATION;
}

static EFI_STATUS EFI_API security_hook(
	const EFI_SECURITY_ARCH_PROTOCOL *This,
	uint32_t AuthenticationStatus,
	const EFI_DEVICE_PATH_PROTOCOL *File
)
{
	(void)This;

	if (security_override.original_hook != NULL)
	{
		EFI_STATUS status = security_override.original_hook(
			security_override.security,
			AuthenticationStatus,
			File
		);
		if (status == EFI_SUCCESS)
		{
			security_override.validated_by_shim = false;
			return EFI_SUCCESS;
		}
	}

	if (security_override.validator != NULL)
	{
		if (security_override.validator(
			security_override.validator_ctx,
			File,
			NULL,
			0
		))
		{
			security_override.validated_by_shim = true;
			return EFI_SUCCESS;
		}
	}
	else
	{
		if (default_shim_validate(File, NULL, 0))
		{
			security_override.validated_by_shim = true;
			return EFI_SUCCESS;
		}
	}

	return EFI_SECURITY_VIOLATION;
}

void install_security_override_ctx(security_validator_t validator, const void *validator_ctx)
{
	if (!secure_boot_active && !shim_loaded())
	{
		return;
	}

	security_override.validator = validator;
	security_override.validator_ctx = validator_ctx;

	if (security_override.installed)
	{
		return;
	}

	EFI_GUID sec2_guid = EFI_SECURITY2_ARCH_PROTOCOL_GUID;
	EFI_SECURITY2_ARCH_PROTOCOL *sec2 = NULL;
	EFI_STATUS status = gBS->LocateProtocol(&sec2_guid, NULL, (void **)&sec2);
	if (status == EFI_SUCCESS && sec2 != NULL)
	{
		security_override.security2 = sec2;
		security_override.original_hook2 = sec2->FileAuthentication;
		sec2->FileAuthentication = security2_hook;
	}

	EFI_GUID sec_guid = EFI_SECURITY_ARCH_PROTOCOL_GUID;
	EFI_SECURITY_ARCH_PROTOCOL *sec = NULL;
	status = gBS->LocateProtocol(&sec_guid, NULL, (void **)&sec);
	if (status == EFI_SUCCESS && sec != NULL)
	{
		security_override.security = sec;
		security_override.original_hook = sec->FileAuthenticationState;
		sec->FileAuthenticationState = security_hook;
	}

	security_override.installed = true;
}

void install_security_override(void)
{
	install_security_override_ctx(NULL, NULL);
}

void uninstall_security_override(void)
{
	if (!security_override.installed)
	{
		return;
	}

	if (security_override.security2 != NULL && security_override.original_hook2 != NULL)
	{
		security_override.security2->FileAuthentication = security_override.original_hook2;
	}
	if (security_override.security != NULL && security_override.original_hook != NULL)
	{
		security_override.security->FileAuthenticationState = security_override.original_hook;
	}

	security_override.security = NULL;
	security_override.security2 = NULL;
	security_override.original_hook = NULL;
	security_override.original_hook2 = NULL;
	security_override.validator = NULL;
	security_override.validator_ctx = NULL;
	security_override.override_buffer = NULL;
	security_override.override_size = 0;
	security_override.validated_by_shim = false;
	security_override.installed = false;
}

bool security_override_available(void)
{
	EFI_SECURITY2_ARCH_PROTOCOL *sec2 = NULL;
	EFI_GUID sec2_guid = EFI_SECURITY2_ARCH_PROTOCOL_GUID;
	if (gBS->LocateProtocol(&sec2_guid, NULL, (void **)&sec2) == EFI_SUCCESS)
	{
		return true;
	}
	EFI_SECURITY_ARCH_PROTOCOL *sec = NULL;
	EFI_GUID sec_guid = EFI_SECURITY_ARCH_PROTOCOL_GUID;
	if (gBS->LocateProtocol(&sec_guid, NULL, (void **)&sec) == EFI_SUCCESS)
	{
		return true;
	}
	return false;
}

EFI_STATUS shim_load_image(
	bool boot_policy,
	EFI_HANDLE parent_image_handle,
	EFI_DEVICE_PATH_PROTOCOL *device_path,
	const void *source_buffer,
	size_t source_size,
	EFI_HANDLE *ret_image_handle
)
{
	if (ret_image_handle == NULL)
	{
		return EFI_INVALID_PARAMETER;
	}

	if (parent_image_handle == NULL)
	{
		parent_image_handle = efi_image_handle;
	}

	EFI_MEMMAP_DEVICE_PATH memdev_path[2];
	EFI_DEVICE_PATH_PROTOCOL *dp = device_path;

	if (dp == NULL && source_buffer != NULL)
	{
		memdev_path[0].Header.Type = EFI_HARDWARE_DEVICE_PATH;
		memdev_path[0].Header.SubType = EFI_HW_MEMMAP_DP;
		memdev_path[0].Header.Length[0] = sizeof(EFI_MEMMAP_DEVICE_PATH);
		memdev_path[0].Header.Length[1] = sizeof(EFI_MEMMAP_DEVICE_PATH) >> 8;

		memdev_path[0].MemoryType       = EfiLoaderCode;
		memdev_path[0].StartingAddress  = (uintptr_t)source_buffer;
		memdev_path[0].EndingAddress    = (uintptr_t)source_buffer + (source_size > 0 ? source_size - 1 : 0);

		memdev_path[1].Header.Type = EFI_END_DEVICE_PATH_TYPE;
		memdev_path[1].Header.SubType = EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE;
		memdev_path[1].Header.Length[0] = sizeof(EFI_DEVICE_PATH_PROTOCOL);
		memdev_path[1].Header.Length[1] = sizeof(EFI_DEVICE_PATH_PROTOCOL) >> 8;

		dp = (EFI_DEVICE_PATH_PROTOCOL *)memdev_path;
	}

	bool have_shim = shim_loaded();

	last_image_validated_by_shim = false;
	security_override.validated_by_shim = false;

	if (have_shim)
	{
		security_override.override_buffer = source_buffer;
		security_override.override_size = source_size;
		install_security_override();
	}

	EFI_STATUS status = gBS->LoadImage(
		boot_policy,
		parent_image_handle,
		dp,
		(void *)source_buffer,
		source_size,
		ret_image_handle
	);

	if (have_shim)
	{
		if (status == EFI_SUCCESS)
		{
			last_image_validated_by_shim = security_override.validated_by_shim;
		}
		uninstall_security_override();
	}

	return status;
}

#endif // UEFI
