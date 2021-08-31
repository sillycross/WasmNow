#include "pochivm/common.h"

namespace PochiVM
{

enum class WasmIntUnaryOps
{
    Clz,
    Ctz,
    Popcnt,
    X_END_OF_ENUM
};

enum class WasmFloatUnaryOps
{
    Abs,
    Neg,
    Sqrt,
    Ceil,
    Floor,
    Trunc,
    Nearest,
    X_END_OF_ENUM
};

}   // namespace PochiVM
