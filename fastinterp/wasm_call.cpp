#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "fastinterp_function_alignment.h"
#include "fastinterp_tpl_stackframe_category.h"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

// Call a generated function
//
struct FICallExprImpl
{
    template<typename ReturnType>
    static constexpr bool cond()
    {
        return std::is_same<ReturnType, void>::value ||
               std::is_same<ReturnType, uint32_t>::value ||
               std::is_same<ReturnType, uint64_t>::value ||
               std::is_same<ReturnType, float>::value ||
               std::is_same<ReturnType, double>::value;
    }

    template<typename ReturnType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool spillReturnValue>
    static constexpr bool cond()
    {
        if (FIOpaqueParamsHelper::CanPush(numOIP) || FIOpaqueParamsHelper::CanPush(numOFP)) { return false; }
        if (std::is_same<void, ReturnType>::value && spillReturnValue) { return false; }
        return true;
    }

    template<typename T>
    using WorkaroundVoidType = typename std::conditional<std::is_same<T, void>::value, void*, T>::type;

    // Unlike most of the other operators, this operator allows no OpaqueParams.
    // GHC has no callee-saved registers, all registers are invalidated after a call.
    // Therefore, it is always a waste to have register-pinned opaque parameters:
    // they must be pushed to stack and then popped in order to be passed to the continuation,
    // so it is cheaper to have spilled them to memory at the very beginning.
    //
    // Placeholder rules:
    // boilerplate placeholder 1: call expression
    // constant placeholder 0: spill location, if spillReturnValue
    //
    template<typename ReturnType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool spillReturnValue,
             typename... OpaqueParams>
    static void f(uintptr_t stackframe, OpaqueParams... opaqueParams) noexcept
    {
        constexpr int newStackframeSize = 512;
        alignas(x_fastinterp_function_stack_alignment) uint8_t newStackframe[newStackframeSize];

        [[maybe_unused]] WorkaroundVoidType<ReturnType> returnValue;

        if constexpr(std::is_same<ReturnType, void>::value)
        {
            // "noescape" is required, otherwise the compiler may assume that "newStackframe" could escape the function,
            // preventing tail call optimization on our continuation.
            //
            DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_1_NO_TAILCALL(
                        ReturnType(*)(uintptr_t, OpaqueParams..., __attribute__((__noescape__)) uint8_t*) noexcept);
            BOILERPLATE_FNPTR_PLACEHOLDER_1(stackframe, opaqueParams..., newStackframe);
        }
        else
        {
            // "noescape" is required, otherwise the compiler may assume that "newStackframe" could escape the function,
            // preventing tail call optimization on our continuation.
            //
            DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_1_NO_TAILCALL(
                        ReturnType(*)(uintptr_t, OpaqueParams..., __attribute__((__noescape__)) uint8_t*) noexcept);
            returnValue = BOILERPLATE_FNPTR_PLACEHOLDER_1(stackframe, opaqueParams..., newStackframe);
        }

        if constexpr(std::is_same<ReturnType, void>::value)
        {
            DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t) noexcept);
            BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe);
        }
        else if constexpr(spillReturnValue)
        {
            *internal::GetStackPush<ReturnType>(stackframe) = returnValue;

            DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t) noexcept);
            BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe);
        }
        else
        {
            DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, ReturnType) noexcept);
            BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe, returnValue);
        }
    }

    static auto metavars()
    {
        return CreateMetaVarList(
                    CreateTypeMetaVar("returnType"),
                    CreateOpaqueIntegralParamsLimit(),
                    CreateOpaqueFloatParamsLimit(),
                    CreateBoolMetaVar("spillReturnValue")
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
    RegisterBoilerplate<FICallExprImpl>();
}
