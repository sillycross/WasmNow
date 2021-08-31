#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

struct FITestEqzOpsImpl
{
    template<typename OperandType>
    static constexpr bool cond()
    {
        return std::is_same<OperandType, uint32_t>::value ||
               std::is_same<OperandType, uint64_t>::value;
    }

    template<typename OperandType,
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
            if (!FIOpaqueParamsHelper::CanPush(numOIP, 1)) { return false; }
        }
        return true;
    }

    template<typename OperandType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister,
             bool spillOutput>
    static constexpr bool cond()
    {
        if (!spillOutput && !FIOpaqueParamsHelper::CanPush(numOIP, 1))
        {
            return false;
        }
        return true;
    }

    template<typename OperandType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister,
             bool spillOutput,
             typename... OpaqueParams>
    static void f(uintptr_t stackframe, OpaqueParams... opaqueParams, [[maybe_unused]] OperandType qa1) noexcept
    {
        using SMA = StackMachineAccessor<OperandType /*inputType*/,
                                         1 - static_cast<int>(isInRegister) /*numOnStack*/,
                                         bool /*outputType*/>;

        OperandType operand;
        if constexpr(!isInRegister)
        {
            operand = SMA::template GetInput<0>(stackframe);
        }
        else
        {
            operand = qa1;
        }

        bool result = (operand == 0);

        if constexpr(!spillOutput)
        {
            DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, OpaqueParams..., bool) noexcept);
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
    RegisterBoilerplate<FITestEqzOpsImpl>();
}
