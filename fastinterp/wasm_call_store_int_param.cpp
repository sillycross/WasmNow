#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_store_block_simple_result.h"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

struct FICallStoreIntParamImpl
{
    template<typename OperandType>
    static constexpr bool cond()
    {
        return std::is_same<OperandType, uint32_t>::value ||
               std::is_same<OperandType, uint64_t>::value;
    }

    template<typename OperandType,
             NumIntegralParamsAfterBlock numOIP,
             FINumOpaqueIntegralParams dummyNumOIP,
             FINumOpaqueFloatingParams numOFP>
    static constexpr bool cond()
    {
        if (!FIOpaqueParamsHelper::IsEmpty(dummyNumOIP)) { return false; }
        if (FIOpaqueParamsHelper::CanPush(numOFP)) { return false; }
        return true;
    }

    template<typename OperandType,
             NumIntegralParamsAfterBlock numOIP,
             FINumOpaqueIntegralParams dummyNumOIP,
             FINumOpaqueFloatingParams numOFP,
             typename... OpaqueParams>
    static void f(uintptr_t stackframe, OpaqueParams... opaqueParams,
                  std::conditional_t<static_cast<int>(numOIP) == 1, OperandType, uint64_t> u1,
                  std::conditional_t<static_cast<int>(numOIP) == 2, OperandType, uint64_t> u2,
                  std::conditional_t<static_cast<int>(numOIP) == 3, OperandType, uint64_t> u3,
                  uint8_t* newStackFrame) noexcept
    {
        using SMA = StackMachineAccessor<OperandType /*inputType*/,
                                         1,
                                         void /*outputType*/>;

        OperandType operand;
        if constexpr(static_cast<int>(numOIP) == 0)
        {
            operand = SMA::template GetInput<0>(stackframe);
        }
        else if constexpr(static_cast<int>(numOIP) == 1)
        {
            operand = u1;
        }
        else if constexpr(static_cast<int>(numOIP) == 2)
        {
            operand = u2;
        }
        else
        {
            static_assert(static_cast<int>(numOIP) == 3);
            operand = u3;
        }

        {
            DEFINE_INDEX_CONSTANT_PLACEHOLDER_2;
            *reinterpret_cast<OperandType*>(newStackFrame + CONSTANT_PLACEHOLDER_2) = operand;
        }

        DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, OpaqueParams...,
                                                       std::conditional_t<static_cast<int>(numOIP) == 1, OperandType, uint64_t>,
                                                       std::conditional_t<static_cast<int>(numOIP) == 2, OperandType, uint64_t>,
                                                       std::conditional_t<static_cast<int>(numOIP) == 3, OperandType, uint64_t>,
                                                       uint8_t*) noexcept);
        BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe, opaqueParams..., u1, u2, u3, newStackFrame);
    }

    static auto metavars()
    {
        return CreateMetaVarList(
                    CreateTypeMetaVar("operandType"),
                    CreateEnumMetaVar<NumIntegralParamsAfterBlock::X_END_OF_ENUM>("trueNumOIP"),
                    CreateOpaqueIntegralParamsLimit(),
                    CreateOpaqueFloatParamsLimit()
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
    RegisterBoilerplate<FICallStoreIntParamImpl>();
}
