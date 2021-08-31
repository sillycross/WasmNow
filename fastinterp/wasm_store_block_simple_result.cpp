#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_common_ops_helper.h"
#include "wasm_store_block_simple_result.h"

namespace PochiVM
{

struct FIStoreBlockSimpleResultImpl
{
    template<typename OperandType>
    static constexpr bool cond()
    {
        return std::is_same<OperandType, uint32_t>::value ||
               std::is_same<OperandType, uint64_t>::value ||
               std::is_same<OperandType, float>::value ||
               std::is_same<OperandType, double>::value;
    }

    template<typename OperandType,
             FINumOpaqueIntegralParams oldNumOIP,
             FINumOpaqueFloatingParams oldNumOFP>
    static constexpr bool cond()
    {
        if (std::is_floating_point<OperandType>::value)
        {
            if (FIOpaqueParamsHelper::CanPush(oldNumOIP)) { return false; }
        }
        else
        {
            if (FIOpaqueParamsHelper::CanPush(oldNumOFP)) { return false; }
        }
        return true;
    }

    template<typename OperandType,
             FINumOpaqueIntegralParams oldNumOIP,
             FINumOpaqueFloatingParams oldNumOFP,
             NumIntegralParamsAfterBlock newNumOIP,
             NumFloatParamsAfterBlock newNumOFP,
             bool isInRegister,
             bool isIn2ndStackTop,
             bool spillOutput>
    static constexpr bool cond()
    {
        if (std::is_floating_point<OperandType>::value)
        {
            if (FIOpaqueParamsHelper::CanPush(static_cast<FINumOpaqueIntegralParams>(newNumOIP))) { return false; }
            if (spillOutput && !FIOpaqueParamsHelper::IsEmpty(static_cast<FINumOpaqueFloatingParams>(newNumOFP))) { return false; }
            if (static_cast<FINumOpaqueFloatingParams>(newNumOFP) > oldNumOFP) { return false; }
            if (!isInRegister && !FIOpaqueParamsHelper::IsEmpty(oldNumOFP)) { return false; }
            if (isInRegister && !FIOpaqueParamsHelper::CanPush(oldNumOFP)) { return false; }
            if (isIn2ndStackTop) { return false; }
            return true;
        }
        else
        {
            if (FIOpaqueParamsHelper::CanPush(static_cast<FINumOpaqueFloatingParams>(newNumOFP))) { return false; }
            if (spillOutput && !FIOpaqueParamsHelper::IsEmpty(static_cast<FINumOpaqueIntegralParams>(newNumOIP))) { return false; }
            if (static_cast<FINumOpaqueIntegralParams>(newNumOIP) > oldNumOIP) { return false; }
            if (!isInRegister && !FIOpaqueParamsHelper::IsEmpty(oldNumOIP)) { return false; }
            if (isInRegister && !FIOpaqueParamsHelper::CanPush(oldNumOIP)) { return false; }
            if (isIn2ndStackTop) { if (isInRegister) { return false; } }
        }
        return true;
    }

