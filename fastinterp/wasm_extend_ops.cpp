#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

struct FIExtendOpsImpl
{
    template<typename SrcType,
             typename DstType>
    static constexpr bool cond()
    {
        if (std::is_same<DstType, int32_t>::value ||
            std::is_same<DstType, int64_t>::value)
        {
            return std::is_same<SrcType, int8_t>::value ||
                   std::is_same<SrcType, int16_t>::value ||
                   (std::is_same<DstType, int64_t>::value &&
                     std::is_same<SrcType, int32_t>::value);
        }
        else
        {
            return false;
        }
    }

    template<typename SrcType,
             typename DstType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister>
    static constexpr bool cond()
    {
        if (FIOpaqueParamsHelper::CanPush(numOFP)) { return false; }
        if (!isInRegister)
        {
            if (!FIOpaqueParamsHelper::IsEmpty(numOIP)) { return false; }
        }
        else
        {
            if (!FIOpaqueParamsHelper::CanPush(numOIP)) { return false; }
        }
        return true;
    }

    template<typename SrcType,
             typename DstType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister,
             bool spillOutput>
    static constexpr bool cond()
    {
        return true;
    }

    template<typename SrcType,
             typename DstType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister,
             bool spillOutput,
             typename... OpaqueParams>
    static void f(uintptr_t stackframe, OpaqueParams... opaqueParams, [[maybe_unused]] DstType qa1) noexcept
    {
        using SMA = StackMachineAccessor<DstType /*inputType*/,
                                         1 - static_cast<int>(isInRegister) /*numOnStack*/,
                                         DstType /*outputType*/>;

        DstType operand;
        if constexpr(!isInRegister)
        {
            operand = SMA::template GetInput<0>(stackframe);
        }
        else
        {
            operand = qa1;
        }

        DstType result = static_cast<DstType>(static_cast<SrcType>(operand));

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
                    CreateTypeMetaVar("srcType"),
                    CreateTypeMetaVar("dstType"),
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
    RegisterBoilerplate<FIExtendOpsImpl>();
}
