#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_memory_ptr.h"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

struct FIMemoryLoadOpsImpl
{
    template<typename DstType,
             typename SrcType>
    static constexpr bool cond()
    {
        if (!(std::is_same<DstType, uint32_t>::value ||
              std::is_same<DstType, uint64_t>::value ||
              std::is_same<DstType, float>::value ||
              std::is_same<DstType, double>::value))
        {
            return false;
        }
        if (std::is_floating_point<DstType>::value)
        {
            return std::is_same<SrcType, DstType>::value;
        }
        else
        {
            return std::is_same<SrcType, uint8_t>::value ||
                   std::is_same<SrcType, int8_t>::value ||
                   std::is_same<SrcType, uint16_t>::value ||
                   std::is_same<SrcType, int16_t>::value ||
                   std::is_same<SrcType, uint32_t>::value ||
                   (std::is_same<DstType, uint64_t>::value &&
                    (std::is_same<SrcType, int32_t>::value ||
                     std::is_same<SrcType, uint64_t>::value));
        }
    }

    template<typename DstType,
             typename SrcType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister>
    static constexpr bool cond()
    {
        if (!std::is_floating_point<DstType>::value)
        {
            if (FIOpaqueParamsHelper::CanPush(numOFP)) { return false; }
        }
        if (!isInRegister)
        {
            if (!FIOpaqueParamsHelper::IsEmpty(numOIP)) { return false; }
        }
        else
        {
            if (!FIOpaqueParamsHelper::CanPush(numOIP, 1)) { return false; }
        }
        return true;
    }

    template<typename DstType,
             typename SrcType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister,
             bool spillOutput>
    static constexpr bool cond()
    {
        if (std::is_floating_point<DstType>::value)
        {
            if (!spillOutput && !FIOpaqueParamsHelper::CanPush(numOFP))
            {
                return false;
            }
        }
        else
        {
            if (!spillOutput && !FIOpaqueParamsHelper::CanPush(numOIP))
            {
                return false;
            }
        }
        return true;
    }

    template<typename DstType,
             typename SrcType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister,
             bool spillOutput,
             typename... OpaqueParams>
    static void f(uintptr_t stackframe, OpaqueParams... opaqueParams, [[maybe_unused]] uint32_t qa1) noexcept
    {
        using SMA = StackMachineAccessor<uint32_t /*inputType*/,
                                         1 - static_cast<int>(isInRegister) /*numOnStack*/,
                                         DstType /*outputType*/>;

        uint32_t var_offset;
        if constexpr(!isInRegister)
        {
            var_offset = SMA::template GetInput<0>(stackframe);
        }
        else
        {
            var_offset = qa1;
        }

        uint64_t final_offset;
        {
            // TODO: this only support up to 0x7fffffff
            //
            DEFINE_INDEX_CONSTANT_PLACEHOLDER_2;
            final_offset = static_cast<uint64_t>(var_offset) + CONSTANT_PLACEHOLDER_2;
        }

        WasmMemPtr<SrcType> ptr = reinterpret_cast<WasmMemPtr<SrcType>>(final_offset);
        SrcType value = *ptr;

        DstType result;
        if constexpr(std::is_floating_point<SrcType>::value)
        {
            static_assert(std::is_same<DstType, SrcType>::value);
            result = value;
        }
        else
        {
            static_assert(sizeof(DstType) >= sizeof(SrcType) && !std::is_floating_point<DstType>::value);
            if constexpr(std::is_signed<SrcType>::value)
            {
                using SignExtendType = typename std::make_signed<DstType>::type;
                result = static_cast<DstType>(static_cast<SignExtendType>(value));
            }
            else
            {
                result = static_cast<DstType>(value);
            }
        }

        if constexpr(!spillOutput)
        {
            DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, OpaqueParams..., DstType) noexcept);
            BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe, opaqueParams..., result);
        }
        else
        {
            *SMA::GetOutputLoc(stackframe) = result;

            DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, OpaqueParams...) noexcept);
            BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe, opaqueParams...);
        }
    }

    static auto metavars()
    {
        return CreateMetaVarList(
                    CreateTypeMetaVar("dstType"),
                    CreateTypeMetaVar("srcType"),
                    CreateOpaqueIntegralParamsLimit(),
                    CreateOpaqueFloatParamsLimit(),
                    CreateBoolMetaVar("isInRegister"),
                    CreateBoolMetaVar("spillOutput")
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
    RegisterBoilerplate<FIMemoryLoadOpsImpl>();
}