    template<typename OperandType,
             FINumOpaqueIntegralParams oldNumOIP,
             FINumOpaqueFloatingParams oldNumOFP,
             NumIntegralParamsAfterBlock newNumOIP,
             NumFloatParamsAfterBlock newNumOFP,
             bool isInRegister,
             bool isIn2ndStackTop,
             bool spillOutput,
             typename... OpaqueParams>
    static void f(uintptr_t stackframe, OpaqueParams... opaqueParams, OperandType qa1) noexcept
    {
        OperandType result;
        if constexpr(isInRegister)
        {
            result = qa1;
        }
        else if constexpr(!isIn2ndStackTop)
        {
            result = *internal::GetStackTop<OperandType>(stackframe);
        }
        else
        {
            result = *internal::GetStack2ndTop<OperandType>(stackframe);
        }

        if constexpr(spillOutput)
        {
            if constexpr(std::is_floating_point<OperandType>::value)
            {
                static_assert(!FIOpaqueParamsHelper::CanPush(static_cast<FINumOpaqueIntegralParams>(newNumOIP)) && !FIOpaqueParamsHelper::CanPush(oldNumOIP));
                static_assert(FIOpaqueParamsHelper::IsEmpty(static_cast<FINumOpaqueFloatingParams>(newNumOFP)));
                static_assert(x_fastinterp_max_integral_params == 3);

                DEFINE_INDEX_CONSTANT_PLACEHOLDER_2;
                *GetLocalVarAddress<OperandType>(stackframe, CONSTANT_PLACEHOLDER_2) = result;

                DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, uint64_t, uint64_t, uint64_t) noexcept);
                BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe,
                                                std::get<0>(std::make_tuple(opaqueParams...)),
                                                std::get<1>(std::make_tuple(opaqueParams...)),
                                                std::get<2>(std::make_tuple(opaqueParams...)));
            }
            else
            {
                static_assert(!FIOpaqueParamsHelper::CanPush(static_cast<FINumOpaqueFloatingParams>(newNumOFP)) && !FIOpaqueParamsHelper::CanPush(oldNumOFP));
                static_assert(FIOpaqueParamsHelper::IsEmpty(static_cast<FINumOpaqueIntegralParams>(newNumOIP)));
                static_assert(x_fastinterp_max_floating_point_params == 3);

                DEFINE_INDEX_CONSTANT_PLACEHOLDER_2;
                *GetLocalVarAddress<OperandType>(stackframe, CONSTANT_PLACEHOLDER_2) = result;

                DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, double, double, double) noexcept);
                BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe,
                                                std::get<static_cast<int>(oldNumOIP)>(std::make_tuple(opaqueParams...)),
                                                std::get<static_cast<int>(oldNumOIP) + 1>(std::make_tuple(opaqueParams...)),
                                                std::get<static_cast<int>(oldNumOIP) + 2>(std::make_tuple(opaqueParams...)));
            }
        }
        else
        {
            static_assert(x_fastinterp_max_integral_params == 3 && x_fastinterp_max_floating_point_params == 3);
            if constexpr(std::is_floating_point<OperandType>::value)
            {
                static_assert(!FIOpaqueParamsHelper::CanPush(static_cast<FINumOpaqueIntegralParams>(newNumOIP)) && !FIOpaqueParamsHelper::CanPush(oldNumOIP));
                static_assert(oldNumOFP >= static_cast<FINumOpaqueFloatingParams>(newNumOFP));
                if constexpr(static_cast<int>(newNumOFP) == 0)
                {
                    DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, uint64_t, uint64_t, uint64_t, OperandType) noexcept);
                    BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe,
                                                    std::get<0>(std::make_tuple(opaqueParams...)),
                                                    std::get<1>(std::make_tuple(opaqueParams...)),
                                                    std::get<2>(std::make_tuple(opaqueParams...)),
                                                    result);
                }
                else if constexpr(static_cast<int>(newNumOFP) == 1)
                {
                    DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, uint64_t, uint64_t, uint64_t, double, OperandType) noexcept);
                    BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe,
                                                    std::get<0>(std::make_tuple(opaqueParams...)),
                                                    std::get<1>(std::make_tuple(opaqueParams...)),
                                                    std::get<2>(std::make_tuple(opaqueParams...)),
                                                    std::get<static_cast<int>(oldNumOIP)>(std::make_tuple(opaqueParams...)),
                                                    result);
                }
                else
                {
                    static_assert(static_cast<int>(newNumOFP) == 2);
                    DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, uint64_t, uint64_t, uint64_t, double, double, OperandType) noexcept);
                    BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe,
                                                    std::get<0>(std::make_tuple(opaqueParams...)),
                                                    std::get<1>(std::make_tuple(opaqueParams...)),
                                                    std::get<2>(std::make_tuple(opaqueParams...)),
                                                    std::get<static_cast<int>(oldNumOIP)>(std::make_tuple(opaqueParams...)),
                                                    std::get<static_cast<int>(oldNumOIP) + 1>(std::make_tuple(opaqueParams...)),
                                                    result);
                }
            }
            else
            {
                static_assert(!FIOpaqueParamsHelper::CanPush(static_cast<FINumOpaqueFloatingParams>(newNumOFP)) && !FIOpaqueParamsHelper::CanPush(oldNumOFP));
                static_assert(oldNumOIP >= static_cast<FINumOpaqueIntegralParams>(newNumOIP));
                if constexpr(static_cast<int>(newNumOIP) == 0)
                {
                    DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, double, double, double, OperandType) noexcept);
                    BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe,
                                                    std::get<static_cast<int>(oldNumOIP)>(std::make_tuple(opaqueParams...)),
                                                    std::get<static_cast<int>(oldNumOIP) + 1>(std::make_tuple(opaqueParams...)),
                                                    std::get<static_cast<int>(oldNumOIP) + 2>(std::make_tuple(opaqueParams...)),
                                                    result);
                }
                else if constexpr(static_cast<int>(newNumOIP) == 1)
                {
                    DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, uint64_t, double, double, double, OperandType) noexcept);
                    BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe,
                                                    std::get<0>(std::make_tuple(opaqueParams...)),
                                                    std::get<static_cast<int>(oldNumOIP)>(std::make_tuple(opaqueParams...)),
                                                    std::get<static_cast<int>(oldNumOIP) + 1>(std::make_tuple(opaqueParams...)),
                                                    std::get<static_cast<int>(oldNumOIP) + 2>(std::make_tuple(opaqueParams...)),
                                                    result);
                }
                else
                {
                    static_assert(static_cast<int>(newNumOIP) == 2);
                    DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, uint64_t, uint64_t, double, double, double, OperandType) noexcept);
                    BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe,
                                                    std::get<0>(std::make_tuple(opaqueParams...)),
                                                    std::get<1>(std::make_tuple(opaqueParams...)),
                                                    std::get<static_cast<int>(oldNumOIP)>(std::make_tuple(opaqueParams...)),
                                                    std::get<static_cast<int>(oldNumOIP) + 1>(std::make_tuple(opaqueParams...)),
                                                    std::get<static_cast<int>(oldNumOIP) + 2>(std::make_tuple(opaqueParams...)),
                                                    result);
                }
            }
        }
    }

    static auto metavars()
    {
        return CreateMetaVarList(
                    CreateTypeMetaVar("operandType"),
                    CreateOpaqueIntegralParamsLimit(),
                    CreateOpaqueFloatParamsLimit(),
                    CreateEnumMetaVar<NumIntegralParamsAfterBlock::X_END_OF_ENUM>("numIntAfterBlock"),
                    CreateEnumMetaVar<NumFloatParamsAfterBlock::X_END_OF_ENUM>("numFloatAfterBlock"),
                    CreateBoolMetaVar("isInRegister"),
                    CreateBoolMetaVar("isInStack2ndTop"),
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
    RegisterBoilerplate<FIStoreBlockSimpleResultImpl>();
}
