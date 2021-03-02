#pragma once

#include "common.h"

#include "for_each_primitive_type.h"
#include "constexpr_array_concat_helper.h"
#include "get_mem_fn_address_helper.h"
#include "cxx2a_bit_cast_helper.h"
#include "fastinterp/fastinterp_tpl_return_type.h"

namespace PochiVM
{

namespace AstTypeHelper
{

// Give each non-pointer type a unique label
//
enum AstTypeLabelEnum
{
    // The order of this enum is fixed (void, primitive types, cpp types).
    // Various places are hardcoded with this assumption of order.
    //
    AstTypeLabelEnum_void

#define F(type) , AstTypeLabelEnum_ ## type
FOR_EACH_PRIMITIVE_TYPE
#undef F

    // must be last element
    //
,   TOTAL_VALUES_IN_TYPE_LABEL_ENUM
};

// human-friendly names of the types, used in pretty-print
//
const char* const AstPrimitiveTypePrintName[1 + x_num_primitive_types] = {
    "void"
#define F(type) , #type
FOR_EACH_PRIMITIVE_TYPE
#undef F
};

const size_t AstPrimitiveTypeSizeInBytes[1 + x_num_primitive_types] = {
      0 /*dummy value for void*/
#define F(type) , sizeof(type)
FOR_EACH_PRIMITIVE_TYPE
#undef F
};

const bool AstPrimitiveTypesIsSigned[1 + x_num_primitive_types] = {
    false /*dummy value for void*/
#define F(type) , std::is_signed<type>::value
FOR_EACH_PRIMITIVE_TYPE
#undef F
};

template<typename T>
struct GetTypeId;

}   // namespace AstTypeHelper

class FastInterpTypeId;

// Unique TypeId for each type possible in codegen
//
struct TypeId
{
    // This is the unique value identifying the type.
    // It should be sufficient to treat it as opaque and only use the API functions,
    // but for informational purpose, the representation is n * x_pointer_typeid_inc +
    // typeLabel + (is generated composite type ? x_generated_composite_type : 0)
    // e.g. int32_t** has TypeId 2 * x_pointer_typeid_inc + int32_t's label in AstTypeLabelEnum
    //
    uint64_t value;

    constexpr explicit TypeId() : value(x_invalid_typeid) {}
    constexpr explicit TypeId(uint64_t _value): value(_value) {}

    constexpr bool operator==(const TypeId& other) const { return other.value == value; }
    constexpr bool operator!=(const TypeId& other) const { return !(*this == other); }

    constexpr bool IsInvalid() const
    {
        return (value == x_invalid_typeid) ? true : (
                    (value >= x_generated_composite_type) ? false : (
                        !(value % x_pointer_typeid_inc < AstTypeHelper::TOTAL_VALUES_IN_TYPE_LABEL_ENUM)));
    }
    bool IsVoid() const { return IsType<void>(); }
    bool IsPrimitiveType() const { return 1 <= value && value <= x_num_primitive_types; }
    bool IsBool() const { return IsType<bool>(); }
    // Including bool type
    //
    bool IsPrimitiveIntType() const { return 1 <= value && value <= x_num_primitive_int_types; }
    bool IsFloat() const { return IsType<float>(); }
    bool IsDouble() const { return IsType<double>(); }
    bool IsFloatingPoint() const { return IsFloat() || IsDouble(); }
    bool IsPrimitiveFloatType() const {
        return x_num_primitive_int_types < value && value <= x_num_primitive_int_types + x_num_primitive_float_types;
    }
    bool IsSigned() const {
        assert(IsPrimitiveType());
        return AstTypeHelper::AstPrimitiveTypesIsSigned[value];
    }
    bool IsPointerType() const {
        return !IsInvalid() && (value % x_generated_composite_type >= x_pointer_typeid_inc);
    }
    // e.g. int**** has 4 layers of pointers
    //
    size_t NumLayersOfPointers() const {
        assert(!IsInvalid());
        return (value % x_generated_composite_type) / x_pointer_typeid_inc;
    }
    constexpr TypeId WARN_UNUSED AddPointer() const {
        assert(!IsInvalid());
        return TypeId { value + x_pointer_typeid_inc };
    }
    TypeId WARN_UNUSED RemovePointer() const {
        assert(IsPointerType());
        return TypeId { value - x_pointer_typeid_inc };
    }
    // The type after removing all layers of pointers
    //
    TypeId GetRawType() const {
        assert(!IsInvalid());
        return TypeId { value - x_pointer_typeid_inc * NumLayersOfPointers() };
    }

