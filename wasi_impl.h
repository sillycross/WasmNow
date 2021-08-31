#pragma once

#include "pochivm/common.h"

struct SimpleWasiImpl
{
    static uint32_t fd_prestat_get(uintptr_t);
    static uint32_t fd_prestat_dir_name(uintptr_t);
    static uint32_t environ_sizes_get(uintptr_t);
    static uint32_t environ_get(uintptr_t);
    static uint32_t args_sizes_get(uintptr_t);
    static uint32_t args_get(uintptr_t);
    static uint32_t clock_time_get(uintptr_t);
    static void __attribute__((__noreturn__)) proc_exit(uintptr_t);
    static uint32_t fd_fdstat_get(uintptr_t);
    static uint32_t fd_close(uintptr_t);
    static uint32_t fd_seek(uintptr_t);
    static uint32_t fd_write(uintptr_t);
};

extern std::map< std::pair<std::string, std::string>, uintptr_t> g_wasiLinkMapping;
