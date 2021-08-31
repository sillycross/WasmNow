#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_binary_ops.h"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

struct FIIntBinaryOpsImpl
{
    template<typename OperandType,
             WasmIntBinaryOps operatorType>
    static constexpr bool cond()
    {
        if (operatorType == WasmIntBinaryOps::Div ||
            operatorType == WasmIntBinaryOps::Rem ||
            operatorType == WasmIntBinaryOps::Shr)
        {
            return std::is_same<OperandType, uint32_t>::value ||
                   std::is_same<OperandType, int32_t>::value ||
                   std::is_same<OperandType, uint64_t>::value ||
                   std::is_same<OperandType, int64_t>::value;
        }
        else
        {
            return std::is_same<OperandType, uint32_t>::value ||
                   std::is_same<OperandType, uint64_t>::value;
        }
    }

    template<typename OperandType,
             WasmIntBinaryOps operatorType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             NumInRegisterOperands numInRegisterOperand>
    static constexpr bool cond()
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
        return true;
    }

    template<typename OperandType,
             WasmIntBinaryOps operatorType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             NumInRegisterOperands numInRegisterOperand,
             bool spillOutput>
    static constexpr bool cond()
    {
        return true;
    }

    template<typename OperandType,
             WasmIntBinaryOps operatorType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             NumInRegisterOperands numInRegisterOperand,
             bool spillOutput,
             typename... OpaqueParams>
    static void f(uintptr_t stackframe, OpaqueParams... opaqueParams, [[maybe_unused]] OperandType qa1, [[maybe_unused]] OperandType qa2) noexcept
    {
        using SMA = StackMachineAccessor<OperandType /*inputType*/,
                                         2 - static_cast<int>(numInRegisterOperand) /*numOnStack*/,
                                         OperandType /*outputType*/>;

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

        OperandType result;
        if constexpr(operatorType == WasmIntBinaryOps::Add)
        {
            result = lhs + rhs;
        }
        else if constexpr(operatorType == WasmIntBinaryOps::Sub)
        {
            result = lhs - rhs;
        }
        else if constexpr(operatorType == WasmIntBinaryOps::Mul)
        {
            result = lhs * rhs;
        }
        else if constexpr(operatorType == WasmIntBinaryOps::Div)
        {
            // TODO: signed overflow?
            result = lhs / rhs;
        }
        else if constexpr(operatorType == WasmIntBinaryOps::Rem)
        {
            result = lhs % rhs;
        }
        else if constexpr(operatorType == WasmIntBinaryOps::And)
        {
            result = lhs & rhs;
        }
        else if constexpr(operatorType == WasmIntBinaryOps::Or)
        {
            result = lhs | rhs;
        }
        else if constexpr(operatorType == WasmIntBinaryOps::Xor)
        {
            result = lhs ^ rhs;
        }
        else if constexpr(operatorType == WasmIntBinaryOps::Shl)
        {
            constexpr OperandType rhs_mask = sizeof(OperandType) * 8 - 1;
            result = lhs << (rhs & rhs_mask);
        }
        else if constexpr(operatorType == WasmIntBinaryOps::Shr)
        {
            constexpr OperandType rhs_mask = sizeof(OperandType) * 8 - 1;
            result = lhs >> (rhs & rhs_mask);
        }
        else if constexpr(operatorType == WasmIntBinaryOps::Rotl)
        {
            constexpr OperandType numDigits = sizeof(OperandType) * 8;
            OperandType r = rhs % numDigits;
            result = (lhs << r) | (lhs >> ((numDigits - r) % numDigits));
        }
        else
        {
            static_assert(operatorType == WasmIntBinaryOps::Rotr);
            constexpr OperandType numDigits = sizeof(OperandType) * 8;
            OperandType r = rhs % numDigits;
            result = (lhs >> r) | (lhs << ((numDigits - r) % numDigits));
        }

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
                    CreateEnumMetaVar<WasmIntBinaryOps::X_END_OF_ENUM>("operatorType"),
                    CreateOpaqueIntegralParamsLimit(),
                    CreateOpaqueFloatParamsLimit(),
                    CreateEnumMetaVar<NumInRegisterOperands::X_END_OF_ENUM>("numInRegisterOperands"),
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
    RegisterBoilerplate<FIIntBinaryOpsImpl>();
}
