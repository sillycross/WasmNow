#pragma once

#include "common.h"
#include "global_codegen_memory_pool.h"
#include "fastinterp/simple_constexpr_power_helper.h"

namespace PochiVM
{

inline GlobalCodegenMemoryPool g_codegenMemoryPool;

class TempArenaAllocator : NonCopyable, NonMovable
{
public:
    TempArenaAllocator()
        : m_listHead(0)
        , m_customSizeListHead(0)
        , m_currentAddress(8)
        , m_currentAddressEnd(0)
    { }

    ~TempArenaAllocator()
    {
        FreeAllMemoryChunks();
    }

    void Reset()
    {
        FreeAllMemoryChunks();
    }

    void* WARN_UNUSED Allocate(size_t alignment, size_t size)
    {
        if (unlikely(size > g_codegenMemoryPool.x_memoryChunkSize - 4096))
        {
            // For large allocations that cannot be supported by the memory pool, directly allocate it.
            //
            return reinterpret_cast<void*>(GetNewMemoryChunkCustomSize(alignment, size));
        }
        else
        {
            AlignCurrentAddress(alignment);
            if (m_currentAddress + size > m_currentAddressEnd)
            {
                GetNewMemoryChunk();
                AlignCurrentAddress(alignment);
                TestAssert(m_currentAddress + size <= m_currentAddressEnd);
            }
            TestAssert(m_currentAddress % alignment == 0);
            uintptr_t result = m_currentAddress;
            m_currentAddress += size;
            TestAssert(m_currentAddress <= m_currentAddressEnd);
            return reinterpret_cast<void*>(result);
        }
    }

private:
    void GetNewMemoryChunk()
    {
        uintptr_t address = g_codegenMemoryPool.GetMemoryChunk();
        AppendToList(address);
        // the first 8 bytes of the region is used as linked list
        //
        m_currentAddress = address + 8;
        m_currentAddressEnd = address + g_codegenMemoryPool.x_memoryChunkSize;
    }

    uintptr_t WARN_UNUSED GetNewMemoryChunkCustomSize(size_t alignment, size_t size)
    {
        size_t headerSize = 16;
        headerSize = std::max(alignment, headerSize);
        size_t allocate_size = size + headerSize;
        allocate_size = (allocate_size + 4095) / 4096 * 4096;

        void* mmapResult = mmap(nullptr, allocate_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
        if (mmapResult == MAP_FAILED)
        {
            ReleaseAssert(false && "Out Of Memory");
        }

        uintptr_t* headerPtr = reinterpret_cast<uintptr_t*>(mmapResult);
        headerPtr[0] = m_customSizeListHead;
        headerPtr[1] = allocate_size;
        m_customSizeListHead = reinterpret_cast<uintptr_t>(mmapResult);
        uintptr_t result = reinterpret_cast<uintptr_t>(mmapResult) + headerSize;
        TestAssert(result % alignment == 0);
        return result;
    }

    void AlignCurrentAddress(size_t alignment)
    {
        TestAssert(alignment <= 4096 && math::is_power_of_2(static_cast<int>(alignment)));
        size_t mask = alignment - 1;
        m_currentAddress += mask;
        m_currentAddress &= ~mask;
    }

    void AppendToList(uintptr_t address)
    {
        *reinterpret_cast<uintptr_t*>(address) = m_listHead;
        m_listHead = address;
    }

    void FreeAllMemoryChunks()
    {
        while (m_listHead != 0)
        {
            uintptr_t next = *reinterpret_cast<uintptr_t*>(m_listHead);
            g_codegenMemoryPool.FreeMemoryChunk(m_listHead);
            m_listHead = next;
        }
        while (m_customSizeListHead != 0)
        {
            uintptr_t next = reinterpret_cast<uintptr_t*>(m_customSizeListHead)[0];
            size_t size = reinterpret_cast<uintptr_t*>(m_customSizeListHead)[1];
            int ret = munmap(reinterpret_cast<void*>(m_customSizeListHead), size);
            if (unlikely(ret != 0))
            {
                int err = errno;
                fprintf(stderr, "[WARNING] [Memory Pool] munmap failed with error %d(%s)\n", err, strerror(err));
            }
            m_customSizeListHead = next;
        }
        m_currentAddress = 8;
        m_currentAddressEnd = 0;
    }

    uintptr_t m_listHead;
    uintptr_t m_customSizeListHead;
    uintptr_t m_currentAddress;
    uintptr_t m_currentAddressEnd;
};

static_assert(math::is_power_of_2(__STDCPP_DEFAULT_NEW_ALIGNMENT__), "std default new alignment is not a power of 2");

}   // namespace PochiVM

inline void* operator new(std::size_t count, PochiVM::TempArenaAllocator& taa)
{
    return taa.Allocate(__STDCPP_DEFAULT_NEW_ALIGNMENT__, count);
}

inline void* operator new[](std::size_t count, PochiVM::TempArenaAllocator& taa)
{
    return taa.Allocate(__STDCPP_DEFAULT_NEW_ALIGNMENT__, count);
}

inline void* operator new(std::size_t count, std::align_val_t al, PochiVM::TempArenaAllocator& taa)
{
    TestAssert(PochiVM::math::is_power_of_2(static_cast<int>(al)));
    return taa.Allocate(static_cast<size_t>(al), count);
}

inline void* operator new[](std::size_t count, std::align_val_t al, PochiVM::TempArenaAllocator& taa)
{
    TestAssert(PochiVM::math::is_power_of_2(static_cast<int>(al)));
    return taa.Allocate(static_cast<size_t>(al), count);
}
