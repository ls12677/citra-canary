// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include "common/logging/log.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/memory.h"
#include "core/hle/kernel/shared_memory.h"
#include "core/memory.h"

namespace Kernel {

SharedMemory::SharedMemory(KernelSystem& kernel) : Object(kernel), kernel(kernel) {}
SharedMemory::~SharedMemory() {
    for (const auto& interval : holding_memory) {
        kernel.GetMemoryRegion(MemoryRegion::SYSTEM)
            ->Free(interval.lower(), interval.upper() - interval.lower());
    }
}

SharedPtr<SharedMemory> KernelSystem::CreateSharedMemory(Process* owner_process, u32 size,
                                                         MemoryPermission permissions,
                                                         MemoryPermission other_permissions,
                                                         VAddr address, MemoryRegion region,
                                                         std::string name) {
    SharedPtr<SharedMemory> shared_memory(new SharedMemory(*this));

    shared_memory->owner_process = owner_process;
    shared_memory->name = std::move(name);
    shared_memory->size = size;
    shared_memory->permissions = permissions;
    shared_memory->other_permissions = other_permissions;

    if (address == 0) {
        // We need to allocate a block from the Linear Heap ourselves.
        // We'll manually allocate some memory from the linear heap in the specified region.
        MemoryRegionInfo* memory_region = GetMemoryRegion(region);
        auto offset = memory_region->LinearAllocate(size);

        ASSERT_MSG(offset, "Not enough space in region to allocate shared memory!");

        shared_memory->backing_blocks = {{Memory::fcram.data() + *offset, size}};
        shared_memory->holding_memory += MemoryRegionInfo::Interval(*offset, *offset + size);
        shared_memory->linear_heap_phys_address = Memory::FCRAM_PADDR + *offset;

        // Increase the amount of used linear heap memory for the owner process.
        if (shared_memory->owner_process != nullptr) {
            shared_memory->owner_process->linear_heap_used += size;
        }
    } else {
        auto& vm_manager = shared_memory->owner_process->vm_manager;
        // The memory is already available and mapped in the owner process.
        // Iterate through VMA and record the backing blocks
        VAddr interval_target = address;
        while (interval_target != address + size) {
            auto vma = vm_manager.FindVMA(interval_target);
            ASSERT_MSG(vma->second.type == VMAType::BackingMemory, "Trying to share freed memory");

            VAddr interval_end = std::min(address + size, vma->second.base + vma->second.size);
            u32 interval_size = interval_end - interval_target;
            u8* backing_memory = vma->second.backing_memory + (interval_target - vma->second.base);
            shared_memory->backing_blocks.push_back({backing_memory, interval_size});

            interval_target += interval_size;
        }
    }

    shared_memory->base_address = address;
    return shared_memory;
}

SharedPtr<SharedMemory> KernelSystem::CreateSharedMemoryForApplet(
    u32 offset, u32 size, MemoryPermission permissions, MemoryPermission other_permissions,
    std::string name) {
    SharedPtr<SharedMemory> shared_memory(new SharedMemory(*this));

    // Allocate memory in heap
    MemoryRegionInfo* memory_region = GetMemoryRegion(MemoryRegion::SYSTEM);
    auto backing_blocks = memory_region->HeapAllocate(size);
    ASSERT_MSG(!backing_blocks.empty(), "Not enough space in region to allocate shared memory!");
    shared_memory->holding_memory = backing_blocks;
    shared_memory->owner_process = nullptr;
    shared_memory->name = std::move(name);
    shared_memory->size = size;
    shared_memory->permissions = permissions;
    shared_memory->other_permissions = other_permissions;
    for (const auto& interval : backing_blocks) {
        shared_memory->backing_blocks.push_back(
            {Memory::fcram.data() + interval.lower(), interval.upper() - interval.lower()});
    }
    shared_memory->base_address = Memory::HEAP_VADDR + offset;

    return shared_memory;
}

ResultCode SharedMemory::Map(Process* target_process, VAddr address, MemoryPermission permissions,
                             MemoryPermission other_permissions) {

    MemoryPermission own_other_permissions =
        target_process == owner_process ? this->permissions : this->other_permissions;

    // Automatically allocated memory blocks can only be mapped with other_permissions = DontCare
    if (base_address == 0 && other_permissions != MemoryPermission::DontCare) {
        return ERR_INVALID_COMBINATION;
    }

    // Error out if the requested permissions don't match what the creator process allows.
    if (static_cast<u32>(permissions) & ~static_cast<u32>(own_other_permissions)) {
        LOG_ERROR(Kernel, "cannot map id={}, address=0x{:08X} name={}, permissions don't match",
                  GetObjectId(), address, name);
        return ERR_INVALID_COMBINATION;
    }

    // Heap-backed memory blocks can not be mapped with other_permissions = DontCare
    if (base_address != 0 && other_permissions == MemoryPermission::DontCare) {
        LOG_ERROR(Kernel, "cannot map id={}, address=0x{08X} name={}, permissions don't match",
                  GetObjectId(), address, name);
        return ERR_INVALID_COMBINATION;
    }

    // Error out if the provided permissions are not compatible with what the creator process needs.
    if (other_permissions != MemoryPermission::DontCare &&
        static_cast<u32>(this->permissions) & ~static_cast<u32>(other_permissions)) {
        LOG_ERROR(Kernel, "cannot map id={}, address=0x{:08X} name={}, permissions don't match",
                  GetObjectId(), address, name);
        return ERR_WRONG_PERMISSION;
    }

    // TODO(Subv): Check for the Shared Device Mem flag in the creator process.
    /*if (was_created_with_shared_device_mem && address != 0) {
        return ResultCode(ErrorDescription::InvalidCombination, ErrorModule::OS,
    ErrorSummary::InvalidArgument, ErrorLevel::Usage);
    }*/

    // TODO(Subv): The same process that created a SharedMemory object
    // can not map it in its own address space unless it was created with addr=0, result 0xD900182C.

    if (address != 0) {
        if (address < Memory::HEAP_VADDR || address + size >= Memory::SHARED_MEMORY_VADDR_END) {
            LOG_ERROR(Kernel, "cannot map id={}, address=0x{:08X} name={}, invalid address",
                      GetObjectId(), address, name);
            return ERR_INVALID_ADDRESS;
        }
    }

    VAddr target_address = address;

    if (base_address == 0 && target_address == 0) {
        // Calculate the address at which to map the memory block.
        target_address = linear_heap_phys_address - Memory::FCRAM_PADDR +
                         target_process->GetLinearHeapAreaAddress();
    }

    auto vma = target_process->vm_manager.FindVMA(target_address);
    if (vma->second.type != VMAType::Free ||
        vma->second.base + vma->second.size < target_address + size) {
        LOG_ERROR(Kernel, "Trying to map to already allocated memory");
        return ERR_INVALID_ADDRESS_STATE;
    }

    // Map the memory block into the target process
    VAddr interval_target = target_address;
    for (const auto& interval : backing_blocks) {
        auto vma = target_process->vm_manager.MapBackingMemory(
            interval_target, interval.first, interval.second, MemoryState::Shared);
        ASSERT(vma.Succeeded());
        target_process->vm_manager.Reprotect(vma.Unwrap(), ConvertPermissions(permissions));
        interval_target += interval.second;
    }

    return RESULT_SUCCESS;
}

ResultCode SharedMemory::Unmap(Process* target_process, VAddr address) {
    // TODO(Subv): Verify what happens if the application tries to unmap an address that is not
    // mapped to a SharedMemory.
    return target_process->vm_manager.UnmapRange(address, size);
}

VMAPermission SharedMemory::ConvertPermissions(MemoryPermission permission) {
    u32 masked_permissions =
        static_cast<u32>(permission) & static_cast<u32>(MemoryPermission::ReadWriteExecute);
    return static_cast<VMAPermission>(masked_permissions);
};

u8* SharedMemory::GetPointer(u32 offset) {
    if (backing_blocks.size() != 1) {
        LOG_WARNING(Kernel, "Unsafe GetPointer on discontinuous SharedMemory");
    }
    return backing_blocks[0].first + offset;
}

} // namespace Kernel
