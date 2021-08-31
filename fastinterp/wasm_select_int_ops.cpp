#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_binary_ops.h"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

struct FISelectIntImpl
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
             TrinaryOpNumInRegisterOperands numInRegisterOperand,
             bool spillOutput>
    static constexpr bool cond()
    {
        if (FIOpaqueParamsHelper::CanPush(numOFP)) { return false; }
        if (numInRegisterOperand != TrinaryOpNumInRegisterOperands::THREE)
        {
            if (!FIOpaqueParamsHelper::IsEmpty(numOIP)) { return false; }
        }
        else
        {
            if (!FIOpaqueParamsHelper::CanPush(numOIP, 3)) { return false; }
        }
        return true;
    }

    template<typename OperandType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             TrinaryOpNumInRegisterOperands numInRegisterOperand,
             bool spillOutput,
             typename... OpaqueParams>
    static void f(uintptr_t stackframe,
                  OpaqueParams... opaqueParams,
                  [[maybe_unused]] std::conditional_t<numInRegisterOperand == TrinaryOpNumInRegisterOperands::ONE, uint32_t, OperandType> qa1,
                  [[maybe_unused]] std::conditional_t<numInRegisterOperand == TrinaryOpNumInRegisterOperands::TWO, uint32_t, OperandType> qa2,
                  [[maybe_unused]] std::conditional_t<numInRegisterOperand == TrinaryOpNumInRegisterOperands::THREE, uint32_t, OperandType> qa3) noexcept
    {
        OperandType val1, val2;
        uint32_t selector;
        if constexpr(numInRegisterOperand == TrinaryOpNumInRegisterOperands::ZERO)
        {
            val1 = *internal::GetStack3rdTop<OperandType>(stackframe);
            val2 = *internal::GetStack2ndTop<OperandType>(stackframe);
            selector = *internal::GetStackTop<uint32_t>(stackframe);
        }
        else if constexpr(numInRegisterOperand == TrinaryOpNumInRegisterOperands::ONE)
        {
            using SMA = StackMachineAccessor<OperandType /*inputType*/,
                                             3 - static_cast<int>(numInRegisterOperand) /*numOnStack*/,
                                             OperandType /*outputType*/>;

            val1 = SMA::template GetInput<1>(stackframe);
            val2 = SMA::template GetInput<0>(stackframe);
            selector = qa1;
        }
        else if constexpr(numInRegisterOperand == TrinaryOpNumInRegisterOperands::TWO)
        {
            using SMA = StackMachineAccessor<OperandType /*inputType*/,
                                             3 - static_cast<int>(numInRegisterOperand) /*numOnStack*/,
                                             OperandType /*outputType*/>;

            val1 = SMA::template GetInput<0>(stackframe);
            val2 = qa1;
            selector = qa2;
        }
        else
        {
            static_assert(numInRegisterOperand == TrinaryOpNumInRegisterOperands::THREE);
            val1 = qa1;
            val2 = qa2;
            selector = qa3;
        }

        OperandType result = (selector != 0) ? val1 : val2;

        if constexpr(!spillOutput)
        {
            DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, OpaqueParams..., OperandType) noexcept);
            BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe, opaqueParams..., result);
        }
        else
        {
            if constexpr(numInRegisterOperand == TrinaryOpNumInRegisterOperands::ZERO)
            {
                *internal::GetStack3rdTop<OperandType>(stackframe) = result;
            }
            else
            {
                using SMA = StackMachineAccessor<OperandType /*inputType*/,
                                                 3 - static_cast<int>(numInRegisterOperand) /*numOnStack*/,
                                                 OperandType /*outputType*/>;
                *SMA::GetOutputLoc(stackframe) = result;
            }

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
                    CreateEnumMetaVar<TrinaryOpNumInRegisterOperands::X_END_OF_ENUM>("numInRegisterOperands"),
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
    RegisterBoilerplate<FISelectIntImpl>();
}
