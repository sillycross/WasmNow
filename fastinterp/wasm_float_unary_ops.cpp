#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_unary_ops.h"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

struct FIFloatUnaryOpsImpl
{
    template<typename OperandType>
    static constexpr bool cond()
    {
        return std::is_same<OperandType, float>::value ||
               std::is_same<OperandType, double>::value;
    }

    template<typename OperandType,
             WasmFloatUnaryOps operatorType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister>
    static constexpr bool cond()
    {
        if (FIOpaqueParamsHelper::CanPush(numOIP)) { return false; }
        if (!isInRegister)
        {
            if (!FIOpaqueParamsHelper::IsEmpty(numOFP)) { return false; }
        }
        else
        {
            if (!FIOpaqueParamsHelper::CanPush(numOFP, 1)) { return false; }
        }
        return true;
    }

    template<typename OperandType,
             WasmFloatUnaryOps operatorType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister,
             bool spillOutput>
    static constexpr bool cond()
    {
        return true;
    }

    template<typename OperandType,
             WasmFloatUnaryOps operatorType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister,
             bool spillOutput,
             typename... OpaqueParams>
    static void f(uintptr_t stackframe, OpaqueParams... opaqueParams, [[maybe_unused]] OperandType qa1) noexcept
    {
        using SMA = StackMachineAccessor<OperandType /*inputType*/,
                                         1 - static_cast<int>(isInRegister) /*numOnStack*/,
                                         OperandType /*outputType*/>;

        OperandType operand;
        if constexpr(!isInRegister)
        {
            operand = SMA::template GetInput<0>(stackframe);
        }
        else
        {
            operand = qa1;
        }

        OperandType result;
        if constexpr(operatorType == WasmFloatUnaryOps::Abs)
        {
            // TODO: ugly workaround for constant table, fix it later..
            //
            if constexpr(std::is_same<OperandType, float>::value)
            {
                uint32_t y = cxx2a_bit_cast<uint32_t>(operand);
                y &= 0x7fffffffU;
                volatile uint32_t z;
                z = y;
                uint32_t w = z;
                result = cxx2a_bit_cast<float>(w);
            }
            else
            {
                static_assert(std::is_same<OperandType, double>::value);
                uint64_t y = cxx2a_bit_cast<uint64_t>(operand);
                y &= 0x7fffffffffffffffULL;
                volatile uint64_t z;
                z = y;
                uint64_t w = z;
                result = cxx2a_bit_cast<double>(w);
            }
        }
        else if constexpr(operatorType == WasmFloatUnaryOps::Neg)
        {
            // TODO: ugly workaround for constant table, fix it later..
            //
            if constexpr(std::is_same<OperandType, float>::value)
            {
                uint32_t y = cxx2a_bit_cast<uint32_t>(operand);
                y ^= 0x80000000U;
                volatile uint32_t z;
                z = y;
                uint32_t w = z;
                result = cxx2a_bit_cast<float>(w);
            }
            else
            {
                static_assert(std::is_same<OperandType, double>::value);
                uint64_t y = cxx2a_bit_cast<uint64_t>(operand);
                y ^= 0x8000000000000000ULL;
                volatile uint64_t z;
                z = y;
                uint64_t w = z;
                result = cxx2a_bit_cast<double>(w);
            }
        }
        else if constexpr(operatorType == WasmFloatUnaryOps::Sqrt)
        {
            if constexpr(std::is_same<OperandType, float>::value)
            {
                result = sqrtf(operand);
            }
            else
            {
                result = sqrt(operand);
            }
        }
        else if constexpr(operatorType == WasmFloatUnaryOps::Ceil)
        {
            if constexpr(std::is_same<OperandType, float>::value)
            {
                result = ceilf(operand);
            }
            else
            {
                result = ceil(operand);
            }
        }
        else if constexpr(operatorType == WasmFloatUnaryOps::Floor)
        {
            if constexpr(std::is_same<OperandType, float>::value)
            {
                result = floorf(operand);
            }
            else
            {
                result = floor(operand);
            }
        }
        else if constexpr(operatorType == WasmFloatUnaryOps::Trunc)
        {
            if constexpr(std::is_same<OperandType, float>::value)
            {
                result = truncf(operand);
            }
            else
            {
                result = trunc(operand);
            }
        }
        else
        {
            static_assert(operatorType == WasmFloatUnaryOps::Nearest);
            if constexpr(std::is_same<OperandType, float>::value)
            {
                result = rintf(operand);
            }
            else
            {
                result = rint(operand);
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
                    CreateEnumMetaVar<WasmFloatUnaryOps::X_END_OF_ENUM>("operatorType"),
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
    RegisterBoilerplate<FIFloatUnaryOpsImpl>();
}
