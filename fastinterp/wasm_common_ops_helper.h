#pragma once

#include "fastinterp_tpl_common.hpp"

// All common wasm opcodes share the same placeholder configuration
// DATA_0: int stack top
// DATA_1: float stack top
// DATA_2: constant
// DATA_8: a fake operand which always equals DATA_0 + 8
// DATA_9: a fake operand which always equals DATA_0 - 8
// DATA_10: a fake operand which always equals DATA_0 - 16
// DATA_11: a fake operand which always equals DATA_1 + 8
// DATA_12: a fake operand which always equals DATA_1 - 8
//

namespace PochiVM
{

#define INT_TOP 0

#define INT_PUSH 8
#define INT_2ND_TOP 9
#define INT_3RD_TOP 10

#define FLOAT_TOP 1
#define FLOAT_PUSH 11
#define FLOAT_2ND_TOP 12

#define DEF_DATA(x) INTERNAL_DEFINE_INDEX_CONSTANT_PLACEHOLDER(x)
#define GET_DATA(x) TOKEN_PASTE(CONSTANT_PLACEHOLDER_, x)

namespace internal
{

template<typename T>
T* ALWAYS_INLINE WARN_UNUSED GetStackTop(uintptr_t stackframe)
{
    if constexpr(!std::is_floating_point<T>::value)
    {
        DEF_DATA(INT_TOP);
        return GetLocalVarAddress<T>(stackframe, GET_DATA(INT_TOP));
    }
    else
    {
        DEF_DATA(FLOAT_TOP);
        return GetLocalVarAddress<T>(stackframe, GET_DATA(FLOAT_TOP));
    }
}

template<typename T>
T* ALWAYS_INLINE WARN_UNUSED GetStack2ndTop(uintptr_t stackframe)
{
    if constexpr(!std::is_floating_point<T>::value)
    {
        DEF_DATA(INT_2ND_TOP);
        return GetLocalVarAddress<T>(stackframe, GET_DATA(INT_2ND_TOP));
    }
    else
    {
        DEF_DATA(FLOAT_2ND_TOP);
        return GetLocalVarAddress<T>(stackframe, GET_DATA(FLOAT_2ND_TOP));
    }
}

template<typename T>
T* ALWAYS_INLINE WARN_UNUSED GetStack3rdTop(uintptr_t stackframe)
{
    static_assert(!std::is_floating_point<T>::value);
    DEF_DATA(INT_3RD_TOP);
    return GetLocalVarAddress<T>(stackframe, GET_DATA(INT_3RD_TOP));
}

template<typename T>
T* ALWAYS_INLINE WARN_UNUSED GetStackPush(uintptr_t stackframe)
{
    if constexpr(!std::is_floating_point<T>::value)
    {
        DEF_DATA(INT_PUSH);
        return GetLocalVarAddress<T>(stackframe, GET_DATA(INT_PUSH));
    }
    else
    {
        DEF_DATA(FLOAT_PUSH);
        return GetLocalVarAddress<T>(stackframe, GET_DATA(FLOAT_PUSH));
    }
}

}   // namespace internal

// A class handling the most common kind of opcodes: it takes several inputs
// of same type, and produce zero or one output
//
template<typename InputType, int numInputOnStack, typename OutputType>
struct StackMachineAccessor
{
    static_assert(numInputOnStack >= 0);
    static_assert(numInputOnStack <= 2);  // support later if we need more

    // 0 is stack top, 1 is next top, etc
    //
    template<int inputOrd>
    static InputType ALWAYS_INLINE WARN_UNUSED GetInput(uintptr_t stackframe)
    {
        static_assert(inputOrd >= 0 && inputOrd < numInputOnStack);
        if constexpr(inputOrd == 0)
        {
            return *internal::GetStackTop<InputType>(stackframe);
        }
        else
        {
            static_assert(inputOrd == 1);
            return *internal::GetStack2ndTop<InputType>(stackframe);
        }
    }

    static OutputType* ALWAYS_INLINE WARN_UNUSED GetOutputLoc(uintptr_t stackframe)
    {
        static_assert(!std::is_same<OutputType, void>::value);
        if constexpr(std::is_floating_point<InputType>::value != std::is_floating_point<OutputType>::value)
        {
            // Input and output are on different stacks
            //
            return internal::GetStackPush<OutputType>(stackframe);
        }
        else if constexpr(numInputOnStack == 0)
        {
            return internal::GetStackPush<OutputType>(stackframe);
        }
        else if constexpr(numInputOnStack == 1)
        {
            return internal::GetStackTop<OutputType>(stackframe);
        }
        else
        {
            static_assert(numInputOnStack == 2);
            return internal::GetStack2ndTop<OutputType>(stackframe);
        }
    }
};

#undef INT_TOP
#undef INT_PUSH
#undef INT_2ND_TOP
#undef INT_3RD_TOP

#undef FLOAT_TOP
#undef FLOAT_PUSH
#undef FLOAT_2ND_TOP

#undef DEF_DATA
#undef GET_DATA

}   // namespace PochiVM




