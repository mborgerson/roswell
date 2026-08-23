#pragma once

#ifdef SARCH_XBOX
#define KERNEL_MODULE_NAME      L"xboxkrnl.exe"
#else
#define KERNEL_MODULE_NAME      L"ntoskrnl.exe"
#endif
#define HAL_MODULE_NAME         L"hal.dll"
#define DRIVER_ROOT_NAME        L"\\Driver\\"
#define FILESYSTEM_ROOT_NAME    L"\\FileSystem\\"