    AstTypeHelper::AstTypeLabelEnum ToTypeLabelEnum() const
    {
        assert(!IsInvalid() && !IsPointerType());
        assert(value < AstTypeHelper::TOTAL_VALUES_IN_TYPE_LABEL_ENUM);
        return static_cast<AstTypeHelper::AstTypeLabelEnum>(value);
    }

    template<typename T>
    bool IsType() const
    {
        return (*this == Get<T>());
    }

    // Return the size of this type in bytes.
    //
    // This agrees with the type size in llvm, except that
    // bool has a size of 1 byte in C++, but 1 bit (i1) in llvm
    //
    size_t Size() const
    {
        assert(!IsInvalid());
        if (IsVoid())
        {
            // why do you want to get the size of void?
            //
            TestAssert(false);
            __builtin_unreachable();
        }
        else if (IsPrimitiveType())
        {
            return AstTypeHelper::AstPrimitiveTypeSizeInBytes[value];
        }
        else if (IsPointerType())
        {
            return sizeof(void*);
        }
        TestAssert(false);
        __builtin_unreachable();
    }

    // Print the human-friendly type name in text
    //
    std::string Print() const
    {
        if (IsInvalid())
        {
            return std::string("(invalid type)");
        }
        if (IsPointerType())
        {
            return GetRawType().Print() + std::string(NumLayersOfPointers(), '*');
        }
        else
        {
            assert(value <= x_num_primitive_types);
            return std::string(AstTypeHelper::AstPrimitiveTypePrintName[value]);
        }
    }

    // Default conversion to FastInterpTypeId:
    // >=2 level of pointer => void**
    // CPP-type* => void*
    // CPP-type => locked down
    //
    FastInterpTypeId GetDefaultFastInterpTypeId();

    // same as above, except that >=1 level of pointer => void*
    //
    FastInterpTypeId GetOneLevelPtrFastInterpTypeId();

    // TypeId::Get<T>() return TypeId for T
    //
    template<typename T>
    static constexpr TypeId Get()
    {
        TypeId ret = AstTypeHelper::GetTypeId<T>::value;
        assert(!ret.IsInvalid());
        return ret;
    }

    const static uint64_t x_generated_composite_type = 1000000000ULL * 1000000000ULL;
    // Craziness: if you want to change this constant for some reason,
    // make sure you make the same change in the definition in fastinterp/metavar.h as well.
    // Unfortunately due to build dependency that header file cannot include this one.
    //
    const static uint64_t x_pointer_typeid_inc = 1000000000;
    const static uint64_t x_invalid_typeid = static_cast<uint64_t>(-1);
};

}   // namespace PochiVM

// Inject std::hash for TypeId
//
namespace std
{
    template<> struct hash<PochiVM::TypeId>
    {
        std::size_t operator()(const PochiVM::TypeId& typeId) const noexcept
        {
            return std::hash<uint64_t>{}(typeId.value);
        }
    };
}   // namespace std

