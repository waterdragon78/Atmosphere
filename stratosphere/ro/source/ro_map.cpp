/*
 * Copyright (c) 2018-2019 Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <switch.h>
#include <cstdio>

#include "ro_map.hpp"

bool MapUtils::CanAddGuardRegions(Handle process_handle, u64 address, u64 size) {
    MemoryInfo mem_info;
    u32 page_info;

    /* Nintendo doesn't validate SVC return values at all. */
    /* TODO: Should we allow these to fail? */
    R_ASSERT(svcQueryProcessMemory(&mem_info, &page_info, process_handle, address - 1));
    if (mem_info.type == MemType_Unmapped && address - GuardRegionSize >= mem_info.addr) {
        R_ASSERT(svcQueryProcessMemory(&mem_info, &page_info, process_handle, address + size));
        return mem_info.type == MemType_Unmapped && address + size + GuardRegionSize <= mem_info.addr + mem_info.size;
    }

    return false;
}

Result MapUtils::LocateSpaceForMap(u64 *out, u64 out_size) {
    if (GetRuntimeFirmwareVersion() >= FirmwareVersion_200) {
        return LocateSpaceForMapModern(out, out_size);
    } else {
        return LocateSpaceForMapDeprecated(out, out_size);
    }
}

Result MapUtils::MapCodeMemoryForProcess(MappedCodeMemory &out_mcm, Handle process_handle, u64 base_address, u64 size) {
    if (GetRuntimeFirmwareVersion() >= FirmwareVersion_200) {
        return MapCodeMemoryForProcessModern(out_mcm, process_handle, base_address, size);
    } else {
        if (R_FAILED(MapCodeMemoryForProcessDeprecated(out_mcm, process_handle, true, base_address, size))) {
            R_TRY(MapCodeMemoryForProcessDeprecated(out_mcm, process_handle, false, base_address, size));
        }
        return ResultSuccess;
    }
}

Result MapUtils::LocateSpaceForMapModern(u64 *out, u64 out_size) {
    MemoryInfo mem_info = {};
    AddressSpaceInfo address_space = {};
    u32 page_info = 0;
    u64 cur_base = 0, cur_end = 0;

    R_TRY(GetAddressSpaceInfo(&address_space, CUR_PROCESS_HANDLE));

    cur_base = address_space.addspace_base;

    cur_end = cur_base + out_size;
    if (cur_end <= cur_base) {
        return ResultKernelOutOfMemory;
    }

    while (true) {
        if (address_space.heap_size && (address_space.heap_base <= cur_end - 1 && cur_base <= address_space.heap_end - 1)) {
            /* If we overlap the heap region, go to the end of the heap region. */
            if (cur_base == address_space.heap_end) {
                return ResultKernelOutOfMemory;
            }
            cur_base = address_space.heap_end;
        } else if (address_space.map_size && (address_space.map_base <= cur_end - 1 && cur_base <= address_space.map_end - 1)) {
            /* If we overlap the map region, go to the end of the map region. */
            if (cur_base == address_space.map_end) {
                return ResultKernelOutOfMemory;
            }
            cur_base = address_space.map_end;
        } else {
            R_ASSERT(svcQueryMemory(&mem_info, &page_info, cur_base));
            if (mem_info.type == 0 && mem_info.addr - cur_base + mem_info.size >= out_size) {
                *out = cur_base;
                return ResultSuccess;
            }
            if (mem_info.addr + mem_info.size <= cur_base) {
                return ResultKernelOutOfMemory;
            }
            cur_base = mem_info.addr + mem_info.size;
            if (cur_base >= address_space.addspace_end) {
                return ResultKernelOutOfMemory;
            }
        }
        cur_end = cur_base + out_size;
        if (cur_base + out_size <= cur_base) {
            return ResultKernelOutOfMemory;
        }
    }
}


