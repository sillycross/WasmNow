#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_relational_ops.h"
#include "wasm_binary_ops.h"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

struct FIRelationalOpsImpl
{
    template<typename OperandType,
             WasmRelationalOps operatorType>
    static constexpr bool cond()
    {
        if (std::is_same<OperandType, float>::value ||
            std::is_same<OperandType, double>::value ||
            std::is_same<OperandType, uint32_t>::value ||
            std::is_same<OperandType, uint64_t>::value)
        {
            return true;
        }
        if (operatorType == WasmRelationalOps::LessThan ||
            operatorType == WasmRelationalOps::LessEqual ||
            operatorType == WasmRelationalOps::GreaterThan ||
            operatorType == WasmRelationalOps::GreaterEqual)
        {
            if (std::is_same<OperandType, int32_t>::value ||
                std::is_same<OperandType, int64_t>::value)
            {
                return true;
            }
        }
        return false;
    }

    template<typename OperandType,
             WasmRelationalOps operatorType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             NumInRegisterOperands numInRegisterOperand>
    static constexpr bool cond()
    {
        if (std::is_floating_point<OperandType>::value)
        {
            if (numInRegisterOperand != NumInRegisterOperands::TWO)
            {
                if (!FIOpaqueParamsHelper::IsEmpty(numOFP)) { return false; }
            }
            else
            {
                if (!FIOpaqueParamsHelper::CanPush(numOFP, 2)) { return false; }
            }
        }
        else
        {
            if (FIOpaqueParamsHelper::CanPush(numOFP)) { return false; }
            if (numInRegisterOperand != NumInRegisterOperands::TWO)
            {
                if (!FIOpaqueParamsHelper::IsEmpty(numOIP)) { return false; }
            }
            else
            {
                if (!FIOpaqueParamsHelper::CanPush(numOIP, 2)) { return false; }
            }
        }
        return true;
    }

    template<typename OperandType,
             WasmRelationalOps operatorType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             NumInRegisterOperands numInRegisterOperand,
             bool spillOutput>
    static constexpr bool cond()
    {
        if (!spillOutput && !FIOpaqueParamsHelper::CanPush(numOIP))
        {
            return false;
        }
        return true;
    }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
    template<typename OperandType,
             WasmRelationalOps operatorType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             NumInRegisterOperands numInRegisterOperand,
             bool spillOutput,
             typename... OpaqueParams>
    static void f(uintptr_t stackframe, OpaqueParams... opaqueParams, [[maybe_unused]] OperandType qa1, [[maybe_unused]] OperandType qa2) noexcept
    {
        using SMA = StackMachineAccessor<OperandType /*inputType*/,
                                         2 - static_cast<int>(numInRegisterOperand) /*numOnStack*/,
                                         bool /*outputType*/>;

        OperandType lhs, rhs;
        if constexpr(numInRegisterOperand == NumInRegisterOperands::ZERO)
        {
            lhs = SMA::template GetInput<1>(stackframe);
            rhs = SMA::template GetInput<0>(stackframe);
        }
        else if constexpr(numInRegisterOperand == NumInRegisterOperands::ONE)
        {
            lhs = SMA::template GetInput<0>(stackframe);
            rhs = qa1;
        }
        else
        {
            static_assert(numInRegisterOperand == NumInRegisterOperands::TWO);
            lhs = qa1;
            rhs = qa2;
        }

        bool result;
        if constexpr(operatorType == WasmRelationalOps::Equal)
        {
            result = (lhs == rhs);
        }
        else if constexpr(operatorType == WasmRelationalOps::NotEqual)
        {
            result = (lhs != rhs);
        }
        else if constexpr(operatorType == WasmRelationalOps::LessThan)
        {
            result = (lhs < rhs);
        }
        else if constexpr(operatorType == WasmRelationalOps::LessEqual)
        {
            result = (lhs <= rhs);
        }
        else if constexpr(operatorType == WasmRelationalOps::GreaterThan)
        {
            result = (lhs > rhs);
        }
        else
        {
            static_assert(operatorType == WasmRelationalOps::GreaterEqual);
            result = (lhs >= rhs);
        }

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
#pragma clang diagnostic pop

    static auto metavars()
    {
        return CreateMetaVarList(
                    CreateTypeMetaVar("operandType"),
                    CreateEnumMetaVar<WasmRelationalOps::X_END_OF_ENUM>("operatorType"),
                    CreateOpaqueIntegralParamsLimit(),
                    CreateOpaqueFloatParamsLimit(),
                    CreateEnumMetaVar<NumInRegisterOperands::X_END_OF_ENUM>("numInRegisterOperand"),
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
    RegisterBoilerplate<FIRelationalOpsImpl>();
}