namespace PochiVM {

namespace AstTypeHelper
{

// 'char' behaves identical to either 'int8_t' or 'uint8_t' (platform dependent),
// but is a different type. This type figures out what it should behaves like
//
using CharAliasType = typename std::conditional<std::is_signed<char>::value, int8_t, uint8_t>::type;

// GetTypeId<T>::value gives the unique TypeId representing type T
//
template<typename T>
struct GetTypeId
{
    constexpr static TypeId value = TypeId();
    static_assert(sizeof(T) == 0, "Bad Type T");
};

#define F(type) \
template<> struct GetTypeId<type> {	\
    constexpr static TypeId value = TypeId { AstTypeLabelEnum_ ## type }; \
};

F(void)
FOR_EACH_PRIMITIVE_TYPE
#undef F

template<> struct GetTypeId<char> {
    constexpr static TypeId value = GetTypeId<CharAliasType>::value;
};

template<typename T> struct GetTypeId<T*> {
    constexpr static TypeId value = TypeId { GetTypeId<T>::value.value + TypeId::x_pointer_typeid_inc };
};

template<typename T>
struct is_any_possible_type : std::integral_constant<bool,
        !GetTypeId<T>::value.IsInvalid()
> {};

// is_primitive_int_type<T>::value
// true for primitive int types, false otherwise
//
template<typename T>
struct is_primitive_int_type : std::false_type {};
#define F(type) \
template<> struct is_primitive_int_type<type> : std::true_type {};
FOR_EACH_PRIMITIVE_INT_TYPE_AND_CHAR
#undef F

// is_primitive_float_type<T>::value
// true for primitive float types, false otherwise
//
template<typename T>
struct is_primitive_float_type : std::false_type {};
#define F(type) \
template<> struct is_primitive_float_type<type> : std::true_type {};
FOR_EACH_PRIMITIVE_FLOAT_TYPE
#undef F

// is_primitive_type<T>::value
// true for primitive types, false otherwise
//
template<typename T>
struct is_primitive_type : std::false_type {};
#define F(type) \
template<> struct is_primitive_type<type> : std::true_type {};
FOR_EACH_PRIMITIVE_TYPE_AND_CHAR
#undef F

// may_explicit_convert<T, U>::value (T, U must be primitive types)
// true if T may be explicitly converted to U using a StaticCast(), false otherwise
//
// Explicit convert is allowed between int-types, between float-types,
// and from int-type to float-type
//
template<typename T, typename U>
struct may_explicit_convert : std::integral_constant<bool,
        (is_primitive_int_type<T>::value && is_primitive_int_type<U>::value) ||
        (is_primitive_int_type<T>::value && is_primitive_float_type<U>::value) ||
        (is_primitive_float_type<T>::value && is_primitive_float_type<U>::value)
> {};

// may_implicit_convert<T, U>::value (T, U must be primitive types)
// true if T may be implicitly converted to U, false otherwise
// The only allowed implicit conversions currently is integer widening conversion
//
template<typename T, typename U>
struct may_implicit_convert : std::false_type {};

#define F(type1, type2) \
template<> struct may_implicit_convert<type1, type2> : std::true_type {                     \
    static_assert(may_explicit_convert<type1, type2>::value, "Bad implicit conversion");    \
};
FOR_EACH_PRIMITIVE_INT_TYPE_WIDENING_CONVERSION
#undef F

// may_static_cast<T, U>::value (T, U must be primitive or pointer types)
// Allows explicit-castable types and up/down casts between pointers
//
// reinterpret_cast and static_cast are different!
// Example: class A : public B, public C {};
// Now static_cast A* to C* results in a shift in pointer value,
// while reinterpret_cast A* to C* is likely a bad idea.
//
// For above reason, we disallow static_cast acting on NULL pointer,
// as there is hardly any legit use to silently changing NULL to a non-zero invalid pointer.
// User should always check for NULL before performing the cast.
// We assert that the operand is not NULL in generated code.
//
// TODO: add runtime check for downcast?
//
template<typename T, typename U>
struct may_static_cast : std::integral_constant<bool,
        may_explicit_convert<T, U>::value
     || (std::is_pointer<T>::value && std::is_pointer<U>::value && std::is_convertible<T, U>::value)
> {};

// may_reinterpret_cast<T, U>::value
// Allows reinterpret_cast between any pointers, and between pointer and uint64_t
//
template<typename T, typename U>
struct may_reinterpret_cast : std::integral_constant<bool,
        (std::is_pointer<T>::value && std::is_pointer<U>::value)
     || (std::is_pointer<T>::value && std::is_same<U, uint64_t>::value)
     || (std::is_same<T, uint64_t>::value && std::is_pointer<U>::value)
> {};

// A list of binary operations supported by operator overloading
// Logical operators (and/or/not) are not listed here.
//
enum class BinaryOps
{
    ADD,
    SUB,
    MUL,
    DIV,
    MODULO,
    EQUAL,
    GREATER
};

// All types except bool supports ADD, SUB, MUL
//
template<typename T>
struct supports_addsubmul_internal : std::integral_constant<bool,
        (is_primitive_type<T>::value && !std::is_same<T, bool>::value)
> {};

template<typename T>
struct supports_div_internal : std::integral_constant<bool,
        (is_primitive_type<T>::value && !std::is_same<T, bool>::value)
> {};

// All int-type except bool supports MODULO
//
template<typename T>
struct supports_modulo_internal : std::integral_constant<bool,
        (is_primitive_int_type<T>::value && !std::is_same<T, bool>::value)
> {};

// All types support EQUAL
//
template<typename T>
struct supports_equal_internal : std::integral_constant<bool,
        (is_primitive_type<T>::value)
> {};

// All types except bool supports GREATER
//
template<typename T>
struct supports_greater_internal : std::integral_constant<bool,
        (is_primitive_type<T>::value && !std::is_same<T, bool>::value)
> {};

// Generate list of supported binary ops for each primitive type
//
template<typename T>
struct primitive_type_supports_binary_op_internal
{
    const static uint64_t value =
              (static_cast<uint64_t>(supports_addsubmul_internal<T>::value) << static_cast<int>(BinaryOps::ADD))
            + (static_cast<uint64_t>(supports_addsubmul_internal<T>::value) << static_cast<int>(BinaryOps::SUB))
            + (static_cast<uint64_t>(supports_addsubmul_internal<T>::value) << static_cast<int>(BinaryOps::MUL))
            + (static_cast<uint64_t>(supports_div_internal<T>::value) << static_cast<int>(BinaryOps::DIV))
            + (static_cast<uint64_t>(supports_modulo_internal<T>::value) << static_cast<int>(BinaryOps::MODULO))
            + (static_cast<uint64_t>(supports_equal_internal<T>::value) << static_cast<int>(BinaryOps::EQUAL))
            + (static_cast<uint64_t>(supports_greater_internal<T>::value) << static_cast<int>(BinaryOps::GREATER));
};

template<typename T, BinaryOps op>
struct primitive_type_supports_binary_op : std::integral_constant<bool,
        is_primitive_type<T>::value &&
        ((primitive_type_supports_binary_op_internal<T>::value & (static_cast<uint64_t>(1) << static_cast<int>(op))) != 0)
> {};

// static_cast_offset<T, U>::get()
// On static_cast-able <T, U>-pair (T, U must both be pointers),
// the value is the shift in bytes needed to add to T when converted to U
// Otherwise, the value is std::numeric_limits<ssize_t>::max() to not cause a compilation error when used.
//
// E.g.
//    class A { uint64_t a}; class B { uint64_t b; } class C : public A, public B { uint64_t c };
// Then:
//    static_cast_offset<A*, C*> = 0
//    static_cast_offset<B*, C*> = -8
//    static_cast_offset<C*, A*> = 0
//    static_cast_offset<C*, B*> = 8
//    static_cast_offset<A*, B*> = std::numeric_limits<ssize_t>::max()
//    static_cast_offset<std::string, int> = std::numeric_limits<ssize_t>::max()
//
template<typename T, typename U, typename Enable = void>
struct static_cast_offset
{
    static ssize_t get()
    {
        return std::numeric_limits<ssize_t>::max();
    }
};

template<typename T, typename U>
struct static_cast_offset<T*, U*, typename std::enable_if<
        std::is_convertible<T*, U*>::value
>::type > {
    static ssize_t get()
    {
        // Figure out the shift offset by doing a fake cast on 0x1000. Any address should
        // also work, but nullptr will not, so just to make things obvious..
        //
        return static_cast<ssize_t>(
            reinterpret_cast<uintptr_t>(static_cast<U*>(reinterpret_cast<T*>(0x1000))) -
            static_cast<uintptr_t>(0x1000));
    }
};

template<typename T>
struct pointer_or_uint64_type: std::integral_constant<bool,
        std::is_pointer<T>::value
     || std::is_same<T, uint64_t>::value
> {};

template<typename T>
struct primitive_or_pointer_type: std::integral_constant<bool,
        std::is_pointer<T>::value
     || is_primitive_type<T>::value
> {};

// function_type_helper<T>:
//    T must be a C style function pointer
//    gives information about arg and return types of the function
//
template<typename R, typename... Args>
struct function_type_helper_internal
{
    static const size_t numArgs = sizeof...(Args);

    using ReturnType = R;

    template<size_t i>
    using ArgType = typename std::tuple_element<i, std::tuple<Args...>>::type;

    template<size_t n, typename Enable = void>
    struct build_typeid_array_internal
    {
        static constexpr std::array<TypeId, n> value =
                constexpr_std_array_concat(
                    build_typeid_array_internal<n-1>::value,
                    std::array<TypeId, 1>{
                        GetTypeId<ArgType<n-1>>::value });
    };

    template<size_t n>
    struct build_typeid_array_internal<n, typename std::enable_if<(n == 0)>::type>
    {
        static constexpr std::array<TypeId, n> value = std::array<TypeId, 0>{};
    };

    static constexpr std::array<TypeId, numArgs> argTypeId =
            build_typeid_array_internal<numArgs>::value;

    static constexpr TypeId returnTypeId = GetTypeId<ReturnType>::value;
};

template<typename T>
struct function_type_helper
{
    static_assert(sizeof(T) == 0,
                  "T must be a C-style function");
};

template<typename R, typename... Args>
struct function_type_helper<R(*)(Args...)>
    : function_type_helper_internal<R, Args...>
{
    using ReturnType = typename function_type_helper_internal<R, Args...>::ReturnType;

    template<size_t i>
    using ArgType = typename function_type_helper_internal<R, Args...>::template ArgType<i>;
};

template<typename R, typename... Args>
struct function_type_helper<R(*)(Args...) noexcept>
    : function_type_helper_internal<R, Args...>
{
    using ReturnType = typename function_type_helper_internal<R, Args...>::ReturnType;

    template<size_t i>
    using ArgType = typename function_type_helper_internal<R, Args...>::template ArgType<i>;
};

template<typename T>
using function_return_type = typename function_type_helper<T>::ReturnType;

template<typename T, size_t i>
using function_arg_type = typename function_type_helper<T>::template ArgType<i>;

// is_function_prototype<T>
// true_type if T is a C-style function pointer
//
template<typename T>
struct is_function_prototype : std::false_type
{ };

template<typename R, typename... Args>
struct is_function_prototype<R(*)(Args...)> : std::true_type
{ };

template<typename R, typename... Args>
struct is_function_prototype<R(*)(Args...) noexcept> : std::true_type
{ };

// is_function_prototype<T>
// true_type if T is a noexcept C-style function pointer
//
template<typename T>
struct is_noexcept_function_prototype : std::false_type
{ };

template<typename R, typename... Args>
struct is_noexcept_function_prototype<R(*)(Args...) noexcept> : std::true_type
{ };

}   // namespace AstTypeHelper

// In interp mode, we only know a limited set of types (fundamental types, pointer to fundamental types, and void**)
// This is a wrapper over typeId so that only such types are allowed.
//
class FastInterpTypeId
{
public:
    FastInterpTypeId() : m_typeId() {}
    explicit FastInterpTypeId(TypeId typeId)
    {
        TestAssert(typeId.NumLayersOfPointers() <= 2);
        TestAssertImp(typeId.NumLayersOfPointers() == 2, typeId == TypeId::Get<void**>());
        TestAssertImp(typeId.NumLayersOfPointers() == 1, typeId.RemovePointer().IsVoid() || typeId.RemovePointer().IsPrimitiveType());
        TestAssertImp(typeId.NumLayersOfPointers() == 0, typeId.IsVoid() || typeId.IsPrimitiveType());
        m_typeId = typeId;
    }

    TypeId GetTypeId() const
    {
        return m_typeId;
    }

private:
    TypeId m_typeId;
};

inline FastInterpTypeId TypeId::GetDefaultFastInterpTypeId()
{
    if (NumLayersOfPointers() >= 2)
    {
        return FastInterpTypeId(TypeId::Get<void**>());
    }
    else if (NumLayersOfPointers() == 1)
    {
        if (!RemovePointer().IsVoid() && !RemovePointer().IsPrimitiveType())
        {
            return FastInterpTypeId(TypeId::Get<void*>());
        }
        else
        {
            return FastInterpTypeId(*this);
        }
    }
    else
    {
        TestAssert(IsVoid() || IsPrimitiveType());
        return FastInterpTypeId(*this);
    }
}

inline FastInterpTypeId TypeId::GetOneLevelPtrFastInterpTypeId()
{
    if (NumLayersOfPointers() >= 1)
    {
        return FastInterpTypeId(TypeId::Get<void*>());
    }
    else
    {
        TestAssert(IsVoid() || IsPrimitiveType());
        return FastInterpTypeId(*this);
    }
}

}   // namespace PochiVM