Result MapUtils::LocateSpaceForMapDeprecated(u64 *out, u64 out_size) {
    MemoryInfo mem_info = {};
    u32 page_info = 0;

    u64 cur_base = 0x8000000ULL;
    do {
        R_TRY(svcQueryMemory(&mem_info, &page_info, cur_base));

        if (mem_info.type == 0 && mem_info.addr - cur_base + mem_info.size >= out_size) {
            *out = cur_base;
            return ResultSuccess;
        }

        const u64 mem_end = mem_info.addr + mem_info.size;
        if (mem_info.type == 0x10 || mem_end < cur_base || (mem_end >> 31)) {
            return ResultKernelOutOfMemory;
        }

        cur_base = mem_end;
    } while (true);
}

Result MapUtils::MapCodeMemoryForProcessModern(MappedCodeMemory &out_mcm, Handle process_handle, u64 base_address, u64 size) {
    AddressSpaceInfo address_space = {};

    R_TRY(GetAddressSpaceInfo(&address_space, process_handle));

    if (size > address_space.addspace_size) {
        return ResultRoInsufficientAddressSpace;
    }

    u64 try_address;
    for (unsigned int i = 0; i < LocateRetryCount; i++) {
        while (true) {
            try_address = address_space.addspace_base + (StratosphereRandomUtils::GetRandomU64((u64)(address_space.addspace_size - size) >> 12) << 12);
            if (address_space.heap_size && (address_space.heap_base <= try_address + size - 1 && try_address <= address_space.heap_end - 1)) {
                continue;
            }
            if (address_space.map_size && (address_space.map_base <= try_address + size - 1 && try_address <= address_space.map_end - 1)) {
                continue;
            }
            break;
        }
        MappedCodeMemory tmp_mcm(process_handle, try_address, base_address, size);

        R_TRY_CATCH(tmp_mcm.GetResult()) {
            R_CATCH(ResultKernelInvalidMemoryState) {
                continue;
            }
        } R_END_TRY_CATCH;

        if (!CanAddGuardRegions(process_handle, try_address, size)) {
            continue;
        }

        /* We're done searching. */
        out_mcm = std::move(tmp_mcm);
        return ResultSuccess;
    }

    return ResultRoInsufficientAddressSpace;
}

Result MapUtils::MapCodeMemoryForProcessDeprecated(MappedCodeMemory &out_mcm, Handle process_handle, bool is_64_bit, u64 base_address, u64 size) {
    u64 addspace_base, addspace_size;
    if (is_64_bit) {
        addspace_base = 0x8000000ULL;
        addspace_size = 0x78000000ULL;
    } else {
        addspace_base = 0x200000ULL;
        addspace_size = 0x3FE0000ULL;
    }

    if (size > addspace_size) {
        return ResultRoInsufficientAddressSpace;
    }

    u64 try_address;
    for (unsigned int i = 0; i < LocateRetryCount; i++) {
        try_address = addspace_base + (StratosphereRandomUtils::GetRandomU64((u64)(addspace_size - size) >> 12) << 12);

        MappedCodeMemory tmp_mcm(process_handle, try_address, base_address, size);

        R_TRY_CATCH(tmp_mcm.GetResult()) {
            R_CATCH(ResultKernelInvalidMemoryState) {
                continue;
            }
        } R_END_TRY_CATCH;

        if (!CanAddGuardRegions(process_handle, try_address, size)) {
            continue;
        }

        /* We're done searching. */
        out_mcm = std::move(tmp_mcm);
        return ResultSuccess;
    }

    return ResultRoInsufficientAddressSpace;
}

Result MapUtils::GetAddressSpaceInfo(AddressSpaceInfo *out, Handle process_h) {
    R_TRY(svcGetInfo(&out->heap_base,      4, process_h, 0));
    R_TRY(svcGetInfo(&out->heap_size,      5, process_h, 0));
    R_TRY(svcGetInfo(&out->map_base,       2, process_h, 0));
    R_TRY(svcGetInfo(&out->map_size,       3, process_h, 0));
    R_TRY(svcGetInfo(&out->addspace_base, 12, process_h, 0));
    R_TRY(svcGetInfo(&out->addspace_size, 13, process_h, 0));

    out->heap_end = out->heap_base + out->heap_size;
    out->map_end = out->map_base + out->map_size;
    out->addspace_end = out->addspace_base + out->addspace_size;
    return ResultSuccess;
}
