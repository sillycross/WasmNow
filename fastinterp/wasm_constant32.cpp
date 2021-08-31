#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

struct FIConstant32Impl
{
    template<typename OperandType>
    static constexpr bool cond()
    {
        return std::is_same<OperandType, int32_t>::value ||
               std::is_same<OperandType, float>::value;
    }

    template<typename OperandType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool spillOutput>
    static constexpr bool cond()
    {
        if (std::is_floating_point<OperandType>::value)
        {
            if (FIOpaqueParamsHelper::CanPush(numOIP)) { return false; }
            if (spillOutput && !FIOpaqueParamsHelper::IsEmpty(numOFP)) { return false; }
        }
        else
        {
            if (FIOpaqueParamsHelper::CanPush(numOFP)) { return false; }
            if (spillOutput && !FIOpaqueParamsHelper::IsEmpty(numOIP)) { return false; }
        }
        return true;
    }

    template<typename OperandType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool spillOutput,
             typename... OpaqueParams>
    static void f(uintptr_t stackframe, OpaqueParams... opaqueParams) noexcept
    {
        using SMA = StackMachineAccessor<OperandType /*inputType*/,
                                         0 /*numOnStack*/,
                                         OperandType /*outputType*/>;

        DEFINE_CONSTANT_PLACEHOLDER_2(OperandType);
        OperandType result = CONSTANT_PLACEHOLDER_2;

        if constexpr(!spillOutput)
        {
            DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, OpaqueParams..., OperandType) noexcept);
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
                    CreateTypeMetaVar("operandType"),
                    CreateOpaqueIntegralParamsLimit(),
                    CreateOpaqueFloatParamsLimit(),
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
    RegisterBoilerplate<FIConstant32Impl>();
}
