#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_memory_ptr.h"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

// the case that memory offset has not been spilled
//
struct FIMemoryStoreOpsNotSpilledImpl
{
    template<typename DstType,
             typename SrcType>
    static constexpr bool cond()
    {
        if (!(std::is_same<DstType, uint8_t>::value ||
              std::is_same<DstType, uint16_t>::value ||
              std::is_same<DstType, uint32_t>::value ||
              std::is_same<DstType, uint64_t>::value ||
              std::is_same<DstType, float>::value ||
              std::is_same<DstType, double>::value))
        {
            return false;
        }
        if (!(std::is_same<SrcType, uint32_t>::value ||
              std::is_same<SrcType, uint64_t>::value ||
              std::is_same<SrcType, float>::value ||
              std::is_same<SrcType, double>::value))
        {
            return false;
        }
        if (std::is_floating_point<DstType>::value)
        {
            return std::is_same<SrcType, DstType>::value;
        }
        else
        {
            return !std::is_floating_point<SrcType>::value &&
                   !(std::is_same<SrcType, uint32_t>::value && std::is_same<DstType, uint64_t>::value);
        }
    }

    template<typename DstType,
             typename SrcType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister>
    static constexpr bool cond()
    {
        if (!isInRegister)
        {
            // the offset param is below data param in stack,
            // so if data is also integral, it's impossible that offset param is spilled but data is not
            //
            if (!std::is_floating_point<SrcType>::value) { return false; }
        }
        if (!std::is_floating_point<SrcType>::value)
        {
            if (FIOpaqueParamsHelper::CanPush(numOFP)) { return false; }
            if (!FIOpaqueParamsHelper::CanPush(numOIP, 2)) { return false; }
        }
        else
        {
            if (!FIOpaqueParamsHelper::CanPush(numOIP)) { return false; }
            if (!isInRegister)
            {
                if (!FIOpaqueParamsHelper::IsEmpty(numOFP)) { return false; }
            }
            else
            {
                if (!FIOpaqueParamsHelper::CanPush(numOFP)) { return false; }
            }
        }
        return true;
    }

    template<typename DstType,
             typename SrcType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister,
             typename... OpaqueParams>
    static void f(uintptr_t stackframe, OpaqueParams... opaqueParams, uint32_t var_offset, [[maybe_unused]] SrcType qa1) noexcept
    {
        SrcType operand;
        if constexpr(!isInRegister)
        {
            static_assert(std::is_floating_point<SrcType>::value);
            operand = *internal::GetStackTop<SrcType>(stackframe);
        }
        else
        {
            operand = qa1;
        }

        uint64_t final_offset;
        {
            // TODO: this only support up to 0x7fffffff
            //
            DEFINE_INDEX_CONSTANT_PLACEHOLDER_2;
            final_offset = static_cast<uint64_t>(var_offset) + CONSTANT_PLACEHOLDER_2;
        }

        WasmMemPtr<DstType> ptr = reinterpret_cast<WasmMemPtr<DstType>>(final_offset);
        DstType storeValue = static_cast<DstType>(operand);

        *ptr = storeValue;

        DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, OpaqueParams...) noexcept);
        BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe, opaqueParams...);
    }

    static auto metavars()
    {
        return CreateMetaVarList(
                    CreateTypeMetaVar("dstType"),
                    CreateTypeMetaVar("srcType"),
                    CreateOpaqueIntegralParamsLimit(),
                    CreateOpaqueFloatParamsLimit(),
                    CreateBoolMetaVar("isInRegister")
        );
    }
};

}   // namespace PochiVM

// build_fast_interp_lib.cpp JIT entry point
//
extern "C"
void __pochivm_build_fast_interp_library__()
{
    using namespace PochiVM;
    RegisterBoilerplate<FIMemoryStoreOpsNotSpilledImpl>();
}
