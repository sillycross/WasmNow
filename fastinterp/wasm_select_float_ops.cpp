#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_binary_ops.h"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

struct FISelectFloatImpl
{
    template<typename OperandType>
    static constexpr bool cond()
    {
        return std::is_same<OperandType, float>::value ||
               std::is_same<OperandType, double>::value;
    }

    template<typename OperandType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             NumInRegisterOperands numInRegisterOperand>
    static constexpr bool cond()
    {
        if (numInRegisterOperand != NumInRegisterOperands::TWO)
        {
            if (!FIOpaqueParamsHelper::IsEmpty(numOFP)) { return false; }
        }
        else
        {
            if (!FIOpaqueParamsHelper::CanPush(numOFP, 2)) { return false; }
        }
        return true;
    }

    template<typename OperandType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             NumInRegisterOperands numInRegisterOperand,
             bool isSelectorSpilled,
             bool spillOutput>
    static constexpr bool cond()
    {
        if (isSelectorSpilled)
        {
            if (!FIOpaqueParamsHelper::IsEmpty(numOIP)) { return false; }
        }
        else
        {
            if (!FIOpaqueParamsHelper::CanPush(numOIP)) { return false; }
        }
        return true;
    }

    template<typename OperandType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             NumInRegisterOperands numInRegisterOperand,
             bool isSelectorSpilled,
             bool spillOutput,
             typename... OpaqueParams>
    static void f(uintptr_t stackframe, OpaqueParams... opaqueParams, [[maybe_unused]] OperandType qa1, [[maybe_unused]] OperandType qa2, [[maybe_unused]] uint32_t i32) noexcept
    {
        using SMA = StackMachineAccessor<OperandType /*inputType*/,
                                         2 - static_cast<int>(numInRegisterOperand) /*numOnStack*/,
                                         OperandType /*outputType*/>;

        OperandType val1, val2;
        if constexpr(numInRegisterOperand == NumInRegisterOperands::ZERO)
        {
            val1 = SMA::template GetInput<1>(stackframe);
            val2 = SMA::template GetInput<0>(stackframe);
        }
        else if constexpr(numInRegisterOperand == NumInRegisterOperands::ONE)
        {
            val1 = SMA::template GetInput<0>(stackframe);
            val2 = qa1;
        }
        else
        {
            static_assert(numInRegisterOperand == NumInRegisterOperands::TWO);
            val1 = qa1;
            val2 = qa2;
        }

        uint32_t selector;
        if constexpr(isSelectorSpilled)
        {
            selector = *internal::GetStackTop<uint32_t>(stackframe);
        }
        else
        {
            selector = i32;
        }

        OperandType result = (selector != 0) ? val1 : val2;

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
                    CreateEnumMetaVar<NumInRegisterOperands::X_END_OF_ENUM>("numInRegisterOperands"),
                    CreateBoolMetaVar("isSelectorSpilled"),
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
    RegisterBoilerplate<FISelectFloatImpl>();
}
