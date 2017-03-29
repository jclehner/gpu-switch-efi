/**
 * gpu-switch-efi
 * Copyright (C) 2017 Joseph Lehner <joseph.c.lehner@gmail.com>
 *
 * bcm2-utils is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * bcm2-utils is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with bcm2-utils.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <efi.h>
#include <efilib.h>

#ifndef VERSION
#define VERSION L"(unknown)"
#endif

#define GPU_DEDICATED 0
#define GPU_INTERNAL 1
#define GPU_NAME(n) ((n) ? "internal" : "dedicated")

#define VAR_ACCESS_BS_RT (EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)
#define VAR_ATTR_MASK (VAR_ACCESS_BS_RT | EFI_VARIABLE_NON_VOLATILE)

#define G2P_NAME L"gpu-power-prefs"
#define GP_NAME L"gpu-policy"
#define GSCRS_NAME L"gfx-saved-config-restore-status"

static EFI_GUID appleNvGuid =
	{ 0x4d1ede05, 0x38c7, 0x4a6a, { 0x9c, 0xc6, 0x4b, 0xcc, 0xa8, 0xb3, 0x8c, 0x14 }};

static EFI_GUID g2pGuid =
	{ 0xfa4ce28d, 0xb62f, 0x4c99, { 0x9c, 0xc3, 0x68, 0x15, 0x68, 0x6e, 0x30, 0xf9 }};

static EFI_GUID gpGuid =
	{ 0x7c436110, 0xab2a, 0x4bbb, { 0xa8, 0x80, 0xfe, 0x41, 0x99, 0x5c, 0x9f, 0x82 }};

static UINT32 verbosity = 0;

static BOOLEAN GetEfiVar(EFI_GUID *guid, CHAR16 *name, CHAR8 *buf, UINTN *len, UINT32 *attrs)
{
	EFI_STATUS status;

	if (verbosity > 2) {
		Print(L"%a(%g, %s, ..., %ld, %02x)\n", __func__, guid, name, *len, attrs ? *attrs : 0);
	}

	status = uefi_call_wrapper(RT->GetVariable, 5, name, guid, attrs, len, buf);
	if (!EFI_ERROR(status)) {
		return *len > 0;
	} else if (verbosity && status != EFI_NOT_FOUND) {
		Print(L"%a: %s: %r\n", __func__, name, status);
	}

	return FALSE;
}

static BOOLEAN SetEfiVar(EFI_GUID *guid, CHAR16 *name, CHAR8 *buf, UINTN len, UINT32 attrs)
{
	EFI_STATUS status;

	if (verbosity > 2) {
		Print(L"%a(%g, %s, ..., %ld, %02x)\n", __func__, guid, name, len, attrs);
	}

	attrs &= VAR_ATTR_MASK;

	if (!(attrs & ~VAR_ACCESS_BS_RT)) {
		attrs |= VAR_ACCESS_BS_RT;
	}

	status = uefi_call_wrapper(RT->SetVariable, 5, name, guid, attrs, len, buf);
	if (EFI_ERROR(status)) {
		if (verbosity) {
			Print(L"%a: %s: %r\n", __func__, name, status);
		}
		return FALSE;
	}

	return TRUE;
}

typedef BOOLEAN (*EditEfiVarCallback)(CHAR16 *name, CHAR8 *buf, UINTN len, VOID *arg);
static BOOLEAN EditEfiVar(EFI_GUID *guid, CHAR16 *name, EditEfiVarCallback cb, VOID *arg)
{
	CHAR8 buf[1024];
	UINTN len;
	UINT32 attrs;

	if (verbosity > 2) {
		Print(L"%a(%g, %s, ..., ...)\n", __func__, guid, name);
	}

	if (!cb) {
		return FALSE;
	}

	len = sizeof(buf);
	attrs = 0;

	if (!GetEfiVar(guid, name, buf, &len, &attrs)) {
		return FALSE;
	}

	if (!cb(name, buf, len, arg)) {
		return FALSE;
	}

	return SetEfiVar(guid, name, buf, len, attrs);
}

static BOOLEAN EditGpuPowerPrefsAndPolicy(CHAR16 *name, CHAR8 *buf, UINTN len, VOID *arg)
{
	if ((!StrCmp(name, L"gpu-power-prefs") && len != 4)
			|| (!StrCmp(name, L"gpu-policy") && len != 1))
	{
		if (verbosity) {
			Print(L"%a: unexpected length %ld for %s\n", __func__, len, name);
		}

		return FALSE;
	}

	if (verbosity > 2) {
		Print(L"%a(%s, ..., %ld, %a)\n", __func__, name, len, *(BOOLEAN*)arg ? "TRUE" : "FALSE");
	}

	buf[0] = (*(BOOLEAN*)arg) ? 1 : 0;
	return TRUE;
}

static BOOLEAN SetNextBootGpu(BOOLEAN internal)
{
	BOOLEAN ret = FALSE;

	ret |= EditEfiVar(&g2pGuid, G2P_NAME, &EditGpuPowerPrefsAndPolicy, &internal);
	ret |= EditEfiVar(&gpGuid, GP_NAME, &EditGpuPowerPrefsAndPolicy, &internal);

	return ret;
}

static BOOLEAN DumpEfiVar(EFI_GUID *guid, CHAR16 *name)
{
	CHAR8 buf[1024];
	UINTN i, len = sizeof(buf);

	ZeroMem(buf, len);

	if (!GetEfiVar(guid, name, buf, &len, NULL)) {
		return FALSE;
	}

	Print(L"%s: ", name, &guid);

	for (i = 0; i < len; ++i) {
		Print(L" %02x", buf[i] & 0xff);
	}

	Print(L"\n");
	FreePool(buf);

	return TRUE;
}

static BOOLEAN DumpEfiVars(VOID)
{
	BOOLEAN ret = FALSE;

	ret |= DumpEfiVar(&appleNvGuid, GSCRS_NAME);
	ret |= DumpEfiVar(&g2pGuid, G2P_NAME);
	ret |= DumpEfiVar(&gpGuid, GP_NAME);

	if (!ret) {
		Print(L"Nothing to dump.\n");
	}

	return ret;
}

static EFI_STATUS Exit(EFI_HANDLE imageHandle, EFI_STATUS status)
{
	return uefi_call_wrapper(BS->Exit, 4, imageHandle, status, 0, NULL);
}

static EFI_STATUS PrintUsageAndExit(EFI_HANDLE imageHandle, BOOLEAN error)
{
	Print(
			L"Usage: gpu-switch.efi [options]\n"
			L"\n"
			L"Options:\n"
			L" -v     Verbose operation\n"
			L" -p     Dump important EFI variables\n"
			L" -i     Force integrated GPU on next boot\n"
			L" -d     Force dedicated GPU on next boot\n"
			L"\n"
			L"gpu-switch.efi " VERSION "\n"
			L"Copyright (C) 2017 Joseph C. Lehner\n"
			L"Licensed under the GNU GPLv3; source:\n"
			L"https://github.com/jclehner/gpu-switch-efi\n");

	return Exit(imageHandle, error ? EFI_INVALID_PARAMETER : EFI_SUCCESS);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE imageHandle, EFI_SYSTEM_TABLE *systemTable)
{
	CHAR16 **argv;
	INTN i, argc;
	BOOLEAN dump = FALSE;
	UINT32 gpu = ~0;
	EFI_STATUS status = EFI_INVALID_PARAMETER;

	InitializeLib(imageHandle, systemTable);
	argc = GetShellArgcArgv(imageHandle, &argv);

	for (i = 0; i < argc; ++i) {
		if (!StrCmp(argv[i], L"-v")) {
			++verbosity;
		} else if (!StrCmp(argv[i], L"-h")) {
			return PrintUsageAndExit(imageHandle, FALSE);
		} else if (!StrCmp(argv[i], L"-i")) {
			gpu = GPU_INTERNAL;
		} else if (!StrCmp(argv[i], L"-d")) {
			gpu = GPU_DEDICATED;
		} else if (!StrCmp(argv[i], L"-p")) {
			dump = TRUE;
		}
	}

	if (!dump && (gpu == ~0U)) {
		return PrintUsageAndExit(imageHandle, TRUE);
	} else if (dump) {
		status = DumpEfiVars() ? EFI_SUCCESS : EFI_NOT_FOUND;
	} else {
		if (SetNextBootGpu(gpu)) {
			Print(L"Successfully switched to %a GPU\n", GPU_NAME(gpu));
			status = EFI_SUCCESS;
		} else {
			Print(L"Failed to switch to %a GPU\n", GPU_NAME(gpu));
			status = EFI_NOT_FOUND;
		}
	}

	return Exit(imageHandle, status);
}
