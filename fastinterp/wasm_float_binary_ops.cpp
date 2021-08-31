#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_binary_ops.h"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

struct FIFloatBinaryOpsImpl
{
    template<typename OperandType>
    static constexpr bool cond()
    {
        return std::is_same<OperandType, float>::value ||
               std::is_same<OperandType, double>::value;
    }

    template<typename OperandType,
             WasmFloatBinaryOps operatorType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             NumInRegisterOperands numInRegisterOperand>
    static constexpr bool cond()
    {
        if (FIOpaqueParamsHelper::CanPush(numOIP)) { return false; }
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
             WasmFloatBinaryOps operatorType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             NumInRegisterOperands numInRegisterOperand,
             bool spillOutput>
    static constexpr bool cond()
    {
        return true;
    }

    template<typename OperandType,
             WasmFloatBinaryOps operatorType,
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
        if constexpr(operatorType == WasmFloatBinaryOps::Add)
        {
            result = lhs + rhs;
        }
        else if constexpr(operatorType == WasmFloatBinaryOps::Sub)
        {
            result = lhs - rhs;
        }
        else if constexpr(operatorType == WasmFloatBinaryOps::Mul)
        {
            result = lhs * rhs;
        }
        else if constexpr(operatorType == WasmFloatBinaryOps::Div)
        {
            // TODO: signed overflow?
            result = lhs / rhs;
        }
        else if constexpr(operatorType == WasmFloatBinaryOps::Min)
        {
            result = std::min(lhs, rhs);
        }
        else if constexpr(operatorType == WasmFloatBinaryOps::Max)
        {
            result = std::max(lhs, rhs);
        }
        else
        {
            static_assert(operatorType == WasmFloatBinaryOps::CopySign);
            // TODO: ugly workaround for constant table, think about it later..
            //
            if constexpr(std::is_same<OperandType, float>::value)
            {
                uint32_t x1 = cxx2a_bit_cast<uint32_t>(lhs);
                uint32_t y1 = cxx2a_bit_cast<uint32_t>(rhs);
                uint32_t x2 = x1 & 0x7FFFFFFFU;
                uint32_t y2 = y1 & 0x80000000U;
                uint32_t r = x2 | y2;
                result = cxx2a_bit_cast<float>(r);
            }
            else
            {
                static_assert(std::is_same<OperandType, double>::value);
                uint64_t x1 = cxx2a_bit_cast<uint64_t>(lhs);
                uint64_t y1 = cxx2a_bit_cast<uint64_t>(rhs);
                uint64_t x2 = x1 & 0x7FFFFFFFFFFFFFFFULL;
                uint64_t y2 = y1 & 0x8000000000000000ULL;
                uint64_t r = x2 | y2;
                result = cxx2a_bit_cast<double>(r);
            }
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
                    CreateEnumMetaVar<WasmFloatBinaryOps::X_END_OF_ENUM>("operatorType"),
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
    RegisterBoilerplate<FIFloatBinaryOpsImpl>();
}
