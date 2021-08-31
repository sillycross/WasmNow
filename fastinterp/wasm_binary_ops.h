#include "pochivm/common.h"

namespace PochiVM
{

enum class WasmIntBinaryOps
{
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    And,
    Or,
    Xor,
    Shl,
    Shr,
    Rotl,
    Rotr,
    X_END_OF_ENUM
};

enum class WasmFloatBinaryOps
{
    Add,
    Sub,
    Mul,
    Div,
    Min,
    Max,
    CopySign,
    X_END_OF_ENUM
};

enum class NumInRegisterOperands
{
    ZERO,
    ONE,
    TWO,
    X_END_OF_ENUM = 3
};

enum class TrinaryOpNumInRegisterOperands
{
    ZERO,
    ONE,
    TWO,
    THREE,
    X_END_OF_ENUM = 4
};

}   // namespace PochiVM
